#include "PitchEditor.h"
#include "../../Utils/ScaleUtils.h"
#include <algorithm>
#include <cmath>
#include <limits>

PitchEditor::PitchEditor() = default;

Note *PitchEditor::findNoteAt(float x, float y)
{
  if (!project || !coordMapper)
    return nullptr;

  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;

    float noteX = framesToSeconds(note.getStartFrame()) *
                  coordMapper->getPixelsPerSecond();
    float noteW = framesToSeconds(note.getDurationFrames()) *
                  coordMapper->getPixelsPerSecond();
    float noteY = coordMapper->midiToY(note.getAdjustedMidiNote());
    float noteH = coordMapper->getPixelsPerSemitone();

    if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH)
    {
      return &note;
    }
  }

  return nullptr;
}

void PitchEditor::startNoteDrag(Note *note, float y)
{
  if (!note || !project)
    return;

  // Capture delta slice from global dense deltaPitch
  auto &audioData = project->getAudioData();
  int startFrame = note->getStartFrame();
  int endFrame = note->getEndFrame();
  int numFrames = endFrame - startFrame;

  std::vector<float> delta(numFrames, 0.0f);
  for (int i = 0; i < numFrames; ++i)
  {
    int globalFrame = startFrame + i;
    if (globalFrame >= 0 &&
        globalFrame < static_cast<int>(audioData.deltaPitch.size()))
      delta[i] = audioData.deltaPitch[static_cast<size_t>(globalFrame)];
  }
  note->setDeltaPitch(std::move(delta));

  isDragging = true;
  draggedNote = note;
  dragStartY = y;
  originalPitchOffset = note->getPitchOffset();
  originalMidiNote = note->getMidiNote();

  // Save boundary F0 values
  int f0Size = static_cast<int>(audioData.f0.size());
  boundaryF0Start = (startFrame > 0 && startFrame - 1 < f0Size)
                        ? audioData.f0[startFrame - 1]
                        : 0.0f;
  boundaryF0End = (endFrame < f0Size) ? audioData.f0[endFrame] : 0.0f;

  // Save original F0 values for undo
  originalF0Values.clear();
  for (int i = startFrame; i < endFrame && i < f0Size; ++i)
    originalF0Values.push_back(audioData.f0[i]);

  prepareDragBasePreview();

  if (onNoteSelected)
    onNoteSelected(note);
}

void PitchEditor::updateNoteDrag(float y)
{
  if (!isDragging || !draggedNote || !coordMapper)
    return;

  float deltaY = dragStartY - y;
  float deltaSemitones = deltaY / coordMapper->getPixelsPerSemitone();
  if (snapToSemitoneDragEnabled)
    deltaSemitones = std::round(deltaSemitones);

  draggedNote->setPitchOffset(deltaSemitones);
  draggedNote->markDirty();
  applyDragBasePreview(deltaSemitones);
}

