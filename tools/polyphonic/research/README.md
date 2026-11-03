# Polyphonic engine research scripts

Reproduce the evaluation in `docs/testing/polyphonic-quality-iteration.md` and retrain the purifier
(`Source/Audio/Polyphonic/PolyPurifierModel.h`). Python 3.10+ with `numpy`,
`scipy`, `soundfile` and `scikit-learn`.

```bash
WORK=~/polyphonic-work
./fetch_samples.sh $WORK                  # ~95 MB of single-note recordings (CC BY 3.0 / CC0)
python make_catalog.py $WORK              # measure each recording's pitch
python make_corpora.py $WORK              # test pieces, training, held-out and repeated-note mixtures

# the CLI, built with ONNX Runtime for Basic Pitch evidence (see ../CMakeLists.txt)
cmake -S .. -B $WORK/build -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_DIR=/path/to/onnxruntime
cmake --build $WORK/build --config Release
MODEL=../../../Resources/models/basic_pitch_nmp.onnx

python evaluate.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL --corpus corpus --purpose diagnostic
python evaluate.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL --corpus tcorpus --purpose test
python evaluate.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL --corpus pcorpus --purpose validation
python evaluate_stretch.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL   # lengthening and shortening notes
```

Leave out `--model` to evaluate the DSP-only path. `train_purifier.py` reads the
features the C++ analyzer computes (`polyphonic_cli --dump-features`), so the
trained model uses the analyzer's feature schema. Feature caches include hashes
of the CLI binary, evidence model, mixture archive, labels and schema; changed
audio also rewrites the intermediate WAV. After retraining, rebuild and re-run
`evaluate.py` before committing the new header. The explicit training workflow
below writes a reviewable candidate header.

## Local recorded-example regressions

When the WAVs in `../test-audio` are available, two additional scripts run
without fetching a corpus and need only NumPy:

```bash
python evaluate_examples.py --cli $WORK/build/polyphonic_cli --model $MODEL --out /tmp/polyphonic-examples
python evaluate_evidence_timing.py --baseline-cli /path/to/old/polyphonic_cli --cli $WORK/build/polyphonic_cli --model $MODEL --out /tmp/polyphonic-evidence-timing
```

`evaluate_examples.py` checks known pitch coverage, identity renders, the
actually played minor-piano and longer-clarinet references, and pitch-curve
continuity on four recorded instrument mixtures. It also checks the unlabelled
Brahms recording when available. Optional `--baseline-cli` compares engines;
both CLIs must support `--pitch-curve` (compile the current CLI against the old
engine library for historical comparisons).

`evaluate_evidence_timing.py` makes a 90-second fixture from the recorded piano
examples and verifies timestamp geometry and neural evidence for a final chord
near the file's end. Both scripts save metrics, WAVs and logs to `--out`.

See `docs/testing/polyphonic-real-audio.md` at the repository root for measured
results and the limitations of these checks.

## Reproducible training and recording holdout

`fetch_samples.sh` pins Salamander to `3382bf9496bba2486f5ab0de55a264d1dfc38404`
and VSCO to `440300901dfe9275fd84e0b7763af1f8443ae62e`. Optional
`SALAMANDER_REV` and `VSCO_REV` environment variables select other revisions.
The catalog records each original recording's relative path and SHA-256.
`make_manifest.py` records the source revisions, recordings, generated mixtures,
implementation hashes and Python package versions.

The original `rcorpus` and `tcorpus` use the same 75 recordings. They hold out
random mixtures and voicings, rather than recordings. Generate additional
training and validation mixtures:

```bash
python make_corpora.py $WORK --partition rcorpus_next --seed 20261102 --count 240 --no-stems
python make_corpora.py $WORK --partition vcorpus --seed 20261026 --count 80
python make_manifest.py --work $WORK
```

