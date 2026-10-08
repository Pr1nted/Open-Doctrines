#!/usr/bin/env python3
"""Assemble the trailer from the cut in docs/trailer/plan.md.

The cut is DATA, in SHOTS below, and it is the same table as the plan's --
start times, durations and captions.  Change one and change the other.

Every shot is rendered to its own 1920x1080 h264 segment with identical
parameters so the concat is a stream copy: no re-encode, no generation loss,
and a single shot can be rebuilt without touching the rest.

What this cannot make is marked PLACEHOLDER and rendered as a slate that says
so, on purpose -- a missing shot that reads as finished is how a trailer ships
with a hole in it.

    python3 tools/trailer_build.py              # everything
    python3 tools/trailer_build.py 7 8 13       # just those shots, then concat
"""
import os, subprocess, sys, shutil

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET = os.path.join(ROOT, "docs/trailer/assets")
WORK  = os.environ.get("TRAILER_WORK", "/tmp/trailer")
IMG   = os.path.join(ROOT, "docs/img")
FONT  = os.path.join(ROOT, "data/fonts/DejaVuSans.ttf")

W, H, FPS = 1920, 1080, 30
# The picture is 2.39:1 and the FILE is 16:9, which is what a scope film
# delivered to a storefront is. The bars are part of the frame, not a crop
# applied to it, so every shot composes into AW x AH and is padded once at the
# end -- captions included, which is why they sit at AH and not at H.
AW, AH = 1920, 804
BAR_Y  = (H - AH) // 2
SUNKEN = (0x09, 0x0B, 0x11)
TEXT   = (0xE8, 0xE4, 0xDA)
GOLD   = (0xC9, 0xA2, 0x27)

# Source footage.  COLLAPSE is `Bench modern CHN (5)`; see plan.md for why.
# 58.0 seconds carries 721 turns, so a turn is 0.0805 s of source.
COLLAPSE = os.environ.get("TL_COLLAPSE", "/tmp/tl-chn-collapse.mp4")
TROOPS   = os.environ.get("TL_TROOPS",   "/tmp/tl-troops.mp4")
FLY      = os.environ.get("FLY_SHOT",    "/tmp/fly_shot.png")
SRC_LEN  = 58.0
def turn(n):
    """Source timestamp of turn n, so the cut can be written in game terms."""
    return round(n * SRC_LEN / 721.0, 2)

# kind:  clip  (slice of a timelapse)   still (ken burns on a png)
#        freeze(hold one frame)         marks (platform icons)
#        card  (caption on the ground)  logo  (wordmark)
# Regions, named in degrees. The edit's first problem was that eight of its
# shots were the identical world view at the identical scale, and a viewer
# reads that as one long shot with things moving in it.
WORLD   = None
EURASIA = (80,  45, 200)
EUROPE  = (18,  48, 130)
SASIA   = (88,  26, 130)
AFRICA  = (14,   4, 140)
SAMER   = (-62, -20, 115)
ATLANT  = (-38,  12, 165)
FAREAST = (128, 50, 140)

# UI plates live in docs/trailer/assets/plates and are produced by
# tools/screenshots.sh, which photographs a SIMULATED world in a scratch data
# directory -- so re-running it gives the same kind of picture rather than
# whatever happened to be on this machine's screen. They all come from one tour
# so the country, the date and the map agree from shot to shot.
def P(name):
    return os.path.join(ASSET, "plates", name + ".png")

