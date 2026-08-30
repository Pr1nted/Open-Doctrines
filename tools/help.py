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
        "release.py", "odver.py", "package.py", "build.py", "screenshots.sh",
        "itch-cover.py", "banner.py", "package_android.sh",
    ]),
    ("The mod ABI and SDKs", [
        "gen_bindings.py", "gen_abi_docs.py", "gen_wiki.py", "publish_wiki.py",
        "check_bindings.py", "check_abi_compat.py", "wasm_imports.py",
        "pack_odmod.sh",
        "sdk_toolchains.sh", "test_all_sdks.sh", "find_python.sh",
    ]),
    ("Testing multiplayer", [
        "playtest.sh", "second_player.sh",
    ]),
    ("Qualifying a platform", [
        "qualify.sh", "qualify_docker.sh",
        "gen_server_raylib_stubs.py",
        # Serves the web build and collects the phone's console over the
        # network, because Safari's inspector needs a cable and the failure
        # being chased kills the tab it would be printed in.
        "devlog_server.py",
    ]),
    ("Training the AI", [
        # Re-deflates the shipped model container with zopfli. Same format,
        # same weights, ~115 KB smaller; miniz inflates it unchanged.
        "shrink_model.py",
        "train.sh", "train_parallel.py", "ai_benchmark.sh", "ai_bench.py",
        # The A/B ruler. --eval-ai's ADVANTAGE line is a ratio and explodes
        # when the control cohort is nearly wiped out; this reads land share
        # over several fixed worlds instead, and refuses to call a difference
        # real when it sits inside the spread.
        "ai_ab.py",
        "od_bench.py",
        # Concurrency gate for game processes: each spikes ~2 GB loading the
        # map, and four at once is what put a 16 GB machine into swap.
        "odlock.py",
    ]),
    ("Inspecting game files", [
        "read_odsv.py", "package_odmap.py", "generate_map_thumb.py",
        "generate_sample_odsv.py", "generate_zero_turn_odsv.py",
    ]),
    ("Building the world data", [
        "run_pipeline.py", "generate_scenario.py", "overlay_real_data.py",
        "download_external_data.py",
        "province_geo.py", "analyze_provinces.py", "build_iso_map.py",
        "generate_province_populations.py", "generate_minorities.py",
        "generate_minority_policies.py", "generate_political_compass.py",
        "generate_relations.py", "generate_resources.py", "generate_claims.py",
        "generate_navy.py", "generate_ships_fast.py", "boost_economies.py",
        "fix_1939_history.py", "fix_map_history.py", "fix_map_colors.py",
        "carve_states.py", "carve_borders.py",
        "check_map_history.py", "reanchor_map_history.py", "fetch_ohm_borders.py",
        "fill_water_speckle.py", "fix_naval_layer.py", "naval_placement.py",
        "rebuild_map_preview.py", "check_map_integrity.py",
        "odmap_pack.py", "shrink_maps.py",
        # How both the .odmap archives and the release zip are deflated: an
        # ordinary zip, found by searching much harder for it.
        "zopfli_zip.py",
        # The same lossless re-encode for the images that are NOT inside an
        # archive: the downloaded flags, the app icon, the map thumbnails.
        "shrink_pngs.py",
        "make_tutorial_map.py",
        # Province geometry repair, applied to the archives that already ship
        # rather than only to maps generated from here on.
        "fix_map_geometry.py",
        # A .odmap carries its own copy of the data files a map needs, and the
        # archive's copy wins over data/ -- so a data fix is not shipped until
        # it has been written into every archive too.
        "update_odmap_member.py",
    ]),
    ("Doctrine data", [
        # policies.json is generated, and its advertised numbers are derived
        # from the effects the resolver actually reads -- gen_policies.py
        # authors it, check_policies.py is the gate that keeps the two one fact
        # rather than two that agree today.
        "gen_policies.py", "check_policies.py",
    ]),
    ("Translating the game", [
        "i18n_extract.py", "i18n_sync.py", "i18n_put.py", "i18n_wrap.py",
        "i18n_merge.py", "i18n_quality.py", "i18n_fit.py",
        # Dialogue rather than UI strings, but the job is the same one: check a
        # translation against the English it came from. A .oddlg is prose
        # wrapped around machinery -- page breaks, :: directives, choice keys --
        # and the machinery has to survive translation intact.
        "dialog_check.py",
    ]),
    ("Licensing and provenance", [
        "gen_notices.py", "audit_flag_licenses.py", "check_data_licences.py",
        "check_flag_dates.py",
    ]),
    ("Flags, icons and symbols", [
        "download_flags.py", "download_flags_fast.py",
        "download_scenario_flags.py", "attach_scenario_flags.py",
        "patch_flag_images.py",
        "generate_flag_svgs.py", "prerender_problematic_flags.py",
        "fix_censored_flags.py",
        "download_wiki_symbols.py", "download_symbols.py",
        "generate_svg_symbols.py",
        "generate_symbols.py",
        "normalize_symbols.py", "restore_symbols.py", "inline_svg_use.py",
        "sync_map_symbols.py",
        "generate_icons.py", "generate_web_favicon.py", "make_watermark.py",
        "subset_font.py", "optimize_flag_svgs.py",
        # The lossless re-encode for the audio: the same packets in fuller
        # Ogg pages, and OptiVorbis over the bitstream.
        "shrink_audio.py",
        # Which OptiVorbis needs: raylib's stb_vorbis rejects a codebook with no
        # entries, and the shipped audio has them. CMakeLists applies this to
        # the raylib it builds, and fails the build if it cannot.
        "patch_raylib_vorbis.py",
        # The relations view says who is at war and who is allied with hue
        # alone, so whether those four can be told apart without normal colour
        # vision is a measurement rather than a claim.
        "check_palette.py",
    ]),
    ("The communication window", [
        "kra_export.py", "comms_signal.py", "comms_derive.py", "make_voices.py",
    ]),
]


def first_line(path):
    """The summary line, taken from the script itself."""
    try:
        if path.endswith(".py"):
            doc = ast.get_docstring(ast.parse(open(path).read())) or ""
            return doc.strip().split("\n")[0]
        # Shell scripts: the first comment line after the shebang that says
        # something. A bare "#" spacer above the summary is ordinary style, and
        # reading it as "no header" would only force every script into one
        # layout to satisfy the checker.
        with open(path) as f:
            for line in f:
                if line.startswith("#!"):
                    continue
                if line.startswith("#"):
                    text = line.lstrip("# ").strip()
                    if text:
                        return text
                    continue
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
