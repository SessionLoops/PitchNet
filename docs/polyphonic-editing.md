# Polyphonic note editing

PitchNet's polyphonic engine analyses a recording of several notes at once (a
piano chord, a guitar strum, a string section) into notes and lets each note be
re-pitched, re-gained, muted, moved in time, lengthened or shortened on its
own, while everything that is not edited comes back untouched.
It is built from published methods: multiple-F0 estimation by harmonic
summation with iterative estimation and cancellation (Klapuri 2003, 2006), a
note filter learned from recordings of real instruments, a pitch-informed
harmonic NMF sound model (Ewert & Müller 2012) and phase-vocoder shifting of
partials (Laroche & Dolson 1999).

Only pitched sound is modelled. Transients and other unpitched sound (hammer
and pick noise, breath, percussion) are never detected as objects of their own
and never attached to notes: they stay in the recording where they are. Muting
or moving a note therefore takes its harmonic body and leaves its attack noise
behind.

Status: the engine (`Source/Audio/Polyphonic`), a command-line tool, unit tests
and the training/evaluation scripts are in place, and the standalone app has an
experimental test window for it, the [Polyphonic Lab](#trying-it-in-pitchnet-the-polyphonic-lab).
It is not yet wired into the main piano roll, ARA or the synthesis pipeline;
[Integrating into PitchNet](#integrating-into-pitchnet) lays out how.

## Current status — 2026-11-02

The [first regression stage](testing/polyphonic-real-audio.md) fixed neural
evidence timing/tail retention and pitch-curve phase continuity using recorded
examples. The [subsequent quality iteration](testing/polyphonic-quality-iteration.md)
retrained the evidence classifier on 480 mixtures, retaining its 21 features
and the existing DSP-only trees. It produced modest F1/recall gains on held-out
mixtures and other recordings from the same libraries, with slightly lower
overlap precision. The report includes paired confidence intervals and
experiments that were rejected after validation.

The evaluator now reports one-to-one note-event matching separately from
many-to-many fragment coverage. The initial results and cross-validation tables
below are historical and should not be compared directly with the corrected
metrics in the current report.

The [detector redesign experiments](testing/polyphonic-detector-research.md)
add an opt-in neural-guided harmonic search and a classifier fitted to its
candidate population. It improves chord-mixture detection and separation in
the measured tests, but loses some events on repeated-note mixtures, so the
original detector remains default. Choose **Guided (experimental)** in the
Polyphonic Lab's **Detector** selector to try it. The API and CLI also support
`DetectionMode::NeuralGuided` in `Params` and
`polyphonic_cli --detector guided --model ...`.
The learned event-boundary and joint-ownership prototypes were rejected.

## What the engine does

```
mono mix ─► STFT (4096 / hop 512, IF, tonality, whitening) ─► spectral peaks
          ─► cents field ─► harmonic-sum salience ─► iterative estimation and cancellation
          ─► candidates: prune, f0 + inharmonicity, split repeated notes
          ─► note filter (gradient-boosted trees, optional Basic Pitch note evidence)
          ─► active notes (+ rejected candidates kept for note assignment)
          ─► harmonic NMF sound model ─► per-note masks
render: input spectrum − edited notes' shares + their transformed shares ─► ISTFT
```

| Stage | Method | Why |
|---|---|---|
| Framing | Hann, 4096 / hop 512 at 44.1 / 48 kHz, 8192 / 1024 above 64 kHz | resolves the harmonics of low notes in ~0.01 s frames |
| Peaks | local maxima at least 4 dB above a 41-bin running median, frequency from the phase advance | the note analysis sees sinusoids only |
| Salience | Klapuri-whitened amplitude × tonality on a 10-cent grid, harmonic weights (f0 + 27) / (h·f0 + 320) (Klapuri 2006) | whitening stops loud low notes swamping the rest; published, tuned weights |
| Estimation and cancellation | take the strongest crest, follow it while it stays within 50 cents and above 10 % of its peak, cancel half of its fundamental and of its other harmonics capped by a median-of-7 smoothness envelope (Klapuri 2003), repeat | energy of other notes on shared partials survives; guaranteed progress |
| Repeated notes | cut where the note's own partials rise again together, far above its own amplitude modulation | a re-struck note becomes two notes; vibrato and bowing do not split notes |
| Note filter | gradient-boosted trees over 19 DSP features (+2 from Basic Pitch) trained on real-instrument mixtures | see below |
| Sound model | pitch-informed harmonic NMF (Ewert & Müller 2012), KL divergence, 0.5 s templates | +4.6 dB SDR over a fixed harmonic model with the same notes (below) |
| Note processing | partial regions moved in the STFT with accumulated phase (Laroche & Dolson 1999) | edits the mixture's spectrum directly: no separate signal per note, pitch curves per frame |
| Stretching | after the attack, each partial region is read at a stretched source time and re-phased to continue its own phase (phase locking per partial) | the attack keeps its speed; nothing else in the recording moves |

**Instantaneous frequency and peaks.** Peaks are local maxima at least 4 dB
above a 41-bin running median, refined by phase-advance frequency (fallback:
parabolic). Tonality is exp(−(Δ/0.35)²) where Δ is the largest disagreement, in
bins, between the peak's instantaneous frequency and its two neighbours'.

**Repeated notes.** A candidate is cut where its own partials are struck again.
Taking the summed amplitude of its first eight partials as the note's level, a
frame qualifies when the level over the next three frames reaches twice its
minimum over the previous four (and the mean over the next four is 1.5 times the
mean over the previous six), stays 1.5 times higher over the following 140 ms
than over the preceding 140 ms, comes after a dip below 70 % of the note's
earlier peak, and rises by more than 2.5 times the note's typical
frame-to-frame change in the surrounding ±200 ms, with at least 60 % of the
partials present rising together. The cut goes where the level steps up most.
Only the note's own pitched energy is consulted; there is no transient or onset
detector.

**Why the note filter is learned.** Spectral-smoothness rules assume harmonic
amplitudes vary smoothly (Klapuri 2003). Real instruments break that: in the
Salamander piano's C4 the 2nd partial is the strongest and the 3rd is 27 dB
down; a clarinet's 2nd partial sits 39 dB below its 1st and 22 dB below its 3rd.
Hand-tuned smoothness thresholds either kept the octave ghosts or deleted real
octaves. The filter (`PolyPurifier.cpp`) instead scores each candidate from
relative crest strength and cancelled energy, the share of its partial energy
no other candidate claims (overall and excluding higher candidates),
fundamental level, whether it sits on the 2nd / 3rd / higher harmonic of a lower
candidate (and start offset, pitch- and amplitude co-modulation with it),
whether a higher candidate sits on its harmonics, overlap count, lowest-voice
flag, duration and pitch. With
[Basic Pitch](https://github.com/spotify/basic-pitch) loaded (Spotify, Bittner
et al., ICASSP 2022, Apache-2.0; a 230 kB model, CPU) two more features come
from its note posteriorgram at the candidate's semitone (mean and maximum over
the candidate). Basic Pitch is used only as evidence: its own note decoding
splits notes at its 2-second window boundaries and adds octave ghosts, but its
posteriors separate real octaves from overtones far better than spectral cues.
Its onset output is not used.

**Sound model.** Each active note gets 40 harmonic kernels (the Hann main lobe
at the measured partial frequency, snapped to a spectral peak when one is within
1 %), a template of harmonic weights per 0.5 s segment (smoothed across
segments) and a per-frame gain, fitted to the magnitude spectrogram with the
median noise floor as a fixed extra component. A note's share of a bin is its
model over the total model including noise, so shares never exceed one and the
noise stays in the recording. Solved in 4-second blocks with 0.5 s margins.

**Rendering.** For an edited note and each partial region in each frame: subtract
`w·X` from the frame; if not muted, add `g·w·X` shifted by
`round((r − 1)·f / binHz)` bins and rotated by `ψ_h += 2π (r − 1) f · hop / fs`,
where `f` is that partial's measured frequency and `r` the pitch ratio for that
frame (so pitch curves work). Phases are counted at the frame centre: the
frames start N/2 samples before it, so bin k carries a factor (−1)^k, and a
region moved by an odd number of bins is flipped back. Without that, vibrato,
which keeps changing a partial's bin shift, flips partials in and out of
phase. Continuous phase accumulation sets the exact frequency, while rounding
the bin relocation attenuates the level. For regions of at least three bins,
the renderer compensates the Hann overlap-add carrier gain using the residual
fractional-bin offset. This is exact for a complete stationary partial and
approximate for separated masks, modulation and fractional time moves. The C5
vibrato regression now scores 20.6 dB against the same tone made at B4
(19.6 dB before gain compensation, 15.6 dB without the sign flip).
Pitch curves retain accumulated phase when returning to zero.
Moves are whole hops plus a linear-phase
fractional delay. Only the edited note's share of the bins around its own
partials changes; everything else in those frames, unpitched sound included,
stays where it was. Unedited notes are not touched, and frames that no edit can
reach skip the transforms altogether: their windowed input goes straight into the
overlap-add, which is what the FFT round trip would return. An unedited render
therefore gives back the input exactly, and re-rendering after an edit costs
time in proportion to the edited notes, not to the length of the recording.
`extractNotes()` renders a set of notes on their own (solo); `renderNotes()` renders
them with their edits applied, over only the samples they reach, which is what
the Lab plays while you hold a note (a 3.7 s note of a 46 s stereo take in about
60 ms).

**Stretching.** A stretched note's first 80 ms (`Params::stretchAttackSeconds`,
counted from its start) play as recorded. After that, output frame u plays the
note's source time τ(u) = a + (u − a) / ratio, a being the end of the attack
(`StretchMap`). Each partial region is taken from source frame ⌊τ⌋, its level
interpolated towards the next frame, and the whole region rotated so that the
partial's phase at its peak (taken at the frame centre, as the peak moves
between bins) continues the phase it had in the previous output frame,
advanced by 2π·r·f·hop / fs. Rotating the region as one block keeps the bins
around each partial phase-locked to it, which is what stops the phasiness of a
per-bin phase vocoder. The note's original share is removed as for any edit, so
shortening a note simply ends it earlier; only its pitched share stretches,
while its attack noise and everything else stay where they were.

