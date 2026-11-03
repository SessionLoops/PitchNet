# Polyphonic quality iteration — 2026-11-02

The accepted change retrains the existing evidence-assisted note purifier on
480 recorded-instrument mixtures. It retains the 21 input features, tree
architecture and DSP-only classifier. The two held-out comparisons show small
increases in event F1 and recall, with slightly lower overlap precision. Most
paired confidence intervals include zero; these results support an incremental
improvement, not a large quality claim or parity with commercial editors.

The baseline already contains the neural-evidence timing and zero-pitch-curve
correctness fixes documented in [polyphonic-real-audio.md](polyphonic-real-audio.md).
The comparisons here isolate the purifier update from those earlier fixes.

## Data and selection

Mixtures are built from real single-note recordings from Salamander Grand Piano
and VSCO Community Edition, with known pitches, note times and stems. They are
controlled mixtures of recordings, not performances captured as complete songs.

| Partition | Seed | Mixtures | Role |
|---|---:|---:|---|
| `rcorpus` | 12345 | 240 | Original training mixtures |
| `rcorpus_next` | 20261102 | 240 | Additional training mixtures |
| `vcorpus` | 20261026 | 80 | Model selection; 311 labelled note events |
| `tcorpus` | 999 | 80 | Held-out mixtures; 301 labelled note events |
| `ocorpus` | 20261103 | 80 | Held-out source recordings; 298 labelled note events |
| `pcorpus` | 4242 | 36 | Repeated-note diagnostics; 125 labelled note events |

The primary partitions share 75 source recordings. Different seeds keep mixtures
and voicings separate but do not establish recording or instrument independence.
The evaluator found no exact training-waveform copies in validation or either
held-out test. `ocorpus` uses 28 other original recordings: alternate velocities
for piano, winds and strings, and other keys for harp and organ. Source SHA-256
sets have zero overlap with the primary catalog. Both catalogs still come from
the same libraries. Different available keys and resampling distances also make
`ocorpus` a different distribution, so it is reported separately.

The evidence classifier was frozen after `vcorpus` comparisons and before
running the final `tcorpus`/`ocorpus` comparisons below. The historical `tcorpus`
had been used in earlier project evaluation, so it is a fixed mixture holdout
for this iteration, not a newly collected benchmark. `pcorpus` was used during
onset-prototype diagnostics and must not be described as an untouched test.

## Selected model and rejected experiments

The accepted model uses the same gradient-boosting settings as the original:
150 trees, maximum depth 3, learning rate 0.1, subsample 0.8, minimum leaf size
50 and random seed 0. It fits 26,369 candidates, of which 2,207 are positive,
using `rcorpus_next` first and `rcorpus` second. No confidence-context features
or neural onset splitting were promoted. The DSP tree section was copied
unchanged and verified byte-for-byte after trimming trailing whitespace.

Selection used the complete analysis pipeline as well as candidate-level
classification. The 4,313 validation candidates have 407 positive labels; these
are overlapping analysis candidates, not the 311 distinct ground-truth events.
Candidate-level F1 therefore must not be substituted for event F1.

| Validation variant | Event overlap F1 | Event onset-50-ms F1 | Mean separation SDR, dB |
|---|---:|---:|---:|
| Correctness-fixed baseline | 0.8608 | 0.7627 | 8.0699 |
| Fresh 240-mixture retrain only | 0.8589 | 0.7566 | 8.2125 |
| Confidence-context classifier | 0.8594 | 0.7531 | 8.2081 |
| Residual correction without context | 0.8621 | 0.7586 | 8.1461 |
| **Accepted 480-mixture retrain** | **0.8648** | **0.7673** | **8.1811** |
| NMF separation prototype | 0.8608 | 0.7627 | 8.0865 |
| Conservative neural-onset splitting | 0.8156 | 0.7226 | 8.0162 |

Adding confidence-context features fit the training set better but reduced
validation candidate F1 from 0.9093 to 0.8969, and reduced onset event F1 in the
full pipeline. A residual-context variation also reduced candidate F1 (0.9040).
The fresh-only and residual-control models did not preserve onset event quality.
The NMF prototype's small mean separation change did not establish a useful
overall improvement. These variants were rejected.

Neural-onset splitting created extra fragments without increasing validation
overlap true positives: detections rose from 321 to 356 while matches stayed at
272. On repeated-note diagnostics, overlap F1 fell from 0.9063 to 0.8298; the
number of second strikes with their own match stayed at 19 of 36. A less
conservative prototype was worse (validation overlap F1 0.7827). Synthetic
no-split guards passed, but the recorded instruments exposed natural modulation
being mistaken for rearticulation. Higher fragment-coverage or separation scores
alone would have hidden that regression.

## Held-out results

Event matching requires pitch error at most 0.5 semitones and overlap strictly
greater than 30% of the shorter interval. Onset metrics additionally require
onset error within 50 or 100 ms. Assignment first maximizes the number of
one-to-one matches and then minimizes total onset/pitch/overlap cost. Each
detection and truth event can be used once. Fragment coverage is a separate
many-to-many diagnostic. Separation SDR sums all eligible estimated fragments
for a ground-truth stem, rather than selecting the best fragment.

