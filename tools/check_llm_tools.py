#!/usr/bin/env python3
"""Every tool an advisor is OFFERED must be a tool the game can ANSWER.

A tool declared in src/llm/Advisor.cpp's catalogue but never answered in
Game_Llm.cpp is offered to the model, called by it, and then answered with
nothing -- the model spends a round of its budget and learns it cannot ask.
That is the same shape as this project's recurring defect, where a validity
mask offers an action the executor refuses; it is invisible at compile time and
shows up as a counter reading zero.

Recording tools (the ones with a `records` flag) are exempt: they are written
down rather than answered, and they take the steering path instead.

Run by tests/run_all.sh. Exits non-zero and names the offenders.
"""
import re
import sys
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
adv = (root / "src/llm/Advisor.cpp").read_text(encoding="utf-8", errors="replace")
llm = (root / "src/Game_Llm.cpp").read_text(encoding="utf-8", errors="replace")

start = adv.find("kTools[]")
end = adv.find("\n};", start)
if start < 0 or end < 0:
    print("check_llm_tools: could not find the kTools catalogue", file=sys.stderr)
    sys.exit(2)
catalogue = adv[start:end]

# One entry per {"name", ... } block. A trailing `true` marks a recording tool.
entries = re.findall(r'\{"([a-z_]+)",(.*?)\}(?=,\s*(?:\{|//|$)|\s*$)', catalogue, re.S)
if len(entries) < 5:
    print(f"check_llm_tools: parsed only {len(entries)} tools, the format moved", file=sys.stderr)
    sys.exit(2)

answered = set(re.findall(r'tool == "([a-z_]+)"', llm))
missing = [name for name, body in entries
           if not re.search(r'\btrue\s*$', body.strip()) and name not in answered]

if missing:
    print("These tools are offered to the model and nothing answers them:", file=sys.stderr)
    for m in missing:
        print(f"    {m}   (add `if (tool == \"{m}\")` to Game_Llm.cpp, or mark it recording)",
              file=sys.stderr)
    sys.exit(1)

print(f"llm tools: {len(entries)} declared, every answering one is answered")