# `into` is how a shot ARRIVES. 31 straight cuts in a row read as a slideshow.
SHOTS = [
    # ---- open --------------------------------------------------------------
    (1,  "0:00.0", 4.0, "clip",  (COLLAPSE, turn(0),  1.0, WORLD), None,
                   dict(fade_in=1.5)),
    (2,  "0:04.0", 4.0, "clip",  (COLLAPSE, turn(60), 1.0, EURASIA),
                   "A GRAND STRATEGY GAME",
                   dict(track=6, caps=True, drift=(-7, 0), into="mix")),

    # ---- the game. The map is not the only thing in it, which is what this
    #      section exists to say: a globe, a tech tree, doctrines, districts,
    #      monuments -- each one angle, none of them captioned as a feature.
    (3,  "0:08.0", 1.8, "still", (P("world-map"),), "1,641 provinces",
                   dict(focus=(0.5, 0.45), into="smear")),
    (4,  "0:09.8", 1.6, "still", (P("province"),), None,
                   dict(focus=(0.14, 0.32), push=0.08, zoom=1.9, into="mix")),
    (5,  "0:11.4", 1.8, "still", (P("globe-names"),), None,
                   dict(focus=(0.60, 0.42), push=0.06, lift=1.25, into="dip")),
    (6,  "0:13.2", 1.6, "still", (P("globe-night"),), None,
                   dict(focus=(0.52, 0.45), push=0.08, lift=1.15, into="mix")),
    (7,  "0:14.8", 1.6, "still", (P("research"),), "86 technologies",
                   dict(focus=(0.22, 0.35), lift=1.90, zoom=1.5, into="smear")),
    (8,  "0:16.4", 1.4, "still", (P("research"),), None,
                   dict(focus=(0.16, 0.72), push=0.08, lift=1.90, zoom=2.1, into="mix")),
    (9,  "0:17.8", 1.6, "still", (P("policies"),), "59 doctrines",
                   dict(focus=(0.55, 0.14), lift=1.80, zoom=1.7, into="smear")),
    (10, "0:19.4", 1.4, "still", (P("districts"),), None,
                   dict(focus=(0.88, 0.48), push=0.08, lift=1.55, zoom=2.2, into="mix")),
    (11, "0:20.8", 1.4, "still", (P("monuments-map"),), None,
                   dict(focus=(0.08, 0.30), push=0.08, lift=1.35, zoom=2.2, into="mix")),

    # ---- orders. Drums are here; the only near-triumphant stretch. ---------
    (12, "0:22.2", 1.6, "still", (P("army-kinds"),), "Your orders are carried out",
                   dict(focus=(0.44, 0.44), push=0.08, lift=1.30, zoom=1.6, into="smear")),
    (13, "0:23.8", 1.4, "still", (P("orders-phase"),), None,
                   dict(focus=(0.50, 0.45), push=0.08, lift=1.30, zoom=2.2, into="mix")),
    (14, "0:25.2", 1.6, "clip",  (TROOPS, 4.0, 1.0, EURASIA), None,
                   dict(drift=(9, 0), into="smear")),
    (15, "0:26.8", 1.4, "clip",  (COLLAPSE, turn(372), 2.5, SASIA), None,
                   dict(drift=(8, 0), into="cut")),
    (16, "0:28.2", 1.4, "clip",  (COLLAPSE, turn(392), 2.5, AFRICA), None,
                   dict(drift=(-8, 0), into="cut")),

    # ---- THE STOP. Do not cut inside this. --------------------------------
    (17, "0:29.6", 3.2, "freeze",(COLLAPSE, turn(417), WORLD), "By someone",
                   dict(push=0.035, pull=True, into="dip")),

    # ---- the honest stretch. All cuts, because deadpan is cuts. -----------
    (18, "0:32.8", 1.8, "still", (P("economy"),), "Industry you cannot afford",
                   dict(focus=(0.5, 0.24), lift=1.20, zoom=1.4, into="cut")),
    (19, "0:34.6", 1.5, "still", (P("goods"),), None,
                   dict(focus=(0.26, 0.33), push=0.08, lift=1.45, zoom=1.9, into="cut")),
    (20, "0:36.1", 1.5, "still", (P("goods-factory"),), None,
                   dict(focus=(0.50, 0.40), push=0.08, lift=1.25, zoom=1.8, into="cut")),
    (21, "0:37.6", 1.8, "still", (P("diplomacy"),), "Allies who will not come",
                   dict(focus=(0.72, 0.34), push=0.08, lift=1.20, zoom=1.6, into="cut")),
    (22, "0:39.4", 1.5, "still", (P("diplo-pending"),), None,
                   dict(focus=(0.02, 0.10), push=0.08, lift=1.50, zoom=2.0, into="cut")),
    (23, "0:40.9", 1.8, "clip",  (COLLAPSE, turn(3), 2.0, EUROPE),
                   "A war you started on turn three",
                   dict(drift=(6, 0), into="cut")),
    (24, "0:42.7", 1.5, "still", (P("diplo-acts"),), None,
                   dict(focus=(0.07, 0.68), push=0.08, lift=1.40, zoom=2.4, into="cut")),
    (25, "0:44.2", 1.8, "still", (P("topic-unrest"),), "Advice, confidently given",
                   dict(focus=(0.05, 0.95), push=0.06, lift=2.10, zoom=2.2, into="cut")),
    (26, "0:46.0", 1.6, "still", (P("economy-sectors"),), None,
                   dict(focus=(0.22, 0.38), push=0.08, lift=1.45, zoom=1.9, into="cut")),
    (27, "0:47.6", 1.8, "clip",  (COLLAPSE, turn(480), 2.0, SAMER),
                   "A country that no longer exists",
                   dict(drift=(0, 5), into="cut")),
    (28, "0:49.4", 1.5, "clip",  (COLLAPSE, turn(520), 2.5, FAREAST), None,
                   dict(drift=(-7, 0), into="cut")),

    # ---- rebuild ----------------------------------------------------------
    (29, "0:50.9", 2.0, "clip",  (COLLAPSE, turn(300), 9.0, ATLANT),
                   "It is played by", dict(into="mix")),
    (30, "0:52.9", 2.0, "clip",  (COLLAPSE, turn(430), 9.0, WORLD), None,
                   dict(into="mix")),
    (31, "0:54.9", 3.0, "still", (P("tutorial"),), "you",
                   dict(focus=(0.02, 0.86), push=0.06, lift=1.90, zoom=1.8, into="dip")),
    (32, "0:57.9", 2.6, "still", (P("multiplayer"),), "your friends",
                   dict(focus=(0.5, 0.26), lift=1.45, zoom=1.5, into="mix")),

    # ---- the fly. Three shots now, not two: it is the best thing in the
    #      trailer and it was getting ten seconds between two map sections.
    (33, "1:00.5", 5.0, "still", (P("fly-desk"),), "and a fruit fly",
                   dict(focus=(0.55, 0.42), push=0.09, into="dip")),
    (34, "1:05.5", 5.0, "freeze",(P("fly-desk"), 0),
                   "138,639 neurons.\nIt is not good at it.",
                   dict(focus=(0.30, 0.45), push=0.06, pull=True, into="mix")),
    (35, "1:10.5", 3.5, "still", (P("fly-wide"),), None,
                   dict(focus=(0.5, 0.45), push=0.07, into="mix")),

    # ---- where it runs ----------------------------------------------------
    (36, "1:14.0", 5.0, "marks", ("windows","apple","linux","browser"),
                   "Windows.  macOS.  Linux.  A browser tab.", dict(into="dip")),
    (37, "1:19.0", 2.0, "still", (P("templeos-game"),),
                   "And an operating system\none man wrote for God.",
                   dict(focus=(0.5, 0.42), into="smear")),
    (38, "1:21.0", 2.0, "still", (P("templeos-game"),), None,
                   dict(focus=(0.62, 0.50), push=0.10, into="mix")),
    (39, "1:23.0", 3.0, "logo",  "big",  "Free. All of it.", dict(into="dip")),
    (40, "1:26.0", 4.0, "logo",  "small","opendoctrines.pages.dev",
                   dict(gold_caption=True, fade_out=1.5, into="mix")),
]

