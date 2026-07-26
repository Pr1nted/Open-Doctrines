#!/usr/bin/env python3
"""What every tool in tools/ is for.

    tools/help.py                 the index, grouped by what you are trying to do
    tools/help.py release         the full help for one tool
    tools/help.py --check         verify every tool is described (used by CI)

The index is built from each script's own docstring rather than hand-written,
so a tool cannot end up described one way here and behaving another. If you add
a tool, give it a docstring and put its name in a group below; --check fails if
you forget the second part, which is how an undocumented tool gets noticed.
"""

import ast
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")

# Grouped by the question you are trying to answer, not by file type. A flat
# list of forty scripts is not help.
GROUPS = [
    ("Releasing", [
        "release.py", "odver.py", "package.py", "build.py",
    ]),
    ("The mod ABI and SDKs", [
        "gen_bindings.py", "gen_abi_docs.py", "gen_wiki.py", "publish_wiki.sh",
        "check_bindings.py", "wasm_imports.py", "pack_odmod.sh",
        "sdk_toolchains.sh", "test_all_sdks.sh",
    ]),
    ("Inspecting game files", [
        "read_odsv.py", "package_odmap.py", "generate_map_thumb.py",
        "generate_sample_odsv.py", "generate_zero_turn_odsv.py",
    ]),
    ("Building the world data", [
        "run_pipeline.py", "overlay_real_data.py", "download_external_data.py",
        "province_geo.py", "analyze_provinces.py", "build_iso_map.py",
        "generate_province_populations.py", "generate_minorities.py",
        "generate_minority_policies.py", "generate_political_compass.py",
        "generate_relations.py", "generate_resources.py", "generate_claims.py",
        "generate_navy.py", "generate_ships_fast.py", "boost_economies.py",
    ]),
    ("Flags, icons and symbols", [
        "download_flags.py", "download_flags_fast.py", "patch_flag_images.py",
        "generate_flag_svgs.py", "prerender_problematic_flags.py",
        "download_wiki_symbols.py", "generate_svg_symbols.py",
        "normalize_symbols.py", "restore_symbols.py", "inline_svg_use.py",
        "generate_icons.py",
    ]),
]


def first_line(path):
    """The summary line, taken from the script itself."""
    try:
        if path.endswith(".py"):
            doc = ast.get_docstring(ast.parse(open(path).read())) or ""
            return doc.strip().split("\n")[0]
        # Shell scripts: the first real comment line after the shebang.
        with open(path) as f:
            for line in f:
                if line.startswith("#!"):
                    continue
                if line.startswith("#"):
                    return line.lstrip("# ").strip()
                if line.strip():
                    break
    except Exception:
        pass
    return ""


def full_doc(path):
    if path.endswith(".py"):
        return ast.get_docstring(ast.parse(open(path).read())) or "(no docstring)"
    out = []
    with open(path) as f:
        for line in f:
            if line.startswith("#!"):
                continue
            if line.startswith("#"):
                out.append(line.lstrip("#").rstrip())
            elif line.strip():
                break
    return "\n".join(out) or "(no header comment)"


def all_tools():
    return sorted(f for f in os.listdir(TOOLS)
                  if (f.endswith(".py") or f.endswith(".sh"))
                  and f != "help.py")


def resolve(name):
    """Accepts 'release', 'release.py' or a path."""
    for cand in (name, name + ".py", name + ".sh"):
        p = os.path.join(TOOLS, cand)
        if os.path.exists(p):
            return p
    return None


def index():
    print(__doc__.strip().split("\n")[0])
    grouped = {t for _, ts in GROUPS for t in ts}
    for title, names in GROUPS:
        print(f"\n{title}")
        for n in names:
            p = os.path.join(TOOLS, n)
            if not os.path.exists(p):
                print(f"  {n:34s} (missing)")
                continue
            print(f"  {n:34s} {first_line(p)[:60]}")

    rest = [t for t in all_tools() if t not in grouped]
    if rest:
        print("\nUngrouped")
        for n in rest:
            print(f"  {n:34s} {first_line(os.path.join(TOOLS, n))[:60]}")

    print("\nFor one tool:  tools/help.py <name>")


def check():
    """CI gate: every tool is grouped and has a summary line."""
    grouped = {t for _, ts in GROUPS for t in ts}
    problems = []
    for t in all_tools():
        if t not in grouped:
            problems.append(f"{t} is not in any group in tools/help.py")
        if not first_line(os.path.join(TOOLS, t)):
            problems.append(f"{t} has no docstring or header comment")
    for _, names in GROUPS:
        for n in names:
            if not os.path.exists(os.path.join(TOOLS, n)):
                problems.append(f"{n} is listed in a group but does not exist")
    if problems:
        print("tools/help.py is out of date:")
        for p in problems:
            print(f"    {p}")
        return 1
    print(f"ok    all {len(all_tools())} tools are grouped and described")
    return 0


def main():
    args = [a for a in sys.argv[1:]]
    if "--check" in args:
        return check()
    if not args:
        index()
        return 0

    p = resolve(args[0])
    if not p:
        print(f"no such tool: {args[0]}\n", file=sys.stderr)
        index()
        return 1

    print(f"=== tools/{os.path.basename(p)} ===\n")
    print(full_doc(p))
    # Scripts with real argument parsing can say more than their docstring.
    if p.endswith(".py"):
        r = subprocess.run([sys.executable, p, "--help"],
                           capture_output=True, text=True, cwd=ROOT)
        if r.returncode == 0 and "usage:" in r.stdout:
            print("\n--- arguments ---\n")
            print(r.stdout.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
