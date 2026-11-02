# Polyphonic editing test audio

Material for trying the Polyphonic Lab: in the standalone app, **Edit → Polyphonic
Lab (Experimental)... → Open Audio...** and pick a file from this folder.

Files 01–07 and 09 are mixed from single-note recordings, so every note in them is
known. File 08 is a real recording.

| File | What's in it | Try | What to expect |
|---|---|---|---|
| `01-piano-triad-C-major.wav` | Piano C4, E4 and G4 struck together | Drag E4 down one semitone (C major → C minor) | A clean result; compare it with 01b. The engine also lists a faint C5, which is really the C's second partial |
| `01b-piano-triad-C-minor-played.wav` | The same piano actually playing C minor | Reference for 01 | |
| `02-piano-arpeggio.wav` | Bass C3, then E4, G4 and C5 entering one after another | Raise G4 to A4 (+2) | The C4 that appears when the C5 enters is a ghost: the bass's second partial, which the C5's partials make look like a note |
| `03-strings-vibrato.wav` | Cello section on C4 and E4, violin section on G4 and C5, with vibrato | Lower E4 to E♭4 | The E can come in two pieces; Shift-click to select both. The violins' C5, an octave above the cellos' C4, is not found (it is among the rejected candidates) |
| `04-woodwind-quartet.wav` | Bassoon C3, clarinet E4, oboe G5, flute C6 | Clarinet E4 → E♭4, then Solo the clarinet | The oboe sits exactly on the bassoon's sixth partial and is not found; the faint C5 is the bassoon's fourth partial |
| `05-organ-chord.wav` | Sustained organ C4, E4 and G4, no attacks | E4 → E♭4 | A clean edit. A short, faint F6 is also listed; it comes from the organ's own partials, not a played note |
| `06-harp-rolled-chord.wav` | Harp C3, E3, G3, C5 and E5 plucked in turn | E3 → F3 | The weakest pitch edit: the top E5 claims E3's fourth partial, so a trace of the old note stays |
| `07-piano-octave-hard-case.wav` | Piano C3, C4 and G4 | C4 → D4 | Known failure: C4 shares every partial with C3, and G4 is not found |
| `08-brahms-hungarian-dance-5-strings.wav` | Brahms, Hungarian Dance No. 5, string orchestra, 46 s stereo | Raise some B♭s to B (G minor → G major), or move single melody notes | No reference exists; judge by ear. About 265 notes, 37 of them B♭ |
| `09-clarinet-short-note.wav` | Clarinet F5 for one second over a held cello G4 and violin-section D4 | Drag the F5's right end from 1.35 s to about 2.47 s (2.08× length) | The F5 then lasts twice as long from its start; compare it with 09b |
| `09b-clarinet-note-played-longer.wav` | The same clarinet note held for two seconds | Reference for 09 | |

For 01–07, edit scores measured against the same edit made on the isolated note
(spectral SNR; the unedited mix in brackets): triad 17.7 dB (5.5), arpeggio 19.2
(5.6), strings 24.7 (11.4), woodwinds 22.0 (8.1), organ 15.9 (5.7), harp 11.4
(9.0), octave 4.2 (3.5).

Muting or moving a note takes its pitched sound only: the hammer or pluck noise
of its attack stays where it was, which you can hear when you mute a piano or
harp note.

Drag a note's right end to make it longer or shorter. Its first 80 ms keep their
speed and only its pitched sound stretches. A note can only be lengthened up to
the end of the file, and in 01–07 the notes ring almost to the end, so try
lengthening on 09 (13.6 dB against 09b; the unedited 09 scores 7.7) or on
melody notes in 08, and shortening on any of them. It suits sustained notes
best: a lengthened piano note decays more slowly instead of ringing on.

Sources: piano notes from Salamander Grand Piano V3 by Alexander Holm (CC BY 3.0);
the other instruments from VSCO 2 Community Edition by Versilian Studios (CC0);
08 by the US Army Strings via Musopen (public domain). Git ignores the WAVs here
(see `.gitignore`); `../research/fetch_samples.sh` and `make_corpora.py` rebuild
the larger chord corpora from the original samples.
