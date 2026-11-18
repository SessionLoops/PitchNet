#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"

#include <functional>

/**
 * One edit of a polyphonic take's notes (a drag, a stretch, a mute, a level
 * change, Restore Original): the take's whole edit map before and after it.
 * The maps are small (one entry per edited note), so keeping both whole is
 * simpler and safer than diffing. Undo and redo put a map back and ask for a
 * render (requiresAudioResynthesis), which re-renders the take.
 *
 * The action refers to the project it was made on; re-analysing a take
 * replaces the project and clears the undo history first.
 */
class PolyphonicEditAction : public UndoableAction
{
public:
    using Edits = Project::PolyphonicEdits;

    PolyphonicEditAction(Project& project, Edits before, Edits after,
                         juce::String name, std::function<void()> changed = {})
        : project(project), before(std::move(before)), after(std::move(after)),
          name(std::move(name)), changed(std::move(changed)) {}

    void undo() override { apply(before); }
    void redo() override { apply(after); }
    juce::String getName() const override { return name; }

private:
    void apply(const Edits& edits)
    {
        project.setPolyphonicEdits(edits);
        if (changed)
            changed();
    }

    Project& project;
    Edits before;
    Edits after;
    juce::String name;
    std::function<void()> changed;
};