Intervals below are 95% percentile intervals for the **candidate-minus-baseline
difference**, using 2,000 paired mixture bootstrap resamples with seed 20261102.
All notes in a mixture stay together. F1 values and differences use the 0–1
scale. SDR uses dB.

| Test set | Metric | Baseline | Candidate | Difference | 95% interval for difference |
|---|---|---:|---:|---:|---:|
| `tcorpus` | Overlap F1 | 0.8567 | 0.8600 | +0.0033 | −0.0111 to +0.0175 |
| `tcorpus` | Onset 50 ms F1 | 0.7067 | 0.7150 | +0.0083 | −0.0082 to +0.0282 |
| `tcorpus` | Onset 100 ms F1 | 0.7700 | 0.7776 | +0.0076 | −0.0098 to +0.0259 |
| `tcorpus` | Mean separation SDR | 7.8012 | 7.8311 | +0.0299 | −0.2312 to +0.2888 |
| `ocorpus` | Overlap F1 | 0.8237 | 0.8336 | +0.0099 | −0.0057 to +0.0254 |
| `ocorpus` | Onset 50 ms F1 | 0.7239 | 0.7420 | +0.0181 | +0.0015 to +0.0365 |
| `ocorpus` | Onset 100 ms F1 | 0.7488 | 0.7664 | +0.0176 | +0.0008 to +0.0361 |
| `ocorpus` | Mean separation SDR | 7.7545 | 7.8195 | +0.0650 | −0.0961 to +0.2429 |

| Test set | Overlap precision | Overlap recall | Overlap matches | Total detections |
|---|---:|---:|---:|---:|
| `tcorpus` | 0.8595 → 0.8529 | 0.8538 → 0.8671 | 257 → 261 | 299 → 306 |
| `ocorpus` | 0.7697 → 0.7647 | 0.8859 → 0.9161 | 264 → 273 | 343 → 357 |

The direction of the F1 gains is consistent across both test sets, but the
precision tradeoff is real: overlap false positives increase from 42 to 45 and
from 79 to 84, respectively. Only the two onset-F1 intervals on `ocorpus` exclude
zero. These are unadjusted intervals across several metrics, not independent
confirmatory tests. Mixture resampling also does not measure uncertainty over
new sample libraries, instruments or performance styles. Mean SDR changes are
small, and median SDR falls slightly on both sets (8.1214 → 8.0934 dB and
8.2569 → 8.1609 dB).

Repeated-note diagnostics improve from 0.9063 to 0.9176 overlap F1, 0.6484 to
0.6667 onset-50-ms F1, and 0.7969 to 0.8157 onset-100-ms F1. Mean separation SDR
changes from 12.8127 to 13.4322 dB. These results come from the existing splitter
with the updated purifier, and do not establish that the unresolved
same-pitch rearticulation problem is solved.

## Reproduction and provenance

The complete generation, training and paired-evaluation commands are in
[the research README](../../tools/polyphonic/research/README.md). The selected
training command, run from the repository root, is:

```bash
python tools/polyphonic/research/train_purifier.py \
  --work /path/to/primary-work \
  --cli /path/to/correctness-fixed-baseline/polyphonic_cli \
  --model Resources/models/basic_pitch_nmp.onnx \
  --training-corpus rcorpus_next --training-corpus rcorpus \
  --validation-corpus vcorpus \
  --preserve-dsp-from /path/to/purifier-before.h \
  --out /path/to/PolyPurifierModel-candidate.h
```

Do not reverse the training partitions: row order affects subsampled tree
fitting. Filenames are sorted within each partition. The tracked training
script reproduced the selected evidence section exactly, including all 9,664
numeric tokens; it also preserved the DSP section exactly. The evidence-section
SHA-256, from `// Evidence:` through the section end with trailing whitespace
removed, is `3c3c7b286944f0e47798765011d6ea180f33be78d9e473c438a2effd2e8db972`.

| Provenance item | Recorded value |
|---|---|
| Salamander source revision | `3382bf9496bba2486f5ab0de55a264d1dfc38404` |
| VSCO source revision | `440300901dfe9275fd84e0b7763af1f8443ae62e` |
| Python | 3.12.14 |
| NumPy / SciPy | 2.5.4 / 1.18.1 |
| SoundFile / scikit-learn | 0.14.0 / 1.9.1 |
| Baseline CLI SHA-256 | `f1b96863bbb3a3594211762789ad227fb3252e28ffa40a8fff003a45c88d3ffe` |
| Evaluated candidate CLI SHA-256 | `966e578974b6b18b7b10169b6864ba9e5f6782ad04a8b68f7b9202565f3f8c4c` |
| Basic Pitch ONNX SHA-256 | `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec` |

