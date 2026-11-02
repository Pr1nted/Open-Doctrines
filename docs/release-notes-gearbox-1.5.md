Two imports and one capability, all additive. A mod built against 1.4 runs
unchanged: `tools/check_abi_compat.py` passes against every shipped ABI from
1.0 onward, and nothing was renamed, removed or re-signed.

**Reshaping the map -- `Map.Write`.** `morph_province` hands the pixels inside
an SVG path to a province (optionally only within its own land), and `terrain`
raises sea to land or sinks land to sea along a path. Borders, adjacency, the
political texture and the labels follow on their own. It is a capability of its
own because it changes the map every player is looking at; the same two
operations are open to map scripts as `morph` and `terrain` (#OD/MapEngine/4).

**A wider capability mask.** `Map.Write` took bit 30 of 32, so the mask is now
64 bits, before the last bit was spent rather than after. Grants saved by an
older build load unchanged.

**Safer to run.** Call-time guards on every import, signed releases, checked
handle lifetimes, mod audio mixed on one bus, and fuel metered on the web
build so a hook cannot hang the tab.
