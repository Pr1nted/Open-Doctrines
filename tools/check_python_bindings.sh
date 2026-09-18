#!/usr/bin/env bash
# Do the generated Python bindings actually compile?
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
    echo "  SKIPPED  no python3-config; the generated bindings were NOT compiled"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
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
    exit 0
fi
echo "  FAILED   the generated Python bindings do not compile"
grep "error:" "$work/err" | head -10 | sed 's/^/      /'
exit 1
