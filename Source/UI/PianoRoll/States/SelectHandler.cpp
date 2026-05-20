#include "SelectHandler.h"
#include "../../PianoRollComponent.h"
#include "../../../Utils/PitchCurveProcessor.h"
#include "../../../Utils/ScaleUtils.h"
#include "../../../Utils/BasePitchPreview.h"
#include "../../../Utils/TransformParams.h"
#include "../../../Undo/PitchToolAction.h"

#include <unordered_set>

SelectHandler::SelectHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool SelectHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                              float worldY)
{
  auto *project = owner_.project;
  if (!project)
    return false;

  // Pitch tool controller interaction
  if (owner_.pitchToolController && owner_.pitchToolHandles)
  {
    juce::MouseEvent adjustedEvent =
        e.withNewPosition(juce::Point<float>(worldX, worldY));
    if (owner_.pitchToolController->mouseDown(
            adjustedEvent, *owner_.pitchToolHandles,
            owner_.getSelectedNotes(), *owner_.coordMapper))
    {
      return true;
    }
  }

  // Check if clicking on a note
  Note *note = owner_.findNoteAt(worldX, worldY);

  if (note)
  {
    // Check if clicking on an already selected note (for multi-note drag)
    auto selectedNotes = project->getSelectedNotes();
    bool clickedOnSelected =
        note->isSelected() && selectedNotes.size() > 1;

    if (clickedOnSelected)
    {
      // Start multi-note drag
      owner_.pitchEditor->startMultiNoteDrag(selectedNotes, worldY, note);
    }
    else
    {
      // Single note selection and drag
      project->deselectAllNotes();
      note->setSelected(true);
      owner_.updatePitchToolHandlesFromSelection();

      if (owner_.onNoteSelected)
        owner_.onNoteSelected(note);

      // Capture delta slice from global dense deltaPitch for this note
      auto &audioData = project->getAudioData();
      int startFrame = note->getStartFrame();
      int endFrame = note->getEndFrame();
      int numFrames = endFrame - startFrame;

      std::vector<float> delta(numFrames, 0.0f);
      for (int i = 0; i < numFrames; ++i)
      {
        int globalFrame = startFrame + i;
        if (globalFrame >= 0 &&
            globalFrame <
                static_cast<int>(audioData.deltaPitch.size()))
          delta[i] =
              audioData.deltaPitch[static_cast<size_t>(globalFrame)];
      }
      note->setDeltaPitch(std::move(delta));

      // Start single note dragging
      isDragging = true;
      draggedNote = note;
      dragStartY = worldY;
      originalPitchOffset = note->getPitchOffset();
      originalMidiNote = note->getMidiNote();

      // Save boundary F0 values and original F0 for undo
      int f0Size = static_cast<int>(audioData.f0.size());

      boundaryF0Start = (startFrame > 0 && startFrame - 1 < f0Size)
                            ? audioData.f0[startFrame - 1]
                            : 0.0f;
      boundaryF0End =
          (endFrame < f0Size) ? audioData.f0[endFrame] : 0.0f;

      // Save original F0 values for undo
      originalF0Values.clear();
      for (int i = startFrame; i < endFrame && i < f0Size; ++i)
        originalF0Values.push_back(audioData.f0[i]);

      prepareDragBasePreview();
    }

    owner_.repaint();
  }
  else
  {
    // Clicked on empty area - start box selection
    project->deselectAllNotes();
    owner_.updatePitchToolHandlesFromSelection();
    owner_.boxSelector->startSelection(worldX, worldY);
    owner_.repaint();
  }

  return true;
}

