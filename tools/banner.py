#!/usr/bin/env python3
"""Build wide banner art from the game's own map rasters.

    python3 tools/banner.py                      # every size, 1914, Europe
    python3 tools/banner.py --scenario 1939
    python3 tools/banner.py --size steam-hero --region atlantic
    python3 tools/banner.py --size steam-vertical --region atlas \
            --out-dir docs/steam
    python3 tools/banner.py --list               # what the presets are

Writes <out-dir>/banner-<size>.png and banner-<size>-plain.png (the same
picture with no wordmark, for typesetting over by hand). The default out-dir is
docs/itch; the Steam capsules want docs/steam, and --list says which size is
which store's.

WHY THE MAP COMES OUT OF AN .ODMAP AND NOT A SCREENSHOT

tools/itch-cover.py builds the 630x500 cover from docs/img/world-map.png, which
is a 1600x900 screenshot: fine at cover size, and about a third of the pixels a
1920-wide banner needs. A scenario's political raster is **8192x4096** -- the
generator's own picture, no UI, no camera, every border exactly as the game
draws it. Cropping that and coming DOWN to size is sharp where upscaling a
screenshot is soft.

The raster is redrawn from the package rather than read out of it, because a
package does not store one: it is a pure function of province ownership, the
game rebuilds it at load, and carrying it cost 4.7 MB of a 6 MB map. See
odmap_pack.political_layer, which is what this calls.

It also means a banner can be made for any scenario, including one somebody
built in the map editor, rather than only for whatever the last screenshot run
happened to be looking at.

WHY THE WORDMARK IS STILL LIFTED FROM A SCREENSHOT

Same reason itch-cover.py does it, and the code is imported from there rather
than copied: the title is drawn in raylib's built-in font, which is not a TTF
anything else can load, so setting it in a lookalike would be visibly not the
game to anyone who has seen one screenshot.
"""

import argparse
import io
import os
import sys
import zipfile

try:
    from PIL import Image, ImageChops, ImageEnhance, ImageFilter
except ImportError:
    sys.exit("Pillow is needed: python3 -m pip install Pillow")


