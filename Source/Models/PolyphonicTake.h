#pragma once

#include "../JuceHeader.h"
#include "../Audio/Polyphonic/PolyEngine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

/**
 * The polyphonic engine's data for one take (docs/polyphonic-integration-plan.md,
 * section 2). A Project holds it through a shared_ptr and copies of the Project
 * share it, so a change replaces the pointer with a modified copy rather than
 * mutating a shared take.
 *
 * The loader creates a take holding only `source` (the file's channels at their
 * own rate) before the take is classified; analysis keeps it for a polyphonic
 * take - resampled to the project rate, with `analysis` filled. A vocal take
 * keeps only `source`, so a switch to polyphonic still has its channels.
 *
 * The take is what the analysis produced and never changes; the user's edits
 * live in the Project (Project::getPolyphonicEdits), which changes them often.
 */
struct PolyphonicTake
{
    /** Notes, candidates and masks; immutable once built. */
    std::shared_ptr<const polyphonic::Analysis> analysis;

    /** The take's own channels (up to two; stereo is kept), at `sourceRate`. */
    juce::AudioBuffer<float> source;
    double sourceRate = 0.0;

    /** Opened from a saved project: the notes are there with their saved
     *  levels (so the take is shown and edited as usual), while its masks and
     *  `source` are rebuilt in the background
     *  (EditorController::restorePolyphonicAsync). Nothing renders until the
     *  restored take replaces this one. */
    bool restoring = false;
    /** While restoring: each note's saved level per frame (see noteLevels). */
    std::vector<std::vector<float>> savedLevels;

    bool hasAnalysis() const { return analysis != nullptr; }
};

/**
 * Each note's level per frame, from its startFrame, one value per frame of the
 * note: the root of its partials' summed squared model amplitudes (empty for
 * an inactive note). The piano roll draws a note's width from it and MIDI
 * export its velocity; a saved project keeps it, so an opened take is drawn
 * exactly before its masks are rebuilt.
 */
inline std::vector<std::vector<float>> noteLevelsFromMasks(const polyphonic::Analysis &analysis)
{
    std::vector<std::vector<float>> levels(analysis.notes.size());
    for (size_t i = 0; i < analysis.notes.size() && i < analysis.masks.size(); ++i)
    {
        const auto &note = analysis.notes[i];
        if (!note.active)
            continue;
        std::vector<double> energy(static_cast<size_t>(std::max(1, note.numFrames())), 0.0);
        for (const auto &partial : analysis.masks[i].partials)
        {
            const int k = partial.frame - note.startFrame;
            if (k >= 0 && k < static_cast<int>(energy.size()))
                energy[static_cast<size_t>(k)] +=
                    static_cast<double>(partial.amplitude) * partial.amplitude;
        }
        auto &level = levels[i];
        level.resize(energy.size());
        for (size_t k = 0; k < energy.size(); ++k)
            level[k] = static_cast<float>(std::sqrt(energy[k]));
    }
    return levels;
}

/** A take's note levels: from its masks, or the saved ones while it is restoring. */
inline std::vector<std::vector<float>> noteLevels(const PolyphonicTake &take)
{
    if (take.restoring)
        return take.savedLevels;
    return take.analysis ? noteLevelsFromMasks(*take.analysis)
                         : std::vector<std::vector<float>>{};
}

/**
 * A polyphonic take as a saved project holds it: the analysis parameters, the
 * notes and their levels (for drawing), without masks (they are rebuilt from
 * the audio with polyphonic::restore, see
 * EditorController::restorePolyphonicAsync). The edits are read into the
 * Project directly; they are keyed by index into `notes`, which keep their
 * order.
 */
struct PolyphonicRestore
{
    polyphonic::Params params;
    std::vector<polyphonic::Note> notes;
    std::vector<std::vector<float>> levels; // per note, as noteLevelsFromMasks
};

/** The take an opened project shows while its masks are rebuilt: the saved
 *  notes and levels in an analysis without masks, over `numSamples` of audio. */
inline std::shared_ptr<PolyphonicTake> makeRestoringTake(const PolyphonicRestore &saved,
                                                         int numSamples)
{
    auto analysis = std::make_shared<polyphonic::Analysis>();
    analysis->params = saved.params;
    analysis->numSamples = numSamples;
    // As the engine's STFT counts them (PolySpectral.cpp).
    analysis->numFrames = 1 + (numSamples + saved.params.hop - 1) / saved.params.hop;
    analysis->numBins = saved.params.fftSize / 2 + 1;
    analysis->notes = saved.notes;
    analysis->masks.assign(analysis->notes.size(), {});
    auto take = std::make_shared<PolyphonicTake>();
    take->analysis = std::move(analysis);
    take->restoring = true;
    take->savedLevels = saved.levels;
    return take;
}

