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
and pick noise, breath, percussion) are never detected as objects of their own.
With plain shares they are not attached to notes either: they stay in the
recording where they are, so muting or moving a note takes its harmonic body and
leaves its attack noise behind. By default (`NoteEdit::takeBackground`, used by
the editor and the Lab) every edited note instead takes its part of what no note
owns, attack noise included; see [What the engine does](#what-the-engine-does).

Status: the engine (`Source/Audio/Polyphonic`), a command-line tool, unit tests
and the training/evaluation scripts are in place. In the standalone app,
polyphonic takes are analysed and edited in the main piano roll (on the
`polyphonic` branch; see `docs/polyphonic-integration-plan.md`); saving and the
plugin (ARA) are still to come. The earlier test window, the
[Polyphonic Lab](#trying-it-in-pitchnet-the-polyphonic-lab), has been removed.

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
| Stretching | after the attack, each partial region is read at a stretched source time and re-phased to continue its own phase (phase locking per partial); a lengthened DECAYING note instead plays as recorded up to its release, rings on at each partial's own decay rate, then plays its release | the attack keeps its speed; nothing else in the recording moves; a piano note made longer rings on instead of slowing down |

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
stays where it was (unless the note takes its background, below). Unedited
notes are not touched, and frames that no edit can reach skip the transforms
altogether: their windowed input goes straight into the overlap-add, which is
what the FFT round trip would return. An unedited render therefore gives back the
input exactly, and re-rendering after an edit costs time in proportion to the
edited notes, not to the length of the recording.
`extractNotes()` renders a set of notes on their own (solo); `renderNotes()` renders
them with their edits applied, over only the samples they reach, which is what
the Lab plays while you hold a note (a 3.7 s note of a 46 s stereo take in about
60 ms).

**Notes taking their background (the default).** With `NoteEdit::takeBackground`
what no note owns is split among the notes sounding in each frame: by level and
spectral shape (each partial widened to fill the gap to its neighbours, a small
floor elsewhere), with what is new at an onset going to the notes starting there,
and with a claim that decays and fades out over 2 s after a note ends. A muted
note removes its part; a moved, re-pitched or re-gained note takes it along, each
partial's band moving with that partial (a stretched note does not).
`extractNotes()` can add the same part, so Solo plays exactly what such a mute
removes. Measured against plain shares:

| | plain | taking their background |
|---|---|---|
| every note muted: what is left, staggered piano arpeggio / a strings recording | −17.3 / −8.2 dB | −136 / −29.9 dB |
| one note kept, the rest muted: SDR against its stem (held-out chords, 259) | 5.8 dB | 9.6 dB |
| one note muted: SDR against the mix without it (held-out chords, 263) | 14.0 dB | 14.4 dB |
| one note moved 0.25 s: spectral SNR against the mix with its stem moved (297 notes) | 15.5 dB | 15.9 dB |
| … notes that start on their own (83) / with other notes (214) | 15.4 / 15.6 dB | 16.9 / 15.6 dB |
| one note up 2 semitones: spectral SNR against the same edit on the stem alone, all of it moved (297 notes) | 14.0 dB | 14.3 dB |

With every note muted, the frames a claim reaches are silent; what is left of the
strings recording is the room after its last note, more than 2 s later. Notes
that start together split their attacks by level, so in chords a muted or moved
note can take part of its neighbours' attacks: the worst single cases lost up to
12.7 dB against plain shares. Where a deleted note's partials coincide with a
kept note's (G4 over a kept C3), the kept note's share of those bins stays.
`polyphonic_cli` leaves it off, so the measurements in this document stay
reproducible; `--take-background` applies it to every edit.

`research/evaluate_attack_move.py` measures how far a moved
note's attack follows it, against the same mix with the note's own recording
moved (Salamander piano, VSCO harp; one note moved 0.25 s):

| | left at the old onset (>2 kHz, re the note's attack) | SNR |
|---|---|---|
| a note on its own | -70 to -150 dB | 32 dB |
| struck over another note, plain shares | +1.1 dB | 15.1 dB |
| struck over another note, taking its background | -6.5 dB | 19.4 dB |

Between the partials, the strike noise left behind is about -25 dB. Most of
the rest lies in bins a held note's partials share: a bin can only be split as
a whole, so moving more of the strike would move the held note's own sound
too (measured: a dip at its harmonics). Longer onset claims, narrower claims, no
lead-in and a median-filter transient layer (Fitzgerald 2010) gave no measurable
gain; handing what is new in a held note's partial bins to the note starting
there helped the measurement for a pluck over a held note but sounded worse,
and was dropped. Notes struck together share one strike, split by level and spectrum, so
moving one note of a chord moves only its share of it.

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

**Lengthening a decaying note (natural-decay continuation).** A piano or harp
note made longer should ring on, as it would if the key were held longer, not
play its whole decay in slow motion. The renderer decides from the note's own
partials (`planNaturalDecay` in `PolyRenderer.cpp`): a line fitted to its
level over the first 0.5 s after the attack falling faster than
`Params::naturalDecaySlopeDb` (−0.05 dB per frame, about −4.3 dB/s) marks a
decaying note. Judged that early because later frames can belong to something
else: a note's masks may run on into its own release or another note's tail.
On recorded single notes every piano and harp note falls faster than that and
every bowed or blown note slower, apart from a clarinet (−0.044) and a bassoon
(−0.034) that are nearly flat either way. The note's release is where its
level drops `Params::releaseDropDb` (3 dB) below that line extended; the held
frame lies half a window (`fftSize / 2`) before it, so its window contains none
of the release, and never where the note has fallen 40 dB below its peak (a
high piano note that has died away holds where it is still audible). Output
frames up to the held frame play as recorded. For the extra length the stretch
asks for, each partial region of the held frame is added again with its phase
continued (as in the stretched part) and its level falling at that partial's
own rate, fitted over the same frames (clamped to [−1.5, 0] dB per frame).
Then the note's release plays that many frames later, from the decayed level.
The note ends exactly where the time map puts it, so `stretch` keeps its
meaning. Notes that do not decay are time-stretched as before;
`naturalDecaySlopeDb = 0` (`polyphonic_cli --no-natural-decay`) time-stretches
every note.

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
those are left out. *(These were measured before natural-decay continuation;
see below.)*

**Natural-decay continuation** (2026-11-17, `evaluate_stretch.py` with Basic
Pitch; Linux ARM64 build, ONNX Runtime 1.19.2). Before (`--no-natural-decay`)
→ after, doing nothing in brackets; shortening and sustained notes are
unchanged:

| | ×1.5 | ×2 |
|---|---|---|
| piano and harp, alone | 10.6 → **23.9 dB** (19.5) | 6.1 → **21.5 dB** (17.7) |
| piano and harp, in a chord | 19.5 → **30.2 dB** (27.5) | 15.5 → **28.2 dB** (25.7) |
| held-out recordings, alone | 12.0 → **23.9 dB** (13.6) | 7.6 → **19.8 dB** (11.6) |
| held-out recordings, in a chord | 17.0 → **25.1 dB** (16.7) | 12.5 → **21.1 dB** (14.7) |

Case by case, 16 of 93 primary and 20 of 93 held-out cases improve and none
gets worse (by more than 0.05 dB). Lengthened piano and harp notes now beat
doing nothing in 14 of 16 primary cases (the other two, a high piano note that
has died away before its end, tie it) and in all 14 held-out cases; before, they
were 8–12 dB worse than doing nothing. The held-out recordings
(`fetch_holdout_samples.py`: another piano velocity layer and other VSCO
recordings) were not used to choose the thresholds.

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
- **Attack noise stays behind** with plain shares. Unpitched sound is not
  attached to notes, so muting a note leaves its hammer, pick or breath noise in
  place, and moving a note moves its pitched body but not its attack. On
  sustained material it hardly matters; on piano and harp it is what remains of
  a muted note. Notes taking their background (as in the Lab) take their attack
  along, except when stretched; notes that start together share their attacks by
  level.
- **Repeated notes** are split only where the note's partials clearly rise
  again after a dip; a fast repetition that never decays in between (common
  with the sustain pedal), a re-tongued wind note or a legato bow change stays
  one note. In dense piano music this is the main source of missed notes.
- **Stretching slows a sustained note down.** Like any time-stretch, a
  lengthened bowed or blown note's vibrato is stretched with it (slower
  vibrato). Decaying notes (piano, harp) ring on at their natural decay instead
  (natural-decay continuation), but the continuation holds one frame's partials,
  so beating between a piano note's unison strings freezes over the added
  length. A stretched note is also only as clean as its separation, so partials
  it shares with other notes smear a little.
- **Onsets.** On the held-out chords the median onset error is +13 ms and
  82 % of found notes start within 50 ms of the truth; slow attacks and notes
  that enter on another note's partial are the outliers. Masks start 3 frames
  before the onset anyway.
- **Offline.** Analysis takes about one seventh of real time on a modest CPU
  with one core; spectra, candidate blocks and mask blocks run on all cores
  (`parallelFor`, merged in block order, so the result does not depend on the
  number of cores). `detect()` gives the notes without masks in about two
  thirds of that, and `restore()` builds the masks from them later (exactly
  `analyze()`'s). The standalone app shows a new take's notes after
  `detect()` and builds its masks in the background, as for a reopened
  project; the plug-in still analyses in one go. Basic Pitch runs on its own
  thread (half the cores) alongside the spectra and, in Harmonic mode, the
  candidate search: the engine asks for its evidence (`EvidenceSource`) only
  where it first needs it, the scoring (Guided: the search; with no
  evidence Guided falls back to Harmonic);
  rendering is fast enough to re-render an edited passage interactively.
- **DSP-only mode** works but mistakes more ghosts for notes and misses more
  real ones; the Basic Pitch
  model costs 230 kB and about 3 % of real time on the CPU.

## Code map

| File | What |
|---|---|
| `PolyEngine.h` | public API: `Params`, `analyze()`, `restore()` (an analysis rebuilt from saved notes, without detection), `render()`, `renderNotes()`, `extractNotes()`, `extractNote()`, `Note`, `NoteEdit`, `AudioSpan`, `NoteEvidence` |
| `PolyEngine.cpp` | the analysis pipeline |
| `PolySpectral.cpp` | streaming STFT, instantaneous frequency, tonality, whitening, peaks |
| `PolyExtraction.cpp` | cents field, harmonic-sum salience, iterative estimation and cancellation, pruning, f0/inharmonicity, repeated-note splits, onset refinement, fragment merging |
| `PolyPurifier.cpp`, `PolyPurifierModel.h` | the note filter: candidate features and the generated tree models |
| `PolySoundModel.cpp` | harmonic NMF masks |
| `PolyRenderer.cpp` | streaming resynthesis with edits (pitch, gain, mute, move, stretch) |
| `BasicPitchEvidence.*` | Basic Pitch note posteriorgram through ONNX Runtime (needs `HAVE_ONNXRUNTIME`) |
| `PolyFFT.h` | dependency-free real FFT |
| `PolyTempo.*` | tempo and downbeat from the notes' onsets |
| `PolyChords.*` | chords from the notes (the chord track) |
| `tools/polyphonic/` | `polyphonic_cli`, standalone CMake for the tool and tests, research scripts |
| `tests/PolyChordTests.cpp` | chords from notes: grid and free time, inversions, silence, a melody over held chords, decaying chords, flat keys, edits, a progression through the engine |
| `tests/PolyphonicTests.cpp` | synthetic-signal tests: detection, identity, pitch, gain, mute, move, stretch, vibrato, stereo, solo, a note on its own with its edit, untouched frames, repeated notes, unpitched sound left in place, notes taking their background |

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

*Removed (2026-11-18) now that the main piano roll edits polyphonic takes; its
code is in git history (`Source/UI/Polyphonic/PolyLab.*`). The section is kept
as a record of the test bench.* It opened a separate window for trying the
engine on real material. It is a test bench, not
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
  keeps its speed and only the rest of the note is stretched (a stretched note
  leaves its attack noise and the rest of its background where they were).
  Shift- or Cmd-click and rubber-band selection edit several notes at once.
  Arrow keys nudge pitch (Shift: an octave) and time. Delete mutes the
  selection. R or a double-click resets. Edited notes turn teal with a dashed
  outline where they were. **Show rejected candidates** draws the note filter's
  rejects in amber.
- **Notes take their background.** Deleted, moved, re-pitched and re-gained
  notes take their part of what no note owns along (`NoteEdit::takeBackground`,
  see [What the engine does](#what-the-engine-does)), and Solo and a held note
  include it; stretched notes leave it where it was.
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
   **Note assignment mode** (built; see [below](#note-assignment-mode)) shows
   the inactive candidates and lets the user activate them; the masks for the
   new active set come from `restore()`, which takes any active set.
4. **Rendering.** Call `polyphonic::render` from the synthesis worker for a
   polyphonic take instead of the vocoder/PSOLA path; it already handles
   stereo, and frames no edit reaches are copied rather than transformed, so a
   re-render after an edit costs time in proportion to the edited notes (a
   quarter of a second for 70 s of stereo piano with four edits). After the
   first render, `EditorController` keeps the result and its edits, and each
   later render adds `renderChange(previousEdits, edits)` to it. Only the
   changed notes are synthesised: each one's new edit minus its old one,
   over `changedRange`. Each note's part of the render is independent and
   linear, so the sum equals the new render up to float rounding (−141 dB
   after 40 changes, test 19). `renderRange` gives the exact splice where
   that matters.
   How what no note owns is split among the notes (`takeBackground`) does
   not depend on the edits. A `RenderCache` per take keeps it for the frames
   around the edited notes, up to 128 MB (about 13 s of a dense passage),
   dropping the least recently used frames first. It starts over when an
   edit changes the take's length.
   Re-editing one note among 140 edited ones (60 s stereo, synthetic): full
   render 1.24 s, `renderChange` 0.027 s with the note's frames cached and
   0.07 s the first time; about the same as with one edited note.
   `extractNotes` serves a solo button and `renderNotes` lets a note be heard
   while it is dragged. The Polyphonic Lab shows the whole loop
   (analysis thread, edits, background render, A/B playback) in one file.
5. **Packaging.** Ship `Resources/models/basic_pitch_nmp.onnx` (from the
   `basic-pitch` repository, `basic_pitch/saved_models/icassp_2022/nmp.onnx`,
   SHA-256 `2c3c1d14…0a0ec`) like the other models, and keep its Apache-2.0
   notice in `THIRD_PARTY_NOTICES.txt`. The engine falls back to DSP-only
   note filtering when the file is absent.

## Note assignment mode

Melodyne's Note Assignment Mode, for polyphonic takes. The toggle sits in a
small capsule of its own just right of the toolbar's centre one, beside the
tools and slider it brings up (icon `Resources/images/assign.png`). Following Melodyne, the mode edits the
detection, not the sound:

- **What is shown.** The take as analysed: notes at their detected pitch,
  time and length, with no edits (they come back when the mode is left). The
  canvas is tinted. Every candidate the analysis kept (`Note::active ==
  false`) is a dashed outline, drawn like a deleted
  note in edit mode: shaped by its level per frame. Candidates have no masks,
  so that level is estimated from the spectral peaks at their partials
  (`Analysis::candidateLevels`, filled by `analyze()`, `detect()` and
  `restore()`; within 2 dB of the note's own level in test 22) and saved with
  the take's other levels. Projects saved before this draw candidates as even
  outlines until reopened takes are rebuilt. Hovering a note, or selecting just one, shows its
  harmonic series (dotted: partials 2 to 8 above it, /2 and /3 below it) and
  highlights its candidates: those that overlap it for at least half the
  shorter one's length and sit within 50 cents of one of its first 8
  harmonics or subharmonics. (The engine's `parent` alone is not enough: it
  links a candidate to any harmonic of a note, up to the 30th.) Hovering a
  candidate highlights it and its `parent`, when that is such a note.
- **The note assignment slider** (`NoteAssignmentSlider`, in the toolbar's
  centre capsule in place of Note / Cent / Level; after Melodyne's) has two
  handles on one track of note-filter probability, on a logistic scale
  (0.995 at the left, 0.003 just before the right end, then everything). The
  handles are triangles pointing at the track, so they never cover each other:
  the ball is the filled one below it (like a solid note), the bracket the
  outlined one above it (like a dashed candidate), and a press in the lower or
  upper half of the slider works on that handle, on the handle itself or on
  the track (the handle jumps there). The track is solid left of the ball
  (notes), dashed between the two (candidates shown) and dim right of the
  bracket (hidden); the handle under the pointer is drawn brighter.
  - **The ball** is the probability a possible note needs to be a note. While
    it is dragged the piano roll previews the result; let go, every note at or
    above it is active and every other one a candidate
    (`polyphonicassignment::notesAtThreshold`), as one undo step and one
    rebuild. It overrides any assignment made by hand (undo brings that back),
    and deactivated notes lose their edits. At the detection's own threshold
    (0.5, guided 0.6; a double-click puts it there) it gives back the detection
    exactly: merged notes carry their pieces' highest probability (checked in
    test 22 and on all 13 test recordings). Elsewhere it is an approximation:
    a lower ball switches on candidate pieces as they are, without the
    fragment merging and onset refinement the detection did. The threshold is
    kept in the take's params (`setThreshold`), so undo and the saved project
    (`activationThreshold`) keep it.
  - **The bracket**, at the ball or right of it, is the probability a
    candidate needs to be shown (default 0.05; double-click). The engine keeps
    4 to 10 times as many candidates as notes, most near 0. A candidate at the
    same pitch (within half a semitone) as an overlapping note, or as a more
    probable overlapping candidate, is a fragment of the same sound and is
    shown once. On the test recordings (DSP-only probabilities) the default
    leaves about one candidate per note: 84 of 344 for 94 notes
    (`10-chords-104bpm`), 487 of 2275 for 240 (`08-brahms…-strings`). Hidden
    candidates cannot be hovered, activated or snapped to.
- **What is heard.** The recording as it is; the edited result again when
  the mode is left. Standalone: the audio engine loads the take's own channels
  and then the render. Plug-in: nothing stored or saved changes. With ARA, the
  document controller marks the clip on the canvas as playing its original
  (`PitchNetDocumentController::playsOriginal`, an atomic pointer the playback
  and preview renderers only compare), so it plays its ARA source instead of
  its processed audio; the processor keeps the mark on the active clip as it
  changes (`refreshOriginalPlayback`). In capture mode the input, which is the
  take as recorded, passes through. Closing the editor ends it.
- **Live Audition** (when it is on). Holding a note plays its own separated
  sound, unedited (the drag audition's renders, with no edits). Everything
  that has no separated sound is heard as a tone instead (`notetone::render`,
  after Melodyne's monitoring synth: four harmonics along the note's own pitch
  curve, shaped by its level): a held candidate; a dragged note's snap target
  (the candidate's pitch curve, or the note's own moved by the harmonic ratio),
  retriggered each time the target changes; and a note whose masks are still
  being rebuilt. Letting go stops it, as in edit mode.
- **Gestures.** Double-click a note to deactivate it (it becomes a
  candidate), or a candidate to activate it. Delete (or the context menu)
  deactivates the selection. Drag a note up or down to move it to a
  candidate it overlaps (within 0.6 semitones of the pointer: the candidate
  becomes the note, the note its candidate) or to its own harmonic or
  subharmonic (x2, x3, x4, /2, /3, /4, within 0.5 semitones: its f0 track is
  scaled). Dropped anywhere else, it springs back. Each change is one undo
  step (`PolyphonicAssignmentAction`).
- **Split and merge** (the Split tool; the capsule holds the Select and Split
  tools in this mode; it always starts with Select, so a first click never
  splits a note, and leaving it restores the tool the editor had before,
  which vocal takes share). Hovering
  a note shows the vocal Split tool's guide and cursor; a click splits it before
  that frame. Each part keeps at least the engine's shortest note
  (`Params::minNoteSeconds`). The first part keeps the note's index and the
  second is appended, so every other note and edit keeps its index; both parts
  keep the note's probability, share its energy and lose its edit. Only
  consecutive notes merge: a note that starts on the frame after another ends,
  with its pitch running on across the joint (within half a semitone), as the
  parts of a split do (`polyphonicassignment::mergePartner`). Their
  separations are drawn as lines; hovering one shows the merge cursor, hides
  that line and highlights both notes, and a click joins them (pitch curves end to end, the
  higher probability, the summed energy). The second note leaves the list, so
  later notes' edits and the candidates' `parent` move down one. Levels are
  sliced or joined from the old notes, so shapes stay put until the masks are
  rebuilt, also on undo (`PolyphonicAssignmentAction` keeps both sides'
  levels). Split parts and a merged note are selected afterwards.
- **Edits.** A note whose assignment changes (deactivated, or moved to another
  pitch) loses its edit, as a re-detected note does in Melodyne
  (`polyphonicassignment::editsFor`). Other notes keep theirs: notes keep
  their order, so edits stay keyed by index.
- **Masks.** Each change makes the project's take a restoring take with the
  new notes and a pending restore (`polyphonicassignment::apply`), exactly as
  for an opened project: drawn and editable at once, rendering nothing yet.
  `MainComponent::rebuildPolyphonicAssignment` runs `polyphonic::restore` on
  the take's own channels in the background (standalone and plug-in alike). A
  newer change cancels the rebuild in flight. When the rebuild lands, the take
  and its edits are rendered again. Until then a just-activated candidate is
  drawn at its estimated level, a deactivated note at its old one, and a note
  moved to another octave or harmonic keeps the shape it had. A clip left while it rebuilds is rebuilt when it is shown again.
  The new notes are saved with the project (the restoring take's notes).
- **Limits.** Reassigning needs the take's channels, so it does nothing in
  the first seconds after a saved project is opened, until its own restore
  has landed. There is no note separation tool (starting points) and no
  assignment slider.

## Chord track

A polyphonic take shows its chords in a lane under the ruler. Where they come
from depends on how PitchNet runs:

| | chords |
|---|---|
| standalone app, non-ARA plug-in | detected from the take's notes |
| ARA plug-in, host with a chord track (Studio One, for example) | the host's (`kARAContentTypeSheetChords`), coloured |
| ARA plug-in, host without one (or an empty one) | detected, as above |

The host's chords are drawn in the accent colour, detected ones in grey. Show Chord Track,
in the side panel's UI card, hides the lane (an app setting, on by default).

**Host chords** (`Source/Plugin/AraChordTrack.h`, read in
`PitchNetDocumentController::refreshHostMusicalTimeline` next to the tempo
map). ARA places sheet chords in quarter notes; they are put on the host's
tempo map in timeline seconds and drawn through the region's timeline offset,
so they line up with the host whichever region is edited. As ARA defines them,
each chord lasts until the next, the first also holds before it and the last
after it, and an undefined chord (no intervals) is a gap. The host's own chord
name is shown when it gives one (its flat and sharp symbols written as b and
#); otherwise the name is built from the root and bass as the host spells them
and the intervals. The controller re-reads them on harmonic and timeline
content updates.

**Detected chords** (`PolyChords.h`, `detectChords()`). The engine knows every
note's pitch and level per frame, so the chords are read from the notes, not
from a chromagram of the audio:

- `chordNotes()` turns the take into notes with their edits: muted notes are
  left out, a re-pitched note counts at its new pitch, a moved one at its new
  time, a stretched one over its new length, a re-gained one at its new level.
  So the track follows editing: lower a C chord's third and it reads Cm.
- Time is cut into slices: a beat when the timeline is in Beats mode (the
  host's grid, or the tempo PolyTempo found; a beat under 0.3 s is read two
  at a time), a quarter of a second in Time mode.
- Each slice gets a pitch-class profile from the notes sounding in it,
  weighted by their level (compressed a little, exponent 0.8), by how recent
  their attack is (a weight that falls to 0.4 over 1.5 s, so what is struck
  now outweighs what still rings from the previous chord), and less above
  C5 (a melody's passing notes are not the harmony); and a bass profile from
  its lowest notes. The chord track gives each note a constant level from its
  energy (and gain edit), not its level per frame: the per-frame levels are
  estimates from the spectral peaks until the masks are built and the sound
  model's afterwards, so reading them changed the chords when the masks
  landed (standalone, where notes come first). The energy comes with the
  notes, the same before and after the masks and in a saved project. Where
  a note rings on past its own chord (the chord its start falls in, or the
  next one if it is struck within 0.1 s of it), it is taken to fade
  steadily from its attack to -20 dB at its end, so a chord left ringing
  does not colour the one struck over it (an F ringing under a G reads G,
  not G7). This needs the chords to know where each note's chord ends:
  detection runs twice, the second time with the tails from the first; in
  a segmentation the user set, the pieces say it. The fade uses only the
  note's start, end and energy, so the chords still do not change when the
  masks land. On four rendered progressions (96 points) this reads 64
  right, against 68 with the model's levels, 67 with the energy and no
  fade, and 56 with the peak estimates: the fade costs where chords share
  tones held over the change (a held note cannot be told from a ringing
  one without its level per frame); -40 dB cost more (61), -12 dB no longer
  fixed the ringing case. `detectChords` still takes per-frame levels when
  given (`chordNotes`); notes with them get no modelled fade.
- Each slice is scored against 23 chord templates (major, minor, 7, maj7, m7,
  dim, m7b5, dim7, aug, sus4, sus2, 7sus4, 6, m6, the power chord, add9, madd9,
  9, maj9, m9, mMaj7, 6/9 and 7#5) by cosine
  similarity, with a small prior per quality that favours the simpler reading,
  a bonus for a bass on the root and a penalty for a bass outside the chord.
  A silent slice has no chord; a slice with one pitch class lets a chord
  continue but does not start one.
- A Viterbi pass with a fixed cost per change picks the sequence (template
  matching with HMM smoothing: Fujishima 1999, Bello & Pickens 2005).
- A span whose bass is mostly another chord tone is an inversion (C/E). In
  Time mode a chord starts at its notes' onset nearest the slice edge.
- Names are spelled with flats in flat keys (F, Bb, Eb, Ab, Db major; D, G,
  C, F, Bb, Eb minor), the key from the Krumhansl-Kessler profiles.

Detection runs again only when the take, its edits or the beat grid change
(`Source/UI/PianoRoll/ChordLane.*`). Chords are not saved; they are rebuilt
from the notes (or kept, once split or merged; below).

**Splitting and merging chords.** A click on a chord, away from its name,
splits it: at the nearest grid line in Beats mode (the timeline's grid
division), where the pointer is in Time mode; a dashed line shows where. A
click where two chords meet merges them; hovering there highlights the area
the merged chord will cover. Dragging a chord's start or end where no chord
is beside it (a gap, or the ends of the track; a resize cursor shows them)
changes its length, snapping as Drag Snap says (Alt inverts); it can grow up
to the next chord, closing the gap, but not into it. Nothing is retuned: both halves of a
split play what they played, and a merged chord is read from all its notes.
From the first split or merge on, the lane keeps that segmentation
(`Project::getChordBoundaries`, saved with the project and the plug-in state,
undoable) and names each piece by what its notes play
(`polyphonic::chordsOver`), so the halves of a split can be given different
chords; a stretch where the notes play no chord is a gap, which the chords
beside it can be stretched over. Right-click the lane and choose Detect Chords Again to go back
to the detected chords. With a host chord track there is no split or merge:
that segmentation is the host's.

**Changing a chord retunes the notes.** Click a chord's name, type another
in place (the chord's own box, outlined while it is edited) and press Return
(Escape or clicking away cancels). The take is
retuned there (`polyphonic::retuneToChord`, one undo step, rendered like a note
edit), and the lane then reads the new chord from the retuned notes.

Any usual chord symbol can be typed (`polyphonic::parseChord`): a root (C, F#,
Bb, lower case too), then m / min / mi / -, maj / M / ma / triangle, dim / o /
degree sign, aug / +, m7b5 / o-slash, 5, 6, 6/9, 7, 9, 11, 13, sus / sus2 /
sus4, add2 / add9 / add4 / add11 / add13, alterations b5 #5 b9 #9 #11 b13 (or
with + and -), alt, no3 / no5, in or out of brackets (Cm(maj7), C7(#9)), and a
slash bass (C/E). What cannot be read is reported under the chord. Names are
written back the same way (`chordSuffix`: a table of common names, then
general rules for anything else, such as Cmaj9#11 or C7b9b13).

- Only the original chord's tones move: every note whose pitch class is in
  it, in every octave (bass, inner voices, a melody landing on a chord tone).
  Passing and neighbour notes, other non-chord tones and muted notes stay.
- Tones map by the smallest total motion that still sounds every tone of the
  new chord the recording can give, the root and third counting most
  (`chordToneMotion`): C to Cm lowers E; C to Am raises G to A; C to F moves E
  to F and G to A; Dm7 to G7 moves A to G and C to B.
- No note is added. If the new chord needs a tone nothing maps to (C to C7
  has no B flat), nothing moves for it, the lane keeps showing what the notes
  play, and a message under the chord names the missing tone.
- A note follows the chord it starts in, so a note held into the next chord
  keeps the pitch of the chord where it began (50 ms tolerance for notes
  struck just before the beat).
- The bass is as typed: the span's lowest chord tones move to it. "Cm" over
  C/E is Cm in root position; type Cm/Eb to keep the inversion. The field
  starts with the chord's name, inversion included.
- With a host chord track the lane only shows the host's chords: nothing in
  it can be clicked, typed, split, merged or resized. They are edited in the
  host (ARA has no way for a plug-in to write them).

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
- T. Fujishima, "Realtime chord recognition of musical sound: a system using Common Lisp Music", ICMC 1999.
- J. P. Bello, J. Pickens, "A robust mid-level representation for harmonic content in music signals", ISMIR 2005.
- B. Pardo, W. P. Birmingham, "Algorithms for chordal analysis", Computer Music Journal 26(2), 2002.
- C. L. Krumhansl, E. J. Kessler, "Tracing the dynamic changes in perceived tonal organization in a spatial representation of musical keys", Psychological Review 89(4), 1982.

Training and test material: Salamander Grand Piano V3 by Alexander Holm (CC BY
3.0), VSCO 2 Community Edition by Versilian Studios and Ivy Audio (CC0). The
trained trees in `PolyPurifierModel.h` are derived from features of these
recordings; no audio is shipped.
