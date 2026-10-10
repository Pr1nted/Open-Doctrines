#!/usr/bin/env python3
"""The Steam trailer's march: "When Johnny Comes Marching Home", arranged.

    python3 tools/trailer_march.py docs/trailer/assets/march.wav
    python3 tools/trailer_march.py --cues          # print the cue sheet only

WHY THIS TUNE, AND WHY IT IS SAFE TO USE. The brief asked for a remix of a
known march in the way Paradox remixed one for Hearts of Iron. The legal half
of that is the whole problem: a trailer on Steam and YouTube runs through
content ID, and a recognisable arrangement that somebody else owns is a claim
on the video even when the underlying tune is ancient. So the tune is taken
from a source that is unambiguously public domain, and the arrangement -- every
chord, every drum, every voice below the melody -- is written here.

    melody: O'Neill's *Music of Ireland* (1903) #468, titled there
            "Johnny Fill Up the Bowl" / "Johnny, I Hardly Knew You" /
            "When Johnny Comes Marching Home".  6/8, A minor, 16 bars.
            Cross-checked against O'Neill's #69 (the 24-bar Irish air) and
            against the Mandozine TOW setting, which agree note for note on
            the 16 bars this file uses.

Patrick Gilmore published the American words in 1863 and O'Neill printed the
air in 1903; both are long out of copyright. Nothing is sampled and no MIDI is
imported -- the note list below IS the melody, transcribed from the ABC.

WHAT THE ARRANGEMENT IS FOR. The previous score was judged "way too repetitive,
no action at all, detached from reality", and the diagnosis was right: it was
one eight-note ostinato under the entire ninety seconds. A march fixes that by
construction, because a march has a form. This one is in eight sections and no
two are scored alike:

    HALT      field drum alone, no tune yet -- establishes key and pulse
    MARCH IN  the tune, solo, one line, nothing under it
    STOP      the cut's hinge at 0:29.6 -- everything gone but the tail
    AFTERMATH the same phrases given to the choir, slow, no drum.  This is the
              point of choosing this tune: the Irish original is a lament for a
              maimed soldier and the American refrain is a victory parade, and
              they are the SAME NOTES.  The trailer gets to play both.
    REBUILD   the B strain in augmentation under the A strain -- the one
              contrapuntal passage, and the only place two strains collide
    FULL      everything, octave-doubled
    WIDE      lead over choir, drum pulled back to breathe
    LOGO      the refrain's hurrah bars, then one sustained cadence

ON THE INSTRUMENTS. These are stylised, not emulated. Synthesis here is
numpy-only (no scipy in this repo, and no sample library anywhere in it), which
will not convincingly impersonate an orchestra, so it does not try: the lead is
a harmonic stack with a brass-like onset rise rather than a fake trumpet, the
choir is formant-filtered tone rather than a fake choir, and the field drum is
the one voice that is genuinely close to the real thing. A stylised score that
knows what it is beats a symphonic one that does not.

THE GRID. The cut's hinge is the full stop at 29.6 s, so a bar edge has to land
there exactly. Six-eight at dotted-quarter 88 gives a 1.3636 s bar; twenty bars
is 27.2727 s, so the music starts 2.327 s in and bar 20 falls on 29.600. That
is the same clock the trip-hop score used -- a 4/4 bar at 88 and two 6/8 bars at
dotted-88 are the same length -- so the existing cut's hinge survives the
change of metre on a finer grid.
"""
import math, os, sys
import numpy as np

SR      = 48000
DOTTED  = 88.0                      # dotted-quarters per minute
BAR     = 2 * 60.0 / DOTTED         # 1.363636 s -- six eighths
E8      = BAR / 6                   # one eighth
STOP_AT = 29.6                      # the cut's full stop, from trailer_build
T0      = STOP_AT - 20 * BAR        # 2.3273 -- see the docstring
LEN     = 90.2
HIDE_IMAGE = None                   # set by --hide/--egg; see hide_image()
FONT_PATH = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "data/fonts/DejaVuSans.ttf")

