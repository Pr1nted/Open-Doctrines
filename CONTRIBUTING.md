# Contributing

## Rights

Clause 4 of [LICENSE](LICENSE) covers this and is short enough to read. The
summary: **you keep the copyright in what you write.** You grant the project a
licence to ship it as part of OpenDoctrines, including under future terms.

There is no CLA to sign and no copyright to assign. Instead, sign off each
commit with the [Developer Certificate of Origin](https://developercertificate.org/):

```bash
git commit -s -m "your message"
```

`-s` appends one line:

```
Signed-off-by: Your Name <you@example.com>
```

That line means you wrote the change, or you have the right to submit it. If
your employer owns your work, get their sign-off before you send it — that is
the part people forget, and it is the part that is expensive to unwind later.

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
