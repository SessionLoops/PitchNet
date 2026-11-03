"""Evaluate polyphonic_cli on mixtures with known stems.

    python evaluate.py --work WORK --cli build/polyphonic_cli [--model basic_pitch_nmp.onnx] [--corpus tcorpus]

Detection: maximum-cardinality, one-to-one event matching requires pitch within half a semitone
and temporal overlap greater than 30% of the shorter event (also onset within 50 / 100 ms for
the onset scores). A merged detection cannot count as two notes, and duplicate fragments are
false positives. Many-to-many fragment coverage is reported separately; separation sums those
fragments against each true stem, without using that coverage as event precision or recall.
For corpora whose JSON lists edits (the hand-made corpus),
also: pitch edit vs the same edit applied to the isolated stem ("oracle", spectral SNR), mute vs the
mix without that stem (SDR) and a 0.25 s move vs the mix with that stem moved (spectral SNR).
For repeated notes, the second strike must have its own one-to-one match within 60 ms.

--out keeps runs from different builds separate. Each run saves input/model/CLI hashes, matching
rules and split-audit results. --compare-to RUN_DIR requires the same corpus, model and rules as
a previous run. Declared training mixtures cannot be presented as a test/validation set; use
--purpose diagnostic for in-sample checks. The generated corpora hold out mixtures, not source
recordings or instruments, so they do not establish generalization to unseen recordings.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import numpy as np

from audio_metrics import magsnr, read_wav, write_float_wav

FS = 44100
MATCHING = dict(pitch_tolerance_semitones=0.5, overlap_fraction_of_shorter=0.3,
                overlap_comparison='strictly greater', onset_tolerances_seconds=[0.05, 0.1],
                assignment='maximum cardinality, then minimum total onset/pitch/overlap cost',
                separation='sum all eligible fragments; coverage is not event recall')


def sdr(ref, est):
    return 10 * np.log10(np.sum(ref ** 2) / (np.sum((ref - est) ** 2) + 1e-20) + 1e-20)


def read_mono(path):
    x, fs = read_wav(path)
    if fs != FS or x.shape[1] != 1:
        raise ValueError(f'{path}: expected mono audio at {FS} Hz')
    return x[:, 0]


class Cli:
    def __init__(self, cli, model):
        self.cli, self.model = cli, model

    def __call__(self, wav, *args):
        cmd = [self.cli, wav, '--json', wav + '.json'] + (['--model', self.model] if self.model else []) + list(args)
        subprocess.run(cmd, check=True, capture_output=True)
        js = json.loads(Path(wav + '.json').read_text())
        hop, fs = js['hop'], js['sampleRate']
        return [dict(idx=i, on=n['start'] * hop / fs, off=n['end'] * hop / fs, midi=69 + 12 * np.log2(np.median(n['f0']) / 440))
                for i, n in enumerate(js['notes']) if n['active']]


def eligible(d, g, onset_tolerance=None):
    """Eligibility shared by event matching and the separate fragment-coverage measure."""
    duration = min(g['offset'] - g['onset'], d['off'] - d['on'])
    overlap = min(g['offset'], d['off']) - max(g['onset'], d['on'])
    return (duration > 0 and abs(d['midi'] - g['midi']) <= MATCHING['pitch_tolerance_semitones']
            and overlap > MATCHING['overlap_fraction_of_shorter'] * duration
            and (onset_tolerance is None or abs(d['on'] - g['onset']) <= onset_tolerance))


def matches(det, g):
    """All eligible fragments for separation/coverage, never an event TP count."""
    return [d for d in det if eligible(d, g)]


def minimum_cost_matching(num_left, num_right, edges):
    """Return deterministic (left, right) pairs with maximum cardinality then minimum cost.

    Successive shortest augmenting paths in a unit-capacity residual graph. Bellman-Ford
    handles the negative-cost reverse edges that let a later assignment correct an earlier
    choice. Corpus chords have few events, so this dependency-free implementation is small
    and sufficient. Sorted edges and strict improvements make equal-cost ties stable.
    """
    if num_left < 0 or num_right < 0:
        raise ValueError('partition sizes must be nonnegative')
    source, sink = num_left + num_right, num_left + num_right + 1
    graph = [[] for _ in range(sink + 1)]

    def add(a, b, cost):
        forward = [b, len(graph[b]), 1, float(cost)]
        reverse = [a, len(graph[a]), 0, -float(cost)]
        graph[a].append(forward)
        graph[b].append(reverse)
        return forward

    for left in range(num_left):
        add(source, left, 0)
    tracked = []
    for left, right, cost in sorted(edges):
        if not (0 <= left < num_left and 0 <= right < num_right) or not np.isfinite(cost) or cost < 0:
            raise ValueError('invalid matching edge')
        tracked.append((left, right, add(left, num_left + right, cost)))
    for right in range(num_right):
        add(num_left + right, sink, 0)
    while True:
        distance = [float('inf')] * len(graph)
        previous = [None] * len(graph)
        distance[source] = 0.0
        for _ in range(len(graph) - 1):
            changed = False
            for a, outgoing in enumerate(graph):
                for index, (b, _, capacity, cost) in enumerate(outgoing):
                    candidate = distance[a] + cost
                    if capacity and candidate < distance[b] - 1e-12:
                        distance[b] = candidate
                        previous[b] = (a, index)
                        changed = True
            if not changed:
                break
        if previous[sink] is None:
            break
        b = sink
        while b != source:
            a, index = previous[b]
            edge = graph[a][index]
            edge[2] = 0
            graph[b][edge[1]][2] = 1
            b = a
    return [(left, right) for left, right, edge in tracked if edge[2] == 0]


def match_events(det, truth, onset_tolerance=None):
    """One-to-one event matches; cost resolves ambiguity without changing the TP count."""
    edges = []
    for gi, g in enumerate(truth):
        for di, d in enumerate(det):
            if not eligible(d, g, onset_tolerance):
                continue
            overlap = min(g['offset'], d['off']) - max(g['onset'], d['on'])
            union = max(g['offset'], d['off']) - min(g['onset'], d['on'])
            # Timing in seconds dominates typical pitch/overlap tie breaks. This cost only
            # selects among maximum-cardinality assignments, so it cannot inflate event F1.
            cost = abs(d['on'] - g['onset']) + 0.01 * abs(d['midi'] - g['midi']) + 0.001 * (1 - overlap / union)
            edges.append((gi, di, cost))
    return minimum_cost_matching(len(truth), len(det), edges)


def event_scores(tp, detected, truth):
    precision, recall = tp / max(1, detected), tp / max(1, truth)
    return dict(tp=tp, detected=detected, truth=truth, precision=precision, recall=recall,
                f1=2 * tp / max(1, detected + truth))


def wav_of(x, path):
    write_float_wav(path, x, FS)
    return path


def run_item(npz, tmp, cli):
    name = os.path.basename(npz)[:-4]
    with np.load(npz) as d:
        mix, stems = d['mix'].astype(np.float64), d['stems'].astype(np.float64)
    meta = json.loads(Path(npz[:-4] + '.json').read_text())
    if (mix.ndim != 1 or stems.shape != (len(meta['notes']), len(mix))
            or not np.all(np.isfinite(mix)) or not np.all(np.isfinite(stems))
            or not np.allclose(mix, stems.sum(axis=0), rtol=1e-5, atol=1e-6)):
        raise ValueError(f'{npz}: finite mono mix and matching additive ground-truth stems required')
    for g in meta['notes']:
        if (not all(np.isfinite(g[k]) for k in ('midi', 'onset', 'offset'))
                or not 0 <= g['onset'] < g['offset'] <= len(mix) / FS + 1e-4):
            raise ValueError(f'{npz}: invalid ground-truth note event')
    wav = wav_of(mix, f'{tmp}/{name}.wav')
    solo = f'{tmp}/{name}_solo'
    os.makedirs(solo, exist_ok=True)
    for f in os.listdir(solo):
        os.remove(os.path.join(solo, f))
    det = cli(wav, '--solo-all', solo)
    r = dict(name=name, n_gt=len(meta['notes']), n_det=len(det),
             instruments=[g.get('inst', 'unknown') for g in meta['notes']])
    r['event_matches'] = match_events(det, meta['notes'])
    r['tp_overlap'] = len(r['event_matches'])
    for tol in (0.05, 0.1):
        r[f'tp_onset_{round(tol * 1000)}ms'] = len(match_events(det, meta['notes'], tol))
    ok, found, seps, mt = set(), 0, [], {}
    for k, g in enumerate(meta['notes']):
        ms = matches(det, g)
        mt[k] = [dd['idx'] for dd in ms]
        found += bool(ms)
        ok.update(mt[k])
        est = sum((read_mono(f'{solo}/note_{i}.wav') for i in mt[k]), np.zeros_like(mix))
        seps.append(sdr(stems[k], est))
    r.update(coverage_notes_found=found, coverage_detections_eligible=len(ok), seps=seps)
    if 'repeated' in meta:
        assigned = match_events(det, meta['notes'], 0.06)
        r['restrike'] = any(gi == meta['repeated'] for gi, _ in assigned)
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
        oracle = mix - st + read_mono(swav + '.edit.wav')
        y = read_mono(f'{tmp}/{name}_edit.wav')
        r.update(edit=magsnr(oracle, y), edit_base=magsnr(oracle, mix))
        cli(wav, *sum([['--edit', f'{i}:0:0:mute'] for i in mt[k]], []), '--out', f'{tmp}/{name}_mute.wav')
        r.update(mute=sdr(mix - st, read_mono(f'{tmp}/{name}_mute.wav')), mute_base=sdr(mix - st, mix))
        cli(wav, *sum([['--move', f'{i}:0.25'] for i in mt[k]], []), '--out', f'{tmp}/{name}_move.wav')
        sh = int(0.25 * FS)
        moved = mix - st + np.concatenate([np.zeros(sh), st[:-sh]])
        r.update(move=magsnr(moved, read_mono(f'{tmp}/{name}_move.wav')), move_base=magsnr(moved, mix))
        cli(wav, '--out', f'{tmp}/{name}_identity.wav')
        r['identity'] = sdr(mix, read_mono(f'{tmp}/{name}_identity.wav'))
    return r


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def mixture_hash(path):
    with np.load(path) as data:
        mix = np.ascontiguousarray(data['mix'], dtype='<f4')
    return hashlib.sha256(str(mix.shape).encode() + mix.tobytes()).hexdigest()


def audit_split(paths, training_corpora, purpose):
    """Detect exact waveform leakage; this cannot establish source-recording independence."""
    training, corpora = {}, []
    evaluation_dir = paths[0].parent.resolve() if paths else None
    for folder in training_corpora:
        folder = Path(folder).resolve()
        if purpose != 'diagnostic' and folder == evaluation_dir:
            raise ValueError('a declared training corpus requires --purpose diagnostic')
        files = sorted(folder.glob('*.npz'))
        corpora.append(dict(path=str(folder), available=folder.is_dir(), mixtures=len(files)))
        for path in files:
            training.setdefault(mixture_hash(path), []).append(str(path))
    duplicates = [dict(evaluation=str(path), training=training[h])
                  for path in paths if (h := mixture_hash(path)) in training]
    if duplicates and purpose != 'diagnostic':
        raise ValueError(f'{len(duplicates)} evaluation mixtures duplicate declared training audio; '
                         'use a disjoint corpus, or --purpose diagnostic for in-sample checks')
    return dict(training_corpora=corpora, checked_training_mixtures=sum(c['mixtures'] for c in corpora),
                duplicate_training_mixtures=duplicates,
                held_out_scope='mixtures only; unseen source recordings/instruments are not established')


def check_comparison(manifest, baseline):
    """Require fixed data, evidence model and scoring rules across engine comparisons."""
    if baseline.get('status') != 'complete':
        raise ValueError('comparison run must have completed successfully')
    for key in ('corpus_inputs', 'model_sha256', 'matching', 'purpose'):
        if manifest[key] != baseline.get(key):
            raise ValueError(f'comparison mismatch in {key}; use identical data, model and scoring rules')


def summarize(rows):
    truth, detected = sum(r['n_gt'] for r in rows), sum(r['n_det'] for r in rows)
    summary = dict(mixtures=len(rows), event_overlap=event_scores(sum(r['tp_overlap'] for r in rows), detected, truth))
    for tol in (50, 100):
        summary[f'event_onset_{tol}ms'] = event_scores(sum(r[f'tp_onset_{tol}ms'] for r in rows), detected, truth)
    summary['fragment_coverage'] = dict(
        ground_truth_events_covered=sum(r['coverage_notes_found'] for r in rows) / max(1, truth),
        detections_eligible_for_any_truth=sum(r['coverage_detections_eligible'] for r in rows) / max(1, detected),
        one_to_one=False)
    separation = np.asarray([score for row in rows for score in row['seps']])
    if separation.size:
        summary['separation_sdr_db'] = dict(mean=float(separation.mean()), median=float(np.median(separation)),
                                            reference='ground-truth stems; all eligible fragments summed')
    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--model')
    ap.add_argument('--corpus', default='tcorpus')
    ap.add_argument('--out', type=Path, help='separate output directory for this build/run')
    ap.add_argument('--purpose', choices=('test', 'validation', 'diagnostic'), default='test')
    ap.add_argument('--training-corpus', action='append', help='training folder, relative to --work or absolute; repeatable (default: rcorpus)')
    ap.add_argument('--compare-to', type=Path, help='completed run directory whose data/model/scoring must match')
    a = ap.parse_args()
    corpus = Path(a.work) / a.corpus
    paths = sorted(corpus.glob('*.npz'))
    if not paths:
        ap.error(f'no mixtures found in {corpus}')
    tmp = a.out or Path(a.work) / 'eval' / f"{a.corpus}-{'evidence' if a.model else 'dsp'}"
    if a.compare_to and tmp.resolve() == a.compare_to.resolve():
        ap.error('--out must differ from --compare-to')
    executable = shutil.which(a.cli)
    if not executable:
        ap.error(f'CLI executable not found: {a.cli}')
    training = [Path(a.work) / name for name in (a.training_corpus or ['rcorpus'])]
    try:
        manifest = dict(status='running', purpose=a.purpose, corpus_path=str(corpus.resolve()),
                        corpus_inputs=[dict(name=p.name, npz_sha256=file_hash(p),
                                            labels_sha256=file_hash(p.with_suffix('.json'))) for p in paths],
                        cli_path=str(Path(executable).resolve()), cli_sha256=file_hash(executable),
                        model_path=str(Path(a.model).resolve()) if a.model else None,
                        model_sha256=file_hash(a.model) if a.model else None,
                        matching=MATCHING, numpy_version=np.__version__,
                        split_audit=audit_split(paths, training, a.purpose))
        if a.compare_to:
            check_comparison(manifest, json.loads((a.compare_to / 'manifest.json').read_text()))
    except (ValueError, OSError) as error:
        ap.error(str(error))
    tmp.mkdir(parents=True, exist_ok=True)
    (tmp / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    cli = Cli(executable, a.model)
    rows = [run_item(str(p), str(tmp), cli) for p in paths]
    G = sum(r['n_gt'] for r in rows)
    D = sum(r['n_det'] for r in rows)
    summary = summarize(rows)
    print(f'{a.corpus}: {len(rows)} mixtures, {G} notes, {D} detected ({"with" if a.model else "without"} Basic Pitch evidence)')
    for key, label in [('event_overlap', 'pitch + overlap'), ('event_onset_50ms', 'pitch + overlap + onset within 50 ms'),
                       ('event_onset_100ms', 'pitch + overlap + onset within 100 ms')]:
        score = summary[key]
        print(f'  note events ({label}, one-to-one): precision {score["precision"]:.3f} recall {score["recall"]:.3f} F1 {score["f1"]:.3f}')
    coverage = summary['fragment_coverage']
    print(f'  fragment coverage (many-to-many, not event F1): truth covered {coverage["ground_truth_events_covered"]:.3f}; '
          f'detections eligible {coverage["detections_eligible_for_any_truth"]:.3f}')
    if 'separation_sdr_db' in summary:
        score = summary['separation_sdr_db']
        print(f'  separation SDR (all eligible fragments): mean {score["mean"]:.2f} dB, median {score["median"]:.2f} dB')
    repeats = [r for r in rows if 'restrike' in r]
    if repeats:
        print(f'  repeated notes: second strike has its own match in {sum(r["restrike"] for r in repeats)} of {len(repeats)}')
    edited = [r for r in rows if 'edit' in r]
    if edited:
        for key in ('edit', 'mute', 'move'):
            print(f'  {key:5s}: {np.mean([r[key] for r in edited]):5.1f} dB (doing nothing: {np.mean([r[key + "_base"] for r in edited]):5.1f} dB)')
        print('  pitch-edit reference uses the same engine on an isolated stem; it measures edit consistency, not independent timbre fidelity')
        print(f'  identity render SDR: min {min(r["identity"] for r in edited):.0f} dB')
    print(f'  split audit: checked {manifest["split_audit"]["checked_training_mixtures"]} declared training mixtures; '
          + manifest['split_audit']['held_out_scope'])
    (tmp / 'results.json').write_text(json.dumps(rows, indent=1) + '\n')
    (tmp / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    manifest['status'] = 'complete'
    (tmp / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
