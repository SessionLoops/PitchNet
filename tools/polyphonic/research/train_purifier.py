"""Train the polyphonic engine's purifier (note vs harmonic-ghost classifier) from features dumped by polyphonic_cli,
and export it as Source/Audio/Polyphonic/PolyPurifierModel.h.

    python train_purifier.py --work WORK --cli path/to/polyphonic_cli --model basic_pitch_nmp.onnx

WORK must contain rcorpus/ (training mixtures, see make_training_corpus.py) and corpus/ (the
hand-made held-out test set, see make_test_corpus.py). Requires numpy, soundfile, scikit-learn.
"""
import argparse, glob, json, os, subprocess, sys
import numpy as np, soundfile as sf
from sklearn.ensemble import GradientBoostingClassifier

FEATURES = ['rel', 'log_energy_rel', 'uniq_hi', 'uniq_any', 'f0_amp_share', 'f0_rel_db', 'dur', 'midi', 'hits',
            'ot2', 'ot3', 'ot4p', 'ot_dt', 'ot_pc', 'ot_ac', 'sub2', 'sub3p', 'n_overlap', 'is_lowest',
            'bp_mean', 'bp_max']
N_DSP = 19      # the first N_DSP features need no neural evidence


def dump(cli, model, npz, tsv):
    wav = tsv[:-4] + '.wav'
    if not os.path.exists(wav):
        sf.write(wav, np.load(npz)['mix'].astype(np.float32), 44100, subtype='FLOAT')
    cmd = [cli, wav, '--dump-features', tsv] + (['--model', model] if model else [])
    subprocess.run(cmd, check=True, capture_output=True)


def label(rows, meta, tol_st=0.5):
    out = []
    for (s, e, midi) in rows:
        ok = 0
        for g in meta['notes']:
            ov = min(g['offset'], e) - max(g['onset'], s)
            if abs(midi - g['midi']) <= tol_st and ov > 0.5 * max(e - s, 1e-3):
                ok = 1
        out.append(ok)
    return out


def family(insts):
    s = ' '.join(insts)
    for k, v in [('piano', 'piano'), ('harp', 'harp'), ('organ', 'organ'), ('cellos', 'strings'), ('bassoon', 'winds'), ('clarinet', 'winds')]:
        if k in s:
            return v
    return 'other'


def load(work, cli, model, sub):
    X, y, fam, items = [], [], [], []
    os.makedirs(f'{work}/features/{sub}', exist_ok=True)
    for npz in sorted(glob.glob(f'{work}/{sub}/*.npz')):
        name = os.path.basename(npz)[:-4]
        meta = json.load(open(npz[:-4] + '.json'))
        tsv = f'{work}/features/{sub}/{name}.tsv'
        if not os.path.exists(tsv):
            dump(cli, model, npz, tsv)
        rows = [list(map(float, l.split('\t'))) for l in open(tsv) if l.strip()]
        if not rows:
            continue
        labs = label([(r[0], r[1], r[2]) for r in rows], meta)
        for r, l in zip(rows, labs):
            X.append(r[4:]); y.append(l); fam.append(family(meta.get('insts', [name]))); items.append(name)
    return np.array(X, float), np.array(y, int), np.array(fam), np.array(items)


def gbt():
    # leaves of at least 50 candidates: without that, a few trees isolate narrow bands of one feature
    # (Basic Pitch's posterior near its floor) and flip clear ghosts to certain notes
    return GradientBoostingClassifier(max_depth=3, n_estimators=150, learning_rate=0.1, subsample=0.8,
                                      min_samples_leaf=50, random_state=0)


def report(name, m, X, y, fam, Xt, yt):
    print(f'{name}:')
    for f in sorted(set(fam)):
        tr, te = fam != f, fam == f
        mm = gbt().fit(X[tr], y[tr])
        pr = mm.predict_proba(X[te])[:, 1] > 0.5
        tp = (pr & (y[te] == 1)).sum()
        print(f'  CV hold-out {f:8s} n={te.sum():5d} precision {tp / max(1, pr.sum()):.3f} recall {tp / max(1, (y[te] == 1).sum()):.3f}')
    pr = m.predict_proba(Xt)[:, 1] > 0.5
    tp = (pr & (yt == 1)).sum()
    print(f'  held-out test corpus: precision {tp / max(1, pr.sum()):.3f} recall {tp / max(1, (yt == 1).sum()):.3f}')


def fl(v):
    t = f'{v:.9g}'
    if '.' not in t and 'e' not in t and 'inf' not in t and 'nan' not in t:
        t += '.0'
    return t + 'f'


def export(m, feats, name):
    nodes, starts = [], []
    for est in m.estimators_[:, 0]:
        tr = est.tree_
        starts.append(len(nodes))
        base = len(nodes)
        for i in range(tr.node_count):
            leaf = tr.children_left[i] == -1
            nodes.append((-1 if leaf else int(tr.feature[i]), 0.0 if leaf else float(np.float32(tr.threshold[i])),
                          -1 if leaf else base + int(tr.children_left[i]), -1 if leaf else base + int(tr.children_right[i]),
                          float(tr.value[i][0][0]) if leaf else 0.0))
    x0 = np.zeros((1, len(feats)), np.float32)
    init = float(m.decision_function(x0)[0] - m.learning_rate * sum(e.predict(x0)[0] for e in m.estimators_[:, 0]))
    out = [f'// {name}: {len(starts)} trees, features: ' + ', '.join(feats),
           f'static const float k{name}Init = {fl(init)};', f'static const float k{name}Rate = {fl(m.learning_rate)};',
           f'static const int k{name}NumTrees = {len(starts)};', f'static const int k{name}NumFeatures = {len(feats)};',
           f'static const int k{name}TreeStart[] = {{' + ','.join(map(str, starts)) + '};', f'static const TreeNode k{name}Nodes[] = {{']
    out += [f'    {{{f}, {fl(th)}, {l}, {r}, {fl(v)}}},' for (f, th, l, r, v) in nodes]
    out.append('};')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--model', help='basic_pitch_nmp.onnx (needs a CLI built with ONNX Runtime)')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '../../../Source/Audio/Polyphonic/PolyPurifierModel.h'))
    a = ap.parse_args()
    X, y, fam, _ = load(a.work, a.cli, a.model, 'rcorpus')
    Xt, yt, _, _ = load(a.work, a.cli, a.model, 'corpus')
    print(f'training candidates {len(y)} ({y.sum()} real notes), test candidates {len(yt)} ({yt.sum()} real)')
    hdr = ['// Generated by tools/polyphonic/research/train_purifier.py - do not edit by hand.',
           '// Gradient-boosted trees (depth 3) scoring note candidates as real notes vs harmonic ghosts.',
           '#pragma once', '', 'namespace polyphonic', '{', 'namespace detail', '{', 'namespace purifier', '{', '',
           'struct TreeNode', '{', '    int feature;      // -1: leaf', '    float threshold;  // go left when x <= threshold',
           '    int left, right;', '    float value;      // leaf value', '};', '']
    for name, k in [('Evidence', len(FEATURES)), ('Dsp', N_DSP)]:
        if k > X.shape[1]:
            print(f'skipping {name}: features were dumped without evidence'); continue
        m = gbt().fit(X[:, :k], y)
        report(name, m, X[:, :k], y, fam, Xt[:, :k], yt)
        hdr += [export(m, FEATURES[:k], name), '']
    hdr += ['} // namespace purifier', '} // namespace detail', '} // namespace polyphonic', '']
    open(a.out, 'w').write('\n'.join(hdr))
    print('wrote', os.path.abspath(a.out))


if __name__ == '__main__':
    main()
