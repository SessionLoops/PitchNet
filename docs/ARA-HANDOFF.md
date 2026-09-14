# ARA refactor — session handoff

Working notes for continuing the ARA work on another machine. **Delete this file
before opening any PR upstream.** It is scratch context, not documentation.

Last updated: 2026-09-13, end of session.

---

## Where things stand

Branch: `refactor/ara-modification-owned-edits` (pushed to `fork`).

```
ac100a1 Store ARA projects in audio-modification time     <- the refactor
ab24417 WIP: modification-owned edit state, plus temporary render diagnostics
7de9fb0 Track audio modification lifetime
9667a12 Add PITCHNET_DEV_BUILD for a side-by-side dev plugin identity
5c79306 Silence the playback buffer when the processing lock is unavailable
28ff9ee Bridge editor-renderer region assignment to the render thread
b9d029f Restore ARA audio-thread safety in the render path   <- PR #4 starts here
a62cc93 update to version 0.5.7                              <- origin/master
```

This branch is **the good one**. It is usable: split, move, resize, save and
reload all work in Cubase and REAPER, and offline export matches what you hear.
It is just memory-inefficient (see "Shared analysis" below).

Other branches:

- `fix/ara-processing-lock` — the branch behind
  [PR #4](https://github.com/SessionLoops/PitchNet/pull/4). Contains `b9d029f`
  plus the two review-fix commits. Keep it clean: no dev-build commit, no
  refactor commits.
- `backup/refactor-pre-rebase` — the refactor branch as it stood before being
  rebased onto the fixed PR commits. Safety net; delete once you trust the
  rebase.

### PR #4 status

jie-chen requested changes on three points. All three were fixed, verified in
both Cubase and REAPER, and pushed. **Still to do: paste the three thread
replies and re-request review.** The reply text is in the session transcript; if
it is lost, re-derive it from the two commit messages, which say the same thing.

---

## The architecture, in one paragraph

ARA gives persistent identity to **audio sources** and **audio modifications**,
not to playback regions — `ARAPlaybackRegionProperties` has no `persistentID`
field (`third_party/ARA_SDK/ARA_API/ARAInterface.h:1012`). So all edit state is
keyed on the audio modification's persistent ID (`pitchnetRegionKey()` in
`Source/Plugin/ARADocumentController.cpp`). Projects are stored in **modification
time** (samples into the audio file, offset 0); timeline placement is applied
only at render and draw time. That is what makes splitting cheap and what fixed
the coordinate drift that used to compound on every edit.

---

## Verified host behaviour — do not re-derive these

**Cubase clones the `AudioModification` when you split an event.** Each half gets
its own modification with its own persistent ID, seeded from the original at the
moment of the split and diverging afterwards. The halves do **not** share one
modification. Proven from the diagnostic log (`createMod ... clonedFrom=<ptr>
cloneSrcId=<id>`), not inferred from the spec. WaveLab behaves identically.
VariAudio *does* share one edit layer across split events, but that is the Sample
Editor and it is **not available through the ARA extension interface** — do not
chase it. We concluded the opposite twice and burned several build cycles on it.

**REAPER has not been checked for clone-on-split.** The design should survive
either answer, because it keys on the modification rather than on regions: if
REAPER shares one modification, both halves simply share edits. But that is
reasoning, not a test result.

**The upstream devs test in REAPER, not Cubase.** That is why their code carries
REAPER-shaped special cases that look speculative from a Cubase seat — e.g. the
`previewRegionIsAssigned` branch that previews a region the host never assigned
to the renderer. Those are probably real observations. It also means a
Cubase-only pass is not verification as far as the reviewers are concerned.

---

## Open issues

### 1. Reset Pitch leaves a click on every note — diagnosed, not fixed

**Symptom:** use the Reset button next to Context Audition, choose Pitch. Blobs
return to their original positions and the pitch is right, but there is a soft
per-syllable "blip", like someone flicking the singer's cheek on every note.

**Cause, from the log:** after the reset, PitchNet emits `publish[edit] ...
samples=374850` with no `clear[edit]`. The region is still served from a
re-synthesised blob instead of falling back to the untouched ARA source. Two
things combine:

