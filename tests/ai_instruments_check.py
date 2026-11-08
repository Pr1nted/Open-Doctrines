#!/usr/bin/env python3
"""Does tools/check_ai_instruments.py still guard anything?

That tool asks whether every OD_* switch docs/ai/LOOP.md names is read by some
file in src/ or tools/, and it refuses to answer when its read looks broken --
the canary, OD_ENV, which every gate in the codebase goes through (journal 527,
commit 45fa20a). The canary works ONLY because the needles and the haystack are
different files: OD_ENV is also written in LOOP.md, BACKLOG.md and
LOOP_JOURNAL.md, so a haystack that included docs/ would contain the canary and
every needle by construction. The check would not fail. It would pass forever,
which is the one failure mode a guard cannot report itself.

So this makes that edit -- one docs pattern prepended to HAYSTACK in a COPY of
the tool -- and requires the copy to refuse. A comment saying "never add docs/"
is advice; this is a failure.

Three arms run first so the fourth means something: the tool must be able to
pass, to report a real phantom, and to refuse an empty read, on trees this
harness builds. Without them a refusal in the trap arm could be the harness
mis-assembling a tree rather than the guard working.

    tests/ai_instruments_check.py
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "check_ai_instruments.py")
fail = 0


def tree(dirname, *, loop, src=None, docs_in_haystack=False):
    """A miniature OpenDoctrines: docs/ai/LOOP.md, a src/ file, the tool."""
    os.makedirs(os.path.join(dirname, "docs", "ai"))
    os.makedirs(os.path.join(dirname, "tools"))
    io.open(os.path.join(dirname, "docs", "ai", "LOOP.md"), "w",
            encoding="utf-8").write(loop)
    if src is not None:
        os.makedirs(os.path.join(dirname, "src"))
        io.open(os.path.join(dirname, "src", "Game.cpp"), "w",
                encoding="utf-8").write(src)
    copy = os.path.join(dirname, "tools", "check_ai_instruments.py")
    if not docs_in_haystack:
        shutil.copyfile(TOOL, copy)
        return copy
    # THE EDIT UNDER TEST, made textually on the copy rather than by patching
    # an imported module, so the file that runs is the real tool with the real
    # mistake in it. The assert is load-bearing: if HAYSTACK is ever renamed,
    # this test must fail loudly rather than quietly run an unmodified tool
    # and report a pass it did not earn.
    text = io.open(TOOL, encoding="utf-8").read()
    needle = "HAYSTACK = ("
    assert text.count(needle) == 1, (
        "tools/check_ai_instruments.py no longer defines HAYSTACK once -- this "
        "test cannot make the edit it exists to catch")
    io.open(copy, "w", encoding="utf-8").write(
        text.replace(needle, needle + '"docs/ai/*.md", ', 1))
    return copy


def case(name, want_zero, **kw):
    """Run the tool in a throwaway tree; want_zero says which answer is right."""
    global fail
    work = tempfile.mkdtemp(prefix="ai-instruments-")
    try:
        tool = tree(work, **kw)
        env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
        p = subprocess.run([sys.executable, tool], capture_output=True,
                           text=True, env=env)
        ok = (p.returncode == 0) if want_zero else (p.returncode != 0)
        out = (p.stdout + p.stderr).strip().replace("\n", "\n        ")
        print(f"{'ok  ' if ok else 'FAIL'}  {name}")
        if not ok:
            fail += 1
            print(f"        exit {p.returncode}, wanted "
                  f"{'0' if want_zero else 'non-zero'}")
        if out:
            print(f"        {out}")
    finally:
        shutil.rmtree(work, ignore_errors=True)


LOOP = ("# LOOP\n\nOD_FOO switches the thing off, OD_BAR scales it.\n")

# 1. The pass path is reachable on a tree this harness builds.
case("a tree whose code reads both switches passes", True,
     loop=LOOP, src="getenv(OD_ENV); OD_FOO; OD_BAR;\n")

# 2. And the check still has teeth: a switch nothing reads is named.
case("a switch no code reads is reported", False,
     loop=LOOP, src="getenv(OD_ENV); OD_FOO;\n")

# 3. The original Windows failure (b6c70d3): a read that found nothing must
#    say so, not report all of LOOP.md as phantom.
case("an empty read refuses instead of reporting phantoms", False,
     loop=LOOP, src=None)

# 4. THE TRAP. docs/ai/*.md in the haystack: docs/ai/LOOP.md supplies OD_ENV
#    and both needles, so the unguarded tool prints "all 2 switches exist"
#    and exits 0 while reading no code at all. Only the exit code is asserted
#    -- refusing loudly about docs/ and skipping docs/ then reporting the two
#    phantoms are both correct answers; passing is the only wrong one.
case("docs/ in the haystack cannot satisfy the check", False,
     loop=LOOP, src="getenv(OD_ENV);\n", docs_in_haystack=True)

if fail:
    print(f"ai instruments guard: {fail} case(s) failed")
sys.exit(1 if fail else 0)
