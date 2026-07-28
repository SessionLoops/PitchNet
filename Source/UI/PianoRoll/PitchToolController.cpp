#include "PitchToolController.h"
#include "../../Utils/PitchCurveProcessor.h"

#include <algorithm>
#include <limits>

PitchToolController::PitchToolController()
{
}

bool PitchToolController::mouseDown(const juce::MouseEvent& e,
                                    const PitchToolHandles& handles,
                                    const std::vector<Note*>& selectedNotes,
                                    const CoordinateMapper& mapper)
{
  juce::ignoreUnused(mapper);


  const int hitIndex = handles.hitTest(e.position.x, e.position.y);
  if (hitIndex < 0) {
    return false;
  }

  activeHandleType = handles.getHandle(hitIndex).type;
  affectedNotes = selectedNotes;
  if (affectedNotes.empty() && handles.getHandle(hitIndex).note)
    affectedNotes.push_back(handles.getHandle(hitIndex).note);
  if (affectedNotes.empty())
    return false;
  
  // Capture original transformation parameters (not curves)
  originalParams.clear();
  originalParams.reserve(affectedNotes.size());
  for (auto* note : affectedNotes)
  {
    if (note)
    {
      auto params = TransformParams::fromNote(*note);
      
      // Store baseline midiNote (without tilt mean shift)
      // This ensures tilt adjustments are absolute, not cumulative
      const float currentTiltMean = (note->getTiltLeft() + note->getTiltRight()) / 2.0f;
      params.midiNote = note->getMidiNote() - currentTiltMean;
      
      originalParams.push_back(params);
    }
    else
    {
      originalParams.emplace_back();
    }
  }

  dragStartPos = e.position;
  dragging = true;
  return true;
}

bool PitchToolController::mouseDrag(const juce::MouseEvent& e,
                                    std::vector<Note*>& selectedNotes,
                                    const CoordinateMapper& mapper)
{
  juce::ignoreUnused(selectedNotes);

  if (!dragging)
    return false;

  const float deltaX = e.position.x - dragStartPos.x;
  const float deltaY = e.position.y - dragStartPos.y;

  applyOperation(affectedNotes, activeHandleType, deltaX, deltaY, mapper);
  return true;
}

bool PitchToolController::mouseUp(const juce::MouseEvent& e,
                                  PitchUndoManager* undoManager,
                                  std::function<void(int, int)> onRangeChanged)
{
  juce::ignoreUnused(e);

  if (!dragging)
    return false;

  // Capture new transformation parameters (not curves)
  std::vector<TransformParams> newParams;
  newParams.reserve(affectedNotes.size());
  for (auto* note : affectedNotes)
  {
    if (note)
      newParams.push_back(TransformParams::fromNote(*note));
    else
      newParams.emplace_back();
  }

  // Fix oldParams to store live midiNote (with tilt mean) for correct undo/redo
  // originalParams stores baseline midiNote (without tilt mean) for applyOperation,
  // but undo/redo needs the live midiNote value
  std::vector<TransformParams> undoOldParams = originalParams;
  for (size_t i = 0; i < undoOldParams.size(); ++i)
  {
    const float tiltMean = (undoOldParams[i].tiltLeft + undoOldParams[i].tiltRight) / 2.0f;
    undoOldParams[i].midiNote += tiltMean;
  }

  auto action = std::make_unique<PitchToolAction>(
      project, affectedNotes, undoOldParams, newParams, onRangeChanged);

  if (undoManager)
    undoManager->addAction(std::move(action));

  if (onRangeChanged)
  {
    for (auto* note : affectedNotes)
    {
      if (note)
        onRangeChanged(note->getStartFrame(), note->getEndFrame());
    }
  }

  dragging = false;
  activeHandleType = PitchToolHandles::HandleType::None;
  affectedNotes.clear();
  originalParams.clear();
  return true;
}

