// Chord recognition from polyphonic notes (PolyChords.h). Part one feeds notes
// directly; part two renders a progression and runs it through the engine, so
// the notes are the ones the editor would see.

#include "../Source/Audio/Polyphonic/PolyChords.h"
#include "../Source/Audio/Polyphonic/PolyEngine.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 44100;

using polyphonic::ChordNote;
using polyphonic::ChordSpan;

void addChord(std::vector<ChordNote>& out, std::initializer_list<int> midi, double start, double end)
{
    for (int m : midi)
        out.push_back({start, end, static_cast<float>(m), {}, 0.0, 1.0});
}

void print(const char* label, const std::vector<ChordSpan>& spans)
{
    std::printf("%s:", label);
    for (const auto& s : spans)
        std::printf(" [%.2f-%.2f %s]", s.startSeconds, s.endSeconds, s.name.c_str());
    std::printf("\n");
}

std::vector<std::string> names(const std::vector<ChordSpan>& spans)
{
    std::vector<std::string> out;
    for (const auto& s : spans)
        out.push_back(s.name);
    return out;
}

// The chord sounding at `t`, or "" for none.
std::string at(const std::vector<ChordSpan>& spans, double t)
{
    for (const auto& s : spans)
        if (t >= s.startSeconds && t < s.endSeconds)
            return s.name;
    return {};
}

void addTone(std::vector<float>& x, double f0, double start, double dur, double gain)
{
    const int s0 = static_cast<int>(start * kRate), n = static_cast<int>(dur * kRate);
    for (int h = 1; h <= 10; ++h)
    {
        const double fh = h * f0;
        if (fh > 12000.0)
            break;
        const double a = gain / h;
        for (int i = 0; i < n && s0 + i < static_cast<int>(x.size()); ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            const double env = std::min(1.0, t / 0.004) * std::exp(-t * (1.2 + 0.3 * h))
                * std::min(1.0, (dur - t) / 0.02);
            x[static_cast<size_t>(s0 + i)] += static_cast<float>(a * env * std::sin(2.0 * kPi * fh * t));
        }
    }
}

double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

// Each note's level per frame from its masks, as the editor computes it
// (PolyphonicTake.h noteLevelsFromMasks, which needs JUCE).
std::vector<std::vector<float>> levelsFromMasks(const polyphonic::Analysis& analysis)
{
    std::vector<std::vector<float>> levels(analysis.notes.size());
    for (size_t i = 0; i < analysis.notes.size() && i < analysis.masks.size(); ++i)
    {
        const auto& note = analysis.notes[i];
        std::vector<double> energy(static_cast<size_t>(std::max(1, note.numFrames())), 0.0);
        for (const auto& p : analysis.masks[i].partials)
        {
            const int k = p.frame - note.startFrame;
            if (k >= 0 && k < static_cast<int>(energy.size()))
                energy[static_cast<size_t>(k)] += static_cast<double>(p.amplitude) * p.amplitude;
        }
        for (double e : energy)
            levels[i].push_back(static_cast<float>(std::sqrt(e)));
    }
    return levels;
}
} // namespace

int main()
{
    // ---- 0. names ----
    {
        CHECK(polyphonic::circleOfFifthsName(0) == "C");
        CHECK(polyphonic::circleOfFifthsName(1) == "G");
        CHECK(polyphonic::circleOfFifthsName(-1) == "F");
        CHECK(polyphonic::circleOfFifthsName(-2) == "Bb");
        CHECK(polyphonic::circleOfFifthsName(6) == "F#");
        CHECK(polyphonic::circleOfFifthsName(-6) == "Gb");
        CHECK(polyphonic::circleOfFifthsName(7) == "C#");
        CHECK(polyphonic::circleOfFifthsName(-8) == "Fb");
        CHECK(polyphonic::circleOfFifthsName(12) == "B#");
        CHECK(polyphonic::circleOfFifthsPitchClass(0) == 0);
        CHECK(polyphonic::circleOfFifthsPitchClass(-1) == 5);
        CHECK(polyphonic::circleOfFifthsPitchClass(4) == 4);
        CHECK(polyphonic::circleOfFifthsPitchClass(-2) == 10);

        polyphonic::Chord c;
        c.root = 0;
        c.bass = 4;
        c.intervals = (1u << 0) | (1u << 4) | (1u << 7);
        CHECK(polyphonic::chordName(c, false) == "C/E");
        c.root = 10;
        c.bass = 10;
        c.intervals = (1u << 0) | (1u << 3) | (1u << 7) | (1u << 10);
        CHECK(polyphonic::chordName(c, true) == "Bbm7");
        CHECK(polyphonic::chordName(c, false) == "A#m7");
        CHECK(polyphonic::chordSuffix((1u << 0) | (1u << 3) | (1u << 6) | (1u << 10)) == "m7b5");
        CHECK(polyphonic::chordSuffix((1u << 0) | (1u << 4) | (1u << 7) | (1u << 10) | (1u << 9) | (1u << 2)) == "13");
        // not in the table: a major seventh with a sharp eleventh
        CHECK(polyphonic::chordSuffix((1u << 0) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 11)) == "maj7#11");
        CHECK(polyphonic::chordName(polyphonic::Chord{}, false).empty());
    }

    // ---- 0b. typed chords: read, then named back ----
    {
        struct Case
        {
            const char* typed;
            const char* name; // as chordName writes it (sharps)
        };
        const Case cases[] = {
            {"C", "C"}, {"Cm", "Cm"}, {"cmin", "Cm"}, {"C-", "Cm"}, {"Cmi7", "Cm7"},
            {"C7", "C7"}, {"Cmaj7", "Cmaj7"}, {"CM7", "Cmaj7"}, {"C\xCE\x94" "7", "Cmaj7"},
            {"C\xCE\x94", "Cmaj7"}, {"Cma9", "Cmaj9"}, {"CM", "C"}, {"Cmaj", "C"},
            {"Cm7b5", "Cm7b5"}, {"C\xC3\xB8", "Cm7b5"}, {"C\xC3\xB8" "7", "Cm7b5"}, {"Cm7-5", "Cm7b5"},
            {"Cdim", "Cdim"}, {"Co", "Cdim"}, {"Cdim7", "Cdim7"}, {"C\xC2\xB0" "7", "Cdim7"},
            {"Caug", "Caug"}, {"C+", "Caug"}, {"C+7", "C7#5"}, {"Caug7", "C7#5"}, {"C7#5", "C7#5"},
            {"C7+5", "C7#5"}, {"Cmaj7#5", "Cmaj7#5"},
            {"Csus", "Csus4"}, {"Csus4", "Csus4"}, {"Csus2", "Csus2"}, {"C4", "Csus4"}, {"C7sus4", "C7sus4"},
            {"C9sus4", "C9sus4"}, {"C5", "C5"}, {"C6", "C6"}, {"Cm6", "Cm6"}, {"C6/9", "C6/9"},
            {"C69", "C6/9"}, {"Cm6/9", "Cm6/9"}, {"Cadd9", "Cadd9"}, {"C2", "Cadd9"}, {"Cadd2", "Cadd9"},
            {"Cmadd9", "Cmadd9"}, {"Cm(add9)", "Cmadd9"}, {"Cadd11", "Cadd11"},
            {"C9", "C9"}, {"Cm9", "Cm9"}, {"Cmaj9", "Cmaj9"}, {"C11", "C11"}, {"Cm11", "Cm11"},
            {"C13", "C13"}, {"Cm13", "Cm13"}, {"Cmaj13", "Cmaj13"},
            {"CmMaj7", "CmMaj7"}, {"Cm(maj7)", "CmMaj7"}, {"CmM7", "CmMaj7"}, {"C-maj7", "CmMaj7"},
            {"CmMaj9", "CmMaj9"},
            {"C7b9", "C7b9"}, {"C7#9", "C7#9"}, {"C7(#9)", "C7#9"}, {"C7#11", "C7#11"}, {"C7b13", "C7b13"},
            {"C7b5", "C7b5"}, {"C7b9b13", "C7b9b13"}, {"C13#11", "C13#11"}, {"Cmaj9#11", "Cmaj9#11"},
            {"C7b9#9", "C7b9#9"}, {"C7alt", "C7#5b9#9"}, {"Cmaj7#11", "Cmaj7#11"},
            {"C7no3", "C7(no3)"}, {"Cno5", "C(no5)"}, {"Cm7(no5)", "Cm7(no5)"},
            {"F#m7", "F#m7"}, {"Bbmaj7", "A#maj7"}, {"B\xE2\x99\xAD" "7", "A#7"}, {"Ebm", "D#m"},
            {"Cb", "B"}, {"E#", "F"}, {"am", "Am"},
            {"C/E", "C/E"}, {"Cm7/Bb", "Cm7/A#"}, {"G7/B", "G7/B"}, {"C6/9/E", "C6/9/E"}, {"C/C", "C"},
            {" D m 7 ", "Dm7"},
        };
        for (const auto& c : cases)
        {
            const auto chord = polyphonic::parseChord(c.typed);
            const std::string got = chord ? polyphonic::chordName(*chord, false) : "(unread)";
            if (got != c.name)
                std::printf("typed %s -> %s, wanted %s\n", c.typed, got.c_str(), c.name);
            CHECK(got == c.name);
            // what a name says, it reads back as
            CHECK(polyphonic::parseChord(got) == chord);
        }
        for (const char* bad : {"", "H", "X7", "C8", "C7 9", "Cfoo", "C/", "C/H", "N.C.", "C#q", "Cnox"})
        {
            if (polyphonic::parseChord(bad))
                std::printf("read %s, wanted no chord\n", bad);
            CHECK(!polyphonic::parseChord(bad));
        }
        // the interval sets themselves
        CHECK(polyphonic::parseChord("C13")->intervals == ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 7) | (1u << 9) | (1u << 10)));
        CHECK(polyphonic::parseChord("Bb/D")->root == 10 && polyphonic::parseChord("Bb/D")->bass == 2);
    }

    // ---- 1. a I-vi-IV-V7 progression on a beat grid ----
    {
        const double beat = 0.6, bar = 4 * beat, origin = 0.3;
        std::vector<ChordNote> notes;
        for (int rep = 0; rep < 2; ++rep)
        {
            const double t = origin + rep * 4 * bar;
            addChord(notes, {36, 60, 64, 67}, t, t + bar);
            addChord(notes, {45, 60, 64, 69}, t + bar, t + 2 * bar);
            addChord(notes, {41, 60, 65, 69}, t + 2 * bar, t + 3 * bar);
            addChord(notes, {43, 59, 62, 65, 67}, t + 3 * bar, t + 4 * bar);
        }
        polyphonic::ChordOptions options;
        options.beatSeconds = beat;
        options.gridOriginSeconds = origin;
        const auto spans = polyphonic::detectChords(notes, options);
        print("grid", spans);
        const std::vector<std::string> expected = {"C", "Am", "F", "G7", "C", "Am", "F", "G7"};
        CHECK(names(spans) == expected);
        for (size_t i = 0; i < spans.size(); ++i)
        {
            CHECK(std::abs(spans[i].startSeconds - (origin + static_cast<double>(i) * bar)) < 1.0e-9);
            CHECK(std::abs(spans[i].endSeconds - (origin + static_cast<double>(i + 1) * bar)) < 1.0e-9);
        }
    }

    // ---- 2. free time: changes follow the onsets ----
    {
        std::vector<ChordNote> notes;
        addChord(notes, {38, 62, 65, 69}, 0.13, 1.71);      // Dm
        addChord(notes, {43, 62, 65, 67, 71}, 1.71, 2.40);  // G7
        addChord(notes, {36, 60, 64, 67, 71}, 2.40, 4.95);  // Cmaj7
        const auto spans = polyphonic::detectChords(notes);
        print("free", spans);
        CHECK(names(spans) == (std::vector<std::string>{"Dm", "G7", "Cmaj7"}));
        CHECK(std::abs(spans[0].startSeconds - 0.13) < 1.0e-9);
        CHECK(std::abs(spans[1].startSeconds - 1.71) < 1.0e-9);
        CHECK(std::abs(spans[2].startSeconds - 2.40) < 1.0e-9);
        CHECK(std::abs(spans[0].endSeconds - 1.71) < 1.0e-9);
        CHECK(std::abs(spans[2].endSeconds - 4.95) < 1.0e-9);
    }

    // ---- 3. inversions, a silence, a melody over held chords ----
    {
        std::vector<ChordNote> notes;
        addChord(notes, {40, 60, 67}, 0.0, 2.0);             // C/E (bass E, C and G above)
        addChord(notes, {43, 59, 62}, 2.0, 4.0);             // G
        // silence from 4 to 6
        addChord(notes, {45, 57, 60, 64}, 6.0, 10.0);        // Am, its melody walking above
        for (int i = 0; i < 8; ++i)
        {
            const int melody[8] = {76, 74, 72, 71, 72, 74, 76, 77};
            notes.push_back({6.0 + 0.5 * i, 6.5 + 0.5 * i, static_cast<float>(melody[i]), {}, 0.0, 0.6});
        }
        const auto spans = polyphonic::detectChords(notes);
        print("inversion", spans);
        CHECK(at(spans, 1.0) == "C/E");
        CHECK(at(spans, 3.0) == "G");
        CHECK(at(spans, 5.0).empty());
        CHECK(at(spans, 7.0) == "Am");
        CHECK(at(spans, 9.5) == "Am");
        CHECK(spans.size() == 3);
    }

    // ---- 4. levels: a decaying chord gives way to the next one struck over it ----
    {
        std::vector<ChordNote> notes;
        const double hop = 512.0 / kRate;
        auto decaying = [&](std::initializer_list<int> midi, double start, double length)
        {
            for (int m : midi)
            {
                ChordNote n{start, start + length, static_cast<float>(m), {}, hop, 1.0};
                for (double t = 0.0; t < length; t += hop)
                    n.levels.push_back(static_cast<float>(std::exp(-t * 1.5)));
                notes.push_back(std::move(n));
            }
        };
        decaying({41, 57, 60, 65}, 0.0, 4.0); // F, ringing on under ...
        decaying({43, 59, 62, 67}, 1.0, 3.0); // ... G struck at 1.0
        polyphonic::ChordOptions options;
        options.beatSeconds = 0.5;
        const auto spans = polyphonic::detectChords(notes, options);
        print("levels", spans);
        CHECK(at(spans, 0.5) == "F");
        CHECK(at(spans, 2.0) == "G");
    }

    // ---- 5. flat keys spell with flats ----
    {
        std::vector<ChordNote> notes;
        addChord(notes, {41, 60, 65, 69}, 0.0, 2.0);  // F
        addChord(notes, {46, 62, 65, 70}, 2.0, 4.0);  // Bb
        addChord(notes, {48, 60, 64, 67}, 4.0, 6.0);  // C
        addChord(notes, {41, 60, 65, 69}, 6.0, 8.0);  // F
        polyphonic::ChordOptions options;
        options.beatSeconds = 0.5;
        const auto spans = polyphonic::detectChords(notes, options);
        print("flats", spans);
        CHECK(names(spans) == (std::vector<std::string>{"F", "Bb", "C", "F"}));
        CHECK(polyphonic::keyPrefersFlats(notes));
    }

    // ---- 5b. extended chords ----
    {
        std::vector<ChordNote> notes;
        addChord(notes, {36, 60, 62, 64, 67}, 0.0, 2.0);       // Cadd9
        addChord(notes, {38, 60, 64, 65, 69}, 2.0, 4.0);       // Dm9 (D F A C E)
        addChord(notes, {41, 60, 64, 67, 69}, 4.0, 6.0);       // Fmaj9 (F A C E G)
        addChord(notes, {43, 57, 59, 62, 64}, 6.0, 8.0);       // G6/9 (G B D E A)
        addChord(notes, {45, 60, 64, 68}, 8.0, 10.0);          // AmMaj7 (A C E G#)
        addChord(notes, {43, 59, 63, 65}, 10.0, 12.0);         // G7#5 (G B D# F)
        polyphonic::ChordOptions options;
        options.beatSeconds = 0.5;
        const auto spans = polyphonic::detectChords(notes, options);
        print("extended", spans);
        CHECK(names(spans) == (std::vector<std::string>{"Cadd9", "Dm9", "Fmaj9", "G6/9", "AmMaj7", "G7#5"}));

        // ... but a D passing at half the level over a C chord is still C
        std::vector<ChordNote> passing;
        addChord(passing, {36, 60, 64, 67}, 0.0, 2.0);
        passing.push_back({0.5, 1.5, 62.0f, {}, 0.0, 0.5});
        options.beatSeconds = 2.0;
        CHECK(names(polyphonic::detectChords(passing, options)) == (std::vector<std::string>{"C"}));
    }

    // ---- 6. edits: a whole take moved up a tone, one note muted ----
    {
        polyphonic::Analysis analysis;
        analysis.params = polyphonic::Params::forSampleRate(kRate);
        const double hopSeconds = analysis.frameToSeconds(1.0);
        auto note = [&](int midi, double start, double end)
        {
            polyphonic::Note n;
            n.startFrame = static_cast<int>(std::lround(start / hopSeconds));
            n.endFrame = static_cast<int>(std::lround(end / hopSeconds)) - 1;
            n.f0.assign(static_cast<size_t>(n.numFrames()), static_cast<float>(midiToHz(midi)));
            n.active = true;
            analysis.notes.push_back(n);
        };
        note(36, 0.0, 2.0); note(60, 0.0, 2.0); note(64, 0.0, 2.0); note(67, 0.0, 2.0);   // C
        note(41, 2.0, 4.0); note(60, 2.0, 4.0); note(65, 2.0, 4.0); note(69, 2.0, 4.0);   // F
        // a candidate that never sounds
        note(80, 0.0, 4.0);
        analysis.notes.back().active = false;

        std::map<int, polyphonic::NoteEdit> edits;
        for (int i = 0; i < 8; ++i)
            edits[i].semitones = 2.0f;
        auto spans = polyphonic::detectChords(polyphonic::chordNotes(analysis, nullptr, &edits));
        print("edited", spans);
        CHECK(names(spans) == (std::vector<std::string>{"D", "G"}));

        // C's third lowered and the F moved half a second later
        edits.clear();
        edits[2].semitones = -1.0f;
        for (int i = 4; i < 8; ++i)
            edits[i].timeOffsetSeconds = 0.5;
        edits[7].mute = true; // F's A gone: F5
        spans = polyphonic::detectChords(polyphonic::chordNotes(analysis, nullptr, &edits));
        print("edited 2", spans);
        CHECK(at(spans, 1.0) == "Cm");
        CHECK(at(spans, 3.5) == "F5");
        CHECK(std::abs(spans.back().endSeconds - 4.5) < 0.02);
    }

    // ---- 6b. retuning: how chord tones move ----
    {
        auto chord = [](int root, std::initializer_list<int> intervals, int bass = -2)
        {
            polyphonic::Chord c;
            c.root = root;
            c.bass = bass == -2 ? root : bass;
            for (int i : intervals)
                c.intervals = static_cast<std::uint16_t>(c.intervals | (1u << i));
            return c;
        };
        const auto C = chord(0, {0, 4, 7}), Cm = chord(0, {0, 3, 7}), Am = chord(9, {0, 3, 7});
        const auto F = chord(5, {0, 4, 7}), G = chord(7, {0, 4, 7}), C7 = chord(0, {0, 4, 7, 10});
        const auto Dm7 = chord(2, {0, 3, 7, 10}), G7 = chord(7, {0, 4, 7, 10});
        const auto C5 = chord(0, {0, 7});
        const auto N = polyphonic::kNotAChordTone;
        using M = std::array<int, 12>;
        auto expect = [&](const char* label, const polyphonic::Chord& a, const polyphonic::Chord& b,
                          std::map<int, int> moves, std::uint16_t missing = 0)
        {
            std::uint16_t lost = 0xFFFF;
            const M m = polyphonic::chordToneMotion(a, b, &lost);
            std::printf("%s:", label);
            for (int pc = 0; pc < 12; ++pc)
                if (m[static_cast<size_t>(pc)] != N)
                    std::printf(" %s%+d", polyphonic::pitchClassName(pc, false).c_str(), m[static_cast<size_t>(pc)]);
            std::printf("\n");
            for (int pc = 0; pc < 12; ++pc)
            {
                const auto it = moves.find(pc);
                CHECK(m[static_cast<size_t>(pc)] == (it == moves.end() ? N : it->second));
            }
            CHECK(lost == missing);
        };
        expect("C>Cm", C, Cm, {{0, 0}, {4, -1}, {7, 0}});
        expect("C>Am", C, Am, {{0, 0}, {4, 0}, {7, 2}});
        expect("C>F", C, F, {{0, 0}, {4, 1}, {7, 2}});
        expect("C>G", C, G, {{0, -1}, {4, -2}, {7, 0}});
        expect("Dm7>G7", Dm7, G7, {{2, 0}, {5, 0}, {9, -2}, {0, -1}});
        expect("C>C7", C, C7, {{0, 0}, {4, 0}, {7, 0}}, 1u << 10);       // no note to become Bb
        expect("C5>Cm", C5, Cm, {{0, 0}, {7, 0}}, 1u << 3);              // no third to lower
        expect("Am>A", Am, chord(9, {0, 4, 7}), {{9, 0}, {0, 1}, {4, 0}});
        expect("none", polyphonic::Chord{}, C, {});

        // On a take: a C bar (bass C, voicing C E G, a melody with a passing D
        // and an E) then a G bar that must not move.
        polyphonic::Analysis analysis;
        analysis.params = polyphonic::Params::forSampleRate(kRate);
        const double hop = analysis.frameToSeconds(1.0);
        auto note = [&](int midi, double start, double end)
        {
            polyphonic::Note n;
            n.startFrame = static_cast<int>(std::lround(start / hop));
            n.endFrame = static_cast<int>(std::lround(end / hop)) - 1;
            n.f0.assign(static_cast<size_t>(n.numFrames()), static_cast<float>(midiToHz(midi)));
            analysis.notes.push_back(n);
            return static_cast<int>(analysis.notes.size() - 1);
        };
        const int bass = note(36, 0.0, 2.0), c4 = note(60, 0.0, 2.0), e4 = note(64, 0.0, 2.0),
                  g4 = note(67, 0.0, 2.0);
        const int passing = note(74, 0.5, 1.0), melodyE = note(76, 1.0, 1.5);
        const int early = note(64, -0.02, 2.0); // struck just before the beat
        const int later = note(71, 2.0, 4.0);   // the next bar's B
        const int muted = note(64, 0.0, 2.0);
        std::map<int, polyphonic::NoteEdit> edits;
        edits[muted].mute = true;
        edits[g4].gainDb = -3.0f;

        auto r = polyphonic::retuneToChord(analysis, edits, 0.0, 2.0, C, Cm);
        CHECK(r.movedNotes == 3); // both E4s and the melody's E
        CHECK(r.edits[e4].semitones == -1.0f && r.edits[early].semitones == -1.0f);
        CHECK(r.edits[melodyE].semitones == -1.0f);
        CHECK(r.edits.count(bass) == 0 && r.edits.count(c4) == 0 && r.edits.count(passing) == 0);
        CHECK(r.edits.count(later) == 0);
        CHECK(r.edits[muted].mute && r.edits[muted].semitones == 0.0f);
        CHECK(r.edits[g4].gainDb == -3.0f && r.edits[g4].semitones == 0.0f); // its other edit kept
        // and it reads Cm now
        auto spans = polyphonic::detectChords(polyphonic::chordNotes(analysis, nullptr, &r.edits));
        CHECK(at(spans, 1.0) == "Cm");

        // Back to C: the edits it made are gone, the others stay.
        auto back = polyphonic::retuneToChord(analysis, r.edits, 0.0, 2.0, Cm, C);
        CHECK(back.movedNotes == 3);
        CHECK(back.edits == edits);

        // An explicit bass: C -> C/E moves the low C up to E, and nothing else.
        auto CoverE = C;
        CoverE.bass = 4;
        auto inv = polyphonic::retuneToChord(analysis, edits, 0.0, 2.0, C, CoverE);
        CHECK(inv.movedNotes == 1 && inv.edits[bass].semitones == 4.0f);
        // Following the mapping instead: C/E -> Cm with bass -1 lowers the bass E too.
        auto follow = Cm;
        follow.bass = -1;
        auto lowered = polyphonic::retuneToChord(analysis, inv.edits, 0.0, 2.0, CoverE, follow);
        CHECK(lowered.edits[bass].semitones == 3.0f);
        spans = polyphonic::detectChords(polyphonic::chordNotes(analysis, nullptr, &lowered.edits));
        CHECK(at(spans, 1.0) == "Cm/Eb");

        // Typed chords say their bass: "F" over the C bar is F in root position,
        // the low C going up to F; "Am" takes the low C down to A.
        auto typedF = polyphonic::retuneToChord(analysis, edits, 0.0, 2.0, C, *polyphonic::parseChord("F"));
        CHECK(typedF.edits[bass].semitones == 5.0f && typedF.edits[e4].semitones == 1.0f
              && typedF.edits[g4].semitones == 2.0f && typedF.edits.count(c4) == 0);
        auto typedAm = polyphonic::retuneToChord(analysis, edits, 0.0, 2.0, C, *polyphonic::parseChord("Am"));
        CHECK(typedAm.edits[bass].semitones == -3.0f && typedAm.edits[g4].semitones == 2.0f);
        spans = polyphonic::detectChords(polyphonic::chordNotes(analysis, nullptr, &typedAm.edits));
        CHECK(at(spans, 1.0) == "Am");
        // From C/E, "Cm" is root position (the E bass to C), "Cm/Eb" keeps the inversion.
        auto rootPos = polyphonic::retuneToChord(analysis, inv.edits, 0.0, 2.0, CoverE, *polyphonic::parseChord("Cm"));
        CHECK(rootPos.edits[bass].semitones == 0.0f || rootPos.edits.count(bass) == 0);
        auto keptInv = polyphonic::retuneToChord(analysis, inv.edits, 0.0, 2.0, CoverE, *polyphonic::parseChord("Cm/Eb"));
        CHECK(keptInv.edits[bass].semitones == 3.0f);
        // Extensions the recording lacks are reported, the rest still moves.
        auto ext = polyphonic::retuneToChord(analysis, edits, 0.0, 2.0, C, *polyphonic::parseChord("Cm9"));
        CHECK(ext.movedNotes == 3 && ext.missing == ((1u << 2) | (1u << 10)));

        // C -> C7: nothing to move, the seventh reported missing.
        auto seventh = polyphonic::retuneToChord(analysis, edits, 0.0, 2.0, C, C7);
        CHECK(seventh.movedNotes == 0 && seventh.missing == (1u << 10) && seventh.edits == edits);
    }

    // ---- 6c. a segmentation the user set ----
    {
        std::vector<ChordNote> notes;
        addChord(notes, {36, 60, 64, 67}, 0.0, 2.0);  // C
        addChord(notes, {41, 60, 65, 69}, 2.0, 4.0);  // F
        addChord(notes, {43, 59, 62, 67}, 5.0, 7.0);  // G after a silence
        // the C bar split in two: both halves C
        auto spans = polyphonic::chordsOver(notes, {0.0, 1.0, 2.0, 4.0, 5.0, 7.0});
        print("segmented", spans);
        CHECK(names(spans) == (std::vector<std::string>{"C", "C", "F", "", "G"}));
        CHECK(spans[1].startSeconds == 1.0 && spans[1].endSeconds == 2.0);
        CHECK(spans[3].chord.isNone());
        // C and F merged: one span, read from both (C F A / C E G: F wins on its bass? either way one chord)
        spans = polyphonic::chordsOver(notes, {0.0, 4.0, 5.0, 7.0});
        print("merged", spans);
        CHECK(spans.size() == 3 && !spans[0].chord.isNone() && spans[1].chord.isNone() && spans[2].name == "G");
        // an inversion over a span
        std::vector<ChordNote> inv;
        addChord(inv, {40, 60, 67}, 0.0, 2.0);
        CHECK(names(polyphonic::chordsOver(inv, {0.0, 2.0})) == (std::vector<std::string>{"C/E"}));
        CHECK(polyphonic::chordsOver(notes, {1.0}).empty());
        CHECK(polyphonic::chordsOver(notes, {2.0, 1.0}).empty()); // not ascending: no span
    }

    // ---- 7. nothing to read ----
    {
        CHECK(polyphonic::detectChords({}).empty());
        std::vector<ChordNote> one;
        one.push_back({0.0, 3.0, 60.0f, {}, 0.0, 1.0});
        CHECK(polyphonic::detectChords(one).empty());
    }

    // ---- 8. through the engine: C - Am - F - G at 96 BPM ----
    {
        const double beat = 60.0 / 96.0, bar = 4 * beat, origin = 0.25;
        const int chords[4][4] = {{36, 60, 64, 67}, {33, 60, 64, 69}, {41, 60, 65, 69}, {43, 59, 62, 67}};
        const double seconds = origin + 8 * bar;
        std::vector<float> x(static_cast<size_t>((seconds + 1.0) * kRate), 0.0f);
        for (int b = 0; b < 8; ++b)
        {
            const double t = origin + b * bar;
            const auto& c = chords[b % 4];
            addTone(x, midiToHz(c[0]), t, bar, 0.30);
            for (int k = 0; k < 4; ++k) // the chord on every beat
                for (int j = 1; j < 4; ++j)
                    addTone(x, midiToHz(c[j]), t + k * beat, beat * 0.95, 0.12);
        }
        const auto params = polyphonic::Params::forSampleRate(kRate);
        const auto analysis = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        polyphonic::ChordOptions options;
        options.beatSeconds = beat;
        options.gridOriginSeconds = origin;
        const auto levels = levelsFromMasks(analysis);
        const auto spans = polyphonic::detectChords(polyphonic::chordNotes(analysis, &levels, nullptr), options);
        print("engine", spans);
        // without levels, each note weighs by its energy per frame
        const auto plain = polyphonic::detectChords(polyphonic::chordNotes(analysis, nullptr, nullptr), options);
        print("engine, no levels", plain);
        const char* expected[4] = {"C", "Am", "F", "G"};
        int right = 0;
        for (int b = 0; b < 8; ++b)
            right += at(spans, origin + (b + 0.5) * bar) == expected[b % 4] ? 1 : 0;
        int plainRight = 0;
        for (int b = 0; b < 8; ++b)
            plainRight += at(plain, origin + (b + 0.5) * bar) == expected[b % 4] ? 1 : 0;
        std::printf("engine: %d / 8 bars right (%d without levels)\n", right, plainRight);
        CHECK(right >= 7);
        CHECK(plainRight >= 6);
    }

    std::printf("PolyChordTests passed\n");
    return 0;
}
