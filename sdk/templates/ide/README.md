# IDE integration

`gearbox new` copies these into every scaffolded mod, so a mod folder opens as a
working project in VS Code, CLion, IntelliJ or Rider with a Build button that
does the right thing.

There is no plugin to install. A plugin would have to be written twice (VSIX and
IntelliJ Platform), signed, published and kept current with two IDE release
trains — for the sake of invoking one script. These files invoke the same script
and work today.

**One edit is needed:** open `.vscode/settings.json` and set `gearbox.path` to
the `tools/gearbox` script in your OpenDoctrines checkout (on Windows,
`tools\gearbox.ps1`). JetBrains users set a `GEARBOX` path variable under
*Settings > Appearance & Behaviour > Path Variables*.

| IDE | What you get |
|---|---|
| VS Code | **Ctrl/Cmd+Shift+B** builds, verifies, packs and installs. Compiler errors are clickable. |
| CLion / IntelliJ / Rider | A *Gearbox: build + install* run configuration. |
| Anything else | `gearbox dev .` — that is all the above actually run. |

After a build: **Mod Menu → Reload modloader** in the game. Mods never load any
other way.