# ---------------------------------------------------------------- captions

def caption_png(path, text, track=0, caps=False, gold=False, placeholder=None,
                parts=None, active=None):
    """A transparent 1920x1080 overlay.  Left-aligned, low, held still.

    PIL rather than ffmpeg's drawtext because the captions carry commas,
    apostrophes and newlines, and every one of those is a quoting bug waiting
    in a filtergraph string.
    """
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new("RGBA", (AW, AH), (0, 0, 0, 0))
    # A scrim, not a motion graphic: captions land over a lit map and the
    # economy screenshot already has pale text exactly where they sit.  Baked
    # into the overlay so no shot needs a second filter pass.
    scrim = Image.new("RGBA", (AW, AH), (0, 0, 0, 0))
    sd = ImageDraw.Draw(scrim)
    for y in range(440, AH):
        a = int(236 * ((y - 440) / (AH - 440)) ** 1.3)
        sd.line([(0, y), (AW, y)], fill=SUNKEN + (a,))
    im.alpha_composite(scrim)
    d  = ImageDraw.Draw(im)
    size = 52 if not caps else 46
    f = ImageFont.truetype(FONT, size)
    colour = GOLD if gold else TEXT
    x0, y0 = 150, 626
    if parts is not None:
        # One caption, four words, one of them lit. The platform beat names
        # four things; showing all four names while three marks are on screen
        # reads as a shot that has not finished loading.
        cx = x0
        for i, word in enumerate(parts):
            on = (i == active)
            col = (GOLD if on else TEXT) + (255 if on else 90,)
            d.text((cx, y0), word, font=f, fill=col)
            cx += d.textlength(word, font=f) + d.textlength("  ", font=f)
        im.save(path)
        return
    lines = (text or "").split("\n")
    y = y0 - (len(lines) - 1) * (size + 14)
    for line in lines:
        if track:
            cx = x0
            for ch in line:
                d.text((cx, y), ch, font=f, fill=colour + (255,))
                cx += d.textlength(ch, font=f) + track
        else:
            d.text((x0, y), line, font=f, fill=colour + (255,))
        y += size + 14
    if placeholder:
        fp = ImageFont.truetype(FONT, 26)
        d.text((150, 742), "PLACEHOLDER: " + placeholder, font=fp,
               fill=(0xC9, 0xA2, 0x27, 200))
    im.save(path)

