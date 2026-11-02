# Cubase track-version regression check

Use a build with ARA enabled and a vocal clip already analysed by PitchNet.

1. In version A, move a note and change its pitch curve. Wait for synthesis to
   finish. Record the note display and listen to the edited playback.
2. Duplicate A with Cubase Track Versions. Open PitchNet in version B. Verify
   that both edits and processed playback match A, with no pitch detection run.
3. Make a different edit in B. Switch to A and back to B several times. Verify
   that each version retains its own edits and playback without pitch detection.
4. Repeat duplication before explicitly saving the Cubase project, to check
   that cloning captures current canvas edits rather than an older archive.
5. Save with A active and B inactive. Close and reopen the Cubase project.
   Verify both versions retain their separate edits and processed playback.
6. Repeat with PitchNet's editor closed during duplication and version switching.
7. Repeat with two unrelated clips, editing only one. The other clip must not
   acquire the first clip's notes or processed audio.

Restoring archived state may read source samples and rebuild waveforms/mel data;
it must reuse the stored pitch analysis and edits instead of running pitch
inference again. These checks require Cubase and are not automated by the build.
