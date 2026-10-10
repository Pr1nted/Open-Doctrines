#!/usr/bin/env python3
"""The Steam cut: the picture for tools/trailer_march.py's march.

    python3 tools/trailer_march_cut.py            # build all 46 shots
    python3 tools/trailer_march_cut.py 20 21 22   # rebuild only these
    python3 tools/trailer_march_cut.py --cards    # just regenerate the cards

This is a second cut, not a replacement. `trailer_build.py` holds the 40-shot
trip-hop cut and still builds; this one reuses its machinery -- the grade, the
scope frame, the eased dolly, the transition table -- and supplies its own shot
list, its own cards, and its own score.

THE TURN AT 0:31. The march was written so that its two halves are the same
sixteen bars played two ways: AFTERMATH is the Irish lament (choir, no drum)
and REBUILD is the parade (everything, gold). The picture does the same thing
with the same NUMBERS. AFTERMATH counts the cost of a real 120-turn game and
REBUILD sells the identical run as a triumph -- 780 conquests become 780
victories, the people who changed flag become the people who were liberated,
and the population graph really did go up seventy-six percent.

Neither half invents anything. Every figure on screen comes out of
`docs/trailer/assets/toll.json`, which `tools/trailer_toll.py` reads from
`data/saves/Quick Start.odsv`. That is the whole argument: propaganda does not
have to lie, it only has to choose which true thing to print. A trailer that
made the numbers up would be making a much weaker point.

WHY THE CUTTING IS FAST IN PLACES. The note on the last cut was that it felt
"stuck on a map". Most plates in the tour ARE the same West Africa map with a
different panel over it, so sitting on any of them reads as one screenshot held
too long. The fix is to punch into the PANEL rather than show the map, and to
break the montage with things that are not maps at all: the globe, the charts,
the TempleOS palette, the fly, and the cards. The four quick cuts at 0:24 and
again at 1:20 land on the march's "hurrah" rests -- the melody leaves four
holes for the drums, and the picture uses them.
"""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trailer_build as TB                                      # noqa: E402

ASSET = TB.ASSET
WORK  = os.environ.get("TRAILER_WORK", "/tmp/trailer-march")
CARDS = os.path.join(WORK, "cards")
P     = TB.P
AW, AH, FPS = TB.AW, TB.AH, TB.FPS
SUNKEN, TEXT, GOLD = TB.SUNKEN, TB.TEXT, TB.GOLD
TOLL = json.load(open(os.path.join(ASSET, "toll.json")))

# ------------------------------------------------------------------- cards

def _font(px):
    from PIL import ImageFont
    return ImageFont.truetype(TB.FONT, px)

def card_png(path, big, small=None, gold=False, rule=False, big_px=150,
             small_px=38, track=0):
    """A full-frame typographic plate.

    The captions in trailer_build sit low and left over a picture; these cards
    ARE the picture, so they are centred and large. Cold white for the cost,
    gold for the parade -- the two halves never share a colour.
    """
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (AW, AH), SUNKEN)
    d = ImageDraw.Draw(im)
    col = GOLD if gold else TEXT
    fb = _font(big_px)
    lines = big.split("\n")
    total = len(lines) * (big_px + 16)
    y = (AH - total) // 2 - (34 if small else 0)
    for line in lines:
        if track:
            w = sum(d.textlength(c, fb) + track for c in line) - track
            x = (AW - w) / 2
            for c in line:
                d.text((x, y), c, font=fb, fill=col)
                x += d.textlength(c, fb) + track
        else:
            d.text(((AW - d.textlength(line, fb)) / 2, y), line, font=fb, fill=col)
        y += big_px + 16
    if rule:
        d.line([(AW / 2 - 240, y + 14), (AW / 2 + 240, y + 14)], fill=col, width=2)
        y += 34
    if small:
        fs = _font(small_px)
        for line in small.split("\n"):
            d.text(((AW - d.textlength(line, fs)) / 2, y + 18), line, font=fs,
                   fill=tuple(int(c * 0.72) for c in col))
            y += small_px + 10
    im.save(path)
    return path

