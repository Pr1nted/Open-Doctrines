# How CI is shaped, and why

CI has a fixed number of runner slots, and its slowest runners (Windows,
Intel macOS, emulated CPUs, a VM per operating system) take an hour or more
each. The old shape ran everything on every push: about 35 jobs per pull
request, and the same again on `main` after it merged. With a stack of pull
requests that was a queue, not a pipeline: two days of near-continuous CI went
mostly to waiting for a machine, and then to re-proving on a ninth CPU what
eight had already proved about a change that touched nothing CPU-specific.

The redesign does the same checks with fewer machine-hours. There are four
parts.

## 1. Tiers, decided once (`tools/ci_plan.py`)

Every workflow's first job asks `tools/ci_plan.py` which tiers this change
needs, and every expensive job waits on that answer.

| Tier | What runs | When |
|---|---|---|
| core | the full suite on linux-x64, windows-x64, macos-arm64; the servers; web; Android arm64; tsan; asan+ubsan; the translation layer | always |
| wide | windows-x86, windows-arm64, macos-x64; linux-x86/armv7/riscv64 (emulated); the other three Android ABIs and the APK | the change touches code that reads bytes, sizes or sockets, or the platform layer |
| bsd | FreeBSD, OpenBSD, NetBSD, each in a VM | the change touches the BSD build |
| otheros | OpenIndiana, Haiku, SerenityOS, the consoles, DOS, RISC OS | the change touches the platform layer or those builds |
| full | all of the above | nightly on `main`, a release, a manual run, or a PR labelled `ci:full` |

A change to the build itself (`CMakeLists.txt`, `cmake/`, `third_party/`, the
workflows, this script, `qualify.sh`) turns every tier on, because it can break
every platform at once. The path lists live in the script, so "what does a
change to X run" has one answer, not four.

`push` to `main` runs the core tier only. The **nightly run** (03:17 UTC) runs
everything. It is where a regression on a platform nothing in the diff pointed
at turns up, and it keeps the compiler caches warm for the next day's PRs.

**What this costs.** A bug that only shows on a CPU or OS a PR's paths didn't
point at is found by the nightly run, not by the PR. Label the PR `ci:full`
when that matters.

## 2. Fail cheap, first

The plan job also runs the checks that need no build: the tool index and the
release-name agreement. A tool missing from `tools/help.py` used to fail every
full-suite job about an hour in. It now fails one Linux runner in a minute,
before anything expensive starts.

## 3. Compile once, not every time (`.github/actions/compiler-cache`)

On windows-x64, 31 minutes of a 55-minute run were compiling (see the note on
the `qualify` job's timeout), and no job cached anything. Every build job now
starts with:

```yaml
- uses: ./.github/actions/compiler-cache
  with:
    key: qualify-${{ matrix.name }}
```

It runs ccache (sccache for MSVC) and sets `CMAKE_<LANG>_COMPILER_LAUNCHER`,
which CMake reads from the environment. Nothing else had to change: not
`qualify.sh`, the cross builds, nor the Android build. A pull request reads
`main`'s cache and writes its own, so an unchanged file costs a copy, not a
compile.

The VM jobs (BSD, other OS) compile inside a guest and are not cached this way
yet. SerenityOS already caches its toolchain and system build. Those caches
are filled from `main`, so the first nightly run after the SerenityOS port
merges is what makes later pull requests fast.

## 4. Fewer slots held for less

- The arm64 BSD VMs (emulated, up to six hours each) run on releases and
  manual runs only, not nightly.
- Builds use all four cores of a hosted runner (`-j 4`, was `-j 2` for
  Android, web and the server).
- Superseded pull-request runs are still cancelled (the existing `concurrency`
  groups).

## 5. The test programs run side by side (`tests/run_all.sh`)

About 74 of the suite's ~93 runs are self-contained test binaries, each
writing only to its own temp directory, and they ran one at a time on a
four-core runner. Consecutive ones now go into a pool sized to the core count
(`OD_TEST_JOBS`). Before any other step the pool is drained, with every
result printed in file order and every failure named as before. The scripts,
including every network and multiplayer test with a clock in it, still run
with nothing else on the machine. `OD_TEST_JOBS=1` restores the serial run.

## 6. Small jobs folded into the runner that is already set up

The server and translation-layer jobs each took a runner on the same three
platforms as `qualify`, only to check out, set up a toolchain and build a
little more. Both are now steps in `qualify`:

- **server**: `qualify.sh` already built and smoke-tested the dedicated
  server. The one check only the server job made (that the binary links
  nothing graphical) now runs on the binary `qualify` built.
- **translation layer**: a second build tree with its own configuration on
  the same runner, compiler cache included.

Six fewer jobs per run, each of which had to wait for a slot. The cross-load
check stays separate, because it has to read every platform's save on every
platform, which only works once all of them have written one.

## Not done yet

- **Splitting the suite across runners.** It was considered and turned down.
  Slots are the scarce thing, so three runners for one suite makes the queue
  longer even though the job is shorter. Parallelism inside the runner
  (section 5) gets the speed without the slots.
