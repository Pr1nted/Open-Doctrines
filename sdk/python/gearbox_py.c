/* gearbox_py.c — the Gearbox binding for Python, on CPython 3.12.
 *
 * A Python mod is this file plus libpython, the standard library frozen into
 * the module, and your script embedded as bytes. There is no filesystem in the
 * sandbox, so none of it can be read from disk -- see tools/gen_frozen.py for
 * how the stdlib gets in.
 *
 * IMPORTANT: this SDK only runs on a host built with -DOD_MODS_FAST_INTERP=OFF.
 * CPython trips a structural INT16_MAX operand-stack limit in WAMR's fast
 * interpreter and will not load there at all. See sdk/python/README.md.
 *
 * Two halves, as in every other SDK here:
 *
 *   1. A builtin `gearbox` module -- EVERY host import the build declares,
 *      generated from sdk/abi.json by tools/gen_bindings.py. Two-call
 *      sizing is handled for you, so a script gets a str back.
 *      It used to be 18 hand-written ones out of 184.
 *   2. Export glue -- mod_load and friends look up same-named globals in the
 *      script and call them.
 *
 * Errors behave the way a Python programmer expects: an exception raised in a
 * hook is caught at the boundary, its traceback logged, and the mod carries on.
 */

#include "gearbox.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "frozen_stdlib.h"   /* generated: gbx_frozen_stdlib[] */
#include "script.h"          /* generated: gbx_script[], gbx_script_len, ... */

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static gearbox_env_t g_env;
static PyObject *g_main;      /* __main__ module dict owner; borrowed */

/* Capability groups, switched at build time. Every wasm import must resolve at
 * instantiation whether or not the script calls it, so binding a capability
 * means the manifest must declare it. Same reasoning as sdk/lua and sdk/js. */
#ifndef GBX_WITH_UI
#define GBX_WITH_UI 1
#endif
#ifndef GBX_WITH_GAMESTATE
#define GBX_WITH_GAMESTATE 1
#endif
#ifndef GBX_WITH_ASSETS
#define GBX_WITH_ASSETS 0
#endif

/* --- error reporting ----------------------------------------------------- */

/* Logs the pending exception and clears it. */
static void gbx_report_exception(void) {
    if (!PyErr_Occurred()) return;

    PyObject *type = NULL, *value = NULL, *tb = NULL;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);

    PyObject *str = value ? PyObject_Str(value) : NULL;
    if (str) {
        Py_ssize_t n = 0;
        const char *s = PyUnicode_AsUTF8AndSize(str, &n);
        if (s) gearbox_log(GEARBOX_LOG_ERROR, s, (uint32_t)n);
        Py_DECREF(str);
    } else {
        gearbox_log(GEARBOX_LOG_ERROR, S("python: raised a non-printable value"));
    }

    /* The type name alone is often the useful half of a traceback, and getting
     * a full one requires the traceback module -- which is frozen in, but not
     * worth importing on an error path that may be an out-of-memory. */
    if (type) {
        PyObject *tn = PyObject_GetAttrString(type, "__name__");
        if (tn) {
            Py_ssize_t n = 0;
            const char *s = PyUnicode_AsUTF8AndSize(tn, &n);
            if (s) gearbox_log(GEARBOX_LOG_ERROR, s, (uint32_t)n);
            Py_DECREF(tn);
        }
    }

    Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(tb);
    PyErr_Clear();
}

/* --- core ---------------------------------------------------------------- */

static PyObject *py_log(PyObject *self, PyObject *args) {
    (void)self;
    int level = GEARBOX_LOG_INFO;
    const char *msg = NULL;
    Py_ssize_t n = 0;
    if (!PyArg_ParseTuple(args, "is#", &level, &msg, &n)) return NULL;
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    gearbox_log((gearbox_log_level)level, msg, (uint32_t)n);
    Py_RETURN_NONE;
}

