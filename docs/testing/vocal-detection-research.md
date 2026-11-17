# Choosing the pipeline automatically: is the input a voice? — 2026-11-17

First step towards putting the polyphonic engine in the main piano roll: decide,
per track (standalone) or per audio source (ARA), whether the vocal pipeline
(SwiftF0/FCPE + GAME + PC-NSF-HiFiGAN) or the polyphonic engine should handle it.
Melodyne does the same during detection and then lets the user override its
choice in the Algorithm menu: Melodic for singing, speech, sax, flute and
monophonic bass; Polyphonic for pianos, strings, organs and guitars.

Routing (decided 2026-11-17): **only a single voice** (singing or speech, one
line at a time) goes to the vocal pipeline. Everything else goes to the
polyphonic engine, including monophonic instruments (sax, flute, bass) and
polyphonic vocals (doubled or stacked harmonies, choir). The reason for
monophonic instruments: the vocal pipeline resynthesises edits through
PC-NSF-HiFiGAN, which is trained on voice only, so an instrument comes back
sounding wrong however good its pitch track is. The polyphonic engine edits
the recording's own spectrum and does not have this problem. This holds until
the vocal pipeline has an instrument-capable resynthesis. That takes two checks,
and both must pass for the vocal pipeline:

1. **Is it monophonic?** Several notes at once → polyphonic, even if they are
   all voices.
2. **Is it a voice?** A monophonic instrument → polyphonic.

## Options considered

| Approach | What it answers | Cost | Licence | Verdict |
|---|---|---|---|---|
| Basic Pitch note posteriorgram, count simultaneous notes | polyphony | already shipped (230 kB, CPU) | Apache-2.0 | **use** for question 1 |
| SwiftF0 confidence and pitch behaviour (off-semitone spread, glides) | voice vs in-tune instrument | already computed during analysis | MIT | useful cue and fallback, not reliable alone |
| Breath/sibilance detector (consonants between voiced runs) | voice | already shipped | — | supporting cue (singing only, not humming) |
| Pretrained AudioSet tagger: EfficientAT `mn05_as` / `mn10_as` (MobileNetV3, 1.4 M / 4.9 M params, mAP .443 / .471) | voice vs instrument, by timbre | 10 / 24 MB ONNX | MIT | works; second opinion (see measurements) |
| Pretrained AudioSet tagger: YAMNet (MobileNetV1, ~3.7 M params) | voice vs instrument, by timbre | 16 MB ONNX | Apache-2.0 | **recommended** for question 2: widest margin measured |
| Essentia `voice_instrumental` classifiers | vocals vs instrumental | TF models | CC BY-NC-ND 4.0 (non-commercial) | **ruled out**: cannot ship in PitchNet |
| Monophony/polyphony from peak harmonicity (patent-era DSP, Weibull models) | polyphony | cheap | — | superseded by Basic Pitch evidence |

AudioSet taggers carry exactly the classes needed — Singing (with Choir,
Chant, Rapping, Humming…), Speech, Human voice, and the instrument classes —
and they judge timbre, which pitch behaviour cannot: a violin or saxophone with
vibrato and slides looks like a singer to F0 statistics but not to a tagger.
Their scores are not calibrated across classes, so thresholds must be set on
our own material.

## Measured: what the models already in PitchNet can do

`tools/polyphonic/research/vocal_probe.py` (NumPy + onnxruntime only) runs
Basic Pitch and SwiftF0 on up to 30 s of each file. Corpus: the 40 vocadito
solo-singing excerpts (`tools/f0_compare/vocadito.zip`), 39 instrument
mixtures from `tools/polyphonic/test-audio`, 3 monophonic instruments (JUCE
`cello.wav`, `guitar_amp.wav`, the stretch-comparison flute), WORLD's
`vaiueo2d.wav` (speech) and r8brain's drum loop. Per-file numbers:
`tools/polyphonic/research/vocal_probe_results.csv`. 84 files took 15 s on CPU.

Features:

- `poly2` — share of note-active Basic Pitch frames with two or more
  local-maximum notes above 0.5.
- `offgrid` — median distance in cents of SwiftF0's voiced frames from the
  nearest semitone, after removing the file's own tuning.
- `glide` — share of consecutive voiced frames whose pitch moves 15–300 cents.

| Class | n | poly2 (min / median / max) | offgrid cents | glide |
|---|---|---|---|---|
| Solo singing (vocadito) | 40 | 0.000 / 0.000 / **0.050** | **12.5** / 22.0 / 24.6 | **0.075** / 0.345 / 0.593 |
| Speech | 1 | 0.000 | 28.0 | 0.821 |
| Instrument mixtures | 39 | 0.000 / 0.838 / 1.000 | 0.0 / 2.5 / 9.7 | 0.000 / 0.000 / 1.000* |
| Monophonic instruments | 3 | 0.000 | 0.0–5.1 | 0.000–0.023 |
| Drum loop | 1 | 0.143 | 2.9 | 0.000 |

