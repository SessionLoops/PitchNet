# Polyphonic real-audio regression check

This report records the first, correctness-only stage: the initial polyphonic
implementation at `0406247` compared with the evidence-timing and pitch-curve
phase fixes, before classifier retraining or later renderer changes. The
existing harmonic analysis, learned note filter and NMF masks were retained.
The figures below belong to that stage. See the subsequent
[quality iteration report](polyphonic-quality-iteration.md) for the expanded
recorded-audio evaluation, retrained classifier and current results. Neither
stage establishes quality parity with a commercial polyphonic editor.

## Changes

- `BasicPitchEvidence` now timestamps retained frames from their input window's
  actual sample position. Each window advances 36,164 samples at 22,050 Hz and
  contributes 142 frames, spaced 256 samples apart within that window. The
  half-overlap padding cancels the 15 discarded left frames. Retention follows
  source sample positions through the end of the recording, replacing the
  rounded 86-frames-per-second length limit and the unrelated 172-frame timing
  correction. This also retains one evidence frame for clips shorter than a hop.
- The renderer retains accumulated phase when a pitch curve returns to exactly
  zero, and records those frames so a later pitch change only integrates over
  its own duration. The former branch reset phase at zero and skipped the
  intervening frames. Constant pitch shifts, gain, mute and identity renders
  keep their existing behavior.
- The CLI accepts `--pitch-curve INDEX:FILE`, where FILE contains finite,
  whitespace-separated semitone offsets, one per analysis frame from the note's
  start. Offsets add to `--edit`'s constant semitone shift. Like the engine API,
  values outside the supplied curve use its nearest endpoint.

