#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Undo/UndoActions.h"
#include "CoordinateMapper.h"
#include <functional>

/**
 * Handles note splitting operations.
 */
class NoteSplitter {
public:
    NoteSplitter() = default;
    ~NoteSplitter() = default;

    void setProject(Project* proj) { project = proj; }
    void setUndoManager(PitchUndoManager* manager) { undoManager = manager; }
    void setCoordinateMapper(CoordinateMapper* mapper) { coordMapper = mapper; }

    /**
     * Find note at the given world coordinates.
     */
    Note* findNoteAt(float x, float y);

    /**
     * Split a note at the given frame position.
     * Returns true if split was successful.
     */
    bool splitNoteAtFrame(Note* note, int splitFrame);

    /**
     * Split note at world X coordinate.
     * Returns true if split was successful.
     */
    bool splitNoteAtX(Note* note, float x);

    /** Returns the adjacent note pair represented by a split boundary at x/y. */
    std::pair<Note*, Note*> findMergeableNotesAt(float x, float y);

    /** Merges two adjacent segments that originated from the same note. */
    bool mergeNotes(Note* first, Note* second);

    // Callbacks
    std::function<void()> onNoteSplit;

private:
    Project* project = nullptr;
    PitchUndoManager* undoManager = nullptr;
    CoordinateMapper* coordMapper = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteSplitter)
};
