-- YOUR FIRST OPEN DOCTRINES MOD
--
-- The whole thing is this file. It adds one doctrine to the politics screen,
-- and that is all it does -- no panel, no turn hook, nothing to clean up.
--
-- Everything a doctrine is lives in the JSON below, and it is the SAME format
-- the game's own data/policies.json uses: a mod's doctrine goes through the
-- very same parser the shipped ones do. There is no separate mod format, and
-- no field that works from the file but not from your mod.
--
-- Build it with ./build.sh, drop the .odmod it produces into your mods folder,
-- and enable it from the mod menu. See docs/gearbox-custom-policy.md for what
-- every field means.

local DOCTRINE = [[
{
  "name": "Rural Electrification",
  "description": "Wire the villages. It costs a fortune and it never stops paying.",

  "category": "left",
  "folder": "My First Mod",

  "cost_per_turn": 8,
  "implementation_turns": 5,

  "requirements": { "max_economic": 40 },
  "compass_shift": { "economic": -10, "social": 4 },

  "levers": {
    "popGrowthPct": 0.6,
    "industryCostPct": 8,
    "resourceModPct": 5
  },

  "effects": { "unrest_reduction": 0.01 },

  "tradeoffs": {
    "gains": [
      "Population growth +0.6%/turn",
      "Industry cost -8%",
      "Resource income +5%",
      "Unrest -1.0%"
    ],
    "costs": [
      "-8/turn income",
      "Economic -10",
      "Social +4"
    ]
  }
}
]]

-- The game calls this once, when your mod loads.
function mod_load()
    -- kind, id, definition, mode.
    --
    -- The id is how a save records a country holding this, so pick one and
    -- never change it -- and put your own name in front of the colon, because
    -- ids are shared with every other mod and the first to claim one keeps it.
    --
    -- CONTENT_PERSIST writes the definition into the save. A campaign that
    -- adopted your doctrine still knows what it was after your mod is gone;
    -- without it, that save becomes a file full of ids nothing can read.
    local ok = gearbox.contentAdd(gearbox.CONTENT_DOCTRINE,
                                  "com_example:rural_electrification",
                                  DOCTRINE,
                                  gearbox.CONTENT_PERSIST)
    if not ok then
        -- Refused: either the JSON could not be read, or another mod already
        -- owns that id. Asking who owns it turns "it did not work" into a
        -- sentence you can act on.
        local owner = gearbox.contentOwnerOf(gearbox.CONTENT_DOCTRINE,
                                             "com_example:rural_electrification")
        if owner ~= "" then
            gearbox.log(gearbox.ERROR, "that id already belongs to " .. owner)
        else
            gearbox.log(gearbox.ERROR, "the doctrine could not be read")
        end
        return 1        -- non-zero refuses the load, and the player is told
    end

    gearbox.log(gearbox.INFO, "Rural Electrification is available")
    return 0
end
