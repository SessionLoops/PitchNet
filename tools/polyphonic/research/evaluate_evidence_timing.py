"""Compare Basic Pitch evidence alignment on repeated real instrument recordings.

The fixture repeats the supplied piano arpeggio for 90 seconds, with a separate
triad entering at 89.4 seconds. The final triad tests tail coverage: rounding the
model output to 86 frames/second used to discard about 0.6 seconds of evidence.
The recordings are local test examples, not synthesized tones. Requires NumPy.

    python evaluate_evidence_timing.py --baseline-cli /tmp/before/polyphonic_cli \
        --cli /tmp/after/polyphonic_cli --model ../../../Resources/models/basic_pitch_nmp.onnx \
        --audio-dir ../test-audio --out /tmp/evidence-timing
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

import numpy as np

from audio_metrics import read_wav, write_float_wav


MODEL_RATE = 22050
MODEL_HOP = 256
WINDOW_SAMPLES = 43844
WINDOW_FRAMES = 172
OVERLAP_FRAMES = 30
KEEP_FRAMES = WINDOW_FRAMES - OVERLAP_FRAMES
WINDOW_STEP = WINDOW_SAMPLES - OVERLAP_FRAMES * MODEL_HOP


def read_evidence(path):
    data = Path(path).read_bytes()
    frames = struct.unpack_from('<i', data)[0]
    if frames < 0 or len(data) != 4 + frames * 89 * 4:
        raise ValueError(f'{path}: invalid evidence dump')
    times = np.frombuffer(data, dtype='<f4', count=frames, offset=4).astype(np.float64)
    notes = np.frombuffer(data, dtype='<f4', offset=4 + frames * 4).reshape(frames, 88)
    return times, notes


def make_fixture(audio_dir, path):
    arpeggio, rate = read_wav(audio_dir / '02-piano-arpeggio.wav')
    triad, triad_rate = read_wav(audio_dir / '01-piano-triad-C-major.wav')
    if rate != triad_rate:
        raise ValueError('fixture recordings must have the same sample rate')
    audio = np.zeros((90 * rate, 1), dtype=np.float32)
    onsets = []
    # Leave the last arpeggio slot empty so the final triad follows silence.
    for repetition in range(19):
        start = int(repetition * 4.5 * rate)
        audio[start:start + len(arpeggio), 0] = arpeggio.mean(axis=1)
        for midi, onset in ((48, .05), (64, .35), (67, .65), (72, .95)):
            onsets.append(dict(repetition=repetition, midi=midi, onset=repetition * 4.5 + onset))
    tail_start = 89.4
    start = round(tail_start * rate)
    audio[start:, 0] = triad[:len(audio) - start].mean(axis=1)
    write_float_wav(path, audio, rate)
    return onsets, dict(start=tail_start, onset=tail_start + .05, end=90.0, midi=[60, 64, 67])


def evaluate(cli, model, fixture, out, label, onsets, tail):
    evidence_path = out / f'{label}.ev'
    process = subprocess.run([str(cli), str(fixture), '--model', str(model), '--dump-evidence',
                              str(evidence_path), '--json', str(out / f'{label}-notes.json')],
                             check=True, text=True, capture_output=True)
    (out / f'{label}.log').write_text(process.stdout + process.stderr)
    times, notes = read_evidence(evidence_path)
    if not len(times):
        raise ValueError(f'{label}: CLI returned no neural evidence')

    # Every retained frame is located from its actual input-window start. The
    # initial half-overlap padding cancels the first retained frame's index15.
    frame = np.arange(len(times))
    expected = ((frame // KEEP_FRAMES) * WINDOW_STEP + (frame % KEEP_FRAMES) * MODEL_HOP) / MODEL_RATE
    errors = (times - expected) * 1000
    onset_rows = []
    for event in onsets:
        region = np.flatnonzero((times >= event['onset'] - .2) & (times <= event['onset'] + .3))
        if not len(region):
            continue
        values = notes[region, event['midi'] - 21]
        peak = float(values.max())
        hits = region[values >= max(.05, .5 * peak)]
        onset_rows.append(dict(**event, peak=peak,
                               error_ms=float((times[hits[0]] - event['onset']) * 1000) if len(hits) else None))
    tail_region = np.flatnonzero((times >= tail['onset'] + .05) & (times < tail['end']))
    tail_peaks = {str(midi): float(notes[tail_region, midi - 21].max()) if len(tail_region) else None
                  for midi in tail['midi']}
    return dict(frames=len(times), first_time_seconds=float(times[0]), last_time_seconds=float(times[-1]),
                max_timestamp_error_ms=float(np.max(np.abs(errors))),
                median_timestamp_error_ms=float(np.median(np.abs(errors))),
                tail_frames=len(tail_region), tail_note_peak_posteriors=tail_peaks, onsets=onset_rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-cli', required=True, type=Path)
    parser.add_argument('--cli', required=True, type=Path)
    parser.add_argument('--model', required=True, type=Path)
    parser.add_argument('--audio-dir', type=Path, default=Path(__file__).resolve().parent.parent / 'test-audio')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    fixture = args.out / 'repeated-piano-90s.wav'
    onsets, tail = make_fixture(args.audio_dir, fixture)
    result = dict(fixture=dict(duration_seconds=90, repetitions=19, tail=tail),
                  baseline=evaluate(args.baseline_cli, args.model, fixture, args.out, 'baseline', onsets, tail),
                  current=evaluate(args.cli, args.model, fixture, args.out, 'current', onsets, tail))
    (args.out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    for label in ('baseline', 'current'):
        row = result[label]
        print(f"{label}: {row['frames']} frames through {row['last_time_seconds']:.4f}s; "
              f"max timestamp error {row['max_timestamp_error_ms']:.3f}ms; "
              f"final triad evidence: {row['tail_frames']} frames, peaks {row['tail_note_peak_posteriors']}")
    print(f'Artifacts: {args.out.resolve()}')


if __name__ == '__main__':
    main()
