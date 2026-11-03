"""Measure the pitch of every fetched single-note recording (mono, 44.1 kHz) and write
WORK/samples/catalog.json.   usage: python make_catalog.py WORK"""
import argparse, glob, hashlib, json, os, re, sys
from math import gcd
import numpy as np, soundfile as sf
from scipy.signal import resample_poly

FS = 44100
NOTE = {'C': 0, 'C#': 1, 'D': 2, 'D#': 3, 'E': 4, 'F': 5, 'F#': 6, 'G': 7, 'G#': 8, 'A': 9, 'A#': 10, 'B': 11}
# VSCO's woodwind and string sets name middle C "C3"; Salamander and the VSCO harp use C4.
YAMAHA_OCTAVES = ('clarinet', 'flute', 'bassoon', 'oboe', 'violins', 'cellos')


def name_to_midi(name):
    m = re.match(r'^([A-G]#?)(-?\d)$', name)
    return 12 * (int(m.group(2)) + 1) + NOTE[m.group(1)]


def load_mono(path):
    x, sr = sf.read(path, always_2d=True)
    x = x.mean(axis=1)
    if sr != FS:
        g = gcd(FS, sr)
        x = resample_poly(x, FS // g, sr // g)
    return x.astype(np.float64)


def trim(x, max_len_s):
    a = np.abs(x)
    i0 = max(0, int(np.argmax(a > a.max() * 10 ** (-40 / 20))) - int(0.005 * FS))
    x = x[i0:i0 + int(max_len_s * FS)].copy()
    n = int(0.05 * FS)
    x[-n:] *= np.linspace(1, 0, n)
    return x


def measure_f0(x, nominal_midi):
    """Harmonic-sum search within +-120 cents of the nominal pitch, refined on partials 1-2."""
    seg = x[int(0.15 * FS): int(1.4 * FS)]
    N = 1 << 17
    S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N))
    logS = np.log(S + 1e-9)
    best = None
    for c in np.arange(-120, 121, 1.0):
        f0 = 440 * 2 ** ((nominal_midi - 69 + c / 100) / 12)
        score = 0
        for h in range(1, 9):
            k = int(round(h * f0 * N / FS))
            if k >= len(S) - 3:
                break
            score += S[max(0, k - 2): k + 3].max() / h
        if best is None or score > best[0]:
            best = (score, f0)
    f0 = best[1]
    ests = []
    for h in range(1, 3):
        k = h * f0 * N / FS
        lo, hi = int(k * 0.985), int(k * 1.015) + 1
        i = lo + int(np.argmax(S[lo:hi]))
        a, b, c_ = logS[i - 1], logS[i], logS[i + 1]
        d = 0.5 * (a - c_) / (a - 2 * b + c_ + 1e-12)
        ests.append(((i + d) * FS / N / h, S[i]))
    fs_, ws_ = np.array(ests).T
    f0 = float(np.sum(fs_ * ws_) / np.sum(ws_))
    return 69 + 12 * np.log2(f0 / 440)


def main(work, piano_velocity=10):
    root = f'{work}/data'
    specs = [('piano', f'{root}/sal/Samples/*v{piano_velocity}.flac', rf'([A-G]#?\d)v{piano_velocity}'),
             ('clarinet', f'{root}/vsco/Woodwinds/Clarinet/susLong/*.wav', r'susLong_([A-G]#?\d)_'),
             ('flute', f'{root}/vsco/Woodwinds/Flute/susNV/*.wav', r'susNV_([A-G]#?\d)_'),
             ('bassoon', f'{root}/vsco/Woodwinds/Bassoon/sus/*.wav', r'PSBassoon_([A-G]#?\d)_'),
             ('oboe', f'{root}/vsco/Woodwinds/Oboe/Sus/*.wav', r'Oboe_Sus_([A-G]#?\d)_'),
             ('violins', f'{root}/vsco/Strings/Violin Section/susVib/*.wav', r'susVib_([A-G]#?\d)_'),
             ('cellos', f'{root}/vsco/Strings/Cello Section/susvib/*.wav', r'susvib_([A-G]#?\d)_'),
             ('harp', f'{root}/vsco/Strings/Harp/*.wav', r'KSHarp_([A-G]#?\d)_'),
             ('organ', f'{root}/vsco/Keys/Organ/Loud/*.wav', r'Man3Open_(\d+)')]
    os.makedirs(f'{work}/samples', exist_ok=True)
    items = []
    for inst, pattern, rx in specs:
        for p in sorted(glob.glob(pattern)):
            m = re.search(rx, os.path.basename(p))
            if not m:
                continue
            tag = m.group(1)
            nominal = 35 + int(tag) if inst == 'organ' else name_to_midi(tag) + (12 if inst in YAMAHA_OCTAVES else 0)
            x = trim(load_mono(p), 8.0 if inst == 'organ' else 6.0)
            midi = measure_f0(x, nominal)
            rel = f'samples/{inst}_{tag.replace("#", "s")}.wav'
            sf.write(f'{work}/{rel}', (x / max(1e-9, np.abs(x).max()) * 0.5).astype(np.float32), FS)
            items.append(dict(inst=inst, tag=tag, nominal=nominal, midi=round(float(midi), 3), path=rel,
                              source=os.path.relpath(p, work),
                              source_sha256=hashlib.sha256(open(p, 'rb').read()).hexdigest()))
            print(f'{inst:9s} {tag:4s} nominal {nominal:3d} measured {midi:7.2f}')
    json.dump(items, open(f'{work}/samples/catalog.json', 'w'), indent=1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work')
    parser.add_argument('--piano-velocity', type=int, default=10)
    args = parser.parse_args()
    main(args.work, args.piano_velocity)