# ---------------------------------------------------------------- ffmpeg

COMMON = ["-c:v", "libx264", "-preset", "medium", "-crf", "18",
          "-pix_fmt", "yuv420p", "-r", str(FPS), "-an"]

# The grade. The site's stylesheet calls the look "a lamplit map table", and
# raw game output is not that: it is pure black behind saturated province
# colours. So the blacks are lifted INTO the sunken blue rather than crushed,
# the highlights are pulled toward the gold, and the whole thing loses a little
# saturation -- a map table is lit by one lamp, not by a monitor.
GRADE = ("curves=all='0/0.035 0.25/0.225 0.75/0.78 1/0.96',"
         "colorbalance=rs=-0.03:bs=0.055:rh=0.045:gh=0.015:bh=-0.035,"
         "eq=saturation=0.90:contrast=1.07:gamma=0.98")
# Applied AFTER the caption, because grain sits on the print, not on the plate.
PRINT = ("pad=%d:%d:0:%d:black,vignette=PI/4.6,"
         "noise=alls=5:allf=t+u" % (W, H, BAR_Y))

def run(args):
    p = subprocess.run(["ffmpeg", "-v", "error", "-y"] + args)
    if p.returncode:
        sys.exit("ffmpeg failed: %s" % " ".join(args))

def hexrgb(c):
    return "0x%02X%02X%02X" % c

# A 2:1 timelapse into a 2.39:1 frame. Cropping 156 rows off a world map
# sounds like damage and is the opposite: the bottom eighth of every map layer
# is the Antarctic ice shelf drawn as flat grey, and it was the brightest thing
# on screen in half the shots. Bias the crop downward and it goes.
FIT_CLIP = ("scale=%d:-2:flags=lanczos,crop=%d:%d:0:40" % (AW, AW, AH))