def _wordmark_fn():
    """itch-cover.py's wordmark(), loaded by path.

    Imported rather than copied so there is one definition of "what the title
    looks like", and by path because the file it lives in has a hyphen in its
    name and cannot be imported the ordinary way.
    """
    import importlib.util
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "itch-cover.py")
    spec = importlib.util.spec_from_file_location("itch_cover", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.wordmark

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS = os.path.join(ROOT, "data", "STDmaps")
OUT = os.path.join(ROOT, "docs", "itch")

# Where each banner is going, and what it is called there. Named after the
# destination rather than the number, because "1920x620" tells you nothing about
# which crop is safe: a store hero is cropped at the edges by the page, a social
# card never is.
SIZES = {
    "wide":          (1920, 620),   # a wide store/page header
    "github-social": (1280, 640),   # GitHub repo social preview (Settings > General)
    "x-header":      (1500, 500),   # X / Twitter profile header
    "itch-wide":     (1600, 500),   # itch page banner, if the theme takes one
    "cover":         (630, 500),    # itch cover image -- the one slot that is
                                    # certain to animate, and the one everybody
                                    # sees before they see the page at all

    # Steam's capsules. Every one of these is REQUIRED before a store page can
    # be submitted, at exactly these pixel dimensions -- Steam rejects the
    # upload rather than scaling, and there is no "close enough".
    #
    # They are not five crops of one picture. Two are portrait, two are
    # thumbnails small enough that a world map in them is a blue-grey smudge,
    # and the hero is nearly four thousand pixels wide. Use --region to pick a
    # crop per capsule; --region atlas is the one that suits the portrait pair.
    #
    # Write them to docs/steam, not docs/itch: --out-dir docs/steam.
    "steam-header":   (460, 215),   # the one in search, library and wishlists
    "steam-small":    (231, 87),    # search results. Tiny -- prefer the
                                    # wordmark reading over the map reading
    "steam-main":     (616, 353),   # front page and "more like this" strips
    "steam-vertical": (374, 448),   # portrait, used in the front-page carousel
    "steam-library":  (600, 900),   # portrait, the player's own library grid
    "steam-hero":     (3840, 1240), # the banner across a library page
    "steam-page-bg":  (1438, 810),  # behind the store page itself
}

# Steam also wants a 1280x720 "library logo": the wordmark alone, TRANSPARENT,
# no map behind it. That is not a crop of anything and so is not a size here --
# tools/itch-cover.py already extracts the wordmark from the game's own menu,
# and docs/steam/README.md says what to do with it.

# Equirectangular, in 8192x4096 pixels: left, top, right, bottom. Hand-picked,
# because the middle of a world map is the Pacific and an automatic centre crop
# gives you an empty blue rectangle.
REGIONS = {
    "europe":   (3250, 620, 6350, 1620),    # Europe, the Med and the Near East
    "atlantic": (2100, 500, 5900, 1730),    # the Americas facing the Old World
    "eurasia":  (3600, 500, 8000, 1920),    # Berlin to Beijing
    "world":    (300, 480, 8000, 2960),     # everything except the poles
    # TALLER THAN IT IS WIDE, which none of the above are. Steam's vertical and
    # library capsules are portrait, and crop_to_aspect fits a landscape region
    # to them by throwing away the outer columns -- from `europe` that leaves a
    # narrow sliver of the Baltic.
    #
    # ITS OWN ASPECT IS ALREADY NEARLY 374x448, which matters more than being
    # tall. crop_to_aspect trims from the CENTRE, so a region far taller than
    # the capsule loses its top and bottom: Scandinavia-to-the-Cape came out as
    # Sahara-to-Zambia, a portrait of the game with Europe cut off. This is
    # sized so the trim is a few pixels and what is in the box is what ships.
    "atlas":    (3641, 500, 5120, 2300),    # North Cape to the Gulf of Guinea
}

DEFAULT_SCENARIO = "map"        # Modern Day; the scenarios are named by year

# The itch page's background, from docs/itch/README.md's theme table (BG,
# #050813, itself sampled from the game's menu). A banner that ends on its own
# rectangle looks pasted onto the page; one that fades into THIS colour reads as
# part of it, which is the whole point of --merge.
PAGE_BG = (5, 8, 19)


# The menu's own two colours, from initMenuBackground() in Game_Menus.cpp.
# Not "a dark blue and a grey" -- these exact ones, so a page background and the
# game's own menu are the same picture.
MENU_SEA = (8, 14, 32)
MENU_LAND = (55, 58, 65)


def menu_silhouette(scenario: str, size):
    """The menu's background: land as a flat grey silhouette on a flat sea.

    The main menu does NOT show the political map. initMenuBackground() reads
    land_sea.png, calls every pixel with red > 128 land, and paints two colours
    -- which is why the menu reads as a quiet grey world and the political
    rasters read as a wall of colour. A page that wants to look like the menu
    wants this, not political.png.

    Returns (image, land_mask) -- the mask being the same thing the menu keeps
    as m_menuBgLandCoords, so shells can be put where the land is.
    """
    try:
        import numpy as np
    except ImportError:
        sys.exit("numpy is needed for --page-bg: python3 -m pip install numpy")

    path = os.path.join(MAPS, f"{scenario}.odmap")
    with zipfile.ZipFile(path) as z:
        ls = Image.open(io.BytesIO(z.read("land_sea.png"))).convert("RGB")
    ls = ls.resize(size, Image.LANCZOS)

    land = np.asarray(ls, dtype=np.int16)[:, :, 0] > 128
    out = np.empty((size[1], size[0], 3), dtype=np.uint8)
    out[...] = MENU_SEA
    out[land] = MENU_LAND
    return Image.fromarray(out, "RGB"), land


def political(scenario: str) -> Image.Image:
    """The scenario's political raster.

    Through odmap_pack rather than straight out of the zip: a package no
    longer stores this layer (it is redrawn from province ownership at load,
    which is where four fifths of a map's size went), so it is rebuilt here
    the same way the game rebuilds it.
    """
    path = os.path.join(MAPS, f"{scenario}.odmap")
    if not os.path.exists(path):
        have = sorted(f[:-6] for f in os.listdir(MAPS) if f.endswith(".odmap"))
        sys.exit(f"no such scenario: {scenario}\navailable: {', '.join(have)}")
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from odmap_pack import political_layer
    img = political_layer(path)
    # Only the maps that are missing them; a scenario raster already has its
    # own, drawn from real province ids, and re-darkening would double them.
    return img if has_outlines(img) else outline_countries(img)


def outline_countries(img: Image.Image, threshold: int = 90) -> Image.Image:
    """Draw the 1px dark country border the base map's raster is missing.

    THE MODERN DAY MAP HAS NO OUTLINES AND EVERY HISTORICAL ONE DOES, which is
    visible the moment they are cut together: the eras dissolve from bordered
    maps into a flat wash of colour.

    It is not the scenarios that are unusual. tools/generate_scenario.py builds
    their political.png in Python and finishes with a 1px pass at a third
    brightness (`out[edge] = out[edge] // 3`), reproducing what the game draws
    at load. data/map.odmap's raster comes from MapGenerator instead
    (Generator.cpp, "Computing political map with border gradient"), which does
    the distance gradient and then writes the image with no such pass. The fix
    belongs there -- this is the same darkening applied after the fact, so the
    banner does not have to wait for a full pipeline run.

    Country identity is read from the colours themselves rather than from
    provinces.json: inside a country the gradient shifts a few levels per pixel,
    while a border is a jump to another country's colour entirely, so a
    threshold separates them without needing the province ids at all.
    """
    try:
        import numpy as np
    except ImportError:
        return img          # numpy is optional; without it, no outlines

    a = np.asarray(img.convert("RGB"), dtype=np.int16)
    s = a.sum(axis=2)
    edge = np.zeros(s.shape, dtype=bool)
    d = np.abs(np.diff(s, axis=1)) > threshold          # vertical seams
    edge[:, :-1] |= d
    edge[:, 1:] |= d
    d = np.abs(np.diff(s, axis=0)) > threshold          # horizontal seams
    edge[:-1, :] |= d
    edge[1:, :] |= d

    # Sea is already dark and already has a coastline; darkening it further only
    # muddies the water. The sea in these rasters is blue-dominant and dim.
    sea = (s < 120) & (a[:, :, 2] >= a[:, :, 0])
    edge &= ~sea

    out = a.copy()
    out[edge] = out[edge] // 3
    return Image.fromarray(out.astype("uint8"), "RGB")


def has_outlines(img: Image.Image) -> bool:
    """Whether this raster already carries dark country borders."""
    try:
        import numpy as np
    except ImportError:
        return True
    a = np.asarray(img.convert("RGB"), dtype=np.int16)
    return bool((a.sum(axis=2) < 60).mean() > 0.02)


def crop_to_aspect(src: Image.Image, region, size) -> Image.Image:
    """Crop `region`, then trim it to the target aspect from the centre.

    Trimming rather than squashing: the map is equirectangular and a stretched
    coastline is the one thing every player of a map game notices immediately.
    """
    left, top, right, bottom = region
    box = src.crop((left, top, right, bottom))
    want = size[0] / size[1]
    have = box.width / box.height
    if have > want:                       # too wide -> take the middle columns
        w = int(box.height * want)
        x = (box.width - w) // 2
        box = box.crop((x, 0, x + w, box.height))
    elif have < want:                     # too tall -> take the middle rows
        h = int(box.width / want)
        y = (box.height - h) // 2
        box = box.crop((0, y, box.width, y + h))
    return box.resize(size, Image.LANCZOS)


def darken_for_text(img: Image.Image, side_frac: float) -> Image.Image:
    """Sink the left edge into shadow so a wordmark can sit on it.

    A flat black bar reads as a UI element that failed to load (the same note is
    on itch-cover.py's title band), so this is a gradient that reaches zero
    before the middle of the picture and never has an edge of its own.
    """
    grad = Image.new("L", (img.width, 1), 0)
    px = grad.load()
    span = max(1, int(img.width * side_frac))
    for x in range(img.width):
        t = max(0.0, 1.0 - x / span)
        px[x, 0] = int(232 * (t ** 1.35))
    grad = grad.resize(img.size)
    shadow = Image.new("RGB", img.size, (3, 5, 12))
    return Image.composite(shadow, img, grad).convert("RGB")


def merge_edges(img: Image.Image, sides: bool) -> Image.Image:
    """Dissolve the edges into the page instead of ending on a rectangle.

    Two things happen at once, so the result is right whichever way itch
    composites it: the pixels are matted towards PAGE_BG, AND the alpha falls
    off. Over the flat background colour the two agree exactly; over the
    background IMAGE the falloff lets the page's own map show through, which is
    the same effect by a different route.

    The bottom gets most of it -- that is the edge that touches the page, and
    the one a reader's eye follows. The top gets a little, because the itch
    header sits above it. The sides are left alone by default: the banner is
    full-bleed there, so a fade only wastes picture.
    """
    w, h = img.size
    mask = Image.new("L", (w, h), 255)     # 255 = keep the picture
    mp = mask.load()

    bottom = max(1, int(h * 0.42))
    top = max(1, int(h * 0.12))
    side = max(1, int(w * 0.10)) if sides else 0

    for y in range(h):
        # 1.0 = fully the picture, 0.0 = fully the page
        ky = 1.0
        if y >= h - bottom:
            t = (y - (h - bottom)) / bottom
            ky = min(ky, (1.0 - t) ** 1.6)
        if y < top:
            ky = min(ky, (y / top) ** 1.4)
        row = int(255 * ky)
        for x in range(w):
            k = row
            if side and (x < side or x >= w - side):
                d = x / side if x < side else (w - 1 - x) / side
                k = int(k * min(1.0, d ** 1.3))
            mp[x, y] = k

    faded = Image.composite(img.convert("RGB"), Image.new("RGB", (w, h), PAGE_BG), mask)
    out = faded.convert("RGBA")
    out.putalpha(mask)
    return out


def build(scenario: str, region_name: str, size_name: str, titled: bool,
          merge: bool = False, merge_sides: bool = False,
          saturation: float = 0.86, brightness: float = 0.72) -> str:
    size = SIZES[size_name]
    base = crop_to_aspect(political(scenario), REGIONS[region_name], size)

    # Muted, not dimmed flat: the scenario colours are the point of the picture,
    # but at full saturation they fight anything placed on top of them.
    #
    # ADJUSTABLE because the scenarios do not agree about colour. The historical
    # ones are dressed in period alliance colours -- browns, reds, imperial
    # blues -- while Modern Day has 185 countries whose colours only have to be
    # distinguishable from their neighbours, so it comes out as neon and needs
    # taking down considerably further before a gold wordmark can sit on it.
    base = ImageEnhance.Color(base).enhance(saturation)
    base = ImageEnhance.Brightness(base).enhance(brightness)

    # A soft vignette, so the edges of a cropped banner do not end on a hard
    # rectangle of bright map when a page trims it.
    vign = Image.new("L", size, 0)
    vp = vign.load()
    cx, cy = size[0] / 2, size[1] / 2
    for y in range(size[1]):
        for x in range(0, size[0], 2):      # every other column, then blurred
            dx, dy = (x - cx) / cx, (y - cy) / cy
            d = min(1.0, (dx * dx * 0.55 + dy * dy) ** 0.5)
            v = int(120 * max(0.0, d - 0.55) / 0.45)
            vp[x, y] = v
            if x + 1 < size[0]:
                vp[x + 1, y] = v
    vign = vign.filter(ImageFilter.GaussianBlur(size[0] / 60))
    base = Image.composite(Image.new("RGB", size, (2, 3, 9)), base, vign)

    name = f"banner-{size_name}" + ("-merged" if merge else "") + ("" if titled else "-plain")
    out = os.path.join(OUT, name + ".png")
    if not titled:
        (merge_edges(base, merge_sides) if merge else base).save(out)
        return out

    art = darken_for_text(base, 0.62).convert("RGBA")
    mark = _wordmark_fn()()
    w = int(size[0] * 0.42)
    h = int(mark.height * (w / mark.width))
    mark = mark.resize((w, h), Image.LANCZOS)

    x = int(size[0] * 0.055)
    y = (size[1] - h) // 2
    # A shadow under the letters, because the wordmark is thin at banner scale
    # and gold on a lit coastline is the one place it disappears.
    glow = Image.new("RGBA", size, (0, 0, 0, 0))
    glow.alpha_composite(mark, (x, y + max(2, h // 22)))
    glow = glow.filter(ImageFilter.GaussianBlur(h / 14))
    art = Image.alpha_composite(art, glow)
    art.alpha_composite(mark, (x, y))

    # Feather AFTER the wordmark, not before: the fade is the banner's edge, and
    # a title that sat on top of it would be the one part still ending on a
    # rectangle. The wordmark is vertically centred, well clear of the bottom
    # falloff, so this dims nothing that has to stay readable.
    if merge:
        merge_edges(art, merge_sides).save(out)
    else:
        art.convert("RGB").save(out)
    return out


# Chronological, ending on the world as it is now and looping back to 1914.
# These are the scenario packages, so the animation is not an illustration of
# the game's history -- it IS the six maps the game ships, at full resolution.
ERAS = ["1914", "1918", "1939", "1945", "1962", "map"]


def collapse(frames: list, ms: int):
    """Merge runs of identical frames into one frame held for longer.

    A quarter of this animation is an era sitting still, stored as N copies of
    the same picture. Every format here takes a per-frame duration, so the
    copies are pure waste -- and in a palette format they are not even cheap,
    because each one still carries its own filtered, deflated image data.
    """
    out, durations = [], []
    for f in frames:
        if out and ImageChops.difference(f.convert("RGB"), out[-1].convert("RGB")).getbbox() is None:
            durations[-1] += ms
        else:
            out.append(f)
            durations.append(ms)
    return out, durations


def animate(region_name: str, size_name: str, hold: int, blend: int, fps: int,
            merge: bool, merge_sides: bool, saturation: float, brightness: float,
            titled: bool, palette: int = 256, truecolor: bool = False) -> list:
    """A looping cross-dissolve through every era, as GIF and WebP.

    WHY THIS AND NOT A TIMELAPSE

    docs/img/timelapse-political.gif already exists and is 640x320 -- a record
    of one game, at a third of the resolution a page banner needs. This is the
    six shipped scenarios instead: same crop, same colour treatment, dissolving
    1914 -> 1918 -> 1939 -> 1945 -> 1962 -> today and back. It says what the
    game is (six worlds, one map) in the only medium an itch page can animate.
    """
    size = SIZES[size_name]
    plates = []
    for era in ERAS:
        img = crop_to_aspect(political(era), REGIONS[region_name], size)
        img = ImageEnhance.Color(img).enhance(saturation)
        img = ImageEnhance.Brightness(img).enhance(brightness)
        plates.append(img.convert("RGB"))

    mark = None
    if titled:
        m = _wordmark_fn()()
        w = int(size[0] * 0.42)
        mark = m.resize((w, int(m.height * (w / m.width))), Image.LANCZOS)

    def dress(plate: Image.Image) -> Image.Image:
        art = (darken_for_text(plate, 0.62) if titled else plate).convert("RGBA")
        if mark is not None:
            x, y = int(size[0] * 0.055), (size[1] - mark.height) // 2
            glow = Image.new("RGBA", size, (0, 0, 0, 0))
            glow.alpha_composite(mark, (x, y + max(2, mark.height // 22)))
            glow = glow.filter(ImageFilter.GaussianBlur(mark.height / 14))
            art = Image.alpha_composite(art, glow)
            art.alpha_composite(mark, (x, y))
        return merge_edges(art, merge_sides) if merge else art

    frames = []
    for i, plate in enumerate(plates):
        frames += [dress(plate)] * hold
        nxt = plates[(i + 1) % len(plates)]
        for b in range(1, blend + 1):
            frames.append(dress(Image.blend(plate, nxt, b / (blend + 1))))

    stem = f"banner-{size_name}-eras" + ("" if titled else "-plain")
    ms = int(1000 / fps)

    # EVERY ANIMATED FRAME IS OPAQUE, matted onto the page colour. Not a
    # limitation of one format -- a correctness rule for all three.
    #
    # An animated frame carrying a soft alpha edge is composited ONTO the frame
    # before it, not in place of it. So the feathered top and bottom stack:
    # each pass adds another semi-transparent copy of the edge, the map ghosts
    # through the fade, and the loop visibly accumulates dirt until it wraps.
    # A still can carry alpha; a sequence cannot, unless every frame declares
    # "replace what is under me", which GIF spells one way, WebP another and
    # APNG a third.
    #
    # Matting costs nothing here, because merge_edges already faded towards
    # exactly this colour -- and it is why the animations must sit on #050813.
    flat = [Image.alpha_composite(Image.new("RGBA", size, PAGE_BG + (255,)), f).convert("RGB")
            for f in frames]

    written = []

    # APNG FIRST, because on itch it is the one that actually moves. Animated
    # GIFs uploaded through the page theme editor have rendered as stills since
    # around October 2025 (itch.io/t/5437790 and itch.io/t/5373866; still open,
    # no staff reply), and the workaround the community converged on is APNG,
    # which the same image pipeline leaves alone. Quantised to a shared palette
    # or a full-colour APNG of a map runs to tens of megabytes.
    out_apng = os.path.join(OUT, stem + ".png")
    # ONE palette, derived from every era at once and from the page colour.
    #
    # Taken from the first frame alone it is a palette of 1914: the later eras
    # get mapped onto colours that were never theirs, and -- worse -- the page
    # background lands on a slightly different entry from frame to frame, so the
    # feathered edge FLICKERS against the flat #050813 it is supposed to
    # disappear into. A montage of thumbnails plus a solid block of the page
    # colour makes the palette cover the whole loop and pins that one colour.
    thumb_w = max(1, size[0] // 8)
    thumb_h = max(1, size[1] // 8)
    montage = Image.new("RGB", (thumb_w * len(flat), thumb_h), PAGE_BG)
    for i, f in enumerate(flat):
        montage.paste(f.resize((thumb_w, thumb_h), Image.NEAREST), (i * thumb_w, 0))
    # A quarter of the montage is the page colour, so no quantiser drops it.
    montage.paste(Image.new("RGB", (montage.width // 4, thumb_h), PAGE_BG), (0, 0))
    pal = montage.quantize(colors=palette, method=Image.MEDIANCUT)
    # NO DITHERING, and the full 256 entries.
    #
    # Floyd-Steinberg scatters a different noise pattern through every frame.
    # That is the crawling dots on the smooth fades, and it also destroys the
    # only compression an animation has -- the encoder stores each frame as a
    # delta against the one before, and dithered noise makes every pixel a
    # difference. Measured on this banner: 96 colours dithered came to 6.2 MB at
    # 32.8 dB, while 256 colours undithered is 3.7 MB at 39.2 dB. Smaller AND
    # better, which is not a trade-off, just a mistake corrected.
    # TRUE COLOUR IS THE ONLY WAY TO HAVE BOTH DISSOLVES AND HONEST COLOUR.
    #
    # 256 entries have to cover six eras AND every step between them. A held
    # frame survives that (mean error 5.0 across the map); a dissolving frame
    # does not (7.1), because its in-between colours belong to no era and get
    # snapped to whichever one is nearest -- so a smooth fade arrives as regions
    # flipping palette entry, which is what "the colours look weird" is. No
    # amount of dithering or palette tuning fixes it: the colours are simply not
    # in the file. Dropping the palette costs bytes and nothing else.
    q = flat if truecolor else [f.quantize(palette=pal, dither=Image.NONE)
                                for f in flat]
    qf, qd = collapse(q, ms)
    # disposal NONE + blend SOURCE: every frame is opaque and full-size, so it
    # simply replaces the one before. Pillow's APNG writer also refuses to
    # dispose-to-background on palette images ("images do not match"), which is
    # the other reason not to ask it to.
    qf[0].save(out_apng, save_all=True, append_images=qf[1:], duration=qd,
               loop=0, disposal=0, blend=0, compress_level=9)
    written.append(out_apng)

    # GIF: 256 colours, 1-bit transparency, disposal 2 so nothing accumulates.
    # The safe fallback everywhere except, ironically, the itch theme slots.
    out_gif = os.path.join(OUT, stem + ".gif")
    gf, gd = collapse(flat, ms)
    gf[0].save(out_gif, save_all=True, append_images=gf[1:], duration=gd,
               loop=0, optimize=True, disposal=2)
    written.append(out_gif)

    # WebP: the only format here that can hold dissolves AND real colour.
    #
    # It is not close. A true-colour APNG of this loop is 13 MB, because PNG is
    # lossless and a cross-dissolve changes every pixel of every frame; the same
    # loop as WebP is 304 KB. Quantising the APNG instead is what put the
    # in-between colours on the nearest era's palette entry and made the fades
    # look wrong. So: WebP where anything will take it, APNG only where it will
    # not, knowing what that costs.
    #
    # EVERY FRAME A KEYFRAME (kmin = kmax = 1), and that is not negotiable for a
    # cross-dissolve.
    #
    # Animated WebP stores a frame as a rectangle of changed pixels over the one
    # before, and decides "changed" with a threshold. A dissolve moves each
    # pixel by a seventh of the way per frame, which over a dark map is under
    # that threshold, so the encoder rules whole regions unchanged and reuses
    # them -- frozen blocks that sit still while the middle of the picture
    # dissolves around them. Measured on this banner: between two dissolve
    # frames the SOURCE changes 25.5% of its pixels; the delta-coded file
    # changed 4.2%, with the left and right thirds at exactly 0.0%. It was small
    # because it had thrown the animation away.
    #
    # Independent frames cost about twice the bytes (306 KB -> 702 KB here) and
    # are the whole picture moving, which is what was asked for. minimize_size
    # is off for the same reason: it is that threshold, turned up.
    out_webp = os.path.join(OUT, stem + ".webp")
    wf, wd = collapse(flat, ms)
    wf[0].save(out_webp, save_all=True, append_images=wf[1:],
               duration=wd, loop=0, quality=68, method=6, kmin=1, kmax=1)
    written.append(out_webp)
    return written


def page_background(scenario: str, count: int) -> list:
    """The menu's silhouette as a page background, plus CSS for shells on land.

    Two files: the picture, and the rules that put every blast over land. The
    positions are not typed by hand -- they are sampled from the same land mask
    the picture is drawn from, which is what the menu does when it picks a
    spawn point (updateMenuBackground: `rand() % m_menuBgLandCoords.size()`).
    Hand-placed shells drift off the coast the moment the crop changes.
    """
    import random

    import numpy as np

    size = (2560, 1280)          # 2:1, the raster's own shape
    img, land = menu_silhouette(scenario, size)

    # LAND ONLY. The sea is a hole, not a colour.
    #
    # Painted, the sea was rgb(8,14,32) -- the menu's -- laid at 62% opacity
    # over the page's #050813, so what actually reached the screen was
    # rgb(7,12,27): not the menu's sea, not the page's background, and not any
    # value written down anywhere. Punching it out means the sea IS the page
    # background, exactly, at any layer opacity, and the only thing this image
    # contributes is land.
    rgba = np.dstack([np.asarray(img, dtype=np.uint8),
                      np.where(land, 255, 0).astype(np.uint8)])
    out_png = os.path.join(OUT, "page-bg-menu.png")
    Image.fromarray(rgba, "RGBA").save(out_png, optimize=True)

    # Sample land points, avoiding the poles (they are ice on the raster and
    # dead space on a page) and the outer margin (cover crops it away).
    ys, xs = land.nonzero()
    keep = (ys > size[1] * 0.18) & (ys < size[1] * 0.74) & \
           (xs > size[0] * 0.06) & (xs < size[0] * 0.94)
    ys, xs = ys[keep], xs[keep]
    rng = random.Random(20260803)          # stable output: no churn per run
    # Draw a surplus of candidates, because the spread filter below REJECTS --
    # sampling exactly `count` and then throwing some away can only ever end up
    # short, which is how a request for fourteen shells quietly became eight.
    picks = rng.sample(range(len(xs)), min(count * 12, len(xs)))

    # Spread them: two shells a few pixels apart are one shell as far as a
    # reader is concerned.
    chosen = []
    for i in picks:
        x, y = float(xs[i]) / size[0] * 100, float(ys[i]) / size[1] * 100
        if all((x - cx) ** 2 + (y - cy) ** 2 > 90 for cx, cy in chosen):
            chosen.append((x, y))
        if len(chosen) == count:
            break

    hosts = [
        ("#wrapper > .main", "::before"), ("#wrapper > .main", "::after"),
        (".view_game_page", "::before"),  (".view_game_page", "::after"),
        (".formatted_description", "::before"), (".formatted_description", "::after"),
        (".screenshot_list", "::before"), (".screenshot_list", "::after"),
        (".info_panel_wrapper", "::before"), (".info_panel_wrapper", "::after"),
        ("#view_game_footer", "::before"), ("#view_game_footer", "::after"),
        (".header_buy_row", "::before"), (".header_buy_row", "::after"),
    ]
    lines = [
        "/* Generated by tools/banner.py --page-bg. Every position is a land",
        "   pixel of page-bg-menu.png, sampled the way updateMenuBackground()",
        "   samples m_menuBgLandCoords -- so no shell bursts at sea.",
        "",
        "   IN MAP UNITS, NOT VIEWPORT UNITS. The map layer is 100vw wide and,",
        "   being 2:1, 50vw tall, pinned to the top of the viewport and moved",
        "   by the scroll. A shell placed in vh would sit still while the map",
        "   slid out from under it and end up in the sea; both coordinates here",
        "   are fractions of that same box, and the shells carry the same",
        "   scroll animation as the map, so they land on the same country at",
        "   every scroll position and every window size. */",
    ]
    for i, (x, y) in enumerate(chosen):
        host, pe = hosts[i % len(hosts)]
        dur = 5.5 + (i % 5) * 0.9          # 5.5..9.1s, so they never sync up
        delay = round((i * 0.61) % dur, 2)
        # TWO VALUES PER PROPERTY, because these elements run two animations:
        # the blast on its own clock and the rise on the scrollbar. A single
        # `animation-duration: 6s` applies to BOTH, and a scroll-driven
        # animation given a time duration completes inside a fraction of the
        # scroll -- the map would rise fully in the first few hundred pixels
        # and the shells would come adrift from it. `auto` is what keeps the
        # rise mapped to the whole scroll range.
        lines.append(
            f"body.game_layout_widget {host}{pe} "
            f"{{ left: {x:.1f}vw; top: {y * 0.5:.2f}vw; "
            f"animation-duration: {dur:.1f}s, auto; "
            f"animation-delay: {delay}s, 0s; }}")
    out_css = os.path.join(OUT, "page-blasts.css")
    with open(out_css, "w") as f:
        f.write("\n".join(lines) + "\n")

    return [out_png, out_css]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--scenario", default=DEFAULT_SCENARIO,
                    help=f"which .odmap to photograph (default {DEFAULT_SCENARIO})")
    ap.add_argument("--region", default="europe", choices=sorted(REGIONS),
                    help="which part of the world (default europe)")
    ap.add_argument("--size", default="all",
                    choices=["all"] + sorted(SIZES), help="which banner (default all)")
    ap.add_argument("--out-dir", default=None,
                    help="where to write (default docs/itch; use docs/steam"
                         " for the steam-* capsules)")
    ap.add_argument("--list", action="store_true", help="print the presets and exit")
    ap.add_argument("--merge", action="store_true",
                    help="fade the edges into the itch page background (#050813)")
    ap.add_argument("--merge-sides", action="store_true",
                    help="with --merge, fade the left and right edges too")
    ap.add_argument("--saturation", type=float, default=0.86,
                    help="colour of the map, 0..1 (default 0.86; try 0.55 for Modern Day)")
    ap.add_argument("--brightness", type=float, default=0.72,
                    help="how far the map is taken down (default 0.72)")
    ap.add_argument("--animate", action="store_true",
                    help="looping dissolve through all six scenarios (GIF + WebP)")
    ap.add_argument("--hold", type=int, default=8, help="frames held on each era")
    ap.add_argument("--blend", type=int, default=10, help="frames of dissolve between eras")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--page-bg", action="store_true",
                    help="the menu's grey silhouette as a page background, plus"
                         " CSS placing the shells on land")
    ap.add_argument("--blasts", type=int, default=14,
                    help="how many shells --page-bg places (default 14)")
    ap.add_argument("--truecolor", action="store_true",
                    help="APNG without a palette: real dissolves, bigger file")
    ap.add_argument("--palette", type=int, default=256,
                    help="colours in the APNG (default 256; lower is smaller, not better)")
    args = ap.parse_args()

    # Rebinding the module global rather than threading a directory through
    # build(), animate() and page_background(): there is exactly one output
    # directory per run, and every writer already reads this one name.
    global OUT
    if args.out_dir:
        OUT = args.out_dir if os.path.isabs(args.out_dir) \
              else os.path.join(ROOT, args.out_dir)

    if args.list:
        print("sizes:")
        for k, (w, h) in SIZES.items():
            print(f"  {k:15s} {w}x{h}")
        print("regions (in the 8192x4096 raster):")
        for k, r in REGIONS.items():
            print(f"  {k:15s} {r}")
        print("scenarios:")
        print("  " + ", ".join(sorted(f[:-6] for f in os.listdir(MAPS)
                                      if f.endswith(".odmap"))))
        return 0

    os.makedirs(OUT, exist_ok=True)

    if args.page_bg:
        for p in page_background(args.scenario, args.blasts):
            print(f"  {os.path.relpath(p, ROOT):46s} ({os.path.getsize(p) // 1024} KB)")
        return 0

    wanted = sorted(SIZES) if args.size == "all" else [args.size]

    if args.animate:
        for s in wanted:
            for titled in (True, False):
                for p in animate(args.region, s, args.hold, args.blend, args.fps,
                                 args.merge, args.merge_sides,
                                 args.saturation, args.brightness, titled,
                                 args.palette, args.truecolor):
                    print(f"  {os.path.relpath(p, ROOT):46s} "
                          f"{Image.open(p).size[0]}x{Image.open(p).size[1]}"
                          f"  ({os.path.getsize(p) // 1024} KB)")
        return 0

    for s in wanted:
        for titled in (True, False):
            p = build(args.scenario, args.region, s, titled,
                      merge=args.merge, merge_sides=args.merge_sides,
                      saturation=args.saturation, brightness=args.brightness)
            print(f"  {os.path.relpath(p, ROOT):46s} "
                  f"{Image.open(p).size[0]}x{Image.open(p).size[1]}"
                  f"  ({os.path.getsize(p) // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
