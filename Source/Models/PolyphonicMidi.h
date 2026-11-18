#pragma once

#include "Project.h"
#include "../Audio/IO/MidiExporter.h"

#include <algorithm>
#include <cmath>
#include <vector>

/**
 * A polyphonic take's notes as MIDI (feature review #21): pitch with its edit,
 * start and end where the edit moves and stretches them, muted notes left out;
 * chords become overlapping notes. Velocity comes from each note's level: its
 * peak level in the analysis plus its gain edit, relative to the take's
 * loudest note, mapped -40...0 dB to 1...127 (a starting point to tune on real
 * takes).
 */
namespace polyphonicmidi
{
constexpr float kQuietestDb = -40.0f;

inline int velocityForLevelDb(float levelDb)
{
    const float t = (juce::jlimit(kQuietestDb, 0.0f, levelDb) - kQuietestDb) / -kQuietestDb;
    return 1 + static_cast<int>(std::lround(t * 126.0f));
}

/** Each note's peak level: the root of its partials' summed squared model
 *  amplitudes, at its loudest frame (0 for inactive notes). */
inline std::vector<double> peakLevels(const polyphonic::Analysis& analysis)
{
    std::vector<double> peaks(analysis.notes.size(), 0.0);
    for (size_t i = 0; i < analysis.notes.size() && i < analysis.masks.size(); ++i)
    {
        const auto& note = analysis.notes[i];
        if (!note.active)
            continue;
        std::vector<double> energy(static_cast<size_t>(std::max(1, note.numFrames())), 0.0);
        for (const auto& partial : analysis.masks[i].partials)
        {
            const int k = partial.frame - note.startFrame;
            if (k >= 0 && k < static_cast<int>(energy.size()))
                energy[static_cast<size_t>(k)] +=
                    static_cast<double>(partial.amplitude) * partial.amplitude;
        }
        for (double e : energy)
            peaks[i] = std::max(peaks[i], std::sqrt(e));
    }
    return peaks;
}

/** The take's notes to export, in time order; empty for a vocal take. */
inline std::vector<MidiExportNote> exportNotes(const Project& project)
{
    std::vector<MidiExportNote> out;
    const auto take = project.getPolyphonicTake();
    if (!take || !take->hasAnalysis())
        return out;
    const auto& analysis = *take->analysis;
    const auto& edits = project.getPolyphonicEdits();
    const auto peaks = peakLevels(analysis);
    const double loudest = *std::max_element(peaks.begin(), peaks.end());

    for (size_t i = 0; i < analysis.notes.size(); ++i)
    {
        const auto& note = analysis.notes[i];
        if (!note.active)
            continue;
        const auto it = edits.find(static_cast<int>(i));
        const auto edit = it != edits.end() ? it->second : polyphonic::NoteEdit{};
        if (edit.mute)
            continue;
        const auto map = polyphonic::stretchMap(analysis, static_cast<int>(i), edit);
        const double lastOut = map.cuts() ? map.end + 1.0 : map.outputFrame(note.endFrame + 1.0);

        MidiExportNote exported;
        exported.startSeconds = analysis.frameToSeconds(note.startFrame) + edit.timeOffsetSeconds;
        exported.endSeconds = analysis.frameToSeconds(lastOut) + edit.timeOffsetSeconds;
        exported.midi = note.medianMidi() + edit.semitones;
        const float levelDb = peaks[i] > 0.0 && loudest > 0.0
                                  ? static_cast<float>(20.0 * std::log10(peaks[i] / loudest))
                                  : kQuietestDb;
        exported.velocity = velocityForLevelDb(levelDb + edit.gainDb);
        out.push_back(exported);
    }
    std::sort(out.begin(), out.end(), [](const MidiExportNote& a, const MidiExportNote& b) {
        return a.startSeconds < b.startSeconds;
    });
    return out;
}
} // namespace polyphonicmidi