def geo(lon, lat, span, src_w=1920):
    """A crop of the map, named in degrees instead of pixels.

    The map is equirectangular at 2:1, so a degree is the same number of pixels
    horizontally and vertically and the arithmetic is one constant. Naming a
    shot `geo(100, 40, 150)` -- centred on central Asia, 150 degrees wide --
    survives a re-render at a different resolution, which a pixel rectangle
    does not.

    These come from the 1920 master because nothing larger exists:
    `--export-timelapse` writes a CORRUPT GIF above 1920x960 -- 2560 and 3840
    both truncate mid-stream with an LZW decode error -- and it exits 0 while
    doing it. So the spans here stay wide enough (130 degrees and up) that the
    upscale is under 3x, which flat map colour survives.
    """
    ppd = src_w / 360.0
    w = span * ppd
    h = w / (AW / float(AH))
    x = (lon + 180.0) * ppd - w / 2.0
    y = (90.0 - lat) * ppd - h / 2.0
    # Keep the window on the plate.
    x = max(0.0, min(x, src_w - w))
    y = max(0.0, min(y, src_w / 2.0 - h))
    return (int(x) & ~1, int(y) & ~1, int(w) & ~1, int(h) & ~1)

def cap_in(overlay, dur):
    """Input arguments for the caption.

    It has to be LOOPED. A PNG is one frame at t=0, and `fade=t=in` evaluates
    that frame's alpha to zero -- then overlay's eof_action repeats the
    transparent frame for the whole shot and the caption never appears at all.
    The symptom is a trailer with no words on it and no error anywhere.
    """
    return ["-loop", "1", "-framerate", str(FPS), "-t", str(dur), "-i", overlay]

def tail(label, overlay, dur, fade=0.45, lift=1.0):
    """grade -> caption -> print, the same for every shot.

    `lift` is a per-shot exposure, applied BEFORE the grade, because the grade
    is one look and the plates are not one exposure: the research tree and the
    doctrine panel are dark UI on a dark map, and a curve that lifts the blacks
    of a lit world map pushes those two further down. Grading shot by shot is
    what grading is.

    The caption fades rather than appears. The plan says the type does not
    MOVE, which a fade is not; type that cuts in reads as a slide, and this is
    the one change that makes the montage feel shot rather than assembled."""
    # ffmpeg's eq takes HIGHER gamma as BRIGHTER. Writing 1/lift here darkened
    # the two plates this exists for, which looked exactly like the grade being
    # too strong and sent me back to the grade twice.
    # GAMMA ONLY. A flat brightness offset lifts the toe as well as the
    # midtones, and the result is milky: the grade's whole point is a deep
    # blue-black, and an offset trades it away to make a dark panel readable.
    pre = "" if lift == 1.0 else "eq=gamma=%g," % lift
    fg = "[%s]%s%s[g];" % (label, pre, GRADE)
    if overlay:
        fg += ("[cap]fade=t=in:st=0:d=%g:alpha=1,"
               "fade=t=out:st=%g:d=%g:alpha=1[cf];"
               "[g][cf]overlay=0:0[c];" % (fade, max(0.0, dur - fade), fade))
    else:
        fg += "[g]null[c];"
    return fg + "[c]%s[o]" % PRINT

def shot_clip(out, src, start, speed, dur, overlay, fade_in, region=None,
              drift=(0.0, 0.0), lift=1.0):
    if region:
        x, y, w, h = region
        dx, dy = drift
        # The window creeps while the shot plays: a map close-up with a static
        # frame reads as a screenshot of a map, which is the thing this edit
        # has too much of already.
        xe = "%d+(%g)*t" % (x, dx)
        ye = "%d+(%g)*t" % (y, dy)
        vf = ["crop=%d:%d:'%s':'%s'" % (w, h, xe, ye),
              "scale=%d:%d:flags=lanczos" % (AW, AH)]
    else:
        vf = [FIT_CLIP]
    if speed != 1.0:
        vf.append("setpts=PTS/%g" % speed)
    vf.append("fps=%d" % FPS)
    ins = ["-ss", str(start), "-t", str(dur * speed + 1), "-i", src]
    fg = "[0:v]" + ",".join(vf) + "[pic];"
    if overlay:
        ins += cap_in(overlay, dur)
        fg += "[1:v]null[cap];"
    fg += tail("pic", overlay, dur, lift=lift)
    if fade_in:
        fg = fg.replace("[o]", "[pre]") + ";[pre]fade=t=in:st=0:d=%g[o]" % fade_in
    run(ins + ["-filter_complex", fg, "-map", "[o]", "-t", str(dur)]
        + COMMON + [out])

