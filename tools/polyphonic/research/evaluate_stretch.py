"""Evaluate note stretching: lengthen or shorten one note of a mixture and compare the result with the
same mixture in which that note was played longer or shorter.

    python evaluate_stretch.py --work WORK --cli build/polyphonic_cli [--model basic_pitch_nmp.onnx]

The target note (1.0 s, starting at 0.1 s) is stretched to 0.6, 1.5 and 2.0 times its length, alone and
over one or two sustained notes of other instruments. The stretch ratio is chosen so that the note ends
where the longer (or shorter) recording ends; its first Params::stretchAttackSeconds keep their speed.
Score: spectral SNR against the played-longer mixture ("doing nothing" is the unedited mixture).
For sustained instruments this is the natural target; for piano and harp a stretched note decays
more slowly instead of ringing on, so their scores mostly measure that difference.
"""
import argparse, json, os, subprocess, sys
import numpy as np, soundfile as sf
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_corpora as mc

FS = 44100
TOTAL, ON, DUR = 3.2, 0.1, 1.0
SUSTAINED = {'cellos', 'violins', 'bassoon', 'clarinet', 'flute', 'oboe', 'organ'}


def place(x, at, total=TOTAL):
    y = np.zeros(int(total * FS))
    a = int(at * FS)
    k = min(len(x), len(y) - a)
    if k > 0:
        y[a:a + k] = x[:k]
    return y


def magsnr(ref, est, n=2048, hop=512):
    def spec(x):
        w = np.hanning(n + 1)[:-1]
        f = np.lib.stride_tricks.sliding_window_view(np.pad(x, (n // 2, n // 2)), n)[::hop] * w
        return np.abs(np.fft.rfft(f, axis=1))
    a, b = spec(ref), spec(est)
    return 10 * np.log10(np.sum(a ** 2) / (np.sum((a - b) ** 2) + 1e-20))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--model')
    a = ap.parse_args()
    tmp = f'{a.work}/eval/stretch'
    os.makedirs(tmp, exist_ok=True)
    notes = mc.Notes(a.work)
    R = notes.ranges
    extra = ['--model', a.model] if a.model else []

    def cli(*args):
        subprocess.run([a.cli] + [str(x) for x in args] + extra, check=True, capture_output=True)

    rng = np.random.default_rng(777)
    insts = ['piano', 'harp', 'cellos', 'violins', 'bassoon', 'clarinet', 'flute', 'oboe', 'organ']
    rows = []
    for inst in insts:
        for rep in range(2):
            m = float(rng.integers(R[inst][0] + 2, R[inst][1] - 2))
            others = []
            for _ in range(2):
                oi = [i for i in insts if i != inst][rng.integers(len(insts) - 1)]
                om = float(rng.integers(R[oi][0] + 1, R[oi][1] - 1))
                if abs(om - m) >= 1 and abs((om - m) % 12) >= 0.5:
                    others.append((oi, om, float(rng.uniform(-6, 0)), float(rng.uniform(0, 0.02))))
            for ratio in (0.6, 1.5, 2.0):
                for chord in (False, True):
                    acc = sum((place(notes.get(oi, om, TOTAL - 0.3, g), 0.1 + d) for (oi, om, g, d) in (others if chord else [])),
                              np.zeros(int(TOTAL * FS)))
                    mix = place(notes.get(inst, m, DUR), ON) + acc
                    target = place(notes.get(inst, m, DUR * ratio), ON) + acc
                    tag = f'{tmp}/{inst}{rep}-{ratio}-{"chord" if chord else "alone"}'
                    sf.write(tag + '.wav', mix.astype(np.float32), FS, subtype='FLOAT')
                    cli(tag + '.wav', '--json', tag + '.json')
                    js = json.load(open(tag + '.json'))
                    hop = js['hop']
                    found = [(i, n) for i, n in enumerate(js['notes']) if n['active'] and abs(n['start'] * hop / FS - ON) < 0.15
                             and abs(69 + 12 * np.log2(np.median(n['f0']) / 440) - m) <= 0.5]
                    if not found:
                        continue
                    idx, n = max(found, key=lambda c: c[1]['end'] - c[1]['start'])
                    # the ratio that makes the note end where the longer recording ends
                    anchor = n['start'] + round(0.08 * FS / hop)
                    r = (n['start'] + ratio * (n['end'] - n['start']) - anchor) / max(1, n['end'] - anchor)
                    cli(tag + '.wav', '--stretch', f'{idx}:{r:.4f}', '--out', tag + '_out.wav')
                    y = sf.read(tag + '_out.wav')[0][:len(mix)]
                    rows.append(dict(inst=inst, ratio=ratio, chord=chord, stretched=magsnr(target, y), nothing=magsnr(target, mix)))
    json.dump(rows, open(f'{tmp}/results.json', 'w'), indent=1)
    print(f'note stretching ({"with" if a.model else "without"} Basic Pitch evidence): spectral SNR against the note played '
          'longer/shorter, mean over instruments (doing nothing in brackets)')
    for fam, members in (('sustained', SUSTAINED), ('piano and harp', {'piano', 'harp'})):
        for chord in (False, True):
            cells = []
            for ratio in (0.6, 1.5, 2.0):
                g = [r for r in rows if r['inst'] in members and r['chord'] == chord and r['ratio'] == ratio]
                if g:
                    cells.append(f'x{ratio}: {np.mean([r["stretched"] for r in g]):5.1f} dB ({np.mean([r["nothing"] for r in g]):5.1f})')
            print(f'  {fam:15s} {"in a chord" if chord else "alone     "}  ' + '   '.join(cells))


if __name__ == '__main__':
    main()
