#!/usr/bin/env python3
"""
OpenDoctrines Data Pipeline
============================
Orchestrates the full data generation pipeline:
   1. Download external data (USGS, WPI — public domain only, see NOTICE.md)
   2. Run MapGenerator (generates provinces, base map, initial gravity-model pop)
   3. Save initial map.odmap to STDmaps/
   4. Override population with corrected province populations
   5. Generate all game data (armies, ports, ships, resources, claims, compass, policies)
   6. Package final .odmap (without MapGenerator overwriting corrected data)
   7. Copy final map.odmap to STDmaps/
   8. Generate zero-turn save
"""

import json
import os
import shutil
import subprocess
import sys
import time

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
STDMAPS_DIR = os.path.join(DATA_DIR, "STDmaps")
TOOLS_DIR = os.path.join(PROJECT_ROOT, "tools")
BUILD_DIR = os.path.join(PROJECT_ROOT, "cmake-build-debug")
MAPGEN = os.path.join(BUILD_DIR, "MapGenerator")

os.makedirs(STDMAPS_DIR, exist_ok=True)


def log(msg):
    print(f"\n{'='*60}")
    print(f"  {msg}")
    print(f"{'='*60}")


def run(cmd, desc="Running"):
    print(f"\n  $ {subprocess.list2cmdline(cmd) if isinstance(cmd, list) else cmd}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if result.returncode != 0:
        print(f"  FAILED: {desc} (exit code {result.returncode})")
        sys.exit(1)
    return result


def run_ignore_fail(cmd, desc="Running"):
    print(f"\n  $ {subprocess.list2cmdline(cmd) if isinstance(cmd, list) else cmd}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if result.returncode != 0:
        print(f"  WARNING: {desc} failed (exit code {result.returncode}), continuing...")
    return result


def step(n, desc):
    log(f"Step {n}: {desc}")


def main():
    # ── Step 1: Download external data ──────────────────────────
    step(1, "Download external data (USGS, WPI)")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "download_external_data.py")],
        "external data download",
    )

    # ── Step 2: MapGenerator ──────────────────────────────────────
    step(2, "MapGenerator — base map + gravity-model population")
    run([MAPGEN], "MapGenerator")

    # ── Step 3: Save initial map for later repackaging ────────────
    step(3, "Save initial map.odmap to STDmaps/")
    src = os.path.join(DATA_DIR, "map.odmap")
    dst = os.path.join(STDMAPS_DIR, "map.odmap")
    shutil.copy2(src, dst)
    print(f"  Copied {src} -> {dst}")

    # ── Step 4: Download real flag SVGs from Wikimedia ──
    step(4, "Download real flag SVGs from Wikimedia Commons")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "download_flags_fast.py"), "--delay", "0.3"],
        "Wikimedia flag download",
    )

    # ── Step 5: Inline <use> elements in SVGs (nanosvg compatibility) ──
    step(5, "Inline <use> elements in flag + symbol SVGs for nanosvg")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "inline_svg_use.py")],
        "SVG <use> inlining (flags)",
    )
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "inline_svg_use.py"), "--path",
         os.path.join(DATA_DIR, "symbols")],
        "SVG <use> inlining (symbols)",
    )
    # Normalize symbol SVGs to canonical viewBox='-100 -100 200 200' for uniform sizing
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "normalize_symbols.py")],
        "Normalize symbol SVGs to canonical viewBox",
    )

    # ── Step 5b: Historical flags for the scenario powers ─────────
    # Before step 6, so the same rasteriser handles them.
    step("5b", "Download historical flags for scenario powers")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "download_scenario_flags.py")],
        "scenario flag download",
    )

    # ── Step 6: Pre-render problem SVGs to PNG (nanosvg doesn't support clipPath)
    step(6, "Pre-render SVGs with clipPath/negative viewBox to PNG (rsvg-convert)")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "prerender_problematic_flags.py")],
        "prerender problematic flags to PNG",
    )

    # ── Step 7: Generate corrected province populations ──────────
    step(7, "Generate corrected province populations (World Bank + city-weighted)")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_province_populations.py")],
        "province population generation",
    )

    # ── Step 8: Override population.json with corrected data ──────
    step(8, "Override population.json with corrected province populations")
    corrected = os.path.join(DATA_DIR, "province_population_2000.json")
    pop_json = os.path.join(DATA_DIR, "population.json")
    if os.path.exists(corrected):
        shutil.copy2(corrected, pop_json)
        print(f"  Copied {corrected} -> {pop_json}")
    else:
        print(f"  WARNING: {corrected} not found, keeping MapGenerator's gravity-model population")

    # ── Step 9: Overlay real data ─────────────────────────────────
    step(9, "Overlay real data (armies, ports, resources, minorities)")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "overlay_real_data.py")],
        "real data overlay",
    )

    # ── Step 9b: Generate minorities ──────────────────────────────
    # Separate from step 9 because it is no longer derived from a shapefile:
    # overlay_real_data.py writes province_centers.json and this reads it
    # together with tools/data/ethnic_groups.json. Must run before step 13,
    # which keys its policy overrides off the group names produced here.
    step("9b", "Generate per-province ethnic composition")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_minorities.py")],
        "minorities generation",
    )

    # ── Step 10: Generate ships ──────────────────────────────────
    step(10, "Generate ships from ports")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_ships_fast.py")],
        "ship generation",
    )

    # ── Step 11: Generate claims ──────────────────────────────────
    step(11, "Generate territorial claims")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_claims.py")],
        "claims generation",
    )

    # ── Step 12: Generate political compass ───────────────────────
    step(12, "Generate political compass")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_political_compass.py")],
        "political compass",
    )

    # ── Step 13: Generate minority policies ───────────────────────
    step(13, "Generate per-country minority policies")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_minority_policies.py")],
        "minority policies",
    )

    # ── Step 14: Generate relations ───────────────────────────────
    step(14, "Generate diplomatic relations")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_relations.py")],
        "relations generation",
    )

    # ── Step 15: Add starting treasury to countries.json ──────────
    step(15, "Add starting treasury to countries.json")
    countries_path = os.path.join(DATA_DIR, "countries.json")
    resources_path = os.path.join(DATA_DIR, "resources.json")
    provinces_path = os.path.join(DATA_DIR, "provinces.json")
    if os.path.exists(countries_path) and os.path.exists(resources_path) and os.path.exists(provinces_path):
        try:
            with open(countries_path) as f:
                countries = json.load(f)
            with open(resources_path) as f:
                resources = json.load(f)
            with open(provinces_path) as f:
                provinces = json.load(f)
            # Compute per-country total income from resources
            country_income = {}
            for pid_str, prov in provinces.items():
                cid = prov.get("country_id", 0)
                if cid == 0 or cid == 65534:  # skip unclaimed
                    continue
                iso = prov.get("iso_a3", "")
                if not iso:
                    continue
                income = 0.0
                if pid_str in resources and "industry" in resources[pid_str]:
                    income = resources[pid_str]["industry"].get("income", 0.0) + \
                             resources[pid_str]["industry"].get("resourceIncome", 0.0) + \
                             resources[pid_str]["industry"].get("popIncome", 0.0)
                country_income[iso] = country_income.get(iso, 0.0) + income
            # Set treasury = exactly net income (min 10 to avoid broke-start for minors)
            for cid_str, c in countries.items():
                iso = c.get("iso_a3", "")
                inc = country_income.get(iso, 0.0) if iso else 0.0
                treasury = max(10.0, round(inc, 2))
                c["treasury"] = treasury
            with open(countries_path, "w") as f:
                json.dump(countries, f, indent=2)
            print(f"  Updated treasury for {len(countries)} countries in countries.json")
        except Exception as e:
            print(f"  WARNING: Could not add treasury: {e}")
    else:
        print("  WARNING: countries.json or resources.json not found, skipping treasury")

    # ── Step 16: Copy policies.json and starting_policies.json from dist ──
    step(16, "Copy policies.json and starting_policies.json from dist to data/")
    for fname in ["policies.json", "starting_policies.json"]:
        for d in ["OpenDoctrines-Web", "OpenDoctrines-Linux", "OpenDoctrines-Windows"]:
            src = os.path.join(PROJECT_ROOT, "dist", d, "data", fname)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(DATA_DIR, fname))
                print(f"  Copied {src} -> {DATA_DIR}/{fname}")
                break
        else:
            print(f"  WARNING: {fname} not found in any dist/ directory")

    # ── Step 17: Package .odmap ───────────────────────────────────
    step(17, "Package final map.odmap (using Python, no gravity-model overwrite)")
    run(
        [sys.executable, os.path.join(TOOLS_DIR, "package_odmap.py")],
        "package .odmap",
    )

    # ── Step 18: Copy to STDmaps ──────────────────────────────────
    step(18, "Copy updated map.odmap to STDmaps/")
    final_map = os.path.join(DATA_DIR, "map.odmap")
    stdmap_dst = os.path.join(STDMAPS_DIR, "map.odmap")
    shutil.copy2(final_map, stdmap_dst)
    print(f"  Copied {final_map} -> {stdmap_dst}")

    # ── Step 18a: Fill the water that is too small to be sea ──────
    # Natural Earth at 8192x4096 leaves 7,436 water bodies under the engine's
    # own MIN_WATER_BODY, and each one is a hole through a country that the
    # border and selection outlines then trace around. Filled here, on the base
    # map only, because step 18b copies land_sea.png into every scenario --
    # so the scenarios inherit a clean raster instead of each needing its own
    # pass, and the fill happens exactly once.
    step("18a", "Fill sub-threshold water bodies in the base map")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "fill_water_speckle.py"),
         "--map", "map"],
        "water speckle fill",
    )

    # ── Steps 18b-18f: the historical maps, in the only order that works ──
    #
    # These five ran by hand for months and were not in this file at all, so a
    # pipeline run produced scenarios with none of the historical corrections:
    # Germany's entire colonial empire handed to Britain and France six years
    # before it was taken from them, no Panama, no Tibet, no Nepal, no
    # Mongolia, no Occupied Japan. The maps in the repository were right only
    # because somebody remembered to run these afterwards.
    #
    # THE ORDER IS NOT ARBITRARY.
    #
    #   18b generate_scenario  re-cuts the province layer per scenario. Every
    #                          province id downstream comes from here.
    #   18c carve_states       cuts NEW provinces for states smaller than one
    #                          province (Bhutan, Luxembourg). Works from traced
    #                          polygons and parent ISO codes, so it does not
    #                          care what the ids are -- but it creates ids, so
    #                          it must precede anything that reads them.
    #   18d fix_1939_history   restores the nine states The Gathering Storm was
    #                          missing -- Ireland, Panama, Egypt, Iraq, Nepal,
    #                          Tibet, Mongolia, Manchukuo, Yemen. Must precede
    #                          18f, which has 1939 reassignments that move
    #                          ground INTO Mongolia and Yemen and can do nothing
    #                          but warn if those states are not there yet.
    #   18e carve_borders      splits provinces along a border that ran THROUGH
    #                          one. Polygon-driven, and each rule also names the
    #                          country it takes ground FROM.
    #   18f fix_map_history    adds, merges, renames and reassigns whole
    #                          provinces on every map, and skips any state 18d
    #                          already put down.
    #   18g carve_borders      AGAIN. See below.
    #   18h fix_naval_layer    re-berths every fleet and moves any land-locked
    #                          port. Last overall, because everything above
    #                          changes which country owns which harbour.
    #
    # WHY carve_borders RUNS TWICE
    #
    # Its two maps need it on opposite sides of fix_map_history, and this was
    # established by running the whole pipeline both ways and diffing the
    # result against the maps in the repository:
    #
    #   1939 needs it FIRST.  The Kresy rule takes ground from the SOVIET UNION
    #                         and gives a slice of it to Poland. fix_map_history
    #                         has a Kresy reassignment of its own that moves a
    #                         WHOLE province; run first, it hands Poland the lot
    #                         and the carve then finds nothing Soviet left to
    #                         cut. Poland ends up 1,338 px too big.
    #   1918 needs it LAST.   The Galicia rule takes ground from UKRAINE, and
    #                         the Ukrainian State does not exist until
    #                         fix_map_history creates it. Run first, the rule
    #                         matches nothing, prints "0 pixels changed hands",
    #                         and Lviv stays Ukrainian in October 1918.
    #
    # Running it in both positions satisfies both, and costs nothing: a carve
    # that has already happened finds its polygon full of the country it was
    # going to give the ground to, and reports zero. Running it twice is only
    # safe because fix_map_history resolves provinces by position rather than
    # by id, so the ids the second carve creates cannot invalidate it.
    #
    # These are `run`, not `run_ignore_fail`, on purpose. Every other data step
    # degrades gracefully -- a missing overlay costs you resources on a map that
    # still works. These do not: skipping one ships a scenario that is silently
    # wrong about the twentieth century, and looks completely normal.
    step("18b", "Generate historical scenario maps")
    run([sys.executable, os.path.join(TOOLS_DIR, "generate_scenario.py"), "--all"],
        "scenario generation")

    step("18c", "Carve provinces for states too small to have one")
    run([sys.executable, os.path.join(TOOLS_DIR, "carve_states.py")],
        "state carving")

    step("18d", "Restore the nine states 1939 was missing")
    run([sys.executable, os.path.join(TOOLS_DIR, "fix_1939_history.py")],
        "1939 state restoration")

    step("18e", "Split provinces along borders that ran through them")
    run([sys.executable, os.path.join(TOOLS_DIR, "carve_borders.py")],
        "border carving (before the state restoration)")

    step("18f", "Restore the states each scenario's own date had")
    run([sys.executable, os.path.join(TOOLS_DIR, "fix_map_history.py")],
        "historical state restoration")

    step("18g", "Border carving again, for the rules that needed a state first")
    run([sys.executable, os.path.join(TOOLS_DIR, "carve_borders.py")],
        "border carving (after the state restoration)")

    # ── Step 18h: real flags for the states 18d-18f added ─────────
    #
    # fix_map_history, fix_1939_history and carve_states each give the state
    # they add a PROCEDURAL flag -- stripes and symbols from the engine's own
    # vocabulary. That is a reasonable default for a tool that has to invent
    # something, and it is never right: Nepal's double pennant is not a
    # rectangle, Bhutan's dragon and Tibet's snow lions have no symbol, and
    # Iraq's hoist trapezoid comes out as two red stars on a white band.
    #
    # This swaps in the real image, at that scenario's date, from the same
    # licence-audited Wikimedia set as the other 214 flags. It has to run after
    # every tool that can add a country and before anything that reads the
    # finished archive.
    step("18h", "Swap the procedural flags for the real ones")
    run([sys.executable, os.path.join(TOOLS_DIR, "attach_scenario_flags.py")],
        "scenario flag attachment")

    step("18i", "Re-berth fleets and move land-locked ports")
    run([sys.executable, os.path.join(TOOLS_DIR, "fix_naval_layer.py")],
        "naval layer repair")

    # ── Step 19: Generate zero-turn save ──────────────────────────
    step(19, "Generate zero-turn save (.odsv)")
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "generate_zero_turn_odsv.py")],
        "zero-turn save",
    )

    # ── Step 20: Clean up generated artifacts from data/ ──────────
    step(20, "Clean up generated artifacts from data/")
    keep_dirs = {"STDmaps", "saves", "fonts", "Icon", "custom_maps", "licenses", "symbols", "scripts", "flags"}
    keep_files = {"tips.json", "config.json", "map.odmap", "credits.txt", "maps_index.json"}
    for f in os.listdir(DATA_DIR):
        fp = os.path.join(DATA_DIR, f)
        if os.path.isdir(fp):
            if f in keep_dirs:
                print(f"  Keeping directory: {f}/")
            else:
                shutil.rmtree(fp)
                print(f"  Removed directory: {f}/")
        elif os.path.isfile(fp):
            if f not in keep_files:
                os.remove(fp)
                print(f"  Removed file: {f}")
    # Clean map.odmap from data/ root (it's already in STDmaps/)
    map_odmap = os.path.join(DATA_DIR, "map.odmap")
    if os.path.exists(map_odmap):
        os.remove(map_odmap)
        print(f"  Removed file: map.odmap (saved in STDmaps/)")
    # ── Step 21: Licensing paperwork ─────────────────────────────
    # Last, and deliberately loud. The audit is what catches a flag that
    # arrived under new terms, and gen_notices is what keeps the credits and
    # NOTICE.md honest about what the map now actually contains. Neither
    # blocks the pipeline — the map is already built — but a failure here
    # means the build is not shippable until it is dealt with.
    step(21, "Verify licensing paperwork")
    # Covers the scenario flags too — they ship in the same archives.
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "audit_flag_licenses.py"), "--check"],
        "flag licence audit",
    )
    run_ignore_fail(
        [sys.executable, os.path.join(TOOLS_DIR, "gen_notices.py"), "--check"],
        "third-party notices",
    )

    log("Pipeline complete!")
    print("\nNow build and run the game:")
    print("  cmake --build cmake-build-debug/ -- -j8")
    print("  ./cmake-build-debug/OpenDoctrines")


if __name__ == "__main__":
    main()