def count_frames(dirpath, target, label, dur, gold=False):
    """The one number that moves.

    A card cannot count, and the cost of the war is the single figure the whole
    first half exists to deliver, so it is rendered as frames and encoded as a
    clip. Eased, so it decelerates into the final value instead of stopping
    dead on it, and the digits are drawn on a fixed grid -- proportional digits
    reflow the number on every frame and the result jitters.
    """
    from PIL import Image, ImageDraw
    os.makedirs(dirpath, exist_ok=True)
    n = max(2, int(dur * FPS))
    fb, fs = _font(150), _font(38)
    col = GOLD if gold else TEXT
    # Widest digit sets the cell, so the number never reflows mid-count.
    probe = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    cw = max(probe.textlength(c, fb) for c in "0123456789")
    sw = probe.textlength(",", fb)
    final = "{:,}".format(target)
    width = sum(sw if c == "," else cw for c in final)
    for i in range(n):
        k = i / (n - 1)
        eased = 1 - (1 - k) ** 3                       # decelerate into it
        val = int(round(target * eased))
        im = Image.new("RGB", (AW, AH), SUNKEN)
        d = ImageDraw.Draw(im)
        x = (AW - width) / 2
        y = (AH - 150) / 2 - 40
        for c in "{:,}".format(val).rjust(len(final)):
            if c != " ":
                adv = sw if c == "," else cw
                d.text((x + (adv - d.textlength(c, fb)) / 2, y), c, font=fb, fill=col)
            x += sw if c == "," else cw
        for j, line in enumerate(label.split("\n")):
            d.text(((AW - d.textlength(line, fs)) / 2, y + 196 + j * 48),
                   line, font=fs, fill=tuple(int(c * 0.72) for c in col))
        im.save(os.path.join(dirpath, "f%04d.png" % i))
    return n

def shot_count(out, dirpath, dur, overlay=None):
    """Encode the counter frames, then put them through the same tail as every
    other shot so the grade, the vignette and the grain match."""
    raw = out + ".raw.mp4"
    TB.run(["-framerate", str(FPS), "-i", os.path.join(dirpath, "f%04d.png")]
           + TB.COMMON + [raw])
    fg = "[0:v]null[pic];" + TB.tail("pic", None, dur)
    TB.run(["-i", raw, "-filter_complex", fg, "-map", "[o]",
            "-t", str(dur)] + TB.COMMON + [out])
    os.remove(raw)

def make_cards():
    os.makedirs(CARDS, exist_ok=True)
    C = lambda n: os.path.join(CARDS, n + ".png")
    t = TOLL
    # ---- the cost.  Cold, centred, no gold anywhere in this half.
    card_png(C("toll1"), "{:,}".format(t["provinces_conquered"]),
             "provinces changed hands\nin one hundred and twenty turns", big_px=190)
    card_png(C("toll2"), "{:,}".format(t["conquests"]),
             "times they changed hands", big_px=190)
    card_png(C("toll3"), "{:,}".format(t["emptied"]),
             "of them lost three quarters of their people", big_px=190)
    # ---- the parade.  Same run, same save, gold.
    card_png(C("pr1"), "{:,}".format(t["conquests"]), "VICTORIES",
             gold=True, rule=True, big_px=190, small_px=46)
    card_png(C("pr2"), "{:,}".format(t["people_unique"]), "LIBERATED",
             gold=True, rule=True, big_px=130, small_px=46)
    card_png(C("pr3"), "+%d%%" % round(t["world_pop_growth_pct"]),
             "POPULATION", gold=True, rule=True, big_px=190, small_px=46)
    card_png(C("kicker"), "EVERY FIGURE\nIS TRUE", big_px=112, track=5)
    card_png(C("black"), " ", big_px=40)
    count_frames(os.path.join(CARDS, "count"), t["lost_after_conquest"],
                 "people gone from provinces\nthat had just been taken", 5.46)
    return CARDS

# --------------------------------------------------------------- the cut
# Times are the march's own section boundaries (tools/trailer_march.py --cues).
# The four 0.45-0.70 s shots at 0:24 and at 1:20 sit on the hurrah rests.

def K(n):
    return os.path.join(CARDS, n + ".png")

# ---- the footage that MOVES ------------------------------------------------
# tools/trailer_timelapse.sh writes these: the real map, one frame per turn,
# 1920x832 at 30 fps. tools/trailer_blender.sh writes the desk shot.
TLDIR = os.path.join(WORK, "tl")
POL   = os.path.join(TLDIR, "political-fast.mp4")
TRP   = os.path.join(TLDIR, "troops-fast.mp4")
FLYV  = os.path.join(WORK, "fly-desk.mp4")

# 820 turns of Quick Start in 19.7 s. A shot has to END inside that, so no
# clip starts later than about turn 640 -- the transition pad runs past the
# slot and a clip that runs out freezes on its last frame mid-dissolve.
TURNS, FOOTAGE = 820.0, 19.7

def tn(turn):
    """Source timestamp of a turn, so the cut is written in game terms."""
    return round(min(turn, 640) * FOOTAGE / TURNS, 2)

W_, EURASIA, EUROPE = TB.WORLD, TB.EURASIA, TB.EUROPE
SASIA, AFRICA, SAMER = TB.SASIA, TB.AFRICA, TB.SAMER
ATLANT, FAREAST = TB.ATLANT, TB.FAREAST