1. `PitchNetAudioProcessor::projectHasRegionEdits()`
   (`Source/Plugin/PluginProcessor.cpp:1683`) returns true if **any** note has
   `hasRenderedEdit()` — a sticky historical flag, not a comparison against the
   original. `IncrementalSynthesizer.cpp:1781` skips every note that is not a
   commit anchor, so resetting one note clears only that note's flag. Any other
   previously-edited note keeps the whole region on the resynthesis path.
2. The audible click is the **splice**: a reset note is re-synthesised "neutral"
   through the vocoder and spliced back into the composite. A neutral vocoder
   pass is not sample-identical to the original, so every splice boundary is a
   small discontinuity.

**Proposed fix (both parts):**

- Copy neutral notes from `originalWaveform` instead of re-synthesising them.
  The concept already exists as `isNeutralReset`
  (`Source/Audio/Synthesis/IncrementalSynthesizer.cpp:1784`); it is currently
  applied as bookkeeping *after* synthesis rather than routing the audio.
- Make `projectHasRegionEdits()` compare current state against original rather
  than trust the flag, so a fully-reset region drops its blob and plays the
  pristine source.

### 2. First edit on a freshly split right slice misbehaves — hypothesis only

**Symptom:** immediately after splitting, the first pitch edit on the right slice
appears not to take. Clicking away and back makes it work, but the audio then
sounds like it is correcting the *previous* correction rather than the original
pitch. Self-corrects shortly after, and survives save/reload correctly.

**Hypothesis:** both clones restore their inherited archive (log 1, lines
147/156: `restoreProject ... bytes=40533` for both halves) and both then render
`blob=374850 blobOffset=0` — i.e. each half starts out playing the inherited full
edited blob. If the right slice's Project is hydrated from an archive whose
`waveform` is already the edited composite, a new edit stacks on top of it.

**Not proven.** Before touching this, add a diagnostic line at hydration
comparing the hydrated `waveform` against `originalWaveform`. Confirm, then fix.

### 3. Shared analysis — planned, sized, not started

About **70% of each `Project` is a byte-identical copy** of analysis derived from
audio that cannot change. Verified by checking every write site: these five
fields are written only during analysis, hydration, and deserialization, never
mutated afterwards.

For a 4-minute mono take (T = 20,671 frames):

| Genuinely per-modification | Duplicated for no reason |
|---|---|
| `waveform` (the edited render) 42.3 MB | `originalWaveform` 42.3 MB |
| `f0`, `baseF0`, `basePitch`, `deltaPitch` 0.33 MB | `melSpectrogram` 10.6 MB |
| notes | `rawF0`, `cleanedF0`, `denseF0` 0.25 MB |
| **≈22 MB** | **≈53 MB** |

**Plan:** hold the immutable five behind a `std::shared_ptr<const>` keyed by the
ARA audio source. Every clone of a modification describes the same source, so
they all alias one copy. No copy-on-write needed — nothing writes. Twenty slices
of a 4-minute take goes from ~1.5 GB to ~490 MB, and scales the right way
afterwards (~22 MB per extra slice instead of ~75 MB).

Second-tier, probably not worth it: the clone constructor also copies the
rendered `processedRegions` blob, identical at the instant of a clone and only
diverging on the next edit. Copy-on-write would defer that, but it is real
per-modification data eventually, so the win is smaller and the complexity
higher.

### 4. Housekeeping before any upstream PR

- The render diagnostics (`ab24417`) are temporary. `AppLogger` reopens the file
  on every call and is **not realtime-safe**; render reporting only fires when a
  region's outcome *changes*, which keeps it survivable, but it should be
  trimmed or removed before this goes upstream.
- "Step B" from the original plan was never done: collapse the archive to one
  entry per modification persistent ID, bump to v5, keep the v4 reader. The
  archive is still region-keyed in places — search for
  `pitchnetRegionKeyForIndex`.
- Delete this file.

---

## Diagnostics

On in the `pitchnet-dev` and `pitchnet-dev-debug` presets, off everywhere else
(the raw option is `-DPITCHNET_ARA_DIAGNOSTICS=ON`). Output:

```
%APPDATA%\PitchNet\Logs\debug_<session-timestamp>.log
```

