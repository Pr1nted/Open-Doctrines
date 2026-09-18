/* gearbox_qjs.c — the Gearbox binding for JavaScript, on QuickJS.
 *
 * A JS mod is this file plus the QuickJS core, compiled to one wasm module,
 * with the script embedded as bytes. There is no filesystem to load a .js from,
 * so build.sh turns main.js into script.h and the engine evaluates it from
 * memory. TypeScript mods take the same path: tsc emits JS first.
 *
 * Two halves, as in the Lua SDK:
 *
 *   1. The `gearbox` global -- each of the 18 host imports as a JS function.
 *      Two-call sizing (country_name, asset read) is handled here so a script
 *      just gets a string back.
 *
 *   2. Export glue -- mod_load and friends look up same-named globals in the
 *      script and call them.
 *
 * Errors behave the way a JS programmer expects, which is worth stating because
 * the Lua SDK cannot manage it: QuickJS signals exceptions with a sentinel
 * return value rather than longjmp, so `try`/`catch` works normally and a
 * throw out of a hook is caught here, logged, and does NOT kill the mod.
 */

#include "gearbox.h"
#include "quickjs.h"

#include "script.h"     /* generated: gbx_script[], gbx_script_len, gbx_script_name */

#include <math.h>
#include <string.h>

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static JSRuntime *g_rt;
static JSContext *g_ctx;
static gearbox_env_t g_env;

/* Capability groups, switched at build time. Every wasm import must resolve at
 * instantiation whether or not the script calls it, so binding a capability
 * means the manifest must declare it. Same reasoning as sdk/lua. */
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

/* Logs a pending exception, with its stack when there is one. Consumes it. */
static void gbx_report_exception(void) {
    JSValue e = JS_GetException(g_ctx);
    size_t n = 0;
    const char *s = JS_ToCStringLen(g_ctx, &n, e);
    if (s) {
        gearbox_log(GEARBOX_LOG_ERROR, s, (uint32_t)n);
        JS_FreeCString(g_ctx, s);
    } else {
        gearbox_log(GEARBOX_LOG_ERROR, S("js: threw a non-printable value"));
    }

    JSValue stack = JS_GetPropertyStr(g_ctx, e, "stack");
    if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
        const char *st = JS_ToCStringLen(g_ctx, &n, stack);
        if (st && n) {
            gearbox_log(GEARBOX_LOG_ERROR, st, (uint32_t)n);
            JS_FreeCString(g_ctx, st);
        }
    }
    JS_FreeValue(g_ctx, stack);
    JS_FreeValue(g_ctx, e);
}

/* --- small helpers ------------------------------------------------------- */

static int arg_i32(JSContext *ctx, JSValueConst v, int32_t *out) {
    return JS_ToInt32(ctx, out, v) == 0;
}

/* rgba arrives as a JS number that may exceed INT32_MAX (0xFFFFFFFF), so it
 * goes through a double and is truncated, not through JS_ToInt32. */
static uint32_t arg_rgba(JSContext *ctx, JSValueConst v) {
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v) != 0) return 0;
    return (uint32_t)(int64_t)d;
}

/* --- core ---------------------------------------------------------------- */

static JSValue j_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    int32_t lvl = 1;
    JS_ToInt32(ctx, &lvl, argv[0]);
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!s) return JS_EXCEPTION;
    gearbox_log((gearbox_log_level)lvl, s, (uint32_t)n);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* console.log, because a JS programmer will reach for it before anything else.
 * There is no stdout here, so it goes to the host log. */
static JSValue j_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char buf[1024];
    size_t at = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, argv[i]);
        if (!s) continue;
        if (at && at < sizeof buf - 1) buf[at++] = ' ';
        if (n > sizeof buf - 1 - at) n = sizeof buf - 1 - at;
        memcpy(buf + at, s, n);
        at += n;
        JS_FreeCString(ctx, s);
        if (at >= sizeof buf - 1) break;
    }
    gearbox_log(GEARBOX_LOG_INFO, buf, (uint32_t)at);
    return JS_UNDEFINED;
}