The accepted evidence classifier combines `rcorpus_next` and `rcorpus` (480
mixtures), using `vcorpus` for model selection. `tcorpus` (seed 999, 80 mixtures)
was kept out of selection during this iteration. `pcorpus` (seed 4242, 36
repeated-note mixtures) was used to diagnose rejected onset-splitting prototypes,
so its results are diagnostic validation, not an untouched final test.

An additional recording holdout uses 28 original recordings that do not occur
in the primary catalog: alternate velocities for piano, winds and strings, and
previously unused keys for harp and organ. Build it in its own work directory:

```bash
HOLDOUT=~/polyphonic-recording-holdout
python fetch_holdout_samples.py $HOLDOUT --source-work $WORK
python make_catalog.py $HOLDOUT --piano-velocity 8
python make_corpora.py $HOLDOUT --partition ocorpus --seed 20261103 --count 80
python make_manifest.py --work $HOLDOUT --recording-holdout-of $WORK
```

The final command fails if any source-recording hash overlaps the primary
catalog. Keep `ocorpus` out of training and model selection. It measures transfer
to other recordings from the same sample libraries; it does not establish
transfer to other libraries or arbitrary music recordings. Its smaller catalog
also changes the available pitch ranges and resampling distances, particularly
for harp and organ, so report it separately from the held-out-mixture results.

## Reproduce the accepted evidence classifier

Run these commands from this research directory after building the CLI and
generating the partitions above. Keep a baseline executable from the
correctness-fixed engine with the previous purifier header for comparison.
`TRAIN_CLI` refers to that executable; the measured run used SHA-256
`f1b96863bbb3a3594211762789ad227fb3252e28ffa40a8fff003a45c88d3ffe`.

```bash
TRAIN_CLI=/path/to/correctness-fixed-baseline/polyphonic_cli
cp ../../../Source/Audio/Polyphonic/PolyPurifierModel.h "$WORK/purifier-before.h"
python train_purifier.py --work "$WORK" --cli "$TRAIN_CLI" --model "$MODEL" \
  --training-corpus rcorpus_next --training-corpus rcorpus \
  --validation-corpus vcorpus --preserve-dsp-from "$WORK/purifier-before.h" \
  --out "$WORK/PolyPurifierModel-candidate.h"
```

The partition order is deliberate: **fresh `rcorpus_next` first, original
`rcorpus` second**, with sorted filenames inside each. Subsampled tree fitting
depends on row order. The run produced 26,369 candidates, including 2,207
positive labels. It keeps the original 21 features and 150 depth-three trees
and copies the DSP trees unchanged. It reproduced the selected evidence tree
section exactly with Python 3.12.14, NumPy 2.5.4, SciPy 1.18.1, SoundFile 0.14.0
and scikit-learn 1.9.1. Other library versions/platforms can change floating-point
training results.

Training rejects duplicate directory aliases, validation aliases of training
directories and exact training-waveform copies in validation. These checks
establish mixture separation only; source-recording separation requires the
catalog hash audit above.

## Paired comparisons and regression tests

Build a candidate CLI with the candidate header, then write separate run
directories. `--compare-to` requires identical corpus/label hashes, evidence
model, matching rules and evaluation purpose. Declare all training partitions
even when the baseline used only the original one, so both runs audit against
the full candidate training set.