Formant preservation is
available but off by default: estimated from one note's own partials it follows
odd/even partial patterns (a clarinet's) that are not formants, and it made
pitch edits measurably worse.

**Memory.** Nothing frame-by-bin is kept for the whole recording: peaks are
sparse, extraction runs in 15 s blocks with 2.5 s margins, the NMF in 4 s
blocks, and the renderer streams frame by frame.

## Initial implementation results (historical)

These tables record the original implementation's evaluation. Its overlap
coverage allowed multiple fragments to match the same note and is not the
current one-to-one event precision/recall/F1. The old figures are retained as
historical context; use the [quality iteration report](testing/polyphonic-quality-iteration.md)
for the corrected scoring, updated model and data-independence limits.

Test material is built from real single-note recordings: Salamander Grand Piano
(velocity layer 10) and VSCO 2 CE (bassoon, clarinet, oboe, flute, cello and
violin sections with vibrato, harp, pipe organ), mixed into chords with known
notes and stems. Numbers below are from `tools/polyphonic/research/evaluate.py`.

**Held-out random chords** (80 mixtures, 301 notes; seed and voicings never seen
in training):

| | with Basic Pitch evidence | DSP only |
|---|---|---|
| notes found (pitch ±50 cents, time overlap): precision / recall / F1 | 0.95 / 0.86 / **0.90** | 0.82 / 0.77 / 0.80 |
| notes with onset within 50 ms: F1 | 0.71 | 0.65 |
| notes with onset within 100 ms: F1 | 0.77 | 0.70 |
| separation SDR per note, mean / median | **7.8 / 8.1 dB** | 6.6 / 6.2 dB |

**Hand-made pieces** (8 pieces, 42 notes): piano triad, staggered arpeggio,
four-chord progression with bass, an octave doubling, organ triad, a woodwind
quartet, strings with vibrato, rolled harp chord. Each has one edit, compared
with the same edit applied to the isolated note (pitch, spectral SNR), the mix
without the note (mute, SDR) and the mix with the note 0.25 s later (move,
spectral SNR). "Doing nothing" is the score of the unedited mix.

| | with evidence | DSP only | doing nothing |
|---|---|---|---|
| notes found F1 | 0.84 | 0.68 | |
| separation SDR mean / median | 7.1 / 8.5 dB | 4.4 / 1.0 dB | |
| pitch edit (e.g. C major → C minor) | **16.9 dB** | 13.2 dB | 7.9 dB |
| mute one note | **16.5 dB** | 12.8 dB | 5.9 dB |
| move one note by 0.25 s | **17.2 dB** | 14.4 dB | 10.0 dB |
| unedited render vs input | identical | identical | |

In the DSP-only column the filter misses the edited note in two of the eight
pieces (the arpeggio's G4 and the organ's E4); that is most of its gap in the
edit rows.

**Repeated notes** (36 mixtures: a piano, harp, cello or violin section,
bassoon, clarinet, flute, oboe or organ note struck again at the same pitch
0.35–1.3 s later, over one or two sustained notes of other instruments): the
second strike is found as a note of its own in 19 of 36 with evidence and
14 of 36 without. Piano, harp, organ, oboe and cello
re-strikes are found; re-tongued clarinet and flute notes and re-bowed violins
that barely dip in between stay joined.

**Stretching** (`tools/polyphonic/research/evaluate_stretch.py`): a 1 s note of
each instrument, alone or over one or two sustained notes of other instruments,
shortened to 0.6 times its length or lengthened to 1.5 and 2 times. Each result
is compared with the same mixture in which the note was played that long
(spectral SNR, with Basic Pitch evidence; "doing nothing" in brackets; the
number of scored mixtures per cell in the first column):

| | ×0.6 | ×1.5 | ×2 |
|---|---|---|---|
| sustained instruments, alone (14) | **8.4 dB** (0.1) | **10.7 dB** (3.7) | **9.2 dB** (2.1) |
| sustained instruments, in a chord (8) | **13.4 dB** (7.4) | **12.5 dB** (7.8) | **10.1 dB** (5.3) |
| piano and harp, alone (4) | 11.3 dB (8.6) | 12.3 dB (19.5) | 8.0 dB (17.7) |
| piano and harp, in a chord (4) | 20.6 dB (18.1) | 19.5 dB (27.5) | 15.5 dB (25.7) |

Sustained notes beat doing nothing in 65 of the 66 mixtures. For piano and harp
the longer recording is not a fair target: a piano note held longer rings on at
its natural decay, which the unedited note matches for its whole length, while
a stretched note decays more slowly. In 6 of the 14 sustained chord mixtures
the note was not found as a note of its own, so it could not be stretched;
those are left out.

**Separation method, same (true) notes** (Python prototype, hand-made pieces):
a fixed harmonic model that splits each spectral peak in proportion to
smoothness-predicted partial amplitudes reaches 4.3 dB mean SDR, harmonic NMF
8.9 dB. With detected notes the C++ engine reaches 7.1 dB, so detection, not
separation, is the main gap.

**Real recording.** *Pistachio Ice Cream Ragtime* (solo piano, 70.8 s, stereo,
Lena Orsa, CC BY-NC 3.0; used only for this check, not shipped): 454 notes;
82 % of them match a note of Basic Pitch's own transcription (same semitone,
onset within 70 ms), and 67 % of Basic Pitch's notes are among them. The
largest group of the notes it misses (76 of 181) are notes and chords struck
again while they still ring, which stay joined to the note before. On a 2-vCPU
2.1 GHz Xeon: Basic Pitch 1.8 s, analysis 9.8 s, a stereo render with four
edits 0.4 s, peak memory 174 MB.