static JSValue j_env(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "gearboxMajor", JS_NewInt32(ctx, (int32_t)g_env.gearbox_major));
    JS_SetPropertyStr(ctx, o, "gearboxMinor", JS_NewInt32(ctx, (int32_t)g_env.gearbox_minor));
    JS_SetPropertyStr(ctx, o, "hostVersion",  JS_NewInt32(ctx, (int32_t)g_env.host_version));
    JS_SetPropertyStr(ctx, o, "platform",     JS_NewInt32(ctx, (int32_t)g_env.platform));
    JS_SetPropertyStr(ctx, o, "isWeb",        JS_NewBool(ctx, g_env.is_web != 0));
    JS_SetPropertyStr(ctx, o, "isHeadless",   JS_NewBool(ctx, g_env.is_headless != 0));
    JS_SetPropertyStr(ctx, o, "screenW",      JS_NewInt32(ctx, (int32_t)g_env.screen_w));
    JS_SetPropertyStr(ctx, o, "screenH",      JS_NewInt32(ctx, (int32_t)g_env.screen_h));
    return o;
}

static JSValue j_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) gearbox_abort(S("js: abort"));
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    gearbox_abort(s ? s : "js: abort", (uint32_t)(s ? n : 9));
    return JS_UNDEFINED;   /* not reached */
}

static JSValue j_fuel_budget(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    uint64_t f = gearbox_fuel_budget();
    /* Unmetered is reported as Infinity rather than a wrapped integer. */
    if (f == 0xFFFFFFFFFFFFFFFFull) return JS_NewFloat64(ctx, INFINITY);
    return JS_NewFloat64(ctx, (double)f);
}

/* --- gamestate.read ------------------------------------------------------ */
/* The hand-written few, named by wire id so they read as bound rather than
 * missing: gearbox:core "log" / "env" / "abort" / "fuel_budget",
 * gearbox:assets "read" (bytes, not text) and gearbox:net "recv" (two
 * results through one call). Everything else is generated. */
#include "gearbox_js_generated.h"


/* --- assets -------------------------------------------------------------- */
#if GBX_WITH_ASSETS

static JSValue j_asset_size(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_EXCEPTION;
    size_t n = 0;
    const char *name = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!name) return JS_EXCEPTION;
    uint32_t sz = gearbox_asset_size(name, (uint32_t)n);
    JS_FreeCString(ctx, name);
    return JS_NewUint32(ctx, sz);
}

/* Returns a Uint8Array, or null when there is no such asset. */
static JSValue j_asset_read(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_EXCEPTION;
    size_t n = 0;
    const char *name = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!name) return JS_EXCEPTION;

    uint32_t size = gearbox_asset_size(name, (uint32_t)n);
    if (size == 0) { JS_FreeCString(ctx, name); return JS_NULL; }

    uint8_t *buf = js_malloc(ctx, size);
    if (!buf) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    uint32_t got = gearbox_asset_read(name, (uint32_t)n, buf, size);
    JS_FreeCString(ctx, name);
    if (got > size) got = size;

    JSValue arr = JS_NewUint8ArrayCopy(ctx, buf, got);
    js_free(ctx, buf);
    return arr;
}

#endif /* GBX_WITH_ASSETS */

/* --- registration -------------------------------------------------------- */

static const JSCFunctionListEntry gbx_funcs[] = {
    JS_CFUNC_DEF("log",                 2, j_log),
    JS_CFUNC_DEF("env",                 0, j_env),
    JS_CFUNC_DEF("abort",               1, j_abort),
    JS_CFUNC_DEF("fuelBudget",          0, j_fuel_budget),
#include "gearbox_js_funcs.inc"
#if GBX_WITH_ASSETS
    JS_CFUNC_DEF("assetSize",           1, j_asset_size),
    JS_CFUNC_DEF("assetRead",           1, j_asset_read),
#endif
    JS_PROP_INT32_DEF("TRACE", GEARBOX_LOG_TRACE, 0),
    JS_PROP_INT32_DEF("INFO",  GEARBOX_LOG_INFO,  0),
    JS_PROP_INT32_DEF("WARN",  GEARBOX_LOG_WARN,  0),
    JS_PROP_INT32_DEF("ERROR", GEARBOX_LOG_ERROR, 0),
};

