"""Evaluate the locally available recorded examples without downloading a corpus.

Requires numpy. See README.md for the command and the limits of these checks.
--baseline-cli must include --pitch-curve (link the current CLI to the old engine
library when comparing an engine from before that CLI option was added).
"""
import argparse
import json
from pathlib import Path
import subprocess

import numpy as np

from audio_metrics import read_wav, magsnr


EXAMPLES = {
    '01-piano-triad-C-major': [60, 64, 67],
    '02-piano-arpeggio': [48, 64, 67, 72],
    '03-strings-vibrato': [60, 64, 67, 72],
    '04-woodwind-quartet': [48, 64, 79, 84],
    '05-organ-chord': [60, 64, 67],
    '06-harp-rolled-chord': [48, 52, 55, 72, 76],
    '07-piano-octave-hard-case': [48, 60, 67],
    '09-clarinet-short-note': [62, 67, 77],
}


class Runner:
    def __init__(self, cli, model, out):
        self.cli, self.model, self.out = str(cli), str(model) if model else None, out
        out.mkdir(parents=True, exist_ok=True)

    def run(self, wav, tag, *args):
        js = self.out / (tag + '.json')
        cmd = [self.cli, str(wav), '--json', str(js)]
        if self.model:
            cmd += ['--model', self.model]
        result = subprocess.run(cmd + list(map(str, args)), capture_output=True, text=True, check=True)
        (self.out / (tag + '.log')).write_text(result.stderr + result.stdout)
        return json.loads(js.read_text())


def pitch(note):
    return float(69 + 12 * np.log2(np.median(note['f0']) / 440))


def target_notes(js, midi):
    return [(i, n) for i, n in enumerate(js['notes']) if n['active'] and abs(pitch(n) - midi) <= 0.5]


def waveform_snr(ref, est):
    return float(10 * np.log10((np.sum(ref ** 2) + 1e-30) / (np.sum((ref - est) ** 2) + 1e-30)))


