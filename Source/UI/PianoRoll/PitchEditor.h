#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Undo/UndoActions.h"
#include "../../Utils/BasePitchPreview.h"
#include "../../Utils/PitchCurveProcessor.h"
#include "CoordinateMapper.h"
#include <memory>
#include <functional>

/**
 * Handles pitch editing operations including single-note and multi-note dragging.
 */
class PitchEditor {
public:
    PitchEditor();
    ~PitchEditor() = default;

    void setProject(Project* proj) { project = proj; }
    void setUndoManager(PitchUndoManager* manager) { undoManager = manager; }
    void setCoordinateMapper(CoordinateMapper* mapper) { coordMapper = mapper; }
    void setSnapToSemitoneDragEnabled(bool enabled) { snapToSemitoneDragEnabled = enabled; }
    void setDragSnapMode(DragSnapMode mode) { dragSnapMode = mode; }
    void setPitchReferenceHz(int hz) { pitchReferenceHz = juce::jlimit(430, 450, hz); }
    float getSnappedDragOffset(float rawOffsetSemitones,
                               float anchorMidiNote) const;

    // Note selection and dragging
    Note* findNoteAt(float x, float y);
    void startNoteDrag(Note* note, float y);
    void updateNoteDrag(float y);
    void endNoteDrag();
    bool isDraggingNote() const { return isDragging; }
    Note* getDraggedNote() const { return draggedNote; }

    // Multi-note dragging
    void startMultiNoteDrag(const std::vector<Note*>& notes, float y,
                            Note* hoveredNote);
    void updateMultiNoteDrag(float x, float y, bool invertSnap = false);
    void endMultiNoteDrag();
    bool isDraggingMultiNotes() const { return isMultiDragging; }
    const std::vector<Note*>& getDraggedNotes() const { return draggedNotes; }
    Note* getHoveredMultiDragNote() const { return hoveredMultiDragNote; }

    // Snap note to semitone
    void snapNoteToSemitone(Note* note);

    // Callbacks
    std::function<void(Note*)> onNoteSelected;
    std::function<void()> onPitchEdited;
    std::function<void()> onPitchEditFinished;
    std::function<void()> onBasePitchCacheInvalidated;

private:
    void prepareDragBasePreview();
    void applyDragBasePreview(float pitchOffsetSemitones);
    void restoreDragBasePreview();
    Note* findDraggedNoteAt(float x, float y) const;

    Project* project = nullptr;
    PitchUndoManager* undoManager = nullptr;
    CoordinateMapper* coordMapper = nullptr;

    // Drag state
    bool isDragging = false;
    Note* draggedNote = nullptr;
    float dragStartY = 0.0f;
    float originalPitchOffset = 0.0f;
    float originalMidiNote = 60.0f;
    float boundaryF0Start = 0.0f;
    float boundaryF0End = 0.0f;
    std::vector<float> originalF0Values;
    float lastDragPitchOffset = 0.0f;
    int dragPreviewStartFrame = -1;
    int dragPreviewEndFrame = -1;
    std::vector<float> dragPreviewWeights;
    std::vector<float> dragBasePitchSnapshot;
    std::vector<float> dragF0Snapshot;

    // Multi-note drag state
    bool isMultiDragging = false;
    std::vector<Note*> draggedNotes;
    Note* hoveredMultiDragNote = nullptr;
    float multiDragSnapAnchorMidi = 60.0f;
    std::vector<float> originalMidiNotes;
    std::vector<std::vector<float>> originalF0ValuesMulti;
    bool snapToSemitoneDragEnabled = false;
    DragSnapMode dragSnapMode = DragSnapMode::Chromatic;
    int pitchReferenceHz = 440;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchEditor)
};