def shot_freeze(out, src, start, dur, overlay, region=None, **kb):
    frame = out + ".png"
    vf = []
    if region:
        x, y, w, h = region
        vf = ["-vf", "crop=%d:%d:%d:%d,scale=%d:%d:flags=lanczos"
              % (w, h, x, y, AW, AH)]
    run(["-ss", str(start), "-i", src, "-vframes", "1"] + vf + [frame])
    _still(out, [frame], dur, overlay, **kb)
    os.remove(frame)

def _still(out, images, dur, overlay, push=0.06, focus=(0.5, 0.5), pull=False,
           lift=1.0, zoom=1.0):
    """A dolly, not a zoom.

    Two changes make a still read as a camera. The move is EASED -- a raised
    cosine, so it accelerates away from rest and settles into it, where a
    linear ramp starts and stops dead and reads as a crop animating. And the
    frame COVERS the 2.39 picture around a focus point rather than fitting
    inside it, so a 16:9 screenshot is pushed into rather than shown whole;
    the plan's own 0:08 beat asks for exactly that.

    `pull` runs the move backwards: a slow pull-out, which is what the held
    beats want so they do not all drift the same direction.
    """
    per = dur / len(images)
    fx, fy = focus
    parts = []
    for i, img in enumerate(images):
        seg = "%s.p%d.mp4" % (out, i)
        n = max(2, int(round(per * FPS)))
        z0, z1 = (1 + push, 1.0) if pull else (1.0, 1 + push)
        # Raised-cosine ease between z0 and z1 over the shot.
        zexpr = ("%g+(%g)*(1-cos(PI*min(on/%d,1)))/2" % (z0, z1 - z0, n))
        vf = ("scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,"
              "crop=%d:%d:(iw-ow)*%g:(ih-oh)*%g,"
              "zoompan=z='%s':x='(iw-iw/zoom)*%g':y='(ih-ih/zoom)*%g':"
              "d=1:s=%dx%d:fps=%d"
              % (int(AW * 2 * zoom), int(AH * 2 * zoom), AW * 2, AH * 2, fx, fy,
                 zexpr, fx, fy, AW, AH, FPS))
        run(["-loop", "1", "-framerate", str(FPS), "-t", str(per), "-i", img,
             "-vf", vf, "-t", str(per)] + COMMON + [seg])
        parts.append(seg)
    if len(parts) == 1:
        joined = parts[0]
    else:
        joined = out + ".join.mp4"
        lst = out + ".txt"
        open(lst, "w").write("".join("file '%s'\n" % p for p in parts))
        run(["-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", joined])
        os.remove(lst)
    ins = ["-i", joined]
    fg = "[0:v]null[pic];"
    if overlay:
        ins += cap_in(overlay, dur)
        fg += "[1:v]null[cap];"
    fg += tail("pic", overlay, dur, lift=lift)
    run(ins + ["-filter_complex", fg, "-map", "[o]"] + COMMON + [out + ".t.mp4"])
    shutil.move(out + ".t.mp4", out)
    for p in parts + [joined]:
        if os.path.exists(p) and p != out:
            os.remove(p)

def shot_card(out, dur, overlay, lift=1.0):
    ins = ["-f", "lavfi", "-i",
           "color=c=%s:s=%dx%d:r=%d" % (hexrgb(SUNKEN), AW, AH, FPS)]
    fg = "[0:v]null[pic];"
    if overlay:
        ins += cap_in(overlay, dur)
        fg += "[1:v]null[cap];"
    fg += tail("pic", overlay, dur, lift=lift)
    run(ins + ["-filter_complex", fg, "-map", "[o]", "-t", str(dur)]
        + COMMON + [out])

