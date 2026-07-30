#!/usr/bin/env bash
# Prints a python command that actually RUNS, or nothing.
#
#   PY="$(tools/find_python.sh)" || exit 1
#   $PY tools/help.py --check          # unquoted: may be two words, "py -3"
#
# WHY EXISTENCE IS NOT THE TEST
#
# Windows ships a python3.exe stub in WindowsApps that is not python. It is on
# PATH, `command -v python3` finds it, and running it prints
#
#     Python was not found; run without arguments to install from the
#     Microsoft Store, or disable this shortcut from Settings > Apps >
#     Advanced app settings > App execution aliases.
#
# and exits. So every check in tests/run_all.sh that shelled out to python3 --
# the tool index, the generated bindings, the SDK bindings, the third-party
# notices, the flag licences and the GIF decode -- failed at once on a machine
# with a perfectly good Python installed as `python`. tools/qualify.sh's own
# `command -v python3` check passed, which is what made it confusing: the name
# resolves, the program does not.
#
# CI never saw it. actions/setup-python puts a real python3 ahead of the stub,
# so the only Windows machine this had run on was the one where the assumption
# held.
#
# Hence: run the candidate and see if it answers. `py -3` is tried early on
# Windows because the launcher is the documented way to find an interpreter
# there and it lives in the Windows directory, on PATH for every process.
set -u

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) candidates="python3 py-3 python" ;;
    *)                    candidates="python3 python" ;;
esac

for c in $candidates; do
    # py-3 in the list, "py -3" in the call: the list is whitespace-split, so a
    # two-word candidate cannot travel through it intact.
    cmd="$c"
    [ "$c" = "py-3" ] && cmd="py -3"
    # Asks python to identify itself. The stub prints its advertisement to
    # stdout and exits non-zero, so both the status and the output are checked --
    # a stub that ever starts exiting 0 would still not print "3".
    if out="$($cmd -c 'import sys; print(sys.version_info[0])' 2>/dev/null)" \
       && [ "$out" = "3" ]; then
        echo "$cmd"
        exit 0
    fi
done

exit 1
