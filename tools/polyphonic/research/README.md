# Polyphonic engine research scripts

Reproduce the evaluation in `docs/polyphonic-editing.md` and retrain the purifier
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

python evaluate.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL --corpus corpus
python evaluate.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL --corpus tcorpus
python evaluate.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL --corpus pcorpus   # repeated notes
python evaluate_stretch.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL   # lengthening and shortening notes
python train_purifier.py --work $WORK --cli $WORK/build/polyphonic_cli --model $MODEL   # rewrites the model header
```

Leave out `--model` to evaluate the DSP-only path. `train_purifier.py` reads the
features the C++ analyzer computes (`polyphonic_cli --dump-features`), so the
trained model always matches the shipped feature code. After retraining,
rebuild and re-run `evaluate.py` before committing the new header.