def shot_marks(out, names, dur, overlay, words=None, lift=1.0):
    """One mark at a time, each centred and large, with its word lit.

    The first version composited the marks left to right into a row that was
    pre-centred for four of them, so for three quarters of the shot the group
    sat off to one side with dead space beside it and the caption named a
    platform that was not on screen yet. Four centred beats also give this
    stretch something to do: it was the flattest five seconds in the cut.
    """
    from PIL import Image
    words = words or ["Windows.", "macOS.", "Linux.", "A browser tab."]
    per = dur / len(names)
    side = 300
    parts = []
    for i, n in enumerate(names):
        base = Image.new("RGBA", (AW, AH), SUNKEN + (255,))
        m = Image.open(os.path.join(ASSET, "mark-%s.png" % n)).convert("RGBA")
        m = m.resize((side, side), Image.LANCZOS)
        base.alpha_composite(m, ((AW - side) // 2, 180))
        img = "%s.m%d.png" % (out, i)
        base.convert("RGB").save(img)
        cap = "%s.c%d.png" % (out, i)
        caption_png(cap, None, parts=words, active=i)
        seg = "%s.s%d.mp4" % (out, i)
        fg = "[0:v]fade=t=in:st=0:d=0.22[pic];[1:v]null[cap];" + \
             tail("pic", cap, per, fade=0.18, lift=lift)
        run(["-loop", "1", "-framerate", str(FPS), "-t", str(per), "-i", img]
            + cap_in(cap, per)
            + ["-filter_complex", fg, "-map", "[o]", "-t", str(per)]
            + COMMON + [seg])
        parts.append(seg)
        os.remove(img); os.remove(cap)
    lst = out + ".txt"
    open(lst, "w").write("".join("file '%s'\n" % p for p in parts))
    run(["-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", out])
    for p in parts + [lst]:
        os.remove(p)

def shot_logo(out, which, dur, overlay):
    """big = the name, in full, because this is where somebody decides whether
    they can find it again.  small = the monogram, by which point they have
    read the name and the URL is the only thing that still has to land."""
    from PIL import Image
    base = Image.new("RGBA", (AW, AH), SUNKEN + (255,))
    if which == "big":
        wm = Image.open(os.path.join(ASSET, "wordmark-gold.png")).convert("RGBA")
        width, top = 1500, 300
    else:
        wm = Image.open(os.path.join(ASSET, "mark-od-gold.png")).convert("RGBA")
        width, top = 420, 250
        wm.putalpha(wm.getchannel("A").point(lambda a: int(a * 0.6)))
    wm = wm.resize((width, int(wm.height * width / wm.width)), Image.LANCZOS)
    base.alpha_composite(wm, ((AW - wm.width) // 2, top))
    f = out + ".png"
    base.convert("RGB").save(f)
    # A card does not move, but it does breathe: a 1.5% pull is under the
    # threshold of looking like a zoom and over the threshold of looking dead.
    _still(out, [f], dur, overlay, push=0.015, pull=True)
    os.remove(f)

# ---------------------------------------------------------------- driver

# How each shot ARRIVES. The default used to be the only option, and 31
# straight cuts in a row reads as a slideshow rather than an edit.
#
#   cut      a true cut (two frames of overlap, invisible)
#   mix      a dissolve -- for two shots of the same subject or the same place
#   dip      through black -- for a section break, where a dissolve would lie
#            about the two sides being continuous
#   smear    a dissolve through blur -- motion between two unrelated places
#
# Dip is the expensive one and it is spent where the trailer turns: into the
# full stop, into the fly, and into the wordmark.
XFADE = {"cut": ("fade", 0.07), "mix": ("fade", 0.40),
         "dip": ("fadeblack", 0.55), "smear": ("smoothleft", 0.45)}

def build(only=None):
    os.makedirs(WORK, exist_ok=True)
    # Shots are rendered LONGER than their slot by the widest transition, so a
    # dissolve has material to cross over instead of running out and freezing.
    pad_t = max(d for _, d in XFADE.values())
    segs, starts, t = [], [], 0.0
    for row in SHOTS:
        starts.append(t)
        t += row[2]
    total = t

    for i, (n, start, dur, kind, src, cap, opt) in enumerate(SHOTS):
        out = os.path.join(WORK, "shot%02d.mp4" % n)
        segs.append(out)
        if only and n not in only:
            if not os.path.exists(out):
                sys.exit("shot %d not built yet and not selected" % n)
            continue
        ov = None
        if cap or opt.get("placeholder"):
            ov = os.path.join(WORK, "cap%02d.png" % n)
            caption_png(ov, cap, track=opt.get("track", 0), caps=opt.get("caps", False),
                        gold=opt.get("gold_caption", False),
                        placeholder=opt.get("placeholder"))
        d = dur + pad_t
        kb = dict(push=opt.get("push", 0.06),
                  focus=opt.get("focus", (0.5, 0.5)),
                  pull=opt.get("pull", False),
                  lift=opt.get("lift", 1.0),
                  zoom=opt.get("zoom", 1.0))
        if kind == "clip":
            reg = geo(*src[3]) if len(src) > 3 and src[3] else None
            shot_clip(out, src[0], src[1], src[2], d, ov, opt.get("fade_in"),
                      region=reg, drift=opt.get("drift", (0.0, 0.0)),
                      lift=opt.get("lift", 1.0))
        elif kind == "still":
            paths = [p if os.path.isabs(p) else os.path.join(IMG, p) for p in src]
            _still(out, paths, d, ov, **kb)
        elif kind == "freeze":
            if src[0].endswith(".png"):
                _still(out, [src[0]], d, ov, **kb)
            else:
                reg = geo(*src[2]) if len(src) > 2 and src[2] else None
                shot_freeze(out, src[0], src[1], d, ov, region=reg, **kb)
        elif kind == "card":
            shot_card(out, d, ov, lift=opt.get("lift", 1.0))
        elif kind == "marks":
            shot_marks(out, src, d, ov)
        elif kind == "logo":
            shot_logo(out, src, d, ov)
        if opt.get("fade_out"):
            tmp = out + ".fade.mp4"
            run(["-i", out, "-vf", "fade=t=out:st=%g:d=%g"
                 % (dur - opt["fade_out"], opt["fade_out"])] + COMMON + [tmp])
            shutil.move(tmp, out)
        print("shot %02d  %s  %ss  %-6s %s" % (n, start, dur, kind,
                                               opt.get("into", "cut")))

    # One xfade chain. Each transition is CENTRED on the incoming shot's slot
    # start, so the music cues the cut was built around do not move: the
    # outgoing shot gives up half the transition and the incoming shot the
    # other half, which is what an editor means by a dissolve ON the beat.
    ins, fg, prev = [], "", None
    for i, out in enumerate(segs):
        ins += ["-i", out]
        if i == 0:
            fg += "[0:v]setpts=PSTART+PTS-STARTPTS[v0];"
            prev = "v0"
            continue
        kindname = SHOTS[i][6].get("into", "cut")
        tr, dt = XFADE[kindname]
        off = max(0.0, starts[i] - dt / 2.0)
        lab = "x%d" % i
        fg += ("[%d:v]setpts=PSTART+PTS-STARTPTS[s%d];"
               "[%s][s%d]xfade=transition=%s:duration=%g:offset=%g[%s];"
               % (i, i, prev, i, tr, dt, off, lab))
        prev = lab
    fg = fg.replace("PSTART+PTS-STARTPTS", "PTS-STARTPTS").rstrip(";")
    silent = os.path.join(WORK, "trailer-silent.mp4")
    run(ins + ["-filter_complex", fg, "-map", "[%s]" % prev,
               "-t", str(total + pad_t / 2.0)] + COMMON + [silent])

    final = os.path.join(WORK, "trailer.mp4")
    score = os.path.join(ASSET, "score.wav")
    if os.path.exists(score):
        run(["-i", silent, "-i", score, "-c:v", "copy", "-c:a", "aac",
             "-b:a", "192k", "-shortest", "-movflags", "+faststart", final])
    else:
        shutil.copy(silent, final)
        print("no %s -- run tools/trailer_music.py" % score)
    print("\n-> %s  (%d shots, %.1f s)" % (final, len(segs), total))
    return final

if __name__ == "__main__":
    build(set(int(a) for a in sys.argv[1:]) or None)
