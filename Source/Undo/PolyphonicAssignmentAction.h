#pragma once

#include "UndoableAction.h"
#include "../Models/PolyphonicAssignment.h"

#include <functional>
#include <vector>

/**
 * One change of a polyphonic take's note assignment (activating a candidate,
 * deactivating a note, moving a note to another candidate or harmonic): the
 * take's notes and edits before and after it. Undo and redo put an
 * assignment back (polyphonicassignment::apply, a restoring take) and report
 * it through `changed`, which starts the rebuild of the take's masks and then
 * renders it; nothing renders before that, so the action itself asks for no
 * resynthesis.
 */
class PolyphonicAssignmentAction : public UndoableAction
{
public:
  using Notes = std::vector<polyphonic::Note>;
  using Edits = Project::PolyphonicEdits;

  using Levels = std::vector<std::vector<float>>;

  /** One side of the change: the notes, their edits, the note assignment
   *  slider's ball (polyphonicassignment::thresholdOf) and the levels the
   *  notes are drawn with until their masks are rebuilt (so undoing a split or
   *  merge keeps the notes' shapes). */
  struct State
  {
    Notes notes;
    Edits edits;
    float threshold = 0.5f;
    Levels levels;
  };

  PolyphonicAssignmentAction(Project &project, State before, State after, juce::String name,
                             std::function<void()> changed = {})
      : project(project), before(std::move(before)), after(std::move(after)),
        name(std::move(name)), changed(std::move(changed))
  {
  }

  void undo() override { apply(before); }
  void redo() override { apply(after); }
  juce::String getName() const override { return name; }
  bool requiresAudioResynthesis() const override { return false; }

private:
  void apply(const State &state)
  {
    if (polyphonicassignment::apply(project, state.notes, state.edits, state.threshold,
                                    state.levels) != nullptr &&
        changed)
      changed();
  }

  Project &project;
  State before;
  State after;
  juce::String name;
  std::function<void()> changed;
};
