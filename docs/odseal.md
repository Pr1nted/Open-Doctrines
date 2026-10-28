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
| Linux aarch64 | `linux-aarch64/` | gcc 12, `-fPIC`, on an Arm runner |
| FreeBSD amd64 | `freebsd-amd64/` | base clang, `-fPIC`, in a VM on a Linux runner |
| FreeBSD arm64 | `freebsd-aarch64/` | base clang, `-fPIC`, in an emulated VM on a Linux runner |
| OpenBSD amd64 | `openbsd-amd64/` | base clang, `-fPIC`, in a VM on a Linux runner |
| OpenBSD arm64 | `openbsd-aarch64/` | base clang, `-fPIC`, in an emulated VM on a Linux runner |
| NetBSD amd64 | `netbsd-amd64/` | base gcc, `-fPIC`, in a VM on a Linux runner |
| NetBSD arm64 | `netbsd-aarch64/` | base gcc, `-fPIC`, in an emulated VM on a Linux runner |
| illumos amd64 | `illumos-amd64/` | gcc from IPS, `-fPIC`, in an OpenIndiana VM on a Linux runner |
| Haiku x86_64 | `haiku-x86_64/` | gcc 13 from HaikuPorts, `-fPIC`, in a Haiku VM on a Linux runner |
| Web (wasm32) | `web-wasm32/` | emscripten `em++` |
| Windows x86_64 | `windows-x86_64/` | MSVC, `/O2 /GR-` |
| Windows x86 (32-bit) | `windows-x86/` | MSVC `amd64_x86`, `/O2 /GR-`, on the x64 runner |
| Windows Arm64 | `windows-arm64/` | MSVC, `/O2 /GR-`, on an Arm runner |
| Linux i686 (32-bit) | `linux-i686/` | `i686-linux-gnu-g++` 11, `-fPIC`, tested under QEMU |
| Linux armv7l (armhf) | `linux-armv7l/` | `arm-linux-gnueabihf-g++` 11, `-fPIC`, tested under QEMU |
| Linux riscv64 | `linux-riscv64/` | `riscv64-linux-gnu-g++` 11, `-fPIC`, tested under QEMU |
| Android arm64-v8a | `android-arm64-v8a/` | NDK clang, API 24 |
| Android armeabi-v7a | `android-armeabi-v7a/` | NDK clang, API 24 |
| Android x86_64 | `android-x86_64/` | NDK clang, API 24 |
| Android x86 | `android-x86/` | NDK clang, API 24 |

If a maintainer working tree has `guard/odseal.cpp`, that is used instead and
the prebuilts are ignored — that is the only tree where the source exists.

When neither is available the **configure** stops with an explanation, rather
than letting the build reach a link error about unresolved `od_*` symbols,
which says nothing about the cause.

The directory name is `${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}`,
lowercased, for everything that is not macOS, Windows, Android or web — so it
follows the OS rather than assuming Linux. **Note that the BSDs say `amd64`
where Linux says `x86_64`, and `aarch64` for 64-bit Arm.** CMake asks FreeBSD
and NetBSD `uname -p` and OpenBSD `arch -s`, and NetBSD alone answers
`x86_64`; CMakeLists.txt evens that out, so every BSD archive is
`<bsd>-amd64` or `<bsd>-aarch64`. A directory named `freebsd-x86_64` or
`netbsd-x86_64` would simply never be found. **illumos is renamed outright:**
CMake calls it `SunOS` and its `uname -p` says `i386` even for a 64-bit
build, so CMakeLists.txt maps a 64-bit SunOS x86 build to `illumos-amd64`.

Two corrections on top of that rule, both because `uname -m` names the
**kernel**, not the compiler:

- **A 32-bit build on a 64-bit kernel** is named by pointer size: `gcc -m32` on
  x86_64 and Raspberry Pi OS's 32-bit userland on its aarch64 kernel would
  otherwise ask for a 64-bit archive. They get `linux-i686` and
  `linux-armv7l` — what a native 32-bit `uname -m` says.
- **Windows** is named by MSVC's own target (`CMAKE_CXX_COMPILER_ARCHITECTURE_ID`):
  `windows-x86_64`, `windows-x86` or `windows-arm64`, under the Visual Studio
  generator and Ninja alike.

## Sealed saves (od_p2 / od_p3)

A fog-of-war world's save is sealed: `od_p2` encrypts an entry with
ChaCha20-Poly1305 under a key derived from a key inside the archive and the
save's own salt, and `od_p3` opens it, refusing anything edited. SaveManager is
the only caller (see "Sealed entries" in `src/SaveManager.cpp`), and
`tests/save_seal_test.cpp` and `tests/odseal_test.cpp` cover both ends.

Callers compile the seal in only under `OD_HAS_P2`, which the source branch
defines. **The prebuilts in `third_party/odseal/` predate it**, so a clone
building against one writes fog-of-war saves unsealed and cannot open a sealed
one (it says so on stderr rather than misreading it). Refresh the prebuilts
with the workflow below, then add `OD_HAS_P2` beside `OD_HAS_T4` on the
imported target in `CMakeLists.txt`.

What the seal is worth: unzip, a text editor and `tools/read_odsv.py` no longer
read what fog hides. A program calling `od_p3`, a debugger or a rebuilt client
still can. It raises the cost of peeking; it is not secrecy.

## Keeping them in sync

Building eight archives on eight machines by hand is the chore that silently
stops happening, and it fails as a red CI on a platform nobody built for. So
`.github/workflows/odseal-prebuilts.yml` does it: run it from the Actions tab
and it rebuilds every archive from one source with one set of flags, tests each
one, and pushes whatever changed to a branch. Run it whenever the sealed source
changes, then open the pull request it links to in the run summary.

It pushes a branch rather than committing, because `main` requires reviews and
status checks and a direct push from the workflow is declined — which spent five
successful builds and reported a red run that said nothing about why. It stops
short of opening the request itself because the setting that would allow that
("Allow GitHub Actions to create and approve pull requests") also lets a
workflow approve one, and on a branch that requires review that is the gate
approving itself.

Two of the archives are not reproducible: the macOS and web `ar` headers carry
a build timestamp, so those two differ on every rebuild even when the source is
identical, while linux and android come out byte-for-byte the same. The pull
request lists the exported symbols of each archive for that reason — it is the
part of a stripped binary a reviewer can actually check.

GitHub hosts no BSD runner, so the six BSD archives build inside a real BSD
VM (`vmactions/*-vm`) on a Linux runner — by the target system's own `cc` and
`ar`, not cross-compiled; the arm64 ones in an emulated VM. They are tested
there too, in the same step (`tools/odseal_bsd.sh`): the archive never leaves
the VM in between, so a test run anywhere else would not be testing what the
job produces.

**A new platform needs its archive before its game build can pass.**
`bsd-game.yml` checks for it first and fails in a second, naming this
workflow, rather than at configure half an hour into an emulated build. So
after adding a platform, run this workflow with `ref` set to the branch that
adds it, and merge the branch it pushes into that one.

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
