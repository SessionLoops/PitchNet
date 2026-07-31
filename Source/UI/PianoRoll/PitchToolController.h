#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Note.h"
#include "../../Models/Project.h"
#include "../../Undo/UndoActions.h"
#include "../../Utils/PitchToolOperations.h"
#include "../../Utils/TransformParams.h"
#include "CoordinateMapper.h"
#include "PitchToolHandles.h"
#include <functional>
#include <memory>
#include <vector>

/**
 * Handles mouse interaction with pitch tool handles.
 */
class PitchToolController {
public:
  PitchToolController();

  /**
   * Handle mouse down on pitch tool handle.
   * @return true if event was handled, false otherwise
   */
  bool mouseDown(const juce::MouseEvent& e,
                 const PitchToolHandles& handles,
                 const std::vector<Note*>& selectedNotes,
                 const CoordinateMapper& mapper);

  /**
   * Handle mouse drag on active handle.
   * @return true if event was handled, false otherwise
   */
  bool mouseDrag(const juce::MouseEvent& e,
                 std::vector<Note*>& selectedNotes,
                 const CoordinateMapper& mapper);

  /**
   * Handle mouse up (commit operation to undo stack).
   * @return true if event was handled, false otherwise
   */
  bool mouseUp(const juce::MouseEvent& e,
               PitchUndoManager* undoManager,
               std::function<void(int, int)> onRangeChanged);

  /**
   * Check if currently dragging a handle.
   */
  bool isDragging() const { return dragging; }
  bool isDraggingVibrato() const
  {
    return dragging && activeHandleType == PitchToolHandles::HandleType::Vibrato;
  }
  bool isDraggingTilt() const
  {
    return dragging &&
           (activeHandleType == PitchToolHandles::HandleType::TiltLeft ||
            activeHandleType == PitchToolHandles::HandleType::TiltRight);
  }
  PitchToolHandles::HandleType getActiveHandleType() const
  {
    return activeHandleType;
  }
  Note* getActiveHandleNote() const { return activeHandleNote; }
  const std::vector<Note*>& getAffectedNotes() const { return affectedNotes; }

  /**
   * Cancel current drag operation.
   */
  void cancel();

  /**
   * Set project reference (required for accessing audioData.deltaPitch).
   */
  void setProject(Project* proj) { project = proj; }

  /**
   * Callback fired when pitch is edited (for triggering repaint).
   */
  std::function<void()> onPitchEdited;

private:
  Project* project = nullptr;
  bool dragging = false;
  PitchToolHandles::HandleType activeHandleType = PitchToolHandles::HandleType::None;
  Note* activeHandleNote = nullptr;
  std::vector<Note*> affectedNotes;
  std::vector<TransformParams> originalParams;
  juce::Point<float> dragStartPos;

  void applyOperation(std::vector<Note*>& notes,
                      PitchToolHandles::HandleType type,
                      float dragDeltaX,
                      float dragDeltaY,
                      const CoordinateMapper& mapper);
};