bool SelectHandler::mouseDrag(const juce::MouseEvent &e, float worldX,
                              float worldY)
{
  auto *project = owner_.project;
  if (!project)
    return false;

  const auto now = juce::Time::currentTimeMillis();
  const bool shouldRepaint =
      (now - owner_.lastDragRepaintTime) >=
      PianoRollComponent::minDragRepaintInterval;

  // Pitch tool drag
  if (owner_.pitchToolController &&
      owner_.pitchToolController->isDragging())
  {
    juce::MouseEvent adjustedEvent =
        e.withNewPosition(juce::Point<float>(worldX, worldY));
    auto selectedNotes = owner_.getSelectedNotes();
    if (owner_.pitchToolController->mouseDrag(
            adjustedEvent, selectedNotes, *owner_.coordMapper))
    {
      owner_.updatePitchToolHandlesFromSelection();
      if (owner_.onPitchEdited)
        owner_.onPitchEdited();
      if (shouldRepaint)
      {
        owner_.repaint();
        owner_.lastDragRepaintTime = now;
      }
      return true;
    }
  }

  // Delta scale drag
  if (isDeltaScaleDragging)
  {
    float deltaY = deltaScaleDragStartY - worldY;
    float newFactor =
        juce::jlimit(0.0f, 4.0f, 1.0f + deltaY * 0.01f);

    if (std::abs(newFactor - deltaScaleFactor) > 0.0001f)
    {
      deltaScaleFactor = newFactor;
      auto &audioData = project->getAudioData();

      for (auto &edit : deltaScaleEdits)
      {
        if (edit.idx < 0 ||
            edit.idx >=
                static_cast<int>(audioData.deltaPitch.size()))
          continue;

        const float newDelta = edit.oldDelta * deltaScaleFactor;
        edit.newDelta = newDelta;
        audioData.deltaPitch[static_cast<size_t>(edit.idx)] =
            newDelta;

        float newF0 = edit.oldF0;
        if (!edit.oldVoiced)
        {
          newF0 = 0.0f;
        }
        else
        {
          float baseMidi = 0.0f;
          bool hasBase = false;
          if (edit.idx >= 0 &&
              edit.idx <
                  static_cast<int>(audioData.basePitch.size()))
          {
            baseMidi =
                audioData.basePitch[static_cast<size_t>(edit.idx)];
            hasBase = true;
          }
          else if (edit.oldF0 > 0.0f)
          {
            baseMidi = freqToMidi(edit.oldF0) - edit.oldDelta;
            hasBase = true;
          }
          if (hasBase)
            newF0 = midiToFreq(baseMidi + newDelta);
        }

        if (edit.idx < static_cast<int>(audioData.f0.size()))
          audioData.f0[static_cast<size_t>(edit.idx)] = newF0;
        edit.newF0 = newF0;
      }
    }

    if (shouldRepaint)
    {
      owner_.repaint();
      owner_.lastDragRepaintTime = now;
    }
    return true;
  }

  // Delta offset drag
  if (isDeltaOffsetDragging)
  {
    deltaOffsetSemitones =
        (deltaOffsetDragStartY - worldY) / owner_.pixelsPerSemitone;
    auto &audioData = project->getAudioData();

    for (auto &edit : deltaOffsetEdits)
    {
      if (edit.idx < 0 ||
          edit.idx >=
              static_cast<int>(audioData.deltaPitch.size()))
        continue;

      const float newDelta = edit.oldDelta + deltaOffsetSemitones;
      edit.newDelta = newDelta;
      audioData.deltaPitch[static_cast<size_t>(edit.idx)] = newDelta;

      float newF0 = edit.oldF0;
      if (!edit.oldVoiced)
      {
        newF0 = 0.0f;
      }
      else
      {
        float baseMidi = 0.0f;
        bool hasBase = false;
        if (edit.idx >= 0 &&
            edit.idx <
                static_cast<int>(audioData.basePitch.size()))
        {
          baseMidi =
              audioData.basePitch[static_cast<size_t>(edit.idx)];
          hasBase = true;
        }
        else if (edit.oldF0 > 0.0f)
        {
          baseMidi = freqToMidi(edit.oldF0) - edit.oldDelta;
          hasBase = true;
        }
        if (hasBase)
          newF0 = midiToFreq(baseMidi + newDelta);
      }

      if (edit.idx < static_cast<int>(audioData.f0.size()))
        audioData.f0[static_cast<size_t>(edit.idx)] = newF0;
      edit.newF0 = newF0;
    }

    if (shouldRepaint)
    {
      owner_.repaint();
      owner_.lastDragRepaintTime = now;
    }
    return true;
  }

  // Box selection
  if (owner_.boxSelector->isSelecting())
  {
    owner_.boxSelector->updateSelection(worldX, worldY);
    if (shouldRepaint)
    {
      owner_.repaint();
      owner_.lastDragRepaintTime = now;
    }
    return true;
  }

  // Multi-note drag
  if (owner_.pitchEditor->isDraggingMultiNotes())
  {
    owner_.pitchEditor->updateMultiNoteDrag(worldX, worldY);
    owner_.updatePitchToolHandlesFromSelection();
    if (shouldRepaint)
    {
      owner_.repaint();
      owner_.lastDragRepaintTime = now;
    }
    return true;
  }

  // Single note drag
  if (isDragging && draggedNote)
  {
    float deltaY = dragStartY - worldY;
    float deltaSemitones = deltaY / owner_.pixelsPerSemitone;
    if (owner_.snapToSemitoneDrag)
    {
      const float targetMidi = originalMidiNote + deltaSemitones;
      const float snappedMidi = ScaleUtils::snapMidiToSemitone(
          targetMidi, owner_.pitchReferenceHz);
      deltaSemitones = snappedMidi - originalMidiNote;
    }

    draggedNote->setPitchOffset(deltaSemitones);
    draggedNote->markDirty();
    applyDragBasePreview(deltaSemitones);

    // Update handle positions to follow notes during drag
    owner_.updatePitchToolHandlesFromSelection();

    if (shouldRepaint)
    {
      owner_.repaint();
      owner_.lastDragRepaintTime = now;
    }
    return true;
  }

  return false;
}

