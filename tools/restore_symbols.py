#!/usr/bin/env python3
"""Regenerate all symbol SVGs from scratch with canonical viewBox."""

import os

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "symbols")
os.makedirs(DIR, exist_ok=True)

def W(inner):
    """Wrap inner content in an SVG with canonical viewBox."""
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">{inner}</svg>'

def G(inner):
    return f'<g fill="#fff">{inner}</g>'

C = '#fff'

SVGS = [
("anchor.svg", W(G(
    f'<path d="M0-60c-15 0-27 12-27 27s12 27 27 27 27-12 27-27-12-27-27-27zm0 14c7 0 13 6 13 13s-6 13-13 13-13-6-13-13 6-13 13-13zM-10-26v20l-30 0c-5 0-7 1-8 3-2 3-1 8 1 8h37v80c-1 12-1 25-1 37 0 10 0 20-1 22-1 4-4 5-12 3-15-4-35-13-39-18-2-3-2-7 0-7 3-1 6-3 6-7-1-4-12-12-22-17-5-3-11-6-14-7-3-2-5-3-5-2-1 2-4 10-4 16 0 12 9 25 14 26 2 1 3-1 4-5 1-4 2-6 3-7 3-3 10 1 18 7 12 9 20 18 28 22l9 6 10 5c12 5 17 7 22 8 9 2 16 4 20 7 4 3 7 4 8 1 4-6 10-11 21-17 9-4 16-6 17-6 11-4 22-9 26-13l5-4c6-6 10-7 14-4l4 4 3-4c2-3 3-9 3-17 0-5-1-10-2-13-1-5-3-10-5-12-2-2-4-2-5 0-3 3-5 5-9 8-9 6-14 11-15 15-2 4-1 5 2 4 3-2 5-2 5 2 1 4-5 9-14 13-14 9-32 16-40 15-3 0-6-2-7-5 0-4 0-32 1-59 1-27 2-42 3-44 1-1 12-1 24 0 10 1 12 2 12-3v-6l-16-2c-6 0-11-1-12-3-1-1-1-5-1-10l3-61-17-1c-5 0-9 0-10-1-1-2 0-6 1-10z"/>'
))),

("circle_stars.svg", W(
    f'<g fill="{C}"><polygon points="0,-90 10,-30 90,-30 20,10 40,80 0,40 -40,80 -20,10 -90,-30 -10,-30"/><polygon points="45,-78 57,-27 124,-51 62,0 72,73 40,28 -12,74 7,17 -63,-35 0,-30" transform="rotate(30)"/><polygon points="78,-45 90,7 145,-22 90,28 72,90 50,40 -5,70 15,28 -40,-10 20,-25" transform="rotate(60)"/><polygon points="90,0 100,50 150,40 95,65 65,100 50,55 0,60 30,35 -10,15 30,5" transform="rotate(90)"/><polygon points="78,45 88,92 130,95 80,105 45,100 38,60 5,50 40,35 15,0 50,15" transform="rotate(120)"/><polygon points="45,78 55,120 90,140 50,135 20,90 20,60 -10,35 30,30 30,-10 55,20" transform="rotate(150)"/></g>'
)),

("crescent.svg", W(
    f'<circle cx="20" cy="0" r="80" fill="{C}"/><circle cx="45" cy="0" r="70" fill="#000"/>'
)),

("crescent_star.svg", W(
    f'<circle cx="20" cy="0" r="80" fill="{C}"/><circle cx="45" cy="0" r="70" fill="#000"/><polygon points="-10,-25 0,-10 15,-10 5,5 10,20 -5,10 -20,20 -15,5 -25,-10 -10,-10" fill="{C}"/>'
)),

("cross_latin.svg", W(
    f'<rect x="-10" y="-90" width="20" height="180" fill="{C}"/><rect x="-50" y="-20" width="100" height="40" fill="{C}"/>'
)),

("cross_maltese.svg", W(
    f'<path d="M-30-80 L-10-30 L30-30 L50-80 L30-10 L80 0 L30 10 L50 80 L30 30 L-10 30 L-30 80 L-10 10 L-80 0 L-10-10 Z" fill="{C}"/>'
)),

("cross_nordic.svg", W(
    f'<rect x="-80" y="-80" width="160" height="160" fill="{C}"/><rect x="-20" y="-80" width="20" height="160" fill="#e00"/><rect x="-80" y="-15" width="160" height="20" fill="#e00"/>'
)),

("cross_pattee.svg", W(
    f'<path d="M-25-100 L25-100 L25-25 L100-25 L100 25 L25 25 L25 100 L-25 100 L-25 25 L-100 25 L-100-25 L-25-25 Z" fill="{C}"/>'
)),

("cross_saltir.svg", W(
    f'<polygon points="-80,-80 -60,-100 0,-20 60,-100 80,-80 20,0 80,80 60,100 0,20 -60,100 -80,80 -20,0" fill="{C}"/>'
)),

("crossed_swords.svg", W(
    f'<g fill="{C}"><g transform="rotate(30)"><rect x="-6" y="-80" width="12" height="60"/><rect x="-25" y="-80" width="50" height="10"/><rect x="-6" y="-20" width="12" height="30"/></g><g transform="rotate(-30)"><rect x="-6" y="-80" width="12" height="60"/><rect x="-25" y="-80" width="50" height="10"/><rect x="-6" y="-20" width="12" height="30"/></g></g>'
)),

("diamond.svg", W(
    f'<polygon points="0,-80 80,0 0,80 -80,0" fill="{C}"/>'
)),

("eagle.svg", W(
    f'<g fill="{C}"><path d="M0-70 C-30-70 -40-50 -50-30 L-80-20 L-60-10 L-70 10 L-40 0 L-30 30 L0 20 L30 30 L40 0 L70 10 L60-10 L80-20 L50-30 C40-50 30-70 0-70Z"/><circle cx="-20" cy="-40" r="8"/><circle cx="20" cy="-40" r="8"/><polygon points="0,10 -10,30 10,30"/></g>'
)),

("fasces.svg", W(
    f'<g fill="{C}"><rect x="-40" y="-80" width="80" height="20"/><rect x="-50" y="-60" width="100" height="15"/><rect x="-40" y="-45" width="80" height="15"/><rect x="-50" y="-30" width="100" height="15"/><rect x="-40" y="-15" width="80" height="15"/><rect x="-10" y="-80" width="20" height="160" rx="5"/><circle cx="0" cy="-90" r="15"/></g>'
)),

("gear.svg", W(
    f'<g fill="{C}"><circle cx="0" cy="0" r="50"/><circle cx="0" cy="0" r="25" fill="#000"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(0)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(30)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(60)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(90)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(120)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(150)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(180)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(210)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(240)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(270)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(300)"/><rect x="-8" y="-70" width="16" height="25" rx="2" transform="rotate(330)"/></g>'
)),

("hammer_sickle.svg", W(
    f'<g fill="{C}"><rect x="-10" y="-60" width="20" height="80" rx="5"/><rect x="-30" y="-75" width="60" height="15" rx="5"/><path d="M10-50 C30-40 50-60 60-80 C50-70 20-30 10-10Z"/><path d="M-10-50 C-50-40 -60-10 -40 20 C-30 0 -40-30 -10-40Z"/></g>'
)),

("mountain.svg", W(
    f'<g fill="{C}"><polygon points="-80,80 0,-60 80,80"/><polygon points="-30,-30 -10,-60 10,-30"/><polygon points="20,10 40,-20 60,10"/></g>'
)),

("rose.svg", W(
    f'<g fill="{C}"><circle cx="0" cy="0" r="40"/><path d="M0-70 C30-70 50-50 40-20 C50-40 30-60 0-60Z" transform="rotate(0)"/><path d="M0-70 C30-70 50-50 40-20 C50-40 30-60 0-60Z" transform="rotate(45)"/><path d="M0-70 C30-70 50-50 40-20 C50-40 30-60 0-60Z" transform="rotate(90)"/><path d="M0-70 C30-70 50-50 40-20 C50-40 30-60 0-60Z" transform="rotate(135)"/><rect x="-3" y="-10" width="6" height="30" rx="2"/></g>'
)),

("star5.svg", W(
    f'<polygon points="0,-90 22,-28 90,-28 38,12 55,80 0,42 -55,80 -38,12 -90,-28 -22,-28" fill="{C}"/>'
)),

("star6.svg", W(
    f'<polygon points="0,-90 26,-30 90,-30 40,10 55,80 0,45 -55,80 -40,10 -90,-30 -26,-30" fill="{C}"/><polygon points="0,-80 -23,30 -90,30 -38,-10 -55,-80 0,-45 55,-80 38,-10 90,30 23,30" fill="{C}"/>'
)),

("star_4.svg", W(
    f'<polygon points="0,-90 15,-30 90,-30 25,10 40,80 0,40 -40,80 -25,10 -90,-30 -15,-30" fill="{C}"/>'
)),

("star_of_david.svg", W(
    f'<polygon points="0,-80 70,60 -70,60" fill="none" stroke="{C}" stroke-width="15" stroke-linejoin="round"/><polygon points="0,80 70,-60 -70,-60" fill="none" stroke="{C}" stroke-width="15" stroke-linejoin="round"/>'
)),

("sun.svg", W(
    f'<g fill="{C}"><circle cx="0" cy="0" r="30"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(0)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(22.5)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(45)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(67.5)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(90)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(112.5)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(135)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(157.5)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(180)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(202.5)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(225)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(247.5)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(270)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(292.5)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(315)"/><polygon points="-6,-80 6,-80 4,-40 -4,-40" transform="rotate(337.5)"/></g>'
)),

("sun_wavy.svg", W(
    f'<g fill="{C}"><circle cx="0" cy="0" r="30"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(0)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(30)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(60)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(90)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(120)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(150)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(180)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(210)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(240)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(270)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(300)"/><path d="M0-80 C15-80 15-60 0-55 C-15-60 -15-80 0-80" transform="rotate(330)"/></g>'
)),

("swastika.svg", W(
    f'<g fill="{C}"><rect x="-30" y="-60" width="60" height="120"/><rect x="-60" y="-30" width="120" height="60"/><rect x="-60" y="-60" width="30" height="30"/><rect x="30" y="-60" width="30" height="30"/><rect x="-60" y="30" width="30" height="30"/><rect x="30" y="30" width="30" height="30"/></g>'
)),

("sword.svg", W(
    f'<g fill="{C}"><rect x="-8" y="-80" width="16" height="80"/><polygon points="-8,-80 8,-80 0,-100"/><rect x="-40" y="-15" width="80" height="12" rx="3"/><rect x="-8" y="0" width="16" height="40"/><circle cx="0" cy="50" r="10"/></g>'
)),

("torch.svg", W(
    f'<g fill="{C}"><path d="M-15,0 C-30,-40 0,-90 0,-90 C0,-90 30,-40 15,0 Z"/><rect x="-15" y="0" width="30" height="50"/><rect x="-25" y="30" width="50" height="10" rx="3"/><rect x="-10" y="50" width="20" height="30"/></g>'
)),

("tree.svg", W(
    f'<g fill="{C}"><polygon points="0,-80 -70,20 70,20"/><polygon points="0,-40 -60,40 60,40"/><rect x="-10" y="20" width="20" height="50"/></g>'
)),
]

count = 0
for fname, content in sorted(SVGS):
    fpath = os.path.join(DIR, fname)
    with open(fpath, 'w') as f:
        f.write(content + '\n')
    count += 1
    print(f'  {fname}')

print(f'\nWrote {count} symbols')
