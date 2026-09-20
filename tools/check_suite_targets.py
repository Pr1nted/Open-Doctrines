#!/usr/bin/env python3
"""Does tests/run_all.sh build every test it runs?

    python3 tools/check_suite_targets.py tests/run_all.sh

WHY THIS EXISTS

The suite names its targets twice: once in the `cmake --build --target ...`
line that compiles them, and once per `run "label" "$bin/XxxTest"`. Nothing
made the two agree, and six tests ended up in the second list and not the
first: WorldProvenanceTest, ModProtectedTest, CountryFieldsTest,
ScriptCommandsTest, ModRenderLayerTest and ModContentTest.

On a developer's machine that is invisible. The binaries are there from an
earlier incremental build, so the suite runs them and passes. On a clean
checkout -- which is every CI run -- the binaries do not exist, `run` reports
six failures, and the first thing anybody learns is that a release failed with
a suite that had been green all day. That is exactly what happened to the
Gearbox 1.3 release on 2026-09-20.

A test that is run but not built is worse than a missing test, because it
reports as passing.
"""
import re
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tests/run_all.sh"
    with open(path) as f:
        src = f.read()

    # The build list is the span between the --target flag and the -j that ends
    # it. Read from the file rather than restated here, so this cannot drift
    # from the thing it checks.
    try:
        start = src.index("--target")
        end = src.index("-j\"$(getconf", start)
    except ValueError:
        print("  FAILED   could not find the --target list in " + path)
        return 1

    built = set(re.findall(r"\b([A-Z]\w*Test)\b", src[start:end]))
    run = set(re.findall(r'"\$bin/([A-Z]\w*Test)"', src))

    missing = sorted(run - built)
    if missing:
        print("  FAILED   run_all.sh runs %d test(s) it never builds:" % len(missing))
        for m in missing:
            print("             " + m)
        print("           They pass on a tree that already has the binaries and")
        print("           fail on every clean checkout. Add them to --target.")
        return 1

    # The other direction is not an error: a target may be built to prove it
    # compiles and driven somewhere else, or not driven at all on this platform.
    extra = len(built - run)
    print("  ok       %d test(s) run, all of them built%s"
          % (len(run), " (%d built but not run here)" % extra if extra else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
