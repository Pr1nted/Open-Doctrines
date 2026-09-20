#!/usr/bin/env bash
# Do the generated bindings actually compile? Python, Lua and JS.
#
# check_bindings.py is a TEXT lint -- it says so itself: "it cannot tell you
# the argument order is right". The Python binding is 2,300 generated lines
# calling 178 host functions, and the failure it cannot catch is exactly the
# one generation makes likely: a parameter pack that produces the wrong arity
# or the wrong type for a symbol declared in sdk/gearbox.h.
#
# So this asks the compiler. Host-side, with every capability group switched
# on, which is stricter than any real mod: a real one compiles a subset.
#
# It needs CPython's headers. When they are absent this SKIPS AND SAYS SO
# rather than passing quietly -- a skip that reads like a pass is how an
# unbuilt binding ships.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"

inc="$(python3-config --includes 2>/dev/null || true)"
if [ -z "$inc" ]; then
    echo "  SKIPPED  no python3-config; the generated Python bindings were NOT compiled"
fi

rc=0
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Every capability on: a real mod compiles a subset, so this superset
# catches a group nothing else builds.
caps() {
    python3 - "$root/sdk/abi.json" <<'CAPEOF'
import json, sys
a = json.load(open(sys.argv[1]))
cs = sorted({i.get("capability") for i in a["imports"]
             if i.get("capability") and i.get("capability") != "WasiStub"})
for c in cs:
    print(f'#define GBX_WITH_{c.upper().replace(".", "_")} 1')
print('#define GBX_WITH_UI 1')
print('#define GBX_WITH_GAMESTATE 1')
print('#define GBX_WITH_ASSETS 1')
CAPEOF
}

{
    echo '#define PY_SSIZE_T_CLEAN'
    echo '#include <Python.h>'
    echo '#include "gearbox.h"'
    # Every capability on: a real mod compiles a subset, so this is the
    # superset and catches a group nothing else builds.
    python3 - "$root/sdk/abi.json" <<'PYEOF'
import json, sys
a = json.load(open(sys.argv[1]))
caps = sorted({i.get("capability") for i in a["imports"]
               if i.get("capability") and i.get("capability") != "WasiStub"})
for c in caps:
    print(f'#define GBX_WITH_{c.upper().replace(".", "_")} 1')
print('#define GBX_WITH_UI 1')
print('#define GBX_WITH_GAMESTATE 1')
print('#define GBX_WITH_ASSETS 1')
PYEOF
    echo '#include "gearbox_py_generated.h"'
    echo 'static PyMethodDef m[] = {'
    echo '#include "gearbox_py_methods.inc"'
    echo '    {NULL, NULL, 0, NULL}'
    echo '};'
    echo 'int main(void) { (void)m; return 0; }'
} > "$work/check.c"

# shellcheck disable=SC2086
if cc -fsyntax-only -Wno-unknown-attributes \
      -I "$root/sdk" -I "$root/sdk/python" $inc "$work/check.c" 2>"$work/err"; then
    n=$(grep -c '^static PyObject \*gbxpy_' "$root/sdk/python/gearbox_py_generated.h")
    echo "  ok       $n generated Python bindings compile"
else
    echo "  FAILED   the generated Python bindings do not compile"
    grep "error:" "$work/err" | head -10 | sed 's/^/      /'
    rc=1
fi

# ---- Lua -------------------------------------------------------------------
lua_src="$(find "$root/sdk/lua" -name lua.h -path '*/src/*' 2>/dev/null | head -1)"
if [ -z "$lua_src" ]; then
    echo "  SKIPPED  no vendored Lua headers; the generated bindings were NOT compiled"
else
    caps > "$work/caps.h"
    {
        echo '#include "lua.h"'
        echo '#include "lauxlib.h"'
        echo '#include "gearbox.h"'
        cat "$work/caps.h"
        echo '#include "gearbox_lua_generated.h"'
        echo 'static const luaL_Reg f[] = {'
        echo '#include "gearbox_lua_funcs.inc"'
        echo '    {NULL, NULL}'
        echo '};'
        echo 'int main(void) { (void)f; return 0; }'
    } > "$work/lua.c"
    if cc -fsyntax-only -Wno-unknown-attributes -I "$root/sdk" -I "$root/sdk/lua"           -I "$(dirname "$lua_src")" "$work/lua.c" 2>"$work/luaerr"; then
        n=$(grep -c '^static int gbxlua_' "$root/sdk/lua/gearbox_lua_generated.h")
        echo "  ok       $n generated Lua bindings compile"
    else
        echo "  FAILED   the generated Lua bindings do not compile"
        grep "error:" "$work/luaerr" | head -10 | sed 's/^/      /'
        rc=1
    fi
