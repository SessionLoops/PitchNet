"""Compare fixed-corpus evaluation runs with paired, mixture-level uncertainty.

    python compare_evaluations.py BEFORE_DIR AFTER_DIR [--out comparison.json]

Requires NumPy. Confidence intervals resample mixtures, keeping all notes and
both engines' results together. They describe these source recordings and
mixture distributions, not generalization to arbitrary instruments or music.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from evaluate import check_comparison


def statistics(rows):
    totals = np.array([[r['n_gt'], r['n_det'], r['tp_overlap'],
                        r['tp_onset_50ms'], r['tp_onset_100ms'],
                        sum(r['seps']), len(r['seps'])] for r in rows], dtype=float)
    return totals


def measures(totals):
    truth, detected = totals[..., 0], totals[..., 1]
    scores = {}
    for column, label in [(2, 'overlap'), (3, 'onset_50ms'), (4, 'onset_100ms')]:
        tp = totals[..., column]
        scores[f'{label}_precision'] = tp / np.maximum(1, detected)
        scores[f'{label}_recall'] = tp / np.maximum(1, truth)
        scores[f'{label}_f1'] = 2 * tp / np.maximum(1, detected + truth)
    scores['mean_separation_sdr_db'] = totals[..., 5] / np.maximum(1, totals[..., 6])
    return scores


def compare(before, after, replicates=2000, seed=20261102):
    first, second = {r['name']: r for r in before}, {r['name']: r for r in after}
    if len(first) != len(before) or len(second) != len(after) or first.keys() != second.keys() or not first:
        raise ValueError('runs must contain the same nonempty set of unique mixture names')
    names = sorted(first)
    for name in names:
        if first[name]['n_gt'] != second[name]['n_gt'] or len(first[name]['seps']) != len(second[name]['seps']):
            raise ValueError(f'{name}: ground-truth note counts differ')
    a, b = statistics([first[n] for n in names]), statistics([second[n] for n in names])
    original_a, original_b = measures(a.sum(axis=0)), measures(b.sum(axis=0))
    rng = np.random.default_rng(seed)
    draws = rng.integers(0, len(names), size=(replicates, len(names)))
    bootstrap_a, bootstrap_b = measures(a[draws].sum(axis=1)), measures(b[draws].sum(axis=1))
    results = {}
    for key in original_a:
        low, high = np.quantile(bootstrap_b[key] - bootstrap_a[key], [.025, .975])
        results[key] = dict(before=float(original_a[key]), after=float(original_b[key]),
                            difference=float(original_b[key] - original_a[key]),
                            difference_95_percent_interval=[float(low), float(high)])
    return dict(mixtures=len(names), bootstrap_replicates=replicates, bootstrap_seed=seed,
                resampling_unit='paired mixtures; all notes of a mixture stay together', metrics=results)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('before', type=Path)
    ap.add_argument('after', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--replicates', type=int, default=2000)
    ap.add_argument('--seed', type=int, default=20261102)
    a = ap.parse_args()
    if a.replicates < 100:
        ap.error('--replicates must be at least 100')
    manifests = [json.loads((folder / 'manifest.json').read_text()) for folder in (a.before, a.after)]
    if any(m.get('status') != 'complete' for m in manifests):
        ap.error('both runs must be complete')
    check_comparison(manifests[1], manifests[0])
    rows = [json.loads((folder / 'results.json').read_text()) for folder in (a.before, a.after)]
    result = compare(*rows, replicates=a.replicates, seed=a.seed)
    result.update(before_manifest=manifests[0], after_manifest=manifests[1])
    for name in ('overlap_f1', 'onset_50ms_f1', 'onset_100ms_f1', 'mean_separation_sdr_db'):
        r = result['metrics'][name]
        lo, hi = r['difference_95_percent_interval']
        print(f"{name}: {r['before']:.4f} -> {r['after']:.4f}; difference {r['difference']:+.4f} "
              f'(paired mixture bootstrap 95% interval {lo:+.4f} to {hi:+.4f})')
    if a.out:
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
