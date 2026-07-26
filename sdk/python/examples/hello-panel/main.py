# Hello Panel, in Python — the same mod as sdk/examples/hello-panel/mod.c.
#
# ModExamplesTest renders this alongside the other SDKs and fails the build if a
# single string differs, so the formatting below is deliberately exact.
#
# Hooks are plain module-level functions: define mod_load, mod_draw_panel,
# mod_unload and the host calls them. `global` is needed to rebind them from
# inside a hook, as usual in Python.

import gearbox

panel = 0
cursor = 0          # 0-based, as the ABI is


def mod_load():
    global panel
    if gearbox.env()["isHeadless"]:
        # A training run has no renderer, so registering a panel would be a
        # no-op. Skipping it makes the intent explicit.
        gearbox.log(gearbox.INFO, "hello-panel: headless, no UI")
        return 0

    panel = gearbox.panelRegister("Hello Panel", 280, 150)
    if panel == 0:
        # UI was declared but revoked, or we hit the panel limit. Degrade
        # rather than raise.
        gearbox.log(gearbox.WARN, "hello-panel: no panel, running quiet")
    return 0


def mod_unload():
    global cursor
    cursor = 0


def mod_draw_panel(p, w, h):
    global cursor

    gearbox.drawRect(p, 0, 0, w, 1, 0x3C3C5AFF)

    gearbox.drawText(p, 8, 8, 0xFFFFFFFF, "Turn %d" % gearbox.turnNumber())

    count = gearbox.countryCount()
    gearbox.drawText(p, 8, 28, 0xB4B4C8FF, "Countries: %d" % count)

    if count == 0:
        gearbox.drawText(p, 8, 52, 0x9696A0FF, "No world loaded")
        return
    if cursor >= count:
        cursor = 0

    c = gearbox.countryAt(cursor)
    if c is not None:
        gearbox.drawText(p, 8, 52, 0xFFFFFFFF, gearbox.countryName(c))
        gearbox.drawText(p, 8, 72, 0xB4B4C8FF,
                         "Provinces: %d" % gearbox.countryProvinceCount(c))

        # int() truncates toward zero, matching the C reference's cast to an
        # unsigned integer: -50.25 prints as -50, not -51 as math.floor gives.
        gearbox.drawText(p, 8, 92, 0xB4B4C8FF,
                         "Treasury: %d" % int(gearbox.countryTreasury(c)))

    if gearbox.button(p, 8, 116, 120, 24, "Next country"):
        cursor += 1
