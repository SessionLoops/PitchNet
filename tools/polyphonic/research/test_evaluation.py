"""Scoring regressions; run with python -m unittest discover -s tools/polyphonic/research -p test_evaluation.py.

These examples test the evaluator's bookkeeping independently of any instrument/model fixtures.
Only NumPy and the Python standard library are required.
"""
import itertools
from pathlib import Path
import tempfile
import unittest

import numpy as np

from evaluate import (MATCHING, audit_split, check_comparison, event_scores, match_events,
                      matches, minimum_cost_matching, summarize)


def truth(onset, offset, midi=60):
    return dict(onset=onset, offset=offset, midi=midi)


def detection(onset, offset, midi=60, idx=0):
    return dict(on=onset, off=offset, midi=midi, idx=idx)


class EventMatchingTests(unittest.TestCase):
    def test_one_merged_detection_cannot_recall_two_events(self):
        gt = [truth(0, 1), truth(1, 2)]
        detected = [detection(0, 2)]
        self.assertEqual(sum(bool(matches(detected, g)) for g in gt), 2)  # coverage only
        pairs = match_events(detected, gt)
        self.assertEqual(len(pairs), 1)
        score = event_scores(len(pairs), len(detected), len(gt))
        self.assertEqual(score['precision'], 1)
        self.assertEqual(score['recall'], 0.5)
        self.assertAlmostEqual(score['f1'], 2 / 3)

    def test_fragments_cannot_all_be_event_true_positives(self):
        gt = [truth(0, 2)]
        detected = [detection(0, 0.9, idx=0), detection(1, 2, idx=1)]
        self.assertEqual(len(matches(detected, gt[0])), 2)  # both remain available for solo summation
        score = event_scores(len(match_events(detected, gt)), len(detected), len(gt))
        self.assertEqual(score['precision'], 0.5)
        self.assertEqual(score['recall'], 1)

    def test_augmenting_path_fixes_greedy_onset_miss(self):
        gt = [truth(0.1, 1), truth(0.18, 1.1)]
        detected = [detection(0.14, 1.1), detection(0.06, 1)]
        self.assertEqual(match_events(detected, gt, 0.05), [(0, 1), (1, 0)])
        # F1 cannot depend on the order annotations or detections were written.
        for ordering in itertools.permutations(detected):
            self.assertEqual(len(match_events(ordering, list(reversed(gt)), 0.05)), 2)

    def test_pitch_onset_and_overlap_all_required(self):
        gt = [truth(0, 10)]
        self.assertEqual(len(match_events([detection(0.05, 1, 60.5)], gt, 0.05)), 1)
        for bad in [detection(0.05001, 1), detection(0, 1, 60.50001),
                    detection(7, 17), detection(0, 0), detection(0, -1), detection(11, 12)]:
            self.assertEqual(match_events([bad], gt, 0.05), [])
        self.assertEqual(match_events([detection(7, 17)], gt), [])  # exactly 30% is insufficient

    def test_empty_partitions(self):
        self.assertEqual(match_events([], [truth(0, 1)]), [])
        self.assertEqual(match_events([detection(0, 1)], []), [])
        self.assertEqual(event_scores(0, 0, 0)['f1'], 0)

    def test_cost_can_reassign_previously_cheapest_pair(self):
        edges = [(0, 0, 0), (0, 1, 1), (1, 0, 1), (1, 1, 100)]
        self.assertEqual(minimum_cost_matching(2, 2, edges), [(0, 1), (1, 0)])
        ties = [(i, j, 0) for i in range(3) for j in range(3)]
        first = minimum_cost_matching(3, 3, ties)
        self.assertEqual(first, minimum_cost_matching(3, 3, list(reversed(ties))))
        self.assertEqual(len(first), 3)

    def test_matches_exhaustive_optimum_on_small_graphs(self):
        rng = np.random.default_rng(742)
        for _ in range(100):
            nl, nr = map(int, rng.integers(1, 5, size=2))
            costs = {(i, j): int(rng.integers(0, 10)) for i in range(nl) for j in range(nr)
                     if rng.random() < 0.65}
            best = (1, float('inf'))
            for assignment in itertools.product(range(-1, nr), repeat=nl):
                used = [j for j in assignment if j >= 0]
                if len(used) != len(set(used)) or any((i, j) not in costs for i, j in enumerate(assignment) if j >= 0):
                    continue
                candidate = (-len(used), sum(costs[(i, j)] for i, j in enumerate(assignment) if j >= 0))
                best = min(best, candidate)
            result = minimum_cost_matching(nl, nr, [(i, j, c) for (i, j), c in costs.items()])
            self.assertEqual((-len(result), sum(costs[p] for p in result)), best)

    def test_summary_separates_event_f1_from_fragment_coverage(self):
        row = dict(n_gt=2, n_det=1, tp_overlap=1, tp_onset_50ms=1, tp_onset_100ms=1,
                   coverage_notes_found=2, coverage_detections_eligible=1, seps=[0, 2])
        summary = summarize([row])
        self.assertEqual(summary['fragment_coverage']['ground_truth_events_covered'], 1)
        self.assertEqual(summary['event_overlap']['recall'], 0.5)
        self.assertFalse(summary['fragment_coverage']['one_to_one'])
        self.assertEqual(summary['separation_sdr_db']['mean'], 1)


class HeldOutControlTests(unittest.TestCase):
    def test_training_audio_rejected_even_when_archive_layout_differs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            train, test = root / 'train', root / 'test'
            train.mkdir()
            test.mkdir()
            audio = np.asarray([0, 0.5, -0.5], dtype=np.float32)
            np.savez(train / 'a.npz', mix=audio)  # training usually omits stems
            np.savez(test / 'b.npz', mix=audio, stems=audio[None, :])
            with self.assertRaisesRegex(ValueError, 'duplicate declared training audio'):
                audit_split([test / 'b.npz'], [train], 'test')
            audit = audit_split([test / 'b.npz'], [train], 'diagnostic')
            self.assertEqual(len(audit['duplicate_training_mixtures']), 1)
            with self.assertRaisesRegex(ValueError, 'declared training corpus'):
                audit_split([train / 'a.npz'], [train], 'validation')
            np.savez(test / 'b.npz', mix=-audio, stems=-audio[None, :])
            self.assertEqual(audit_split([test / 'b.npz'], [train], 'test')['duplicate_training_mixtures'], [])

    def test_missing_training_data_is_recorded_without_claiming_audit(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit = audit_split([], [Path(tmp) / 'missing'], 'test')
            self.assertEqual(audit['checked_training_mixtures'], 0)
            self.assertFalse(audit['training_corpora'][0]['available'])
            self.assertIn('not established', audit['held_out_scope'])

    def test_comparison_holds_data_model_and_rules_fixed(self):
        manifest = dict(corpus_inputs=[dict(name='a', npz_sha256='x', labels_sha256='y')],
                        model_sha256='m', matching=MATCHING, purpose='test')
        baseline = dict(manifest, status='complete', cli_sha256='different-engine-is-allowed')
        check_comparison(manifest, baseline)
        for key in ('corpus_inputs', 'model_sha256', 'matching', 'purpose'):
            with self.assertRaisesRegex(ValueError, key):
                check_comparison(manifest, dict(baseline, **{key: None}))
        with self.assertRaisesRegex(ValueError, 'completed'):
            check_comparison(manifest, dict(baseline, status='running'))


if __name__ == '__main__':
    unittest.main()
