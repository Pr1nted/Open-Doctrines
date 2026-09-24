#!/usr/bin/env python3
"""Render what the agent door's turn would look like on a TempleOS screen.

Parsed from a REAL capture of `OpenDoctrinesServer --bench-agent`, not invented,
so the column widths and the wrapping are the ones a HolyC client would actually
have to deal with. The point of the mockup is to find out whether it reads at
80x60 in 16 colours before anybody writes HolyC.

TempleOS is 640x480 with an 8x8 font: 80 columns by 60 rows, 16 fixed colours,
white ground. This renders at 2x (1280x960) because that is how anyone looks at
it in an emulator anyway.
"""
import re, sys, pathlib
from PIL import Image, ImageDraw, ImageFont

COLS, ROWS, CELL = 80, 60, 16          # 2x of TempleOS's 8x8

# TempleOS's 16. Names as the OS spells them.
C = {
    "BLACK": (0, 0, 0), "BLUE": (0, 0, 170), "GREEN": (0, 170, 0),
    "CYAN": (0, 170, 170), "RED": (170, 0, 0), "PURPLE": (170, 0, 170),
    "BROWN": (170, 85, 0), "LTGRAY": (170, 170, 170), "DKGRAY": (85, 85, 85),
    "LTBLUE": (85, 85, 255), "LTGREEN": (85, 255, 85), "LTCYAN": (85, 255, 255),
    "LTRED": (255, 85, 85), "LTPURPLE": (255, 85, 255), "YELLOW": (255, 255, 85),
    "WHITE": (255, 255, 255),
}

# ── read the real capture ────────────────────────────────────────────────────

def parse(path):
    txt = pathlib.Path(path).read_text(errors="replace").splitlines()
    g = {"menus": {}}
    for ln in txt:
        if m := re.match(r"\[AGENT\] ===== turn (\d+)/(\d+)\s+(.*) \((\w+)\)", ln):
            g["turn"], g["turns"], g["country"], g["iso"] = m[1], m[2], m[3], m[4]
        elif m := re.match(r"\[AGENT\] land (\d+)/(\d+) \(([\d.]+)%.*army (-?\d+)\s+treasury (-?[\d.]+)", ln):
            g.update(mine=m[1], owned=m[2], share=m[3], army=int(m[4]), treas=m[5])
        elif m := re.match(r"\[AGENT\] gross (-?[\d.]+) net (-?[\d.]+)\s+\((.*)\)", ln):
            g.update(gross=m[1], net=m[2], breakdown=m[3])
        elif m := re.match(r"\[AGENT\] at war with: (.*)", ln):
            g["war"] = m[1].strip()
        elif m := re.match(r"\[AGENT\] (economy|politics|war|navy)\s+(.*)", ln):
            g["menus"][m[1]] = re.findall(r"([epwn]):(\d+) (.+?)(?=\s{2}[epwn]:\d|\s*$)", m[2])
        elif m := re.match(r"\[AGENT\] budget e:(\d+) p:(\d+) w:(\d+) n:(\d+)", ln):
            g["budget"] = dict(zip("epwn", m.groups()))
    return g

# ── a character grid, because that is all the OS has ─────────────────────────

class Screen:
    def __init__(self):
        self.ch = [[" "] * COLS for _ in range(ROWS)]
        self.fg = [["BLACK"] * COLS for _ in range(ROWS)]
        self.bg = [["WHITE"] * COLS for _ in range(ROWS)]

    def put(self, x, y, s, fg="BLACK", bg="WHITE"):
        for i, c in enumerate(s):
            if 0 <= x + i < COLS and 0 <= y < ROWS:
                self.ch[y][x + i] = c
                self.fg[y][x + i] = fg
                self.bg[y][x + i] = bg

    def rule(self, y, fg="DKGRAY"):
        self.put(0, y, "-" * COLS, fg)

