/* gearbox_lua.c — the Gearbox binding for Lua 5.4.
 *
 * A Lua mod is this file plus the Lua core, compiled to one wasm module, with
 * the script embedded as bytes. There is no filesystem to load a .lua from, so
 * build.sh turns main.lua into script.h and the interpreter runs it from memory.
 *
 * Two halves:
 *
 *   1. The `gearbox` table -- each of the 18 host imports as a Lua function.
 *      Two-call sizing (country_name, asset read) is handled here so a script
 *      just gets a string back.
 *
 *   2. Export glue -- mod_load and friends look up same-named globals in the
 *      script and call them.
 *
 * Errors: see gbx_throw.h. A Lua error terminates the mod; it is logged first.
 */

#include "gearbox.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "script.h"     /* generated: gbx_script[], gbx_script_len, gbx_script_name */

#include <math.h>
#include <string.h>

/* Capability groups, switched at build time.
 *
 * Every wasm import must resolve at instantiation, whether or not the script
 * ever calls it -- so binding a capability here means the mod's MANIFEST.json
 * must declare it, and the user must grant it. Binding all 18 unconditionally
 * would make every Lua mod ask for Assets and GameState.Read even when it only
 * draws a panel, which is exactly the "fewer is better" rule the ABI asks us to
 * hold. So each group is opt-out, and Assets -- the one most mods do not want
 * -- is opt-in.
 *
 * Set these in build.sh (-DGBX_WITH_ASSETS=1) and keep MANIFEST.json's
 * "modules" list in step. tools/wasm_imports.py prints what you actually ended
 * up importing. */
#ifndef GBX_WITH_UI
#define GBX_WITH_UI 1
#endif
#ifndef GBX_WITH_GAMESTATE
#define GBX_WITH_GAMESTATE 1
#endif
#ifndef GBX_WITH_ASSETS
#define GBX_WITH_ASSETS 0
#endif

#define S(lit) lit, (uint32_t)(sizeof(lit) - 1)

static lua_State *L_state;
static gearbox_env_t g_env;

/* --- error path --------------------------------------------------------- */

/* Called from LUAI_THROW. The error object is on top of the stack for real
 * errors; memory errors and error-in-error may not have one. */
void gbx_lua_throw(struct lua_State *L, int errcode) {
    lua_State *S = (lua_State *)L;
    const char *msg = NULL;
    size_t n = 0;

    if (S && lua_gettop(S) > 0 && lua_type(S, -1) == LUA_TSTRING)
        msg = lua_tolstring(S, -1, &n);

    if (msg && n > 0)
        gearbox_log(GEARBOX_LOG_ERROR, msg, (uint32_t)n);
    else if (errcode == LUA_ERRMEM)
        gearbox_log(GEARBOX_LOG_ERROR, S("lua: out of memory"));
    else
        gearbox_log(GEARBOX_LOG_ERROR, S("lua: unrecoverable error"));

    /* No unwinding available -- see gbx_throw.h. Trap the instance; the host
     * disables the mod and shows the log line above. */
    __builtin_trap();
}

/* Replaces Lua's own luaL_loadfilex, which is compiled under a different name
 * (see build.sh: -DluaL_loadfilex=...) so that --gc-sections can drop it.
 *
 * This is not cosmetic. Lua's version calls freopen, and freopen drags in the
 * whole of stdio -- which meant the module imported env.__syscall_dup3, an
 * import outside the Gearbox ABI that the host refuses outright, plus fd_read,
 * fd_seek and fd_close. Cutting the one function it all hangs off takes the
 * module's WASI surface down to fd_write.
 *
 * There is no filesystem here, so failing is the honest answer. */
LUALIB_API int luaL_loadfilex(lua_State *L, const char *filename,
                              const char *mode) {
    (void)mode;
    lua_pushfstring(L, "cannot load '%s': a Gearbox mod has no filesystem",
                    filename ? filename : "=(stdin)");
    return LUA_ERRFILE;
}

/* --- core --------------------------------------------------------------- */

static int l_log(lua_State *L) {
    lua_Integer lvl = luaL_checkinteger(L, 1);
    size_t n = 0;
    const char *s = luaL_checklstring(L, 2, &n);
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    gearbox_log((gearbox_log_level)lvl, s, (uint32_t)n);
    return 0;
}

