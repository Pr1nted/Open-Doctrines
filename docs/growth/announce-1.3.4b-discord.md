# Open Doctrines 1.3.4b is out

Hello everyone, **1.3.4b** is out. This is a follow-up update to 1.3.3b and preparing ourselves to 1.4.0b. This update concentrates on more QOL features, enjoy!

## Eight calendars
The in-game date now reads in **BC/AD, BCE/CE, Ab urbe condita, the Chinese sexagenary cycle, Kōki, Juche, the Hebrew calendar, or the Holocene era** - flip it in **Settings -> Display** and the whole HUD counts in it.

## Rome, 50 BC
A new map ships alongside: the **Roman Republic at its height**, real borders from OpenHistoricalMap. Set the date to *Ab urbe condita* and read the year like a Roman.

## The AI settles empty land
The AI now **colonises unclaimed territory**, so the blank quarters of the map fill in over a long campaign instead of staying empty.

## Watch the AI think
Turn on **Debug Mode** (Settings -> Display), then **AI Debug** (Settings -> Advanced), and press **F8** in a game. You get a live feed of every decision the AI made this turn, a trace of the one under the cursor, and a neural-activity panel that replays a decision with the network's input, hidden, embedding and attention-head layers lighting up as it thinks. A look straight into the AI's head.

## Random events
Worlds can be created with **deterministic random events** (Experimental) - same seed, same run, every time. Three ship to start, all upside:
- **Gold Rush** - a gold province strikes it rich (treasury up)
- **Bumper Harvest** - a populous province booms (population up)
- **Boom Town** - an industrial province pulls in workers + trade (both up)

They're plain data (`od-events/1` format), so maps and mods can add their own.

## Modding: GDTL + Shaders SDK
- **Greater Diplomacy translation layer** ships enabled - convert maps to/from **Greater Diplomacy 5** and **Unciv**.
- **Gearbox SDK 1.6** adds the **Shaders** capability: mods can apply the host's screen effects to the map or their own panels - safely (no raw framebuffer, can't strobe, player can switch it all off). Every 1.5 mod still works.

## Fixes
- **Every exotic port builds again** - DOS, **PS Vita**, and the **3DS** are all back and green.
- **Web build** fixed, and the **AI correspondents** (campaign mail) are talking to their model again.

Grab your build from the download list. On Vita / 3DS / DOS? Tell us how it runs. See you on the map.
