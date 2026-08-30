# odseal

`odseal` is a small sealed dependency the language layer links against. It
ships as a built archive rather than as source.

## Why a clone can still build

A closed component and a buildable clone are only compatible if the binary is
provided for every platform the project targets. So `third_party/odseal/` holds
one prebuilt per platform, and CMake picks the matching one:

| platform | directory | built with |
|---|---|---|
| macOS (Intel + Apple Silicon) | `macos-universal/` | clang, `-arch x86_64 -arch arm64`, `-mmacosx-version-min=11.0` |
| Linux x86_64 | `linux-x86_64/` | gcc 12, `-fPIC` |
| Web (wasm32) | `web-wasm32/` | emscripten `em++` |
| Windows x86_64 | `windows-x86_64/` | MSVC, `/O2 /GR-` |
| Android arm64-v8a | `android-arm64-v8a/` | NDK clang, API 24 |

If a maintainer working tree has `guard/odseal.cpp`, that is used instead and
the prebuilts are ignored — that is the only tree where the source exists.

When neither is available the **configure** stops with an explanation, rather
than letting the build reach a link error about unresolved `od_*` symbols,
which says nothing about the cause.

## Keeping them in sync

Building five archives on five machines by hand is the chore that silently
stops happening, and it fails as a red CI on a platform nobody built for. So
`.github/workflows/odseal-prebuilts.yml` does it: run it from the Actions tab
and it rebuilds every archive from one source with one set of flags, tests each
one, and commits whatever changed. Run it whenever the sealed source changes.

It needs two repository secrets, because the source is not in this repository:

| secret | what it is |
|---|---|
| `ODSEAL_SOURCE_REPO` | the private repo holding `odseal.cpp` and `odseal.h`, as `owner/name` |
| `ODSEAL_SOURCE_TOKEN` | a token with read access to it |

Without them the job stops on its first step and says which one is missing,
rather than building nothing and committing an empty archive. The source is
never printed and never uploaded; the checkout is deleted before any artifact
is packed, so only the stripped archives leave the runner.

## Building a prebuilt

By hand, if you need to. This is what the workflow runs.

All platforms use the same flags: optimise, hide symbols, no RTTI, then strip.

```bash
# macOS universal
for A in arm64 x86_64; do
  c++ -std=c++17 -O3 -fvisibility=hidden -fno-rtti -fomit-frame-pointer \
      -arch $A -mmacosx-version-min=11.0 -c guard/odseal.cpp -o odseal-$A.o
  strip -x odseal-$A.o
done
lipo -create odseal-arm64.o odseal-x86_64.o -output odseal.o
ar rcs third_party/odseal/macos-universal/libodseal.a odseal.o

# Linux x86_64 (reproducible in a container)
docker run --rm -v "$PWD":/w -w /w gcc:12 bash -c '
  g++ -std=c++17 -O3 -fvisibility=hidden -fno-rtti -fomit-frame-pointer -fPIC \
      -c guard/odseal.cpp -o odseal.o && strip --strip-unneeded odseal.o &&
  ar rcs third_party/odseal/linux-x86_64/libodseal.a odseal.o'

# Web
em++ -std=c++17 -O3 -fvisibility=hidden -fno-rtti -c guard/odseal.cpp -o odseal-wasm.o
emar rcs third_party/odseal/web-wasm32/libodseal.a odseal-wasm.o
```

## Checking a prebuilt

A prebuilt that links but misbehaves is worse than one that is missing, so
build `tests/odseal_test.cpp` against the archive for the platform and run it.
It asserts both halves of the contract: the two coverages that must be refused
are refused, and the ones that must pass — including the languages that share
a script with them — pass.

Only four symbols should be exported:

```bash
nm --defined-only -g odseal.o | grep ' T '   # od_fz od_k9 od_s7 od_v3
```
