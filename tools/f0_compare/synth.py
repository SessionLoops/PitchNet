"""Synthetic sung-vocal generator with exact ground-truth F0 (source-filter model).
Glottal pulse train + formant resonators + vibrato/portamento/jitter/shimmer/breath noise."""
import numpy as np
from scipy.signal import lfilter, fftconvolve

SR = 44100
VOWELS = {  # formants (Hz) F1..F4, bandwidths
    'a': ([800,1200,2800,3500],[80,90,120,130]),
    'e': ([500,1800,2600,3400],[70,90,120,130]),
    'i': ([300,2300,3000,3600],[60,90,120,130]),
    'o': ([500, 900,2800,3500],[70,80,120,130]),
    'u': ([320, 800,2600,3400],[60,80,120,130]),
}

def midi2hz(m): return 440.0 * 2 ** ((np.asarray(m) - 69) / 12)

def f0_track(rng, lo, hi, dur, vib_depth_cents, scoop):
    """Return per-sample f0 (0 = unvoiced) built from a note sequence."""
    n = int(dur * SR); f0 = np.zeros(n); t = 0.0; cur = rng.uniform(lo, hi)
    pos = 0
    while pos < n:
        note_len = rng.uniform(0.25, 1.1)
        gap = rng.uniform(0.04, 0.18) if rng.random() < 0.7 else 0.0   # consonant / breath
        step = rng.choice([-5,-3,-2,-1,0,1,2,3,5,7,-7,12,-12], p=None)
        cur = np.clip(cur + step, lo, hi)
        L = int(note_len * SR); seg = np.arange(L) / SR
        base = np.full(L, float(cur))
        if scoop:  # glide into note from below (portamento)
            g = rng.uniform(0.05, 0.25); s = np.clip(seg / g, 0, 1)
            base = cur - rng.uniform(1, 4) * (1 - s) ** 2 * rng.choice([1, -1])
        vib_rate = rng.uniform(4.8, 6.5); onset = rng.uniform(0.15, 0.4)
        vib = (vib_depth_cents / 100) * np.sin(2*np.pi*vib_rate*seg) * np.clip((seg-onset)/0.3, 0, 1)
        drift = np.cumsum(rng.normal(0, 0.0004, L))  # slow random pitch wander (semitones)
        fm = midi2hz(base + vib + drift)
        end = min(n, pos + L)
        f0[pos:end] = fm[:end-pos]
        pos = end
        g = int(gap * SR); f0[pos:min(n, pos+g)] = 0; pos += g
    return f0

def render(f0, rng, vowel_seq, breath=0.03, jitter=0.004):
    n = len(f0); voiced = f0 > 0
    # phase accumulation with jitter
    f = f0 * (1 + jitter * np.convolve(rng.normal(size=n), np.ones(200)/200, 'same') * 15)
    ph = np.cumsum(f / SR)
    # Rosenberg-ish glottal pulse (derivative form gives -12dB/oct source + radiation)
    x = ph % 1.0; Tp, Tn = 0.4, 0.16
    g = np.where(x < Tp, 0.5*(1-np.cos(np.pi*x/Tp)),
        np.where(x < Tp+Tn, np.cos(np.pi*(x-Tp)/(2*Tn)), 0.0))
    src = np.diff(g, prepend=0) * SR / 2000.0
    src *= voiced
    src *= (1 + 0.05 * np.convolve(rng.normal(size=n), np.ones(400)/400, 'same') * 20)  # shimmer
    src += breath * rng.normal(size=n) * np.where(voiced, 1.0, 0.15)  # aspiration / breath
    # time-varying vowel: switch formants in blocks, crossfade
    out = np.zeros(n); blk = int(0.35 * SR)
    for b0 in range(0, n, blk):
        v = vowel_seq[(b0 // blk) % len(vowel_seq)]
        F, B = VOWELS[v]; y = src[max(0,b0-blk//2):min(n,b0+blk+blk//2)].copy()
        for fc, bw in zip(F, B):
            r = np.exp(-np.pi * bw / SR); th = 2*np.pi*fc/SR
            y = lfilter([1-r], [1, -2*r*np.cos(th), r*r], y)
        w = np.ones(len(y)); k = blk//2
        w[:k] = np.linspace(0,1,k) if b0>0 else 1; w[-k:] = np.linspace(1,0,k) if b0+blk<n else 1
        out[max(0,b0-blk//2):max(0,b0-blk//2)+len(y)] += y * w
    amp = 0.6 + 0.4 * np.sin(2*np.pi*0.3*np.arange(n)/SR + rng.uniform(0, 6))
    out *= amp
    return out / (np.abs(out).max() + 1e-9) * 0.7

def reverb_ir(rng, rt60=1.4):
    n = int(rt60 * SR); t = np.arange(n) / SR
    ir = rng.normal(size=n) * 10 ** (-3 * t / rt60); ir[0] = 1.0
    return ir / np.abs(ir).max() * 0.5

def add_noise(x, snr_db, rng, kind='pink'):
    n = rng.normal(size=len(x))
    if kind == 'pink':
        n = lfilter([1], [1, -0.97], n)
    p = np.mean(x[np.abs(x) > 1e-3] ** 2)
    n *= np.sqrt(p / (10 ** (snr_db/10)) / np.mean(n ** 2))
    return x + n

VOICES = {  # name: (lo midi, hi midi, vibrato depth cents)
    'bass':    (40, 57, 35),
    'baritone':(45, 64, 40),
    'alto':    (55, 74, 45),
    'soprano': (62, 86, 55),
}

def make_set(seed=0, dur=14.0, per_voice=2):
    rng = np.random.default_rng(seed); items = []
    for name, (lo, hi, vd) in VOICES.items():
        for k in range(per_voice):
            f0 = f0_track(rng, lo, hi, dur, vd, scoop=bool(k % 2))
            vs = list(rng.choice(list('aeiou'), 6))
            x = render(f0, rng, vs)
            items.append(dict(name=f"{name}{k}", audio=x, f0=f0))
    return items, rng
