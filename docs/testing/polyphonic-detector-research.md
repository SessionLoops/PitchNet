# Polyphonic detector research — 2026-11-02

This iteration investigates changes to note inference beyond retraining the
existing independent candidate classifier. The baseline is the 480-mixture
classifier and renderer fixes described in
[polyphonic-quality-iteration.md](polyphonic-quality-iteration.md). The
joint-decoding and learned-boundary variants below remain isolated prototypes.
The strongest alternative, neural-guided harmonic pursuit, is implemented as an
opt-in detector. It improves the measured chord results but regresses on
repeated-note mixtures, so it does not replace the default.

## Research basis

The current candidate extractor follows the iterative estimate-and-subtract
approach described by Klapuri: find a prominent fundamental, estimate its
harmonic contribution, subtract it, and repeat. This makes overlapping
harmonic notes a natural target for joint inference. See
[Klapuri, *Multiple Fundamental Frequency Estimation Based on Harmonicity and
Spectral Smoothness* (2003)](https://researchportal.tuni.fi/en/publications/multiple-fundamental-frequency-estimation-based-on-harmonicitiy-a/).

An alternative is to fit all note hypotheses simultaneously. Ewert and Müller
show how harmonic template constraints and temporal activation constraints
make NMF decompositions more musically meaningful, and how onset components
can be added. Their method is score-informed separation; it does not establish
that spectral reconstruction alone can decide which notes were played.
[Ewert and Müller, *Using Score-Informed Constraints for NMF-Based Source
Separation* (2012)](https://sebewert.github.io/publications_pdf/2012_EwertMueller_ScoreConstrainedNMF_ICASSP.pdf).

The following two adaptations are engineering experiments in this repository,
not claims of new published algorithms or reproductions of those papers.

## Joint-decoding experiment controls

Both experiments use the same harmonic candidates and frozen 21-feature
classifier as the current baseline. They run after candidate scoring and before
active/inactive partitioning, fragment merging, onset refinement and final mask
construction. They change note-set inference, not pitch extraction or rendering.

Selection uses `vcorpus`: 80 mixtures of real recorded instruments, with 311
labelled note events. The 480 training mixtures and these validation mixtures
share source recordings but contain no exact waveform copies. Neither
`tcorpus` nor `ocorpus` was used for these joint-decoding experiments. Scoring
uses the corrected one-to-one event matching described in the earlier report.

| Variant | Detections | Overlap matches | Overlap F1 | Onset-50-ms F1 | Mean separation SDR |
|---|---:|---:|---:|---:|---:|
| Frozen 480-mixture baseline | 325 | 275 | 0.8648 | 0.7673 | 8.1811 dB |
| Conditional classifier, half log-odds blend | 410 | 281 | 0.7795 | 0.6879 | 8.0347 dB |
| Conditional classifier, full replacement | 586 | 283 | 0.6310 | 0.5530 | 7.3104 dB |
| Joint NMF ownership, weight 1 | 303 | 264 | 0.8599 | 0.7590 | 7.9915 dB |

These are validation results used to reject hypotheses, not held-out evidence
of generalization. No broad parameter sweep or test-set tuning was performed.

## Conditional active-set classifier: rejected

The hypothesis was that rejected low-octave ghosts contaminate the shared
partial and harmonic-relationship features of real upper notes. Re-evaluating
a candidate against only the current accepted note set might remove this
interference without changing the feature schema.

Start with the baseline active set `S = {i : p_i >= 0.5}`. For each candidate
whose baseline probability is at least 0.01, re-run the frozen classifier on
`S minus {i}, plus {i}` and retain only that candidate's conditional probability
`q_i`. Blend the log-odds:

```text
logit(p_new_i) = (1 - w) * logit(p_i) + w * logit(q_i)
```

Probabilities are clamped to `[1e-6, 1 - 1e-6]` before taking logarithms.
All candidate probes see the same previous active set, so this is one
simultaneous update rather than order-dependent greedy insertion. One update
was tested with `w = 0.5` and `w = 1`; the activation threshold remains 0.5.

Both variants add many false positives. Full replacement recovers the missing
G4 in the local piano-octave example, but also adds C5 and E5 ghosts to the
ordinary piano triad, a high ghost to the strings example and several wrong
wind pitches. Improving one hard fixture while reducing validation overlap F1
from 0.8648 to 0.6310 is not an acceptable detector improvement.

The likely explanation is feature-distribution shift: the frozen classifier
was trained with all candidates present, including rejected hypotheses.
Removing those hypotheses changes the meaning and distribution of its
relationship features. This experiment therefore does not reject joint
inference generally; it rejects using this classifier as an unmodified
conditional probability model.

## Joint harmonic ownership: rejected

This prototype tests actual competition for spectral energy. Retain every
candidate with baseline probability at least 0.001, then fit the existing
harmonic NMF simultaneously over those candidates before choosing the final
note set. Each candidate keeps its original time/frequency support, harmonic
template and the normal 40 NMF iterations.

For candidate `i`, integrate the fraction of each claimed spectral peak that
the fitted joint model assigns to it. At each partial-mask frame, use the
nearest spectral peak when its integer peak bin lies inside that mask:

```text
ownership_i = sum(peak_amplitude^2 * assigned_mask_share)
              / sum(peak_amplitude^2)
logit(p_new_i) = logit(p_i) + logit(ownership_i)
```

Ownership is clamped to `[0.0001, 0.9999]`; baseline probabilities use the
same logarithm clamp as above. The ownership weight is fixed at 1 and the
final activation threshold remains 0.5. Final output masks are rebuilt from
the selected notes through the normal pipeline.

Precision increases to 0.8713, but recall falls to 0.8489. The resulting F1,
onset F1 and separation SDR all fall below the baseline. The local recorded
examples retain the ordinary triad's three pitches and the strings' four
pitches, but the missed wind G5 and piano-octave G4 remain missed. The existing
wind overtone false positive remains. An unedited render of the recorded piano
triad is exactly the input, with zero maximum sample error.

The experiment supports a narrower inference: this static ownership score
trades recall for precision and does not supply the independent evidence
needed to distinguish a played harmonic note from a lower note's partials.
Temporal attacks, note transitions or instrument-specific structure remain
promising additional evidence. This is not evidence that all joint NMF
detectors must fail.

## Local reproduction of the joint prototypes

The isolated source snapshot is
`/private/tmp/pitchnet-joint-detector/Source/Audio/Polyphonic/PolyEngine.cpp`,
with the copied engine and CLI beside it. The prototype is built using:

```bash
PROTOTYPE=/private/tmp/pitchnet-joint-detector
cmake -S "$PROTOTYPE/tools/polyphonic" -B "$PROTOTYPE/build" \
  -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_DIR=/path/to/onnxruntime
cmake --build "$PROTOTYPE/build" --target polyphonic_cli -j 4
```

From the repository root, run each configuration into a separate directory.
The actual run used `WORK=/private/tmp/pitchnet-poly-research` and the bundled
`Resources/models/basic_pitch_nmp.onnx` model:

```bash
POLY_JOINT_ROUNDS=1 POLY_JOINT_WEIGHT=0.5 POLY_OWNERSHIP_WEIGHT=0 \
python tools/polyphonic/research/evaluate.py --work "$WORK" \
  --cli "$PROTOTYPE/build/polyphonic_cli" --model Resources/models/basic_pitch_nmp.onnx \
  --corpus vcorpus --purpose validation \
  --training-corpus rcorpus_next --training-corpus rcorpus \
  --out "$PROTOTYPE/eval/half-vcorpus"

POLY_JOINT_ROUNDS=1 POLY_JOINT_WEIGHT=1 POLY_OWNERSHIP_WEIGHT=0 \
python tools/polyphonic/research/evaluate.py --work "$WORK" \
  --cli "$PROTOTYPE/build/polyphonic_cli" --model Resources/models/basic_pitch_nmp.onnx \
  --corpus vcorpus --purpose validation \
  --training-corpus rcorpus_next --training-corpus rcorpus \
  --out "$PROTOTYPE/eval/full-vcorpus"

POLY_JOINT_ROUNDS=0 POLY_OWNERSHIP_WEIGHT=1 \
python tools/polyphonic/research/evaluate.py --work "$WORK" \
  --cli "$PROTOTYPE/build/polyphonic_cli" --model Resources/models/basic_pitch_nmp.onnx \
  --corpus vcorpus --purpose validation \
  --training-corpus rcorpus_next --training-corpus rcorpus \
  --out "$PROTOTYPE/eval/ownership-vcorpus"
```

`POLY_JOINT_FLOOR=0.01` is the conditional prototype's default candidate floor.
These environment controls are temporary experiment settings, not production
API options. Each run directory contains the evaluator manifest, results,
analysis JSON and estimated stems. The environment settings are recorded in
`experimental_configuration.json` as well as these commands, because the
general evaluator manifest does not capture environment variables.
`/private/tmp/pitchnet-joint-detector/summary.json` collects exact scores.
The isolated source, executables and run artifacts are local temporary
evidence, not a permanently versioned benchmark package.

## Implemented alternative: neural-guided harmonic pursuit

The useful redesign moves neural evidence **inside candidate search**, before
shared harmonic energy is withdrawn. Previously the acoustic search generated
all candidates first and Basic Pitch supplied only two classifier features.
The new detector lets the neural pitch estimate influence which acoustic
hypothesis gets the first opportunity to explain that energy. It continues to
track continuous frequencies acoustically and uses the same renderer and masks.

For the harmonic relevance `R(t,c)` and Basic Pitch note posterior interpolated
onto cents bin `c`, search uses:

```text
search_relevance(t,c) = R(t,c) * (0.25 + 0.75 * sqrt(note_posterior(t,c)))
```

Neural frames use the first evidence timestamp at or after the analysis-frame
centre, matching the existing classifier convention. Semitone posteriors are
linearly interpolated in pitch. Pitches outside the model's MIDI 21–108 range
keep their acoustic relevance. The multiplier has a floor, so low neural
confidence does not delete a pitch hypothesis. It is reapplied after each
harmonic withdrawal. The classifier's reference relevance and candidate
strengths are kept in acoustic units, rather than silently changing those
feature definitions.

This adaptation combines the existing harmonic pursuit with Basic Pitch's
instrument-agnostic note evidence; it is an engineering proposal for this
repository, not a claim of a new published transcription method.
[Basic Pitch, Bittner et al. (2022)](https://arxiv.org/abs/2203.09893) jointly
predicts note, onset and multipitch outputs. The accepted guided detector uses
only its note posterior; its onset output is used only in the rejected
segmentation experiment described below.

The changed candidate population needs its own classifier. Reusing the previous
weights recovered more true notes but added fragments. A fresh 150-tree,
depth-three classifier was fitted on the same 480 training mixtures, in order
`rcorpus_next` then `rcorpus`. Other training settings are unchanged. This gives
17,519 candidate rows, including 2,094 positives. The DSP-only classifier remains
shared. A separate generated evidence model avoids applying guided-search
weights to the default detector.

| Validation variant, 80 mixtures / 311 events | Overlap F1 | Onset-50 F1 | Mean SDR |
|---|---:|---:|---:|
| Current harmonic detector | 0.8648 | 0.7673 | 8.1811 dB |
| Guidance 0.50, previous classifier | 0.8628 | 0.7470 | 8.5163 dB |
| Guidance 0.75, previous classifier | 0.8663 | 0.7538 | 8.5621 dB |
| Guidance 0.75, ±93 ms maximum-pooled prior | 0.8563 | 0.7443 | 8.4568 dB |
| **Guidance 0.75, detector-specific classifier** | **0.8851** | **0.7640** | **8.5396 dB** |

Maximum pooling the neural prior was tested to reduce onset-lag sensitivity but
was rejected. The selected candidate improves validation overlap precision
0.8462→0.8559 and recall 0.8842→0.9164. Onset-50 F1 falls by 0.0033; this was
within a 0.005 selection guard. The classifier, guide weight and selection were
frozen at **2026-11-02 12:31:32 UTC**, before either new held-out comparison.
There was no tuning after the following results.

### Fresh held-out mixtures

Two partitions were generated and hashed before candidate selection:
`hcorpus`, seed 20261104, has 80 random mixtures / 314 events; `qcorpus`, seed
20261105, has 36 repeated-note mixtures / 120 events. They use the 28 alternate
recordings from the prior iteration. Those recordings have no source-hash
overlap with the 75 training recordings, but had already been evaluated in the
previous iteration. These are fresh-mixture checks on previously evaluated
alternate recordings, not newly collected recordings or new sample libraries.
The 116 mixtures have no exact waveform overlap with the 480 training mixtures
or the 276 previously evaluated mixtures, and no duplicates within either set.

Differences and intervals are candidate minus baseline; intervals use 2,000
paired mixture bootstrap draws, seed 20261102, with no multiple-test correction.

| Held-out set | Metric | Baseline | Guided | Difference, 95% interval |
|---|---|---:|---:|---:|
| Chords, `hcorpus` | Overlap F1 | 0.8346 | 0.8614 | +0.0269 [−0.0017, +0.0534] |
| Chords | Onset-50 F1 | 0.7630 | 0.7651 | +0.0020 [−0.0298, +0.0346] |
| Chords | Mean SDR | 7.0349 dB | 7.3786 dB | +0.3438 [+0.0781, +0.6333] |
| Repeated notes, `qcorpus` | Overlap F1 | 0.8906 | 0.8740 | −0.0166 [−0.0501, +0.0204] |
| Repeated notes | Onset-50 F1 | 0.7188 | 0.7244 | +0.0057 [−0.0502, +0.0573] |
| Repeated notes | Mean SDR | 10.3792 dB | 10.1809 dB | −0.1983 [−0.7510, +0.3438] |

On the chord set, precision improves 0.7843→0.8171, recall 0.8917→0.9108,
matches 280→286, and detections fall 357→350. On the repeated-note set, matches
fall 114→111 and detections 136→134. Separately matched second strikes improve
24→25 of 36, but that does not offset the aggregate note loss. Most F1 intervals
include zero. The evidence supports a promising alternative for chord material,
not a universal replacement. **The original harmonic detector remains default.**

A small runtime check uses three validation clips, three runs per clip after
warmup. Median process wall time is 0.283 s for the baseline and 0.201 s for the
guided candidate per three-second clip; analysis-stage medians are 0.21 and
0.15 s. This is a local timing observation, not a hardware-independent speed
claim. The guided search produces fewer training candidates and can reduce
unproductive pursuit iterations despite computing a neural prior.

### Local recorded examples and integration

All eight labelled example mixtures retain their pitch coverage. Strings remains
4/4; the wind G5 and piano-octave G4 are still missed. Every identity render,
including the unlabelled 46-second Brahms recording, has zero maximum sample
error. Against independently played references, edited piano spectral SNR is
15.81→15.82 dB and stretched clarinet is 13.60→13.73 dB. Brahms gives 277 versus
274 active notes; that count change cannot establish accuracy without labels.
Pitch-curve numerical continuity remains above 98 dB on all four fixtures.

The integrated mode was checked against the frozen experiments on all 116
held-out files: guided note arrays, probabilities, frequency tracks and framing
match the frozen candidate exactly. The default mode matches the previous
baseline exactly on all 116 as well: 232/232 analysis parity checks passed.
Builds with and without ONNX Runtime pass CTest, the 20 research unit tests pass,
and the CLI integration check passes 35 assertions, including mode overrides,
invalid names, cached evidence and no-evidence DSP fallback.

The integrated changes are `DetectionMode::NeuralGuided`, the prior in
`PolyExtraction.cpp`, and `PolyGuidedPurifierModel.h`. No new runtime dependency
or neural-network weights are required. Select it in the API using
`params.detectionMode = DetectionMode::NeuralGuided`, or use the standalone CLI:

```bash
cmake -S tools/polyphonic -B /tmp/polyphonic-guided \
  -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_DIR=/path/to/onnxruntime
cmake --build /tmp/polyphonic-guided -j 4
/tmp/polyphonic-guided/polyphonic_cli recording.wav \
  --model Resources/models/basic_pitch_nmp.onnx --detector guided --json notes.json
```

`polyphonic_guided_cli` is an equivalent executable with guided mode as its
default; both accept `--detector harmonic|guided`. Its separate executable hash
also makes the training/evaluation configuration explicit without extra flags.
Without note evidence, guided mode falls back to the original DSP-only path.
The JSON `detector` field records the requested mode; fallback is reported on
stderr. The main app and Polyphonic Lab keep their existing default strategy.

The selected model parameters, training counts, frozen hashes and complete paired
held-out score differences are versioned in
[`guided-model.json`](../../tools/polyphonic/research/guided-model.json).
The original fit exports both standard model sections; convert its evidence
section without changing any numbers using `export_guided_model.py`. A complete
training reproduction from the research directory is:

```bash
python train_purifier.py --work "$WORK" \
  --cli /tmp/polyphonic-guided/polyphonic_guided_cli --model "$MODEL" \
  --training-corpus rcorpus_next --training-corpus rcorpus \
  --validation-corpus vcorpus --preserve-dsp-from "$WORK/purifier-before.h" \
  --out "$WORK/PolyPurifierModel-guided-fit.h"
python export_guided_model.py "$WORK/PolyPurifierModel-guided-fit.h" \
  --out "$WORK/PolyGuidedPurifierModel-candidate.h"
```

Feature generation uses the guided proposal strategy; classifier probabilities
are not input features, so the fitting process does not depend on which guided
classifier header is loaded. Same-version package requirements and row order
from the previous report still apply. The export tool reproduces the tracked
guided header byte-for-byte from the frozen fitted model.

Local artifacts: `/private/tmp/pitchnet-detector-guided/models`,
`/private/tmp/pitchnet-detector-eval/{validation,hcorpus,qcorpus,examples,runtime}`,
and `integration-parity.json`. These include source snapshots, selection/freeze
manifests, model/binary hashes, analyses, WAVs and paired confidence intervals.

## Learned event-boundary decoding: rejected prototype

Primary research motivates integrating note activity, attacks and duration:
[Onsets and Frames](https://colinraffel.com/publications/ismir2018onsets.pdf)
conditions piano note formation on onset/frame agreement; [Cheng, Dixon and
Mauch (2015)](https://webspace.eecs.qmul.ac.uk/s.e.dixon/pub/2015/ChengDixonMauch-EUSIPCO2015-NoteTracking.pdf)
uses silence/attack/decay/release states and a direct decay-to-attack transition
for repeats. These are precedents, not guarantees for this adaptation or for
arbitrary multi-instrument recordings.

The isolated prototype retains Basic Pitch onset outputs and learns attack
versus continuation from 31 features: onset context/background, sustained-note
posterior, multiscale harmonic rises, partial-rise consensus, local modulation,
pitch and age. A renewal dynamic program chooses boundary log-odds with a
100 ms minimum segment duration. It operates on existing active tracks after
fragment merging; it is not a full four-state HMM or an all-pitch decoder.

Training uses 240 ordinary plus 108 fresh repeated mixtures (seeds
20261121/22/23): 183,003 boundary frames / 411 positives. Validation uses the
ordinary 80 plus 36 fresh repeats (seed 20261124). The classifier has 100
depth-three trees, minimum leaf 30, learning rate 0.1, subsample 0.8, seed 0.
The tested boundary penalty is `logit(0.7) = 0.847297860`.

| Validation | Variant | Overlap F1 | Onset-50 F1 | Second strikes |
|---|---|---:|---:|---:|
| Ordinary 80 | Baseline | 0.8648 | 0.7673 | — |
| Ordinary 80 | Learned boundaries | 0.8371 | 0.7549 | — |
| Ordinary 80 | With nearby onset replacement | 0.8501 | 0.7666 | — |
| Fresh repeats 36 | Baseline | 0.9249 | 0.6877 | 20/36 |
| Fresh repeats 36 | Learned boundaries | 0.9023 | 0.7068 | 26/36 |

It finds additional strikes but duplicates events; repeat detections rise
127→140 for only three additional overlap matches. Mean repeat SDR rises
12.41→14.75 dB despite worse event F1. Neither fragment coverage nor SDR alone
would reveal the regression. Nearby onset replacement does not fix it.

The failure is partly in the training objective: labels within 25 ms of a true
onset are positive, the 25–100 ms region is ignored, and later frames are
negative. Broad onset pulses can have secondary shoulders roughly 100 ms later,
which the duration decoder can accept as another attack. Existing overlapping
same-pitch tracks also retain an old boundary alongside the corrected one.
This makes row-level precision overstate event reliability. No event-boundary
code or onset outputs were promoted, and this branch never used the sealed
`hcorpus`/`qcorpus` tests.

Source, training script/model and measurements remain under
`/private/tmp/pitchnet-detector-events`; the detailed local report is
`/private/tmp/event-report.md`. The next substantial redesign should decode a
complete pitch region and make **continuation, relocation of an existing
boundary, and creation of a new event** compete in one objective. Its training
loss must penalize duplicate events throughout the onset neighborhood. This is
a research direction, not an implemented or validated improvement.