Each evaluation's `manifest.json` stores executable/model hashes, corpus and
label hashes, matching rules and split audit. `--compare-to` rejects mismatched
inputs or scoring rules. `make_manifest.py` separately records pinned source
revisions, source recording hashes, generated mixtures, implementation hashes
and package versions. Training caches are keyed by executable, ONNX model,
archive, labels and feature schema. Training audits canonical directory paths
and exact waveform hashes, so aliases and repackaged copies cannot bypass
validation separation.

Local run artifacts for this iteration are under
`/private/tmp/pitchnet-quality-final/{tcorpus,ocorpus}/{baseline,control-v2}`;
each set's `comparison.json` contains the paired intervals and both manifests.
Repeated-note runs are under
`/private/tmp/pitchnet-poly-onsets/eval/baseline-pcorpus` and
`/private/tmp/pitchnet-quality-final/pcorpus/control-v2`. Generation manifests
are `/private/tmp/pitchnet-poly-research/manifest.json` and
`/private/tmp/pitchnet-poly-recording-holdout/manifest.json`. These temporary
artifacts are local evidence, not a permanent downloadable benchmark archive.

The research regression suite passes 20 tests, including event assignment,
fragment handling, comparison guards, cache invalidation and split aliases.
No claim is made here about dense commercial mixes, drums, vocals, reverberant
recordings or perceptual edit quality. Those need broader labelled recordings
and listening evaluation.

## Renderer verification

The renderer retains integer-bin relocation and continuous phase accumulation.
The latter sets the exact target frequency, but a fractional-bin residual
attenuates the carrier through Hann analysis/synthesis overlap-add. For residual
`a = shift - round(shift)`, the correction is
`(pi*a / sin(pi*a)) * (1-a*a) * (1-a*a/4)`, with value 1 at zero.
Its maximum is approximately 1.105. Regions narrower than three bins skip the
correction because they do not contain a full Hann main lobe.

An isolated-tone probe swept 30–8,000 Hz and small positive/negative edits,
including a dense half-cent sweep at 440 and 8,000 Hz. On the dense sweep the
largest carrier-level error fell from 0.8607 to 0.00004 dB; on the broader
frequency grid it fell from 0.7779 to 0.0014 dB. Minimum carrier-fit residual SNR
stayed above 78 dB on that grid. These are stationary-tone renderer checks,
not perceptual-quality scores for musical mixtures.

A fractional-bin interpolation prototype was rejected: despite correcting
level, its truncated 17-tap kernel introduced spurs, reducing worst carrier-fit
SNR to approximately 32 dB. Wider/tapered kernels also performed worse than
the original relocation. The accepted scalar correction preserved spectral
purity in a separate 480-case mask/move probe. It is approximate for narrow or
uneven separation masks, changing pitch and fractional time moves; it cannot
repair incomplete separation. Three-bin masks can retain roughly 0.34 dB of
level error. Fractional moves retain the existing window-overlap attenuation.

`PolyphonicTests` now fits analytic sinusoids at the expected target frequency
to check amplitude, distortion and stereo scaling at 44.1, 48 and 96 kHz. It
includes exact integer shifts, both sides of half-bin crossings and low
off-bin frequencies. The independent zero-pitch-curve phase regression and
the vibrato/stretched-note tests remain in place.

## Combined build on local recordings

The eight labelled example mixtures use recorded piano, strings, winds, organ
and harp. With the new purifier, strings now finds all four expected pitches
(previously three); the other seven retain their previous pitch coverage.
The wind G5 and octave-example G4 remain missed. Pitch coverage permits
fragments and is not event F1. All eight identity renders, plus the unlabelled
46-second Brahms performance, have zero maximum sample error.

| Independent played reference | Unedited mix | Previous build | Combined build |
|---|---:|---:|---:|
| Piano major chord edited to minor | 5.46 dB | 15.58 dB | 15.81 dB |
| Clarinet note stretched ×2.08 | 7.67 dB | 13.60 dB | 13.60 dB |

Scores are spectral SNR against separately played references, using Hann 2048 /
hop 512. The synthetic vibrato C5→B4 reference also improves from 19.6 to
20.6 dB. Pitch-curve zero-versus-epsilon continuity remains above 97 dB on the
four recorded fixtures. Brahms yields 277 active notes versus 264 in the
correctness-fixed baseline; without annotations this count change does not
demonstrate transcription improvement.

The final CLI, JSON analyses, WAVs and metrics are available locally under
`/private/tmp/pitchnet-poly-final` and
`/private/tmp/pitchnet-quality-final/examples-final`. The earlier paired local
example run is under `/private/tmp/pitchnet-quality-final/examples`.
The classifier holdout comparisons above use a binary with the previous
renderer; note decisions and unedited solo separation do not use the gain
correction, so those comparisons isolate the classifier change.

Final validation includes the standalone build with ONNX Runtime (DSP,
evidence geometry and actual-model inference tests), a build without ONNX
Runtime, and the 20 Python evaluation/cache regressions. This work improves the
experimental engine; main piano-roll/ARA integration, attack-noise attachment
and reliable dense same-pitch rearticulation remain open.
