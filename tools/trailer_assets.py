#!/usr/bin/env python3
"""Build the still assets the trailer cut needs.

Everything here is derived from something already in the repo or already
licensed for the download page -- the wordmark from art/logo, the platform
marks from packaging/web/site/download.html, the browser mark from Lucide.
Nothing is drawn by hand, which is the rule the download page set.

Writes docs/trailer/assets/.  Run it again after changing the palette; it is
idempotent and overwrites.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "docs/trailer/assets")

# The site stylesheet's lamplit-map-table palette.  plan.md fixes these.
SUNKEN = "#090B11"
TEXT   = "#E8E4DA"
GOLD   = "#C9A227"

def sh(cmd):
    subprocess.run(cmd, check=True)

def rsvg(src_svg_text, out_png, width, height=None):
    tmp = out_png + ".svg"
    open(tmp, "w").write(src_svg_text)
    cmd = ["rsvg-convert", "-w", str(width)]
    if height:
        cmd += ["-h", str(height), "--keep-aspect-ratio"]
    cmd += ["-o", out_png, tmp]
    sh(cmd)
    os.remove(tmp)

def wordmark():
    """Two lockups, both gold.

    The repo's art/logo/od-mark.svg is the O.D. MONOGRAM, which is a site
    favicon and a sidebar mark -- it does not say the name.  An end card whose
    only job is to leave somebody able to look the game up cannot be a
    monogram, so the full name is generated from the same script and the same
    font, and the monogram is kept for the second card where the name has
    already been read.
    """
    s = open(os.path.join(ROOT, "art/logo/od-mark.svg")).read()
    rsvg(s.replace('fill="currentColor"', 'fill="%s"' % GOLD),
         os.path.join(OUT, "mark-od-gold.png"), 600)
    full = os.path.join(OUT, "_full.svg")
    sh([sys.executable, os.path.join(ROOT, "tools/make_logo_svg.py"),
        "--text", "OPEN DOCTRINES", "--out", full])
    s = open(full).read()
    rsvg(s.replace('fill="currentColor"', 'fill="%s"' % GOLD),
         os.path.join(OUT, "wordmark-gold.png"), 2800)
    os.remove(full)

def platform_marks():
    """The download page's own icons, recoloured to the caption text colour.

    Order in download.html is fixed by the page's own card order: Windows,
    Apple, Linux, Haiku, ChromeOS, FreeBSD, OpenBSD, NetBSD (its flag),
    TempleOS (a church).  Taking them
    by index is why this asserts the count -- if the page gains a platform,
    this stops rather than silently relabelling every mark.
    """
    html = open(os.path.join(ROOT, "packaging/web/site/download.html")).read()
    svgs = re.findall(r"<svg[^>]*>.*?</svg>", html, re.S)
    assert len(svgs) == 9, "download.html has %d icons, expected 9" % len(svgs)
    names = ["windows", "apple", "linux", "haiku", "chromeos", "freebsd", "openbsd", "netbsd", "templeos"]
    for name, svg in zip(names, svgs):
        stroked = 'class="stroked"' in svg
        svg = svg.replace("<svg", '<svg xmlns="http://www.w3.org/2000/svg"', 1)
        if stroked:
            svg = svg.replace("<svg", '<svg fill="none" stroke="%s" stroke-width="2" '
                              'stroke-linecap="round" stroke-linejoin="round"' % TEXT, 1)
        else:
            svg = svg.replace("<svg", '<svg fill="%s"' % TEXT, 1)
        rsvg(svg, os.path.join(OUT, "mark-%s.png" % name), 256, 256)

def browser_mark():
    """Lucide app-window -- the one platform the download page has no icon for,
    because a browser tab is not a download."""
    src = os.path.join(ROOT, "docs/trailer/vendor/app-window.svg")
    if not os.path.exists(src):
        sys.exit("missing %s -- fetch lucide-static icons/app-window.svg" % src)
    s = open(src).read()
    assert "<svg" in s, "not an SVG: %s" % src
    s = re.sub(r'stroke="[^"]*"', 'stroke="%s"' % TEXT, s)
    rsvg(s, os.path.join(OUT, "mark-browser.png"), 256, 256)

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    wordmark(); platform_marks(); browser_mark()
    for f in sorted(os.listdir(OUT)):
        print(f, os.path.getsize(os.path.join(OUT, f)))