/* Replaces the stock `print`, which would write to a stdout no one reads. */
static int l_print(lua_State *L) {
    luaL_Buffer b;
    int argc = lua_gettop(L);
    luaL_buffinit(L, &b);
    for (int i = 1; i <= argc; i++) {
        size_t n = 0;
        const char *s = luaL_tolstring(L, i, &n);   /* honours __tostring */
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_addlstring(&b, s, n);
        lua_pop(L, 1);
    }
    luaL_pushresult(&b);
    {
        size_t n = 0;
        const char *s = lua_tolstring(L, -1, &n);
        gearbox_log(GEARBOX_LOG_INFO, s, (uint32_t)n);
    }
    lua_pop(L, 1);
    return 0;
}

static int l_env(lua_State *L) {
    lua_createtable(L, 0, 8);
#define SET_I(k, v) lua_pushinteger(L, (lua_Integer)(v)); lua_setfield(L, -2, k)
#define SET_B(k, v) lua_pushboolean(L, (int)(v));         lua_setfield(L, -2, k)
    SET_I("gearboxMajor", g_env.gearbox_major);
    SET_I("gearboxMinor", g_env.gearbox_minor);
    SET_I("hostVersion",  g_env.host_version);
    SET_I("platform",     g_env.platform);
    SET_B("isWeb",        g_env.is_web);
    SET_B("isHeadless",   g_env.is_headless);
    SET_I("screenW",      g_env.screen_w);
    SET_I("screenH",      g_env.screen_h);
#undef SET_I
#undef SET_B
    return 1;
}

static int l_abort(lua_State *L) {
    size_t n = 0;
    const char *s = luaL_checklstring(L, 1, &n);
    gearbox_abort(s, (uint32_t)n);
    return 0;   /* not reached */
}

static int l_fuel_budget(lua_State *L) {
    uint64_t f = gearbox_fuel_budget();
    /* Lua integers are signed 64-bit; UINT64_MAX (unmetered) would wrap to -1,
     * so report it as a float the script can compare against math.huge. */
    if (f == 0xFFFFFFFFFFFFFFFFull) lua_pushnumber(L, (lua_Number)HUGE_VAL);
    else                            lua_pushinteger(L, (lua_Integer)f);
    return 1;
}

/* --- gamestate.read ----------------------------------------------------- */
#if GBX_WITH_GAMESTATE

static int l_turn_number(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)gearbox_turn_number());
    return 1;
}

static int l_country_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)gearbox_country_count());
    return 1;
}

static int l_country_at(lua_State *L) {
    /* Lua is 1-based; the ABI is 0-based. Convert here so scripts read
     * naturally, and document it in the README. */
    lua_Integer i = luaL_checkinteger(L, 1);
    gearbox_country c = gearbox_country_at((uint32_t)(i - 1));
    if (c == GEARBOX_INVALID) lua_pushnil(L);
    else                      lua_pushinteger(L, (lua_Integer)c);
    return 1;
}

static int l_country_name(lua_State *L) {
    gearbox_country c = (gearbox_country)luaL_checkinteger(L, 1);
    uint32_t need = gearbox_country_name(c, NULL, 0);
    if (need == 0) { lua_pushliteral(L, ""); return 1; }

    luaL_Buffer b;
    char *dst = luaL_buffinitsize(L, &b, need);
    uint32_t got = gearbox_country_name(c, dst, need);
    if (got > need) got = need;     /* host grew the name between calls */
    luaL_pushresultsize(&b, got);
    return 1;
}

static int l_country_treasury(lua_State *L) {
    gearbox_country c = (gearbox_country)luaL_checkinteger(L, 1);
    lua_pushnumber(L, (lua_Number)gearbox_country_treasury(c));
    return 1;
}

static int l_country_province_count(lua_State *L) {
    gearbox_country c = (gearbox_country)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)gearbox_country_province_count(c));
    return 1;
}

static int l_province_population(lua_State *L) {
    gearbox_province p = (gearbox_province)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)gearbox_province_population(p));
    return 1;
}

static int l_province_owner(lua_State *L) {
    gearbox_province p = (gearbox_province)luaL_checkinteger(L, 1);
    gearbox_country c = gearbox_province_owner(p);
    if (c == GEARBOX_INVALID) lua_pushnil(L);
    else                      lua_pushinteger(L, (lua_Integer)c);
    return 1;
}

