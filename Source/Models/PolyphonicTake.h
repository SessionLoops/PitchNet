#pragma once

#include "../JuceHeader.h"
#include "../Audio/Polyphonic/PolyEngine.h"

#include <map>
#include <memory>

/**
 * The polyphonic engine's data for one take (docs/polyphonic-integration-plan.md,
 * section 2). A Project holds it through a shared_ptr and copies of the Project
 * share it, so a change replaces the pointer with a modified copy rather than
 * mutating a shared take.
 *
 * The loader creates a take holding only `source` (the file's channels at their
 * own rate) before the take is classified; analysis keeps it for a polyphonic
 * take - resampled to the project rate, with `analysis` filled - and drops it
 * for a vocal one.
 */
struct PolyphonicTake
{
    /** Notes, candidates and masks; immutable once built. */
    std::shared_ptr<const polyphonic::Analysis> analysis;

    /** The take's own channels (up to two; stereo is kept), at `sourceRate`. */
    juce::AudioBuffer<float> source;
    double sourceRate = 0.0;

    /** Per-note edits, keyed by index into analysis->notes (M3). */
    std::map<int, polyphonic::NoteEdit> edits;

    bool hasAnalysis() const { return analysis != nullptr; }
};