void PitchEditor::endNoteDrag()
{
  if (!isDragging || !draggedNote || !project)
    return;

  float newOffset = draggedNote->getPitchOffset();
  constexpr float CHANGE_THRESHOLD = 0.001f;
  bool hasChange = std::abs(newOffset) >= CHANGE_THRESHOLD;

  if (hasChange)
  {
    int startFrame = draggedNote->getStartFrame();
    int endFrame = draggedNote->getEndFrame();
    auto &audioData = project->getAudioData();
    int f0Size = static_cast<int>(audioData.f0.size());

    // Bake pitchOffset into midiNote
    draggedNote->setMidiNote(originalMidiNote + newOffset);
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
        expandedStart = std::min(expandedStart, note.getStartFrame());
      }
      if (note.getStartFrame() < endFrame + 30 &&
          note.getStartFrame() >= endFrame)
      {
        expandedEnd = std::max(expandedEnd, note.getEndFrame());
      }
    }

    // Rebuild pitch curves
    PitchCurveProcessor::rebuildBaseFromNotes(*project);

    if (onBasePitchCacheInvalidated)
      onBasePitchCacheInvalidated();

    // Mark dirty range
    int smoothStart = std::max(0, expandedStart - 60);
    int smoothEnd = std::min(f0Size, expandedEnd + 60);
    project->setF0DirtyRange(smoothStart, smoothEnd);

    // Create undo action
    if (undoManager)
    {
      std::vector<F0FrameEdit> f0Edits;
      for (int i = startFrame; i < endFrame && i < f0Size; ++i)
      {
        int localIdx = i - startFrame;
        F0FrameEdit edit;
        edit.idx = i;
        edit.oldF0 = (localIdx < static_cast<int>(originalF0Values.size()))
                         ? originalF0Values[localIdx]
                         : 0.0f;
        edit.newF0 = audioData.f0[static_cast<size_t>(i)];
        f0Edits.push_back(edit);
      }

      int capturedExpandedStart = expandedStart;
      int capturedExpandedEnd = expandedEnd;
      int capturedF0Size = f0Size;
      auto action = std::make_unique<NotePitchDragAction>(
          draggedNote, &audioData.f0, originalMidiNote,
          originalMidiNote + newOffset, std::move(f0Edits),
          [this, capturedExpandedStart, capturedExpandedEnd,
           capturedF0Size](Note *n)
          {
            if (project)
            {
              PitchCurveProcessor::rebuildBaseFromNotes(*project);
              if (onBasePitchCacheInvalidated)
                onBasePitchCacheInvalidated();
              int smoothStart = std::max(0, capturedExpandedStart - 60);
              int smoothEnd =
                  std::min(capturedF0Size, capturedExpandedEnd + 60);
              project->setF0DirtyRange(smoothStart, smoothEnd);
              if (n)
                n->markSynthDirty();
            }
          });
      undoManager->addAction(std::move(action));
    }

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();
  }
  else
  {
    restoreDragBasePreview();
    draggedNote->setPitchOffset(0.0f);
  }

  isDragging = false;
  draggedNote = nullptr;
  dragPreviewStartFrame = -1;
  dragPreviewEndFrame = -1;
  dragPreviewWeights.clear();
  dragBasePitchSnapshot.clear();
  dragF0Snapshot.clear();
}

void PitchEditor::startDrawing(float x, float y)
{
  isDrawing = true;
  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  drawCurves.clear();
  activeDrawCurve = nullptr;
  lastDrawFrame = -1;
  lastDrawValueCents = 0;

  continueDrawing(x, y);
}

void PitchEditor::continueDrawing(float x, float y)
{
  if (!project || !coordMapper)
    return;

  auto &audioData = project->getAudioData();
  if (audioData.f0.empty())
    return;

  double time = coordMapper->xToTime(x);
  float midi =
      coordMapper->yToMidi(y - coordMapper->getPixelsPerSemitone() * 0.5f);
  int frameIndex = coordMapper->secondsToFrames(static_cast<float>(time));
  int midiCents = static_cast<int>(std::round(midi * 100.0f));

  applyPitchPoint(frameIndex, midiCents);

  if (onPitchEdited)
    onPitchEdited();
}

void PitchEditor::endDrawing()
{
  if (drawingEdits.empty())
  {
    isDrawing = false;
    return;
  }

  // Calculate dirty frame range
  int minFrame = std::numeric_limits<int>::max();
  int maxFrame = std::numeric_limits<int>::min();
  for (const auto &e : drawingEdits)
  {
    minFrame = std::min(minFrame, e.idx);
    maxFrame = std::max(maxFrame, e.idx);
  }

  // Clear deltaPitch for notes in edited range
  if (project && minFrame <= maxFrame)
  {
    const int maxFrameExclusive = maxFrame + 1;
    auto &notes = project->getNotes();
    for (auto &note : notes)
    {
      if (note.getEndFrame() > minFrame &&
          note.getStartFrame() < maxFrameExclusive)
      {
        if (note.hasDeltaPitch())
        {
          note.setDeltaPitch(std::vector<float>());
        }
      }
    }
    project->setF0DirtyRange(minFrame, maxFrameExclusive);
  }

  // Create undo action
  if (undoManager && project)
  {
    auto &audioData = project->getAudioData();
    auto action = std::make_unique<F0EditAction>(
        &audioData.f0, &audioData.deltaPitch, &audioData.voicedMask,
        drawingEdits, [this](int minFrame, int maxFrame)
        {
          if (project) {
            project->setF0DirtyRange(minFrame, maxFrame + 1);
            if (onPitchEditFinished)
              onPitchEditFinished();
          } });
    undoManager->addAction(std::move(action));
  }

  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  activeDrawCurve = nullptr;
  drawCurves.clear();

  isDrawing = false;

  if (onPitchEditFinished)
    onPitchEditFinished();
}

