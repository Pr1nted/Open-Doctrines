Sixty-two imports and two capabilities, all additive. A mod built against 1.3
runs unchanged: `tools/check_abi_compat.py` passes 874 checks against every
shipped ABI from 1.0 onward, and nothing was renamed, removed or re-signed.

**Fog of war — `Fog` and `Fog.Write`.** Thirteen reads and three writes. The
reads answer what a country can SEE and what it REMEMBERS, which under fog are
different questions from what is true: `fog_province_visible`,
`fog_province_seen_turn`, `fog_known_owner`, `fog_point_visible`,
`fog_ship_visible` and five `fog_intel_*` fields for the garrison, population,
industry, fort and port a country last saw. In a world without fog every one
of them answers as though everything were visible, so a mod written for fog
needs no branch for its absence.

The writes grant sight: `fog_add_sight` for an area, `fog_reveal_province` for
one province, `fog_clear_grants` to drop them. A grant is a watchpost, not a
reveal — the fog is rebuilt from scratch every turn, so grants are held and
folded in on every recompute and saved with the world. They join the country's
own sight and so reach an ally one hop, exactly as a coast watch does.

`Fog.Write` is a separate capability from `Fog` on purpose: it changes what
another player can see, which makes it the one to look hardest at after
`Core.Protected`.

**Economy — 29 imports.** Monuments (`can_build_monument`, slot costs and
upkeep, `build_monument`), the goods economy (`goods_economy_on`, stocks,
demand, shortfall, living standards, directed output), sector taxes and
subsidies, auto-sell and nationalisation.

**Diplomacy — 11.** Greater-diplomacy acts and their state.

**Elsewhere — 6.** Province monuments through `gamestate.read`, and three
military reads and writes.

A note on the capability mask: 30 of 32 bits are now spent. The capability
after the next two needs a wider type and a save-format decision, not another
bit.