**Initial note-filter cross-validation (historical mixture groups).** These
figures use candidates of the original 240 training mixtures and report
precision / recall of "real note". The grouping assigned one family label to
each mixture, including mixtures containing multiple families. Recordings and
families could therefore appear on both sides of a split; this was not a strict
instrument-family or source-recording holdout.

| held-out mixture group | with evidence | DSP only |
|---|---|---|
| piano | 0.82 / 0.79 | 0.42 / 0.68 |
| harp | 0.94 / 0.90 | 0.91 / 0.59 |
| strings (vibrato) | 1.00 / 0.90 | 1.00 / 0.35 |
| woodwinds | 0.87 / 0.78 | 0.94 / 0.71 |
| organ* | 0.35 / 0.96 | 0.45 / 0.87 |

\*The organ samples carry a quiet sub-octave rank (a C4 key also sounds C3 at
−22 dB), which the labels count as wrong; organ precision is understated.

## Known limitations

- **Octave doublings.** C3 + C4 struck together: every partial of C4 is an even
  partial of C3, so C4 is usually missed and separation of the pair is near
  0 dB. A note that is an exact harmonic of a lower note (an oboe on a
  bassoon's 6th harmonic) can be missed the same way. This is the classic
  failure of separating notes from the spectrum; the engine keeps rejected
  candidates (with the active note they belong to) so a UI can offer
  note-assignment editing.
- **Octave ghosts.** Conversely an overtone can survive as a note (C5 over a
  piano C4 at probability 0.53). Editing the real note then leaves those
  partials behind until the ghost is deactivated.
- **Attack noise stays behind.** Unpitched sound is not attached to notes, so
  muting a note leaves its hammer, pick or breath noise in place, and moving a
  note moves its pitched body but not its attack. On sustained material it
  hardly matters; on piano and harp it is what remains of a muted note.
- **Repeated notes** are split only where the note's partials clearly rise
  again after a dip; a fast repetition that never decays in between (common
  with the sustain pedal), a re-tongued wind note or a legato bow change stays
  one note. In dense piano music this is the main source of missed notes.
- **Stretching slows the note down.** Like any time-stretch, a lengthened note's
  vibrato and decay are stretched with it: a piano note made longer decays more
  slowly instead of ringing on as a later key release would. A stretched note
  is also only as clean as its separation, so partials it shares with other
  notes smear a little.
- **Onsets.** On the held-out chords the median onset error is +13 ms and
  82 % of found notes start within 50 ms of the truth; slow attacks and notes
  that enter on another note's partial are the outliers. Masks start 3 frames
  before the onset anyway.
- **Offline.** Analysis takes about one seventh of real time on a modest CPU;
  rendering is fast enough to re-render an edited passage interactively.
- **DSP-only mode** works but mistakes more ghosts for notes and misses more
  real ones; the Basic Pitch
  model costs 230 kB and about 3 % of real time on the CPU.

## Code map

| File | What |
|---|---|
| `PolyEngine.h` | public API: `Params`, `analyze()`, `render()`, `renderNotes()`, `extractNotes()`, `extractNote()`, `Note`, `NoteEdit`, `AudioSpan`, `NoteEvidence` |
| `PolyEngine.cpp` | the analysis pipeline |
| `PolySpectral.cpp` | streaming STFT, instantaneous frequency, tonality, whitening, peaks |
| `PolyExtraction.cpp` | cents field, harmonic-sum salience, iterative estimation and cancellation, pruning, f0/inharmonicity, repeated-note splits, onset refinement, fragment merging |
| `PolyPurifier.cpp`, `PolyPurifierModel.h` | the note filter: candidate features and the generated tree models |
| `PolySoundModel.cpp` | harmonic NMF masks |
| `PolyRenderer.cpp` | streaming resynthesis with edits (pitch, gain, mute, move, stretch) |
| `BasicPitchEvidence.*` | Basic Pitch note posteriorgram through ONNX Runtime (needs `HAVE_ONNXRUNTIME`) |
| `PolyFFT.h` | dependency-free real FFT |
| `tools/polyphonic/` | `polyphonic_cli`, standalone CMake for the tool and tests, research scripts |
| `tests/PolyphonicTests.cpp` | synthetic-signal tests: detection, identity, pitch, gain, mute, move, stretch, vibrato, stereo, solo, a note on its own with its edit, untouched frames, repeated notes, unpitched sound left in place |
| `Source/UI/Polyphonic/PolyLab.*` | the Polyphonic Lab window (standalone app, Edit menu) |

```cpp
#include "Audio/Polyphonic/PolyEngine.h"
#include "Audio/Polyphonic/BasicPitchEvidence.h"

const auto params = polyphonic::Params::forSampleRate(sampleRate);
polyphonic::BasicPitchEvidence basicPitch;                       // optional
polyphonic::NoteEvidence evidence;
if (basicPitch.load(modelPath))                               // std::filesystem::path
    evidence = basicPitch.compute(mono.data(), (int) mono.size(), sampleRate);
const auto analysis = polyphonic::analyze(mono.data(), (int) mono.size(), params,
                                       evidence.empty() ? nullptr : &evidence, &cancelFlag,
                                       [](float progress) { /* 0..1 */ });
std::map<int, polyphonic::NoteEdit> edits;
edits[third].semitones = -1.0f;                               // major -> minor
edits[root].stretch = 1.5f;                                   // half as long again
const auto out = polyphonic::render(analysis, {left, right}, edits);
const auto solo = polyphonic::extractNotes(analysis, {left, right}, {third});
```

`analysis.notes` holds the active notes first (`active == true`, sorted by
onset) and then the rejected candidates (`parent` = the active note whose
harmonic they sit on). Frames are hops of `params.hop` samples.

## Trying it in PitchNet: the Polyphonic Lab

In the standalone app, **Edit → Polyphonic Lab (Experimental)...** opens a
separate window for trying the engine on real material. It is a test bench, not
the product integration below: it works on its own copy of the audio and never
changes the PitchNet project, its undo history or its render.

- **Load.** It analyses the open project's audio when it opens (the mono
  44.1 kHz copy PitchNet keeps). **Open Audio...** loads any file at its own
  sample rate, stereo included, up to 10 minutes. Analysis runs in the
  background with Basic Pitch evidence when `basic_pitch_nmp.onnx` is found.
  With **Harmonic (default)** selected, untick the evidence box to compare with
  DSP-only note filtering (this re-analyses and clears the edits).