void PitchEditor::applyPitchPoint(int frameIndex, int midiCents)
{
  if (!project)
    return;

  auto &audioData = project->getAudioData();
  if (audioData.f0.empty())
    return;

  const int f0Size = static_cast<int>(audioData.f0.size());
  if (audioData.deltaPitch.size() < audioData.f0.size())
    audioData.deltaPitch.resize(audioData.f0.size(), 0.0f);
  if (audioData.basePitch.size() < audioData.f0.size())
    audioData.basePitch.resize(audioData.f0.size(), 0.0f);
  if (frameIndex < 0 || frameIndex >= f0Size)
    return;

  auto applyFrame = [&](int idx, int cents)
  {
    if (idx < 0 || idx >= f0Size)
      return;

    const float newFreq = midiToFreq(static_cast<float>(cents) / 100.0f);
    const float oldF0 = audioData.f0[idx];
    const float oldDelta = (idx < static_cast<int>(audioData.deltaPitch.size()))
                               ? audioData.deltaPitch[idx]
                               : 0.0f;
    const bool oldVoiced = (idx < static_cast<int>(audioData.voicedMask.size()))
                               ? audioData.voicedMask[idx]
                               : false;

    float baseMidi = (idx < static_cast<int>(audioData.basePitch.size()))
                         ? audioData.basePitch[static_cast<size_t>(idx)]
                         : 0.0f;
    float newMidi = static_cast<float>(cents) / 100.0f;
    float newDelta = newMidi - baseMidi;

    auto it = drawingEditIndexByFrame.find(idx);
    if (it == drawingEditIndexByFrame.end())
    {
      drawingEditIndexByFrame.emplace(idx, drawingEdits.size());
      drawingEdits.push_back(F0FrameEdit{idx, oldF0, newFreq, oldDelta,
                                         newDelta, oldVoiced, true});

      // Clear deltaPitch for notes containing this frame
      auto &notes = project->getNotes();
      for (auto &note : notes)
      {
        if (note.getStartFrame() <= idx && note.getEndFrame() > idx &&
            note.hasDeltaPitch())
        {
          note.setDeltaPitch(std::vector<float>());
          break;
        }
      }
    }
    else
    {
      auto &e = drawingEdits[it->second];
      e.newF0 = newFreq;
      e.newDelta = newDelta;
      e.newVoiced = true;
    }

    audioData.f0[idx] = newFreq;
    if (idx < static_cast<int>(audioData.deltaPitch.size()))
    {
      audioData.deltaPitch[static_cast<size_t>(idx)] = newDelta;
    }
    if (idx < static_cast<int>(audioData.voicedMask.size()))
      audioData.voicedMask[idx] = true;
  };

  // Only start a new curve if there's no active curve (first point of drawing)
  if (!activeDrawCurve)
  {
    startNewPitchCurve(frameIndex, midiCents);
    applyFrame(frameIndex, midiCents);
    return;
  }

  // Helper to append/prepend value to the active curve
  auto appendValue = [&](int idx, int cents)
  {
    if (!activeDrawCurve)
      return;

    const int curveStart = activeDrawCurve->localStart();
    auto &vals = activeDrawCurve->mutableValues();

    // Handle backward drawing: prepend values if idx < curveStart
    if (idx < curveStart)
    {
      const int prependCount = curveStart - idx;
      std::vector<int> newVals(static_cast<size_t>(prependCount), cents);
      newVals.insert(newVals.end(), vals.begin(), vals.end());
      activeDrawCurve->setValues(std::move(newVals));
      activeDrawCurve->setLocalStart(idx);
      return;
    }

    const int offset = idx - curveStart;
    if (offset < static_cast<int>(vals.size()))
    {
      vals[static_cast<std::size_t>(offset)] = cents;
      return;
    }

    while (static_cast<int>(vals.size()) < offset)
    {
      int fill = vals.empty() ? cents : vals.back();
      vals.push_back(fill);
    }
    vals.push_back(cents);
  };

  if (lastDrawFrame < 0)
  {
    appendValue(frameIndex, midiCents);
    applyFrame(frameIndex, midiCents);
  }
  else
  {
    int start = lastDrawFrame;
    int end = frameIndex;
    int startVal = lastDrawValueCents;
    int endVal = midiCents;

    if (start == end)
    {
      appendValue(frameIndex, midiCents);
      applyFrame(frameIndex, midiCents);
    }
    else
    {
      int step = (end > start) ? 1 : -1;
      int length = std::abs(end - start);
      for (int i = 0; i <= length; ++i)
      {
        int idx = start + i * step;
        float t = length == 0
                      ? 0.0f
                      : static_cast<float>(i) / static_cast<float>(length);
        float v = juce::jmap(t, 0.0f, 1.0f, static_cast<float>(startVal),
                             static_cast<float>(endVal));
        int cents = static_cast<int>(std::round(v));
        appendValue(idx, cents);
        applyFrame(idx, cents);
      }
    }
  }

  lastDrawFrame = frameIndex;
  lastDrawValueCents = midiCents;
}

