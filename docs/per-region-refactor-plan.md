# PitchNet per-region (per-track) refactor plan

Goal: match VocalNet's model where **each ARA playback region is an independent
analysed unit**, instead of PitchNet's current design that merges every region
into one composite `Project`. Clicking a region switches the canvas to that
region; every region plays back processed simultaneously; each region keeps its
own analysis/edits across save/reload.

Decisions locked with the user:
- **Selection UX:** native Pro Tools selection now (Stage 1), in-plugin
  VocalNet-style timeline later (Stage 2 — "Both").
- **Per-region state:** a persistent `Project` per region, retained across
  switches and reload.

---

## 1. Why it's broken today

- `PitchNetDocumentController::processDocument` (ARADocumentController.cpp ~L796)
  renders **all** regions from **all** sources into a single composite buffer and
  analyses it as one timeline → one `Project`.
- `MainComponent` owns exactly one `Project` (`editorController->getProject()`).
- The processor holds a single analysis slot: `araAnalysisProjectSnapshot`,
  `araAnalysisProjectJson`, `araAnalysisSourceKey`, `araAnalysisReady`,
  `araPlaybackRegionRanges`, `araAnalysisTimelineOffsetSeconds`.
- There is no per-region identity in the UI and no selection→canvas switch, so
  multiple regions can't be told apart, and after reload the code latches onto
  whichever single region the composite/first-region logic picks.

VocalNet, by contrast: `convertARARegion(region)` processes one region, stored as
per-region `ConvertedRegionData` on the region's modification keyed by
`getPlaybackRegionRuntimeKey(region)`; the renderer iterates **all** regions and
mixes each region's own processed audio; the timeline draws one clip per region
and selection chooses which region is edited.

---

## 2. Target architecture

### 2.1 Stable per-region key
Add a single helper (mirrors VocalNet's intent, but reload-stable):

```cpp
// key = <audio-modification persistentID> ":" <region index in modification>
juce::String pitchnetRegionKey(const juce::ARAPlaybackRegion& region);
```

Region host refs are reassigned every session, so the key must be built from the
modification's ARA persistent ID + the region's index. (Same reasoning as the
Bug 2 fix already applied to `arrangementKey`.)

### 2.2 Per-region Project store (processor)
Replace the single analysis slot on `PitchNetAudioProcessor` with a map:

```cpp
struct AraRegionEntry {
    std::unique_ptr<Project> project;   // persistent per-region analysis/edits
    juce::String            projectJson;
    double                  timelineOffsetSeconds = 0.0;
    std::pair<double,double> playbackRange {0.0, 0.0};
    bool                    ready = false;
    bool                    loading = false;
};
std::map<juce::String /*regionKey*/, AraRegionEntry> araRegions;
juce::String activeRegionKey;   // which region the canvas currently shows
```

Keep the existing single-slot fields as thin accessors over
`araRegions[activeRegionKey]` during migration to limit churn, then remove them.

### 2.3 Per-region analysis
Rework `processDocument` so that instead of compositing, it iterates regions and,
for each region without a ready/cached entry, kicks off analysis **for that one
region** (read just that region's audio range, like VocalNet's
`readPlaybackRangeIntoBuffer` / the existing per-region extract in `run()`).
Results populate `araRegions[key]`. The `attachCachedAnalysisCallback` /
`requestAnalysisCallback` pair becomes per-region (add the region key to the
signature, or resolve the key inside the processor).

Reuse the existing analysis worker; just scope its input buffer to a single
region and tag the result with the region key instead of `arrangementKey`.

### 2.4 Selection → canvas switch (Stage 1, native)
`onNewSelection` (already added in PluginEditor.cpp for Bug 1) also carries the
selected region:

```cpp
auto regions = viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>();
if (!regions.empty())
    audioProcessor.setActiveAraRegion(regions.front());
```

`setActiveAraRegion(region)`:
1. compute `key = pitchnetRegionKey(*region)`;
2. if `araRegions[key]` isn't ready, ensure analysis is requested for it;
3. set `activeRegionKey = key`;
4. point the UI at that region's project on the message thread:
   `mainComponent->setActiveProject(entry.project.get())` →
   `pianoRoll.setProject(...)`, `pianoRollView.setProject(...)`,
   `parameterPanel.setProject(...)`, `toolbar.setTotalTime(...)`, repaint.

`MainComponent` gains `setActiveProject(Project*)` (a thin re-point of the views;
distinct from the owning `editorController->setProject`). The per-region projects
are owned by the processor so they outlive editor open/close.

### 2.5 Playback (all regions processed at once)
This is the deepest change. PitchNet's renderer currently plays one merged
project via `realtimeProcessor`. To match VocalNet, playback must be per-region:

Option A (recommended, mirrors VocalNet): after a region is analysed, render its
**processed audio** once and store it per region (a `ConvertedRegionData`-style
buffer keyed by region). `PitchNetPlaybackRenderer::processBlock` iterates
`getPlaybackRegions()`, and for each region copies/mixes its processed buffer at
the region's timeline position — exactly like VocalNet's `processBlock`. Editing
the selected region re-renders only that region's buffer.

Option B (interim): keep realtime synthesis but drive it from the active region
only. Simpler, but non-selected regions won't play processed — not DAW-correct.
Use only as a stepping stone.

### 2.6 Persistence (per region)
Follow VocalNet's `doStoreObjectsToStream` / `doRestoreObjectsFromStream`:
- store one archive entry per region, keyed by `(modification persistentID,
  region index)`, containing that region's project JSON (and, if Option A,
  its processed audio);