- **Detector.** Choose **Harmonic (default)** or **Guided (experimental)** in
  the settings row (at the right of the top bar, or on a row of its own below
  it when the window is too narrow). Guided performed better on the tested
  chords but lost some repeated-note events. Changing the detector re-analyses
  the loaded audio and clears note edits. Guided automatically enables neural
  note evidence and locks its checkbox while selected; switching back to
  Harmonic restores the checkbox as you left it. If that evidence is
  unavailable, analysis falls back to the harmonic detector with DSP-only
  filtering; the status line reports the fallback and the detector actually
  used ("guided detector" when Guided ran).
- **Edit.** Each note is a blob along its own f0 track, thicker where it is
  louder. Drag a note up or down to change its pitch in semitones (hold Alt for
  cents) and sideways to move it, or drag either end to make it longer or
  shorter: the right end keeps the start where it is, the left end keeps the
  end where it is and moves the start, attack included. Either way the attack
  keeps its speed and only the rest of the note is stretched (dragging the left
  end also moves the note, so like a move it leaves the attack noise where it
  was). Shift- or Cmd-click and rubber-band selection edit several notes at
  once. Arrow keys nudge pitch (Shift: an octave) and
  time, Delete mutes, R or a double-click resets. Edited notes turn teal with a
  dashed outline where they were. **Show rejected candidates** draws the
  note filter's rejects in amber.