void PitchEditor::startNewPitchCurve(int frameIndex, int midiCents)
{
  drawCurves.push_back(std::make_unique<DrawCurve>(frameIndex, 1));
  activeDrawCurve = drawCurves.back().get();
  activeDrawCurve->appendValue(midiCents);
  lastDrawFrame = frameIndex;
  lastDrawValueCents = midiCents;
}

void PitchEditor::snapNoteToSemitone(Note *note)
{
  if (!note || !project)
    return;

  const float currentOffset = note->getPitchOffset();
  const float adjustedMidi = note->getMidiNote() + currentOffset;
  const float snappedMidi = static_cast<float>(ScaleUtils::snapMidiToScale(
      adjustedMidi, project->getScaleMode(), project->getScaleRootNote(),
      project->getPitchReferenceHz()));
  const float snappedOffset = snappedMidi - note->getMidiNote();

  if (std::abs(snappedOffset - currentOffset) > 0.001f)
  {
    if (undoManager)
    {
      auto action = std::make_unique<NoteFloatPropertyAction>(
          note, currentOffset, snappedOffset,
          &Note::setPitchOffset, "Change Pitch Offset");
      undoManager->addAction(std::move(action));
    }

    note->setPitchOffset(snappedOffset);
    note->markDirty();

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();
  }
}

void PitchEditor::startMultiNoteDrag(const std::vector<Note *> &notes,
                                     float y, Note *hoveredNote)
{
  if (notes.empty() || !project)
    return;

  draggedNotes = notes;
  hoveredMultiDragNote = hoveredNote;
  originalMidiNotes.clear();
  originalF0ValuesMulti.clear();
  dragStartY = y;

  auto &audioData = project->getAudioData();
  int f0Size = static_cast<int>(audioData.f0.size());

  for (auto *note : draggedNotes)
  {
    originalMidiNotes.push_back(note->getMidiNote());

    // Capture delta slice for each note
    int startFrame = note->getStartFrame();
    int endFrame = note->getEndFrame();
    int numFrames = endFrame - startFrame;

    std::vector<float> delta(numFrames, 0.0f);
    for (int i = 0; i < numFrames; ++i)
    {
      int globalFrame = startFrame + i;
      if (globalFrame >= 0 &&
          globalFrame < static_cast<int>(audioData.deltaPitch.size()))
        delta[i] = audioData.deltaPitch[static_cast<size_t>(globalFrame)];
    }
    note->setDeltaPitch(std::move(delta));

    // Save original F0 values
    std::vector<float> f0Values;
    for (int i = startFrame; i < endFrame && i < f0Size; ++i)
      f0Values.push_back(audioData.f0[i]);
    originalF0ValuesMulti.push_back(std::move(f0Values));
  }

  prepareDragBasePreview();

  isMultiDragging = true;
}

void PitchEditor::updateMultiNoteDrag(float x, float y)
{
  if (!isMultiDragging || draggedNotes.empty() || !coordMapper)
    return;

  float deltaY = dragStartY - y;
  float deltaSemitones = deltaY / coordMapper->getPixelsPerSemitone();
  if (snapToSemitoneDragEnabled)
    deltaSemitones = std::round(deltaSemitones);

  for (auto *note : draggedNotes)
  {
    note->setPitchOffset(deltaSemitones);
    note->markDirty();
  }

  hoveredMultiDragNote = findDraggedNoteAt(x, y);
  applyDragBasePreview(deltaSemitones);
}