\* the woodwind quartet, where SwiftF0 voiced only 0.9 % of frames.

The rule "polyphonic if `poly2` ≥ 0.15; else voice if `offgrid` ≥ 11 and
`glide` ≥ 0.05; else unsure" classifies all 40 singing excerpts and the speech
as voice, 36 of 39 mixtures as polyphonic, and no instrument as voice. The 3
missed mixtures (`3d-clarinet-missed-flute`, `01-clarinet-flute-violins-missed-Cs5`,
`07-bassoon-violins-missed-Eb4`) come from the missed-notes set, where Basic
Pitch itself does not see the second voice; they fall to "unsure", not "voice".

**Limits of this test.** Apart from the Brahms recording, the instruments are
mixed from in-tune sample-library notes, which flatters the pitch cues: real
solo violin, cello, sax, trombone or fretless bass will have vibrato and slides
and will overlap singing on `offgrid` and `glide`. vocadito is dry solo singing;
stacked harmonies, choirs, heavy reverb, rap and full mixes are not in the
corpus. So the polyphony gate is supported by this evidence; the voice decision
from pitch cues alone is not.

## Measured: AudioSet taggers

The exported models in `Resources/tools` (see its README) were run on the same
84 files by `tools/polyphonic/research/vocal_probe_taggers.py`: YAMNet on the
whole file at 16 kHz, EfficientAT on 10 s windows at 32 kHz with 50 % overlap
(shorter files zero-padded), scores averaged over windows. "Voice" is the
maximum over the speech classes and the Singing family (Singing, Choir,
Yodeling, Chant, Mantra, Male/Female/Child/Synthetic singing, Rapping,
Humming); "instrument" is the maximum over the Musical instrument subtree.
Columns `y_*`, `m05_*`, `m10_*` in `vocal_probe_results.csv`.

| Model | Voice score, voices (min / median) | Voice score, non-voices (max) | Gap (lowest voice ÷ highest non-voice) | CPU per 30 s |
|---|---|---|---|---|
| YAMNet | 0.156 / 0.615 | 0.017 | **9.2×** | 98 ms |
| EfficientAT mn05 | 0.093 / 0.339 | 0.029 | 3.2× | 92 ms |
| EfficientAT mn10 | 0.127 / 0.348 | 0.031 | 4.1× | 131 ms |

All three separate every voice from every non-voice. The strings with vibrato
(`03-strings-vibrato`, the Brahms recording) score 0.001 (YAMNet) and
0.010–0.017 (EfficientAT) as voice, about as low as the piano. The
highest-scoring non-voice for every model is the 1 s JUCE `guitar_amp.wav`.
The lowest-scoring voices are quiet, breathy vocadito excerpts (27, 37).

