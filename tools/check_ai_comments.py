#!/usr/bin/env python3
"""A numerate comment in src/ai must not describe a version this tree is not.

Journal 440 found `AI_CAMPAIGN_SHARE` documenting "0.20 since ParrotZero 8.4.0"
above a constant reading 0.35f. Journal 447 found `AI_MAX_CONCURRENT_WARS`
documenting "2 since ParrotZero 8.6.0" -- with two models' numbers and "not one
seat lost on the whole bench" -- above a constant reading 1. Both comments
arrived with 801fd20, "Complete the revert: restore pre-branch AI code for
1.2.0a", which restored the VALUES and kept the branch's COMMENTS.

Both were then re-measured on this build and BOTH FAILED: the campaign share
was flat, and the war cap cost the rung seats 101.9 points and cleared its
floor. So the revert was right on the merits twice, and the comments were
lying twice -- and each one cost a two-hour bench to find out.

This check is the cheap half of that. It flags two things:

  1. A comment citing a ParrotZero version NEWER than AIVersion.h. Such a
     comment describes measurements taken in a game this tree is not, which is
     evidence about nothing (memory a-revert-keeps-the-comment-and-restores-the-value).
  2. A "<value> since ParrotZero <v>" claim whose value disagrees with the
     constant defined under it.

It cannot check prose, and it is not meant to: the goal is to stop the next
reader treating a stranded number as a finding. Run by tests/run_all.sh.
"""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
ver = (root / "src/ai/AIVersion.h").read_text(encoding="utf-8", errors="replace")


def field(name):
    m = re.search(rf"{name}\s*=\s*(\d+)", ver)
    if not m:
        sys.exit(f"check_ai_comments: could not read {name} from AIVersion.h")
    return int(m.group(1))


cur = (field("ARCH"), field("RULES"))
problems = []

# A stranded comment is ACCEPTABLE once its correction sits beside it -- which is
# what this check's own error message asks for, so it has to recognise the thing
# it demands. Without this the check fires on its own fix: the first run flagged
# AISystem.h:1193, a line that exists to say the comment above it is wrong, and
# quoting the wrong claim is not making it.
ACK = ("RE-MEASURED", "DESCRIBE A VERSION THIS TREE IS NOT",
       "describes a version this tree is not", "CLAIM WITHDRAWN",
       "STAYS OFF", "was WRONG")


def acknowledged(lines, idx, window=30):
    lo, hi = max(0, idx - window), min(len(lines), idx + window)
    return any(any(k in lines[j] for k in ACK) for j in range(lo, hi))


for path in sorted((root / "src/ai").glob("*.h")) + sorted((root / "src/ai").glob("*.cpp")):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(lines, 1):
        if "//" not in line:
            continue
        comment = line.split("//", 1)[1]

        # 1. a version this tree is not
        for m in re.finditer(r"ParrotZero\s+(\d+)\.(\d+)", comment):
            cited = (int(m.group(1)), int(m.group(2)))
            if cited > cur and not acknowledged(lines, i - 1):
                problems.append(
                    (path, i, f"cites ParrotZero {cited[0]}.{cited[1]} but AIVersion.h "
                               f"is {cur[0]}.{cur[1]} -- describes a version this tree is not",
                     line.strip()[:100]))

        # 2. "<value> since ParrotZero <v>" against the constant below it
        m = re.match(r"\s*([0-9]+(?:\.[0-9]+)?)\s+since ParrotZero", comment)
        if m:
            claimed = m.group(1)
            for j in range(i, min(i + 40, len(lines))):
                c = re.search(r"constexpr\s+\w+\s+(\w+)\s*=\s*([0-9.]+)f?\s*;", lines[j])
                if c:
                    actual = c.group(2).rstrip(".")
                    if float(claimed) != float(actual) and not acknowledged(lines, i - 1):
                        problems.append(
                            (path, i, f"claims the value is {claimed} but {c.group(1)} "
                                      f"is {actual}", line.strip()[:100]))
                    break

if problems:
    print("Numerate comments in src/ai that do not describe this build:\n", file=sys.stderr)
    for path, line, why, text in problems:
        print(f"  {path.relative_to(root)}:{line}", file=sys.stderr)
        print(f"      {why}", file=sys.stderr)
        print(f"      {text}", file=sys.stderr)
    print("\nEither correct the comment or record the re-measurement beside it.",
          file=sys.stderr)
    print("A number measured on another build is a hypothesis, not a finding.",
          file=sys.stderr)
    sys.exit(1)

print("ai comments: no numerate comment cites a version newer than this build")