# Section starts, in bars from T0.  See the docstring for what each one is.
HALT, MARCH, STOP, AFTER, REBUILD, FULL, WIDE, LOGO, END = \
    0, 4, 20, 21, 29, 41, 53, 57, 63

def bar(n):
    return T0 + n * BAR

N = int(LEN * SR)

def hz(name):
    names = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4,
             "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9,
             "A#": 10, "Bb": 10, "B": 11}
    p, octv = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((names[p] + 12 * (octv - 4) - 9) / 12.0)

def blank():
    return np.zeros(N, dtype=np.float64)

def put(buf, t, sig, gain=1.0):
    i = int(t * SR)
    if i < 0:
        sig, i = sig[-i:], 0
    j = min(N, i + len(sig))
    if j > i:
        buf[i:j] += sig[:j - i] * gain

def env(n, a, d, s, r, sus=1.0):
    a, d, r = int(a * SR), int(d * SR), int(r * SR)
    s = max(0, n - a - d - r)
    return np.concatenate([
        np.linspace(0, 1, a, endpoint=False) if a else np.zeros(0),
        np.linspace(1, sus, d, endpoint=False) if d else np.zeros(0),
        np.full(s, sus),
        np.linspace(sus, 0, r) if r else np.zeros(0)])[:n]

# ------------------------------------------------------------------- filters
# FFT brick-walls with a fourth-order skirt.  Indistinguishable from a real
# filter on a bed, and they run in milliseconds instead of minutes -- which
# matters, because this file is re-run every time a section is re-voiced.

def lowpass(x, cutoff):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    return np.fft.irfft(X / (1.0 + (f / max(cutoff, 1.0)) ** 4), len(x))

def highpass(x, cutoff):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    r = (f / max(cutoff, 1.0)) ** 4
    return np.fft.irfft(X * r / (1.0 + r), len(x))

def bandpass(x, lo, hi):
    return highpass(lowpass(x, hi), lo)

def fftconv(x, ir):
    """Convolution in the frequency domain.  numpy's convolve is direct, and a
    five-second tail against ninety seconds of audio is 10^12 multiplies --
    minutes of waiting every time a section gets re-voiced.  This is two FFTs."""
    n = len(x) + len(ir) - 1
    m = 1 << (n - 1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x, m) * np.fft.rfft(ir, m), m)
    return y[:len(x)]

def resonate(x, f0, q=12.0, gain=1.0):
    """One resonant peak, for the choir's formants and the drum's head modes."""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    bw = max(f0 / q, 1.0)
    X *= 1.0 + gain / (1.0 + ((f - f0) / bw) ** 2)
    return np.fft.irfft(X, len(x))

def reverb(x, decay=2.6, mix=0.26, predelay=0.018):
    """Convolution with a synthesised tail.  The 0:29.6 stop is one reverb tail
    over silence, so the tail has to be long and real -- and FFT convolution
    makes a four-second one free, where the old Schroeder loop cost minutes."""
    n_ir = int(decay * 2.0 * SR)
    t = np.arange(n_ir) / SR
    rng = np.random.default_rng(1863)               # Gilmore's year, for luck
    ir = rng.normal(0, 1, n_ir) * np.exp(-t * (6.0 / decay))
    ir = lowpass(ir, 4200) * (1 - np.exp(-t * 180))  # soften the onset
    ir[:int(predelay * SR)] = 0.0
    ir /= np.sqrt(np.sum(ir ** 2))
    return x * (1 - mix) + fftconv(x, ir) * mix

# -------------------------------------------------------------------- voices

