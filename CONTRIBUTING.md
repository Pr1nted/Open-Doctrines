# Contributing

## Rights

**Read [CLA.md](CLA.md) before you send a pull request. Sending one means you
accept it.** It is the Contributor License Agreement, and clause 4 of
[LICENSE](LICENSE) makes it part of the licence.

The summary, which CLA.md overrides wherever the two differ:

- **You assign the copyright in your contribution to the project.** It becomes
  the Licensor's to use, change, relicense, sell or sublicense, without asking
  you, paying you or crediting you. You are not a co-owner of OpenDoctrines or
  of anything in it.
- **You keep the right to use your own work anywhere else, forever** —
  clause 6. Commercially, in your own projects, in someone else's. What you
  give up is exclusivity, not access to your own code.
- **Mods, maps, saves and videos are not contributions.** Clause 3 of LICENSE
  still says they are yours, and the CLA does not reach them. Nothing here
  applies to a separate project you build on the Gearbox SDK either.

The reason is in CLA.md under "Why this exists". Short version: the licence can
change in future, that power reaches only rights the project holds, and one
accepted patch under a non-exclusive grant would leave a piece that cannot be
relicensed and cannot be removed without rewriting it.

Do both of these on your first pull request:

**1. Add yourself to [CLA-SIGNATURES.md](CLA-SIGNATURES.md)**, in the same pull
request as your change. Git records who added the line and when; that is the
signature. Any name or handle will do — the project does not collect legal
names.

**2. Sign off each commit:**

```bash
git commit -s -m "your message"
```

`-s` appends one line:

```
Signed-off-by: Your Name <you@example.com>
```

That line means the [Developer Certificate of Origin](https://developercertificate.org/)
and clause 7 of the CLA: you wrote the change, or you have the right to submit
it. If your employer owns your work, get their sign-off before you send it —
that is the part people forget, and it is the part that is expensive to unwind
later.

## Data and assets

**Anything with facts or artwork in it goes through `tools/provenance.json`.**
`NOTICE.md`, `data/credits.txt` and the credits screen are generated from that
file, and `tests/run_all.sh` fails if they have drifted, so adding a source
without recording it breaks the build. That is deliberate.

Before adding a dataset, answer one question: **can you point at the sentence
that grants redistribution?** Not a citation request. Not "it's academic". Not
"everyone uses it". A sentence that says you may redistribute it, or a public
domain declaration.

Two datasets failed that test and were removed in July 2026 — GREG and ACOR,
both from ETH Zurich, both shipping no licence at all while their derived data
sat inside `map.odmap`. The reasoning is in [NOTICE.md](NOTICE.md), under "Data
deliberately not used". If a dataset would be genuinely useful and its terms are
unclear, say so in the pull request rather than adding it and hoping.

Flags are audited automatically:

```bash
python3 tools/audit_flag_licenses.py
```

It asks Wikimedia Commons what each file is under and refuses anything not on
the accepted list. Add a flag, rerun it, commit the result.

## Before you open a pull request

```bash
cmake --build cmake-build-debug/ -- -j8
tests/run_all.sh
```

The suite covers the mod ABI, the archive reader, the runtime sandbox, the
network protocol, the account and lobby rules, the updater, and the generated
paperwork. It should be green before and after your change.

## Style

Match the file you are editing — its comment density, its naming, its idiom.
The codebase explains *why* rather than *what*, particularly where a decision
looks odd; if you find yourself deleting a comment that explains a trade-off,
read it first, because it is probably load-bearing.

## Conduct

[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
