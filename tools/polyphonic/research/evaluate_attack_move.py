#!/usr/bin/env python3
"""How completely a moved note takes its attack (hammer / pluck noise) along.

Builds short mixtures from single-note recordings (Salamander piano, VSCO harp),
moves one note 0.25 s later with NoteEdit::takeBackground (polyphonic_cli
--move ... --take-background) and compares with the truth: the same mixture with
that note's own recording moved.

Per case:
  left    error energy above 2 kHz around the OLD onset, re the note's attack energy (dB; lower is better)
  missing the same around the NEW onset (dB; lower is better)
  snr     whole-file SNR of the render against the truth (dB; higher is better)
"""
import argparse, json, os, subprocess, sys
import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

RATE = 44100
MOVE = 0.25


def load(path, seconds=None):
    x, sr = sf.read(path, always_2d=True)
    x = x.mean(axis=1)
    if sr != RATE:
        x = resample_poly(x, RATE, sr)
    if seconds:
        x = x[: int(seconds * RATE)]
        fade = int(0.05 * RATE)
        x[-fade:] *= np.linspace(1, 0, fade)
    return x.astype(np.float64)


def onset(x):
    a = np.abs(x)
    return int(np.argmax(a > 0.05 * a.max()))


def place(total, parts):
    """parts: (signal, start seconds, gain) -> mix and each placed stem (signal onset at start)."""
    mix = np.zeros(int(total * RATE))
    stems = []
    for sig, start, gain in parts:
        s = np.zeros_like(mix)
        o = onset(sig)
        at = int(start * RATE) - o
        a, b = max(0, at), min(len(mix), at + len(sig))
        s[a:b] = gain * sig[a - at : b - at]
        stems.append(s)
        mix += s
    return mix, stems


def shift(x, seconds):
    n = int(round(seconds * RATE))
    y = np.zeros_like(x)
    y[n:] = x[: len(x) - n]
    return y


def high_energy(x, t0, t1):
    a, b = max(0, int(t0 * RATE)), min(len(x), int(t1 * RATE))
    seg = x[a:b] * np.hanning(b - a)
    spec = np.fft.rfft(seg)
    hz = np.fft.rfftfreq(len(seg), 1 / RATE)
    return float(np.sum(np.abs(spec[hz > 2000]) ** 2)) + 1e-20


def run_cli(cli, model, wav, args):
    cmd = [cli, wav] + (["--model", model] if model else []) + args
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"cli failed: {' '.join(cmd)}\n{r.stderr}")
    return r


