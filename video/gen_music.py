"""A short tactical beat for the BlockBreach video, synthesised (no third-party music): 100 bpm, a low pulse, a kick,
closed hats, a minor-key bass drone and a few square-wave "blocky" plucks (the Minecraft touch). Writes video/music.wav.

  uv run --with numpy python video/gen_music.py [seconds=48]
"""
import sys
import wave
from pathlib import Path

import numpy as np

SR = 48000
BPM = 100.0
OUT = Path(__file__).resolve().parent / "music.wav"


def env(n, attack, decay):
    t = np.arange(n) / SR
    return np.minimum(1.0, t / max(attack, 1e-4)) * np.exp(-t / decay)


def kick(n):
    t = np.arange(n) / SR
    f = 50 + 90 * np.exp(-t * 30)
    return np.sin(2 * np.pi * np.cumsum(f) / SR) * env(n, 0.002, 0.18)


def hat(n, rng):
    noise = rng.standard_normal(n)
    noise = np.diff(noise, prepend=0)  # brighter
    return noise * env(n, 0.001, 0.03) * 0.25


def pluck(freq, n):
    t = np.arange(n) / SR
    sq = np.sign(np.sin(2 * np.pi * freq * t))
    return sq * env(n, 0.002, 0.12) * 0.18


def main(seconds):
    rng = np.random.default_rng(3)
    n = int(seconds * SR)
    mix = np.zeros(n)
    beat = 60.0 / BPM
    steps = int(seconds / beat * 4)
    root = 55.0  # A1
    # bass drone: A minor, a slow filter-ish sweep by mixing harmonics
    t = np.arange(n) / SR
    sweep = 0.5 + 0.5 * np.sin(2 * np.pi * t / (beat * 16))
    drone = (np.sin(2 * np.pi * root * t) + 0.35 * sweep * np.sin(2 * np.pi * root * 2 * t) + 0.15 * sweep * np.sin(2 * np.pi * root * 3 * t))
    mix += drone * 0.22
    notes = [220.0, 261.63, 329.63, 293.66, 220.0, 196.0, 261.63, 246.94]
    for s in range(steps):
        at = int(s * beat / 4 * SR)
        bar_step = s % 16
        if bar_step in (0, 6, 8, 11):
            k = kick(min(int(0.4 * SR), n - at))
            mix[at:at + len(k)] += k * 0.9
        if s % 2 == 1:
            h = hat(min(int(0.08 * SR), n - at), rng)
            mix[at:at + len(h)] += h
        if bar_step in (4, 12):
            # a snare-ish noise burst on 2 and 4
            sn = rng.standard_normal(min(int(0.2 * SR), n - at)) * env(min(int(0.2 * SR), n - at), 0.001, 0.07) * 0.35
            mix[at:at + len(sn)] += sn
        if s >= 32 and bar_step in (2, 10, 14) and (s // 16) % 2 == 1:
            p = pluck(notes[(s // 4) % len(notes)], min(int(0.25 * SR), n - at))
            mix[at:at + len(p)] += p
    # fade in/out and normalise
    fade = int(1.0 * SR)
    mix[:fade] *= np.linspace(0, 1, fade)
    mix[-fade:] *= np.linspace(1, 0, fade)
    mix /= np.max(np.abs(mix)) * 1.12
    stereo = np.stack([mix, np.roll(mix, int(0.004 * SR))], axis=1)
    data = (stereo * 32767).astype("<i2").tobytes()
    with wave.open(str(OUT), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data)
    print("->", OUT, f"{seconds} s")


if __name__ == "__main__":
    main(float(sys.argv[1]) if len(sys.argv) > 1 else 48.0)
