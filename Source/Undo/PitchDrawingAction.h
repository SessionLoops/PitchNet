#pragma once

#include "UndoableAction.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/TransformParams.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

struct PitchDrawingNoteState
{
    Note* note = nullptr;
    TransformParams params;
    std::vector<float> bakedDeltaPitch;
};

/**
 * Commits an anchor-curve preview into each note's editable baked contour and
 * resets its pitch-tool transforms. The immutable analysis contour is never
 * changed; the previous baked contour and transforms are restored by undo/redo.
 */
class PitchDrawingAction final : public UndoableAction
{
public:
    PitchDrawingAction(Project* projectToEdit,
                      std::vector<PitchDrawingNoteState> beforeStates,
                      std::vector<PitchDrawingNoteState> afterStates)
        : project(projectToEdit), before(std::move(beforeStates)),
          after(std::move(afterStates))
    {
    }

    void undo() override { apply(before); }
    void redo() override { apply(after); }
    juce::String getName() const override { return "Apply Pitch Drawing"; }

private:
    void apply(const std::vector<PitchDrawingNoteState>& states)
    {
        int minFrame = std::numeric_limits<int>::max();
        int maxFrame = std::numeric_limits<int>::min();

        for (const auto& state : states)
        {
            if (!state.note)
                continue;

            state.params.applyToNote(*state.note);
            state.note->setBakedDeltaPitch(state.bakedDeltaPitch);
            state.note->markDirty();
            state.note->markSynthDirty();
            minFrame = std::min(minFrame, state.note->getStartFrame());
            maxFrame = std::max(maxFrame, state.note->getEndFrame());
        }

        if (!project || minFrame > maxFrame)
            return;

        PitchCurveProcessor::rebuildBaseFromNotes(*project);
        project->markNoteEditDirtyRange(minFrame, maxFrame);
    }

    Project* project = nullptr;
    std::vector<PitchDrawingNoteState> before;
    std::vector<PitchDrawingNoteState> after;
};
