# Java / Kotlin SDK

Write a mod in any JVM language. TeaVM compiles the bytecode ahead of time to
wasm, so what ships is an ordinary `.odmod` with no JVM inside it — a Java mod
is closer in cost to the Rust one than to the Lua one.

```bash
sdk/java/examples/hello-panel/build.sh          # Java
sdk/java/examples/hello-panel-kotlin/build.sh   # Kotlin
```

Both need a JDK 17+ and Maven. `tools/sdk_toolchains.sh install` will fetch
Maven into `.toolchains/`; Kotlin needs nothing further, because kotlinc
arrives as a Maven plugin.

Built and measured with:

| | |
|---|---|
| TeaVM | 0.12.0 |
| Kotlin | 2.0.21 |
| JDK | 21 (Temurin) |
| `mod.wasm` | 156 KB Java, 166 KB Kotlin |
| `.odmod` | 49 KB Java, 52 KB Kotlin |
| `mod_load` cost | between 10k and 20k instructions, measured by bisecting `fuelPerTurn` until the load stopped trapping |

The examples declare `fuelPerTurn: 200000`, the same as the C example. Nothing
here is interpreted at runtime, so a JVM-language mod costs roughly what a
native one costs.

## WEBASSEMBLY_WASI, not WEBASSEMBLY_GC

Worth stating plainly, because the obvious choice is the wrong one and
`docs/gearbox-interpreter-sdks.md` originally recommended it.

TeaVM's **WasmGC** backend cannot be used here. Its output:

- has a **tag section** — it uses the exception-handling proposal for Java
  exceptions, and WAMR 2.4.5 implements those opcodes only in the *classic*
  interpreter. `wasm_interp_fast.c` answers them with "unsupported opcode".
- imports **22 functions the host cannot provide**: `teavmJso.*` and
  `wasm:js-string.*`. That backend targets a JavaScript host and expects the JS
  engine to supply string builtins and an interop layer.

The **WEBASSEMBLY_WASI** backend has neither problem. It puts Java objects in
linear memory, implements exceptions without the proposal, and imports exactly
four WASI functions — `fd_write`, `clock_time_get`, `args_sizes_get`,
`args_get` — all of which the host's `WasiStub` already provides. It needs no
change to WAMR's build flags: not exception handling, not even GC.

## What does not work

TeaVM's WASI target does not implement all of the class library. Three things
bite in practice, and all three compile cleanly and fail only at runtime:

**`String.getBytes()` traps.** There is no working charset support. The binding
carries its own UTF-8 encoder and decoder — use `Gearbox.log`,
`Gearbox.drawText` and friends, which take a `String` and do the conversion, and
do not reach for `getBytes()` yourself.

**String concatenation with `+` fails.** javac 9+ and kotlinc compile `"a" + b`
to an invokedynamic call into `StringConcatFactory`, which TeaVM does not
implement. Use an explicit `StringBuilder`:

```java
new StringBuilder().append("Turn ").append(Gearbox.turnNumber()).toString()
```

The Kotlin example passes `-Xstring-concat=inline` so kotlinc emits
`StringBuilder` for any concatenation it generates internally — but Kotlin
string templates (`"Turn $n"`) are still best avoided for the same reason.

**`@Export` must be on a static method.** In Kotlin that means `@JvmStatic`
inside an `object`. On an instance method the export silently does not appear,
and the mod then fails to load with "does not export mod_load".

Plain allocation, arrays, `StringBuilder`, integer and long formatting, and
`Math` all work.

## Hooks and the API

Exports are static methods annotated with `@Export`:

```java
@Export(name = "mod_load")        public static int  modLoad()            { return 0; }
@Export(name = "mod_unload")      public static void modUnload()          { }
@Export(name = "mod_draw_panel")  public static void modDrawPanel(int panel, int w, int h) { }
@Export(name = "mod_pre_turn")    public static void modPreTurn(int turn)  { }
@Export(name = "mod_post_turn")   public static void modPostTurn(int turn) { }
```

TeaVM also needs a `main` method as its entry point. A Gearbox mod is driven
entirely through its exports, so leave it empty.

Everything else is `org.opendoctrines.gearbox.Gearbox`:

```java
Gearbox.log(Gearbox.LOG_INFO, "hello");
Gearbox.env();                       // Env: isHeadless, screenW, platform, …
Gearbox.abort("unrecoverable");
Gearbox.fuelBudget();                // long; Long.MAX_VALUE when unmetered

Gearbox.turnNumber();
Gearbox.countryCount();
Gearbox.countryAt(i);                // 0-based; Gearbox.INVALID if out of range
Gearbox.countryName(c);              // String; two-call sizing done for you
Gearbox.countryTreasury(c);          // double
Gearbox.countryProvinceCount(c);
Gearbox.provincePopulation(p);
Gearbox.provinceOwner(p);

Gearbox.panelRegister(title, minW, minH);
Gearbox.drawText(panel, x, y, rgba, text);
Gearbox.drawRect(panel, x, y, w, h, rgba);
Gearbox.button(panel, x, y, w, h, label);   // returns boolean, not 0/1

Gearbox.assetSize(name);
Gearbox.assetRead(name);             // byte[], or null if absent
```

Indices stay **0-based** here, unlike the Lua SDK — Java arrays are 0-based, so
matching the ABI is also the least surprising choice. `button` returns a
`boolean` rather than the ABI's 0/1.

The raw `Gearbox.rawXxx` natives are the ABI verbatim, taking pointers as
`int`. You should not need them; they are public so that a mod doing something
unusual is not forced to fork the binding.

## Capabilities

TeaVM drops any import the mod never calls, so a Java mod only imports the
capabilities it actually uses — there is no equivalent of the Lua SDK's
build-time capability flags, and none is needed.

That does mean the right `modules` list depends on what your code calls. After
building, run:

```bash
python3 tools/wasm_imports.py sdk/java/examples/hello-panel/mod.wasm
```

and keep `MANIFEST.json` in step with what it reports.

## Adding your own mod

Copy `examples/hello-panel`, change the `artifactId`, the `mainClass`, and the
`MANIFEST.json` id, and add the module to `sdk/java/pom.xml`. The binding is a
normal Maven dependency (`org.opendoctrines:gearbox`), so a mod split across
several classes or packages needs nothing special.
