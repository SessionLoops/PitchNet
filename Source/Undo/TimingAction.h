#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/TimingRegionUtils.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

struct NoteTimingState
{
    Note* note = nullptr;
    int startFrame = 0;
    int endFrame = 0;

    static NoteTimingState capture(Note& note)
    {
        return {&note, note.getStartFrame(), note.getEndFrame()};
    }
};

inline void applyNoteTimingStates(Project& project,
                                  const std::vector<NoteTimingState>& states)
{
    int dirtyStart = std::numeric_limits<int>::max();
    int dirtyEnd = std::numeric_limits<int>::min();

    for (const auto& state : states)
    {
        if (!state.note)
            continue;

        const bool leftEdgeChanged =
            state.note->getStartFrame() != state.startFrame;
        const bool rightEdgeChanged =
            state.note->getEndFrame() != state.endFrame;
        if (!leftEdgeChanged && !rightEdgeChanged)
            continue;

        if (leftEdgeChanged &&
            timingRegions::isFirstNote(project, *state.note))
        {
            const auto region =
                timingRegions::getSourceRegion(project, *state.note);
            const int currentRegionStart =
                region.start + state.note->getStartFrame() -
                state.note->getSrcStartFrame();
            const int destinationRegionStart =
                region.start + state.startFrame -
                state.note->getSrcStartFrame();
            dirtyStart = std::min({dirtyStart, region.start,
                                   currentRegionStart,
                                   destinationRegionStart});
        }

        if (rightEdgeChanged &&
            timingRegions::isLastNote(project, *state.note))
        {
            const auto region =
                timingRegions::getSourceRegion(project, *state.note);
            const int currentRegionEnd =
                region.end + state.note->getEndFrame() -
                state.note->getSrcEndFrame();
            const int destinationRegionEnd =
                region.end + state.endFrame - state.note->getSrcEndFrame();
            dirtyEnd = std::max({dirtyEnd, region.end,
                                 currentRegionEnd,
                                 destinationRegionEnd});
        }

        dirtyStart = std::min({dirtyStart, state.note->getStartFrame(),
                               state.startFrame});
        dirtyEnd = std::max({dirtyEnd, state.note->getEndFrame(),
                             state.endFrame});
        state.note->setStartFrame(state.startFrame);
        state.note->setEndFrame(state.endFrame);
        state.note->markDirty();
        state.note->markSynthDirty();
    }

    PitchCurveProcessor::rebuildBaseFromNotes(project);
    if (dirtyStart <= dirtyEnd)
    {
        const int frameCount = project.getAudioData().getNumFrames();
        project.setF0DirtyRange(std::max(0, dirtyStart - 24),
                                std::min(frameCount, dirtyEnd + 24));
    }
    project.setModified(true);
}

class TimingAction final : public UndoableAction
{
public:
    TimingAction(Project& project,
                 std::vector<NoteTimingState> before,
                 std::vector<NoteTimingState> after,
                 juce::String name = "Edit Timing")
        : project(project), before(std::move(before)), after(std::move(after)),
          name(std::move(name)) {}

    void undo() override { applyNoteTimingStates(project, before); }
    void redo() override { applyNoteTimingStates(project, after); }
    juce::String getName() const override { return name; }

private:
    Project& project;
    std::vector<NoteTimingState> before;
    std::vector<NoteTimingState> after;
    juce::String name;
};
