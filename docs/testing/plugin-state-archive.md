# Non-ARA plug-in state archive

`tests/PluginStateArchiveTests.cpp` covers the v10 archive: the mel spectrogram
is omitted from self-contained archives and rebuilt from `originalWaveform` on
load (with and without a timeline offset), the streaming writer produces bytes
identical to the `MemoryBlock` writer and survives a patched length prefix, and
host-backed ARA archives stay sourceless with an empty mel for hydration to
fill. It links against `pitchnet_core` with the plugin's JUCE build flags, like
the ARA region tests.

Host verification:

1. Load PitchNet as an ordinary (non-ARA) plug-in, capture a few minutes of
   vocal, edit pitch, timing and volume, and let the render finish.
2. Save the DAW project. Compare the project file size against a pre-v10 save
   of the same take; the mel is roughly 8MB per three minutes of audio.
3. Save again without touching anything. The second save should be noticeably
   faster than the first - that is the state cache hitting.
4. Reopen the project with the editor closed. Playback must be the edited
   audio, not the raw capture, which requires the rebuilt mel.
5. Open the editor and confirm notes, curves, lyrics and plug-in parameters all
   came back, then make one edit and save again to confirm the cache
   invalidates on content change.
6. Load a project saved by 0.6.0 or earlier. Older archives still carry their
   mel and must load unchanged.
7. Repeat 1-5 in an ARA host to confirm the ARA path, which persists region
   audio separately, is unaffected.

Validation in this workspace: the archive regression above passed on Linux, and
both changed plugin translation units passed g++ syntax checking with ARA
enabled. A full Xcode build and the DAW checks remain manual verification.