- **Listen.** Original, Edited and Solo (the selected notes on their own) play
  through the app's audio device and switch at the same position with a short
  crossfade; Space plays, A toggles Original/Edited. Each edit re-renders in
  the background, usually in well under a second.
- **Hear a note.** While playback is stopped, holding a note (or either end)
  plays it on its own, as edited. Drag it and it keeps sounding at its new pitch
  or length; let go and it fades out. The speaker button next to Solo turns this
  off.
- **Export.** **Export WAV...** writes the edited take as a 24-bit WAV at the
  source rate.

It is offered only in the standalone app (in the plug-in there is no audio
device of its own to play through). Its strings are in `Resources/lang/en.json`
under `polylab.*`; other languages fall back to English.

## Integrating into PitchNet

The engine has no JUCE dependency and is already globbed into `pitchnet_core`.
What is missing is the product around it:

1. **Mode and analysis.** A per-track (standalone) or per-audio-source (ARA)
   choice between the current vocal pipeline and "Polyphonic". For polyphonic
   material, run `polyphonic::analyze` on the mono mix on the existing analysis
   thread, with `BasicPitchEvidence` loaded from
   `PlatformPaths::getModelFile("basic_pitch_nmp.onnx")` after
   `OnnxRuntime::initialise()`. On Windows build the path from
   `File::getFullPathName().toWideCharPointer()` so non-ASCII paths survive.
   Cache the `Analysis` per audio source; it is the expensive part.