Combined with the polyphony gate ("vocal pipeline if `poly2` < 0.15 and voice
score ≥ 0.05, otherwise polyphonic") every model routes all 84 files
correctly: 41 voices to the vocal pipeline and all 43 instrument files and the
drum loop to polyphonic. That includes the 3 mixtures where Basic Pitch misses
the second note: the tagger sends them to polyphonic because they are not a
voice.

The higher AudioSet mAP of EfficientAT does not carry over to this narrower
question. On this material YAMNet leaves the widest gap, handles clips shorter
than its window, and costs the same.

## Recommendation

Three checks on the analysis thread, before the vocal or polyphonic analysis
starts, on at most ~30 s of the source's non-silent audio:

1. **Polyphony gate (Basic Pitch).** `poly2` ≥ 0.15 → polyphonic, voice or
   not. Reuses `BasicPitchEvidence`, which the polyphonic engine needs anyway,
   so its posteriorgram can be cached and handed to `polyphonic::analyze`.
2. **Voice identity (YAMNet).** Run YAMNet (Apache-2.0, 16 MB) at 16 kHz on
   the same excerpt, take the maximum over the voice classes, and treat ≥ 0.05
   as a voice; not a voice → polyphonic. 0.05 sits between the lowest voice
   (0.156) and the highest non-voice (0.017) on a log scale and is provisional
   until the corpus has real solo instruments. Keep EfficientAT mn05 in the
   probe as a second opinion; mn10 adds cost and needs ≥ 5 s of audio.
   YAMNet now lives in `Resources/models/yamnet.onnx` (notice in
   `THIRD_PARTY_NOTICES.txt`); CMake downloads it, and Basic Pitch, from the
   repository's `models` release, pinned by SHA-256.
3. **Pitch cues, leaning polyphonic.** With the tagger in its unsure band
   (0.025–0.10), the input is a voice only if the score is at or above 0.05
   *and* the pitch cues (`offgrid`, `glide`) are voice-like; everything else in
   the band goes to polyphonic, because pitch cues are what a violin or sax with
   vibrato fools and an instrument sent to the voice vocoder is the costlier
   mistake. Without a tagger the cues decide on their own (as the polyphonic
   engine falls back to DSP-only filtering), flagged as not confident.

**Under 3 s of sound** (after dropping blocks more than 40 dB below the
loudest) nothing is classified and the input goes to polyphonic. On 0.5 s of
the JUCE `guitar_amp.wav`, shorter than one YAMNet patch, YAMNet scored 0.056
as voice; such clips are not a realistic input.

Decision: **vocal pipeline only if monophonic and a voice; otherwise
polyphonic.** When the checks stay unsure, use the polyphonic engine, which can
represent any input; a misrouted voice loses only the vocal-specific tools
(breath notes, Sibilant Balance) and the user can switch the track back.

The hard cases this routing creates are monophonic instruments that sound
voice-like (violin, cello, sax with vibrato), which the tagger must reject
(routing one to the vocal pipeline makes the vocoder resynthesise it as a voice), and
a lead vocal with a faint double or reverb tail, which the polyphony gate must
not push to polyphonic. Both need test material before the thresholds are
fixed.

Then show the chosen mode on the track, with a per-track override, and cache
the decision with the audio source so re-opening a project does not re-detect.

## Implementation: C++ classifier

`Source/Audio/Analysis/InputTypeClassifier.{h,cpp}` and `YamnetVoiceTagger.{h,cpp}`
(no JUCE). `inputtype::selectExcerpt()` takes up to 30 s of audible audio
(0.5 s blocks within 40 dB of the loudest block and above −70 dBFS).
`classify()` then runs the three checks above on the excerpt and its SwiftF0
track. The caller runs SwiftF0, so the classifier does not depend on JUCE.
Either model may be missing, and the check it serves is then skipped.

- The gate needs ≥ 10 note-active frames before it can fire.
- YAMNet scores in [0.025, 0.10] count as unsure, and the pitch cues decide
  them. When the cues also abstain (< 30 voiced frames), the 0.05 threshold
  decides.
- Without a tagger the cues decide alone, and polyphonic is the default.
- The breath/sibilance consonant rate is not used yet. It was never measured
  as a classifier feature.

`tests/InputTypeClassifierTests.cpp` covers the features, every decision
branch, excerpt selection and the no-model path on synthetic input. Given
`yamnet.onnx` and `basic_pitch_nmp.onnx` it also routes labelled WAV files. The
standalone build (`tools/polyphonic`, with `ONNXRUNTIME_DIR`) registers this as
`InputTypeClassifierModelTests` on six files from the repo and submodules.

On the 84-file corpus (C++, Basic Pitch + YAMNet, no SwiftF0) every file
routes correctly. 36 are gated as polyphonic, 41 voices pass the tagger, 6
non-voices fail it, and the 1 s `guitar_amp.wav` (0.031) falls in the unsure
band and is decided by the threshold. Against the Python probe the median
`poly2` difference is 0.002 and the largest is 0.14 (the drum loop: 0.14 in
Python, 0 in C++, polyphonic either way via the tagger). YAMNet scores match to
a median ratio of 1.00; `guitar_amp` (0.017 → 0.031) is the outlier. The
differences come from the resampler (Kaiser sinc versus FFT) and the Basic
Pitch windowing.

Not done yet: calling `classify()` from analysis, showing the mode with a
per-track override, caching the decision with the audio source, and moving
`yamnet.onnx` to `Resources/models` with its notice.

## Next steps

- ~~Export EfficientAT and YAMNet to ONNX~~: done 2026-11-17, in
  `Resources/tools` (see its README for inputs, provenance and the mn10
  short-clip caveat). Tagger columns added (`vocal_probe_taggers.py`), results
  above.
- Extend the corpus: real solo instruments with vibrato (violin, cello, sax,
  trombone, fretless bass), which must route to polyphonic; harmony vocals and
  choir, which must route to polyphonic; single lead vocals with doubles,
  reverb or bleed, which must stay vocal; rap and a few full mixes.

## References

- Celemony, Melodyne 5 manual, "Audio characteristics and algorithms".
- Schmid, Koutini, Widmer, "Efficient Large-scale Audio Tagging via
  Transformer-to-CNN Knowledge Distillation", ICASSP 2023; github.com/fschmid56/EfficientAT.
- YAMNet, tensorflow/models research/audioset/yamnet.
- Bittner et al., "A Lightweight Instrument-Agnostic Model for Polyphonic Note
  Transcription and Multipitch Estimation", ICASSP 2022 (Basic Pitch).
- Essentia models licensing, essentia.upf.edu/licensing_information.html.