bool SelectHandler::mouseUp(const juce::MouseEvent &e, float worldX,
                            float worldY)
{
  auto *project = owner_.project;
  if (!project)
    return false;

  // Pitch tool mouseUp
  if (owner_.pitchToolController &&
      owner_.pitchToolController->isDragging())
  {
    auto *ownerPtr = &owner_;
    auto onRangeChanged = [ownerPtr](int startFrame, int endFrame)
    {
      if (ownerPtr->onReinterpolateUV)
        ownerPtr->onReinterpolateUV(startFrame, endFrame);
    };
    owner_.pitchToolController->mouseUp(e, owner_.undoManager,
                                        onRangeChanged);
    owner_.updatePitchToolHandlesFromSelection();
    if (owner_.onPitchEdited)
      owner_.onPitchEdited();
    if (owner_.onPitchEditFinished)
      owner_.onPitchEditFinished();
    owner_.repaint();
    return true;
  }

  // Delta scale commit
  if (isDeltaScaleDragging)
  {
    const bool hasChange =
        std::abs(deltaScaleFactor - 1.0f) >= 0.001f;
    auto &audioData = project->getAudioData();

    if (!hasChange)
    {
      // No meaningful change: restore global arrays from captured old values
      for (const auto &edit : deltaScaleEdits)
      {
        if (edit.idx >= 0 &&
            edit.idx <
                static_cast<int>(audioData.deltaPitch.size()))
          audioData.deltaPitch[static_cast<size_t>(edit.idx)] =
              edit.oldDelta;
        if (edit.idx >= 0 &&
            edit.idx < static_cast<int>(audioData.f0.size()))
          audioData.f0[static_cast<size_t>(edit.idx)] = edit.oldF0;
      }
    }
    else if (!deltaScaleTargetNotes.empty())
    {
      // Capture old per-note params BEFORE updating
      std::vector<TransformParams> oldParams;
      oldParams.reserve(deltaScaleTargetNotes.size());
      for (auto *note : deltaScaleTargetNotes)
        oldParams.push_back(note ? TransformParams::fromNote(*note) : TransformParams{});

      // Update per-note deltaScale/deltaOffset
      for (auto *note : deltaScaleTargetNotes)
      {
        if (!note)
          continue;
        note->setDeltaScale(note->getDeltaScale() * deltaScaleFactor);
        note->setDeltaOffset(note->getDeltaOffset() * deltaScaleFactor);
        note->markDirty();
      }

      // Rebuild global arrays from per-note data (ensures consistency)
      PitchCurveProcessor::rebuildBaseFromNotes(*project);

      // Capture new per-note params AFTER updating
      std::vector<TransformParams> newParams;
      newParams.reserve(deltaScaleTargetNotes.size());
      for (auto *note : deltaScaleTargetNotes)
        newParams.push_back(note ? TransformParams::fromNote(*note) : TransformParams{});

      // Set dirty range for synthesis
      const int f0Size = static_cast<int>(audioData.f0.size());
      const int smoothStart =
          std::max(0, deltaScaleMinFrame - 60);
      const int smoothEnd =
          std::min(f0Size, deltaScaleMaxFrame + 60);
      project->setF0DirtyRange(smoothStart, smoothEnd);

      // Create undo action using PitchToolAction (saves per-note params)
      if (owner_.undoManager)
      {
        auto *ownerPtr = &owner_;
        auto onRangeChanged = [ownerPtr](int startFrame, int endFrame)
        {
          if (ownerPtr->onPitchEditFinished)
            ownerPtr->onPitchEditFinished();
        };
        auto action = std::make_unique<PitchToolAction>(
            project, deltaScaleTargetNotes, oldParams, newParams,
            onRangeChanged);
        owner_.undoManager->addAction(std::move(action));
      }

      if (owner_.onPitchEdited)
        owner_.onPitchEdited();
      if (owner_.onPitchEditFinished)
        owner_.onPitchEditFinished();
    }

    isDeltaScaleDragging = false;
    deltaScaleDragStartY = 0.0f;
    deltaScaleFactor = 1.0f;
    deltaScaleMinFrame = std::numeric_limits<int>::max();
    deltaScaleMaxFrame = std::numeric_limits<int>::min();
    deltaScaleTargetNotes.clear();
    deltaScaleEdits.clear();
    owner_.repaint();
    return true;
  }

  // Delta offset commit
  if (isDeltaOffsetDragging)
  {
    const bool hasChange =
        std::abs(deltaOffsetSemitones) >= 0.001f;
    auto &audioData = project->getAudioData();

    if (!hasChange)
    {
      // No meaningful change: restore global arrays from captured old values
      for (const auto &edit : deltaOffsetEdits)
      {
        if (edit.idx >= 0 &&
            edit.idx <
                static_cast<int>(audioData.deltaPitch.size()))
          audioData.deltaPitch[static_cast<size_t>(edit.idx)] =
              edit.oldDelta;
        if (edit.idx >= 0 &&
            edit.idx < static_cast<int>(audioData.f0.size()))
          audioData.f0[static_cast<size_t>(edit.idx)] = edit.oldF0;
      }
    }
    else if (!deltaOffsetTargetNotes.empty())
    {
      // Capture old per-note params BEFORE updating
      std::vector<TransformParams> oldParams;
      oldParams.reserve(deltaOffsetTargetNotes.size());
      for (auto *note : deltaOffsetTargetNotes)
        oldParams.push_back(note ? TransformParams::fromNote(*note) : TransformParams{});

      // Update per-note deltaOffset
      for (auto *note : deltaOffsetTargetNotes)
      {
        if (!note)
          continue;
        note->setDeltaOffset(note->getDeltaOffset() + deltaOffsetSemitones);
        note->markDirty();
      }

      // Rebuild global arrays from per-note data (ensures consistency)
      PitchCurveProcessor::rebuildBaseFromNotes(*project);

      // Capture new per-note params AFTER updating
      std::vector<TransformParams> newParams;
      newParams.reserve(deltaOffsetTargetNotes.size());
      for (auto *note : deltaOffsetTargetNotes)
        newParams.push_back(note ? TransformParams::fromNote(*note) : TransformParams{});

      // Set dirty range for synthesis
      const int f0Size = static_cast<int>(audioData.f0.size());
      const int smoothStart =
          std::max(0, deltaOffsetMinFrame - 60);
      const int smoothEnd =
          std::min(f0Size, deltaOffsetMaxFrame + 60);
      project->setF0DirtyRange(smoothStart, smoothEnd);

      // Create undo action using PitchToolAction (saves per-note params)
      if (owner_.undoManager)
      {
        auto *ownerPtr = &owner_;
        auto onRangeChanged = [ownerPtr](int startFrame, int endFrame)
        {
          if (ownerPtr->onPitchEditFinished)
            ownerPtr->onPitchEditFinished();
        };
        auto action = std::make_unique<PitchToolAction>(
            project, deltaOffsetTargetNotes, oldParams, newParams,
            onRangeChanged);
        owner_.undoManager->addAction(std::move(action));
      }

      if (owner_.onPitchEdited)
        owner_.onPitchEdited();
      if (owner_.onPitchEditFinished)
        owner_.onPitchEditFinished();
    }

    isDeltaOffsetDragging = false;
    deltaOffsetDragStartY = 0.0f;
    deltaOffsetSemitones = 0.0f;
    deltaOffsetMinFrame = std::numeric_limits<int>::max();
    deltaOffsetMaxFrame = std::numeric_limits<int>::min();
    deltaOffsetTargetNotes.clear();
    deltaOffsetEdits.clear();
    owner_.repaint();
    return true;
  }

  // Box selection end
  if (owner_.boxSelector->isSelecting())
  {
    auto notesInRect = owner_.boxSelector->getNotesInRect(
        project, owner_.coordMapper.get());
    for (auto *note : notesInRect)
    {
      note->setSelected(true);
    }
    owner_.boxSelector->endSelection();
    owner_.updatePitchToolHandlesFromSelection();
    owner_.repaint();
    return true;
  }

  // Multi-note drag end
  if (owner_.pitchEditor->isDraggingMultiNotes())
  {
    owner_.pitchEditor->endMultiNoteDrag();
    owner_.repaint();
    return true;
  }

  // Single note drag end
  if (isDragging && draggedNote)
  {
    float newOffset = draggedNote->getPitchOffset();
    if (owner_.snapToSemitoneDrag)
    {
      const float snappedMidi = ScaleUtils::snapMidiToSemitone(
          originalMidiNote + newOffset, owner_.pitchReferenceHz);
      newOffset = snappedMidi - originalMidiNote;
      draggedNote->setPitchOffset(newOffset);
    }

    // Check if there was any meaningful change
    constexpr float CHANGE_THRESHOLD = 0.001f;
    bool hasChange = std::abs(newOffset) >= CHANGE_THRESHOLD;

    if (hasChange)
    {
      int startFrame = draggedNote->getStartFrame();
      int endFrame = draggedNote->getEndFrame();
      auto &audioData = project->getAudioData();
      int f0Size = static_cast<int>(audioData.f0.size());

      // Update note's midiNote with final offset
      const float finalMidiNote = originalMidiNote + newOffset;
      draggedNote->setMidiNote(finalMidiNote);
      draggedNote->setPitchOffset(0.0f);
      draggedNote->markSynthDirty();

      // Find adjacent notes to expand dirty range
      const auto &notes = project->getNotes();
      int expandedStart = startFrame;
      int expandedEnd = endFrame;
      for (const auto &note : notes)
      {
        if (&note == draggedNote)
          continue;
        if (note.getEndFrame() > startFrame - 30 &&
            note.getEndFrame() <= startFrame)
        {
          expandedStart =
              std::min(expandedStart, note.getStartFrame());
        }
        if (note.getStartFrame() < endFrame + 30 &&
            note.getStartFrame() >= endFrame)
        {
          expandedEnd =
              std::max(expandedEnd, note.getEndFrame());
        }
      }

      // Rebuild base pitch curve and F0
      PitchCurveProcessor::rebuildBaseFromNotes(*project);
      owner_.invalidateBasePitchCache();

      // Mark dirty range for synthesis
      int smoothStart = std::max(0, expandedStart - 60);
      int smoothEnd = std::min(f0Size, expandedEnd + 60);
      project->setF0DirtyRange(smoothStart, smoothEnd);

      // Create undo action
      if (owner_.undoManager)
      {
        std::vector<F0FrameEdit> f0Edits;
        for (int i = startFrame; i < endFrame && i < f0Size; ++i)
        {
          int localIdx = i - startFrame;
          F0FrameEdit edit;
          edit.idx = i;
          edit.oldF0 =
              (localIdx <
               static_cast<int>(originalF0Values.size()))
                  ? originalF0Values[localIdx]
                  : 0.0f;
          edit.newF0 = audioData.f0[static_cast<size_t>(i)];
          f0Edits.push_back(edit);
        }
        int capturedExpandedStart = expandedStart;
        int capturedExpandedEnd = expandedEnd;
        int capturedF0Size = f0Size;
        auto *ownerPtr = &owner_;
        auto action = std::make_unique<NotePitchDragAction>(
            draggedNote, &audioData.f0, originalMidiNote,
            finalMidiNote, std::move(f0Edits),
            [ownerPtr, capturedExpandedStart, capturedExpandedEnd,
             capturedF0Size](Note *n)
            {
              if (ownerPtr->project)
              {
                PitchCurveProcessor::rebuildBaseFromNotes(
                    *ownerPtr->project);
                ownerPtr->invalidateBasePitchCache();
                int smoothStart =
                    std::max(0, capturedExpandedStart - 60);
                int smoothEnd = std::min(capturedF0Size,
                                         capturedExpandedEnd + 60);
                ownerPtr->project->setF0DirtyRange(smoothStart,
                                                   smoothEnd);
                if (n)
                {
                  n->markSynthDirty();
                }
              }
            });
        owner_.undoManager->addAction(std::move(action));
      }

      if (owner_.onPitchEdited)
        owner_.onPitchEdited();
      owner_.repaint();
      if (owner_.onPitchEditFinished)
        owner_.onPitchEditFinished();
    }
    else
    {
      // No meaningful change: reset and repaint
      restoreDragBasePreview();
      draggedNote->setPitchOffset(0.0f);
      owner_.repaint();
    }
  }

  isDragging = false;
  draggedNote = nullptr;
  dragPreviewStartFrame = -1;
  dragPreviewEndFrame = -1;
  dragPreviewWeights.clear();
  dragBasePitchSnapshot.clear();
  dragF0Snapshot.clear();
  return true;
}