void PitchEditor::endMultiNoteDrag()
{
  if (!isMultiDragging || draggedNotes.empty() || !project)
  {
    isMultiDragging = false;
    draggedNotes.clear();
    hoveredMultiDragNote = nullptr;
    originalMidiNotes.clear();
    originalF0ValuesMulti.clear();
    return;
  }

  float newOffset = draggedNotes[0]->getPitchOffset();
  constexpr float CHANGE_THRESHOLD = 0.001f;
  bool hasChange = std::abs(newOffset) >= CHANGE_THRESHOLD;

  if (hasChange)
  {
    auto &audioData = project->getAudioData();
    int f0Size = static_cast<int>(audioData.f0.size());

    int expandedStart = std::numeric_limits<int>::max();
    int expandedEnd = std::numeric_limits<int>::min();

    // Bake pitchOffset into midiNote for all notes
    for (size_t i = 0; i < draggedNotes.size(); ++i)
    {
      auto *note = draggedNotes[i];
      note->setMidiNote(originalMidiNotes[i] + newOffset);
      note->setPitchOffset(0.0f);
      note->markSynthDirty();

      expandedStart = std::min(expandedStart, note->getStartFrame());
      expandedEnd = std::max(expandedEnd, note->getEndFrame());
    }

    // Find adjacent notes to expand dirty range
    const auto &allNotes = project->getNotes();
    for (const auto &note : allNotes)
    {
      if (note.getEndFrame() > expandedStart - 30 &&
          note.getEndFrame() <= expandedStart)
        expandedStart = std::min(expandedStart, note.getStartFrame());
      if (note.getStartFrame() < expandedEnd + 30 &&
          note.getStartFrame() >= expandedEnd)
        expandedEnd = std::max(expandedEnd, note.getEndFrame());
    }

    // Rebuild pitch curves
    PitchCurveProcessor::rebuildBaseFromNotes(*project);

    if (onBasePitchCacheInvalidated)
      onBasePitchCacheInvalidated();

    // Mark dirty range
    int smoothStart = std::max(0, expandedStart - 60);
    int smoothEnd = std::min(f0Size, expandedEnd + 60);
    project->setF0DirtyRange(smoothStart, smoothEnd);

    // Create undo action for multi-note drag
    if (undoManager)
    {
      std::vector<F0FrameEdit> f0Edits;
      for (size_t i = 0; i < draggedNotes.size(); ++i)
      {
        auto *note = draggedNotes[i];
        int startFrame = note->getStartFrame();
        int endFrame = note->getEndFrame();
        for (int j = startFrame; j < endFrame && j < f0Size; ++j)
        {
          int localIdx = j - startFrame;
          F0FrameEdit edit;
          edit.idx = j;
          edit.oldF0 =
              (localIdx < static_cast<int>(originalF0ValuesMulti[i].size()))
                  ? originalF0ValuesMulti[i][localIdx]
                  : 0.0f;
          edit.newF0 = audioData.f0[static_cast<size_t>(j)];
          f0Edits.push_back(edit);
        }
      }

      int capturedExpandedStart = expandedStart;
      int capturedExpandedEnd = expandedEnd;
      int capturedF0Size = f0Size;
      std::vector<Note *> capturedNotes = draggedNotes;
      std::vector<float> capturedOriginalMidi = originalMidiNotes;
      float capturedNewOffset = newOffset;

      auto action = std::make_unique<MultiNotePitchDragAction>(
          capturedNotes, &audioData.f0, capturedOriginalMidi, capturedNewOffset,
          std::move(f0Edits),
          [this, capturedExpandedStart, capturedExpandedEnd,
           capturedF0Size](const std::vector<Note *> &)
          {
            if (project)
            {
              PitchCurveProcessor::rebuildBaseFromNotes(*project);
              if (onBasePitchCacheInvalidated)
                onBasePitchCacheInvalidated();
              int smoothStart = std::max(0, capturedExpandedStart - 60);
              int smoothEnd =
                  std::min(capturedF0Size, capturedExpandedEnd + 60);
              project->setF0DirtyRange(smoothStart, smoothEnd);
            }
          });
      undoManager->addAction(std::move(action));
    }

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();
  }
  else
  {
    // No meaningful change: reset pitchOffset
    restoreDragBasePreview();
    for (auto *note : draggedNotes)
      note->setPitchOffset(0.0f);
  }

  isMultiDragging = false;
  draggedNotes.clear();
  hoveredMultiDragNote = nullptr;
  originalMidiNotes.clear();
  originalF0ValuesMulti.clear();
  dragPreviewStartFrame = -1;
  dragPreviewEndFrame = -1;
  dragPreviewWeights.clear();
  dragBasePitchSnapshot.clear();
  dragF0Snapshot.clear();
}

