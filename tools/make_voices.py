#!/usr/bin/env python3
"""Synthesise the blips a speaker makes while talking.

    python3 tools/make_voices.py data/audio/sfx/voice

WHAT THIS IS. Dialogue in this game is typed, not spoken, and a character
that types in silence reads as a caption rather than as a person. The fix the
genre settled on long ago is a short blip per letter, pitched per character --
Undertale, Animal Crossing, Phoenix Wright all do it, and it carries an
astonishing amount of personality for a few kilobytes.

WHY GENERATED. Six takes per character keeps a line from machine-gunning one
sample, and the engine already picks between `<name>_1..N` on its own. Six
recordings per character is a recording session; six synthesised ones is this
file, and they can be replaced one at a time by real ones later because they
are only WAVs in a folder.

THE SOUND. Each blip is a short, quiet, lowpassed tone with a burst of noise
at its attack -- see the note on Voice for why the first attempt was annoying
and what changed. What separates one character from another is pitch, how
breathy the attack is, and how fast it dies: a low muffled blip reads as a
heavy man on a bad line, a light quick one as somebody younger.
"""
import math
import os
import struct
import sys
import wave

RATE = 22050


class Voice:
    """One character's blip family.

    WHAT MAKES A BLIP ANNOYING, learned the hard way: the first set were 84 ms
    long and fired every 95 ms, so they overlapped into one continuous drone
    rather than a chatter, and at peak 0.32 of a nearly pure tone they were
    both loud and beepy. So these are SHORT, QUIET and DIRTY:

      - 32-46 ms, well under the gap between them, so each one ends before the
        next begins and the ear hears a rhythm instead of a note
      - peak around 0.18, which sits under the music rather than over it
      - a noise transient in the first few milliseconds. Speech is mostly
        consonants -- transients -- and a tone with no attack noise reads as a
        machine. This is the single biggest difference.
      - a gentle lowpass, because what makes a small speaker unpleasant is the
        top end, and a radio link has none of it anyway
    """

    def __init__(self, name, pitch, noise, decay, tilt, dur=0.040):
        self.name = name
        self.pitch = pitch      # Hz -- kept low; high blips are what grate
        self.noise = noise      # 0 clean .. 1 breathy, at the attack
        self.decay = decay      # fraction of dur the body lasts
        self.tilt = tilt        # lowpass corner, Hz
        self.dur = dur

    def take(self, i, of):
        """Blip `i`: the same voice, a step along its own small scale."""
        steps = [0, 2, 3, 5, 3, 1][i % 6]
        f0 = self.pitch * (2.0 ** (steps / 12.0))
        n = int(RATE * self.dur)
        # A deterministic noise source: the same six takes on every machine.
        seed = 0x9E37 + i * 977 + len(self.name) * 31
        out = []
        y = 0.0
        a = 1.0 - math.exp(-2.0 * math.pi * self.tilt / RATE)
        for s in range(n):
            t = s / RATE
            env = math.exp(-t / (self.dur * self.decay))
            if t < 0.0015:
                env *= t / 0.0015                    # a click, not a pop
            ph = 2.0 * math.pi * f0 * t
            v = math.sin(ph) + 0.22 * math.sin(ph * 2.0)
            # The consonant: noise only at the very start, gone within 8 ms.
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            r = (seed / 0x3FFFFFFF) - 1.0
            v += self.noise * r * math.exp(-t / 0.008) * 1.6
            y += a * (v - y)                          # one-pole lowpass
            out.append(y * env * 0.18)
        return out


VOICES = [
    #     name            pitch noise decay tilt   dur
    Voice("advisor",        150, 0.55, 0.30, 2600, 0.042),  # low, dry, unhurried
    Voice("officer",        190, 0.75, 0.22, 3400, 0.034),  # clipped, hard, breathy
    Voice("unknown",        104, 0.85, 0.34, 1500, 0.046),  # muffled and low: a voice
                                                            # through a filter, unplaceable
    Voice("analyst",        232, 0.45, 0.26, 3000, 0.036),  # lighter and quicker
    Voice("quartermaster",  128, 0.60, 0.36, 2200, 0.046),  # heavy and slow
    Voice("narrator",       176, 0.40, 0.28, 2800, 0.038),  # neutral, for anyone unnamed
]


def write_wav(path, samples):
    with wave.open(path, "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(
            struct.pack("<h", max(-32767, min(32767, int(v * 32767)))) for v in samples))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "data/audio/sfx/voice"
    os.makedirs(outdir, exist_ok=True)
    takes = 6
    for v in VOICES:
        for i in range(takes):
            # `voice_<name>_<n>` -- the engine strips the trailing _<n> and
            # picks between the takes itself, so the game asks for
            # playSfx("voice_<name>") and never has to know how many there are.
            path = os.path.join(outdir, f"voice_{v.name}_{i + 1}.wav")
            write_wav(path, v.take(i, takes))
        print(f"  voice_{v.name}: {takes} takes")
    print(f"wrote {len(VOICES) * takes} files to {outdir}")


if __name__ == "__main__":
    main()