void SelectHandler::mouseMove(const juce::MouseEvent &e, float worldX,
                              float worldY)
{
  // Pitch tool handle hover
  if (owner_.pitchToolHandles && !owner_.pitchToolHandles->isEmpty() &&
      e.y >= PianoRollComponent::headerHeight &&
      e.x >= PianoRollComponent::pianoKeysWidth)
  {
    int hitIndex = owner_.pitchToolHandles->hitTest(e.position.x,
                                                    e.position.y);
    if (hitIndex != owner_.hoveredPitchToolHandle)
    {
      owner_.hoveredPitchToolHandle = hitIndex;
      owner_.pitchToolHandles->setHoveredHandleIndex(hitIndex);
      owner_.repaint();
    }
  }
  else if (owner_.hoveredPitchToolHandle != -1)
  {
    owner_.hoveredPitchToolHandle = -1;
    if (owner_.pitchToolHandles)
      owner_.pitchToolHandles->setHoveredHandleIndex(-1);
    owner_.repaint();
  }
}

void SelectHandler::mouseDoubleClick(const juce::MouseEvent &e,
                                     float worldX, float worldY)
{
  auto *project = owner_.project;
  if (!project)
    return;

  // Check if double-clicking on a pitch tool handle
  if (owner_.pitchToolHandles &&
      !project->getSelectedNotes().empty())
  {
    int hitIndex =
        owner_.pitchToolHandles->hitTest(worldX, worldY);
    if (hitIndex >= 0)
    {
      const auto &handle =
          owner_.pitchToolHandles->getHandle(hitIndex);

      // SmoothLeft/SmoothRight: Toggle smoothing (moved from mouseMove bug fix)
      if (handle.type ==
              PitchToolHandles::HandleType::SmoothLeft ||
          handle.type ==
              PitchToolHandles::HandleType::SmoothRight)
      {
        auto selectedNotes = project->getSelectedNotes();
        if (!selectedNotes.empty())
        {

          // Capture old params
          std::vector<TransformParams> oldParams;
          oldParams.reserve(selectedNotes.size());
          for (auto *note : selectedNotes)
          {
            if (note)
              oldParams.push_back(TransformParams::fromNote(*note));
            else
              oldParams.emplace_back();
          }

          // Apply toggle
          for (auto *note : selectedNotes)
          {
            if (!note)
              continue;
            if (handle.type ==
                PitchToolHandles::HandleType::SmoothLeft)
            {
              int current = note->getSmoothLeftFrames();
              note->setSmoothLeftFrames(
                  current != 0 ? 0 : 1);
            }
            else
            {
              int current = note->getSmoothRightFrames();
              note->setSmoothRightFrames(
                  current != 0 ? 0 : 1);
            }
            note->markDirty();
          }

          // Capture new params
          std::vector<TransformParams> newParams;
          newParams.reserve(selectedNotes.size());
          for (auto *note : selectedNotes)
          {
            if (note)
              newParams.push_back(TransformParams::fromNote(*note));
            else
              newParams.emplace_back();
          }

          // Register undo
          if (owner_.undoManager)
          {
            auto action = std::make_unique<PitchToolAction>(
                project, selectedNotes, oldParams, newParams,
                [this](int, int)
                { rebuildAndNotify(); });
            owner_.undoManager->addAction(std::move(action));
          }

          // Rebuild and update
          PitchCurveProcessor::rebuildBaseFromNotes(*project);

          // Mark dirty range
          int minFrame = std::numeric_limits<int>::max();
          int maxFrame = std::numeric_limits<int>::min();
          for (const auto *note : selectedNotes)
          {
            if (note)
            {
              minFrame =
                  std::min(minFrame, note->getStartFrame());
              maxFrame =
                  std::max(maxFrame, note->getEndFrame());
            }
          }
          if (minFrame <= maxFrame)
            project->setF0DirtyRange(minFrame, maxFrame);

          owner_.updatePitchToolHandlesFromSelection();
          if (owner_.onPitchEdited)
            owner_.onPitchEdited();
          if (owner_.onPitchEditFinished)
            owner_.onPitchEditFinished();
          owner_.repaint();
          return;
        }
      }

      // ReduceVariance: Toggle variance scale between 0 and 1
      if (handle.type ==
          PitchToolHandles::HandleType::ReduceVariance)
      {
        auto selectedNotes = project->getSelectedNotes();

        float currentScale =
            selectedNotes[0]->getVarianceScale();
        float newScale =
            (std::abs(currentScale - 1.0f) < 0.001f) ? 0.0f : 1.0f;

        if (owner_.undoManager)
        {
          std::vector<float> oldScales;
          std::vector<float> newScales;
          oldScales.reserve(selectedNotes.size());
          newScales.reserve(selectedNotes.size());

          for (auto *note : selectedNotes)
          {
            if (note)
            {
              oldScales.push_back(note->getVarianceScale());
              newScales.push_back(newScale);
            }
          }

          auto action = std::make_unique<MultiNoteFloatPropertyAction>(
              selectedNotes, oldScales, newScales,
              &Note::setVarianceScale, "Toggle Variance Scale",
              [this]()
              { rebuildAndNotify(); });
          owner_.undoManager->addAction(std::move(action));
        }

        for (auto *note : selectedNotes)
        {
          if (note)
          {
            note->setVarianceScale(newScale);
            note->markDirty();
          }
        }

        rebuildAndNotify();
        return;
      }

      // TiltLeft: Reset tiltLeft to 0
      if (handle.type ==
          PitchToolHandles::HandleType::TiltLeft)
      {
        auto selectedNotes = project->getSelectedNotes();

        if (owner_.undoManager)
        {
          std::vector<float> oldTilts;
          std::vector<float> oldMidiNotes;
          oldTilts.reserve(selectedNotes.size());
          oldMidiNotes.reserve(selectedNotes.size());

          for (auto *note : selectedNotes)
          {
            if (note)
            {
              oldTilts.push_back(note->getTiltLeft());
              oldMidiNotes.push_back(note->getMidiNote());
            }
          }

          auto action = std::make_unique<TiltResetAction>(
              selectedNotes, TiltResetAction::TiltSide::Left,
              oldTilts, oldMidiNotes,
              [this]()
              { rebuildAndNotify(); });
          owner_.undoManager->addAction(std::move(action));
        }

        for (auto *note : selectedNotes)
        {
          if (note)
          {
            const float oldTiltMean =
                (note->getTiltLeft() + note->getTiltRight()) /
                2.0f;
            const float baseline =
                note->getMidiNote() - oldTiltMean;
            note->setTiltLeft(0.0f);
            const float newTiltMean =
                (note->getTiltLeft() + note->getTiltRight()) /
                2.0f;
            note->setMidiNote(baseline + newTiltMean);
            note->markDirty();
          }
        }

        rebuildAndNotify();
        return;
      }

      // TiltRight: Reset tiltRight to 0
      if (handle.type ==
          PitchToolHandles::HandleType::TiltRight)
      {
        auto selectedNotes = project->getSelectedNotes();

        if (owner_.undoManager)
        {
          std::vector<float> oldTilts;
          std::vector<float> oldMidiNotes;
          oldTilts.reserve(selectedNotes.size());
          oldMidiNotes.reserve(selectedNotes.size());

          for (auto *note : selectedNotes)
          {
            if (note)
            {
              oldTilts.push_back(note->getTiltRight());
              oldMidiNotes.push_back(note->getMidiNote());
            }
          }

          auto action = std::make_unique<TiltResetAction>(
              selectedNotes, TiltResetAction::TiltSide::Right,
              oldTilts, oldMidiNotes,
              [this]()
              { rebuildAndNotify(); });
          owner_.undoManager->addAction(std::move(action));
        }

        for (auto *note : selectedNotes)
        {
          if (note)
          {
            const float oldTiltMean =
                (note->getTiltLeft() + note->getTiltRight()) /
                2.0f;
            const float baseline =
                note->getMidiNote() - oldTiltMean;
            note->setTiltRight(0.0f);
            const float newTiltMean =
                (note->getTiltLeft() + note->getTiltRight()) /
                2.0f;
            note->setMidiNote(baseline + newTiltMean);
            note->markDirty();
          }
        }

        rebuildAndNotify();
        return;
      }
    }
  }

  // Check if double-clicking on a note
  Note *note = owner_.findNoteAt(worldX, worldY);

  if (note)
  {
    auto snapForDoubleClick = [&owner_ = owner_](float midi)
    {
      const bool hasActiveScale =
          owner_.selectedScaleMode != ScaleMode::None &&
          owner_.selectedScaleMode != ScaleMode::Chromatic &&
          owner_.selectedScaleRootNote >= 0;

      switch (owner_.doubleClickSnapMode)
      {
      case DoubleClickSnapMode::NearestSemitone:
        return ScaleUtils::snapMidiToSemitone(
            midi, owner_.pitchReferenceHz);
      case DoubleClickSnapMode::NearestScale:
        if (hasActiveScale)
          return ScaleUtils::snapMidiToScale(
              midi, owner_.selectedScaleMode,
              owner_.selectedScaleRootNote,
              owner_.pitchReferenceHz);
        return midi;
      case DoubleClickSnapMode::PitchCenter:
      default:
        if (hasActiveScale)
          return ScaleUtils::snapMidiToScale(
              midi, owner_.selectedScaleMode,
              owner_.selectedScaleRootNote,
              owner_.pitchReferenceHz);
        return ScaleUtils::snapMidiToSemitone(
            midi, owner_.pitchReferenceHz);
      }
    };

    if (note->isSelected())
    {
      auto selectedNotes = project->getSelectedNotes();
      if (selectedNotes.size() > 1)
      {
        std::vector<Note *> notesToSnap;
        std::vector<float> oldMidis;
        std::vector<float> oldOffsets;
        std::vector<float> newMidis;

        notesToSnap.reserve(selectedNotes.size());
        oldMidis.reserve(selectedNotes.size());
        oldOffsets.reserve(selectedNotes.size());
        newMidis.reserve(selectedNotes.size());

        for (auto *selected : selectedNotes)
        {
          if (!selected || selected->isRest())
            continue;

          float oldMidi = selected->getMidiNote();
          float oldOffset = selected->getPitchOffset();
          float adjustedMidi = oldMidi + oldOffset;
          float snappedMidi = snapForDoubleClick(adjustedMidi);

          if (std::abs(snappedMidi - adjustedMidi) <= 0.001f)
            continue;

          notesToSnap.push_back(selected);
          oldMidis.push_back(oldMidi);
          oldOffsets.push_back(oldOffset);
          newMidis.push_back(snappedMidi);
        }

        if (!notesToSnap.empty())
        {
          if (owner_.undoManager)
          {
            auto action =
                std::make_unique<MultiNoteSnapToSemitoneAction>(
                    notesToSnap, oldMidis, oldOffsets, newMidis,
                    [this](const std::vector<Note *> &)
                    {
                      rebuildAndNotify();
                    });
            owner_.undoManager->addAction(std::move(action));
          }

          for (size_t i = 0; i < notesToSnap.size(); ++i)
          {
            notesToSnap[i]->setMidiNote(newMidis[i]);
            notesToSnap[i]->setPitchOffset(0.0f);
            notesToSnap[i]->markDirty();
          }

          rebuildAndNotify();
        }
        return;
      }
    }

    // Snap single note pitch
    float oldMidi = note->getMidiNote();
    float oldOffset = note->getPitchOffset();
    float adjustedMidi = oldMidi + oldOffset;
    float snappedMidi = snapForDoubleClick(adjustedMidi);

    if (std::abs(snappedMidi - adjustedMidi) > 0.001f)
    {
      if (owner_.undoManager)
      {
        auto action =
            std::make_unique<NoteSnapToSemitoneAction>(
                note, oldMidi, oldOffset, snappedMidi,
                [this](Note *)
                { rebuildAndNotify(); });
        owner_.undoManager->addAction(std::move(action));
      }

      note->setMidiNote(snappedMidi);
      note->setPitchOffset(0.0f);
      note->markDirty();
      rebuildAndNotify();
    }
  }
}