#endif /* GBX_WITH_GAMESTATE */

/* --- ui ----------------------------------------------------------------- */
#if GBX_WITH_UI

static int l_panel_register(lua_State *L) {
    size_t n = 0;
    const char *title = luaL_checklstring(L, 1, &n);
    uint32_t w = (uint32_t)luaL_optinteger(L, 2, 240);
    uint32_t h = (uint32_t)luaL_optinteger(L, 3, 120);
    lua_pushinteger(L, (lua_Integer)gearbox_panel_register(title, (uint32_t)n, w, h));
    return 1;
}

static int l_draw_text(lua_State *L) {
    gearbox_panel p = (gearbox_panel)luaL_checkinteger(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    uint32_t rgba = (uint32_t)luaL_checkinteger(L, 4);
    size_t n = 0;
    const char *s = luaL_checklstring(L, 5, &n);
    gearbox_draw_text(p, x, y, rgba, s, (uint32_t)n);
    return 0;
}

static int l_draw_rect(lua_State *L) {
    gearbox_panel p = (gearbox_panel)luaL_checkinteger(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    int32_t w = (int32_t)luaL_checkinteger(L, 4);
    int32_t h = (int32_t)luaL_checkinteger(L, 5);
    uint32_t rgba = (uint32_t)luaL_checkinteger(L, 6);
    gearbox_draw_rect(p, x, y, w, h, rgba);
    return 0;
}

static int l_button(lua_State *L) {
    gearbox_panel p = (gearbox_panel)luaL_checkinteger(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    int32_t w = (int32_t)luaL_checkinteger(L, 4);
    int32_t h = (int32_t)luaL_checkinteger(L, 5);
    size_t n = 0;
    const char *s = luaL_checklstring(L, 6, &n);
    lua_pushboolean(L, gearbox_button(p, x, y, w, h, s, (uint32_t)n) != 0);
    return 1;
}

#endif /* GBX_WITH_UI */

/* --- assets ------------------------------------------------------------- */
#if GBX_WITH_ASSETS

static int l_asset_size(lua_State *L) {
    size_t n = 0;
    const char *name = luaL_checklstring(L, 1, &n);
    lua_pushinteger(L, (lua_Integer)gearbox_asset_size(name, (uint32_t)n));
    return 1;
}

static int l_asset_read(lua_State *L) {
    size_t n = 0;
    const char *name = luaL_checklstring(L, 1, &n);
    uint32_t size = gearbox_asset_size(name, (uint32_t)n);
    if (size == 0) { lua_pushnil(L); return 1; }

    luaL_Buffer b;
    char *dst = luaL_buffinitsize(L, &b, size);
    uint32_t got = gearbox_asset_read(name, (uint32_t)n, dst, size);
    if (got > size) got = size;
    luaL_pushresultsize(&b, got);
    return 1;
}

#endif /* GBX_WITH_ASSETS */

/* --- registration ------------------------------------------------------- */

static const luaL_Reg gbx_funcs[] = {
    {"log",                 l_log},
    {"env",                 l_env},
    {"abort",               l_abort},
    {"fuelBudget",          l_fuel_budget},

#if GBX_WITH_GAMESTATE
    {"turnNumber",          l_turn_number},
    {"countryCount",        l_country_count},
    {"countryAt",           l_country_at},
    {"countryName",         l_country_name},
    {"countryTreasury",     l_country_treasury},
    {"countryProvinceCount",l_country_province_count},
    {"provincePopulation",  l_province_population},
    {"provinceOwner",       l_province_owner},
#endif

#if GBX_WITH_UI
    {"panelRegister",       l_panel_register},
    {"drawText",            l_draw_text},
    {"drawRect",            l_draw_rect},
    {"button",              l_button},
#endif

#if GBX_WITH_ASSETS
    {"assetSize",           l_asset_size},
    {"assetRead",           l_asset_read},
#endif
    {NULL, NULL}
};

/* Only the libraries that cannot reach outside the sandbox. io, os, package and
 * debug are deliberately absent: they are not merely unregistered here, their
 * .c files are never compiled (see build.sh), so nothing links a single
 * filesystem call into the module. */
static void gbx_open_libs(lua_State *L) {
    static const luaL_Reg libs[] = {
        {LUA_GNAME,       luaopen_base},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_COLIBNAME,   luaopen_coroutine},
        {NULL, NULL}
    };
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    /* dofile and loadfile exist in the base library and would call into stdio.
     * Nothing implements it here, so remove them rather than leave a trap. */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    lua_pushcfunction(L, l_print); lua_setglobal(L, "print");

    luaL_newlib(L, gbx_funcs);

    /* Log levels, so a script says gearbox.INFO rather than 1. */
    lua_pushinteger(L, GEARBOX_LOG_TRACE); lua_setfield(L, -2, "TRACE");
    lua_pushinteger(L, GEARBOX_LOG_INFO);  lua_setfield(L, -2, "INFO");
    lua_pushinteger(L, GEARBOX_LOG_WARN);  lua_setfield(L, -2, "WARN");
    lua_pushinteger(L, GEARBOX_LOG_ERROR); lua_setfield(L, -2, "ERROR");

    lua_setglobal(L, "gearbox");
}

/* --- export glue -------------------------------------------------------- */

/* Push script global `name` if it is a function. Returns 0 if absent. */
static int push_hook(const char *name) {
    if (!L_state) return 0;
    if (lua_getglobal(L_state, name) == LUA_TFUNCTION) return 1;
    lua_pop(L_state, 1);
    return 0;
}

/* With this build a Lua error traps inside gbx_lua_throw and never returns, so
 * the error branch is unreachable today. It is written out anyway so that
 * turning exceptions back on is a one-file change. */
static int call_hook(int nargs, int nres) {
    int rc = lua_pcall(L_state, nargs, nres, 0);
    if (rc != LUA_OK) {
        size_t n = 0;
        const char *msg = lua_tolstring(L_state, -1, &n);
        if (msg) gearbox_log(GEARBOX_LOG_ERROR, msg, (uint32_t)n);
        lua_pop(L_state, 1);
        return 0;
    }
    return 1;
}

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) {
    g_env.size = sizeof g_env;
    gearbox_env(&g_env);

    L_state = luaL_newstate();
    if (!L_state) {
        gearbox_log(GEARBOX_LOG_ERROR, S("lua: could not create interpreter"));
        return 1;
    }
    gbx_open_libs(L_state);

    if (luaL_loadbuffer(L_state, (const char *)gbx_script,
                        (size_t)gbx_script_len, gbx_script_name) != LUA_OK) {
        size_t n = 0;
        const char *msg = lua_tolstring(L_state, -1, &n);
        if (msg) gearbox_log(GEARBOX_LOG_ERROR, msg, (uint32_t)n);
        return 1;
    }
    if (!call_hook(0, 0)) return 1;     /* run the chunk: defines the hooks */

    if (push_hook("mod_load")) {
        if (!call_hook(0, 1)) return 1;
        lua_Integer rc = lua_tointeger(L_state, -1);
        lua_pop(L_state, 1);
        if (rc != 0) return (int32_t)rc;
    }
    return 0;
}

GEARBOX_EXPORT("mod_unload")
void mod_unload(void) {
    if (!L_state) return;
    if (push_hook("mod_unload")) call_hook(0, 0);
    lua_close(L_state);
    L_state = NULL;
}

GEARBOX_EXPORT("mod_draw_panel")
void mod_draw_panel(gearbox_panel panel, uint32_t w, uint32_t h) {
    if (!push_hook("mod_draw_panel")) return;
    lua_pushinteger(L_state, (lua_Integer)panel);
    lua_pushinteger(L_state, (lua_Integer)w);
    lua_pushinteger(L_state, (lua_Integer)h);
    call_hook(3, 0);
}

GEARBOX_EXPORT("mod_pre_turn")
void mod_pre_turn(uint32_t turn) {
    if (!push_hook("mod_pre_turn")) return;
    lua_pushinteger(L_state, (lua_Integer)turn);
    call_hook(1, 0);
}

GEARBOX_EXPORT("mod_post_turn")
void mod_post_turn(uint32_t turn) {
    if (!push_hook("mod_post_turn")) return;
    lua_pushinteger(L_state, (lua_Integer)turn);
    call_hook(1, 0);
}