def brass(f, dur, bright=1.0, det=1.0):
    """The lead.  Not a trumpet: a harmonic stack with the two things that make
    brass read as brass -- a pitch that scoops up into the note over the first
    forty milliseconds, and a spectrum that opens as the note gets louder."""
    n = int(dur * SR)
    t = np.arange(n) / SR
    e = env(n, 0.022, 0.06, 0, min(0.10, dur * 0.3), sus=0.86)
    scoop = 1.0 - 0.017 * np.exp(-t * 26)            # ~30 cents, 40 ms
    out = np.zeros(n)
    for cents, g in ((0.0, 1.0), (+4.0, 0.5), (-5.0, 0.5)):
        fd = f * 2 ** (cents / 1200.0) * det
        ph = 2 * np.pi * np.cumsum(fd * scoop) / SR
        for k in range(1, 13):
            out += g * np.sin(k * ph) / k ** 1.12
    out /= 2.0
    # Spectrum tracks loudness: crossfade a dark and a bright copy by the
    # envelope, which is a two-FFT stand-in for a filter with an envelope on it.
    dark, brt = lowpass(out, 1500), lowpass(out, 5200 * bright)
    return (dark * (1 - e) + brt * e) * e

def lowstr(f, dur, level=1.0):
    """Low strings as texture.  Three detuned saws, rolled hard off, slow in."""
    n = int(dur * SR)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for cents, g in ((0.0, 1.0), (+7.0, 0.62), (-9.0, 0.62)):
        ph = (f * 2 ** (cents / 1200.0)) * t
        out += g * (2 * (ph % 1.0) - 1.0)
    out *= 1.0 + 0.03 * np.sin(2 * np.pi * 4.1 * t)
    a = min(0.18, dur * 0.25)
    return lowpass(out, 620) * env(n, a, 0, 0, min(0.35, dur * 0.4)) * level

def choir(names, dur, level=1.0):
    """Wordless voices.  Tone through two vowel formants plus a little breath;
    stylised, and it never carries the melody -- only the harmony under it."""
    n = int(dur * SR)
    t = np.arange(n) / SR
    out = np.zeros(n)
    rng = np.random.default_rng(len(names) * 97 + n % 1021)
    for nm in names:
        f = hz(nm)
        drift = 1.0 + 0.0016 * np.sin(2 * np.pi * rng.uniform(0.7, 1.4) * t
                                      + rng.uniform(0, 6.28))
        ph = 2 * np.pi * np.cumsum(f * drift) / SR
        out += np.sin(ph) + 0.45 * np.sin(2 * ph) + 0.22 * np.sin(3 * ph)
    out = resonate(resonate(out, 650, 9, 2.2), 1080, 11, 1.4)   # an "ah"
    breath = bandpass(rng.normal(0, 1, n), 900, 2600) * 0.06
    a = min(0.45, dur * 0.35)
    return lowpass(out / len(names) + breath, 3800) \
        * env(n, a, 0, 0, min(0.5, dur * 0.4)) * level