bool SelectHandler::isActive() const
{
  return isDragging || isDeltaScaleDragging || isDeltaOffsetDragging ||
         (owner_.pitchToolController &&
          owner_.pitchToolController->isDragging()) ||
         owner_.boxSelector->isSelecting() ||
         owner_.pitchEditor->isDraggingMultiNotes();
}

void SelectHandler::cancel()
{
  if (isDragging && draggedNote)
  {
    restoreDragBasePreview();
    draggedNote->setPitchOffset(0.0f);
    isDragging = false;
    draggedNote = nullptr;
    dragPreviewStartFrame = -1;
    dragPreviewEndFrame = -1;
    dragPreviewWeights.clear();
    dragBasePitchSnapshot.clear();
    dragF0Snapshot.clear();
  }
  if (isDeltaScaleDragging)
  {
    auto *project = owner_.project;
    if (project)
    {
      auto &audioData = project->getAudioData();
      for (const auto &edit : deltaScaleEdits)
      {
        if (edit.idx >= 0 &&
            edit.idx <
                static_cast<int>(audioData.deltaPitch.size()))
          audioData.deltaPitch[static_cast<size_t>(edit.idx)] =
              edit.oldDelta;
        if (edit.idx >= 0 &&
            edit.idx < static_cast<int>(audioData.f0.size()))
          audioData.f0[static_cast<size_t>(edit.idx)] = edit.oldF0;
      }
    }
    isDeltaScaleDragging = false;
    deltaScaleTargetNotes.clear();
    deltaScaleEdits.clear();
  }
  if (isDeltaOffsetDragging)
  {
    auto *project = owner_.project;
    if (project)
    {
      auto &audioData = project->getAudioData();
      for (const auto &edit : deltaOffsetEdits)
      {
        if (edit.idx >= 0 &&
            edit.idx <
                static_cast<int>(audioData.deltaPitch.size()))
          audioData.deltaPitch[static_cast<size_t>(edit.idx)] =
              edit.oldDelta;
        if (edit.idx >= 0 &&
            edit.idx < static_cast<int>(audioData.f0.size()))
          audioData.f0[static_cast<size_t>(edit.idx)] = edit.oldF0;
      }
    }
    isDeltaOffsetDragging = false;
    deltaOffsetTargetNotes.clear();
    deltaOffsetEdits.clear();
  }
  owner_.repaint();
}