/* print() goes nowhere useful otherwise: there is no stdout a player sees. */
static PyObject *py_print(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self; (void)kwargs;
    PyObject *sep = PyUnicode_FromString(" ");
    PyObject *joined = NULL;
    PyObject *strs = PyTuple_New(PyTuple_GET_SIZE(args));
    if (!sep || !strs) { Py_XDECREF(sep); Py_XDECREF(strs); return NULL; }

    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); i++) {
        PyObject *s = PyObject_Str(PyTuple_GET_ITEM(args, i));
        if (!s) { Py_DECREF(sep); Py_DECREF(strs); return NULL; }
        PyTuple_SET_ITEM(strs, i, s);
    }
    joined = PyUnicode_Join(sep, strs);
    Py_DECREF(sep);
    Py_DECREF(strs);
    if (!joined) return NULL;

    Py_ssize_t n = 0;
    const char *s = PyUnicode_AsUTF8AndSize(joined, &n);
    if (s) gearbox_log(GEARBOX_LOG_INFO, s, (uint32_t)n);
    Py_DECREF(joined);
    Py_RETURN_NONE;
}

/* netRole is here for a reason worth stating: WITHOUT IT A SCRIPT CANNOT TELL
 * SINGLE PLAYER FROM MULTIPLAYER AT ALL. sdk/gearbox.h derives
 * gearbox_is_server() and gearbox_is_multiplayer() from this field and a C mod
 * has had both since 1.0 -- the three interpreted bindings simply dropped it
 * from env(), so a Python, Lua or JS mod could not ask which side it was on.
 * That is the question a mod must answer before writing to the world: one that
 * mutates wherever it runs desynchronises the game the moment two clients
 * disagree. 0 standalone, 1 client, 2 server, 3 host-player; GEARBOX_NET_* in
 * gearbox.h names them. */
static PyObject *py_env(PyObject *self, PyObject *args) {
    (void)self; (void)args;
    return Py_BuildValue(
        "{s:i,s:i,s:i,s:i,s:O,s:O,s:i,s:i,s:i}",
        "gearboxMajor", (int)g_env.gearbox_major,
        "gearboxMinor", (int)g_env.gearbox_minor,
        "hostVersion",  (int)g_env.host_version,
        "platform",     (int)g_env.platform,
        "isWeb",        g_env.is_web ? Py_True : Py_False,
        "isHeadless",   g_env.is_headless ? Py_True : Py_False,
        "screenW",      (int)g_env.screen_w,
        "screenH",      (int)g_env.screen_h,
        "netRole",      (int)g_env.net_role);
}

static PyObject *py_abort(PyObject *self, PyObject *args) {
    (void)self;
    const char *msg = NULL;
    Py_ssize_t n = 0;
    if (!PyArg_ParseTuple(args, "s#", &msg, &n)) return NULL;
    gearbox_abort(msg, (uint32_t)n);
    Py_RETURN_NONE;   /* not reached */
}

static PyObject *py_fuel_budget(PyObject *self, PyObject *args) {
    (void)self; (void)args;
    uint64_t f = gearbox_fuel_budget();
    /* Unmetered is float('inf'), not a wrapped integer. */
    if (f == 0xFFFFFFFFFFFFFFFFull) return PyFloat_FromDouble(Py_HUGE_VAL);
    return PyLong_FromUnsignedLongLong(f);
}

/* --- gamestate.read ------------------------------------------------------ */
/* The hand-written five, named by their wire ids so a reader -- and
 * check_bindings.py -- can see they are bound rather than missing:
 *   gearbox:core "log"           the level enum, and print() routes through it
 *   gearbox:core "env"           fills a struct; this returns a dict
 *   gearbox:core "abort"         does not return, so it builds no PyObject
 *   gearbox:core "fuel_budget"   a u64 sentinel that means float('inf')
 *   gearbox:assets "read"        bytes, not text: UTF-8 decoding a PNG
 *   gearbox:net "recv"           two results through one call
 *
 * The generated bindings live here. Everything this file still defines
 * by hand is a case the generator cannot derive: log takes the level enum and
 * print() routes through it, env fills a struct, abort does not return,
 * fuelBudget's u64 sentinel means infinity, and an asset is bytes rather than
 * text. See tools/gen_bindings.py, PY_SKIP. */
#include "gearbox_py_generated.h"

/* gearbox:net "recv" -- the one import the generator cannot derive.
 *
 * It has TWO results: the message bytes and the sender's peer id, returned
 * through an out buffer and an out pointer. Every other sized getter in the
 * ABI has exactly one out buffer in the last position, which is the shape the
 * two-call sizing idiom covers; this does not, so it is written here.
 *
 * Returns (bytes, peer) or None when the queue is empty -- a tuple rather than
 * a buffer the caller has to size, and None rather than an empty bytes, since
 * an empty message is a real message and "nothing waiting" is not.
 */
