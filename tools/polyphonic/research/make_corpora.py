"""Build the polyphonic engine's evaluation and training mixtures from the catalogued single-note recordings.

    python make_corpora.py WORK

writes
    WORK/corpus/   8 hand-made test pieces with stems and an edit each (major->minor and so on)
    WORK/rcorpus/  240 random chord mixtures (training set for the purifier, seed 12345)
    WORK/tcorpus/  80 random chord mixtures with stems (held-out test set, seed 999)
    WORK/pcorpus/  36 mixtures with one note struck again at the same pitch (repeated notes, seed 4242)
Every mixture is a .npz (mix, and stems where kept) plus a .json with the true notes.
"""
import json, os, sys
import numpy as np, soundfile as sf
from scipy.signal import resample

FS = 44100


class Notes:
    def __init__(self, work):
        self.work = work
        self.cat = json.load(open(f'{work}/samples/catalog.json'))
        self.ranges = {}
        for c in self.cat:
            lo, hi = self.ranges.get(c['inst'], (999, -999))
            self.ranges[c['inst']] = (min(lo, c['midi']), max(hi, c['midi']))

    def get(self, inst, midi, dur, gain_db=0.0, release=0.15):
        """A real recording of `inst` at `midi`: nearest sample, resampled by the remaining interval."""
        c = min((c for c in self.cat if c['inst'] == inst), key=lambda c: abs(c['midi'] - midi))
        x, _ = sf.read(f"{self.work}/{c['path']}")
        ratio = 2 ** ((midi - c['midi']) / 12)
        if abs(ratio - 1) > 1e-4:
            x = resample(x, int(round(len(x) / ratio)))
        n = int(dur * FS)
        x = np.concatenate([x, np.zeros(max(0, n - len(x)))])[:n]
        r = int(release * FS)
        if 0 < r < n:
            x[n - r:] *= np.cos(np.linspace(0, np.pi / 2, r)) ** 2
        rms = np.sqrt(np.mean(x[: int(min(1.0, dur) * FS)] ** 2)) + 1e-12
        return x / rms * 0.05 * 10 ** (gain_db / 20)


def write(out, name, notes_obj, played, total, extra, keep_stems=True):
    n = int(total * FS)
    stems, notes = [], []
    for i, (inst, midi, on, dur, g) in enumerate(played):
        x = notes_obj.get(inst, midi, dur, g)
        s = np.zeros(n)
        a = int(on * FS)
        k = min(len(x), n - a)
        s[a:a + k] = x[:k]
        stems.append(s)
        notes.append(dict(id=i, inst=inst, midi=float(midi), onset=on, offset=min(total, on + dur)))
    mix = np.sum(stems, axis=0)
    k = 0.7 / np.abs(mix).max()
    arrays = dict(mix=(mix * k).astype(np.float32))
    if keep_stems:
        arrays['stems'] = (np.array(stems) * k).astype(np.float32)
    os.makedirs(out, exist_ok=True)
    np.savez_compressed(f'{out}/{name}.npz', **arrays)
    json.dump(dict(name=name, notes=notes, **extra), open(f'{out}/{name}.json', 'w'), indent=1)


def hand_made(work, N):
    out = f'{work}/corpus'
    items = [
        ('piano_triad', [('piano', 60, .05, 3.5, 0), ('piano', 64, .05, 3.5, -1), ('piano', 67, .05, 3.5, -2)], 4.0,
         [dict(note=1, semitones=-1)], 'piano C4-E4-G4 struck together; edit E4->Eb4'),
        ('piano_arpeggio', [('piano', 48, .05, 3.8, 2), ('piano', 64, .35, 3.4, -2), ('piano', 67, .65, 3.1, -2),
                            ('piano', 72, .95, 2.8, -3)], 4.0, [dict(note=2, semitones=2)], 'piano C3 then E4, G4, C5 staggered; edit G4->A4'),
        ('piano_progression', [('piano', m, t, 1.0, 2 if j == 0 else -1) for t, ch in
                               [(.05, [36, 60, 64, 67]), (1.05, [33, 60, 64, 69]), (2.05, [41, 60, 65, 69]), (3.05, [43, 59, 62, 67])]
                               for j, m in enumerate(ch)], 4.3, [dict(note=2, semitones=-1), dict(note=14, semitones=-1)],
         'piano C-Am-F-G with bass; edit E4->Eb4 in chord 1 and D4->Db4 in chord 4'),
        ('piano_octave', [('piano', 48, .05, 3.5, 0), ('piano', 60, .05, 3.5, -2), ('piano', 67, .05, 3.5, -3)], 4.0,
         [dict(note=1, semitones=2)], 'piano C3+C4+G4 (octave doubling); edit C4->D4'),
        ('organ_triad', [('organ', 60, .05, 3.5, 0), ('organ', 64, .05, 3.5, -1), ('organ', 67, .05, 3.5, -2)], 3.8,
         [dict(note=1, semitones=-1)], 'pipe organ C4-E4-G4 sustained; edit E4->Eb4'),
        ('woodwinds', [('bassoon', 48, .05, 3.5, 0), ('clarinet', 64, .05, 3.5, -1), ('oboe', 79, .05, 3.5, -3),
                       ('flute', 84, .05, 3.5, -3)], 3.8, [dict(note=1, semitones=-1)], 'bassoon C3, clarinet E4, oboe G5, flute C6; edit clarinet E4->Eb4'),
        ('strings', [('cellos', 60, .05, 3.5, 0), ('cellos', 64, .05, 3.5, -1), ('violins', 67, .05, 3.5, -1),
                     ('violins', 72, .05, 3.5, -2)], 3.8, [dict(note=1, semitones=-1)], 'cello section C4, E4 + violin section G4, C5; edit E4->Eb4'),
        ('harp', [('harp', 48, .05, 3.5, 0), ('harp', 52, .13, 3.4, -1), ('harp', 55, .21, 3.3, -1), ('harp', 72, .29, 3.2, -2),
                  ('harp', 76, .37, 3.1, -3)], 3.8, [dict(note=1, semitones=1)], 'harp C3, E3, G3, C5, E5 rolled; edit E3->F3'),
    ]
    for name, played, total, edits, desc in items:
        write(out, name, N, played, total, dict(desc=desc, edits=edits, insts=sorted({e[0] for e in played})))
        print('corpus', name)


