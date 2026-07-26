#!/usr/bin/env python3
"""The version scheme, in one place, so the release util and CI agree.

    MAJOR.MINOR.PATCH<state>[counter]

    1.0.2a      alpha
    1.0.2b      beta
    1.0.2r      release
    1.0.2s3     snapshot 3 of 1.0.2

The state letter is part of the version, not decoration: a save stamped 1.0.2a
and one stamped 1.0.2r came from different builds and should not be confused.

Bump rules, matching how releases are actually cut:

    patch (default)   1.0.2a -> 1.0.3a      the everyday one
    --new-update      1.0.2a -> 1.1.2a      bumps MINOR, leaves PATCH alone
    snapshot          1.0.2a -> 1.0.2s1     first snapshot of this version
                      1.0.2s1 -> 1.0.2s2    subsequent ones just count up

Snapshots deliberately do NOT advance PATCH: a snapshot is "somewhere after
1.0.2", not a release of its own, so it hangs off the version it came from.

Run the self-check with:  python3 tools/odver.py --selftest
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_FILE = os.path.join(ROOT, "VERSION")

STATES = {"alpha": "a", "a": "a",
          "beta": "b",  "b": "b",
          "release": "r", "r": "r",
          "snapshot": "s", "s": "s"}

STATE_NAMES = {"a": "alpha", "b": "beta", "r": "release", "s": "snapshot"}

_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)([abrs])(\d*)$")


class Version:
    def __init__(self, major, minor, patch, state, counter=None):
        self.major, self.minor, self.patch = major, minor, patch
        self.state = state
        # Only meaningful for snapshots; None everywhere else.
        self.counter = counter

    @classmethod
    def parse(cls, text):
        m = _RE.match(text.strip())
        if not m:
            raise ValueError(
                f"malformed version {text.strip()!r}; expected MAJOR.MINOR.PATCH"
                f" plus one of a/b/r/s, e.g. 1.0.2a or 1.0.2s3")
        major, minor, patch, state, counter = m.groups()
        if state == "s" and counter == "":
            # "1.0.2s" with no number means the zeroth, so the next is 1.
            counter = 0
        return cls(int(major), int(minor), int(patch), state,
                   int(counter) if counter != "" else None)

    def __str__(self):
        base = f"{self.major}.{self.minor}.{self.patch}{self.state}"
        if self.state == "s":
            return f"{base}{self.counter if self.counter is not None else 0}"
        return base

    def numeric(self):
        """What CMake's project(VERSION) accepts: digits only."""
        return f"{self.major}.{self.minor}.{self.patch}"

    def sort_key(self):
        """Ordering for 'is this newer?'.

        A snapshot sorts BEFORE the plain version it hangs off, because
        1.0.2s3 is work in progress toward 1.0.2, not past it. Among the
        non-snapshot states alpha < beta < release, so a release supersedes the
        beta of the same number."""
        rank = {"s": 0, "a": 1, "b": 2, "r": 3}[self.state]
        return (self.major, self.minor, self.patch, rank,
                self.counter if self.counter is not None else 0)

    def bump(self, new_update=False, state=None):
        """Returns the next version. Never mutates in place -- the caller
        writes it out only after everything else has succeeded."""
        target = STATES[state] if state else self.state

        if target == "s":
            # Staying in (or entering) snapshot: count up from wherever we are.
            if self.state == "s":
                return Version(self.major, self.minor, self.patch, "s",
                               (self.counter or 0) + 1)
            # Entering snapshot from a/b/r: the first snapshot of THIS version.
            return Version(self.major, self.minor, self.patch, "s", 1)

        # Leaving snapshot for a real state keeps the number and drops the
        # counter: 1.0.2s4 --switch-state release becomes 1.0.2r.
        if self.state == "s" and state:
            return Version(self.major, self.minor, self.patch, target)

        if new_update:
            return Version(self.major, self.minor + 1, self.patch, target)
        return Version(self.major, self.minor, self.patch + 1, target)


def read(path=VERSION_FILE):
    with open(path) as f:
        return Version.parse(f.read())


def write(v, path=VERSION_FILE):
    with open(path, "w") as f:
        f.write(str(v) + "\n")


# ------------------------------------------------------------- self-test ----

def _selftest():
    cases = [
        # (start, new_update, state, expected)
        ("1.0.2a", False, None,       "1.0.3a"),   # the everyday bump
        ("1.0.2a", True,  None,       "1.1.2a"),   # --new-update: MINOR, not PATCH
        ("1.0.2a", False, "beta",     "1.0.3b"),
        ("1.0.2a", False, "release",  "1.0.3r"),
        ("1.0.2a", False, "snapshot", "1.0.2s1"),  # snapshot does NOT advance patch
        ("1.0.2s1", False, "snapshot","1.0.2s2"),
        ("1.0.2s", False, "snapshot", "1.0.2s1"),  # bare 's' counts as zero
        ("1.0.2s4", False, "release", "1.0.2r"),   # leaving snapshot keeps the number
        ("1.1.9a", True,  None,       "1.2.9a"),
    ]
    bad = 0
    for start, nu, st, expect in cases:
        got = str(Version.parse(start).bump(new_update=nu, state=st))
        ok = got == expect
        bad += not ok
        flag = "ok  " if ok else "FAIL"
        extra = "" if ok else f"   (expected {expect})"
        print(f"  {flag} {start:8s} new_update={str(nu):5s} state={str(st):9s} -> {got}{extra}")

    # Ordering, which the in-game update check depends on being right.
    order = ["1.0.2s1", "1.0.2s2", "1.0.2a", "1.0.2b", "1.0.2r", "1.0.3a", "1.1.0a", "2.0.0a"]
    keys = [Version.parse(v).sort_key() for v in order]
    if keys != sorted(keys):
        print("  FAIL ordering is wrong:", order)
        bad += 1
    else:
        print("  ok   ordering: " + " < ".join(order))

    for junk in ["1.0.2", "1.0.2x", "1.0", "", "v1.0.2a"]:
        try:
            Version.parse(junk)
            print(f"  FAIL {junk!r} should have been rejected")
            bad += 1
        except ValueError:
            pass
    print(f"  ok   malformed versions are rejected")

    print(f"\n{'all version rules hold' if not bad else str(bad) + ' FAILED'}")
    return 1 if bad else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    print(read())
