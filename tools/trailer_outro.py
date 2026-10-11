#!/usr/bin/env python3
"""The animated outro: the wordmark assembles itself out of provinces.

    python3 tools/trailer_outro.py                       # 9 s, 1920x1080
    python3 tools/trailer_outro.py --seconds 6.06 --height 804 --out X.mp4
    python3 tools/trailer_outro.py --alpha                # transparent, for NLEs

WHAT IT IS. A branded sting to end a video on -- the shape a YouTube channel
outro takes. The brief named Minecraft's, where the logo assembles out of
blocks, and the translation to this game is obvious once stated: Open
Doctrines' blocks are PROVINCES. So the letterforms are built from map tiles in
the game's own colours, they fly in from outside the frame, and the map keeps
changing behind them the whole time.

HOW THE TILES ARE FOUND. The wordmark is not drawn here and not re-typeset;
`docs/trailer/assets/wordmark-gold.png` is sampled on a grid and every cell
whose alpha is mostly covered becomes one tile. So the outro cannot drift from
the logo -- regenerate the logo and the outro follows, because the glyphs ARE
the source of the tile positions.

THE COLOURS ARE THE GAME'S. Sampled off the political map plate rather than
picked: saturated pixels, quantised, most common first. A tile arrives in one
of those, then a gold sweep crosses the wordmark left to right and converts
them, which is the beat that turns "a pile of map" into "the logo".

WHY THE TILES ARE POLYGONS AND NOT ROTATED IMAGES. Every tile spins as it
flies. Pasting a rotated sprite per tile per frame is thousands of resamples a
second; four corners through a rotation matrix into `draw.polygon` is the same
picture for a fraction of the cost, and it is what makes this render in under a
minute instead of over ten.

The background is any video -- by default the political timelapse, so the thing
behind the logo is a real game being played. With `--alpha` it is nothing, and
the output is a transparent .mov to drop over footage in an editor.
"""
import argparse, math, os, random, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET = os.path.join(ROOT, "docs/trailer/assets")
WORDMARK = os.path.join(ASSET, "wordmark-gold.png")
FPS = 30
GOLD = (0xC9, 0xA2, 0x27)
SUNKEN = (0x09, 0x0B, 0x11)

# Sampled from docs/trailer/assets/plates/world-map.png -- see the docstring.
PALETTE = [(132, 60, 60), (132, 36, 36), (60, 84, 132), (156, 84, 60),
           (180, 84, 60), (156, 60, 60), (180, 60, 60), (132, 108, 60),
           (84, 132, 60), (180, 84, 84), (60, 108, 156), (156, 132, 60),
           (60, 108, 180), (204, 84, 84)]

def ease_out_back(t, overshoot=1.34):
    """Lands past the mark and comes back. A tile that eases to a dead stop
    reads as a crop animating; the overshoot is what makes it read as weight."""
    c1 = overshoot
    c3 = c1 + 1.0
    return 1.0 + c3 * (t - 1.0) ** 3 + c1 * (t - 1.0) ** 2

def ease_in_out(t):
    return 0.5 - 0.5 * math.cos(math.pi * max(0.0, min(1.0, t)))

def smoothstep(a, b, x):
    if b <= a:
        return 1.0 if x >= b else 0.0
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)

# ------------------------------------------------------------------- tiles

def build_tiles(W, H, cell, mark_frac, rng):
    """One tile per covered cell of the wordmark's alpha channel."""
    from PIL import Image
    wm = Image.open(WORDMARK).convert("RGBA").getchannel("A")
    tw = int(W * mark_frac)
    th = max(1, int(wm.height * tw / wm.width))
    wm = wm.resize((tw, th), Image.LANCZOS)
    px = wm.load()
    ox, oy = (W - tw) // 2, int(H * 0.44) - th // 2
    tiles = []
    for gy in range(0, th, cell):
        for gx in range(0, tw, cell):
            # Average coverage of the cell, so edges of glyphs do not grow a
            # fringe of tiles that are mostly background.
            tot = n = 0
            for y in range(gy, min(gy + cell, th)):
                for x in range(gx, min(gx + cell, tw)):
                    tot += px[x, y]
                    n += 1
            if n and tot / n > 140:
                tiles.append({
                    "tx": ox + gx + cell / 2.0,
                    "ty": oy + gy + cell / 2.0,
                    "col": PALETTE[rng.randrange(len(PALETTE))],
                    "spin": rng.uniform(-3.4, 3.4),
                    "delay": rng.uniform(0.0, 0.30),
                })
    return tiles, (ox, oy, tw, th)