```bash
BASELINE_CLI=/path/to/correctness-fixed-baseline/polyphonic_cli
CANDIDATE_CLI=/path/to/candidate/polyphonic_cli
RUNS="$WORK/comparisons"
python evaluate.py --work "$WORK" --cli "$BASELINE_CLI" --model "$MODEL" \
  --corpus tcorpus --purpose test --training-corpus rcorpus --training-corpus rcorpus_next \
  --out "$RUNS/tcorpus/baseline"
python evaluate.py --work "$WORK" --cli "$CANDIDATE_CLI" --model "$MODEL" \
  --corpus tcorpus --purpose test --training-corpus rcorpus --training-corpus rcorpus_next \
  --out "$RUNS/tcorpus/candidate" --compare-to "$RUNS/tcorpus/baseline"
python compare_evaluations.py "$RUNS/tcorpus/baseline" "$RUNS/tcorpus/candidate" \
  --out "$RUNS/tcorpus/comparison.json"

python evaluate.py --work "$HOLDOUT" --cli "$BASELINE_CLI" --model "$MODEL" \
  --corpus ocorpus --purpose test --training-corpus "$WORK/rcorpus" --training-corpus "$WORK/rcorpus_next" \
  --out "$RUNS/ocorpus/baseline"
python evaluate.py --work "$HOLDOUT" --cli "$CANDIDATE_CLI" --model "$MODEL" \
  --corpus ocorpus --purpose test --training-corpus "$WORK/rcorpus" --training-corpus "$WORK/rcorpus_next" \
  --out "$RUNS/ocorpus/candidate" --compare-to "$RUNS/ocorpus/baseline"
python compare_evaluations.py "$RUNS/ocorpus/baseline" "$RUNS/ocorpus/candidate" \
  --out "$RUNS/ocorpus/comparison.json"

python -m unittest discover -s . -p 'test_*.py'
```

The evaluator uses one-to-one note-event matching; many-to-many fragment
coverage is reported separately. The comparison script computes paired
mixture bootstrap intervals (2,000 resamples, seed 20261102). The accepted
model's overlap F1 changed from 0.8567 to 0.8600 on held-out mixtures and from
0.8237 to 0.8336 on held-out recordings, with slightly lower overlap precision.
Most confidence intervals include zero. See
`docs/testing/polyphonic-quality-iteration.md` at the repository root for
the full results, intervals, rejected experiments and limits.

## Experimental guided detector

The standalone build also produces `polyphonic_guided_cli`, which selects the
neural-guided candidate search and its separately fitted classifier. The normal
`polyphonic_cli` keeps the harmonic detector; both accept `--detector harmonic|guided`.
Guided mode needs note evidence; without it the engine uses the original DSP-only
path. The JSON records the requested mode and stderr reports this fallback.

```bash
python evaluate_examples.py --cli "$WORK/build/polyphonic_guided_cli" \
  --baseline-cli "$WORK/build/polyphonic_cli" --model "$MODEL" \
  --out "$WORK/guided-examples"
python test_detector_cli.py --harmonic-cli "$WORK/build/polyphonic_cli" \
  --guided-cli "$WORK/build/polyphonic_guided_cli" --model "$MODEL"
```

Neural evidence guides pursuit before harmonic subtraction; its multiplier is
`0.25 + 0.75 * sqrt(posterior)`. The classifier was retrained on guided candidates
from the same 480 mixtures. It improves held-out chord F1 (0.8346→0.8614) and mean
separation SDR (+0.344 dB), but repeated-note F1 falls (0.8906→0.8740). It is an
experimental alternative, not the new default. See
`docs/testing/polyphonic-detector-research.md` and `guided-model.json` for frozen
selection, complete results, intervals and limitations.

For fitting a guided classifier, pass `polyphonic_guided_cli` to
`train_purifier.py`, keeping the same fresh-first partition order and validation
partition above. Export to a candidate file first, then convert only its
evidence section for the guided namespace:

```bash
python train_purifier.py --work "$WORK" --cli "$WORK/build/polyphonic_guided_cli" \
  --model "$MODEL" --training-corpus rcorpus_next --training-corpus rcorpus \
  --validation-corpus vcorpus --preserve-dsp-from "$WORK/purifier-before.h" \
  --out "$WORK/PolyPurifierModel-guided-fit.h"
python export_guided_model.py "$WORK/PolyPurifierModel-guided-fit.h" \
  --out "$WORK/PolyGuidedPurifierModel-candidate.h"
```

The converter preserves every fitted number and shares the existing DSP trees.
Do not replace the standard purifier header with the guided classifier: its
training distribution corresponds to the guided proposal strategy.
