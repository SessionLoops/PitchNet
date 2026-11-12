# Breath and sibilance detection

PitchNet labels every analysis frame of a take as **sibilance**, **breath** or
neither, and uses the labels in these places:

| Where | What changes |
| --- | --- |
| Note segmentation (`EditorController::segmentIntoNotesImpl`, `NoteEdges.h`) | GAME's notes are re-split between their vowels (see [Note edges](#note-edges)): a consonant joins the note it leads into, a word-final consonant before a pause or breath stays with its note, breaths stay out of notes, a note GAME placed on unvoiced audio that is a third or more breath is dropped, and one that is a third or more sibilance (more sibilance than breath, no voice) joins a neighbour in the same region - the note it leads into within ~300 ms, else the one before within ~255 ms, never across a breath - instead of standing as a note of its own (`noteedges::mergeSibilantOnly`). |
| Resynthesis (`IncrementalSynthesizer::generateBlendMask`) | Breaths and sibilants keep the original recording, at any length, instead of being vocoded. A note with a timing edit is rendered whole, except its breaths and sibilants, which are copied from where they were recorded (see [Stretching notes](#stretching-notes)). |
| Timing edits (`NoteTimeMap.h`, `NoteTiming.h`) | A note made longer or shorter holds its vowel: consonants keep their recorded length and move with the note (see [Stretching notes](#stretching-notes)). |
| Piano roll (`BreathSibilanceRenderer`, `PitchCurveRenderer`) | Inside a note, sibilant/breath frames are hatched - following the note's waveform like the rest of it, at least 1 px thick - and the pitch curve is left out over them: it would only show interpolated pitch, and those frames play the original recording. While pitch-drawing points are being placed, the new line is dashed over such frames between its first and last point; elsewhere on the curve, and once the drawing is applied, they are left out. Every breath outside the notes is a **breath note** (see [Breath notes](#breath-notes)). Other noise outside the notes - a stray sibilant, such as a final "s" the note stops short of - is not drawn. Within 4 frames of a note it belongs to that note: it takes the note's volume and its Sibilant Balance's cut for hatched frames, so turning a note or its sibilants down leaves no stray "s" behind (`projectgain::appendStrayRegions`). It keeps its label, so it still plays the original recording. |
| Gain edits (`BreathNotes.h`, `ProjectGain.h`, `GainEditAction.h`) | A breath note's level, and each note's **Sibilant Balance**, which turns down its hatched frames or the rest of it (see [Sibilant Balance](#sibilant-balance)). |

Why keep the recording: breaths and sibilants carry no pitch to correct, so
regenerating them can only lose detail. Before this, original audio was kept
only for unvoiced runs of 24+ frames (~280 ms); every consonant inside a
phrase went through the vocoder. PC-NSF-HiFiGAN reproduces them well - it does
not make them buzz, even 12 semitones up (see Evaluation) - but not exactly:
it takes about 1 dB off the top band of an "s". Keeping the recording
returns them bit-identical.

## Code

- `Source/Audio/BreathSibilanceDetector.{h,cpp}` - the detector. No JUCE
  dependency; uses the polyphonic engine's `RealFFT`.
- `Source/Audio/BreathSibilanceAnalysis.h` - `analyse()` / `ensure()` glue that
  fills `AudioData::breathSibilance`, and `labelAtNoteFrame()`, which maps a
  moved or stretched note's frames back onto the audio it plays.
- `Source/Audio/NoteEdges.h` - where note edges go around the vowels. No JUCE
  dependency.
- `Source/Audio/NoteTimeMap.h` - which recorded frame a stretched or squeezed
  note plays where. No JUCE dependency.
- `Source/Audio/NoteTiming.h` - the map for a `Note`, and its pitch contour
  laid out through it.
- `Source/Audio/Synthesis/BreathSibilanceProtection.h` - the blend-mask steps.
- `Source/Audio/BreathNotes.h` - where breath notes are, which note each
  belongs to, and their levels. No UI dependency.
- `Source/Audio/ProjectGain.h` - every gain-only edit (note volume, Sibilant
  Balance, breath levels) as one set of gain regions.
- `Source/Undo/GainEditAction.h` - applies a gain edit to the rendered audio,
  and undoes it.
- `Source/UI/PianoRoll/BreathSibilanceRenderer.{h,cpp}` - hatching, breath
  notes and capsules; `PitchCurveRenderer` leaves the curve out over labelled
  frames.
- `tests/BreathSibilanceTests.cpp` - synthetic takes, the blend mask, the
  labels across save/load and timeline re-padding, the note-edge rules, the
  time map, breath notes and the Sibilant Balance.

## Data flow

`AudioData::breathSibilance` holds one `breathsibilance::Label` per
`voicedMask` frame (0 none, 1 sibilance, 2 breath). It is computed in
`analyzeAudioImpl` right after the voicing and VAD masks, from the same audio.

The labels are **not stored** in project files or plug-in state. They are a
deterministic function of `originalWaveform` and `voicedMask`, like the mel
spectrogram (which v10 archives also omit), so the archive format is
unchanged and older PitchNet versions can still read new sessions.
`breathsibilance::ensure()` rebuilds them wherever a project comes back
without them: `ProjectSerializer::fromBinaryArchive`, the JSON project load in
`MainComponent_ProjectIO`, ARA hydration in `hydrateAraRegionProject`, and as
a last resort at the start of `synthesizeRegion`. Projects saved before this
change get labels the first time they are opened.

In-memory reshaping keeps them aligned like the other per-frame vectors: the
non-ARA capture merge, `makeRegionWindowProject` (slice),
`repadProjectToTimelineOffset` and `mergeProjectPlaybackParts`.

Cost: about 1.2 ms per second of audio (0.2 s for a 3-minute take).

## Algorithm

Frames sit on the analysis grid: frame `i` is centred on sample `i * 512`.

**Per-frame features** (1024-sample Hann window, ~23 ms):
- energy in a low (80-900 Hz), mid (0.9-3.5 kHz) and high (3.5-16 kHz) band;
- spectral centroid over 80 Hz-16 kHz;
- periodicity: the window-corrected normalised autocorrelation peak over
  60-1100 Hz of the frame low-passed at 4 kHz (Boersma 1993), on a 2048
  window. It costs two FFTs, so it is only measured where it can change the
  answer.

**References** (all relative, so input gain does not matter): each band's
background is its 10th percentile over the audible frames (digital silence
excluded); the vocal reference is the 95th percentile of the voiced frames'
level.

**Frame classes.** The pitch detector's voicing is the voiced/unvoiced
decision.
- *Sibilant-like*: unvoiced, at least 35% of the energy in the high band, the
  high band 9 dB over its background, and within 50 dB of the vocal
  reference. A frame the detector called voiced still qualifies with 65% high
  band and periodicity below 0.4 (voicing leaking a frame into an "s").
- *Breath-like*: unvoiced, mid+high energy 9 dB over its background, within
  55 dB of the vocal, centroid 1-4.5 kHz, at most 55% of the energy below
  900 Hz, periodicity below 0.8.

**Context.** Classified frames are joined into noise regions across unvoiced
gaps of up to 4 frames; within a region gap frames take the previous class
and a 5-frame majority vote removes flips. Then:
- A region shorter than ~150 ms that touches singing (voicing within 2 frames
  of either end) is a consonant: sibilance, including breath-like bursts such
  as an aspirated "h".
- A region of ~150 ms or more is a breath, except a sibilant run at an edge
  that touches singing (the "s" before an inhale stays sibilance).
- Sibilance must lie within ~175 ms of voicing (a click in a pause is not an
  "s"); breaths within ~1 s (room noise in a long pause is not a breath).
- Minimum lengths: 35 ms sibilance, 70 ms breath.

All thresholds are fields of `breathsibilance::Config`.

## Note edges

GAME starts each note at its vowel's pitch onset, so the consonant that
leads into a syllable ends up at the end of the previous note: on vocadito,
94% of the sibilant frames between two sung vowels sat in the previous
note's GAME span. `noteedges::attachConsonants` re-splits each pair of
neighbouring notes in the unvoiced gap between their vowels:

1. **Onsets first.** From the start of each note's vowel, the consonant
   frames running back join that note, up to ~300 ms, even where GAME put
   them in the previous note. The search stops at the previous vowel, so no
   note loses voiced frames.
2. **Codas.** Consonant frames right after a vowel that no onset took stay
   with that note, up to ~255 ms. This is a word-final consonant before a
   pause or a breath.
3. **Breaths** never join a note; one GAME put in a note's unvoiced head or
   tail is cut off at the breath nearest the vowel.

A consonant frame is unvoiced, not a breath, and has audio energy (the VAD
mask) or is labelled sibilance, so a quiet "s" counts. Silent closures of up
to ~46 ms inside a cluster ("st", "ks") are bridged. Where the voice runs
straight from one note into the next (legato), GAME's boundary stays. The
rules replace the earlier refinement, which grew each note's tail before the
next note's head, over loud frames only.

This does not change how a retuned note sounds: sibilants and breaths keep
the original recording whichever note holds them. It decides which note's
volume change a consonant takes, and that it moves with the vowel it leads
into when a note is moved in time.

## Stretching notes

A note made longer or shorter holds its vowel, as a singer would.
`notetiming::TimeMap` maps the note's timeline frames back to the recorded
frames it plays. The vowel - the voiced frames, with voicing gaps of up to 2
frames filled - takes up the whole change in length. Everything unvoiced
(consonants, stop closures, a breath) moves with the note at its recorded
length. Squeezed, the note shortens its vowel first, down to 30% of it (and
no fewer than 3 frames); past that, consonants and vowel shrink together. A
note without a vowel of 3 or more frames stretches evenly, as every note did
before.

With consonants at full length, every consonant frame lands on a whole frame
and plays exactly one recorded frame. So a retimed note's breaths and
sibilants keep the recording, copied from where they were recorded
(`protectMovedNoiseInBlend`), while the rest of the note is synthesized as
before. A note that is only moved keeps them the same way.

Everything that lays a note out over its frames uses the same map
(`NoteTiming.h`), so picture, pitch and sound stay together:

- the vocoder's mel and PSOLA's read position;
- the note's pitch contour. A contour is stored at the length it was written
  at (the recorded length for the analysed contour, the note's length at the
  time for a drawn one) and laid out through the map, so it stays on its
  syllable however often the note is stretched;
- the blend with the recording;
- splitting a note and slicing it at a region edge: a stretched note is cut
  at the recorded frame it plays at the cut, and each stored curve is cut
  there too;
- the piano roll: hatching, the pitch curve, and the waveform shown while a
  timing boundary is being dragged.

The timing tool moves the boundary between two notes. Since a consonant
belongs to the note it leads into, the boundary sits before it; with the
consonant at its recorded length, the vowel onset - where the beat is heard -
moves exactly as far as the boundary.

## Breath notes

Every breath outside the notes is drawn as a **breath note**: its waveform,
hatched like the other noise outside the notes but in the colour of the
note it belongs to, at that note's pitch. Dragging it up or down changes its level, 1 dB per 10 pixels, from
-60 dB to +24 dB; while dragging, the level shows and the background
waveform follows it, drawn with the gain the edit will apply
(`projectgain::changeRatio`). Double-click puts it back to 0 dB.

- **Placement** (`breathnotes::place`). Noise outside the notes is gathered
  into events (gaps of up to 4 frames, at least 3 frames long); an event
  holding any breath frame is a breath. A breath reaches across up to ~0.5 s
  (43 frames) of silence to meet the note it leads into, or else the note
  before it, taking in any other noise on the way. A breath further from
  every note stands on its own, at the pitch of the nearest note. Where a
  note is moved or stretched over a breath, the note's own audio plays
  there, so the breath note is cut back to the part no note covers.
- **Size.** Breath notes are drawn at the same scale as the note bodies, so
  they read as loud as they are, and at least 1 px thick. A breath is
  typically 15-30 dB below the singing around it, so most show as a thin
  line; their size follows their level, and the area around their row is
  what the pointer grabs.
- **Hover, no selection.** Breath notes are not selected: hovering one
  highlights its whole area - its shape and the band around its row, where
  it answers the pointer even when it is a thin line - as a hovered note is
  highlighted, and the highlight stays while it is dragged. A drag changes
  that one breath's level and leaves the note selection as it is; box
  selection and Select All are for notes only.
- **Storage.** Levels are kept per recorded frame in
  `AudioData::breathGainDb` and saved in the project as `breathGains`, runs
  of `[start, end, dB]` (written only when a level is set). A breath note's
  level is the one at its first breath frame; setting it sets every frame it
  spans, so it holds when notes around it move. Region slicing, merging,
  capture merges and timeline re-padding carry the levels with the labels.

## Sibilant Balance

The pitch tool's bottom-left handle - where Pitch Drift was; drift is still
in the pitch macro popup - sets a note's Sibilant Balance, from -100% to
+100% (0% by default). Like Melodyne's, it only ever turns audio down:

| Value | Hatched frames (sibilants, breath) | Rest of the note |
| --- | --- | --- |
| below 0 | down, to -60 dB at -100% | unchanged |
| 0 | unchanged | unchanged |
| above 0 | unchanged | down, to -60 dB at +100% |

The gain is the amplitude ratio `1 - |value|` in dB (-50% is -6 dB), floored
at -60 dB. Double-clicking the handle sets -100%; double-clicking again goes
back to 0%. With several notes selected, the handle changes all of them.
The value is saved per note (`sibilantBalance`, written only when it is not
0).

The hatched frames are the ones the note plays labelled audio at, through
its time map (`projectgain::hatchedSamples`), so the balance follows a moved
or stretched note. They follow the note's waveform, so the hatched part and
the rest of the note show their new levels while the handle is dragged.

Both edits, like the amplitude handle, are gain-only: `GainEditAction`
multiplies the rendered audio by the ratio of the project's gain curve
after and before the edit (`projectgain::regions`), with no resynthesis, and
the same curve is applied whenever audio is rendered again (in
`IncrementalSynthesizer`, including to the recording kept for breaths and
sibilants). Every gain floors at -60 dB, so the ratio always exists and
undo restores the audio exactly. While the amplitude or Sibilant Balance
handle is dragged, the note and the background waveform are drawn through
that same ratio (`projectgain::changeRatio`), so the preview is exactly
what the edit will do - also where an earlier cut left audio on the
smoothed edge of the curve, which a flat per-region gain would blow up into
spikes.

## Evaluation

On [vocadito](https://zenodo.org/records/5557945) (40 solo vocal tracks,
817 s, 44.1 kHz, F0 annotations, several languages) with SwiftF0 voicing at
its 0.5 threshold, as PitchNet runs it:

- **Voiced frames:** 0 of 38,500 annotated-voiced frames labelled, away from
  annotated voicing boundaries; 0.2% of the 7,987 frames within 3 frames of a
  boundary.
- **Labels:** 387 sibilance and 156 breath regions. Spot checks of random
  regions against spectrograms and lyrics found the breaths at phrase
  boundaries and the sibilants on s/sh/ch/f/t/h; unlabelled unvoiced frames
  were mostly voicing tails, stop closures, nasals and clicks.
- **Note segmentation:** through the full analysis (SwiftF0 + GAME), breath
  frames inside notes fell from 246 to under 30 across the 40 tracks; the
  remainder are breaths between two voiced parts of one GAME note, which are
  left alone.
- **Note edges:** the previous and the new refinement applied to the same
  GAME notes (GAME's output varies a little from run to run, so separate runs
  are not comparable):

  | Sibilant frames | Previous refinement | Note edges |
  | --- | --- | --- |
  | between two vowels of a phrase, in the next note | 4.5% | 95.8% |
  | between two vowels of a phrase, in the previous note | 92.0% | 3.2% |
  | after a breath or pause, in the next note | 33.2% | 92.8% |
  | before a breath or pause, in the previous note | 52.5% | 80.9% |
  | stray amber capsules (3+ frames) | 58 | 11 |

  The note count is unchanged (1,952).
- **Stretching:** on four tracks (vocadito 3, 6, 18 and 38), every boundary
  into a note that starts with a sibilant was moved later by up to 8 frames -
  the timing tool's edit - and rendered: 14 boundaries, the note after each
  shortened by 16% on average. All 14 moved "s" runs played the recording
  sample for sample (73,728 samples, fades excluded), with the vocoder and
  with PSOLA, at their recorded length: 11.8 frames on average. The even
  stretch used before would have squeezed them to 10.0 frames and
  re-synthesized them.
- **Resynthesis:** on six tracks, every note containing sibilance was shifted
  +3 and +12 semitones and rendered through the vocoder (500+ sibilant frames
  inside the re-rendered notes). With the labels, those frames are
  bit-identical to the recording. Without them, the vocoder's versions were
  close: SwiftF0 called 1.5% (+3) and 0.6% (+12) of them voiced, against 0.9%
  in the recording, and per band they were within 0.5 dB below 3.5 kHz and
  about 1 dB duller above it (medians).
- **Robustness.** Recall of clear sibilant frames (annotated unvoiced, at
  least half the energy above 3.5 kHz, next to singing), voiced frames
  labelled (boundaries included) and breath frames found:

  | Condition | Sibilant recall | Voiced labelled | Breath frames |
  | --- | --- | --- | --- |
  | clean | 90.4% | 0.03% | 3,069 |
  | pink noise, 30 dB below the voice | 86.1% | 0.03% | 929 |
  | white noise, 30 dB below the voice | 76.8% | 0.03% | 582 |
  | room reverb (DRR ~5 dB) | 64.0% | 0.07% | 2,002 |

  Noise mostly costs breaths, which sit 25-40 dB under the voice and sink
  into the floor; it barely creates false ones (7 and 1 breath frames that
  the clean take leaves unlabelled). Reverb does: 211 frames of reverb tail
  after phrases are labelled breath.

Voicing cues compared on the same frames (ROC AUC against the annotations,
boundaries excluded): SwiftF0 confidence 0.998, **hnsep** harmonic share
0.990, autocorrelation periodicity 0.985. hnsep (`hnsep_VR.onnx`, the
harmonic/noise separation model upstream HachiTune ships and PitchNet carried
until 2e7e4ad, 92.6 MB) separates
the noise of breaths and sibilants cleanly, but its noise share tracks
SwiftF0's voicing almost exactly and says nothing about breath versus
sibilance or background, while costing ~0.33x real time on two CPU cores.
The detector therefore uses SwiftF0's voicing and does not need the model.

## Limitations

- Voiced fricatives (z, v) are left to the voice: they are voiced, and their
  pitch should follow the note.
- Heavy reverb fills consonant gaps with periodic, low-frequency energy;
  those frames are mixed, and are rightly not kept as original audio.
- A breath in the middle of a GAME note, between two voiced parts, is not
  split out.
- Note edges cannot tell a word-final consonant from the next word's onset
  when nothing separates them ("this song"): the whole cluster joins the
  next note. A pause or a breath between them splits it correctly.
- Kept-original frames take a note's volume edits but not its formant shift,
  the same as the long unvoiced runs that were already kept original.
- Sibilant Balance acts on the hatched frames only: a voiced consonant (z, v,
  m) is part of the rest of the note.
- Stretching: voiced consonants (m, n, l, z, v) are voiced, so they stretch
  with the vowel. A stretched vowel's vibrato slows by the vowel's stretch
  factor, which is larger than the note's now that the consonants take none
  of it; the scoop into the vowel and its release are stretched too.
- Labels assume the pitch detector's voicing; FCPE/RMVPE (Debug builds) work,
  but the thresholds were checked with SwiftF0.