// --- Private helpers ---

void SelectHandler::rebuildAndNotify()
{
  PitchCurveProcessor::rebuildBaseFromNotes(*owner_.project);
  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();
  owner_.repaint();
}

bool SelectHandler::initDeltaDrag(
    float worldY,
    bool &isDraggingOut,
    float &dragStartYOut,
    std::vector<Note *> &targetNotesOut,
    std::vector<F0FrameEdit> &editsOut,
    int &minFrameOut,
    int &maxFrameOut)
{

  auto *project = owner_.project;
  if (!project)
    return false;

  isDraggingOut = true;
  dragStartYOut = worldY;
  targetNotesOut.clear();
  editsOut.clear();
  minFrameOut = std::numeric_limits<int>::max();
  maxFrameOut = std::numeric_limits<int>::min();

  auto &audioData = project->getAudioData();
  if (audioData.deltaPitch.size() < audioData.f0.size())
    audioData.deltaPitch.resize(audioData.f0.size(), 0.0f);

  auto selectedNotes = project->getSelectedNotes();
  std::unordered_set<int> seenFrames;
  for (auto *selected : selectedNotes)
  {
    if (!selected || selected->isRest())
      continue;
    targetNotesOut.push_back(selected);

    const int startFrame = std::max(0, selected->getStartFrame());
    const int endFrame = std::min(
        selected->getEndFrame(),
        static_cast<int>(audioData.deltaPitch.size()));
    for (int frame = startFrame; frame < endFrame; ++frame)
    {
      if (!seenFrames.insert(frame).second)
        continue;
      F0FrameEdit edit;
      edit.idx = frame;
      edit.oldDelta =
          audioData.deltaPitch[static_cast<size_t>(frame)];
      if (frame < static_cast<int>(audioData.f0.size()))
        edit.oldF0 = audioData.f0[static_cast<size_t>(frame)];
      if (frame < static_cast<int>(audioData.voicedMask.size()))
        edit.oldVoiced =
            audioData.voicedMask[static_cast<size_t>(frame)];
      else
        edit.oldVoiced = true;
      edit.newVoiced = edit.oldVoiced;
      editsOut.push_back(edit);
      minFrameOut = std::min(minFrameOut, frame);
      maxFrameOut = std::max(maxFrameOut, frame);
    }
  }

  if (editsOut.empty())
  {
    isDraggingOut = false;
    targetNotesOut.clear();
    return false;
  }

  return true;
}

