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
 *   1. A builtin `gearbox` module -- each of the 18 host imports as a Python
 *      function. Two-call sizing is handled here so a script gets a str back.
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

static PyObject *py_env(PyObject *self, PyObject *args) {
    (void)self; (void)args;
    return Py_BuildValue(
        "{s:i,s:i,s:i,s:i,s:O,s:O,s:i,s:i}",
        "gearboxMajor", (int)g_env.gearbox_major,
        "gearboxMinor", (int)g_env.gearbox_minor,
        "hostVersion",  (int)g_env.host_version,
        "platform",     (int)g_env.platform,
        "isWeb",        g_env.is_web ? Py_True : Py_False,
        "isHeadless",   g_env.is_headless ? Py_True : Py_False,
        "screenW",      (int)g_env.screen_w,
        "screenH",      (int)g_env.screen_h);
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
#if GBX_WITH_GAMESTATE

static PyObject *py_turn_number(PyObject *s, PyObject *a) {
    (void)s; (void)a; return PyLong_FromUnsignedLong(gearbox_turn_number());
}
static PyObject *py_country_count(PyObject *s, PyObject *a) {
    (void)s; (void)a; return PyLong_FromUnsignedLong(gearbox_country_count());
}

/* 0-based, as the ABI is -- Python sequences are 0-based too. None when out of
 * range, which is what a Python caller expects instead of a sentinel int. */
static PyObject *py_country_at(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int i = 0;
    if (!PyArg_ParseTuple(args, "I", &i)) return NULL;
    gearbox_country c = gearbox_country_at(i);
    if (c == GEARBOX_INVALID) Py_RETURN_NONE;
    return PyLong_FromUnsignedLong(c);
}

static PyObject *py_country_name(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int c = 0;
    if (!PyArg_ParseTuple(args, "I", &c)) return NULL;
    uint32_t need = gearbox_country_name(c, NULL, 0);
    if (need == 0) return PyUnicode_FromString("");

    char *buf = (char *)PyMem_Malloc(need);
    if (!buf) return PyErr_NoMemory();
    uint32_t got = gearbox_country_name(c, buf, need);
    if (got > need) got = need;
    PyObject *v = PyUnicode_DecodeUTF8(buf, (Py_ssize_t)got, "replace");
    PyMem_Free(buf);
    return v;
}

static PyObject *py_country_treasury(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int c = 0;
    if (!PyArg_ParseTuple(args, "I", &c)) return NULL;
    return PyFloat_FromDouble(gearbox_country_treasury(c));
}
static PyObject *py_country_province_count(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int c = 0;
    if (!PyArg_ParseTuple(args, "I", &c)) return NULL;
    return PyLong_FromUnsignedLong(gearbox_country_province_count(c));
}
static PyObject *py_province_population(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int p = 0;
    if (!PyArg_ParseTuple(args, "I", &p)) return NULL;
    return PyLong_FromLongLong((long long)gearbox_province_population(p));
}
static PyObject *py_province_owner(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int p = 0;
    if (!PyArg_ParseTuple(args, "I", &p)) return NULL;
    gearbox_country c = gearbox_province_owner(p);
    if (c == GEARBOX_INVALID) Py_RETURN_NONE;
    return PyLong_FromUnsignedLong(c);
}

#endif /* GBX_WITH_GAMESTATE */

/* --- ui ------------------------------------------------------------------ */
#if GBX_WITH_UI

static PyObject *py_panel_register(PyObject *s, PyObject *args) {
    (void)s;
    const char *title = NULL;
    Py_ssize_t n = 0;
    unsigned int w = 240, h = 120;
    if (!PyArg_ParseTuple(args, "s#|II", &title, &n, &w, &h)) return NULL;
    return PyLong_FromUnsignedLong(gearbox_panel_register(title, (uint32_t)n, w, h));
}

static PyObject *py_draw_text(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int panel = 0, rgba = 0;
    int x = 0, y = 0;
    const char *text = NULL;
    Py_ssize_t n = 0;
    if (!PyArg_ParseTuple(args, "IiiIs#", &panel, &x, &y, &rgba, &text, &n))
        return NULL;
    gearbox_draw_text(panel, x, y, rgba, text, (uint32_t)n);
    Py_RETURN_NONE;
}

static PyObject *py_draw_rect(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int panel = 0, rgba = 0;
    int x = 0, y = 0, w = 0, h = 0;
    if (!PyArg_ParseTuple(args, "IiiiiI", &panel, &x, &y, &w, &h, &rgba))
        return NULL;
    gearbox_draw_rect(panel, x, y, w, h, rgba);
    Py_RETURN_NONE;
}

static PyObject *py_button(PyObject *s, PyObject *args) {
    (void)s;
    unsigned int panel = 0;
    int x = 0, y = 0, w = 0, h = 0;
    const char *label = NULL;
    Py_ssize_t n = 0;
    if (!PyArg_ParseTuple(args, "Iiiiis#", &panel, &x, &y, &w, &h, &label, &n))
        return NULL;
    /* True/False, not 0/1: `if gearbox.button(...)` should read as Python. */
    if (gearbox_button(panel, x, y, w, h, label, (uint32_t)n)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

#endif /* GBX_WITH_UI */

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
#if GBX_WITH_GAMESTATE
    {"turnNumber",          py_turn_number,          METH_NOARGS,  "turnNumber() -> int"},
    {"countryCount",        py_country_count,        METH_NOARGS,  "countryCount() -> int"},
    {"countryAt",           py_country_at,           METH_VARARGS, "countryAt(index) -> int or None"},
    {"countryName",         py_country_name,         METH_VARARGS, "countryName(country) -> str"},
    {"countryTreasury",     py_country_treasury,     METH_VARARGS, "countryTreasury(country) -> float"},
    {"countryProvinceCount",py_country_province_count,METH_VARARGS,"countryProvinceCount(country) -> int"},
    {"provincePopulation",  py_province_population,  METH_VARARGS, "provincePopulation(province) -> int"},
    {"provinceOwner",       py_province_owner,       METH_VARARGS, "provinceOwner(province) -> int or None"},
#endif
#if GBX_WITH_UI
    {"panelRegister",       py_panel_register,       METH_VARARGS, "panelRegister(title, minW, minH) -> int"},
    {"drawText",            py_draw_text,            METH_VARARGS, "drawText(panel, x, y, rgba, text)"},
    {"drawRect",            py_draw_rect,            METH_VARARGS, "drawRect(panel, x, y, w, h, rgba)"},
    {"button",              py_button,               METH_VARARGS, "button(panel, x, y, w, h, label) -> bool"},
#endif
#if GBX_WITH_ASSETS
    {"assetSize",           py_asset_size,           METH_VARARGS, "assetSize(name) -> int"},
    {"assetRead",           py_asset_read,           METH_VARARGS, "assetRead(name) -> bytes or None"},
#endif
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef gbx_module = {
    PyModuleDef_HEAD_INIT, "gearbox",
    "OpenDoctrines mod ABI, Gearbox v1.0.", -1, gbx_methods,
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
