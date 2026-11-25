#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"

#include <functional>
#include <vector>

/**
 * A split or merge in the chord track, or going back to the detected chords:
 * the chord segmentation before and after (Project::getChordBoundaries).
 * Nothing renders: a segmentation changes no note.
 */
class ChordBoundariesAction : public UndoableAction
{
public:
    ChordBoundariesAction(Project& project, std::vector<double> before, std::vector<double> after,
                          juce::String name, std::function<void()> changed = {})
        : project(project), before(std::move(before)), after(std::move(after)),
          name(std::move(name)), changed(std::move(changed)) {}

    void undo() override { apply(before); }
    void redo() override { apply(after); }
    juce::String getName() const override { return name; }
    bool requiresAudioResynthesis() const override { return false; }

private:
    void apply(const std::vector<double>& boundaries)
    {
        project.setChordBoundaries(boundaries);
        if (changed)
            changed();
    }

    Project& project;
    std::vector<double> before;
    std::vector<double> after;
    juce::String name;
    std::function<void()> changed;
};