void PitchToolController::applyOperation(std::vector<Note*>& notes,
                                         PitchToolHandles::HandleType type,
                                         float dragDeltaX,
                                         float dragDeltaY,
                                         const CoordinateMapper& mapper)
{
  if (!project)
    return;  // Cannot proceed without project reference

  const float pixelsPerSemitone = juce::jmax(1.0f, mapper.getPixelsPerSemitone());
  const float semitoneDelta = -dragDeltaY / pixelsPerSemitone;

  // Debug logging

  for (size_t i = 0; i < notes.size(); ++i)
  {
    auto* note = notes[i];
    if (!note || i >= originalParams.size())
      continue;

    // Restore original parameters before applying new transformation
    const auto& origParams = originalParams[i];
    origParams.applyToNote(*note);

    // Apply new transformation by updating the appropriate parameter
    switch (type)
    {
      case PitchToolHandles::HandleType::TiltLeft:
      {
        // Drag UP = positive semitoneDelta = left edge goes UP
        const float amount = semitoneDelta;
        note->setTiltLeft(origParams.tiltLeft + amount);
        
        // Calculate tilt mean shift
        const float newTiltMean = (note->getTiltLeft() + note->getTiltRight()) / 2.0f;
        note->setMidiNote(origParams.midiNote + newTiltMean);
        break;
      }
      case PitchToolHandles::HandleType::TiltRight:
      {
        const float amount = semitoneDelta;
        note->setTiltRight(origParams.tiltRight + amount);
        
        // Calculate tilt mean shift
        const float newTiltMean = (note->getTiltLeft() + note->getTiltRight()) / 2.0f;
        note->setMidiNote(origParams.midiNote + newTiltMean);
        break;
      }
      case PitchToolHandles::HandleType::Vibrato:
      {
        // Additive accumulation from original value (consistent with tilt handles)
        // Drag UP increases modulation; drag DOWN decreases it.
        const float dragDelta = -dragDeltaY / 100.0f;
        const float newScale = origParams.vibrato + dragDelta;
        note->setVibrato(newScale);
        
        // Preserve tilt offset when adjusting vibrato.
        const float currentTiltMean = (note->getTiltLeft() + note->getTiltRight()) / 2.0f;
        note->setMidiNote(origParams.midiNote + currentTiltMean);
        break;
      }
      case PitchToolHandles::HandleType::SmoothLeft:
      {
        // Drag RIGHT (positive X) into note = more smoothing
        // 5px per additional frame, range [5, 50]
        const int transitionFrames = juce::jlimit(5, 50, 5 + static_cast<int>(dragDeltaX / 5.0f));
        note->setSmoothLeftFrames(transitionFrames);
        
        // Preserve tilt offset when adjusting smoothing
        const float currentTiltMean = (note->getTiltLeft() + note->getTiltRight()) / 2.0f;
        note->setMidiNote(origParams.midiNote + currentTiltMean);
        break;
      }
      case PitchToolHandles::HandleType::SmoothRight:
      {
        // Drag LEFT (negative X) into note = more smoothing
        // Use -dragDeltaX so LEFT drag increases frames
        const int transitionFrames = juce::jlimit(5, 50, 5 + static_cast<int>(-dragDeltaX / 5.0f));
        note->setSmoothRightFrames(transitionFrames);
        
        // Preserve tilt offset when adjusting smoothing
        const float currentTiltMean = (note->getTiltLeft() + note->getTiltRight()) / 2.0f;
        note->setMidiNote(origParams.midiNote + currentTiltMean);
        break;
      }
      case PitchToolHandles::HandleType::None:
      default:
        continue;  // Skip this note
    }

    note->markDirty();
  }

  // NON-DESTRUCTIVE: Recompose only the affected notes' delta + f0
  // This is much faster than rebuildBaseFromNotes() which rebuilds everything
  PitchCurveProcessor::rebuildDeltaForNotes(*project, notes);
  
  // Mark dirty range for synthesis
  if (!notes.empty())
  {
    int minFrame = std::numeric_limits<int>::max();
    int maxFrame = std::numeric_limits<int>::min();
    for (const auto* note : notes)
    {
      minFrame = std::min(minFrame, note->getStartFrame());
      maxFrame = std::max(maxFrame, note->getEndFrame());
    }
    project->setF0DirtyRange(minFrame, maxFrame);
  }

  // Trigger visual update
  if (onPitchEdited)
    onPitchEdited();
}

void PitchToolController::cancel()
{
  if (!dragging)
    return;

  // Restore original transformation parameters
  for (size_t i = 0; i < affectedNotes.size(); ++i)
  {
    if (i < originalParams.size() && affectedNotes[i])
    {
      const auto& params = originalParams[i];
      params.applyToNote(*affectedNotes[i]);
      
      // Restore midiNote with tilt mean (params.midiNote is baseline without tilt)
      const float tiltMean = (params.tiltLeft + params.tiltRight) / 2.0f;
      affectedNotes[i]->setMidiNote(params.midiNote + tiltMean);
    }
  }

  dragging = false;
  activeHandleType = PitchToolHandles::HandleType::None;
  affectedNotes.clear();
  originalParams.clear();
}