void SelectHandler::prepareDragBasePreview()
{
  auto *project = owner_.project;
  if (!project || !draggedNote)
    return;

  auto &audioData = project->getAudioData();
  if (audioData.basePitch.empty() || audioData.f0.empty())
    return;

  auto range = computeBasePitchPreviewRange(
      project->getNotes(),
      static_cast<int>(audioData.basePitch.size()),
      [this](const Note &note)
      { return &note == draggedNote; });

  if (range.startFrame < 0 ||
      range.endFrame <= range.startFrame || range.weights.empty())
  {
    return;
  }

  dragPreviewStartFrame = range.startFrame;
  dragPreviewEndFrame = range.endFrame;
  dragPreviewWeights = std::move(range.weights);

  const int count = dragPreviewEndFrame - dragPreviewStartFrame;
  dragBasePitchSnapshot.resize(static_cast<size_t>(count));
  dragF0Snapshot.resize(static_cast<size_t>(count));

  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    dragBasePitchSnapshot[static_cast<size_t>(i)] =
        audioData.basePitch[static_cast<size_t>(frame)];
    dragF0Snapshot[static_cast<size_t>(i)] =
        audioData.f0[static_cast<size_t>(frame)];
  }

  lastDragPitchOffset = 0.0f;
}

void SelectHandler::applyDragBasePreview(float pitchOffsetSemitones)
{
  if (std::abs(pitchOffsetSemitones - lastDragPitchOffset) < 0.0001f)
    return;

  lastDragPitchOffset = pitchOffsetSemitones;
  auto *project = owner_.project;
  if (!project || dragPreviewStartFrame < 0 ||
      dragPreviewEndFrame <= dragPreviewStartFrame ||
      dragPreviewWeights.empty() || dragBasePitchSnapshot.empty())
    return;

  auto &audioData = project->getAudioData();
  const int count = dragPreviewEndFrame - dragPreviewStartFrame;

  if (audioData.basePitch.size() <
      static_cast<size_t>(dragPreviewEndFrame))
    return;

  if (audioData.baseF0.size() < audioData.basePitch.size())
    audioData.baseF0.resize(audioData.basePitch.size(), 0.0f);

  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    const float baseMidi =
        dragBasePitchSnapshot[static_cast<size_t>(i)] +
        pitchOffsetSemitones *
            dragPreviewWeights[static_cast<size_t>(i)];
    audioData.basePitch[static_cast<size_t>(frame)] = baseMidi;
    audioData.baseF0[static_cast<size_t>(frame)] =
        midiToFreq(baseMidi);

    const float deltaMidi =
        (frame < static_cast<int>(audioData.deltaPitch.size()))
            ? audioData.deltaPitch[static_cast<size_t>(frame)]
            : 0.0f;
    if (frame < static_cast<int>(audioData.voicedMask.size()) &&
        !audioData.voicedMask[static_cast<size_t>(frame)])
    {
      audioData.f0[static_cast<size_t>(frame)] = 0.0f;
    }
    else
    {
      audioData.f0[static_cast<size_t>(frame)] =
          midiToFreq(baseMidi + deltaMidi);
    }
  }
}

void SelectHandler::restoreDragBasePreview()
{
  auto *project = owner_.project;
  if (!project || dragPreviewStartFrame < 0 ||
      dragPreviewEndFrame <= dragPreviewStartFrame ||
      dragBasePitchSnapshot.empty() || dragF0Snapshot.empty())
    return;

  auto &audioData = project->getAudioData();
  const int count = dragPreviewEndFrame - dragPreviewStartFrame;
  if (audioData.basePitch.size() <
      static_cast<size_t>(dragPreviewEndFrame))
    return;

  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    audioData.basePitch[static_cast<size_t>(frame)] =
        dragBasePitchSnapshot[static_cast<size_t>(i)];
    if (frame < static_cast<int>(audioData.baseF0.size()))
      audioData.baseF0[static_cast<size_t>(frame)] = midiToFreq(
          audioData.basePitch[static_cast<size_t>(frame)]);
    audioData.f0[static_cast<size_t>(frame)] =
        dragF0Snapshot[static_cast<size_t>(i)];
  }
  lastDragPitchOffset = 0.0f;
}
