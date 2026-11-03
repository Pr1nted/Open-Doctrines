**Message 1**

**1.3.3b is out — our biggest update yet.** :rocket:

**30+ platforms.** Steam Deck (touch, controller, Game Mode fullscreen), Raspberry Pi OS, ChromeOS, NetBSD + Arm builds of every BSD, OpenIndiana, Haiku, SerenityOS. Homebrew for **PS Vita, Wii, 3DS, DOS and RISC OS**. Two experiments: **FreeRTOS** over a serial line and **Plan 9** with a clickable map — both play the exact same game, turn for turn. The **Unifico launcher (0.1.2)** runs on all the new desktops too.

**~3× faster turns**, bit-identical results (borders rebuilt only where they changed, NEON/SSE2 inner loop). Saves store each turn as a diff, compressed with libdeflate: faster to write, smaller. Map editor ~3× faster. Worlds can load at 1/2–1/8 resolution for small machines, there's a no-GPU software renderer, and experimental **ANGLE** (Metal/D3D11/Vulkan) for bad GL drivers.

**New Accessibility tab:** high contrast, text up to 120%, reduce motion, hold to confirm war and deletes, longer messages, visible sound cues, and the **Atkinson Hyperlegible** font. In every language.

**Message 2**

**Modders: Gearbox 1.5.** Mods can reshape the map at runtime — morph provinces, raise or sink land — under the new `Map.Write` capability. Map scripts get `morph` and `terrain` as `#OD/MapEngine/4`. Sandbox hardened (call-time guards, signed mod releases, a time budget on web). 1.4 mods run unchanged.

**Multiplayer:** orders resolve identically on every machine, boat landings go over the wire, tournaments seat **32**, hosts can `schedule +3d start` and auto-release idle seats. Friendlier sign-in (30-min browser window, a way back from a mislinked account).

**Safer:** releases are now **cryptographically signed** — the game verifies every update before installing it. Crash-proof saves, sealed fog-of-war saves, more stack protection, updated TLS.

**Also:** readable vector font by default (or import your own), monuments and sector flows in the economy breakdown, Retina-sharp web build with an on-screen keyboard, raylib 6.2.

**Next up (1.3.4b):** the AI groundwork is in — a network visualiser and a search-based Parrot Zero that looks ahead.

Download: https://github.com/Pr1nted/Open-Doctrines/releases/tag/v1.3.3b (or update via itch / Unifico)
Running it on something unusual? Show us in #showcase :eyes:
