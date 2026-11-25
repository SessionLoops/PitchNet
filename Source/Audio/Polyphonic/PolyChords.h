#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

/**
 * Chord recognition for a polyphonic take, from its notes.
 *
 * The engine already knows every note's pitch and its level in every frame,
 * so the chords are read from the notes rather than from a chromagram of the
 * audio: each slice of time (a beat when there is a beat grid, a quarter of a
 * second otherwise) gets a pitch-class profile from the notes sounding in it,
 * weighted by their level, and a bass profile from the lowest of them. Each
 * slice is scored against chord templates (triads, sevenths, sixths,
 * suspensions, power chords) by cosine similarity, with a small prior per
 * chord quality and a term for the bass, and a Viterbi pass with a cost per
 * chord change picks the sequence (template matching with HMM smoothing, as in
 * Fujishima 1999 and Bello & Pickens 2005; on symbolic notes, cf. Pardo &
 * Birmingham 2002). A span whose bass is another chord tone is an inversion
 * (C/E). Note names are spelled with flats in flat keys (the key from the
 * Krumhansl-Kessler profiles over the whole take).
 *
 * Free of JUCE so it can be unit tested on its own (tests/PolyChordTests.cpp).
 */
namespace polyphonic
{
struct Analysis;
struct NoteEdit;

/** A note as chord recognition sees it. */
struct ChordNote
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float midi = 0.0f;
    // Optional level per step from startSeconds (one value per analysis frame);
    // empty: a constant level of 1 over the note.
    std::vector<float> levels;
    double levelStepSeconds = 0.0;
    double gain = 1.0; // linear, from the note's gain edit
};

/** A chord symbol: root, bass and the intervals above the root. */
struct Chord
{
    int root = -1;           // pitch class, C = 0; -1: no chord
    int bass = -1;           // pitch class of the bass; the root unless an inversion
    std::uint16_t intervals = 0; // bit i: the pitch class i semitones above the root

    bool isNone() const { return root < 0 || intervals == 0; }
    bool operator==(const Chord& o) const
    {
        return root == o.root && bass == o.bass && intervals == o.intervals;
    }
    bool operator!=(const Chord& o) const { return !(*this == o); }
};

/** A chord from startSeconds to endSeconds, with its name for display. */
struct ChordSpan
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    Chord chord;
    std::string name; // "C", "F#m7", "Bb/D"; empty for no chord

    bool operator==(const ChordSpan& o) const
    {
        return startSeconds == o.startSeconds && endSeconds == o.endSeconds && chord == o.chord
            && name == o.name;
    }
    bool operator!=(const ChordSpan& o) const { return !(*this == o); }
};

struct ChordOptions
{
    // A beat grid (seconds per beat and where a beat falls); chords then change
    // on beats. 0: no grid, chords change on the notes' onsets.
    double beatSeconds = 0.0;
    double gridOriginSeconds = 0.0;
    double sliceSeconds = 0.25; // without a grid
    double changeCost = 0.18;   // Viterbi cost of a chord change, in score units
};

/** The take's chords, in time order; spans without a chord are left out. */
std::vector<ChordSpan> detectChords(const std::vector<ChordNote>& notes,
                                    const ChordOptions& options = {});

/**
 * The chord each span between neighbouring `boundaries` (ascending, seconds)
 * plays, read from all its notes at once, without segmenting it further: the
 * chord track once the user has split or merged its chords. One span per pair
 * of neighbours, in order; a span that plays no chord has none (empty name).
 */
std::vector<ChordSpan> chordsOver(const std::vector<ChordNote>& notes, const std::vector<double>& boundaries);

/**
 * The take's active notes as chord recognition sees them, with their edits:
 * muted notes are left out, a re-pitched note counts at its new pitch, a moved
 * one at its new time, a stretched one over its new length, a re-gained one at
 * its new level. `levels` are the notes' levels per frame from their first
 * frame (PolyphonicTake.h noteLevels); null or empty: constant levels.
 */
std::vector<ChordNote> chordNotes(const Analysis& analysis,
                                  const std::vector<std::vector<float>>* levels,
                                  const std::map<int, NoteEdit>* edits);

// ---- retuning to another chord ----

/** No motion: the pitch class is not a tone of the original chord. */
constexpr int kNotAChordTone = 1000;

/**
 * How each pitch class moves (in semitones, -6..6) when a `from` chord becomes
 * `to`: the original chord's tones go to the new chord's by the smallest total
 * motion that still sounds every tone of the new chord it can (the root and
 * third count most), so C -> Am moves G up to A and keeps C and E, and C -> F
 * moves E to F and G to A. Pitch classes outside `from` get kNotAChordTone.
 * `missing`, when given, gets the intervals of `to` (above its root) that no
 * tone of `from` reaches: C -> C7 has no note to become Bb.
 */
std::array<int, 12> chordToneMotion(const Chord& from, const Chord& to, std::uint16_t* missing = nullptr);

struct ChordRetune
{
    std::map<int, NoteEdit> edits; // the take's whole edit map, retuned
    int movedNotes = 0;
    std::uint16_t missing = 0;     // intervals of the new chord no note reaches
};

/**
 * Retunes a span of the take from one chord to another (the chord track's
 * edit): every note that starts in [startSeconds - tolerance, endSeconds -
 * tolerance) and is a tone of `from` moves by chordToneMotion (a pitch edit,
 * added to the one it has); other notes - passing notes, a melody's notes
 * outside the chord - and muted notes are left alone. No note is added. With
 * an explicit bass (`to.bass` >= 0) the span's lowest chord tones move to it
 * instead; with `to.bass` < 0 the bass moves like the other tones (C/E -> Cm
 * gives Cm/Eb). Times are the notes' edited times.
 */
ChordRetune retuneToChord(const Analysis& analysis, const std::map<int, NoteEdit>& edits,
                          double startSeconds, double endSeconds, const Chord& from, const Chord& to,
                          double toleranceSeconds = 0.05);

// ---- names ----

/** "C", "C#" / "Db", ... */
std::string pitchClassName(int pitchClass, bool preferFlats);
/** A note spelled by its place on the circle of fifths (C = 0, G = 1, F = -1), as ARA gives it. */
std::string circleOfFifthsName(int index);
/** The pitch class at a place on the circle of fifths. */
int circleOfFifthsPitchClass(int index);
/**
 * A typed chord symbol: a root (C, F#, Bb), then the usual quality and
 * extension notation (m, min, -, maj7, M7, dim, o, aug, +, m7b5, sus2, sus4, 5,
 * 6, 6/9, 7, 9, 11, 13, add9, b9, #9, #11, b13, alt, no3, no5, with or without
 * brackets; the flat, sharp, degree, o-slash and triangle signs too), and an
 * optional slash bass (C/E). Bass is the root unless given. nullopt when it
 * cannot be read.
 */
std::optional<Chord> parseChord(const std::string& text);
/** The chord's quality as written after its root: "", "m", "7", "maj7", "m7b5", "sus4", ... */
std::string chordSuffix(std::uint16_t intervals);
/** Root, suffix and, for an inversion, "/bass". */
std::string chordName(const Chord& chord, bool preferFlats);
/** Whether the notes' key is written with flats (F, Bb, Eb, ... major; D, G, C, ... minor). */
bool keyPrefersFlats(const std::vector<ChordNote>& notes);
} // namespace polyphonic
