#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Undo/UndoActions.h"
#include "../../Utils/UI/DrawCurve.h"
#include "../../Utils/BasePitchPreview.h"
#include "../../Utils/PitchCurveProcessor.h"
#include "CoordinateMapper.h"
#include <deque>
#include <memory>
#include <unordered_map>
#include <functional>

/**
 * Handles pitch editing operations including note dragging and pitch drawing.
 */
class PitchEditor {
public:
    PitchEditor();
    ~PitchEditor() = default;

    void setProject(Project* proj) { project = proj; }
    void setUndoManager(PitchUndoManager* manager) { undoManager = manager; }
    void setCoordinateMapper(CoordinateMapper* mapper) { coordMapper = mapper; }
    void setSnapToSemitoneDragEnabled(bool enabled) { snapToSemitoneDragEnabled = enabled; }
    void setPitchReferenceHz(int hz) { pitchReferenceHz = juce::jlimit(380, 480, hz); }

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
    void updateMultiNoteDrag(float x, float y);
    void endMultiNoteDrag();
    bool isDraggingMultiNotes() const { return isMultiDragging; }
    const std::vector<Note*>& getDraggedNotes() const { return draggedNotes; }
    Note* getHoveredMultiDragNote() const { return hoveredMultiDragNote; }

    // Pitch drawing
    void startDrawing(float x, float y);
    void continueDrawing(float x, float y);
    void endDrawing();
    bool isDrawingPitch() const { return isDrawing; }

    // Snap note to semitone
    void snapNoteToSemitone(Note* note);

    // Callbacks
    std::function<void(Note*)> onNoteSelected;
    std::function<void()> onPitchEdited;
    std::function<void()> onPitchEditFinished;
    std::function<void()> onBasePitchCacheInvalidated;

private:
    void applyPitchPoint(int frameIndex, int midiCents);
    void startNewPitchCurve(int frameIndex, int midiCents);
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
    std::vector<float> originalMidiNotes;
    std::vector<std::vector<float>> originalF0ValuesMulti;
    bool snapToSemitoneDragEnabled = false;
    int pitchReferenceHz = 440;

    // Draw state
    bool isDrawing = false;
    std::vector<F0FrameEdit> drawingEdits;
    std::unordered_map<int, size_t> drawingEditIndexByFrame;
    int lastDrawFrame = -1;
    int lastDrawValueCents = 0;
    DrawCurve* activeDrawCurve = nullptr;
    std::deque<std::unique_ptr<DrawCurve>> drawCurves;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchEditor)
};