def field_drum(accent=1.0, dur=0.30):
    """A rope-tension field drum: wide noise, two head modes, snares buzzing
    just long enough to hear.  Brighter and louder together, as a struck head
    actually is -- an accent is not the same hit turned up."""
    n = int(dur * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(int(accent * 1000) & 0x7FFF)
    noise = bandpass(rng.normal(0, 1, n), 190, 2300 + 4000 * accent)
    head = (0.55 * np.sin(2 * np.pi * 192 * t) * np.exp(-t * 30)
            + 0.30 * np.sin(2 * np.pi * 338 * t) * np.exp(-t * 42))
    buzz = bandpass(rng.normal(0, 1, n), 3000, 9000) * np.exp(-t * 16) * 0.35
    s = (noise * (0.55 + 0.45 * accent) + head + buzz)
    return s * env(n, 0.0008, 0, 0, dur) * (0.45 + 0.55 * accent)

def bass_drum(dur=0.46, level=1.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = 96 * np.exp(-t * 26) + 47
    s = np.sin(2 * np.pi * np.cumsum(f) / SR)
    s += 0.18 * bandpass(np.random.default_rng(5).normal(0, 1, n), 60, 300)
    return lowpass(s, 240) * env(n, 0.001, 0, 0, dur) * level

def crash(dur=2.6, level=1.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(13)
    s = bandpass(rng.normal(0, 1, n), 1100, 9000) * np.exp(-t * (4.2 / dur))
    s += bandpass(rng.normal(0, 1, n), 380, 1100) * np.exp(-t * (6.0 / dur)) * .5
    return s * env(n, 0.002, 0, 0, dur * 0.5) * level

def roll(dur, to=1.0):
    """A crescendo roll.  Alternating hands at a thirty-second, amplitude
    swelling, with the hand-to-hand unevenness a real roll has."""
    n = int(dur * SR)
    out = np.zeros(n + SR // 2)
    step = E8 / 4
    rng = np.random.default_rng(29)
    k, t = 0, 0.0
    while t < dur:
        g = (t / dur) ** 1.7 * to
        hand = 1.0 if k % 2 == 0 else 0.72          # strong hand, weak hand
        h = field_drum(accent=0.25 + 0.35 * (t / dur), dur=0.10)
        i = int((t + rng.normal(0, 0.0016)) * SR)
        i = max(0, i)
        out[i:i + len(h)] += h[:len(out) - i] * g * hand * 0.5
        t += step
        k += 1
    return out

# --------------------------------------------------------------- the melody
# O'Neill's #468, transcribed from the ABC.  (name, eighths); None is a rest;
# ties are merged into one note.  Four four-bar phrases, 6/8, A minor.

A1 = [("E4", 2), ("A4", 1), ("A4", 2), ("B4", 1),
      ("C5", 2), ("B4", 1), ("C5", 2), ("A4", 1),
      ("G4", 5), ("E4", 1),
      ("G4", 5), ("A4", 1)]
A2 = [("E4", 2), ("A4", 1), ("A4", 2), ("B4", 1),
      ("C5", 2), ("B4", 1), ("C5", 2), ("D5", 1),
      ("E5", 5), ("C5", 1),
      ("E5", 5), ("C5", .5), ("D5", .5)]
B1 = [("E5", 2), ("E5", 1), ("E5", 1), ("D5", 1), ("C5", 1),
      ("D5", 2), ("D5", 1), ("D5", 2), ("B4", 1),
      ("C5", 2), ("C5", 1), ("C5", 1), ("B4", 1), ("A4", 1),
      ("B4", 2), ("B4", 1), ("B4", 2), ("C5", .5), ("D5", .5)]
# The refrain.  Those rests are where "Hur-rah!  Hur-rah!" goes, and they are
# the reason this tune works for a trailer: the melody leaves holes for drums.
B2 = [("E5", 2), (None, 1), ("D5", 2), (None, 1),
      ("C5", 2), (None, 1), ("B4", 2), (None, 1),
      ("E4", 1), ("A4", 1), ("A4", 1), ("A4", 2), ("G#4", 1),
      ("A4", 6)]
PHRASE = {"A1": A1, "A2": A2, "B1": B1, "B2": B2}
HOLES  = {"B2": [(0, 2), (0, 5), (1, 2), (1, 5)]}   # (bar in phrase, eighth)

# Harmony, derived from the melody rather than copied from any setting: the
# tune is A aeolian with the seventh raised only at the cadence, so the
# dominant is minor everywhere except bar 15, where the melody's G# forces E.
CHORDS = {
    "A1": [["Am"], ["Am"], ["G"], ["G"]],
    "A2": [["Am"], ["Am"], ["C"], ["C"]],
    "B1": [["Am"], ["G"], ["Am"], ["Em"]],
    "B2": [["Am", "G"], ["C", "Em"], ["E"], ["Am"]],
}
BASS  = {"Am": "A2", "G": "G2", "C": "C3", "Em": "E2", "E": "E2"}
VOICE = {"Am": ("A4", "C5", "E5"), "G": ("G4", "B4", "D5"),
         "C": ("G4", "C5", "E5"), "Em": ("G4", "B4", "E5"),
         "E": ("G#4", "B4", "E5")}

# Field-drum patterns: (eighth within the bar, accent).  Six-eight marches
# accent the two dotted-quarters, so 0 and 3 carry and the rest fill.
P_LIGHT = [(0, .80), (3, .62)]
P_MARCH = [(0, 1.0), (1, .40), (2, .46), (3, .88), (4, .40), (5, .46)]
P_DRIVE = [(0, 1.0), (.5, .28), (1, .42), (1.5, .28), (2, .48),
           (3, .92), (3.5, .28), (4, .42), (4.5, .28), (5, .48)]
P_RIM   = [(0, .34), (2, .22), (3, .30), (5, .22)]

# ----------------------------------------------------------------- rendering

class Stems:
    def __init__(self):
        self.lead = blank(); self.str_ = blank()
        self.cho = blank();  self.drm = blank()

    def all(self):
        return (self.lead, self.str_, self.cho, self.drm)

def phrase(S, at, name, *, lead=0.0, octave=0.0, strings=0.0, choir_=0.0,
           pat=None, bd=0.0, hurrah=0.0, aug=1):
    """Render one four-bar phrase at bar index `at`.  Every voice is a level,
    so a section is described by which voices are up and how far -- which is
    what makes eight differently-scored sections cheap to write."""
    notes, span = PHRASE[name], 4 * aug
    if lead or octave:
        t = bar(at)
        for nm, d in notes:
            dur = d * E8 * aug
            if nm:
                g = min(1.0, 0.25 + 0.75 * d / 2.0)      # long notes sing
                if lead:
                    put(S.lead, t, brass(hz(nm), dur * 0.97, bright=1.0),
                        lead * g)
                if octave:
                    put(S.lead, t, brass(hz(nm) * 2, dur * 0.97, bright=0.8),
                        octave * g * 0.5)
            t += dur
    for b, chords in enumerate(CHORDS[name]):
        step = BAR * aug / len(chords)
        for c, ch in enumerate(chords):
            t = bar(at + b * aug) + c * step
            if strings:
                put(S.str_, t, lowstr(hz(BASS[ch]), step * 1.04), strings)
            if choir_:
                put(S.cho, t, choir(VOICE[ch], step * 1.06), choir_)
    if pat:
        for b in range(span):
            for e, acc in pat:
                put(S.drm, bar(at + b) + e * E8, field_drum(acc), 0.52)
            if bd:
                for e in (0, 3):
                    put(S.drm, bar(at + b) + e * E8, bass_drum(), bd)
    if hurrah and name in HOLES:
        for b, e in HOLES[name]:
            t = bar(at + b * aug) + e * E8 * aug
            put(S.drm, t, field_drum(1.0), 0.70 * hurrah)
            put(S.drm, t, bass_drum(level=1.15), hurrah)
    return span

def gate(n_len, *spans):
    """Piecewise level with ten-millisecond ramps at the edges, so a voice
    entering or leaving never clicks."""
    g = np.zeros(n_len)
    for t0, t1, v in spans:
        i, j = int(t0 * SR), min(n_len, int(t1 * SR))
        if j <= i:
            continue
        g[i:j] = np.maximum(g[i:j], v)
        k = int(0.010 * SR)
        g[i:i + k] = np.maximum(g[i:i + k], np.linspace(0, v, k)[:j - i])
        g[max(i, j - k):j] = np.minimum(g[max(i, j - k):j],
                                       np.linspace(v, 0, min(k, j - i)))
    return g

def ramp(t0, t1, v0, v1, cos=False):
    """Raised-cosine when `cos`, which is what a fade wants: a linear one is
    audible as a corner at each end."""
    g = np.full(N, v1)
    i, j = int(t0 * SR), int(t1 * SR)
    g[:i] = v0
    k = np.linspace(0, 1, max(j - i, 1))
    if cos:
        k = 0.5 - 0.5 * np.cos(np.pi * k)
    g[i:j] = v0 + (v1 - v0) * k
    return g


# ------------------------------------------------------------------- the egg

EGG_PNG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "docs/trailer/assets/egg-spectrogram.png")

def make_egg(path=None):
    """Build the picture that gets painted into the spectrogram.

    Two lines: the wordmark, taken from the real asset's ALPHA channel (the
    gold fill would quantise to one flat grey and the letters would fill in),
    and a greeting underneath for whoever went looking. A spectrogram's
    vertical axis is frequency, so "underneath" means the second line lives in
    the lower part of the band -- it needs to be the shorter, fatter line or it
    loses legibility to the resample.
    """
    from PIL import Image, ImageDraw, ImageFont
    path = path or EGG_PNG
    mark = Image.open(os.path.join(os.path.dirname(path),
                                   "wordmark-gold.png")).convert("RGBA")
    top = mark.getchannel("A")
    W = top.width
    top = top.resize((W, int(top.height * 1.0)), Image.LANCZOS)
    font_px = int(top.height * 0.74)
    f = ImageFont.truetype(FONT_PATH, font_px)
    msg = "hi data miners!"
    probe = ImageDraw.Draw(Image.new("L", (8, 8)))
    tw = probe.textlength(msg, f)
    gap = int(top.height * 0.34)
    im = Image.new("L", (W, top.height + gap + font_px + 20), 0)
    im.paste(top, (0, 0))
    d = ImageDraw.Draw(im)
    d.text(((W - tw) / 2, top.height + gap), msg, font=f, fill=255)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    im.save(path)
    return path



def hide_image(x, img_path, t0, t1, f_lo=7000.0, f_hi=12800.0, level=0.012,
               rows=200):
    """Paint a picture into the spectrogram, between t0 and t1.

    A BANK OF OSCILLATORS, not an inverse STFT. One continuous sinusoid per
    frequency row, amplitude-modulated by that row of the image as time runs
    left to right. Continuous phase is the whole reason: an inverse STFT
    reconstructed from magnitudes alone has a different phase in every frame,
    and the frame boundaries click -- which is audible exactly where this is
    trying not to be.

    WHERE IT GOES, AND WHY NOT HIGHER. The obvious hiding place is 16-20 kHz,
    where almost nobody can hear it. It does not survive: YouTube and Steam
    re-encode to AAC/Opus, which low-pass around 15-16 kHz and spend no bits on
    quiet content near the limit, so an ultrasonic picture arrives as an empty
    band. 7-12.5 kHz survives a 192k AAC and still sits under the cymbals, so
    the default window is the LOGO section where the crashes mask it.

    This is additive: it IS sound, and the honest claim is "masked", not
    "inaudible". At the default level it is about 30 dB under the music in its
    own band.
    """
    from PIL import Image
    im = Image.open(img_path).convert("L")
    i0, i1 = int(t0 * SR), min(len(x), int(t1 * SR))
    n = i1 - i0
    if n <= 0:
        return x
    cols = max(2, min(1400, n // 64))
    im = im.resize((cols, rows), Image.LANCZOS)
    a = np.asarray(im, dtype=np.float64) / 255.0
    a = a[::-1]                      # image row 0 is the TOP = highest bin
    a = a ** 1.6                     # bias to the bright strokes, drop the haze
    t = np.arange(n) / SR
    # Each row's gain over time, smoothed so a hard column edge does not
    # broadband-splatter into its neighbours and smear the letters.
    k = max(1, int(0.012 * SR))
    win = np.hanning(k); win /= win.sum()
    seg = np.zeros(n)
    rng = np.random.default_rng(468)                 # O'Neill's tune number
    freqs = np.linspace(f_lo, f_hi, rows)
    for r in range(rows):
        g = np.interp(np.linspace(0, cols - 1, n), np.arange(cols), a[r])
        if g.max() < 0.02:
            continue
        g = np.convolve(g, win, mode="same")
        seg += g * np.sin(2 * np.pi * freqs[r] * t + rng.uniform(0, 6.283))
    seg *= level / max(np.max(np.abs(seg)), 1e-9)
    # Ramp the ends: the picture starting mid-note is fine, a step is not.
    e = int(0.08 * SR)
    seg[:e] *= np.linspace(0, 1, e)
    seg[-e:] *= np.linspace(1, 0, e)
    out = x.copy()
    out[i0:i1] += seg
    return out

def score():
    S = Stems()

    # HALT -- the drum alone.  No tune: the viewer gets the key and the pulse
    # and nothing to hum, so the melody's arrival four bars later is an event.
    put(S.drm, bar(HALT), roll(3.0 * BAR / 1.0, to=0.9), 0.44)
    for b in range(4):
        put(S.drm, bar(HALT + b), bass_drum(level=0.9), 0.42)
    for e, acc in ((0, 1.0), (3, 0.8)):
        put(S.drm, bar(HALT + 3) + e * E8, field_drum(acc), 0.6)
    put(S.str_, bar(HALT), lowstr(hz("A1"), 4 * BAR, 0.8), 0.55)

    # MARCH IN -- the whole strain, one line, almost nothing under it.  Strings
    # join at A2, the drum at B1, so each phrase adds exactly one thing.
    at = MARCH
    at += phrase(S, at, "A1", lead=0.92, pat=P_LIGHT)
    at += phrase(S, at, "A2", lead=0.95, strings=0.50, pat=P_LIGHT)
    at += phrase(S, at, "B1", lead=1.00, strings=0.62, pat=P_MARCH, bd=0.62)
    at += phrase(S, at, "B2", lead=1.00, strings=0.66, choir_=0.26,
                 pat=P_MARCH, bd=0.70, hurrah=0.85)
    assert at == STOP, at

    # STOP -- one bar of nothing but the tail of the bar before it.
    put(S.drm, bar(STOP), crash(2.2, 0.5), 0.34)

    # AFTERMATH -- the same two phrases, choir only, no drum, no lead.  The
    # lament inside the parade.  Augmented x2, so eight bars carry four.
    at = AFTER
    at += phrase(S, at, "A1", choir_=0.62, strings=0.44, aug=2)
    assert at == REBUILD, at

    # REBUILD -- the counterpoint.  Low strings take the B strain stretched to
    # twice its length while the lead plays the A strain over it, then the B
    # strain arrives at tempo and the drum comes back with it.
    phrase(S, REBUILD, "B1", strings=0.70, choir_=0.30, aug=2)
    phrase(S, REBUILD, "A1", lead=0.70, pat=P_RIM)
    phrase(S, REBUILD + 4, "A1", lead=0.80, pat=P_LIGHT)
    at = REBUILD + 8
    at += phrase(S, at, "B1", lead=0.92, strings=0.62, pat=P_MARCH, bd=0.66)
    assert at == FULL, at

    # FULL -- everything, doubled at the octave.
    at = FULL
    at += phrase(S, at, "A1", lead=1.0, octave=0.55, strings=0.72,
                 choir_=0.34, pat=P_DRIVE, bd=0.80)
    at += phrase(S, at, "A2", lead=1.0, octave=0.60, strings=0.74,
                 choir_=0.38, pat=P_DRIVE, bd=0.82)
    at += phrase(S, at, "B1", lead=1.0, octave=0.62, strings=0.78,
                 choir_=0.40, pat=P_DRIVE, bd=0.86)
    assert at == WIDE, at
    put(S.drm, bar(FULL), crash(2.8, 0.62), 0.34)

    # WIDE -- lead over choir, drum back to a rim pattern.  Room to breathe
    # before the end, and the one place the tune is allowed to be pretty.
    at = WIDE
    at += phrase(S, at, "A1", lead=0.88, strings=0.56, choir_=0.52, pat=P_RIM)
    assert at == LOGO, at

    # LOGO -- the refrain's hurrah bars at full weight, then the cadence held
    # under the wordmark.  The G# in bar 15 is the only raised seventh in the
    # tune, and it is what makes the last chord land.
    phrase(S, LOGO, "B2", lead=1.0, octave=0.68, strings=0.80, choir_=0.46,
           pat=P_DRIVE, bd=0.90, hurrah=1.0)
    put(S.drm, bar(LOGO), crash(3.0, 0.7), 0.36)
    t_end = bar(LOGO + 4)
    hold = (END - LOGO - 4) * BAR
    put(S.cho, t_end, choir(("A4", "C5", "E5", "A5"), hold + 1.6), 0.70)
    put(S.str_, t_end, lowstr(hz("A1"), hold + 1.8, 0.9), 0.62)
    put(S.str_, t_end, lowstr(hz("A2"), hold + 1.8, 0.8), 0.48)
    put(S.drm, t_end, bass_drum(0.9, 1.2), 0.9)
    put(S.drm, t_end, crash(3.4, 0.8), 0.40)
    return S

def mix():
    S = score()
    lead, str_, cho, drm = S.all()

    # Carve a little room for the lead where everything is playing at once,
    # rather than turning the beds down for the whole piece.
    duck = 1.0 - 0.18 * gate(N, (bar(FULL), bar(WIDE), 1.0),
                             (bar(LOGO), bar(END), 1.0))
    dry = (lead * 0.46 + str_ * 0.40 * duck
           + cho * 0.30 * duck + drm * 0.44)

    wet = reverb(dry, decay=2.8, mix=0.24)
    wet *= (ramp(0, T0 + BAR * 0.5, 0.0, 1.0, cos=True)
            * ramp(bar(END) + 0.3, LEN, 1.0, 0.0, cos=True))
    # Take the top off the whole mix.  Synthesised noise runs flat to Nyquist
    # and a trailer is watched on phone speakers and laptop lids.
    wet = lowpass(wet, 13000)

    # The easter egg, if one was asked for.  After the roll-off on purpose.
    if HIDE_IMAGE:
        # AFTERMATH, not LOGO.  The loudest section is the best masking and the
        # WORST canvas: the crashes fill 7-13 kHz with noise and the letters
        # cannot be read out of it.  AFTERMATH has no drum at all by design and
        # its choir sits below 1 kHz, so the band is empty, the picture is
        # legible -- and there is still music playing over it.
        wet = hide_image(wet, HIDE_IMAGE, bar(AFTER) + 0.35, bar(REBUILD) - 0.35)

    # DC blocker.  The saws and the FFT filters both leave an offset, and an
    # offset is headroom spent on nothing plus a click at the fade edges.
    wet = highpass(wet, 22)
    wet /= np.max(np.abs(wet)) / 0.89

    # Width: the choir and the drum's buzz off centre, the lead dead ahead.
    sp = 0.07
    left = wet + sp * np.roll(cho, 320) * 0.30 + sp * np.roll(drm, 90) * 0.12
    right = wet - sp * np.roll(cho, -320) * 0.30 - sp * np.roll(drm, -90) * 0.12
    st = np.stack([left, right], axis=1)
    st /= max(np.max(np.abs(st)) / 0.95, 1.0)
    return st

def write_wav(path, data):
    import wave
    d = (np.clip(data, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(d.tobytes())

CUES = [("HALT", HALT, "field drum alone, no tune"),
        ("MARCH IN", MARCH, "the tune arrives, solo"),
        ("STOP", STOP, "cut hinge -- everything out"),
        ("AFTERMATH", AFTER, "choir only, the lament"),
        ("REBUILD", REBUILD, "B strain augmented under A"),
        ("FULL", FULL, "everything, octave doubled"),
        ("WIDE", WIDE, "lead over choir, drum back"),
        ("LOGO", LOGO, "hurrah bars, then the cadence"),
        ("END", END, "ring-out under the wordmark")]

def print_cues():
    print("%-10s %5s %8s   %s" % ("section", "bar", "time", "what"))
    for nm, b, what in CUES:
        print("%-10s %5d %7.2fs   %s" % (nm, b, bar(b), what))
    print("\ndotted-quarter %.0f   bar %.4fs   %d bars   tail to %.1fs"
          % (DOTTED, BAR, END, LEN))

if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--hide" in argv:
        i = argv.index("--hide")
        globals()["HIDE_IMAGE"] = argv[i + 1]
        del argv[i:i + 2]
    if "--egg" in argv:
        argv.remove("--egg")
        globals()["HIDE_IMAGE"] = make_egg()
    args = [a for a in argv if a != "--cues"]
    if "--cues" in argv:
        print_cues()
        if not args:
            raise SystemExit(0)
    out = args[0] if args else "docs/trailer/assets/march.wav"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    write_wav(out, mix())
    print("%s  %.1f s  %.1f MB" % (out, LEN, os.path.getsize(out) / 1e6))
    print_cues()