def origin_for(t, W, H, rng):
    """Off-frame, on a ring, biased to the side the tile belongs on -- so the
    wordmark converges inward instead of every tile crossing every other."""
    ang = math.atan2(t["ty"] - H * 0.44, t["tx"] - W * 0.5)
    ang += rng.uniform(-0.65, 0.65)
    r = max(W, H) * rng.uniform(0.75, 1.25)
    return W * 0.5 + math.cos(ang) * r, H * 0.44 + math.sin(ang) * r

# ------------------------------------------------------------------- render

def render_frames(outdir, W, H, seconds, url, cell, mark_frac, seed, gutter=2,
                  land=None, url_at=None, raw=False):
    from PIL import Image, ImageDraw, ImageFont
    rng = random.Random(seed)
    tiles, (ox, oy, tw, th) = build_tiles(W, H, cell, mark_frac, rng)
    for t in tiles:
        t["ox"], t["oy"] = origin_for(t, W, H, rng)
    n = int(round(seconds * FPS))
    os.makedirs(outdir, exist_ok=True)

    # `land` is when the last tile arrives, in seconds. For the trailer that is
    # the march's cadence; for the standalone it defaults to just under half.
    land = land if land is not None else seconds * 0.46
    url_at = url_at if url_at is not None else land + 1.3
    FLY_IN = 0.04
    FLY_OUT = land / seconds
    SWEEP_A = max(0.0, (land - 0.4) / seconds)   # gold crosses as they settle
    SWEEP_B = min(1.0, (land + 1.4) / seconds)
    URL_A = url_at / seconds
    URL_B = min(1.0, (url_at + 0.9) / seconds)
    # raw: the trailer applies its own vignette, grain and fades, so the clip
    # must not arrive with a second set baked in.
    FADE = 2.0 if raw else 0.90
    font = ImageFont.truetype(os.path.join(ROOT, "data/fonts/DejaVuSans.ttf"),
                              max(13, int(H * 0.030)))
    # 2h+1 is what a polygon from -h to +h actually covers, so solve for the
    # width we want rather than halving the cell and overlapping the neighbour.
    half = max(1.0, (cell - gutter - 1) * 0.5)
    for f in range(n):
        p = f / max(n - 1, 1)
        im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        out = smoothstep(FADE, 1.0, p)           # 0 -> 1 as it leaves
        for t in tiles:
            a0 = FLY_IN + t["delay"] * (FLY_OUT - FLY_IN)
            k = (p - a0) / max(FLY_OUT - a0, 1e-6)
            if k <= 0:
                continue
            e = ease_out_back(min(k, 1.0)) if k < 1.0 else 1.0
            cx = t["ox"] + (t["tx"] - t["ox"]) * e
            cy = t["oy"] + (t["ty"] - t["oy"]) * e
            ang = t["spin"] * (1.0 - e)
            size = half * (1.0 + 1.5 * (1.0 - e))
            # The sweep converts map colour to gold, left to right.
            s = smoothstep(SWEEP_A, SWEEP_B,
                           p - 0.16 * ((t["tx"] - ox) / max(tw, 1)))
            col = tuple(int(c + (g - c) * s) for c, g in zip(t["col"], GOLD))
            alpha = int(255 * min(1.0, k * 3.0) * (1.0 - out))
            if alpha <= 2:
                continue
            ca, sa = math.cos(ang), math.sin(ang)
            pts = [(cx + (dx * ca - dy * sa), cy + (dx * sa + dy * ca))
                   for dx, dy in ((-size, -size), (size, -size),
                                  (size, size), (-size, size))]
            d.polygon(pts, fill=col + (alpha,))
        if url:
            ua = smoothstep(URL_A, URL_B, p) * (1.0 - out)
            if ua > 0.01:
                w = d.textlength(url, font=font)
                d.text(((W - w) / 2, oy + th + H * 0.055), url, font=font,
                       fill=(0xE8, 0xE4, 0xDA, int(235 * ua)))
        im.save(os.path.join(outdir, "f%04d.png" % f))
    return n, len(tiles)

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(ASSET, "outro.mp4"))
    ap.add_argument("--seconds", type=float, default=9.0)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--cell", type=int, default=0, help="tile size; 0 = auto")
    ap.add_argument("--mark", type=float, default=0.80, help="wordmark width")
    ap.add_argument("--gutter", type=int, default=2,
                    help="pixels of gap between tiles; 0 makes them fuse")
    ap.add_argument("--url", default="opendoctrines.pages.dev")
    ap.add_argument("--bg", default=os.path.join(
        os.environ.get("TRAILER_WORK", "/tmp/trailer-march"),
        "tl", "political-fast.mp4"))
    ap.add_argument("--alpha", action="store_true",
                    help="no background; write a transparent .mov")
    ap.add_argument("--seed", type=int, default=1863)
    ap.add_argument("--land", type=float, default=None,
                    help="seconds at which the last tile lands (sync point)")
    ap.add_argument("--url-at", dest="url_at", type=float, default=None,
                    help="seconds at which the URL appears")
    ap.add_argument("--raw", action="store_true",
                    help="no vignette/grain/fades -- for use inside the cut")
    a = ap.parse_args()

    cell = a.cell or max(7, int(a.width / 190))
    work = os.path.join(os.environ.get("TRAILER_WORK", "/tmp/trailer-march"),
                        "outro-frames")
    shutil.rmtree(work, ignore_errors=True)
    n, ntiles = render_frames(work, a.width, a.height, a.seconds, a.url,
                              cell, a.mark, a.seed, a.gutter,
                              a.land, a.url_at, a.raw)
    print("%d frames, %d tiles (%dpx cells)" % (n, ntiles, cell))

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    seq = os.path.join(work, "f%04d.png")
    if a.alpha:
        out = os.path.splitext(a.out)[0] + ".mov"
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-framerate", str(FPS),
                        "-i", seq, "-c:v", "qtrle", "-pix_fmt", "argb", out],
                       check=True)
    else:
        out = a.out
        if not os.path.exists(a.bg):
            sys.exit("no background video: %s\n"
                     "generate one with tools/trailer_timelapse.sh, "
                     "or pass --alpha" % a.bg)
        # The map is pushed down, desaturated AND BLURRED. The blur is not a
        # style choice: the timelapse draws armies and cities as small white
        # squares, which at this size are indistinguishable from the tiles
        # still flying in, so a sharp background reads as tiles that never
        # landed. Three pixels of blur separates ground from figure and the
        # confusion goes.
        bg = ("scale=%d:%d:force_original_aspect_ratio=increase,"
              "crop=%d:%d,gblur=sigma=3.2,"
              "eq=brightness=-0.26:saturation=0.42:contrast=0.92,"
              "colorbalance=bs=0.06:rh=0.03" % (a.width, a.height,
                                                a.width, a.height))
        subprocess.run([
            "ffmpeg", "-y", "-v", "error",
            "-stream_loop", "-1", "-i", a.bg,
            "-framerate", str(FPS), "-i", seq,
            "-filter_complex",
            "[0:v]%s,trim=duration=%g,setpts=PTS-STARTPTS[bg];"
            "[1:v]null[fg];[bg][fg]overlay=0:0:format=auto%s[v]"
            % (bg, a.seconds, "" if a.raw else
               ",fade=t=in:st=0:d=0.35,fade=t=out:st=%g:d=0.6,"
               "vignette=PI/4.6,noise=alls=5:allf=t+u"
               % max(0.0, a.seconds - 0.6)),
            "-map", "[v]", "-c:v", "libx264", "-preset", "medium",
            "-crf", "17", "-pix_fmt", "yuv420p", "-r", str(FPS),
            "-t", str(a.seconds), "-movflags", "+faststart", out], check=True)
    shutil.rmtree(work, ignore_errors=True)
    print("-> %s  (%.2f s, %dx%d)" % (out, a.seconds, a.width, a.height))

if __name__ == "__main__":
    main()