2. **Data model.** A polyphonic take holds the `Analysis` plus
   `std::map<int, polyphonic::NoteEdit>`. Serialise the edits keyed by note
   (start frame and median pitch, so they survive re-analysis) and re-analyse on
   load, or serialise the analysis (notes and masks take ~23 MB for 70 s of
   dense piano).
3. **Piano roll.** Notes overlap in time, so `Note`-per-frame assumptions
   (one F0 curve per project in `AudioData`) do not apply. Draw each
   `polyphonic::Note` as a blob with its own f0 curve; drag up/down sets
   `semitones`, left/right sets `timeOffsetSeconds`, dragging the right end sets
   `stretch` (use `stretchMap()` to draw the stretched note) and dragging the
   left end sets `stretch` and `timeOffsetSeconds` together so that the note
   still ends where it did, the volume handle
   sets `gainDb`, delete sets `mute`, and per-frame pitch tools write
   `pitchCurve`.
   A "note assignment" view can show inactive candidates; activating one needs
   the masks rebuilt for the new active set (`buildMasks`, a fraction of the
   analysis time; expose it as a public call when that view is built).
4. **Rendering.** Call `polyphonic::render` from the synthesis worker for a
   polyphonic take instead of the vocoder/PSOLA path; it already handles
   stereo, and frames no edit reaches are copied rather than transformed, so a
   re-render after an edit costs time in proportion to the edited notes (a
   quarter of a second for 70 s of stereo piano with four edits).
   `extractNotes` serves a solo button and `renderNotes` lets a note be heard
   while it is dragged. The Polyphonic Lab shows the whole loop
   (analysis thread, edits, background render, A/B playback) in one file.
