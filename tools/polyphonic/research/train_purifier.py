"""Train the polyphonic engine's purifier (note vs harmonic-ghost classifier) from features dumped by polyphonic_cli,
and export it as Source/Audio/Polyphonic/PolyPurifierModel.h.

    python train_purifier.py --work WORK --cli path/to/polyphonic_cli --model basic_pitch_nmp.onnx

WORK must contain the selected training partitions and a separate validation
partition (see make_corpora.py). Defaults: rcorpus/ and the hand-made corpus/.
Requires numpy, soundfile, scikit-learn. Cached features are keyed by the CLI,
evidence model, audio archive, labels and feature schema.
"""
import argparse, hashlib, json, os, re, shutil, subprocess
from pathlib import Path
import numpy as np, soundfile as sf
from sklearn.ensemble import GradientBoostingClassifier

from evaluate import audit_split

FEATURES = ['rel', 'log_energy_rel', 'uniq_hi', 'uniq_any', 'f0_amp_share', 'f0_rel_db', 'dur', 'midi', 'hits',
            'ot2', 'ot3', 'ot4p', 'ot_dt', 'ot_pc', 'ot_ac', 'sub2', 'sub3p', 'n_overlap', 'is_lowest',
            'bp_mean', 'bp_max']
N_DSP = 19      # the first N_DSP features need no neural evidence


def dump(cli, model, npz, tsv):
    wav = tsv[:-4] + '.wav'
    # A changed mixture must also replace the cached WAV, not just its features.
    with np.load(npz) as data:
        sf.write(wav, data['mix'].astype(np.float32), 44100, subtype='FLOAT')
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


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def load(work, cli, model, sub):
    X, y, fam, items = [], [], [], []
    identity = dict(cli_sha256=file_hash(cli), model_sha256=file_hash(model) if model else None,
                    schema=FEATURES if model else FEATURES[:N_DSP])
    root = Path(work).resolve()
    corpus = (root / sub).resolve()
    try:
        cache_name = corpus.relative_to(root)
    except ValueError:
        cache_name = Path(corpus.name + '-' + hashlib.sha256(str(corpus).encode()).hexdigest()[:12])
    cache = root / 'features' / cache_name
    cache.mkdir(parents=True, exist_ok=True)
    for npz in sorted(corpus.glob('*.npz')):
        name = npz.stem
        with npz.with_suffix('.json').open() as source:
            meta = json.load(source)
        tsv = str(cache / (name + '.tsv'))
        key = dict(identity, audio_sha256=file_hash(npz), labels_sha256=file_hash(npz.with_suffix('.json')))
        manifest = tsv + '.manifest.json'
        cached = None
        if os.path.exists(manifest):
            try:
                with open(manifest) as source:
                    cached = json.load(source)
            except (OSError, ValueError):
                pass
        if not os.path.exists(tsv) or cached != key:
            dump(cli, model, npz, tsv)
            with open(manifest, 'w') as target:
                json.dump(key, target, sort_keys=True)
        with open(tsv) as source:
            rows = [list(map(float, l.split('\t'))) for l in source if l.strip()]
        if not rows:
            continue
        if any(len(r) != 4 + len(identity['schema']) for r in rows):
            raise ValueError(f'{tsv}: feature width differs from the declared schema')
        labs = label([(r[0], r[1], r[2]) for r in rows], meta)
        for r, l in zip(rows, labs):
            X.append(r[4:]); y.append(l); fam.append(family(meta.get('insts', [name]))); items.append(name)
    return np.array(X, float), np.array(y, int), np.array(fam), np.array(items)


def audit_training_split(work, partitions, validation):
    """Reject aliases and copied training waveforms before extracting any features."""
    root = Path(work).resolve()
    training = [(root / name).resolve() for name in partitions]
    validation = (root / validation).resolve()
    if len(set(training)) != len(training):
        raise ValueError('training partitions must refer to distinct directories')
    if validation in training:
        raise ValueError('validation corpus must differ from the training partitions')
    for folder in training:
        if not any(folder.glob('*.npz')):
            raise ValueError(f'no training mixtures found in {folder}')
    paths = sorted(validation.glob('*.npz'))
    if not paths:
        raise ValueError(f'no validation mixtures found in {validation}')
    return audit_split(paths, training, 'validation')