def find_note(js, midi, start):
    best, cost = None, 1e9
    seconds = js["hop"] / js["sampleRate"]
    for i, n in enumerate(js["notes"]):
        if not n.get("active", True) or not n["f0"]:
            continue
        f0 = np.median([f for f in n["f0"] if f > 0] or [1.0])
        note_midi = 69 + 12 * np.log2(f0 / 440.0)
        c = abs(note_midi - midi) + 4 * abs(n["start"] * seconds - start)
        if c < cost:
            best, cost = i, c
    return best, cost


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--model")
    ap.add_argument("--data", required=True, help="directory with sal/ and vsco/")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default="run")
    ap.add_argument("--plain", action="store_true", help="without --take-background, for comparison")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    sal = lambda n: load(os.path.join(a.data, "sal/Samples", f"{n}v10.flac"), 2.5)
    harp = lambda n: load(os.path.join(a.data, "vsco/Strings/Harp", f"KSHarp_{n}_mf.wav"), 2.5)
    midi = {"C": 0, "D#": 3, "D": 2, "E": 4, "F": 5, "F#": 6, "G": 7, "A": 9, "B": 11}

    def m(name):
        p = name.rstrip("0123456789")
        return 12 * (int(name[len(p):]) + 1) + midi[p]

    # (name, instrument loader, [(note, start, gain)], index of the moved part)
    cases = [
        ("piano solo C4", sal, [("C4", 0.4, 0.5)], 0),
        ("piano solo F#2", sal, [("F#2", 0.4, 0.5)], 0),
        ("piano solo A5", sal, [("A5", 0.4, 0.5)], 0),
        ("piano staggered C3 / A4", sal, [("C3", 0.3, 0.4), ("A4", 1.0, 0.4)], 1),
        ("piano staggered A4 / C3", sal, [("A4", 0.3, 0.4), ("C3", 1.0, 0.4)], 1),
        ("piano over held C2: D#4", sal, [("C2", 0.1, 0.4), ("D#4", 0.9, 0.4)], 1),
        ("piano over held A2: F#5", sal, [("A2", 0.1, 0.4), ("F#5", 0.9, 0.4)], 1),
        ("piano chord C4 D#4 A4: D#4", sal, [("C4", 0.4, 0.3), ("D#4", 0.4, 0.3), ("A4", 0.4, 0.3)], 1),
        ("piano chord C3 A3 F#4: A3", sal, [("C3", 0.4, 0.3), ("A3", 0.4, 0.3), ("F#4", 0.4, 0.3)], 1),
        ("piano chord C3 A3 F#4: F#4", sal, [("C3", 0.4, 0.3), ("A3", 0.4, 0.3), ("F#4", 0.4, 0.3)], 2),
        ("harp solo D4", harp, [("D4", 0.4, 0.5)], 0),
        ("harp staggered C3 / A4", harp, [("C3", 0.3, 0.4), ("A4", 1.0, 0.4)], 1),
        ("harp chord C3 G3 E5: G3", harp, [("C3", 0.4, 0.3), ("G3", 0.4, 0.3), ("E5", 0.4, 0.3)], 1),
    ]

    rows = []
    for name, loader, parts, moved in cases:
        sigs = [(loader(n), s, g) for n, s, g in parts]
        mix, stems = place(3.0, sigs)
        truth = mix - stems[moved] + shift(stems[moved], MOVE)
        slug = name.replace(" ", "_").replace("/", "-").replace(":", "")
        wav = os.path.join(a.out, slug + ".wav")
        sf.write(wav, mix.astype(np.float32), RATE, subtype="FLOAT")
        js_path = os.path.join(a.out, slug + ".json")
        run_cli(a.cli, a.model, wav, ["--json", js_path])
        js = json.load(open(js_path))
        note, cost = find_note(js, m(parts[moved][0]), parts[moved][1])
        if note is None or cost > 1.5:
            print(f"{name}: moved note not found (cost {cost})")
            continue
        out = os.path.join(a.out, f"{slug}.{a.tag}.wav")
        run_cli(a.cli, a.model, wav, ["--move", f"{note}:{MOVE}", "--out", out] + ([] if a.plain else ["--take-background"]))
        y, _ = sf.read(out)
        y = y if y.ndim == 1 else y.mean(axis=1)
        y = y[: len(truth)]
        err = y - truth
        t0 = parts[moved][1]
        attack = high_energy(stems[moved], t0 - 0.01, t0 + 0.05)
        left = 10 * np.log10(high_energy(err, t0 - 0.06, t0 + 0.10) / attack)
        missing = 10 * np.log10(high_energy(err, t0 + MOVE - 0.06, t0 + MOVE + 0.10) / attack)
        snr = 10 * np.log10(np.sum(truth ** 2) / (np.sum(err ** 2) + 1e-20))
        rows.append({"case": name, "left": left, "missing": missing, "snr": snr})
        print(f"{name:32s} left {left:6.1f} dB  missing {missing:6.1f} dB  snr {snr:5.1f} dB", flush=True)

    # A note on its own is near perfect and would dominate a mean in dB; the cases with other
    # notes sounding are the ones to compare.
    for label, group in (("mean, solo", [r for r in rows if " solo " in r["case"]]),
                         ("mean, with other notes", [r for r in rows if " solo " not in r["case"]])):
        if group:
            print(f"{label:32s} left {np.mean([r['left'] for r in group]):6.1f} dB  "
                  f"missing {np.mean([r['missing'] for r in group]):6.1f} dB  snr {np.mean([r['snr'] for r in group]):5.1f} dB")
    json.dump(rows, open(os.path.join(a.out, f"results.{a.tag}.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