5. **Packaging.** Ship `Resources/models/basic_pitch_nmp.onnx` (from the
   `basic-pitch` repository, `basic_pitch/saved_models/icassp_2022/nmp.onnx`,
   SHA-256 `2c3c1d14…0a0ec`) like the other models, and keep its Apache-2.0
   notice in `THIRD_PARTY_NOTICES.txt`. The engine falls back to DSP-only
   note filtering when the file is absent.

## Reproducing and retraining

`tools/polyphonic/research/README.md` fetches the samples, rebuilds the corpora
bit-for-bit, evaluates any build of `polyphonic_cli`, and retrains the note filter
from features the C++ analyzer itself dumps (`--dump-features`), then rewrites
`PolyPurifierModel.h`.

## References

- A. Klapuri, "Multiple fundamental frequency estimation based on harmonicity and spectral smoothness", IEEE Trans. Speech and Audio Processing 11(6), 2003.
- A. Klapuri, "Multiple fundamental frequency estimation by summing harmonic amplitudes", ISMIR 2006.
- M. Every, J. Szymanski, "Separation of synchronous pitched notes by spectral filtering of harmonics", IEEE TASLP 14(5), 2006.
- T. Virtanen, A. Klapuri, "Separation of harmonic sounds using linear models for the overtone series", ICASSP 2002.
- S. Ewert, M. Müller, "Using score-informed constraints for NMF-based source separation", ICASSP 2012.
- J. Laroche, M. Dolson, "New phase-vocoder techniques for pitch-shifting, harmonizing and other exotic effects", IEEE WASPAA 1999; "Improved phase vocoder time-scale modification of audio", IEEE TSAP 7(3), 1999.
- R. Bittner, J. J. Bosch, D. Rubinstein, G. Meseguer-Brocal, S. Ewert, "A lightweight instrument-agnostic model for polyphonic note transcription and multipitch estimation", ICASSP 2022.

Training and test material: Salamander Grand Piano V3 by Alexander Holm (CC BY
3.0), VSCO 2 Community Edition by Versilian Studios and Ivy Audio (CC0). The
trained trees in `PolyPurifierModel.h` are derived from features of these
recordings; no audio is shipped.