def draw(g):
    s = Screen()
    bud = g.get("budget", {})

    # Title bar, inverted the way TempleOS marks a window's own strip.
    s.put(0, 0, " " * COLS, "WHITE", "BLUE")
    s.put(1, 0, "OpenDoctrines  Agent Session", "WHITE", "BLUE")
    right = f"Turn {g['turn']}/{g['turns']}"
    s.put(COLS - len(right) - 1, 0, right, "YELLOW", "BLUE")

    # Who you are, and the four numbers that decide everything.
    s.put(1, 2, f"{g['country']} ({g['iso']})", "RED")
    s.put(20, 2, "Land", "DKGRAY")
    s.put(26, 2, f"{g['mine']}/{g['owned']}  {g['share']}%", "BLACK")
    s.put(46, 2, "Army", "DKGRAY")
    s.put(52, 2, f"{g['army']:,}", "BLACK")
    s.put(20, 3, "Treas", "DKGRAY")
    s.put(26, 3, f"{g['treas']}", "BLACK")
    s.put(46, 3, "Net", "DKGRAY")
    net = float(g["net"])
    s.put(52, 3, f"{'+' if net >= 0 else ''}{g['net']}  (gross {g['gross']})",
          "GREEN" if net >= 0 else "RED")
    war = g.get("war", "(nobody)")
    s.put(1, 4, "At war with:", "DKGRAY")
    s.put(14, 4, war, "DKGRAY" if war == "(nobody)" else "RED")
    s.rule(5)

    # ── THE MENUS, IN TWO COLUMNS ──
    #
    # The door prints these as single lines up to 144 characters. At 80 columns
    # they cannot be echoed, so the client re-lays them out. Two columns of 38
    # fits the widest label with room to spare and keeps all four modules on
    # screen at once, which is the thing that makes the turn playable.
    order = [("economy", "ECONOMY", "e"), ("war", "WAR", "w"),
             ("politics", "POLITICS", "p"), ("navy", "NAVY", "n")]
    col_x, top = [1, 41], 7
    tallest = 0
    for i, (key, label, letter) in enumerate(order):
        x = col_x[i % 2]
        y = top + (i // 2) * (tallest + 3 if tallest else 0)
        items = g["menus"].get(key, [])
        s.put(x, y, label, "BLUE")
        s.put(x + 20, y, f"budget {bud.get(letter,'?')}", "DKGRAY")
        for j, (_, num, name) in enumerate(items):
            row = y + 1 + j
            if row >= ROWS - 6:
                break
            s.put(x + 1, row, f"{letter}:{num:<2}", "CYAN")
            s.put(x + 6, row, name[:30], "BLACK")
        if i % 2 == 1:
            tallest = max(len(g["menus"].get(order[i-1][0], [])),
                          len(items))
    # The second row of modules, placed under the tallest of the first pair.
    second = top + max(len(g["menus"].get("economy", [])),
                       len(g["menus"].get("war", []))) + 3
    for i, (key, label, letter) in enumerate(order[2:]):
        x = col_x[i]
        items = g["menus"].get(key, [])
        s.put(x, second, label, "BLUE")
        s.put(x + 20, second, f"budget {bud.get(letter,'?')}", "DKGRAY")
        for j, (_, num, name) in enumerate(items):
            s.put(x + 1, second + 1 + j, f"{letter}:{num:<2}", "CYAN")
            s.put(x + 6, second + 1 + j, name[:30], "BLACK")

    # ── THE ORDER LINE ──
    #
    # Exactly the string the door reads back, shown as it is typed, so what you
    # send is never hidden behind a menu abstraction.
    y = ROWS - 4
    s.rule(y - 1)
    s.put(1, y, "Orders>", "BLUE")
    s.put(9, y, "e:1, e:1, w:1, p:9", "BLACK")
    s.put(27, y, "_", "BLACK", "LTGRAY")
    s.put(1, y + 2, "ENTER send    TAB module    BKSP undo    F1 help    ESC hold turn",
          "DKGRAY")
    return s

# ── to pixels ────────────────────────────────────────────────────────────────

def render(s, out, px=13, unit=16):
    """One glyph per cell, drawn at natural metrics, then stretched to square.

    TempleOS's font is a square 8x8 and no ordinary TTF is square. Forcing a
    square cell around a narrow face either letter-spaces every word (cell wider
    than the glyph) or overlaps the rows (cell shorter than the line). So the
    text is laid out at the font's own advance and line height, and the finished
    image is stretched horizontally onto square cells -- which is precisely what
    a square font looks like, and lands on 80x60 at 4:3, the shape of 640x480.
    """
    font = None
    for path in ("/System/Library/Fonts/Menlo.ttc",
                 "/System/Library/Fonts/Monaco.ttf",
                 "/System/Library/Fonts/Supplemental/Andale Mono.ttf"):
        try:
            font = ImageFont.truetype(path, px)
            break
        except Exception:
            continue
    if font is None:
        font = ImageFont.load_default()
    cw = round(font.getlength("M")) or 8
    chh = unit                                   # rows at their final height
    img = Image.new("RGB", (COLS * cw, ROWS * chh), C["WHITE"])
    d = ImageDraw.Draw(img)
    for y in range(ROWS):
        for x in range(COLS):
            bg = C[s.bg[y][x]]
            if bg != C["WHITE"]:
                d.rectangle([x*cw, y*chh, x*cw+cw-1, y*chh+chh-1], fill=bg)
            c = s.ch[y][x]
            if c != " ":
                d.text((x*cw, y*chh + 1), c, font=font, fill=C[s.fg[y][x]])
    img = img.resize((COLS * unit, ROWS * unit), Image.LANCZOS)
    img.save(out)
    return img.size

if __name__ == "__main__":
    g = parse(sys.argv[1])
    size = render(draw(g), sys.argv[2])
    print(f"{sys.argv[2]}  {size[0]}x{size[1]} (2x of 640x480, {COLS}x{ROWS} cells)")
    print(f"turn {g['turn']}/{g['turns']} {g['country']} ({g['iso']}), "
          f"{sum(len(v) for v in g['menus'].values())} legal actions across "
          f"{len(g['menus'])} modules")
