# Gearbox SDK — C++

**Status: built and loaded successfully with clang 21 (wasm32), verified against
the host with `odmod-check`.**

C++ uses the same header as C, [`sdk/gearbox.h`](../gearbox.h). It is wrapped in
`extern "C"`, so the imports and exports are unaffected by name mangling — the
wire names come from `__attribute__((import_name))` / `export_name`, not from
the symbol.

## Build

```bash
./build.sh
```

Or by hand:

```bash
clang++ --target=wasm32 -nostdlib -fno-exceptions -fno-rtti -std=c++17 -O2 \
        -I ../ -Wl,--no-entry -Wl,--allow-undefined -o mod.wasm mod.cpp
../../tools/pack_odmod.sh . hello-panel-cpp.odmod
```

Apple's bundled `clang++` cannot target wasm32. The one inside Emscripten can,
as can any upstream LLVM. `build.sh` finds one for you.

The example builds to **2.8 KB**.

## What you give up

Freestanding wasm32 has no libc and no libc++. Concretely:

- **No `std::`** — no `string`, no `vector`, no `<algorithm>`. Nothing that
  allocates, because there is no allocator unless you write one.
- **No exceptions.** `-fno-exceptions` is not a preference; there is no
  unwinder. A `throw` will not link.
- **No RTTI.** `-fno-rtti` for the same reason: no typeinfo emission.
- **No static destructors you can rely on.** There is no `atexit` and the module
  is torn down wholesale. Release resources in `mod_unload`.

Global constructors *do* run — LLVM emits them into the module's start path —
but do not rely on ordering across translation units.

## What C++ is actually good for here

Types, at zero cost. The single easiest mistake against this ABI is passing a
pointer with the wrong length, and that is a mistake a type can prevent:

```cpp
struct Str {
    const char* p;
    uint32_t    n;
    template <uint32_t N>
    constexpr Str(const char (&lit)[N]) : p(lit), n(N - 1) {}
    constexpr Str(const char* q, uint32_t m) : p(q), n(m) {}
};

inline void text(gearbox_panel pl, int32_t x, int32_t y, uint32_t c, Str s) {
    gearbox_draw_text(pl, x, y, c, s.p, s.n);
}

text(panel, 8, 8, 0xFFFFFFFFu, "Turn");   // length computed at compile time
```

`mod.cpp` also shows a fixed-capacity `Line<N>` builder with `operator<<`, since
you cannot use `std::ostringstream` and formatting integers is the first thing
every mod needs.

## Gotchas

- **`sizeof` a string literal includes the NUL.** `sizeof("hi") - 1` is the
  length. The `Str` constructor above does this for you; hand-written calls
  usually get it wrong once.
- **`gearbox_country_name` uses two-call sizing** and returns the *full* length,
  which may exceed your buffer. A return greater than `cap` means truncation,
  not failure.
- **`mod_load` returning non-zero refuses the load** and the value is shown to
  the user. Return 0 on success.
- Check the module's imports before shipping:
  `python3 ../../tools/wasm_imports.py mod.wasm`