One file per DAW launch, newest wins. Lines are prefixed `[ARA]`. Routed through
`Source/Utils/AppLogger.h`, so model-graph and render events interleave
chronologically. Macro lives in `Source/Plugin/AraDiagnostics.h`.

Events: `createMod` (and whether it is a clone), `modProps` (when the persistent
ID lands), `regionAdd`, `regionProps`, `renderResources`, `render`, `publish`,
`clear`, `store`, `restore*`, `canvasRequest`, and `editBegin`/`editEnd`.

**Every ARA bug in this project was found by reading these logs**, usually from
one line with exact arithmetic — a blob growing by precisely `startInMod`
samples, for instance. Guessing from the code produced multiple wrong diagnoses,
each costing a DAW session. Read the log before theorising.

---

## Build

Steve builds and tests himself; do not run builds for him. A build costs real
time, so batch fixes rather than spending a build cycle per speculative change.

Build configurations live in `CMakePresets.json` at the repo root, so every
machine gets the same ones. Visual Studio reads presets natively — pick
**PitchNet Dev** from the configuration dropdown. From a shell, from the repo
root:

```
cmake --build --preset pitchnet-dev
```

Repo checkout paths differ between machines (`source\repos\PitchNet` at home,
`source\repos\StevenLeonCooper\PitchNet` on the laptop), which is why the command
above is relative. The build dir is always `out/build/<preset-name>`.

**Configure from an x64 environment.** These are Ninja presets, and Ninja takes
the target architecture from whatever `cl.exe` the environment provides. Visual
Studio sets that up before invoking Ninja, but a plain PowerShell prompt does
not — `cl.exe` then defaults to x86, and since every vendored ONNX Runtime is
x64-only, the build dies at link time on unresolved `_OrtGetApiBase@0`. Use the
VS configuration dropdown, an "x64 Native Tools Command Prompt", or
`Enter-VsDevShell -Arch amd64`. CMakeLists now hard-fails on a 32-bit toolchain
rather than letting it reach the linker. If it ever does trip, **delete the
build directory** — the wrong compiler is cached and switching shells alone will
not fix it.

Presets:

| Preset | Type | Identity | Diagnostics |
|---|---|---|---|
| `pitchnet-dev` | RelWithDebInfo | PitchNet Dev | on |
| `pitchnet-dev-debug` | Debug | PitchNet Dev | on |
| `pitchnet-release` | Release | PitchNet | off |

`pitchnet-dev` is the one to use. RelWithDebInfo is optimized and sets `NDEBUG`,
so `JUCE_DEBUG=0` and realtime behaviour in the DAW matches a release build —
but symbols survive, so the VS debugger can still attach to ARA callbacks. **Do
not diagnose realtime behaviour from `pitchnet-dev-debug`:** an unoptimized
vocoder will drop out on its own, and those dropouts look exactly like the bugs
being hunted.

`PITCHNET_DEV_BUILD=ON` produces "PitchNet Dev" with its own plugin code and
bundle id, so it loads alongside a release build. JUCE derives the ARA factory id
and document-archive id from the bundle id, which is what keeps the two genuinely
separate ARA plug-ins. Configure is only needed when `CMakeLists.txt` changes or
an option is flipped; otherwise the build command re-runs it.

Note on the old command: it named a `--config Release` that a single-config Ninja
build dir silently ignores. If the home build dir was Ninja, that build was
whatever `CMAKE_BUILD_TYPE` happened to be cached, not necessarily Release. The
presets remove the ambiguity.

Test from a fresh, unsaved project with a short clip — never a carried-over
session. That is why the logs have been clean enough to diagnose from.

---

## Working agreements

- **Confirm before executing commands.** Propose and wait; do not run then
  report. What gets said often changes whether the effort is worth it.
- **Edit scripts must buffer every write until all substitutions succeed.**
  Partial writes caused duplicate-symbol build failures twice when a script was
  re-run after a mid-way assert.
- **Do not commit code that has not compiled.**
- Verify statically before handing over: brace balance, duplicate symbols,
  exact-string matches.
- This is a passion project. Scope is not a constraint — a large, unmergeable
  change is acceptable if it is the right change.