The Basic Pitch input/output geometry is also documented in the
[upstream timing issue](https://github.com/spotify/basic-pitch/issues/190).
Here the old correction already approximately matched the long-term frame
rate: the measured interior error was bounded jitter, while the rounded output
length discarded an increasingly long tail.

## Recorded examples

The recordings already available in `tools/polyphonic/test-audio` are from
Salamander piano and VSCO instruments, plus the Brahms string-orchestra
performance. Their attribution is in that directory's README. No audio was
downloaded and no model was retrained during this first stage. The later quality
iteration fetched pinned source recordings and retrained the evidence classifier.

Two independent, actually played references check the ordinary edit path.
Spectral SNR uses Hann 2048 / hop 512; higher is better. These scores are
unchanged by this patch:

| Edit and reference | Unedited | Before | After |
|---|---:|---:|---:|
| Piano C major → C minor, against the played minor chord | 5.46 dB | 15.58 dB | 15.58 dB |
| Clarinet F5 stretched ×2.08, against the played longer note | 7.67 dB | 13.60 dB | 13.60 dB |

For pitch curves, compare a curve returning to exactly zero with the same curve
whose zero plateau is replaced by +0.0000001 semitone. These edits should produce
almost identical waveforms. Waveform agreement SNR measures numerical
continuity, **not separation quality or similarity to a played reference**:

| Recording / edited note | Before | After |
|---|---:|---:|
| Piano triad / E4 | 17.5 dB | 116.1 dB |
| Strings with vibrato / E4 | 9.9 dB | 107.2 dB |
| Woodwind quartet / E4 | 2.2 dB | 104.4 dB |
| Organ chord / E4 | 3.5 dB | 97.9 dB |

The independent synthetic regression checks actual waveform phase after two
pitch excursions. It fails against the original engine at −6.0 dB SNR and
passes at 139.3 / 144.3 dB after the fix.

A 90-second fixture repeats the real piano arpeggio, then adds the recorded
piano triad after silence, with its attack at 89.45 seconds:

| Evidence check | Before | After |
|---|---:|---:|
| Last timestamp | 89.3953 s | 89.9929 s |
| Maximum error against source-window sample positions | 10.231 ms | 0.004 ms |
| Frames after 89.50 s | 0 | 43 |
| Final C4 / E4 / G4 peak posteriors | unavailable | 0.872 / 0.881 / 0.872 |

The timestamp error is a geometry check; it does not claim the model predicts
musical onsets to 0.004 ms precision.

All eight labelled example mixtures retain their previous pitch coverage:
piano triad 3/3, arpeggio 4/4, strings 3/4, winds 3/4, organ 3/3, harp 5/5,
piano octave case 2/3, clarinet mixture 3/3. This counts expected pitches found
and permits fragmented detections; it is not note-event precision/recall. All
eight unedited renders have zero maximum sample error. The unlabelled 46-second
Brahms recording returns 265 notes before and 264 after the timing correction;
its identity render also has zero error. Without a score annotation, that count
change is not evidence of better or worse transcription accuracy.

On Brahms, all 2,545 pre-merge candidates retain identical DSP features and the
shared neural posterior values are bit-for-bit identical. Corrected alignment
changes two marginal classifications: a 58 ms low candidate at 1.927 seconds
drops from probability 0.653 to 0.452, and a B♭4 fragment at 23.417 seconds drops
from 0.558 to 0.214. The latter had merged into a stronger note, which now ends
one hop (11.61 ms) earlier. Correcting evidence timing can therefore change
borderline note decisions; unchanged transcription is not guaranteed on other
recordings. Evidence coverage extends by 310 ms at this recording's end.

Locally saved listening examples are in
`tools/polyphonic/test-audio/regression-results`: organ pitch curves before and
after, the edited minor piano chord and the lengthened clarinet. As with the
original examples, WAVs are excluded from Git.

## Bottlenecks identified in the first stage

This was the diagnosis before the later quality iteration. Confidence-context
experiments were subsequently evaluated and rejected; their results and the
accepted retrain are in the [quality report](polyphonic-quality-iteration.md).

The missed strings C5, wind G5 and octave-example G4 all exist as candidates but
are rejected by the note filter, with probabilities 0.373, 0.010 and 0.209.
Simply lowering the threshold is unsafe for quality: the woodwind example also
contains a bassoon overtone falsely classified as C5 with probability 0.997.

Candidate features currently let every candidate claim shared partials and
influence harmonic relationships before classification. A rejected wind G4
ghost, for example, makes the real G5 look like its octave overtone. The next
research step is to compare confidence-weighted relationships or joint note-set
scoring, then retrain and evaluate on independent recordings and instrument
families. These few local fixtures should remain regression checks, rather than
becoming the entire training and evaluation set.

Attack noise still stays in the original recording when a note is moved or
muted. The engine remains experimental and is not integrated into the main
piano roll/ARA workflow.

## Reproduction

Build the standalone tools as described in
`tools/polyphonic/research/README.md`, with ONNX Runtime and the Basic Pitch
model. CTest runs the DSP tests, exact frame geometry tests and, when the model
is present, inference through a minute-long signal's final note.

The new Python checks need only NumPy:

```sh
python tools/polyphonic/research/evaluate_examples.py \
  --cli /path/to/current/polyphonic_cli \
  --model Resources/models/basic_pitch_nmp.onnx \
  --out /tmp/polyphonic-examples

python tools/polyphonic/research/evaluate_evidence_timing.py \
  --baseline-cli /path/to/0406247/polyphonic_cli \
  --cli /path/to/current/polyphonic_cli \
  --model Resources/models/basic_pitch_nmp.onnx \
  --out /tmp/polyphonic-evidence-timing
```

For before/after **pitch-curve** comparisons, compile the current CLI source
against the original engine library so both binaries accept `--pitch-curve`,
then pass it as `evaluate_examples.py --baseline-cli ...`. An unmodified
historical CLI silently ignores unknown flags; the evaluator rejects curve
renders with no signal change to prevent that invalid comparison.

Both scripts save JSON metrics, analysis logs and WAVs in their output directory.