#if GBX_WITH_NET
static PyObject *gbxpy_recv(PyObject *self, PyObject *args) {
    (void)self; (void)args;
    /* A message longer than the buffer is TRUNCATED rather than dropped (see
     * the ABI doc), so the buffer is the largest message this binding will
     * hand back whole. 64 KiB matches the send limit. */
    enum { CAP = 65536 };
    char *buf = (char *)PyMem_Malloc(CAP);
    if (!buf) return PyErr_NoMemory();
    uint32_t peer = 0;
    uint32_t n = gearbox_recv(buf, (uint32_t)CAP, &peer);
    if (n == 0) { PyMem_Free(buf); Py_RETURN_NONE; }
    if (n > (uint32_t)CAP) n = (uint32_t)CAP;
    PyObject *payload = PyBytes_FromStringAndSize(buf, (Py_ssize_t)n);
    PyMem_Free(buf);
    if (!payload) return NULL;
    PyObject *t = Py_BuildValue("(Ok)", payload, (long)peer);
    Py_DECREF(payload);
    return t;
}
#endif /* GBX_WITH_NET */


/* --- assets -------------------------------------------------------------- */
#if GBX_WITH_ASSETS

static PyObject *py_asset_size(PyObject *s, PyObject *args) {
    (void)s;
    const char *name = NULL;
    Py_ssize_t n = 0;
    if (!PyArg_ParseTuple(args, "s#", &name, &n)) return NULL;
    return PyLong_FromUnsignedLong(gearbox_asset_size(name, (uint32_t)n));
}

/* bytes, or None when there is no such asset. */
static PyObject *py_asset_read(PyObject *s, PyObject *args) {
    (void)s;
    const char *name = NULL;
    Py_ssize_t n = 0;
    if (!PyArg_ParseTuple(args, "s#", &name, &n)) return NULL;
    uint32_t size = gearbox_asset_size(name, (uint32_t)n);
    if (size == 0) Py_RETURN_NONE;

    char *buf = (char *)PyMem_Malloc(size);
    if (!buf) return PyErr_NoMemory();
    uint32_t got = gearbox_asset_read(name, (uint32_t)n, buf, size);
    if (got > size) got = size;
    PyObject *v = PyBytes_FromStringAndSize(buf, (Py_ssize_t)got);
    PyMem_Free(buf);
    return v;
}

#endif /* GBX_WITH_ASSETS */

/* --- module definition --------------------------------------------------- */

