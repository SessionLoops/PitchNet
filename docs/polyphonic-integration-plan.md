# Polyphonic editing in the main interface — integration plan

Draft, 2026-11-17; decisions of 2026-11-17 folded in (see
[Decisions](#decisions)). It builds on
[polyphonic-editing.md](polyphonic-editing.md) (the engine and its
"Integrating into PitchNet" outline) and
[testing/vocal-detection-research.md](testing/vocal-detection-research.md)
(the input-type classifier).

Goal: a track whose input is not a single voice is analysed by the polyphonic
engine and edited in the main piano roll — pitch, timing, length, level and
mute per note — in the standalone app and in the plugin (ARA and non-ARA),
saved with the project, in the same look as the vocal editor. A single voice
keeps today's pipeline unchanged. This is a production feature: no
experimental flag.

Note assignment (activating rejected candidates) has since been built: see
[polyphonic-editing.md](polyphonic-editing.md#note-assignment-mode). The guided detector is in, as a per-take choice (feature review
#17). Which of today's editing features carry
over is decided one by one in the [feature review](#feature-review).

## What is already in place

| Piece | Where | What it gives the integration |
|---|---|---|
| Input-type classifier | `Source/Audio/Analysis/InputTypeClassifier`, `YamnetVoiceTagger` | `classify()` → `Decision{pipeline, reason, features, confident()}`; not called yet |
| Engine | `Source/Audio/Polyphonic/PolyEngine.h` | `analyze()` → `Analysis` (notes, candidates, masks); `render(analysis, channels, edits)`; `renderNotes()` for auditioning; `NoteEdit{semitones, gainDb, mute, timeOffsetSeconds, stretch, takeBackground, …}`; JUCE-free, worker-thread only |
| Basic Pitch evidence | `Source/Audio/Polyphonic/BasicPitchEvidence` | note posteriorgram for both the classifier and `analyze()` |
| Full edit loop, standalone | `Source/UI/Polyphonic/PolyLab.cpp` | analysis thread, `NoteCanvas` (blobs, drag, stretch handles), background render, audition, A/B |
| Rendering seam | `AudioData::waveform` | every consumer — standalone playback (`AudioEngine`), export, and ARA publishing (`setProcessedAudio` from the project's waveform, `PluginProcessor.cpp` ~1700 and ~1850) — reads the rendered result from here; `originalWaveform` stays pristine |
| Per-take ownership | ARA: one `Project` per `PitchNetAudioModification` (project archive + processed audio, persisted with the modification); standalone: one `Project` per file | the polyphonic take can live on the `Project` and inherit ownership, persistence and playback |
| Views | `IMainView` / `MainComponent`, `PianoRollComponent` and the shared parts in `Source/UI/PianoRoll` (`CoordinateMapper`, `GridRenderer`, `PianoKeysRenderer`, `TimelineRenderer`, `ScrollZoomController`, `WaveformBackgroundRenderer`) | a polyphonic note layer can reuse everything except the note and pitch-curve drawing |
| Models | `Resources/models/basic_pitch_nmp.onnx`, `yamnet.onnx` | downloaded by CMake (`OPTIONAL_MODELS`), pinned by SHA-256 |

## Design

### 1. The mode belongs to the Project

```cpp
enum class AnalysisMode : std::uint8_t { Vocal, Polyphonic };

struct AnalysisModeState {
    AnalysisMode mode = AnalysisMode::Vocal;
    bool userChosen = false;                   // override: never re-detected
    inputtype::Reason reason{};                // why the classifier chose it
    float voiceScore = -1.0f, poly2 = -1.0f;   // shown in the UI, saved for diagnostics
    bool confident = false;
    polyphonic::DetectionMode detector = polyphonic::DetectionMode::Harmonic; // polyphonic takes
};
```

**One mode per track** (decided, feature review #23). Classification still runs
per take, but the mode that applies is the track's:

- **Standalone** and **non-ARA capture**: one take is one track, so the take's
  classification is the track's mode.
- **ARA**: the track's mode is the duration-weighted majority of its clips'
  classifications, taken when the track is first analysed. After that it is
  **sticky**: clips added later are analysed in the track's mode (their own
  classification is still stored, for the UI and diagnostics), and only the
  user's override changes it. Otherwise adding one clip could flip the
  majority and re-analyse — discarding the edits of — every other clip.
  Changing the mode re-analyses every clip on the track after one
  confirmation, and their edits are discarded (decision 2).

ARA region sequences have no persistent ID, so the track's mode is stored as
a copy in each of its modifications' projects (serialised with
`AnalysisModeState`) and keyed at run time by the existing `araTrackKey()`
(the key the per-track undo managers use). A modification that appears on two
tracks with different modes follows the track it was first analysed on.

### 2. A polyphonic take next to the vocal data

```cpp
struct PolyphonicTake {
    std::shared_ptr<const polyphonic::Analysis> analysis;  // immutable once built
    juce::AudioBuffer<float> source;   // the take's own channels (stereo kept), project rate
};
// in Project: std::map<int, polyphonic::NoteEdit> polyphonicEdits (by index into
// analysis->notes), and the latest render for playback
```

*As built (M3): the edits live in the `Project` (`getPolyphonicEdits`), not in
the shared, immutable take, since they change on every edit. The render does
not go into `AudioData::waveform` either: `waveform` stays mono (the render's
mix, for display), and the stereo render is kept as the project's polyphonic
playback (`Project::getPlaybackBuffer`), which the audio engine now plays in
stereo.*

`Project` gains `std::shared_ptr<PolyphonicTake>` (copy-on-write: the plugin
copies whole Projects for snapshots, e.g. `std::make_shared<Project>(*current)`
in `requestCapturedAudioAnalysis`, and a 23 MB analysis must not be copied each
time). For a polyphonic project the vocal analysis fields (`melSpectrogram`,
`f0` and the other pitch stages, `notes`, `breathSibilance`) stay empty.

**Channels and rate.** Today the standalone loader mixes to mono and resamples
to 44.1 kHz by linear interpolation (`EditorController.cpp` ~515–570) before
analysis. That is acceptable for a voice; a piano or string section must keep
its stereo image. The take therefore keeps its own `source` buffer: the input's
channels, resampled per channel to the project rate (44.1 kHz) with
`AudioResampler` (r8brain). Analysis runs on their mono mix, `render()` on the
channels, and the render (stereo) goes into `AudioData::waveform`. Keeping the
project rate at 44.1 kHz leaves all existing time arithmetic
(`SAMPLE_RATE`/`HOP_SIZE`, `CoordinateMapper`) valid; polyphonic note frames
(`Analysis::params.hop`) convert through seconds (`Analysis::frameToSeconds`).
Every consumer of `waveform` must then accept two channels — see
[Risks](#risks).

### 3. Analysis: classify, then branch

In `EditorController::analyzeAudioImpl`, which both the standalone loader and
the host paths (`setHostAudioAsync`, ARA and non-ARA capture) reach:

1. If `AnalysisModeState::userChosen`, use that mode. Otherwise classify:
   `selectExcerpt()` on the mono mix, SwiftF0 on the excerpt (the pitch track
   must cover the excerpt, as `classify()` requires), `classify()` with the
   shared Basic Pitch and YAMNet instances. About 0.2 s for 30 s of audio.
2. **Vocal**: today's path, unchanged.
3. **Polyphonic**: Basic Pitch evidence on the whole take, then
   `polyphonic::analyze(mono, Params::forSampleRate(44100), &evidence, cancel,
   progress)` — about one seventh of real time — and build the
   `PolyphonicTake`, with the take's detector (Harmonic or Guided, chosen in the
   side panel's Analysis section). Both use the Basic Pitch evidence: Harmonic
   in its note filter (2 of 21 features; without the model it falls back to
   DSP-only weights and keeps more overtone ghosts), Guided also to steer the
   candidate search, so Guided is unavailable without the model. No mel spectrogram, F0, GAME or
   vocoder.

Basic Pitch and YAMNet join `SharedInferenceModels` (CPU only, each behind its
own mutex like SwiftF0), so one instance serves the classifier, every analysis
and every editor. A missing model degrades as designed: no YAMNet → pitch cues
decide; no Basic Pitch → no polyphony gate and DSP-only note filtering.

Changing the mode in the UI sets `userChosen` and re-runs analysis in the new
mode. **Decided:** the take's edits are discarded, after a confirmation when
there are any; undo history for the take is cleared.

### 4. Rendering

A new `EditorController::renderPolyphonicAsync(Project&, …)` beside
`resynthesizeIncrementalAsync`, with the same contract (one render in flight,
`pendingRerun` coalescing, completion on the message thread):

```
polyphonic::render(*take.analysis, take.source channels, project edits, cancel)
    → AudioData::waveform → AudioEngine's deferred waveform hand-off
                          → (plugin) the existing publish to the modification
```

The engine copies the frames no edit reaches, so a full render costs time in
proportion to the edits (0.25 s for 70 s of stereo piano with four edits);
re-rendering the whole take on each committed edit is fine to start with. The
publish code keys off `projectHasRegionEdits()`, which must also return true for
a polyphonic project with non-identity edits. While a note is being dragged,
`renderNotes()` on the dragged notes feeds the existing drag-audition callback
(`setOnRequestDragAudition`). The synthesis-engine and GPU settings do not apply
to polyphonic projects and are hidden for them.

`takeBackground`: **decided on**, as in the Lab — every edit sets it, so muting
a piano note also removes its hammer noise and a moved note takes its attack
along. The engine documentation still calls it experimental; promote it there
(and make it the `NoteEdit` default) as part of M3.

### 5. Piano roll

`PianoRollComponent` (4,600 lines) is built around one pitch curve per project
and `Note` frames on the vocal grid; generalising it would touch most of it.
Instead, add a `PolyphonicNoteLayer` in `Source/UI/PianoRoll`, ported from the
Lab's `NoteCanvas`, and have the piano roll draw and route input to it when the
project is polyphonic, keeping the shared grid, keys, timeline, waveform
background, zoom and scroll.

The vocal toolbar tools (Main, Note Separation, Pitch Drawing, Timing) are
vocal-only and hidden for a polyphonic take, and polyphonic notes have no
handles. Polyphonic notes use one
interaction, as in the Lab's `NoteCanvas` today:

| Gesture | Edit |
|---|---|
| drag a note (snapped to semitones; Alt: cents) | locked to the direction of its first 6 px: mostly vertical changes `semitones` only, mostly horizontal `timeOffsetSeconds` only; moving more than 40 px the other way adds that axis for the rest of the drag |
| drag the right end | `stretch` (drawn with `stretchMap()`). It may pass the end of the recording: the render pads the take with silence to fit, so the take grows (and shrinks back on undo); the canvas has one bar of room past the end |
| drag the left end | `stretch` + `timeOffsetSeconds`, end stays put. On a selected note either end stretches the whole selection: every note's grabbed end moves by the same time, within its own limits (deleted notes excluded) |
| click / Shift-click / drag on empty space | select / add to selection / band select; overlapping notes stay individually selectable (Cmd-drag is the canvas's zoom gesture on macOS, so Shift adds) |
| Delete | toggles `mute` on the selection. Muted (deleted) notes stay drawn as a dashed outline and selectable, so Delete again or Restore Original brings them back; they take no other edit (no drag, stretch, double-click snap or level) |
| right-click | menu with Restore Original for the selection (#19) |
| Note / Cent / Level | unlabelled boxes in the centre of the main toolbar (where the vocal edit tools are), each showing its unit and named in its tooltip, "–" with nothing selected; for the selection: Note (name; drag, type a name or +N/-N), Cent (from the nearest semitone), Level (dB). Several notes: Note and Cent show the first selected note or "–" where they differ, and a change moves every note by the same amount (their spread is kept); Level sets them all. One commit on release or Enter; deleted notes are left out (#9) |

Undo: one `PolyphonicEditAction` holding before/after `NoteEdit`s of the notes
it touched, in the existing `PitchUndoManager`. Track mode: a pinned
polyphonic track draws its notes as the pinned overlay, like a vocal one. MIDI
export (`MidiExporter`, which takes `std::vector<Note>`) takes the edited
polyphonic notes through a small adapter — pitch plus moved and stretched
times, velocity from each note's level (#21) — the first real benefit of
polyphonic notes outside editing.

### 6. Showing and overriding the mode

The track list and the toolbar show the track's mode; a menu offers "Analyse
track as Vocal / Polyphonic". When the decision was not confident (`Decision::confident()`
false: pitch cues only, or a default), the badge says so, so a user knows to
check it. The classifier's reason and scores go to the log and the diagnostics
the plugin already writes.

### 7. Saving

Bump `ProjectSerializer::FORMAT_VERSION` 5 → 6 with a `polyphonic` section;
files without it load as vocal. **Decided: save the notes and the edits, and
rebuild the masks on load.**

| | Saves | Load | Size |
|---|---|---|---|
| A. Edits only, re-analyse on load | mode, edits keyed by note start and median pitch | full analysis and re-matching of edits to notes | tiny |
| **B. Notes + edits, rebuild masks (chosen)** | mode, `Analysis::notes` (active and candidates), edits by index | spectral analysis + `buildMasks` on the saved notes, no detection | a few hundred kB per minute of dense piano (estimate) |
| C. Full analysis | B plus masks | none | ~20 MB per minute: too much for host session files |

**Measured rebuild cost** (`analyzeSpectra` + `buildMasks` on the saved active
notes, DSP-only analysis, `-O2`, this machine's Linux VM): the rebuilt masks
are bit-identical to the original analysis on all three files, so edits always
land on the same notes. The rebuild is not instant:

| File | Audio | Full analysis | Rebuild (spectra + masks) | Share |
|---|---|---|---|---|
| Brahms, string orchestra | 45 s, 240 notes | 4.03 s | 0.89 + 1.51 = 2.40 s | 60 % |
| Piano arpeggio | 4 s | 0.19 s | 0.15 s | 78 % |
| Strings with vibrato | 3 s | 0.21 s | 0.15 s | 70 % |

With Basic Pitch evidence (about 3 % of real time, only in a full analysis) the
rebuild is roughly half of a full analysis: about 3 s per minute of dense audio.
Masks are only needed to render an edit, so the rebuild runs **in the
background after load**: the notes are drawn at once from the saved notes;
playback uses the original audio (unedited take) or, under ARA, the processed
audio the modification already persists; only a standalone project with edits
waits for the rebuild before its first render. The next edit waits for it only
if it arrives first.

B needs a public entry point that runs `analyzeSpectra` + `buildMasks` for
given notes (both internal, `PolyInternal.h`); polyphonic-editing.md already
anticipates it for note assignment. The `source` channels are not saved:
standalone re-reads the audio file (its SHA-256 is already checked), ARA reads
the source again, as for vocal projects today.

### 8. Plugin specifics

- **ARA**: the mode is the track's (section 1). Nothing new in the playback renderer — it mixes each modification's
  processed audio, wherever it came from. Mode, take and edits travel in the
  modification's project archive. Memory is the concern: an `Analysis` is about
  20 MB per minute of dense material while its project is alive; a session
  with many polyphonic regions needs a policy (drop analyses of regions not
  shown and rebuild them from saved notes on demand, about 3 s per minute).
- **Non-ARA capture**: after capture it is the standalone flow.
- The track-analysis queue (testing/track-analysis-queue.md) is unchanged: one
  job per modification, whichever mode it runs.

## Milestones

No experimental flag, so nothing user-visible may land on master half-done.
M1 can ship on its own (it changes no analysis). M2–M5 are developed on a
`polyphonic` branch, kept rebased on master, and merged when M5 passes the DAW
checks; each milestone still ends in its own commits and tests.

1. **M1 — Mode only (master).** Classifier in `analyzeAudioImpl`,
   `AnalysisModeState` saved and shown, override in the menu — but every take is
   still analysed as vocal. Lets the classifier meet real sessions with no risk.
   *Done 2026-11-17 for the standalone app (branch `polyphonic-m1`). In the
   plugin each clip is classified and shown on its own; the track-level mode
   (majority, sticky, track-wide override) moves to M5, together with the
   classify-before-analysing pre-pass that gives "majority at first analysis"
   a defined moment.*
2. **M2 — Polyphonic analysis, read-only (branch).** Polyphonic takes get a
   `PolyphonicTake` (stereo source, Basic Pitch in `SharedInferenceModels`);
   notes drawn in the piano roll in the current look; playback is the original.
   *Done 2026-11-18 for the standalone app (branch `polyphonic`): analysis,
   the note layer, the Analysis card (Harmonic / Guided) and re-analysis on a
   mode or detector change (`EditorController::reanalyzeAsync`, after a
   confirmation when the take has edits). In the plugin the Input menu still
   only stores the choice (M5).*
3. **M3 — Editing, standalone (branch).** The features agreed in the
   [feature review](#feature-review), undo, `renderPolyphonicAsync`, drag
   audition, `takeBackground` on.
   *Done 2026-11-18 for the standalone app (branch `polyphonic`):*
   - *Engine: `NoteEdit::takeBackground` on by default (tests that measure
     plain shares now say so); `NoteEdit` equality.*
   - *Rendering: `EditorController::renderPolyphonicAsync` (one at a time,
     cancel and re-run with the newest edits), reached through
     `resynthesizeIncremental()`, so commits, undo and redo all render; the
     result plays in stereo (`AudioEngine` now plays two channels when the
     waveform has them).*
   - *Editing: `PolyphonicNoteLayer` draws notes where their edits put them
     (muted ones dashed) and takes the section 5 gestures; drag snap is
     relative and on by default (`snapToSemitonesPolyphonic`, saved), Alt
     inverts it and gives cents; double-click tunes a note to its Double
     Click Snap target; Delete toggles mute; right-click offers Restore
     Original. One `PolyphonicEditAction` per gesture.*
   - *Live audition: the dragged notes through `renderNotes`, newest request
     wins.*
   - *MIDI export and Drag to DAW: the edited notes, velocity from level
     (`PolyphonicMidi.h`). Key detection from the take's notes.*
   - *Added while testing: Note / Cent / Level boxes in the main toolbar
     (in sync with the notes both ways, render on release); group stretch;
     lengthening past the end of the recording (the take grows); deleted
     notes read-only; Live Audition on by default for polyphonic takes and
     sounding on press; the Polyphonic Lab removed.*
   - *Not yet: saving (M4), the plugin (M5).*
4. **M4 — Saving (branch).** Format 6, notes + edits, background mask rebuild,
   load tests.
   *Done 2026-11-19 for the standalone app, awaiting a test in the app:*
   - *Engine: `polyphonic::restore()` rebuilds an analysis from saved notes
     (spectra + `buildMasks`, no detection): masks bit-identical to the
     analysis (PolyphonicTests #18), 64% of a full analysis on 46 s of
     Brahms (2.4 s vs 3.7 s), without Basic Pitch.*
   - *File: `FORMAT_VERSION` 6, a `polyphonic` section (sample rate, FFT
     size, hop, detector; the notes as a base64 binary block, exact; the
     edits as JSON). A project read with one holds it as a pending restore
     (`Project::getPendingPolyphonicRestore`) and its edits; saved again
     before the restore, it is written back unchanged.*
   - *Load: the project opens at once in the polyphonic view with a
     restoring take (`makeRestoringTake`): the saved notes, drawn from their
     saved levels (notes block version 2: each note's level per frame,
     `noteLevelsFromMasks`), so it looks exactly as it will once restored; it
     can be selected and edited and its saved (edited) audio plays. In the
     background `EditorController::restorePolyphonicAsync` rebuilds the masks
     from the audio file when it is there and unchanged (stereo), else from
     the saved mono waveform, with nothing shown (rendering waits for it).
     Rendering and Live Audition wait for it; the restored take then goes
     into the live project (edits made meanwhile, their undo steps and the
     selection stay) and the edits render. The restore does not mark the
     project as changed. (Version 1 files, without levels, show thin notes
     until restored.)*
   - *Tests: `PolyphonicSerializationTests` (JSON, binary archive, saved
     again before the restore, vocal project, malformed section).*
5. **M5 — Plugin (branch → master).** Track-level mode (pre-pass that
   classifies a track's clips before analysing them, duration-weighted majority,
   sticky, track-wide override written to every clip's stored project), ARA
   publish, archive, memory policy,
   non-ARA capture, Track mode overlays; the track-analysis-queue checks in a
   DAW; then merge.
   *Part 1 (ARA, one clip at a time), 2026-11-19, awaiting a test in REAPER:*
   - *Analysis: the plugin's controllers analyse polyphonic clips
     (`setPolyphonicAnalysisEnabled`); the ARA read keeps the source's
     channels (at most two) beside the mono read the vocal path always used
     (`setHostAudioAsync(..., takeChannels)`); a clip analysed as vocal drops
     them again.*
   - *Rendering: the host's render path (`requestPluginProjectRender` →
     `resynthesizeIncrementalAsync`) renders a polyphonic take with
     `renderPolyphonicAsync`; the stereo render is what is published to the
     modification (`projectHasRegionEdits`: the take's note edits).*
   - *Archive: the format 6 section travels in the modification's archive. On
     reopen the region is hydrated from the host source: the saved render is
     its playback, the notes are drawn at once from the saved levels and the
     masks are rebuilt on a background thread (`restorePolyphonicTake`);
     edits made meanwhile render once it lands.*
   - *Live Audition in the plug-in (after the merge): the dragged notes are
     rendered as in the standalone app and played through the host's preview,
     as the vocal drag audition is (`MainComponent::auditionPolyphonicNotes`).*
   *Part 2 (one mode per track, ARA), 2026-11-19, awaiting a test in REAPER:*
   - *A clip is analysed in its track's mode
     (`PitchNetAudioProcessor::analyzeAraRegionForCanvas`). The track's mode
     is found in `araTrackModes` (session, keyed like the track list), else in
     the copy an analysed clip of the track keeps (a reopened session), else
     made by a pre-pass: every clip on the track without an analysis is
     classified (`EditorController::classifyHostAudioAsync`) and the
     duration-weighted majority wins (`analysismode::trackMode`; nothing
     classified gives Vocal). The clips' own classifications are reused by
     their analyses and stay recorded.*
   - *Sticky: clips added later are analysed in the track's mode
     (`HostAnalysisOptions::trackMode`, applied after the clip's own
     classification).*
   - *Override: the Input menu in the plugin applies to the track
     (`changeAraTrackAnalysisMode`): after a confirmation when any of its clips
     has edits, every clip's analysis, archive and published render are
     discarded and the clips are analysed again (the active one at once, the
     others by the track queue or when opened).*
   *Part 3 (Track mode and memory, ARA), 2026-11-19, awaiting a test in REAPER:*
   - *Track mode draws the track's other polyphonic clips with a preview
     `PolyphonicNoteLayer`, and pinned polyphonic tracks as outlines in the
     track colour (`setOutlineColour`). A preview holds a drawing take
     (`makeDrawingTake`: notes and levels, no masks), so it keeps no analysis
     alive; a clip not yet restored is drawn from its saved notes. The
     overview gets the notes where their edits put them.*
   - *Memory: a polyphonic clip leaving the canvas is set aside
     (`setAsidePolyphonicTake`): its notes, levels and edits stay (as a
     pending restore), the masks, the take's channels and its render go.
     Shown again, it is hydrated from the host source like a reopened session
     (its published render is its playback) and its masks are rebuilt in the
     background; edits made meanwhile render once they land.*
   - *Behind a polyphonic take the waveform is drawn onset-aligned (a window
     ending at each point, forward smoothing only), so it no longer shows a
     sound before it starts (`VisualWaveformEnvelope::build(..., alignOnsets)`).*
   *Non-ARA capture, 2026-11-19, awaiting a test in a host without ARA:*
   - *The instance's mode is the first capture's classification (or the
     user's choice), sticky for later takes (`HostAnalysisOptions::trackMode`).*
   - *A polyphonic capture is placed at its host position and spliced into the
     earlier ones (`placePolyphonicCapture`): one take for the whole timeline,
     whose notes are the earlier notes the new take does not record over (with
     their edits) plus the new take's, shifted by whole frames; its channels,
     waveform and playback (the earlier render with the new take raw) are
     pasted at the exact sample. It is drawn and editable at once and its
     masks are rebuilt for the whole timeline in the background
     (`startCapturePolyphonicRestore`); polyphonic timelines are never re-padded
     (`repadProjectToTimelineOffset`).*
   - *The realtime processor plays `Project::getPlaybackBuffer()`, so the
     stereo render.*
   - *Plug-in state: the format 6 section; on load the timeline is drawn from
     its saved notes, plays its saved render and its masks are rebuilt from the
     saved original. Since the merge a stereo take keeps its channels and
     render in the state's waveform slots (`ProjectSerializer::toBinaryArchive`;
     read back into `PolyphonicRestore::channels` / `playback`, the project
     keeping their mono mixes), so the take stays stereo after a reload.*
   - *The Input menu in capture mode analyses the takes already recorded again
     in the chosen mode (after a confirmation when they have edits), as one
     take from the timeline's start (`reanalyzeCapturedTimeline`); later takes
     follow it.*
6. **After the first release.** Note assignment, and whatever the feature
   review defers.

## Feature review

Same look as the vocal editor: amplitude-shaped blobs on the semitone grid,
the waveform behind, the overview strip below, the same panels. What changes is
that notes overlap in time and no pitch line is drawn (#2). Each existing
feature is reviewed one by one; the "Proposal" column is a starting point,
"Decision" is filled in as we go.

| # | Feature | What it does for a voice | Proposal for polyphonic | Decision |
|---|---|---|---|---|
| 1 | Note blobs | width follows the loudness of the mix; colour shows tuning (blue in tune → purple → pink → red) | width from the note's own level (its mask amplitudes per frame), not the mix | **Own level; same tuning colours** (median pitch + edit). With the mix's level every note sounding at once would get the same width, a held bass would swell with the melody, and a gain edit would hardly show. **No hover state; a selected note gets a white outline that follows its shape**, not a box and not a colour change: colour already means tuning, and a box around each note's bounding rectangle runs into its neighbours in a chord. |
| 2 | Pitch line | continuous F0 line, also between notes | each note's own f0 inside its blob; nothing between notes | **Not shown for polyphonic takes.** Notes are edited as whole units (SampLab-style: position, pitch, length, level), with no handles or curve tools, so a pitch line has nothing to act on; the blob's colour already shows tuning. `Note::f0` stays in the engine for analysis and a possible later curve editor (#6, #10). |
| 3 | Waveform background, overview strip | the take's loudness centred behind the notes (not tied to pitch); overview of the take with notes as short coloured lines | keep | **Keep both unchanged**: the background from the take's mono mix; chords show as stacked lines in the overview. |
| 4 | Main tool (1) | select, box select, drag a note up/down for pitch, double-click snap, the note handles | — | **Vocal only.** The four toolbar tools (#4–#7) stay with the vocal editor and are hidden for polyphonic takes, which use the Lab's single interaction instead (section 5). |
| 5 | Note Separation tool (2) | split a note | — | **Vocal only** (see #4); the engine edits whole notes. |
| 6 | Pitch Drawing tool (3) | draw a new pitch curve | drop for now (`NoteEdit::pitchCurve` exists for later) | **Vocal only** (see #4); not in the first release for polyphonic. The engine supports per-frame curves (`NoteEdit::pitchCurve`: phase-continuous, follows stretch, tested); the missing part is the UI for drawing on overlapping notes. |
| 7 | Timing tool (4) | drag the boundary between neighbouring notes: one gets longer, the next shorter | — | **Vocal only** (see #4). Polyphonic timing and stretching work as in the Polyphonic Lab: drag a note to move it, drag either end to stretch it. |
| 8 | Fine Pitch handle (cents) | detune a note | — | **No handles on polyphonic notes.** Cents come from Alt-dragging the note, as in the Lab. |
| 9 | Amplitude handle | note level | — | **No handle; a Level (dB) field**, applied to every selected note (`gainDb`). It sits in the main toolbar's centre beside Note and Cent boxes (polyphonic takes only; section 5); one undo step per drag or typed value. The Lab never exposed level. |
| 10 | Pitch Drift, Left/Right Slope, Pitch Modulation handles | reshape the pitch curve | drop for now (could map to `pitchCurve` later) | **No handles on polyphonic notes**; each would generate a `pitchCurve` if pitch-curve editing comes later (#6). |
| 11 | Formant Shift handle, Formant parameter | voice timbre | drop: instrument partials are not formants | **Dropped** (vocal-only; no handles on polyphonic notes). |
| 12 | Sibilant Balance, breath notes, Show Breaths & Sibilance | consonants and breaths | drop | **Dropped** (vocal-only; no handles on polyphonic notes). |
| 13 | Pitch Center macro | pull notes to the semitone | — | **Correct Pitch Macro button removed for polyphonic takes for now** (#13–#15). A quantize toggle for polyphonic may come later. |
| 14 | Pitch Drift macro | flatten drift within notes | — | **Removed with the macro button** (#13). |
| 15 | Snap to Scale, scale/key, Use Project Key | key-aware snapping in the macro and in drag snapping | — | **The macro's Snap to Scale goes with the button** (#13). **Scale and key settings stay** for Scale drag snapping and double-click (#20); the key is detected from the polyphonic notes. |
| 16 | Global Pitch, Volume | whole-take transpose and level | — | **Legacy, not shown in the side panel.** Global Pitch is not applied to polyphonic renders; Volume works as for vocal (shared playback path). |
| 17 | Rendering section (AI Resynthesis / Classic), device | vocoder choice | — | **For polyphonic takes the side panel's Rendering section is replaced by an Analysis section: Harmonic / Guided** (`DetectionMode`). Default Harmonic, the engine's default. Changing it re-analyses the take and discards its edits after a confirmation, as for a mode change. Both use Basic Pitch (Harmonic in its note filter, falling back to DSP-only without it; Guided also for its search, so it needs it). The global device setting is unchanged (vocal only). |
| 18 | Base / Delta pitch overlays, Fourier Pitch Filter | pitch-curve analysis views | — | **Nothing to change in menus.** Show Base/Delta Pitch are in no menu, only on Cmd/Ctrl+Shift+B / D (all builds): they do nothing for a polyphonic take. The Fourier Pitch Filter is unreachable (its command is never registered, so the Edit-menu entry is skipped): left as is. |
| 19 | Live audition, context audition, Restore Original | hear a note while dragging (toolbar toggle); hover buttons on a note: play it in context, reset its edits | — | **Live Audition kept** (dragged notes through `renderNotes`). **Context Audition removed** for polyphonic. **Restore Original from a right-click menu** on the selection (clears the selected notes' `NoteEdit`s, one undo step); no hover buttons on polyphonic notes. |
| 20 | Drag Snap, Double Click Snap, Snap Cycle, grid, tempo | snapping and grid | — | **As in the vocal editor, with drag snap on by default.** Drag snap is relative (`PitchEditor::getSnappedDragOffset`: whole-semitone offsets, Chromatic or Scale; a sharp note stays sharp), with the same one-drag inversion; Alt gives cents. Default **on** for polyphonic (vocal stays off): `MacroParameters` is shared by every project of a plugin instance and a track can mix vocal and polyphonic takes, so polyphonic gets its own field (`snapToSemitonesPolyphonic = true`) and each mode remembers its own on/off. Double Click Snap (Pitch Center / Scale / MIDI Reference) sets the note's pitch edit to reach the target: the deliberate way to tune a note. Timeline settings unchanged; time moves are not grid-snapped. |
| 21 | MIDI export, Drag to DAW, MIDI reference guides | notes as MIDI (`MidiExporter`: one channel, velocity 100, pitch rounded) | — | **Keep all three.** Export and Drag to DAW go through an adapter from polyphonic notes: edited pitch, moved and stretched start and end, muted notes skipped; chords export as overlapping notes on one channel. **Velocity from each note's level**: its peak level from the analysis plus its `gainDb`, relative to the take's loudest note, mapped −40…0 dB → velocity 1…127 (clamped; the mapping is a starting point to tune on real takes). MIDI reference guides unchanged. |
| 22 | Vertical auto-zoom, blob height, follow, loop | view | — | **Keep all unchanged.** Auto-zoom fits a wider range on polyphonic material (thinner semitone rows); Blob Height (shared with vocal) keeps tall blobs in neighbouring rows from overlapping. |
| 23 | Track mode pinned tracks | other tracks' notes as overlay | — | **One mode per track**: the duration-weighted majority of its clips' classifications at the track's first analysis, then sticky (new clips follow it; only the user's override changes it, re-analysing the track's clips after a confirmation). Track mode therefore never mixes vocal and polyphonic clips. Pinned polyphonic tracks draw their blobs outlined in the track colour, like pinned vocal tracks. See section 1. |
| 24 | Select All, undo/redo, rendering after edits | | — | **Keep.** There is no Render button any more (removed: edits render automatically); polyphonic edits render automatically too (`renderPolyphonicAsync`, coalesced), with undo through `PolyphonicEditAction`. Select All selects the active notes. |

Not planned for now: per-note pan and note slicing (both in SampLab). Pan needs
new engine work (`NoteEdit` has no pan); slicing would split one note's masks
into two notes.

## Risks

- **Code that assumes vocal data.** 39 sites in 16 files test
  `f0.empty()` / `melSpectrogram.empty()`, and more read `getNotes()` or `f0`
  directly (piano roll handlers, `PitchEditor`, `IncrementalSynthesizer`,
  `MainComponent`, `PluginProcessor`). Each must either be unreachable for a
  polyphonic project or treat it explicitly; M2 starts with that audit.
- **Stereo `waveform`.** The standalone loader builds it mono, and
  `AudioEngine` plays channel 0 and copies it to the other outputs
  (`AudioEngine.cpp` ~187, ~297); the host path keeps the host's channels. The
  engine's playback, the waveform display, export and the ARA publish must
  all handle two channels.
- **Analysis time.** One seventh of real time: a 5-minute take takes about 40 s.
  The loader's progress and cancel paths must cover it.
- **Memory** in ARA sessions (section 8).
- **Classifier thresholds** are provisional until real solo instruments and
  harmony vocals are tested (vocal-detection-research.md); M1 exists partly to
  gather that evidence.

## Decisions

1. **Saving**: notes + edits, masks rebuilt on load (in the background;
   measured above).
2. **Changing the mode** on a take with edits: the edits are discarded after a
   confirmation.
3. **`takeBackground`**: on for every edit.
4. **Release**: production feature, no experimental flag; M2–M5 on a branch.
5. **Look**: the vocal editor's look; features decided one by one in the
   [feature review](#feature-review).
