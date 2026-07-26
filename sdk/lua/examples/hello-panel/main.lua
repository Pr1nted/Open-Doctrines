-- Hello Panel, in Lua — the same mod as sdk/examples/hello-panel/mod.c.
--
-- Shows the turn number, how many countries are alive, and the treasury of
-- whichever country you step to with the button. ModExamplesTest renders this
-- alongside the other SDKs and fails the build if a single string differs, so
-- the formatting below is deliberately exact rather than idiomatic.
--
-- Hooks are plain globals: define mod_load, mod_draw_panel, mod_unload and the
-- host calls them. Everything else is local, as usual.

local panel  = 0
local cursor = 1        -- gearbox.countryAt is 1-based; see sdk/lua/README.md

function mod_load()
    if gearbox.env().isHeadless then
        -- A training run has no renderer, so registering a panel would be a
        -- no-op. Skipping it makes the intent explicit.
        gearbox.log(gearbox.INFO, "hello-panel: headless, no UI")
        return 0
    end

    panel = gearbox.panelRegister("Hello Panel", 280, 150)
    if panel == 0 then
        -- UI was declared but revoked, or we hit the panel limit. Degrade
        -- rather than trap.
        gearbox.log(gearbox.WARN, "hello-panel: no panel, running quiet")
    end
    return 0
end

function mod_unload()
    cursor = 1
end

-- Truncates toward zero, matching the C reference's (uint64_t) cast rather
-- than Lua's floor, so -50.25 prints as -50 and not -51.
local function trunc(v)
    return v < 0 and -math.floor(-v) or math.floor(v)
end

function mod_draw_panel(p, w, h)
    gearbox.drawRect(p, 0, 0, w, 1, 0x3C3C5AFF)

    gearbox.drawText(p, 8, 8, 0xFFFFFFFF,
                     string.format("Turn %d", gearbox.turnNumber()))

    local count = gearbox.countryCount()
    gearbox.drawText(p, 8, 28, 0xB4B4C8FF,
                     string.format("Countries: %d", count))

    if count == 0 then
        gearbox.drawText(p, 8, 52, 0x9696A0FF, "No world loaded")
        return
    end
    if cursor > count then cursor = 1 end

    local c = gearbox.countryAt(cursor)
    if c then
        gearbox.drawText(p, 8, 52, 0xFFFFFFFF, gearbox.countryName(c))
        gearbox.drawText(p, 8, 72, 0xB4B4C8FF,
                         string.format("Provinces: %d",
                                       gearbox.countryProvinceCount(c)))
        gearbox.drawText(p, 8, 92, 0xB4B4C8FF,
                         string.format("Treasury: %d",
                                       trunc(gearbox.countryTreasury(c))))
    end

    if gearbox.button(p, 8, 116, 120, 24, "Next country") then
        cursor = cursor + 1
    end
end