SHOTS = [
    # ---- HALT 0:02.3 -- field drum alone, no tune --------------------------
    (1,  "0:00.0", 2.33, "card",   None, None, dict(fade_in=1.2)),
    (2,  "0:02.3", 2.72, "clip",   (POL, tn(0), 1.0, ATLANT), None,
                   dict(drift=(-6, 0), lift=0.92, into="mix")),
    (3,  "0:05.1", 2.73, "clip",   (POL, tn(40), 1.0, EURASIA),
                   "EVERY BORDER IS AN ARGUMENT",
                   dict(drift=(7, -2), caps=True, track=6, into="mix")),

    # ---- MARCH IN 0:07.8.  Phrase A1: the board ----------------------------
    (4,  "0:07.8", 1.85, "clip",   (POL, tn(80), 1.0, W_), "1,641 provinces",
                   dict(into="dip")),
    (5,  "0:09.6", 1.80, "freeze", (P("province"),), None,
                   dict(focus=(0.30, 0.40), zoom=1.6, push=0.08, into="mix")),
    (6,  "0:11.4", 1.80, "clip",   (POL, tn(140), 1.0, EUROPE), None,
                   dict(drift=(5, 0), into="smear")),
    # Phrase A2: the systems.  The only run of stills in the first half -- a
    # tech tree and a ledger do not move, and pretending they do is worse.
    (7,  "0:13.2", 1.80, "freeze", (P("research"),), "86 technologies",
                   dict(focus=(0.22, 0.35), zoom=1.5, lift=1.90, into="dip")),
    (8,  "0:15.0", 1.80, "freeze", (P("goods"),), None,
                   dict(focus=(0.45, 0.40), zoom=1.4, lift=1.45, into="smear")),
    (9,  "0:16.8", 1.86, "freeze", (P("country-profile"),), "every number is yours to move",
                   dict(focus=(0.30, 0.32), zoom=1.15, lift=1.70, into="mix")),
    # Phrase B1: the drum enters, and so do the armies.
    (10, "0:18.7", 1.40, "clip",   (TRP, tn(200), 1.0, EURASIA), None,
                   dict(drift=(-8, 0), into="dip")),
    (11, "0:20.1", 1.35, "freeze", (P("army-kinds"),), None,
                   dict(focus=(0.44, 0.52), zoom=1.45, into="cut")),
    (12, "0:21.4", 1.35, "clip",   (TRP, tn(260), 1.0, SASIA), None,
                   dict(drift=(6, 0), into="cut")),
    (13, "0:22.8", 1.35, "freeze", (P("globe-orders"),), None,
                   dict(focus=(0.44, 0.44), push=0.07, lift=1.12, into="smear")),
    # Phrase B2: the four hurrah rests.  Four hard cuts, on the holes.
    (14, "0:24.1", 0.46, "freeze", (P("globe-resources"),), None,
                   dict(focus=(0.46, 0.44), push=0.06, lift=1.15, into="cut")),
    (15, "0:24.6", 0.68, "freeze", (P("globe-population"),), None,
                   dict(focus=(0.46, 0.44), push=0.06, lift=1.15, into="cut")),
    (16, "0:25.3", 0.68, "freeze", (P("specifics"),), None,
                   dict(focus=(0.70, 0.38), zoom=1.25, into="cut")),
    (17, "0:26.0", 0.68, "freeze", (P("editor-districts"),), None,
                   dict(focus=(0.40, 0.45), zoom=1.5, into="cut")),
    (18, "0:26.6", 2.96, "clip",   (POL, tn(320), 1.0, W_),
                   "AND NONE OF IT IS FREE",
                   dict(caps=True, track=5, into="mix")),

    # ---- STOP 0:29.6 -- the hinge.  Nothing. --------------------------------
    (19, "0:29.6", 1.36, "card",   None, None, dict(into="dip")),

    # ---- AFTERMATH 0:31.0 -- what it cost.  Held still ON PURPOSE: this is
    #      the one section that should stop moving, and the counter is the only
    #      thing in the trailer that animates by itself.
    (20, "0:31.0", 1.85, "freeze", (K("toll1"),), None, dict(push=0.03, into="dip")),
    (21, "0:32.8", 1.80, "freeze", (K("toll2"),), None, dict(push=0.03, into="mix")),
    (22, "0:34.6", 1.80, "freeze", (K("toll3"),), None, dict(push=0.03, into="mix")),
    (23, "0:36.4", 5.46, "still",  (os.path.join(CARDS, "count"),), None,
                   dict(into="mix")),

    # ---- REBUILD 0:41.9 -- the same run, sold ------------------------------
    (24, "0:41.9", 2.40, "freeze", (K("pr1"),), None, dict(push=0.04, into="dip")),
    (25, "0:44.3", 2.40, "freeze", (K("pr2"),), None, dict(push=0.04, into="cut")),
    (26, "0:46.7", 2.40, "freeze", (K("pr3"),), None, dict(push=0.04, into="cut")),
    (27, "0:49.1", 2.00, "clip",   (POL, tn(440), 1.0, W_), None,
                   dict(lift=1.22, into="smear")),
    (28, "0:51.1", 2.00, "freeze", (P("economy-local"),), None,
                   dict(focus=(0.58, 0.42), zoom=1.35, lift=1.55, into="cut")),
    (29, "0:53.1", 2.00, "clip",   (POL, tn(520), 1.0, AFRICA), None,
                   dict(drift=(5, 0), lift=1.18, into="smear")),
    (30, "0:55.1", 3.17, "freeze", (K("kicker"),), None, dict(push=0.02, into="dip")),

    # ---- FULL 0:58.2 -- everything, octave doubled --------------------------
    (31, "0:58.2", 1.90, "freeze", (P("globe-navy"),), None,
                   dict(focus=(0.46, 0.43), push=0.07, lift=1.18, into="dip")),
    (32, "1:00.1", 1.80, "clip",   (TRP, tn(400), 1.0, FAREAST), None,
                   dict(drift=(-7, 0), into="smear")),
    (33, "1:01.9", 1.80, "freeze", (P("diplomacy"),), "they argue back",
                   dict(focus=(0.80, 0.30), zoom=1.7, into="mix")),
    (34, "1:03.7", 1.80, "freeze", (P("tutorial"),), None,
                   dict(focus=(0.76, 0.45), zoom=1.7, into="cut")),
    (35, "1:05.5", 1.80, "freeze", (P("topic-economy"),), None,
                   dict(focus=(0.76, 0.42), zoom=1.7, into="cut")),
    (36, "1:07.3", 1.80, "freeze", (P("mp-lobby"),), "and against each other",
                   dict(focus=(0.48, 0.44), zoom=1.25, lift=1.55, into="smear")),
    (37, "1:09.1", 1.80, "freeze", (P("templeos-game"),), None,
                   dict(focus=(0.50, 0.45), push=0.06, lift=1.10, into="mix")),
    (38, "1:10.9", 1.80, "clip",   (POL, tn(600), 1.0, SAMER), None,
                   dict(drift=(4, 0), into="cut")),
    (39, "1:12.7", 1.86, "marks",  ("windows", "apple", "linux", "browser"), None,
                   dict(into="dip")),

    # ---- WIDE 1:14.6 -- the 3-D shot.  Modelled in tools/trailer_blender.py,
    #      with the political timelapse playing on the monitor, so the camera
    #      moves THROUGH a room into the game and the next section can cut out
    #      of the screen as if through it.
    (40, "1:14.6", 5.45, "clip",   (FLYV, 0.0, 1.0, None),
                   "even the fly has a save file", dict(into="dip")),

    # ---- LOGO 1:20.1 -- four wars on the hurrah rests, then the name -------
    (41, "1:20.1", 0.45, "clip",   (TRP, tn(430), 1.0, EUROPE), None, dict(into="dip")),
    (42, "1:20.5", 0.69, "clip",   (TRP, tn(480), 1.0, SASIA), None, dict(into="cut")),
    (43, "1:21.2", 0.68, "clip",   (TRP, tn(540), 1.0, FAREAST), None, dict(into="cut")),
    (44, "1:21.9", 0.68, "clip",   (TRP, tn(600), 1.0, AFRICA), None, dict(into="cut")),
    (45, "1:22.6", 1.59, "clip",   (POL, tn(640), 1.0, W_), None, dict(into="cut")),
    (46, "1:24.1", 6.06, "logo",   "big", "open-doctrines.com",
                   dict(into="dip", fade_out=1.6)),
]

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    os.makedirs(WORK, exist_ok=True)
    make_cards()
    if "--cards" in sys.argv[1:]:
        print("cards -> " + CARDS)
        return
    TB.WORK  = WORK
    TB.SHOTS = SHOTS
    TB.SCORE = os.path.join(ASSET, "march.wav")
    # One extra shot kind: the counter.  Patch it in rather than branching
    # trailer_build, so there is still exactly one assembly path.
    real_still = TB._still
    def still_or_count(out, images, dur, overlay, **kb):
        if len(images) == 1 and os.path.isdir(images[0]):
            return shot_count(out, images[0], dur, overlay)
        return real_still(out, images, dur, overlay, **kb)
    TB._still = still_or_count
    TB.build(set(int(a) for a in args) or None)

if __name__ == "__main__":
    main()
