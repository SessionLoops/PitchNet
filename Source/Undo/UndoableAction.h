#pragma once

#include "../JuceHeader.h"
#include <memory>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
#include "../Models/Note.h"

/**
 * Base class for undoable actions.
 */
class UndoableAction
{
public:
    virtual ~UndoableAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getName() const = 0;
    virtual bool ownsProjectContext() const { return false; }
    virtual bool belongsToContext(const juce::String&) const { return false; }

    // Most edits alter rendered audio. Layout-only actions can override this
    // so undo/redo mirrors the original action's processing behaviour.
    virtual bool requiresAudioResynthesis() const { return true; }

    // Splits and merges insert into or erase from the project's note vector,
    // which moves or reallocates it, so a Note* kept in history can dangle or
    // point at another note. Actions report their Note* fields here; history
    // keeps them as indices and resolves them again before each replay.
    // Undo is LIFO, so the note layout at replay matches the one at record.
    virtual void collectNotePointers(std::vector<Note **> &) {}

    void bindNoteStorage(std::function<std::vector<Note> *()> storage)
    {
        noteStorage = std::move(storage);
        noteRefs.clear();
        auto *notes = noteStorage ? noteStorage() : nullptr;
        if (!notes || notes->empty())
            return;
        std::vector<Note **> fields;
        collectNotePointers(fields);
        const auto begin = reinterpret_cast<std::uintptr_t>(notes->data());
        const auto end = reinterpret_cast<std::uintptr_t>(notes->data() + notes->size());
        for (auto **field : fields)
        {
            const auto address = reinterpret_cast<std::uintptr_t>(*field);
            // Notes of other projects (track-wide edits) keep their pointer.
            if (*field != nullptr && address >= begin && address < end)
                noteRefs.emplace_back(field, (address - begin) / sizeof(Note));
        }
    }

    void performUndo() { resolveNotePointers(); undo(); }
    void performRedo() { resolveNotePointers(); redo(); }

private:
    void resolveNotePointers()
    {
        if (noteRefs.empty() || !noteStorage)
            return;
        auto *notes = noteStorage();
        if (!notes)
            return;
        for (auto &[field, index] : noteRefs)
            *field = index < notes->size() ? &(*notes)[index] : nullptr;
    }

    std::function<std::vector<Note> *()> noteStorage;
    std::vector<std::pair<Note **, std::size_t>> noteRefs;
};
