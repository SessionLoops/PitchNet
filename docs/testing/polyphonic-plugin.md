# Polyphonic takes in the plug-in (M5)

Manual DAW checks before `polyphonic` is merged into master
(docs/polyphonic-integration-plan.md, milestone 5). Run the ARA checks in every
ARA host that is supported (REAPER, Logic, Studio One, Cubase), the capture
checks in a host without ARA (Ableton Live, FL Studio), and
[track-analysis-queue.md](track-analysis-queue.md) once more with polyphonic
clips. Test material: `tools/polyphonic/test-audio` (piano triads, harp,
woodwind quartet) and a vocal take.

## ARA: one clip

1. A stereo piano clip: Input shows Instrument, polyphonic notes are drawn and
   the waveform behind them starts where the sound does.
2. Move, transpose, stretch, mute and level-change notes: playback follows each
   edit after release and stays stereo. Undo back to no edits: the clip plays
   its original audio again.
3. Edit, save, close and reopen the host project: the edits are there at once,
   the edited audio plays, and an edit made straight away renders (after the
   background rebuild).
4. Bounce/render the track in the host: the bounce matches playback.

## ARA: one mode per track

5. A new track with piano and vocal clips, mostly piano: every clip is analysed
   as polyphonic. Mostly vocal: every clip as vocal.
6. Drop a vocal clip on a polyphonic track (and the reverse): it follows the
   track's mode.
7. Change Input on a track with edits: the confirmation appears; Re-analyse
   analyses every clip of the track again and discards their edits; Cancel
   leaves everything as it was. Without edits there is no question.
8. Save and reopen, then add a clip to that track: it follows the track's mode.

## ARA: Track mode and memory

9. Track mode with several polyphonic clips: the inactive clips draw their
   notes (with edits); a pinned polyphonic track draws outlines in its colour;
   the overview shows the notes.
10. Edit clip A, switch to B, back to A: A's notes and edits are there at once,
    its edited audio plays, an edit right away renders.
11. Undo an edit made on a clip that is no longer selected: the clip is selected
    and the undo plays.
12. A session with many polyphonic clips: memory stays bounded while switching
    between them (only the shown clip keeps its analysis).

## Non-ARA capture

13. Record a piano take: it is analysed as polyphonic and plays in stereo.
14. Record a second take later in the song, then one over part of an edited
    take: earlier notes and edits stay, only the covered notes are replaced.
15. Edit right after a recording: the edit renders once the background rebuild
    lands.
16. Save, close and reopen: notes, edits and the edited audio are there (mono
    after a reload, a known limitation).

## Vocal unchanged

17. A vocal session saved before M5 opens, plays and edits as before, in ARA and
    in capture mode; the pitch macro button is there for vocal takes and absent
    for polyphonic ones.