def evaluate(runner, audio):
    rows, analyses = {}, {}
    for name, expected in EXAMPLES.items():
        wav = audio / (name + '.wav')
        x, fs = read_wav(wav)
        identity = runner.out / (name + '-identity.wav')
        js = runner.run(wav, name, '--out', identity)
        analyses[name] = js
        y, yfs = read_wav(identity)
        if yfs != fs or y.shape != x.shape:
            raise ValueError(f'identity render shape/rate mismatch: {name}')
        detected = [round(pitch(n), 2) for n in js['notes'] if n['active']]
        rows[name] = dict(expected=expected, detected=detected,
                          expected_pitches_found=sum(bool(target_notes(js, m)) for m in expected),
                          identity_max_error=float(np.max(np.abs(y - x))))

    recording = audio / '08-brahms-hungarian-dance-5-strings.wav'
    if recording.exists():
        x, fs = read_wav(recording)
        output = runner.out / 'brahms-identity.wav'
        js = runner.run(recording, 'brahms', '--out', output)
        y, yfs = read_wav(output)
        if yfs != fs or y.shape != x.shape:
            raise ValueError('Brahms identity render shape/rate mismatch')
        rows['brahms_recording'] = dict(detected_notes=sum(n['active'] for n in js['notes']),
                                       duration_seconds=len(x) / fs,
                                       identity_max_error=float(np.max(np.abs(y - x))))

    # Independent, actually played references; these are not engine-generated oracles.
    name = '01-piano-triad-C-major'
    edits = sum((['--edit', f'{i}:-1'] for i, _ in target_notes(analyses[name], 64)), [])
    if not edits:
        raise ValueError('piano E4 was not detected')
    output = runner.out / 'piano-minor.wav'
    runner.run(audio / (name + '.wav'), 'piano-minor', *edits, '--out', output)
    reference, fs = read_wav(audio / '01b-piano-triad-C-minor-played.wav')
    original, _ = read_wav(audio / (name + '.wav'))
    edited, _ = read_wav(output)
    rows['played_piano_minor'] = dict(edited_spectral_snr_db=magsnr(reference, edited),
                                      unedited_spectral_snr_db=magsnr(reference, original))

    name = '09-clarinet-short-note'
    candidates = target_notes(analyses[name], 77)
    if not candidates:
        raise ValueError('clarinet F5 was not detected')
    index, _ = max(candidates, key=lambda p: p[1]['end'] - p[1]['start'])
    output = runner.out / 'clarinet-long.wav'
    runner.run(audio / (name + '.wav'), 'clarinet-long', '--stretch', f'{index}:2.08', '--out', output)
    reference, _ = read_wav(audio / '09b-clarinet-note-played-longer.wav')
    original, _ = read_wav(audio / (name + '.wav'))
    edited, _ = read_wav(output)
    rows['played_clarinet_long'] = dict(edited_spectral_snr_db=magsnr(reference, edited),
                                        unedited_spectral_snr_db=magsnr(reference, original))

    # Equivalent pitch curves must converge as epsilon -> 0. This is a numerical
    # continuity check on real recordings, not a perceptual-quality reference.
    for name, midi in [('05-organ-chord', 64), ('03-strings-vibrato', 64),
                       ('04-woodwind-quartet', 64), ('01-piano-triad-C-major', 64)]:
        js = analyses[name]
        candidates = target_notes(js, midi)
        if not candidates:
            continue
        index, note = max(candidates, key=lambda p: p[1]['end'] - p[1]['start'])
        original, _ = read_wav(audio / (name + '.wav'))
        length = note['end'] - note['start'] + 1
        t = np.arange(length) / max(1, length - 1)
        curve = np.interp(t, [0, .2, .35, .7, .85, 1], [-1, -1, 0, 0, 1, 1])
        near = curve.copy()
        near[curve == 0] = 1e-7
        outputs = []
        for label, values in [('zero', curve), ('epsilon', near)]:
            curve_file = runner.out / f'{name}-curve-{label}.txt'
            np.savetxt(curve_file, values, fmt='%.10g')
            output = runner.out / f'{name}-curve-{label}.wav'
            runner.run(audio / (name + '.wav'), f'{name}-curve-{label}',
                       '--pitch-curve', f'{index}:{curve_file}', '--out', output)
            outputs.append(read_wav(output)[0])
            if np.max(np.abs(outputs[-1] - original)) < 1e-8:
                raise ValueError(f'{runner.cli}: nonzero pitch curve had no effect; CLI must support --pitch-curve')
        rows[name]['curve_zero_vs_epsilon_snr_db'] = waveform_snr(outputs[1], outputs[0])
        rows[name]['curve_zero_vs_epsilon_max_error'] = float(np.max(np.abs(outputs[1] - outputs[0])))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', required=True, type=Path)
    ap.add_argument('--baseline-cli', type=Path)
    ap.add_argument('--model', type=Path)
    ap.add_argument('--audio-dir', type=Path, default=Path(__file__).resolve().parents[1] / 'test-audio')
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    results = {}
    for label, cli in [('baseline', a.baseline_cli), ('current', a.cli)]:
        if cli is None:
            continue
        rows = evaluate(Runner(cli.resolve(), a.model, a.out / label), a.audio_dir)
        results[label] = rows
        print(label, flush=True)
        for name, row in rows.items():
            if 'edited_spectral_snr_db' in row:
                print(f"  {name}: {row['edited_spectral_snr_db']:.2f} dB (unedited {row['unedited_spectral_snr_db']:.2f})")
            elif 'expected' in row:
                print(f"  {name}: {row['expected_pitches_found']}/{len(row['expected'])} expected pitches; "
                      f"identity error {row['identity_max_error']:.2g}" +
                      (f"; curve continuity {row['curve_zero_vs_epsilon_snr_db']:.1f} dB"
                       if 'curve_zero_vs_epsilon_snr_db' in row else ''))
            else:
                print(f"  {name}: {row['detected_notes']} notes (unlabelled); identity error {row['identity_max_error']:.2g}")
    (a.out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