static void gbx_install(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue gbx = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, gbx, gbx_funcs,
                               (int)(sizeof gbx_funcs / sizeof gbx_funcs[0]));
    JS_SetPropertyStr(ctx, global, "gearbox", gbx);

    /* console.log/warn/error -> the host log. quickjs-libc is not compiled in
     * (it is what would bring a filesystem), so without this there is no
     * console object at all and the first thing a modder writes fails. */
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, j_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "info",
                      JS_NewCFunction(ctx, j_console_log, "info", 1));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, j_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, j_console_log, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_FreeValue(ctx, global);
}

/* --- export glue --------------------------------------------------------- */

/* Push script global `name` if it is callable. */
static int get_hook(const char *name, JSValue *out) {
    if (!g_ctx) return 0;
    JSValue global = JS_GetGlobalObject(g_ctx);
    JSValue fn = JS_GetPropertyStr(g_ctx, global, name);
    JS_FreeValue(g_ctx, global);
    if (!JS_IsFunction(g_ctx, fn)) { JS_FreeValue(g_ctx, fn); return 0; }
    *out = fn;
    return 1;
}

/* Calls a hook and consumes it. Returns 0 if it threw -- which is logged and
 * survivable, unlike the Lua SDK where an error takes the mod down. */
static int call_hook(JSValue fn, int argc, JSValue *argv, JSValue *ret) {
    JSValue r = JS_Call(g_ctx, fn, JS_UNDEFINED, argc, (JSValueConst *)argv);
    JS_FreeValue(g_ctx, fn);
    for (int i = 0; i < argc; i++) JS_FreeValue(g_ctx, argv[i]);
    if (JS_IsException(r)) { gbx_report_exception(); JS_FreeValue(g_ctx, r); return 0; }
    if (ret) *ret = r; else JS_FreeValue(g_ctx, r);
    return 1;
}

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    g_rt = JS_NewRuntime();
    if (!g_rt) {
        gearbox_log(GEARBOX_LOG_ERROR, S("js: could not create the runtime"));
        return 1;
    }
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) {
        gearbox_log(GEARBOX_LOG_ERROR, S("js: could not create a context"));
        return 1;
    }
    gbx_install(g_ctx);

    JSValue v = JS_Eval(g_ctx, (const char *)gbx_script, (size_t)gbx_script_len,
                        gbx_script_name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { gbx_report_exception(); JS_FreeValue(g_ctx, v); return 1; }
    JS_FreeValue(g_ctx, v);

    JSValue fn;
    if (get_hook("mod_load", &fn)) {
        JSValue r = JS_UNDEFINED;
        if (!call_hook(fn, 0, NULL, &r)) return 1;
        int32_t rc = 0;
        if (!JS_IsUndefined(r)) JS_ToInt32(g_ctx, &rc, r);
        JS_FreeValue(g_ctx, r);
        if (rc != 0) return rc;
    }
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    if (!g_ctx) return;
    JSValue fn;
    if (get_hook("mod_unload", &fn)) call_hook(fn, 0, NULL, NULL);
    JS_FreeContext(g_ctx);
    JS_FreeRuntime(g_rt);
    g_ctx = NULL;
    g_rt = NULL;
}

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    JSValue fn;
    if (!get_hook("mod_draw_panel", &fn)) return;
    JSValue argv[3];
    argv[0] = JS_NewUint32(g_ctx, panel);
    argv[1] = JS_NewUint32(g_ctx, w);
    argv[2] = JS_NewUint32(g_ctx, h);
    call_hook(fn, 3, argv, NULL);
}

GEARBOX_EXPORT("mod_pre_turn")
void mod_pre_turn(uint32_t turn) {
    JSValue fn;
    if (!get_hook("mod_pre_turn", &fn)) return;
    JSValue argv[1];
    argv[0] = JS_NewUint32(g_ctx, turn);
    call_hook(fn, 1, argv, NULL);
}

GEARBOX_EXPORT("mod_post_turn")
void mod_post_turn(uint32_t turn) {
    JSValue fn;
    if (!get_hook("mod_post_turn", &fn)) return;
    JSValue argv[1];
    argv[0] = JS_NewUint32(g_ctx, turn);
    call_hook(fn, 1, argv, NULL);
}
