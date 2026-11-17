# AudioSet taggers (research)

YAMNet, the tagger the input-type classifier uses, has moved to
`Resources/models/yamnet.onnx` and ships with the app; it is not in git: CMake
downloads it from the repository's `models` release (see `OPTIONAL_MODELS` in
the root `CMakeLists.txt`). The EfficientAT files below stay here for research
and are git-ignored; rebuild them with the export script.

ONNX audio taggers for the "is the input a single voice?" check in
`docs/testing/vocal-detection-research.md`. Nothing in the build or packaging
reads this folder: packaging copies only the models it names from
`Resources/models`. Exported 2026-11-17; the scripts are in
`tools/polyphonic/research/export/`.

| File | Model | Input | Outputs | Licence |
|---|---|---|---|---|
| `efficientat_mn05_as.onnx` (10 MB) | EfficientAT MobileNetV3 `mn05_as`, AudioSet mAP .443 | `waveform` float32 `[batch, samples]`, mono, **32 kHz** | `scores` `[batch, 527]` (sigmoid), `embedding` `[batch, 480]` | MIT (Florian Schmid) |
| `efficientat_mn10_as.onnx` (24 MB) | EfficientAT MobileNetV3 `mn10_as`, AudioSet mAP .471 | same | `scores` `[batch, 527]`, `embedding` `[batch, 960]` | MIT |
| `../models/yamnet.onnx` (16 MB) | Google YAMNet (MobileNetV1) | `waveform` float32 `[samples]`, mono, **16 kHz** | `scores` `[patches, 521]`, `embeddings` `[patches, 1024]`, `log_mel` `[frames, 64]`; one patch is 0.96 s with a 0.48 s hop | Apache-2.0 |

Class names: `efficientat_class_labels.csv` (527 AudioSet classes) and
`yamnet_class_map.csv` (521), both indexed by output column.

## Provenance

- **EfficientAT**: github.com/fschmid56/EfficientAT @ `a425fdc`, weights
  `mn05_as_mAP_443.pt` and `mn10_as_mAP_471.pt` from release v0.0.1. The mel
  frontend (`AugmentMelSTFT` in eval mode: pre-emphasis, 1024-point STFT with an
  800-sample window and 320-sample hop, 128 Kaldi mel bands, `(log(x+1e-5)+4.5)/5`)
  is baked into the graph as a conv1d STFT, so the model takes raw audio. Scores
  match the original PyTorch pipeline to within 8e-6.
- **YAMNet**: tensorflow/models `research/audioset/yamnet` (`features.py`,
  `params.py`, `yamnet_class_map.csv`) with Google's `yamnet.h5` weights. The
  network is rebuilt as plain TF ops (tf_keras does not load under TF 2.22) and
  converted with tf2onnx at opset 17, using YAMNet's own TFLite-compatible STFT.
  Scores match TF's `tf.signal.stft` path to within 2e-6. Short inputs are
  zero-padded to one patch, as upstream does.

## Notes for use

- **Give mn10 at least 5 s of audio, preferably 10 s windows.** On the 1 s JUCE
  `cello.wav` it returns nonsense (Snare drum 1.00, Field recording 1.00), and it
  does the same at 2–3 s even when the clip is looped. The original PyTorch model
  gives the same result, so this is not an export error. mn05 and YAMNet are
  steadier on short clips. EfficientAT was trained on 10 s clips.
- **Scores are not calibrated across classes.** Solo singing in vocadito comes
  back as "Mantra" / "Male singing" / "Speech" rather than "Singing". Sum or max
  over the voice classes (Speech, Singing and its children, Chant, Mantra,
  Humming, Rapping…) and set thresholds on our own corpus.