static PyMethodDef gbx_methods[] = {
    {"log",         py_log,         METH_VARARGS, "log(level, message)"},
    {"env",         py_env,         METH_NOARGS,  "env() -> dict"},
    {"abort",       py_abort,       METH_VARARGS, "abort(message) -- does not return"},
    {"fuelBudget",  py_fuel_budget, METH_NOARGS,  "fuelBudget() -> int or inf"},
#if GBX_WITH_NET
    {"recv", gbxpy_recv, METH_NOARGS, "recv() -> (bytes, peer) or None"},
#endif
#include "gearbox_py_methods.inc"
#if GBX_WITH_ASSETS
    {"assetSize",           py_asset_size,           METH_VARARGS, "assetSize(name) -> int"},
    {"assetRead",           py_asset_read,           METH_VARARGS, "assetRead(name) -> bytes or None"},
#endif
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef gbx_module = {
    PyModuleDef_HEAD_INIT, "gearbox",
    "OpenDoctrines mod ABI, Gearbox. Every import the manifest declares.", -1, gbx_methods,
    NULL, NULL, NULL, NULL
};

static PyObject *gbx_module_init(void) {
    PyObject *m = PyModule_Create(&gbx_module);
    if (!m) return NULL;
    PyModule_AddIntConstant(m, "TRACE", GEARBOX_LOG_TRACE);
    PyModule_AddIntConstant(m, "INFO",  GEARBOX_LOG_INFO);
    PyModule_AddIntConstant(m, "WARN",  GEARBOX_LOG_WARN);
    PyModule_AddIntConstant(m, "ERROR", GEARBOX_LOG_ERROR);
    return m;
}

/* --- export glue --------------------------------------------------------- */

/* Fetch script global `name` if it is callable. New reference, or NULL. */
static PyObject *get_hook(const char *name) {
    if (!g_main) return NULL;
    PyObject *fn = PyObject_GetAttrString(g_main, name);
    if (!fn) { PyErr_Clear(); return NULL; }
    if (!PyCallable_Check(fn)) { Py_DECREF(fn); return NULL; }
    return fn;
}

/* Calls a hook and releases it. Returns the result, or NULL if it raised --
 * which is logged and survivable, unlike the Lua SDK where an error is fatal. */
static PyObject *call_hook(PyObject *fn, PyObject *args) {
    PyObject *r = PyObject_CallObject(fn, args);
    Py_DECREF(fn);
    Py_XDECREF(args);
    if (!r) { gbx_report_exception(); return NULL; }
    return r;
}

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    /* Both of these must happen before Py_InitializeFromConfig: the frozen
     * table is consulted during startup (encodings is imported from it), and
     * an inittab entry has to exist before the import machinery runs. */
    PyImport_FrozenModules = gbx_frozen_stdlib;
    if (PyImport_AppendInittab("gearbox", &gbx_module_init) != 0) {
        gearbox_log(GEARBOX_LOG_ERROR, S("python: could not register the gearbox module"));
        return 1;
    }

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import            = 0;   /* site.py wants a filesystem */
    config.write_bytecode         = 0;   /* nowhere to write it */
    config.user_site_directory    = 0;
    config.install_signal_handlers = 0;
    config.faulthandler           = 0;
    config.use_environment        = 0;
    config.pathconfig_warnings    = 0;
    config.module_search_paths_set = 1;  /* deliberately empty: frozen only */

    PyStatus st = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(st)) {
        const char *m = st.err_msg ? st.err_msg : "python: initialisation failed";
        gearbox_log(GEARBOX_LOG_ERROR, m, (uint32_t)strlen(m));
        return 1;
    }

    g_main = PyImport_AddModule("__main__");     /* borrowed */
    if (!g_main) { gbx_report_exception(); return 1; }

    /* Replace print() before the script runs, so a modder's first instinct
     * reaches the host log instead of a stdout nobody reads. */
    {
        static PyMethodDef printdef = {
            "print", (PyCFunction)py_print, METH_VARARGS | METH_KEYWORDS, "print(...)"
        };
        PyObject *pf = PyCFunction_New(&printdef, NULL);
        if (pf) {
            PyObject *builtins = PyEval_GetBuiltins();
            if (builtins) PyDict_SetItemString(builtins, "print", pf);
            Py_DECREF(pf);
        }
    }

    PyObject *code = Py_CompileString((const char *)gbx_script, gbx_script_name,
                                      Py_file_input);
    if (!code) { gbx_report_exception(); return 1; }
    PyObject *globals = PyModule_GetDict(g_main);     /* borrowed */
    PyObject *r = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!r) { gbx_report_exception(); return 1; }
    Py_DECREF(r);

    PyObject *fn = get_hook("mod_load");
    if (fn) {
        PyObject *rc = call_hook(fn, NULL);
        if (!rc) return 1;
        long v = PyLong_Check(rc) ? PyLong_AsLong(rc) : 0;
        Py_DECREF(rc);
        if (v != 0) return (int32_t)v;
    }
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    if (!Py_IsInitialized()) return;
    PyObject *fn = get_hook("mod_unload");
    if (fn) Py_XDECREF(call_hook(fn, NULL));
    g_main = NULL;
    Py_FinalizeEx();
}

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    PyObject *fn = get_hook("mod_draw_panel");
    if (!fn) return;
    Py_XDECREF(call_hook(fn, Py_BuildValue("(III)", panel, w, h)));
}

GEARBOX_EXPORT("mod_pre_turn")
void mod_pre_turn(uint32_t turn) {
    PyObject *fn = get_hook("mod_pre_turn");
    if (!fn) return;
    Py_XDECREF(call_hook(fn, Py_BuildValue("(I)", turn)));
}

GEARBOX_EXPORT("mod_post_turn")
void mod_post_turn(uint32_t turn) {
    PyObject *fn = get_hook("mod_post_turn");
    if (!fn) return;
    Py_XDECREF(call_hook(fn, Py_BuildValue("(I)", turn)));
}