def gbt():
    # leaves of at least 50 candidates: without that, a few trees isolate narrow bands of one feature
    # (Basic Pitch's posterior near its floor) and flip clear ghosts to certain notes
    return GradientBoostingClassifier(max_depth=3, n_estimators=150, learning_rate=0.1, subsample=0.8,
                                      min_samples_leaf=50, random_state=0)


def report(name, m, X, y, fam, Xt, yt):
    print(f'{name}:')
    print('  mixture-group diagnostics share source recordings; these are not unseen-family tests')
    for f in sorted(set(fam)):
        tr, te = fam != f, fam == f
        if len(set(y[tr])) < 2:
            continue
        mm = gbt().fit(X[tr], y[tr])
        pr = mm.predict_proba(X[te])[:, 1] > 0.5
        tp = (pr & (y[te] == 1)).sum()
        print(f'  CV mixture group {f:8s} n={te.sum():5d} precision {tp / max(1, pr.sum()):.3f} recall {tp / max(1, (y[te] == 1).sum()):.3f}')
    pr = m.predict_proba(Xt)[:, 1] > 0.5
    tp = (pr & (yt == 1)).sum()
    print(f'  validation corpus: precision {tp / max(1, pr.sum()):.3f} recall {tp / max(1, (yt == 1).sum()):.3f}')


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
    ap.add_argument('--training-corpus', action='append', help='training partition; repeat to combine (default: rcorpus)')
    ap.add_argument('--validation-corpus', default='corpus', help='validation partition (default: hand-made corpus)')
    ap.add_argument('--preserve-dsp-from', help='copy the DSP trees from this header and train only the evidence model')
    a = ap.parse_args()
    a.cli = shutil.which(a.cli)
    if not a.cli:
        ap.error('CLI executable was not found')
    partitions = a.training_corpus or ['rcorpus']
    try:
        split_audit = audit_training_split(a.work, partitions, a.validation_corpus)
    except (ValueError, OSError) as error:
        ap.error(str(error))
    print(f'split audit: {split_audit["checked_training_mixtures"]} training mixtures, '
          'no exact validation-waveform overlap; source recordings may still be shared')
    datasets = [load(a.work, a.cli, a.model, sub) for sub in partitions]
    if any(len(d[1]) == 0 for d in datasets):
        ap.error('each training partition must contain labelled candidates')
    X, y, fam, _ = [np.concatenate([d[k] for d in datasets]) for k in range(4)]
    Xt, yt, _, _ = load(a.work, a.cli, a.model, a.validation_corpus)
    if len(yt) == 0:
        ap.error('validation partition contains no labelled candidates')
    preserved_dsp = None
    if a.preserve_dsp_from:
        if not a.model:
            ap.error('--preserve-dsp-from requires neural evidence via --model')
        with open(a.preserve_dsp_from) as source:
            match = re.search(r'(// Dsp:.*?)(?=} // namespace purifier)', source.read(), re.DOTALL)
        if not match:
            ap.error('cannot find the DSP tree section in --preserve-dsp-from')
        preserved_dsp = match.group(1).rstrip()
    print(f'training candidates {len(y)} ({y.sum()} real notes), validation candidates {len(yt)} ({yt.sum()} real)')
    hdr = ['// Generated by tools/polyphonic/research/train_purifier.py - do not edit by hand.',
           '// Gradient-boosted trees (depth 3) scoring note candidates as real notes vs harmonic ghosts.',
           '#pragma once', '', 'namespace polyphonic', '{', 'namespace detail', '{', 'namespace purifier', '{', '',
           'struct TreeNode', '{', '    int feature;      // -1: leaf', '    float threshold;  // go left when x <= threshold',
           '    int left, right;', '    float value;      // leaf value', '};', '']
    for name, k in [('Evidence', len(FEATURES)), ('Dsp', N_DSP)]:
        if name == 'Dsp' and preserved_dsp is not None:
            hdr += [preserved_dsp, '']
            continue
        if k > X.shape[1]:
            print(f'skipping {name}: features were dumped without evidence'); continue
        m = gbt().fit(X[:, :k], y)
        report(name, m, X[:, :k], y, fam, Xt[:, :k], yt)
        hdr += [export(m, FEATURES[:k], name), '']
    hdr += ['} // namespace purifier', '} // namespace detail', '} // namespace polyphonic', '']
    with open(a.out, 'w') as target:
        target.write('\n'.join(hdr))
    print('wrote', os.path.abspath(a.out))


if __name__ == '__main__':
    main()
