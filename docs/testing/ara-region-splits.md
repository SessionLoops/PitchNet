# ARA region split regression

`tests/AraRegionSliceTests.cpp` exercises splitting through a time-edited note,
curve partitioning, metadata preservation, analysis-cache preservation, rendered
waveform boundaries, and independence from the original project. It can be linked
against the existing `pitchnet_core` library with the plugin's JUCE build flags.

Host verification:

1. Analyze an ARA region. Change note pitch, contour, timing, volume and lyrics;
   wait for the render to finish.
2. Split through an edited note in the DAW. Select both halves. Neither should
   show analysis progress; notes must stop at the split, and the crossing note's
   right portion must retain its edits and text.
3. Play across the split and compare with playback before splitting.
4. Split the right half again. Repeat with a cut between notes and a nonzero
   timeline start; also test a split while a render is pending.
5. Save/reopen the DAW project and select both halves. Check edited playback and
   note data. Test with the editor closed when splitting.
6. Confirm ordinary region moves and unrelated new regions still work.

The controller snapshots before host editing and recognizes contiguous partitions
with the same source-to-playback mapping after editing. This supports hosts that
resize first, create first, or replace the original playback region. Distinct
regions sharing a source are not donors unless they form a complete partition.
PitchNet's local per-region undo history is cleared when replacing the unsplit
project because its actions retain pointers into the old note data; DAW region
undo remains host-owned.

Validation in this workspace: the executable slice regression passed, and both
changed plugin translation units passed Clang syntax checking. A full Xcode build
was blocked by a module-cache session-file write error before compilation.
DAW callback, playback, and save/reopen checks above remain manual verification.

## Gluing regions

`tests/AraRegionMergeTests.cpp` covers two independently edited regions, absolute
frame/audio placement, per-note metadata and pitch/gain differences, stale-note
exclusion, archive round-trip, splitting after glue, and source-less hydration.
It also rejects gaps, overlaps, and a single donor.

In the DAW, glue two adjacent edited regions and verify that neither region is
reanalyzed, both sets of note/curve/text edits survive, and playback is unchanged.
Repeat after splitting one region, after changing each half independently, with
the editor closed, and after saving/reopening. Test both a host that enlarges an
existing event and one that creates a bounced source. Adding a longer overlapping
event without consuming the original events must not inherit their state.

Glue inherits the left region's project-level controls and retains both rendered
audio portions. Different global pitch and gain values are carried into the
corresponding notes. Like splitting, glue resets the local note undo history.

## Reopening after an editor-closed split

1. Edit a region, select it, and close the plugin editor.
2. Split it in the DAW, keeping the original object as the left part.
3. Reopen on the left part first. Verify its clipped notes/edits and playback,
   and confirm no pitch detection or segmentation starts.
4. Select the right part and repeat. Repeat after saving/reopening the project
   and with ARA sample access initially disabled.

A successful live-key restore may move the project directly into the canvas;
its empty map slot must not trigger loading the older index archive. Split/glue
now refresh both identities, and installing preserved state retires any pending
analysis for that region even without an editor. The slice regression checks
host-backed archive round-trips for both halves; the callback/selection sequence
above still requires verification in the DAW.
