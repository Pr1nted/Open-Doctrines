"""Generate clean SVG symbol files for flag rendering."""
import os

SYMBOLS_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "data", "flags", "symbols")
os.makedirs(SYMBOLS_DIR, exist_ok=True)

symbols = {
    "star5.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="0,-90 22,-28 90,-28 38,12 55,80 0,42 -55,80 -38,12 -90,-28 -22,-28"\n'
        '    fill="white"/>\n'
        '</svg>'
    ),
    "star6.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="0,-90 26,-30 90,-30 40,10 55,90 0,50 -55,90 -40,10 -90,-30 -26,-30"\n'
        '    fill="white"/>\n'
        '</svg>'
    ),
    "star_of_david.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="0,-90 80,50 -80,50" fill="white" stroke="white" stroke-width="4"/>\n'
        '  <polygon points="0,90 80,-50 -80,-50" fill="white" stroke="white" stroke-width="4"/>\n'
        '</svg>'
    ),
    "crescent.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <path d="M 30,-85 A 85,85 0 1,0 30,85 A 60,60 0 1,1 30,-85 Z" fill="white"/>\n'
        '</svg>'
    ),
    "crescent_star.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <path d="M 20,-80 A 75,75 0 1,0 20,80 A 55,55 0 1,1 20,-80 Z" fill="white"/>\n'
        '  <polygon points="30,-20 36,-5 52,-8 39,6 45,22 30,14 15,22 21,6 8,-8 24,-5" fill="white"/>\n'
        '</svg>'
    ),
    "hammer_sickle.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <rect x="-8" y="-70" width="16" height="55" fill="white"/>\n'
        '  <rect x="-30" y="-70" width="60" height="14" fill="white"/>\n'
        '  <path d="M 15,50 Q 75,30 65,-20 Q 55,15 10,30 Z" fill="white"/>\n'
        '</svg>'
    ),
    "cross_latin.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <rect x="-10" y="-90" width="20" height="110" fill="white"/>\n'
        '  <rect x="-40" y="-25" width="80" height="22" fill="white"/>\n'
        '</svg>'
    ),
    "cross_maltese.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="-10,-90 10,-90 10,-30 35,-45 40,-25 15,-10 30,10 10,5 10,40 -10,40 -10,5 -30,10 -15,-10 -40,-25 -35,-45 -10,-30" fill="white"/>\n'
        '</svg>'
    ),
    "cross_saltir.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <rect x="-10" y="-90" width="20" height="180" fill="white" transform="rotate(45)"/>\n'
        '  <rect x="-10" y="-90" width="20" height="180" fill="white" transform="rotate(-45)"/>\n'
        '</svg>'
    ),
    "cross_nordic.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <rect x="-10" y="-90" width="20" height="180" fill="white"/>\n'
        '  <rect x="-90" y="-12" width="180" height="24" fill="white"/>\n'
        '</svg>'
    ),
    "sun.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <circle cx="0" cy="0" r="30" fill="white"/>\n'
        '  <g fill="white">\n'
        '    <rect x="-4" y="-90" width="8" height="20" rx="2"/>\n'
        '    <rect x="-4" y="70" width="8" height="20" rx="2"/>\n'
        '    <rect x="-90" y="-4" width="20" height="8" rx="2"/>\n'
        '    <rect x="70" y="-4" width="20" height="8" rx="2"/>\n'
        '    <rect x="58" y="-58" width="8" height="16" rx="2" transform="rotate(45)"/>\n'
        '    <rect x="-66" y="42" width="8" height="16" rx="2" transform="rotate(45)"/>\n'
        '    <rect x="42" y="-66" width="16" height="8" rx="2" transform="rotate(45)"/>\n'
        '    <rect x="-58" y="58" width="16" height="8" rx="2" transform="rotate(45)"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "sun_wavy.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <circle cx="0" cy="0" r="35" fill="white"/>\n'
        '  <g fill="white">\n'
        '    <rect x="-3" y="-92" width="6" height="25"/>\n'
        '    <rect x="-3" y="67" width="6" height="25"/>\n'
        '    <rect x="-92" y="-3" width="25" height="6"/>\n'
        '    <rect x="67" y="-3" width="25" height="6"/>\n'
        '    <path d="M 55,-60 Q 48,-56 55,-50" fill="none" stroke="white" stroke-width="5" stroke-linecap="round"/>\n'
        '    <path d="M -55,60 Q -48,56 -55,50" fill="none" stroke="white" stroke-width="5" stroke-linecap="round"/>\n'
        '    <path d="M 60,55 Q 56,48 50,55" fill="none" stroke="white" stroke-width="5" stroke-linecap="round"/>\n'
        '    <path d="M -60,-55 Q -56,-48 -50,-55" fill="none" stroke="white" stroke-width="5" stroke-linecap="round"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "gear.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <circle cx="0" cy="0" r="40" fill="none" stroke="white" stroke-width="16"/>\n'
        '  <g fill="white">\n'
        '    <rect x="-10" y="-100" width="20" height="30" rx="3"/>\n'
        '    <rect x="-10" y="70" width="20" height="30" rx="3"/>\n'
        '    <rect x="-100" y="-10" width="30" height="20" rx="3"/>\n'
        '    <rect x="70" y="-10" width="30" height="20" rx="3"/>\n'
        '    <rect x="48" y="-76" width="20" height="30" rx="3" transform="rotate(45)"/>\n'
        '    <rect x="-68" y="46" width="20" height="30" rx="3" transform="rotate(45)"/>\n'
        '    <rect x="46" y="-68" width="30" height="20" rx="3" transform="rotate(45)"/>\n'
        '    <rect x="-76" y="48" width="30" height="20" rx="3" transform="rotate(45)"/>\n'
        '  </g>\n'
        '  <circle cx="0" cy="0" r="22" fill="black"/>\n'
        '  <circle cx="0" cy="0" r="18" fill="white"/>\n'
        '  <circle cx="0" cy="0" r="8" fill="black"/>\n'
        '</svg>'
    ),
    "mountain.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="-90,80 -40,-70 10,-20 60,-90 90,80" fill="white"/>\n'
        '  <polygon points="-30,80 -10,10 20,30 40,80" fill="black"/>\n'
        '</svg>'
    ),
    "sword.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <rect x="-6" y="-40" width="12" height="80" fill="white"/>\n'
        '  <polygon points="-20,-40 20,-40 0,-80" fill="white"/>\n'
        '  <rect x="-25" y="30" width="50" height="8" fill="white"/>\n'
        '  <rect x="-5" y="38" width="10" height="25" fill="white"/>\n'
        '  <rect x="-12" y="58" width="24" height="6" fill="white"/>\n'
        '</svg>'
    ),
    "crossed_swords.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g transform="rotate(45)">\n'
        '    <rect x="-5" y="-30" width="10" height="60" fill="white"/>\n'
        '    <polygon points="-16,-30 16,-30 0,-65" fill="white"/>\n'
        '    <rect x="-20" y="25" width="40" height="6" fill="white"/>\n'
        '    <rect x="-4" y="31" width="8" height="20" fill="white"/>\n'
        '  </g>\n'
        '  <g transform="rotate(-45)">\n'
        '    <rect x="-5" y="-30" width="10" height="60" fill="white"/>\n'
        '    <polygon points="-16,-30 16,-30 0,-65" fill="white"/>\n'
        '    <rect x="-20" y="25" width="40" height="6" fill="white"/>\n'
        '    <rect x="-4" y="31" width="8" height="20" fill="white"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "diamond.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="0,-80 60,0 0,80 -60,0" fill="white"/>\n'
        '</svg>'
    ),
    "tree.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <rect x="-8" y="30" width="16" height="35" fill="white"/>\n'
        '  <polygon points="0,-80 -70,30 70,30" fill="white"/>\n'
        '  <polygon points="0,-50 -55,30 55,30" fill="white"/>\n'
        '</svg>'
    ),
    "swastika.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <path d="M -10,-10 L 10,-10 L 10,-80 L 40,-80 L 40,-10 L 50,-10 L 50,-30 L 80,-30 '
        'L 80,10 L 50,10 L 50,40 L 80,40 L 80,70 L 40,70 L 40,40 L 10,40 '
        'L 10,80 L -10,80 L -10,40 L -40,40 L -40,20 L -10,20 L -10,-10 Z" fill="white"/>\n'
        '</svg>'
    ),
    "eagle.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="white">\n'
        '    <path d="M 0,-80 Q 30,-80 40,-60 L 35,-50 Q 50,-35 45,-20 L 55,-10 '
        'L 50,0 Q 60,10 55,25 L 65,35 L 55,45 L 40,30 L 30,40 L 15,90 '
        'L -15,90 L -30,40 L -40,30 L -55,45 L -65,35 L -55,25 '
        'Q -60,10 -50,0 L -55,-10 Q -45,-20 -50,-35 L -35,-50 '
        'Q -40,-60 -30,-80 Z"/>\n'
        '    <path d="M -25,-65 Q -20,-70 -15,-65 Q -20,-55 -25,-65 Z" fill="black"/>\n'
        '    <path d="M 15,-65 Q 20,-70 25,-65 Q 20,-55 15,-65 Z" fill="black"/>\n'
        '    <path d="M -5,-20 Q 0,-25 5,-20 Q 0,-10 -5,-20 Z" fill="black"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "fasces.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="white">\n'
        '    <rect x="-4" y="-80" width="8" height="55" rx="2"/>\n'
        '    <rect x="-4" y="-14" width="8" height="60" rx="2"/>\n'
        '    <rect x="-4" y="38" width="8" height="25" rx="2"/>\n'
        '    <ellipse cx="0" cy="-82" rx="10" ry="6"/>\n'
        '    <polygon points="0,-95 -15,-80 15,-80"/>\n'
        '    <rect x="-6" y="44" width="12" height="4"/>\n'
        '    <rect x="-6" y="50" width="12" height="4"/>\n'
        '    <rect x="-6" y="56" width="12" height="4"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "torch.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="white">\n'
        '    <rect x="-8" y="-30" width="16" height="60" rx="3"/>\n'
        '    <rect x="-12" y="25" width="24" height="6" rx="2"/>\n'
        '    <rect x="-16" y="35" width="32" height="6" rx="2"/>\n'
        '    <path d="M 0,-80 Q 20,-60 10,-40 Q 5,-30 0,-35 Q -5,-30 -10,-40 Q -20,-60 0,-80 Z"/>\n'
        '    <ellipse cx="0" cy="-45" rx="6" ry="10"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "rose.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="white">\n'
        '    <ellipse cx="-20" cy="-20" rx="25" ry="20" transform="rotate(-30)"/>\n'
        '    <ellipse cx="20" cy="-20" rx="25" ry="20" transform="rotate(30)"/>\n'
        '    <ellipse cx="0" cy="10" rx="25" ry="20" transform="rotate(0)"/>\n'
        '    <ellipse cx="-15" cy="25" rx="20" ry="15" transform="rotate(20)"/>\n'
        '    <ellipse cx="15" cy="25" rx="20" ry="15" transform="rotate(-20)"/>\n'
        '    <circle cx="0" cy="0" r="8"/>\n'
        '    <rect x="-3" y="30" width="6" height="30"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "anchor.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="none" stroke="white" stroke-width="8" stroke-linecap="round">\n'
        '    <line x1="0" y1="-80" x2="0" y2="30"/>\n'
        '    <line x1="-30" y1="-80" x2="30" y2="-80"/>\n'
        '    <circle cx="0" cy="30" r="25" fill="white"/>\n'
        '    <path d="M -40,30 Q -60,60 0,70 Q 60,60 40,30" fill="white" stroke="none"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "circle_stars.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="white">\n'
        '    <polygon points="0,-73 7,-60 21,-60 10,-50 13,-36 0,-42 -13,-36 -10,-50 -21,-60 -7,-60"/>\n'
        '    <polygon points="53,-53 58,-40 72,-38 60,-30 62,-16 50,-24 38,-16 40,-30 28,-38 42,-40"/>\n'
        '    <polygon points="73,0 76,13 90,17 74,22 72,36 62,24 48,30 56,16 46,4 60,6"/>\n'
        '    <polygon points="53,53 54,66 66,72 52,72 46,84 38,70 24,72 32,60 20,54 34,50"/>\n'
        '    <polygon points="0,73 0,86 12,92 2,84 0,96 -2,84 -12,92 0,86"/>\n'
        '    <polygon points="-53,53 -54,66 -66,72 -52,72 -46,84 -38,70 -24,72 -32,60 -20,54 -34,50"/>\n'
        '    <polygon points="-73,0 -76,13 -90,17 -74,22 -72,36 -62,24 -48,30 -56,16 -46,4 -60,6"/>\n'
        '    <polygon points="-53,-53 -58,-40 -72,-38 -60,-30 -62,-16 -50,-24 -38,-16 -40,-30 -28,-38 -42,-40"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "cross_pattee.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <path d="M -15,-100 L 15,-100 L 15,-15 L 100,-15 L 100,15 L 15,15 L 15,100 '
        'L -15,100 L -15,15 L -100,15 L -100,-15 L -15,-15 Z" fill="white"/>\n'
        '</svg>'
    ),
    "star_4.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="0,-90 20,-20 90,0 20,20 0,90 -20,20 -90,0 -20,-20" fill="white"/>\n'
        '</svg>'
    ),
    "lightning.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <polygon points="-15,-100 15,-30 -5,-10 20,30 5,50 25,100 -10,20 10,0 -15,-40" fill="white"/>\n'
        '</svg>'
    ),
    "hand.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="white">\n'
        '    <rect x="-8" y="-40" width="16" height="80" rx="6"/>\n'
        '    <rect x="-8" y="-55" width="10" height="25" rx="5"/>\n'
        '    <rect x="-12" y="-65" width="10" height="25" rx="5" transform="rotate(-30)"/>\n'
        '    <rect x="2" y="-60" width="10" height="25" rx="5" transform="rotate(20)"/>\n'
        '    <rect x="-15" y="-50" width="10" height="20" rx="5" transform="rotate(-50)"/>\n'
        '    <rect x="6" y="-50" width="10" height="20" rx="5" transform="rotate(40)"/>\n'
        '  </g>\n'
        '</svg>'
    ),
    "wheel.svg": (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -100 200 200">\n'
        '  <g fill="none" stroke="white" stroke-width="8">\n'
        '    <circle cx="0" cy="0" r="65"/>\n'
        '    <circle cx="0" cy="0" r="15" fill="white"/>\n'
        '    <line x1="0" y1="-65" x2="0" y2="65"/>\n'
        '    <line x1="-65" y1="0" x2="65" y2="0"/>\n'
        '    <line x1="-46" y1="-46" x2="46" y2="46"/>\n'
        '    <line x1="-46" y1="46" x2="46" y2="-46"/>\n'
        '  </g>\n'
        '</svg>'
    ),
}

for name, content in symbols.items():
    path = os.path.join(SYMBOLS_DIR, name)
    with open(path, "w") as f:
        f.write(content)
    print(f"  {name} ({len(content)} bytes)")

print(f"\n{len(symbols)} symbols written to {SYMBOLS_DIR}")
