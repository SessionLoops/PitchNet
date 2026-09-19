#pragma once

#include "UndoableAction.h"
#include "F0FrameEdit.h"
#include "../Models/Note.h"
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
        the notes permanently marked as edited. */
    struct NoteFlagEdit
    {
        Note* note;
        bool wasDirectF0Edit;
    };

    F0EditAction(std::vector<float>* f0Array,
                 std::vector<float>* deltaPitchArray,
                 std::vector<bool>* voicedMask,
                 std::vector<F0FrameEdit> edits,
                 std::function<void(int, int)> onF0Changed = nullptr,
                 std::vector<NoteFlagEdit> noteFlagEdits = {})
        : f0Array(f0Array), deltaPitchArray(deltaPitchArray), voicedMask(voicedMask),
          edits(std::move(edits)), onF0Changed(onF0Changed),
          noteFlagEdits(std::move(noteFlagEdits)) {}

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
            if (n.note)
                n.note->setDirectF0Edit(n.wasDirectF0Edit);
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
            if (n.note)
                n.note->setDirectF0Edit(true);
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    juce::String getName() const override { return "Edit Pitch Curve"; }

private:
    std::vector<float>* f0Array;
    std::vector<float>* deltaPitchArray;
    std::vector<bool>* voicedMask;
    std::vector<NoteFlagEdit> noteFlagEdits;
    std::vector<F0FrameEdit> edits;
    std::function<void(int, int)> onF0Changed;
};
