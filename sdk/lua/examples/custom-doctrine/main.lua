-- Custom Doctrine, in Lua — the same mod as sdk/examples/custom-doctrine.
--
-- Adds two doctrines to the game's catalogue and does nothing else: no panel,
-- no turn hook, no state. Two capabilities, Core (log) and Content.
--
-- ── WHAT LUA CHANGES, AND WHAT IT DOES NOT ──
--
-- The C version cannot parse JSON -- it is freestanding, with no libc -- so its
-- build script splits doctrines.json at build time and hands the C a table of
-- (id, json) pairs. Lua has no json library here either (the sandbox compiles
-- Lua's own core and nothing else), so this mod takes the same road: build.sh
-- turns doctrines.json into doctrines.lua, a plain Lua table of id and the
-- definition as a string.
--
-- What Lua does buy is that the table is REAL LUA. You can inspect it, build a
-- definition at load time, or generate twenty variants in a loop -- none of
-- which the C version can do without an allocator.
--
-- ── WHAT CROSSES THE BOUNDARY ──
--
-- gearbox.contentAdd(kind, id, definition, mode) -- four arguments, not six.
-- The string lengths the ABI wants are taken from the Lua strings at the
-- binding, which is the one place in this SDK where the ergonomics differ from
-- the wire and the reason the generated binding exists at all.
--
-- See docs/gearbox-custom-policy.md for what every field in doctrines.json
-- does, including the sign convention for cost levers, which is inverted.
--
-- ── THE SAME IDS AS THE C EXAMPLE, ON PURPOSE ──
--
-- These two mods add the same two doctrines, from the same doctrines.json, so
-- installing BOTH means the second one loses: catalogue ids are global, the
-- first mod to claim one keeps it, and the second logs which mod has it. That
-- is not a bug in the example, it is the collision path -- and having it
-- reachable by installing two mods that ship together is a better way to see
-- it than reading about it.
--
-- Build:  ./build.sh          (produces custom-doctrine-lua.odmod)

-- `doctrines` is not required or loaded: there is no filesystem in the sandbox
-- and no `require` that could reach one. build.sh writes the generated table
-- into the front of the SAME CHUNK as this file, so its `local doctrines` is
-- already in scope here -- which is also why this file cannot be run on its
-- own with a stock lua binary, and why build.sh is the only way to try it.

function mod_load()
    local added = 0

    for _, d in ipairs(doctrines) do
        -- kind 0 is doctrine, mode 1 is PERSIST: the save keeps the definition,
        -- so a country that adopted this can still say what it adopted after
        -- the mod is uninstalled.
        if gearbox.contentAdd(gearbox.CONTENT_DOCTRINE, d.id, d.json,
                              gearbox.CONTENT_PERSIST) then
            added = added + 1
        else
            -- Refused: either somebody else owns the id or the definition
            -- could not be read. contentOwnerOf separates the two, and a mod
            -- that logged only "refused" would leave its author guessing.
            --
            -- Asked only AFTER the failure, never before it: on a reload the
            -- owner would be this mod itself, and a mod that checked first
            -- would skip its own entries every time it started.
            local owner = gearbox.contentOwnerOf(gearbox.CONTENT_DOCTRINE, d.id)
            if owner ~= "" then
                gearbox.log(gearbox.ERROR, string.format(
                    "doctrine '%s' is already owned by %s", d.id, owner))
            else
                gearbox.log(gearbox.ERROR, string.format(
                    "doctrine '%s' was refused: check the id and the JSON", d.id))
            end
        end
    end

    -- A content mod that added nothing has nothing to do, and saying so at load
    -- is kinder than sitting in the mod list looking active. The value is shown
    -- to the player.
    if added == 0 then return 1 end

    gearbox.log(gearbox.INFO, string.format(
        "custom-doctrine-lua: added %d of %d doctrines", added, #doctrines))
    return 0
end

-- The host removes a mod's content when it unloads, so there is nothing to
-- undo. Defined anyway, because its absence reads as an oversight.
function mod_unload()
end