- on restore, re-key to the current region and populate `araRegions[key]` with
  `ready = true`, so no re-analysis is needed after reload.
This subsumes the earlier single-blob persistence and makes reload deterministic.

### 2.7 Sample-access + reload
Keep the `didEnableAudioSourceSamplesAccess` handler already added, but have it
re-run per-region analysis only for regions of the enabled source that aren't
ready yet.

---

## 3. Stage 2 — in-plugin timeline (VocalNet DocumentView/TimelineView)
Port `DocumentView.*` and `TimelineView.*` into PitchNet's editor as a region
strip. Wire `ARADocumentView::setSelectionChangedCallback` → `setActiveAraRegion`
(same entry point as native selection). The timeline's `timeline::Model` reads
the per-region store for clip names/waveforms/selection. Needs `PlayHeadState`
(VocalNet's shared playhead) or an adapter over PitchNet's existing cursor state.

---

## 4. File-by-file touch list
- `Source/Plugin/ARADocumentController.h/.cpp`
  - add `pitchnetRegionKey`; rework `processDocument` to per-region; per-region
    attach/request callbacks; per-region `doStore/doRestoreObjectsToStream`;
    scope `didEnableAudioSourceSamplesAccess` to not-ready regions.
- `Source/Plugin/PluginProcessor.h/.cpp`
  - `araRegions` map + `activeRegionKey`; `setActiveAraRegion`,
    `attachCachedAraAnalysis`/`requestAraSourceAnalysis` keyed by region;
    per-region persistence; (Option A) per-region processed-audio store +
    renderer mixing.
- `Source/Plugin/PluginEditor.cpp`
  - extend `onNewSelection` to route selected region → `setActiveAraRegion`.
- `Source/UI/MainComponent.h/.cpp`
  - `setActiveProject(Project*)` to re-point views without taking ownership.
- Stage 2: add `Source/UI/DocumentView.*`, `Source/UI/TimelineView.*` (ported),
  editor layout to host the timeline, selection callback wiring.

---

## 5. Verification (no local compile — macOS/Xcode only)
1. Build in Xcode (AAX + VST3/AU).
2. Two audio regions on one track, one on another: each analyses independently;
   clicking each switches the canvas; both play back processed together.
3. Edit region A, switch to B, switch back: A's edits retained (persistent
   per-region project).
4. Save session, reopen: all regions restore ready without re-analysis; selection
   still switches correctly.
5. Remove + re-insert plugin: regions re-analyse via sample-access handler.
6. Stopped-transport playhead still follows (Bug 1) and reload recognises regions
   (earlier fixes) — regression check.

---

## 6. Suggested order
1. `pitchnetRegionKey` + `araRegions` store + `setActiveAraRegion` + MainComponent
   `setActiveProject` + extend `onNewSelection` (native switch skeleton).
2. Per-region analysis in `processDocument` (+ keyed callbacks).
3. Per-region persistence (store/restore).
4. Per-region playback (Option A).
5. Stage 2 timeline port.

---

## 7. FINALISED DIRECTION (user-confirmed)
- **Primary view = the region timeline** (VocalNet's `DocumentView`/`TimelineView`),
  with the piano-roll shown for the *selected* region. Replaces the current
  single-canvas main view in ARA mode.
- **Clips drawn from per-region PROCESSED audio**, stored on a custom ARA audio
  modification (VocalNet's `ConvertedRegionData` model). This is the data model
  the whole thing hangs on.
- **Selection response = VocalNet's exact pattern**: `onNewSelection` re-reads
  `getViewSelection()`, resolves the front playback region, updates the model,
  fires `selectionChangedCallback(region)`, and reveals the clip. No canvas swap,
  no re-analyse-on-select. The timeline always shows all regions; the callback
  drives which region the piano-roll edits.

### Ordered build (each stage compile-verified before the next)
- **A. Per-region data model (foundation).** Add a custom
  `PitchNetAudioModification : juce::ARAAudioModification` holding per-region
  processed audio + `AudioThumbnail`, keyed by `pitchnetRegionKey`. Override
  `doCreateAudioModification` to return it. (New file
  `Source/Plugin/PitchNetAudioModification.h`.)
- **B. Per-region rendering.** After a region's Project is analysed, render its
  processed audio (`EditorController::renderProcessedAudioAsync`) and store it in
  the modification (`setProcessedAudioForRegion`). Update the thumbnail.
- **C. Per-region playback.** Rewrite `PitchNetPlaybackRenderer::processBlock`
  to iterate `getPlaybackRegions()` and mix each region's stored processed audio
  at its timeline position (VocalNet's `processBlock`). Retire the composite
  realtime path. Works headless (data on the model, not the view).
- **D. Per-region persistence.** Store/restore each region's processed audio in
  `doStore/doRestoreObjectsToStream`, keyed by modification persistent ID +
  region index (VocalNet's scheme). Deterministic reload, no re-analysis.
- **E. Timeline port.** Bring `DocumentView`/`TimelineView` into PitchNet; back
  `timeline::Model` with the per-region modifications (clip name/colour/waveform
  from the store). Provide a `PlayHeadState` adapter over PitchNet's cursor.
- **F. Editor relayout + selection wiring.** In ARA mode make the timeline the
  main view; `selectionChangedCallback(region) -> setActiveAraRegion(region)`
  drives the piano-roll for the selected region; host `onNewSelection` is a
  secondary sync into the same model.

Stages A–D are non-UI and can be validated with the existing UI. E–F are the UI
swap. The current bolt-on per-region-canvas-analysis (Stage 2a) is superseded by
B/C and will be removed once B lands.