TEMPLATES = [[0, 4, 7], [0, 3, 7], [0, 5, 7], [0, 4, 7, 11], [0, 3, 7, 10], [0, 4, 7, 10], [0, 7, 12], [0, 12],
             [0, 4, 7, 14], [0, 4, 9], [0, 3], [0, 4], [0, 5], [0, 7], [0, 9], [0, 2], [-12, 0, 4, 7], [-12, 0, 3, 7],
             [-12, 4, 7, 12], [0, 7, 16], [0, 4, 7, 12], [0, 12, 19], [0, 16, 19], [0, 3, 7, 12, 15]]


def random_corpus(work, N, sub, seed, count, keep_stems):
    rng = np.random.default_rng(seed)
    R = N.ranges
    for idx in range(count):
        r = rng.random()
        insts = (['piano'] if r < .35 else ['harp'] if r < .5 else ['organ'] if r < .6 else ['cellos', 'violins'] if r < .75
                 else ['bassoon', 'clarinet', 'oboe', 'flute'] if r < .9 else ['piano', 'cellos', 'violins'])
        tpl = list(TEMPLATES[rng.integers(len(TEMPLATES))])
        lo = min(R[i][0] for i in insts) - 2
        hi = max(R[i][1] for i in insts) + 2
        root = rng.integers(int(lo - min(tpl)), max(int(lo - min(tpl)) + 1, int(hi - max(tpl))))
        staggered = rng.random() < 0.4
        total = 3.0
        played = []
        for j, m in enumerate(sorted(root + t for t in tpl)):
            cands = [i for i in insts if R[i][0] - 2.5 <= m <= R[i][1] + 2.5]
            if not cands:
                continue
            inst = cands[rng.integers(len(cands))]
            on = 0.05 + (j * rng.uniform(0.08, 0.35) if staggered else rng.uniform(0, 0.015))
            played.append((inst, float(m), on, total - on - rng.uniform(0.1, 0.8), rng.uniform(-6, 2)))
        if rng.random() < 0.35 and len(played) >= 2:          # a second chord moving some voices
            t2 = rng.uniform(1.3, 1.9)
            new = []
            for (inst, m, on, dur, g) in played:
                if rng.random() < 0.5:
                    m2 = m + float(rng.choice([-2, -1, 1, 2, 3, -3, 5]))
                    if R[inst][0] - 2.5 <= m2 <= R[inst][1] + 2.5:
                        new += [(inst, m, on, t2 - on, g), (inst, m2, t2, total - t2 - 0.1, g)]
                        continue
                new.append((inst, m, on, dur, g))
            played = new
        write(f'{work}/{sub}', f'r{idx:03d}', N, played, total, dict(insts=insts), keep_stems)
    print(sub, count, 'mixtures')


def repeated_corpus(work, N, sub='pcorpus', seed=4242):
    """One note struck again at the same pitch 0.35-1.3 s later, over one or two sustained notes of
    other instruments. Note 1 of each mixture is the second strike."""
    rng = np.random.default_rng(seed)
    R = N.ranges
    insts = ['piano', 'harp', 'cellos', 'violins', 'bassoon', 'clarinet', 'flute', 'organ', 'oboe']
    idx = 0
    for gap in (0.35, 0.6, 0.9, 1.3):
        for inst in insts:
            m = float(rng.integers(R[inst][0] + 2, R[inst][1] - 2))
            again = 0.1 + gap
            played = [(inst, m, 0.1, gap, 0.0), (inst, m, again, 2.8 - again, rng.uniform(-3, 0))]
            others = [i for i in insts if i != inst]
            for _ in range(rng.integers(1, 3)):
                oi = others[rng.integers(len(others))]
                om = float(rng.integers(R[oi][0] + 1, R[oi][1] - 1))
                if abs(om - m) < 1 or abs((om - m) % 12) < 0.5:
                    continue
                played.append((oi, om, 0.1 + rng.uniform(0, 0.02), 2.6, rng.uniform(-6, 0)))
            write(f'{work}/{sub}', f'p{idx:03d}', N, played, 3.0, dict(insts=[inst], repeated=1))
            idx += 1
    print(sub, idx, 'mixtures')


if __name__ == '__main__':
    work = sys.argv[1]
    N = Notes(work)
    hand_made(work, N)
    random_corpus(work, N, 'rcorpus', 12345, 240, keep_stems=False)
    random_corpus(work, N, 'tcorpus', 999, 80, keep_stems=True)
    repeated_corpus(work, N)
