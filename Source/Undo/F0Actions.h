#pragma once

#include "UndoableAction.h"
#include "F0FrameEdit.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include <vector>
#include <functional>
#include <limits>

/**
 * Action for changing multiple F0 values (hand-drawing).
 */
class F0EditAction : public UndoableAction
{
public:
    /** A note whose directF0Edit flag this edit set, with its prior value.
        Drawing marks the notes it covers so they are not judged neutral, but
        that flag lives on the note rather than in the F0 arrays below - without
        capturing it here, undoing a drawing would restore the curve and leave
        the notes permanently marked as edited.

        The note is identified by its start frame and resolved through the
        project, never held as a pointer: Project stores notes by value in a
        vector, and NoteSplitAction/NoteMergeAction add and remove entries on
        undo, which reallocates it. A pointer captured here would dangle after
        draw -> split -> undo split -> undo drawing. This matches how those two
        actions already address notes. A note whose start frame has since moved
        does not resolve and its flag is left alone, which is a no-op rather
        than a stale write. */
    struct NoteFlagEdit
    {
        int noteStartFrame;
        bool wasDirectF0Edit;
    };

    F0EditAction(std::vector<float>* f0Array,
                 std::vector<float>* deltaPitchArray,
                 std::vector<bool>* voicedMask,
                 std::vector<F0FrameEdit> edits,
                 std::function<void(int, int)> onF0Changed = nullptr,
                 std::vector<NoteFlagEdit> noteFlagEdits = {},
                 Project* project = nullptr)
        : f0Array(f0Array), deltaPitchArray(deltaPitchArray), voicedMask(voicedMask),
          project(project), noteFlagEdits(std::move(noteFlagEdits)),
          edits(std::move(edits)), onF0Changed(onF0Changed) {}

    void undo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.oldF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.oldDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.oldVoiced;
        }
        for (const auto& n : noteFlagEdits)
            if (Note* note = findNote(n.noteStartFrame))
                note->setDirectF0Edit(n.wasDirectF0Edit);
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    void redo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.newF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.newDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.newVoiced;
        }
        for (const auto& n : noteFlagEdits)
            if (Note* note = findNote(n.noteStartFrame))
                note->setDirectF0Edit(true);
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    juce::String getName() const override { return "Edit Pitch Curve"; }

private:
    /** Resolve a recorded note by start frame. Returns nullptr when the note no
        longer exists or has moved, so a stale entry is simply skipped. */
    Note* findNote(int startFrame) const
    {
        if (!project)
            return nullptr;
        for (auto& note : project->getNotes())
            if (note.getStartFrame() == startFrame)
                return &note;
        return nullptr;
    }

    std::vector<float>* f0Array;
    std::vector<float>* deltaPitchArray;
    std::vector<bool>* voicedMask;
    Project* project;
    std::vector<NoteFlagEdit> noteFlagEdits;
    std::vector<F0FrameEdit> edits;
    std::function<void(int, int)> onF0Changed;
};
