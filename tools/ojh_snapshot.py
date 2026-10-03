#!/usr/bin/env python3
"""Copy the newest Objective Judge Horizon run into this repo as one JSON.

    python3 tools/ojh_snapshot.py [--ojh ~/CLionProjects/objective-judge-horizon]

WHY A SNAPSHOT AND NOT A FETCH. The benchmark lives in a separate, private
repository; the website cannot read it at run time and a visitor's browser
certainly cannot. So the numbers are committed here, reviewable in a diff, and
the page reads a file it is served alongside. The cost is that this has to be
re-run when a new benchmark lands, which is the same bargain the release list
makes with CHANGELOG.md.

WHAT IT KEEPS. Turns per minute for every game, because that is the only metric
the whole field reports; frames per second and network bytes where they exist.
Missing values are written as null and SAID SO on the page -- OpenDoctrines and
Freeciv have no FPS figure in the matched run, and a chart that quietly dropped
them would read as though they had scored zero.
"""
from __future__ import annotations

import argparse, json, pathlib, re, sys

HERE = pathlib.Path(__file__).resolve().parent.parent
# The driver name as OJH writes it -> what to call it, and the chart marker.
# Markers rather than logos: these are six separate projects whose marks are
# theirs, and a legend is how a chart distinguishes series anyway.
GAMES = {
    "opendoctrines": ("Open Doctrines", "circle"),
    "unciv":         ("Unciv",          "square"),
    "freecol":       ("FreeCol",        "triangle"),
    "freeciv":       ("Freeciv",        "diamond"),
    "gd5":           ("Greater Diplomacy 5", "cross"),
    "freeorion":     ("FreeOrion",      "star"),
}


def newest_run(root: pathlib.Path) -> pathlib.Path:
    runs = [d for d in (root / "results").iterdir()
            if d.is_dir() and re.match(r"matched-\d{8}$", d.name)]
    if not runs:
        raise SystemExit("no matched-YYYYMMDD run under results/")
    return max(runs, key=lambda d: d.name)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ojh", default="~/CLionProjects/objective-judge-horizon")
    ap.add_argument("--out", default="docs/ojh/latest.json")
    a = ap.parse_args()
    root = pathlib.Path(a.ojh).expanduser()
    if not (root / "results").is_dir():
        print(f"no OJH checkout at {root}", file=sys.stderr)
        return 1

    run = newest_run(root)
    # The scenario folders inside a run; od-1939 is the one played on Open
    # Doctrines' own map, which is the fair comparison for this site to show.
    scen = run / "od-1939"
    if not scen.is_dir():
        scen = next(d for d in run.iterdir() if d.is_dir() and d.name != "maps")

    out = {"run": run.name, "scenario": scen.name, "games": [], "machine": None}
    for key, (name, marker) in GAMES.items():
        row = {"key": key, "name": name, "marker": marker,
               "tpm": None, "fps": None, "fps_low": None, "nipm": None, "turns": None}
        for metric, fields in (("tpm", ("turns_per_minute", "tpm")),
                               ("fps", ("map_average_fps",)),
                               ("net", ("nipm_bytes_per_minute",))):
            f = scen / f"{key}-{metric}.json"
            if not f.is_file():
                continue
            d = json.loads(f.read_text()).get("result", {})
            if out["machine"] is None:
                out["machine"] = json.loads(f.read_text()).get("machine", {}).get("model")
            if metric == "tpm":
                row["turns"] = d.get("turns")
                for k in fields:
                    if d.get(k) is not None:
                        row["tpm"] = d[k]; break
            elif metric == "fps":
                row["fps"] = d.get("map_average_fps")
                row["fps_low"] = d.get("one_percent_low_fps")
            else:
                row["nipm"] = d.get("nipm_bytes_per_minute")
        out["games"].append(row)

    out["games"].sort(key=lambda g: (g["tpm"] is None, -(g["tpm"] or 0)))
    dest = HERE / a.out
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(out, indent=1) + "\n")
    have = sum(1 for g in out["games"] if g["tpm"] is not None)
    print(f"{a.out}: {run.name}/{scen.name}, {have}/{len(out['games'])} games with a TPM figure")
    for g in out["games"]:
        print(f"  {g['name']:20s} tpm={g['tpm']}  fps={g['fps']}  nipm={g['nipm']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
