"""Evaluate polyphonic_cli on mixtures with known stems.

    python evaluate.py --work WORK --cli build/polyphonic_cli [--model basic_pitch_nmp.onnx] [--corpus tcorpus]

Detection: a true note is found when a detected note within half a semitone overlaps it in time
(and, for onset F1, starts within 50 / 100 ms). Separation: SDR of each true note's stem against the
sum of the detected notes matched to it. For corpora whose JSON lists edits (the hand-made corpus),
also: pitch edit vs the same edit applied to the isolated stem ("oracle", spectral SNR), mute vs the
mix without that stem (SDR) and a 0.25 s move vs the mix with that stem moved (spectral SNR).
For the repeated-note corpus: whether the second strike is found as a note of its own (same pitch,
onset within 60 ms).
"""
import argparse, glob, json, os, subprocess
import numpy as np, soundfile as sf

FS = 44100


def sdr(ref, est):
    return 10 * np.log10(np.sum(ref ** 2) / (np.sum((ref - est) ** 2) + 1e-20) + 1e-20)


def magsnr(ref, est, n=2048, hop=512):
    def spec(x):
        w = np.hanning(n + 1)[:-1]
        f = np.lib.stride_tricks.sliding_window_view(np.pad(x, (n // 2, n // 2)), n)[::hop] * w
        return np.abs(np.fft.rfft(f, axis=1))
    R, E = spec(ref), spec(est)
    return 10 * np.log10(np.sum(R ** 2) / (np.sum((R - E) ** 2) + 1e-20))


class Cli:
    def __init__(self, cli, model):
        self.cli, self.model = cli, model

    def __call__(self, wav, *args):
        cmd = [self.cli, wav, '--json', wav + '.json'] + (['--model', self.model] if self.model else []) + list(args)
        subprocess.run(cmd, check=True, capture_output=True)
        js = json.load(open(wav + '.json'))
        hop, fs = js['hop'], js['sampleRate']
        return [dict(idx=i, on=n['start'] * hop / fs, off=n['end'] * hop / fs, midi=69 + 12 * np.log2(np.median(n['f0']) / 440))
                for i, n in enumerate(js['notes']) if n['active']]


def matches(det, g):
    return [d for d in det if abs(d['midi'] - g['midi']) <= 0.5 and
            min(g['offset'], d['off']) - max(g['onset'], d['on']) > 0.3 * min(g['offset'] - g['onset'], d['off'] - d['on'])]


def wav_of(x, path):
    sf.write(path, x.astype(np.float32), FS, subtype='FLOAT')
    return path


def run_item(npz, tmp, cli):
    name = os.path.basename(npz)[:-4]
    d = np.load(npz)
    meta = json.load(open(npz[:-4] + '.json'))
    mix, stems = d['mix'].astype(np.float64), d['stems'].astype(np.float64)
    wav = wav_of(mix, f'{tmp}/{name}.wav')
    solo = f'{tmp}/{name}_solo'
    os.makedirs(solo, exist_ok=True)
    for f in os.listdir(solo):
        os.remove(os.path.join(solo, f))
    det = cli(wav, '--solo-all', solo)
    r = dict(name=name, n_gt=len(meta['notes']), n_det=len(det))
    for tol in (0.05, 0.1):
        used, tp = set(), 0
        for g in meta['notes']:
            for j, dd in enumerate(det):
                if j not in used and abs(dd['midi'] - g['midi']) <= 0.5 and abs(dd['on'] - g['onset']) <= tol:
                    used.add(j); tp += 1; break
        r[f'tp{tol}'] = tp
    ok, found, seps, mt = set(), 0, [], {}
    for k, g in enumerate(meta['notes']):
        ms = matches(det, g)
        mt[k] = [dd['idx'] for dd in ms]
        found += bool(ms)
        ok.update(mt[k])
        est = sum((sf.read(f'{solo}/note_{i}.wav')[0] for i in mt[k]), np.zeros_like(mix))
        seps.append(sdr(stems[k], est))
    r.update(found=found, det_ok=len(ok), seps=seps)
    if 'repeated' in meta:
        g = meta['notes'][meta['repeated']]
        r['restrike'] = any(abs(dd['midi'] - g['midi']) <= 0.5 and abs(dd['on'] - g['onset']) <= 0.06 for dd in det)
    if meta.get('edits'):
        e = meta['edits'][0]
        k = e['note']
        st = stems[k]
        # pitch edit vs the same engine on the isolated stem
        edit = sum([['--edit', f"{i}:{e['semitones']}"] for i in mt[k]], [])
        cli(wav, *edit, '--out', f'{tmp}/{name}_edit.wav')
        swav = wav_of(st, f'{tmp}/{name}_stem.wav')
        sdet = cli(swav)
        sedit = sum([['--edit', f"{dd['idx']}:{e['semitones']}"] for dd in sdet if abs(dd['midi'] - meta['notes'][k]['midi']) <= 0.5], [])
        cli(swav, *sedit, '--out', swav + '.edit.wav')
        oracle = mix - st + sf.read(swav + '.edit.wav')[0]
        y = sf.read(f'{tmp}/{name}_edit.wav')[0]
        r.update(edit=magsnr(oracle, y), edit_base=magsnr(oracle, mix))
        cli(wav, *sum([['--edit', f'{i}:0:0:mute'] for i in mt[k]], []), '--out', f'{tmp}/{name}_mute.wav')
        r.update(mute=sdr(mix - st, sf.read(f'{tmp}/{name}_mute.wav')[0]), mute_base=sdr(mix - st, mix))
        cli(wav, *sum([['--move', f'{i}:0.25'] for i in mt[k]], []), '--out', f'{tmp}/{name}_move.wav')
        sh = int(0.25 * FS)
        moved = mix - st + np.concatenate([np.zeros(sh), st[:-sh]])
        r.update(move=magsnr(moved, sf.read(f'{tmp}/{name}_move.wav')[0]), move_base=magsnr(moved, mix))
        cli(wav, '--out', f'{tmp}/{name}_identity.wav')
        r['identity'] = sdr(mix, sf.read(f'{tmp}/{name}_identity.wav')[0])
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--model')
    ap.add_argument('--corpus', default='tcorpus')
    a = ap.parse_args()
    tmp = f"{a.work}/eval/{a.corpus}-{'evidence' if a.model else 'dsp'}"
    os.makedirs(tmp, exist_ok=True)
    cli = Cli(a.cli, a.model)
    rows = [run_item(p, tmp, cli) for p in sorted(glob.glob(f'{a.work}/{a.corpus}/*.npz'))]
    G = sum(r['n_gt'] for r in rows)
    D = sum(r['n_det'] for r in rows)
    print(f'{a.corpus}: {len(rows)} mixtures, {G} notes, {D} detected ({"with" if a.model else "without"} Basic Pitch evidence)')
    P, R = sum(r['det_ok'] for r in rows) / max(1, D), sum(r['found'] for r in rows) / max(1, G)
    print(f'  notes (pitch + overlap): precision {P:.3f} recall {R:.3f} F1 {2 * P * R / max(1e-9, P + R):.3f}')
    for tol in (0.05, 0.1):
        tp = sum(r[f'tp{tol}'] for r in rows)
        P, R = tp / max(1, D), tp / max(1, G)
        print(f'  notes (onset within {int(tol * 1000)} ms): precision {P:.3f} recall {R:.3f} F1 {2 * P * R / max(1e-9, P + R):.3f}')
    s = np.concatenate([r['seps'] for r in rows])
    print(f'  separation SDR: mean {s.mean():.2f} dB, median {np.median(s):.2f} dB')
    if 'restrike' in rows[0]:
        print(f'  repeated notes: second strike found as a note of its own in {sum(r["restrike"] for r in rows)} of {len(rows)}')
    if 'edit' in rows[0]:
        for key in ('edit', 'mute', 'move'):
            print(f'  {key:5s}: {np.mean([r[key] for r in rows]):5.1f} dB (doing nothing: {np.mean([r[key + "_base"] for r in rows]):5.1f} dB)')
        print(f'  identity render SDR: min {min(r["identity"] for r in rows):.0f} dB')
    json.dump(rows, open(f'{tmp}/results.json', 'w'), indent=1)


if __name__ == '__main__':
    main()
