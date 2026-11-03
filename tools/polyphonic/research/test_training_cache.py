"""Training-cache and split guards, independent of model accuracy or real datasets.

Run with python -m unittest discover -s tools/polyphonic/research -p test_training_cache.py.
Requires the training dependencies, but does not run the analyzer or fit a model.
"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import soundfile as sf

import train_purifier as training


class TrainingCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.corpus = self.root / 'train'
        self.corpus.mkdir()
        self.npz = self.corpus / 'r000.npz'
        self.labels = self.npz.with_suffix('.json')
        self.audio = np.array([0, .125, -.25, .5], dtype=np.float32)
        np.savez(self.npz, mix=self.audio)
        self.meta = dict(insts=['piano'], notes=[dict(midi=60, onset=0, offset=1)])
        self.labels.write_text(json.dumps(self.meta))
        self.cli, self.model = self.root / 'cli', self.root / 'model.onnx'
        self.cli.write_bytes(b'first analyzer binary')
        self.model.write_bytes(b'first evidence model')
        self.inputs = []
        self.mock_run = patch.object(training.subprocess, 'run', side_effect=self.analyzer).start()
        self.addCleanup(patch.stopall)

    def analyzer(self, command, **kwargs):
        audio, rate = sf.read(command[1], dtype='float32')
        self.assertEqual(rate, 44100)
        self.inputs.append(audio.copy())
        width = len(training.FEATURES) if '--model' in command else training.N_DSP
        row = [0, 1, 60, .5, float(audio.mean())] + [0] * (width - 1)
        target = Path(command[command.index('--dump-features') + 1])
        target.write_text('\t'.join(map(str, row)) + '\n')
        return subprocess.CompletedProcess(command, 0, stdout=b'', stderr=b'')

    def load(self, model=True, partition='train'):
        return training.load(str(self.root), str(self.cli), str(self.model) if model else None, partition)

    def test_unchanged_inputs_reuse_features_and_aliases_share_cache(self):
        first = self.load()
        second = self.load(partition='./train')
        self.assertEqual(self.mock_run.call_count, 1)
        for a, b in zip(first, second):
            np.testing.assert_array_equal(a, b)
        self.assertEqual(first[0].shape, (1, 21))
        self.assertEqual(first[1].tolist(), [1])

    def test_binary_model_labels_and_schema_each_invalidate_cache(self):
        self.load()
        self.cli.write_bytes(b'changed analyzer binary')
        self.load()
        self.assertEqual(self.mock_run.call_count, 2)
        self.model.write_bytes(b'changed evidence model')
        self.load()
        self.assertEqual(self.mock_run.call_count, 3)
        self.meta['notes'][0]['midi'] = 64
        self.labels.write_text(json.dumps(self.meta))
        _, labels, _, _ = self.load()
        self.assertEqual(self.mock_run.call_count, 4)
        self.assertEqual(labels.tolist(), [0])
        with patch.object(training, 'FEATURES', ['renamed_feature'] + training.FEATURES[1:]):
            self.load()
        self.assertEqual(self.mock_run.call_count, 5)

    def test_changed_archive_replaces_the_cached_wav(self):
        original = self.load()[0][0, 0]
        replacement = self.audio * -.5
        np.savez(self.npz, mix=replacement)
        updated = self.load()[0][0, 0]
        self.assertEqual(self.mock_run.call_count, 2)
        np.testing.assert_array_equal(self.inputs[0], self.audio)
        np.testing.assert_array_equal(self.inputs[1], replacement)
        cached_wav, rate = sf.read(self.root / 'features/train/r000.wav', dtype='float32')
        np.testing.assert_array_equal(cached_wav, replacement)
        self.assertEqual(rate, 44100)
        self.assertAlmostEqual(updated, -.5 * original)

    def test_evidence_and_dsp_modes_cannot_reuse_each_others_features(self):
        self.assertEqual(self.load()[0].shape, (1, 21))
        self.assertEqual(self.load(model=False)[0].shape, (1, 19))
        self.assertEqual(self.load()[0].shape, (1, 21))
        self.assertEqual(self.mock_run.call_count, 3)

    def test_legacy_or_corrupt_manifests_force_regeneration(self):
        self.load()
        manifest = self.root / 'features/train/r000.tsv.manifest.json'
        manifest.unlink()
        self.load()
        manifest.write_text('{interrupted')
        self.load()
        self.assertEqual(self.mock_run.call_count, 3)

    def test_failed_extraction_does_not_certify_the_new_binary(self):
        self.load()
        manifest = self.root / 'features/train/r000.tsv.manifest.json'
        old_key = manifest.read_text()
        self.cli.write_bytes(b'broken analyzer')
        with patch.object(training.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['cli'])):
            with self.assertRaises(subprocess.CalledProcessError):
                self.load()
        self.assertEqual(manifest.read_text(), old_key)
        self.load()
        self.assertNotEqual(manifest.read_text(), old_key)

    def test_rows_are_loaded_in_filename_order(self):
        np.savez(self.corpus / 'r002.npz', mix=-self.audio)
        (self.corpus / 'r002.json').write_text(json.dumps(self.meta))
        np.savez(self.corpus / 'r001.npz', mix=self.audio * .5)
        (self.corpus / 'r001.json').write_text(json.dumps(self.meta))
        _, _, _, items = self.load()
        self.assertEqual(items.tolist(), ['r000', 'r001', 'r002'])


class TrainingSplitTests(unittest.TestCase):
    def test_alias_symlink_and_copied_audio_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            train, validation = root / 'train', root / 'validation'
            train.mkdir()
            validation.mkdir()
            audio = np.array([0, .125, -.25], dtype=np.float32)
            np.savez(train / 'a.npz', mix=audio)
            np.savez_compressed(validation / 'renamed.npz', mix=audio, stems=audio[None, :])
            (root / 'alias').symlink_to(train, target_is_directory=True)
            for alias in ('train', './train', 'alias', str(train.resolve())):
                with self.subTest(alias=alias):
                    with self.assertRaisesRegex(ValueError, 'validation corpus must differ'):
                        training.audit_training_split(root, ['train'], alias)
            with self.assertRaisesRegex(ValueError, 'distinct directories'):
                training.audit_training_split(root, ['train', './train'], 'validation')
            with self.assertRaisesRegex(ValueError, 'duplicate declared training audio'):
                training.audit_training_split(root, ['train'], 'validation')
            np.savez_compressed(validation / 'renamed.npz', mix=-audio)
            audit = training.audit_training_split(root, ['train'], str(validation.resolve()))
            self.assertEqual(audit['checked_training_mixtures'], 1)
            self.assertEqual(audit['duplicate_training_mixtures'], [])

    def test_empty_partitions_fail_before_feature_extraction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'train').mkdir()
            with self.assertRaisesRegex(ValueError, 'no training mixtures'):
                training.audit_training_split(root, ['train'], 'validation')
            np.savez(root / 'train/a.npz', mix=np.zeros(4, dtype=np.float32))
            with self.assertRaisesRegex(ValueError, 'no validation mixtures'):
                training.audit_training_split(root, ['train'], 'validation')


if __name__ == '__main__':
    unittest.main()