Note *PitchEditor::findDraggedNoteAt(float x, float y) const
{
  if (!coordMapper)
    return nullptr;

  for (auto *note : draggedNotes)
  {
    if (!note || note->isRest())
      continue;

    const float noteX = framesToSeconds(note->getStartFrame()) *
                        coordMapper->getPixelsPerSecond();
    const float noteW = framesToSeconds(note->getDurationFrames()) *
                        coordMapper->getPixelsPerSecond();
    const float noteY = coordMapper->midiToY(note->getAdjustedMidiNote());
    const float noteH = coordMapper->getPixelsPerSemitone();

    if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH)
      return note;
  }

  return nullptr;
}

void PitchEditor::prepareDragBasePreview()
{
  if (!project)
    return;

  auto &audioData = project->getAudioData();
  if (audioData.basePitch.empty() || audioData.f0.empty())
    return;

  std::vector<Note *> selectedNotes = draggedNotes;
  if (selectedNotes.empty() && draggedNote)
    selectedNotes.push_back(draggedNote);

  if (selectedNotes.empty())
    return;

  auto range = computeBasePitchPreviewRange(
      project->getNotes(), static_cast<int>(audioData.basePitch.size()),
      [&selectedNotes](const Note &note)
      {
        return std::find(selectedNotes.begin(), selectedNotes.end(), &note) !=
               selectedNotes.end();
      });

  if (range.startFrame < 0 || range.endFrame <= range.startFrame ||
      range.weights.empty())
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

void PitchEditor::applyDragBasePreview(float pitchOffsetSemitones)
{
  if (std::abs(pitchOffsetSemitones - lastDragPitchOffset) < 0.0001f)
    return;

  lastDragPitchOffset = pitchOffsetSemitones;
  if (!project || dragPreviewStartFrame < 0 ||
      dragPreviewEndFrame <= dragPreviewStartFrame ||
      dragPreviewWeights.empty() || dragBasePitchSnapshot.empty())
    return;

  auto &audioData = project->getAudioData();
  const int count = dragPreviewEndFrame - dragPreviewStartFrame;

  if (audioData.basePitch.size() < static_cast<size_t>(dragPreviewEndFrame))
    return;

  if (audioData.baseF0.size() < audioData.basePitch.size())
    audioData.baseF0.resize(audioData.basePitch.size(), 0.0f);

  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    const float baseMidi =
        dragBasePitchSnapshot[static_cast<size_t>(i)] +
        pitchOffsetSemitones * dragPreviewWeights[static_cast<size_t>(i)];
    audioData.basePitch[static_cast<size_t>(frame)] = baseMidi;
    audioData.baseF0[static_cast<size_t>(frame)] = midiToFreq(baseMidi);

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
      audioData.f0[static_cast<size_t>(frame)] = midiToFreq(baseMidi + deltaMidi);
    }
  }
}

void PitchEditor::restoreDragBasePreview()
{
  if (!project || dragPreviewStartFrame < 0 ||
      dragPreviewEndFrame <= dragPreviewStartFrame ||
      dragBasePitchSnapshot.empty() || dragF0Snapshot.empty())
    return;

  auto &audioData = project->getAudioData();
  const int count = dragPreviewEndFrame - dragPreviewStartFrame;

  if (audioData.basePitch.size() < static_cast<size_t>(dragPreviewEndFrame))
    return;

  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    audioData.basePitch[static_cast<size_t>(frame)] =
        dragBasePitchSnapshot[static_cast<size_t>(i)];
    if (frame < static_cast<int>(audioData.baseF0.size()))
      audioData.baseF0[static_cast<size_t>(frame)] =
          midiToFreq(audioData.basePitch[static_cast<size_t>(frame)]);
    audioData.f0[static_cast<size_t>(frame)] =
        dragF0Snapshot[static_cast<size_t>(i)];
  }
  lastDragPitchOffset = 0.0f;
}