fi

# ---- JS (QuickJS) ----------------------------------------------------------
qjs="$(find "$root/sdk/js" -name quickjs.h 2>/dev/null | head -1)"
if [ -z "$qjs" ]; then
    echo "  SKIPPED  no vendored QuickJS headers; the generated bindings were NOT compiled"
else
    {
        echo '#include "quickjs.h"'
        echo '#include "gearbox.h"'
        echo '#include <math.h>'
        cat "$work/caps.h"
        echo 'static int arg_i32(JSContext *c, JSValueConst v, int32_t *o) { return JS_ToInt32(c, o, v) == 0; }'
        echo '#include "gearbox_js_generated.h"'
        echo 'static const JSCFunctionListEntry f[] = {'
        echo '#include "gearbox_js_funcs.inc"'
        echo '};'
        echo 'int main(void) { (void)f; (void)arg_i32; return 0; }'
    } > "$work/js.c"
    if cc -fsyntax-only -Wno-unknown-attributes -Wno-unused-function           -I "$root/sdk" -I "$root/sdk/js" -I "$(dirname "$qjs")"           "$work/js.c" 2>"$work/jserr"; then
        n=$(grep -c '^static JSValue gbxjs_' "$root/sdk/js/gearbox_js_generated.h")
        echo "  ok       $n generated JS bindings compile"
    else
        echo "  FAILED   the generated JS bindings do not compile"
        grep "error:" "$work/jserr" | head -10 | sed 's/^/      /'
        rc=1
    fi
fi

# ---- the hosts' OWN defaults ------------------------------------------------
#
# Everything above compiles with EVERY capability defined, which is a superset
# no real mod uses -- and that is why it could not see the break it was written
# to catch. The generated bindings gate each group on the ABI's capability name
# (GBX_WITH_GAMESTATE_READ), while the three hosts defaulted a group called
# GBX_WITH_GAMESTATE, which nothing reads. Every script mod rebuilt after that
# split lost turnNumber and every other GameState.Read function, and died on
# "attempt to call a nil value" at its first draw. The superset build stayed
# green throughout.
#
# So: each GBX_WITH_* a host names must be one the generated code actually
# gates on. A macro nothing reads is a capability the host thinks it enabled.
python3 - "$root" <<'DEFEOF' || rc=1
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
hosts = {
    "lua":    ("sdk/lua/gearbox_lua.c",  ["sdk/lua/gearbox_lua_generated.h",
                                          "sdk/lua/gearbox_lua_funcs.inc"]),
    "python": ("sdk/python/gearbox_py.c", ["sdk/python/gearbox_py_generated.h",
                                           "sdk/python/gearbox_py_methods.inc"]),
    "js":     ("sdk/js/gearbox_qjs.c",   ["sdk/js/gearbox_js_generated.h",
                                          "sdk/js/gearbox_js_funcs.inc"]),
}
bad = 0
for lang, (host, generated) in hosts.items():
    used = set()
    for g in generated:
        used |= set(re.findall(r"GBX_WITH_[A-Z_]+", (root / g).read_text()))
    # Comments stripped first: these files EXPLAIN the macro that went wrong,
    # and a check that reads prose as code fails on its own documentation.
    src = re.sub(r"/\*.*?\*/", " ", (root / host).read_text(), flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    named = set(re.findall(r"GBX_WITH_[A-Z_]+", src))
    orphans = sorted(named - used)
    if orphans:
        print("  FAILED   %s names %s, which the generated bindings never read"
              % (host, ", ".join(orphans)))
        print("           (the group it meant to switch on stays off)")
        bad = 1
    else:
        print("  ok       %-6s capability macros all reach a generated group" % lang)
sys.exit(bad)
DEFEOF

exit $rc
