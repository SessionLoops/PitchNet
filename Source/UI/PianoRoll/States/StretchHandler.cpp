#include "StretchHandler.h"

#if HACHITUNE_ENABLE_STRETCH

#include "../../PianoRollComponent.h"
#include "../../../Utils/UI/Theme.h"
#include "../../../Utils/CurveResampler.h"
#include "../../../Utils/PitchCurveProcessor.h"

StretchHandler::StretchHandler(PianoRollComponent &owner)
    : InteractionHandler(owner)
{
  centeredMelComputer = std::make_unique<CenteredMelSpectrogram>();
}

bool StretchHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                               float worldY)
{
  auto *project = owner_.project;
  if (!project)
    return false;

  int boundaryIndex = findStretchBoundaryIndex(worldX, stretchHandleHitPadding);
  if (boundaryIndex >= 0)
  {
    auto boundaries = collectStretchBoundaries();
    if (boundaryIndex < static_cast<int>(boundaries.size()))
    {
      // Lock the effective mode at drag start (Alt toggles)
      StretchMode effectiveMode =
          owner_.getEffectiveStretchMode(e.mods);
      startStretchDrag(boundaries[static_cast<size_t>(boundaryIndex)],
                       effectiveMode);
      owner_.repaint();
      return true;
    }
  }

  // In stretch mode, allow selecting notes but disable pitch dragging.
  Note *note = owner_.findNoteAt(worldX, worldY);
  if (note)
  {
    project->deselectAllNotes();
    note->setSelected(true);
    owner_.updatePitchToolHandlesFromSelection();
    if (owner_.onNoteSelected)
      owner_.onNoteSelected(note);
    owner_.repaint();
    return true;
  }

  // Box selection fallback
  project->deselectAllNotes();
  owner_.updatePitchToolHandlesFromSelection();
  owner_.boxSelector->startSelection(worldX, worldY);
  owner_.repaint();
  return true;
}

bool StretchHandler::mouseDrag(const juce::MouseEvent &e, float worldX,
                               float worldY)
{
  if (!stretchDrag.active)
    return false;

  auto *project = owner_.project;
  if (!project)
    return false;

  const double time = owner_.xToTime(worldX);
  const int targetFrame =
      static_cast<int>(secondsToFrames(static_cast<float>(time)));
  updateStretchDrag(targetFrame);

  // Use the owner's drag repaint throttling
  const auto now = juce::Time::currentTimeMillis();
  const bool shouldRepaint =
      (now - owner_.lastDragRepaintTime) >=
      PianoRollComponent::minDragRepaintInterval;
  if (shouldRepaint)
  {
    owner_.repaint();
    owner_.lastDragRepaintTime = now;
  }
  return true;
}

bool StretchHandler::mouseUp(const juce::MouseEvent &e, float worldX,
                             float worldY)
{
  if (!stretchDrag.active)
    return false;

  finishStretchDrag();
  owner_.repaint();
  return true;
}

void StretchHandler::mouseMove(const juce::MouseEvent &e, float worldX,
                               float worldY)
{
  if (e.y >= PianoRollComponent::headerHeight &&
      e.x >= PianoRollComponent::pianoKeysWidth)
  {
    float adjustedX =
        e.x - PianoRollComponent::pianoKeysWidth +
        static_cast<float>(owner_.scrollX);
    int boundaryIndex =
        findStretchBoundaryIndex(adjustedX, stretchHandleHitPadding);
    hoveredStretchBoundaryIndex = boundaryIndex;
    if (boundaryIndex >= 0)
    {
      owner_.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    }
    else
    {
      owner_.setMouseCursor(juce::MouseCursor::NormalCursor);
    }
  }
  else
  {
    hoveredStretchBoundaryIndex = -1;
  }
  owner_.repaint();
}

void StretchHandler::draw(juce::Graphics &g)
{
  auto *project = owner_.project;
  if (!project)
    return;

  auto boundaries = collectStretchBoundaries();
  if (boundaries.empty())
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * owner_.pixelsPerSemitone;

  for (size_t i = 0; i < boundaries.size(); ++i)
  {
    int frame = boundaries[i].frame;
    const bool active =
        stretchDrag.active &&
        boundaries[i].left == stretchDrag.boundary.left &&
        boundaries[i].right == stretchDrag.boundary.right;
    if (active)
      frame = stretchDrag.currentBoundary;

    float x = framesToSeconds(frame) * owner_.pixelsPerSecond;

    const bool isHovered = static_cast<int>(i) == hoveredStretchBoundaryIndex;
    float alpha = isHovered || active ? 0.8f : 0.35f;
    float thickness = isHovered || active ? 2.0f : 1.0f;

    g.setColour(APP_COLOR_PRIMARY.withAlpha(alpha));
    g.drawLine(x, 0.0f, x, height, thickness);
  }
}

bool StretchHandler::isActive() const { return stretchDrag.active; }

void StretchHandler::cancel() { cancelStretchDrag(); }

// --- Private implementation ---

std::vector<StretchHandler::StretchBoundary>
StretchHandler::collectStretchBoundaries() const
{
  std::vector<StretchBoundary> boundaries;
  auto *project = owner_.project;
  if (!project)
    return boundaries;

  std::vector<Note *> ordered;
  ordered.reserve(project->getNotes().size());
  for (auto &note : project->getNotes())
  {
    if (!note.isRest())
      ordered.push_back(&note);
  }

  if (ordered.empty())
    return boundaries;

  std::sort(ordered.begin(), ordered.end(),
            [](const Note *a, const Note *b)
            {
              return a->getStartFrame() < b->getStartFrame();
            });

  // Gap threshold: if gap between notes > this, treat them as separate segments
  constexpr int gapThreshold = 3; // frames

  for (size_t i = 0; i < ordered.size(); ++i)
  {
    Note *current = ordered[i];
    Note *prev = (i > 0) ? ordered[i - 1] : nullptr;
    Note *next = (i + 1 < ordered.size()) ? ordered[i + 1] : nullptr;

    // Check if there's a gap before this note
    bool hasGapBefore = true;
    if (prev)
    {
      int gap = current->getStartFrame() - prev->getEndFrame();
      hasGapBefore = (gap > gapThreshold);
    }

    // Check if there's a gap after this note
    bool hasGapAfter = true;
    if (next)
    {
      int gap = next->getStartFrame() - current->getEndFrame();
      hasGapAfter = (gap > gapThreshold);
    }

    // Add left boundary if there's a gap before (or it's the first note)
    if (hasGapBefore)
    {
      boundaries.push_back({nullptr, current, current->getStartFrame()});
    }

    // Add right boundary if there's a gap after (or it's the last note)
    if (hasGapAfter)
    {
      boundaries.push_back({current, nullptr, current->getEndFrame()});
    }

    // Add boundary between adjacent notes (no gap)
    if (next && !hasGapAfter)
    {
      boundaries.push_back({current, next, current->getEndFrame()});
    }
  }

  // Sort boundaries by frame position
  std::sort(boundaries.begin(), boundaries.end(),
            [](const StretchBoundary &a, const StretchBoundary &b)
            {
              return a.frame < b.frame;
            });

  return boundaries;
}

int StretchHandler::findStretchBoundaryIndex(float worldX,
                                             float tolerancePx) const
{
  auto boundaries = collectStretchBoundaries();
  int bestIndex = -1;
  float bestDist = tolerancePx;

  for (size_t i = 0; i < boundaries.size(); ++i)
  {
    float boundaryX =
        framesToSeconds(boundaries[i].frame) * owner_.pixelsPerSecond;
    float dist = std::abs(worldX - boundaryX);
    if (dist <= bestDist)
    {
      bestIndex = static_cast<int>(i);
      bestDist = dist;
    }
  }

  return bestIndex;
}

std::vector<Note *> StretchHandler::collectRippleNotes(int afterFrame) const
{
  auto *project = owner_.project;
  if (!project)
    return {};

  std::vector<Note *> result;
  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;
    // Collect notes whose start is strictly after the boundary
    // (the note being stretched itself is NOT included)
    if (note.getStartFrame() >= afterFrame)
    {
      // Exclude the notes that are part of the boundary
      if (&note != stretchDrag.boundary.left &&
          &note != stretchDrag.boundary.right)
      {
        result.push_back(&note);
      }
    }
  }
  std::sort(result.begin(), result.end(),
            [](const Note *a, const Note *b)
            {
              return a->getStartFrame() < b->getStartFrame();
            });
  return result;
}

void StretchHandler::applyRippleShift(int deltaFrames)
{
  for (auto *note : stretchDrag.rippleNotes)
  {
    // Find this note's original index
    for (size_t i = 0; i < stretchDrag.rippleNotes.size(); ++i)
    {
      if (stretchDrag.rippleNotes[i] == note)
      {
        int newStart = stretchDrag.originalRippleStarts[i] + deltaFrames;
        int newEnd = stretchDrag.originalRippleEnds[i] + deltaFrames;
        // Clamp to prevent negative frames
        newStart = std::max(0, newStart);
        newEnd = std::max(newStart + 1, newEnd);
        note->setStartFrame(newStart);
        note->setEndFrame(newEnd);
        note->markDirty();
        break;
      }
    }
  }
}

void StretchHandler::restoreRippleNotes()
{
  for (size_t i = 0; i < stretchDrag.rippleNotes.size(); ++i)
  {
    stretchDrag.rippleNotes[i]->setStartFrame(
        stretchDrag.originalRippleStarts[i]);
    stretchDrag.rippleNotes[i]->setEndFrame(
        stretchDrag.originalRippleEnds[i]);
    stretchDrag.rippleNotes[i]->markDirty();
  }
}

void StretchHandler::startStretchDrag(const StretchBoundary &boundary,
                                      StretchMode effectiveMode)
{
  auto *project = owner_.project;
  if (!project)
    return;

  // At least one note must exist
  if (!boundary.left && !boundary.right)
    return;

  stretchDrag = {};
  stretchDrag.active = true;
  stretchDrag.boundary = boundary;
  stretchDrag.mode = effectiveMode;

  auto &audioData = project->getAudioData();
  const int totalFrames = static_cast<int>(audioData.f0.size());
  if (totalFrames <= 0)
  {
    stretchDrag.active = false;
    return;
  }

  // Determine the boundary frame and limits based on which notes exist
  if (boundary.left && boundary.right)
  {
    // Both notes exist - stretch boundary between them
    stretchDrag.originalBoundary = boundary.left->getEndFrame();
    stretchDrag.originalLeftStart = boundary.left->getStartFrame();
    stretchDrag.originalLeftEnd = boundary.left->getEndFrame();
    stretchDrag.originalRightStart = boundary.right->getStartFrame();
    stretchDrag.originalRightEnd = boundary.right->getEndFrame();

    if (effectiveMode == StretchMode::Absorb)
    {
      // Absorb: constrain within the two-note span
      stretchDrag.minFrame =
          stretchDrag.originalLeftStart + minStretchNoteFrames;
      stretchDrag.maxFrame =
          stretchDrag.originalRightEnd - minStretchNoteFrames;
    }
    else
    {
      // Ripple: the left note can grow freely (right note + all after shift)
      // Left note still has a minimum size
      stretchDrag.minFrame =
          stretchDrag.originalLeftStart + minStretchNoteFrames;
      // Right note still has minimum size, but the boundary can go further
      // since subsequent notes shift
      stretchDrag.maxFrame =
          stretchDrag.originalRightEnd - minStretchNoteFrames;
      // Actually in ripple mode the right note stays the SAME length,
      // it just shifts. So maxFrame is essentially unlimited (clamped to
      // totalFrames - minStretchNoteFrames for the right note's new position).
      // But we need to ensure the right note doesn't go past totalFrames.
      // For now, allow generous range; we'll validate during drag.
      stretchDrag.maxFrame = totalFrames - minStretchNoteFrames;
    }
  }
  else if (boundary.right)
  {
    // Only right note - stretch its left boundary
    stretchDrag.originalBoundary = boundary.right->getStartFrame();
    stretchDrag.originalLeftStart = 0;
    stretchDrag.originalLeftEnd = 0;
    stretchDrag.originalRightStart = boundary.right->getStartFrame();
    stretchDrag.originalRightEnd = boundary.right->getEndFrame();
    stretchDrag.minFrame = 0;
    stretchDrag.maxFrame =
        stretchDrag.originalRightEnd - minStretchNoteFrames;
  }
  else
  {
    // Only left note - stretch its right boundary
    stretchDrag.originalBoundary = boundary.left->getEndFrame();
    stretchDrag.originalLeftStart = boundary.left->getStartFrame();
    stretchDrag.originalLeftEnd = boundary.left->getEndFrame();
    stretchDrag.originalRightStart = totalFrames;
    stretchDrag.originalRightEnd = totalFrames;
    stretchDrag.minFrame =
        stretchDrag.originalLeftStart + minStretchNoteFrames;
    stretchDrag.maxFrame = totalFrames;
  }

  stretchDrag.currentBoundary = stretchDrag.originalBoundary;

  // Ensure all notes have clip waveforms
  if (audioData.waveform.getNumSamples() > 0)
  {
    const float *src = audioData.waveform.getReadPointer(0);
    const int totalSamples = audioData.waveform.getNumSamples();
    const float *origSrc = audioData.originalWaveform.getNumSamples() > 0
                               ? audioData.originalWaveform.getReadPointer(0)
                               : nullptr;
    const int origTotalSamples = audioData.originalWaveform.getNumSamples();
    for (auto &note : project->getNotes())
    {
      if (!note.hasClipWaveform())
      {
        int startSample = note.getStartFrame() * HOP_SIZE;
        int endSample = note.getEndFrame() * HOP_SIZE;
        startSample = std::max(0, std::min(startSample, totalSamples));
        endSample = std::max(startSample, std::min(endSample, totalSamples));
        std::vector<float> clip;
        clip.reserve(static_cast<size_t>(endSample - startSample));
        for (int i = startSample; i < endSample; ++i)
          clip.push_back(src[i]);
        note.setClipWaveform(std::move(clip));
      }
      if (!note.hasSrcClipWaveform() && origSrc)
      {
        int startSample = note.getSrcStartFrame() * HOP_SIZE;
        int endSample = note.getSrcEndFrame() * HOP_SIZE;
        startSample = std::max(0, std::min(startSample, origTotalSamples));
        endSample = std::max(startSample, std::min(endSample, origTotalSamples));
        std::vector<float> srcClip;
        srcClip.reserve(static_cast<size_t>(endSample - startSample));
        for (int i = startSample; i < endSample; ++i)
          srcClip.push_back(origSrc[i]);
        note.setSrcClipWaveform(std::move(srcClip));
      }
    }
  }

  if (audioData.deltaPitch.size() < static_cast<size_t>(totalFrames))
    audioData.deltaPitch.resize(static_cast<size_t>(totalFrames), 0.0f);
  if (audioData.voicedMask.size() < static_cast<size_t>(totalFrames))
    audioData.voicedMask.resize(static_cast<size_t>(totalFrames), true);

  if (stretchDrag.maxFrame <= stretchDrag.minFrame)
  {
    stretchDrag.active = false;
    return;
  }

  // Calculate range for undo/redo - must include all potentially affected frames
  if (boundary.left && boundary.right)
  {
    stretchDrag.rangeStartFull =
        std::max(0, stretchDrag.originalLeftStart);
    stretchDrag.rangeEndFull =
        std::min(totalFrames, stretchDrag.originalRightEnd);
  }
  else if (boundary.left)
  {
    stretchDrag.rangeStartFull =
        std::max(0, stretchDrag.originalLeftStart);
    stretchDrag.rangeEndFull =
        std::min(totalFrames, stretchDrag.maxFrame);
  }
  else
  {
    stretchDrag.rangeStartFull = std::max(0, stretchDrag.minFrame);
    stretchDrag.rangeEndFull =
        std::min(totalFrames, stretchDrag.originalRightEnd);
  }

  if (stretchDrag.rangeEndFull <= stretchDrag.rangeStartFull)
  {
    stretchDrag.active = false;
    return;
  }

  // Save left note data if exists
  if (boundary.left)
  {
    int leftStart = std::max(0, stretchDrag.originalLeftStart);
    int leftEnd = std::min(stretchDrag.originalLeftEnd, totalFrames);
    if (leftEnd > leftStart)
    {
      stretchDrag.leftDelta.assign(audioData.deltaPitch.begin() + leftStart,
                                   audioData.deltaPitch.begin() + leftEnd);
      stretchDrag.leftVoiced.assign(audioData.voicedMask.begin() + leftStart,
                                    audioData.voicedMask.begin() + leftEnd);
    }
    if (boundary.left->hasClipWaveform())
      stretchDrag.originalLeftClip = boundary.left->getClipWaveform();
  }

  // Save right note data if exists
  if (boundary.right)
  {
    int rightStart = std::max(0, stretchDrag.originalRightStart);
    int rightEnd = std::min(stretchDrag.originalRightEnd, totalFrames);
    if (rightEnd > rightStart)
    {
      stretchDrag.rightDelta.assign(audioData.deltaPitch.begin() + rightStart,
                                    audioData.deltaPitch.begin() + rightEnd);
      stretchDrag.rightVoiced.assign(audioData.voicedMask.begin() + rightStart,
                                     audioData.voicedMask.begin() + rightEnd);
    }
    if (boundary.right->hasClipWaveform())
      stretchDrag.originalRightClip = boundary.right->getClipWaveform();
  }

  // Save full range data for undo
  stretchDrag.originalDeltaRangeFull.assign(
      audioData.deltaPitch.begin() + stretchDrag.rangeStartFull,
      audioData.deltaPitch.begin() + stretchDrag.rangeEndFull);
  stretchDrag.originalVoicedRangeFull.assign(
      audioData.voicedMask.begin() + stretchDrag.rangeStartFull,
      audioData.voicedMask.begin() + stretchDrag.rangeEndFull);

  if (!audioData.melSpectrogram.empty() &&
      stretchDrag.rangeStartFull <
          static_cast<int>(audioData.melSpectrogram.size()))
  {
    int melEnd = std::min(stretchDrag.rangeEndFull,
                          static_cast<int>(audioData.melSpectrogram.size()));
    stretchDrag.originalMelRangeFull.assign(
        audioData.melSpectrogram.begin() + stretchDrag.rangeStartFull,
        audioData.melSpectrogram.begin() + melEnd);
  }

  // --- Ripple mode: collect and save subsequent notes ---
  if (effectiveMode == StretchMode::Ripple)
  {
    // Determine the frame after which notes should be collected for ripple
    int rippleAfterFrame = stretchDrag.originalBoundary;
    stretchDrag.rippleNotes = collectRippleNotes(rippleAfterFrame);
    stretchDrag.originalRippleStarts.reserve(stretchDrag.rippleNotes.size());
    stretchDrag.originalRippleEnds.reserve(stretchDrag.rippleNotes.size());
    stretchDrag.rippleVoicedData.reserve(stretchDrag.rippleNotes.size());
    stretchDrag.rippleMelData.reserve(stretchDrag.rippleNotes.size());
    for (auto *note : stretchDrag.rippleNotes)
    {
      stretchDrag.originalRippleStarts.push_back(note->getStartFrame());
      stretchDrag.originalRippleEnds.push_back(note->getEndFrame());

      // Save per-note voicedMask
      int ns = std::max(0, note->getStartFrame());
      int ne = std::min(totalFrames, note->getEndFrame());
      if (ne > ns && static_cast<int>(audioData.voicedMask.size()) >= ne)
      {
        stretchDrag.rippleVoicedData.emplace_back(
            audioData.voicedMask.begin() + ns,
            audioData.voicedMask.begin() + ne);
      }
      else
      {
        stretchDrag.rippleVoicedData.emplace_back();
      }

      // Save per-note mel
      int melSize = static_cast<int>(audioData.melSpectrogram.size());
      if (ne > ns && ns < melSize)
      {
        int melEnd = std::min(ne, melSize);
        stretchDrag.rippleMelData.emplace_back(
            audioData.melSpectrogram.begin() + ns,
            audioData.melSpectrogram.begin() + melEnd);
      }
      else
      {
        stretchDrag.rippleMelData.emplace_back();
      }
    }

    // Extend rangeEndFull to cover maximum possible shifted extent.
    // Max shift covers both drag directions (right-edge and left-edge).
    int maxShift = std::max(
        stretchDrag.maxFrame - stretchDrag.originalBoundary,
        stretchDrag.originalBoundary - stretchDrag.minFrame);
    // Right boundary note extends to originalRightEnd + maxShift
    int maxRightEnd = stretchDrag.originalRightEnd + maxShift;
    stretchDrag.rangeEndFull = std::max(stretchDrag.rangeEndFull,
                                        std::min(totalFrames, maxRightEnd));
    // Ripple notes extend to their original end + maxShift
    if (!stretchDrag.rippleNotes.empty())
    {
      int lastRippleEnd = stretchDrag.originalRippleEnds.back() + maxShift;
      stretchDrag.rangeEndFull = std::max(stretchDrag.rangeEndFull,
                                          std::min(totalFrames, lastRippleEnd));
    }

    // Re-capture the expanded range snapshots
    if (stretchDrag.rangeEndFull > stretchDrag.rangeStartFull)
    {
      int rStart = stretchDrag.rangeStartFull;
      int rEnd = std::min(stretchDrag.rangeEndFull, totalFrames);

      stretchDrag.originalDeltaRangeFull.assign(
          audioData.deltaPitch.begin() + rStart,
          audioData.deltaPitch.begin() + rEnd);
      stretchDrag.originalVoicedRangeFull.assign(
          audioData.voicedMask.begin() + rStart,
          audioData.voicedMask.begin() + rEnd);

      if (!audioData.melSpectrogram.empty() &&
          rStart < static_cast<int>(audioData.melSpectrogram.size()))
      {
        int melEnd = std::min(rEnd,
                              static_cast<int>(audioData.melSpectrogram.size()));
        stretchDrag.originalMelRangeFull.assign(
            audioData.melSpectrogram.begin() + rStart,
            audioData.melSpectrogram.begin() + melEnd);
      }
    }
  }
}

void StretchHandler::updateStretchDrag(int targetFrame)
{
  if (!stretchDrag.active || !owner_.project)
    return;

  if (!stretchDrag.boundary.left && !stretchDrag.boundary.right)
    return;

  targetFrame =
      juce::jlimit(stretchDrag.minFrame, stretchDrag.maxFrame, targetFrame);
  if (targetFrame == stretchDrag.currentBoundary)
    return;

  const bool isRipple = (stretchDrag.mode == StretchMode::Ripple);

  // Validate minimum note lengths
  if (stretchDrag.boundary.left && stretchDrag.boundary.right)
  {
    if (targetFrame - stretchDrag.originalLeftStart < minStretchNoteFrames)
      return;
    if (!isRipple &&
        stretchDrag.originalRightEnd - targetFrame < minStretchNoteFrames)
      return;
  }
  else if (stretchDrag.boundary.right)
  {
    if (stretchDrag.originalRightEnd - targetFrame < minStretchNoteFrames)
      return;
  }
  else
  {
    if (targetFrame - stretchDrag.originalLeftStart < minStretchNoteFrames)
      return;
  }

  stretchDrag.currentBoundary = targetFrame;
  stretchDrag.changed = true;

  // Position-only drag: only note frame positions change during drag.
  // Global data arrays (voicedMask, mel) are NOT modified here;
  // deltaPitch is handled by rebuildBaseFromNotesForDrag below.
  // All data resampling is deferred to finishStretchDrag for correctness.
  if (isRipple)
    restoreRippleNotes();

  // Update left note position
  if (stretchDrag.boundary.left)
  {
    stretchDrag.boundary.left->setEndFrame(targetFrame);
    stretchDrag.boundary.left->markDirty();
  }

  // Update right note position
  if (stretchDrag.boundary.right)
  {
    if (isRipple && stretchDrag.boundary.left)
    {
      // Shared boundary ripple: right note shifts, keeps original length
      int rippleDelta = targetFrame - stretchDrag.originalBoundary;
      stretchDrag.boundary.right->setStartFrame(
          stretchDrag.originalRightStart + rippleDelta);
      stretchDrag.boundary.right->setEndFrame(
          stretchDrag.originalRightEnd + rippleDelta);
      stretchDrag.boundary.right->markDirty();
      stretchDrag.rippleDelta = rippleDelta;
      applyRippleShift(rippleDelta);
    }
    else
    {
      // Absorb or free-left-edge ripple: stretch right note's start
      stretchDrag.boundary.right->setStartFrame(targetFrame);
      stretchDrag.boundary.right->setEndFrame(stretchDrag.originalRightEnd);
      stretchDrag.boundary.right->markDirty();
      if (isRipple)
        stretchDrag.rippleDelta = 0;
    }
  }
  else if (isRipple && stretchDrag.boundary.left)
  {
    // Left-only boundary with ripple: shift subsequent notes
    int rippleDelta = targetFrame - stretchDrag.originalBoundary;
    stretchDrag.rippleDelta = rippleDelta;
    applyRippleShift(rippleDelta);
  }

  // Clear stale per-note waveforms so rendering falls back to the
  // global waveform composed from originalWaveform (which correctly
  // stretches/shifts audio to match current note positions).
  if (stretchDrag.boundary.left)
  {
    stretchDrag.boundary.left->setClipWaveform({});
    if (stretchDrag.boundary.left->hasSynthWaveform())
      stretchDrag.boundary.left->markSynthDirty();
  }
  if (stretchDrag.boundary.right)
  {
    stretchDrag.boundary.right->setClipWaveform({});
    if (stretchDrag.boundary.right->hasSynthWaveform())
      stretchDrag.boundary.right->markSynthDirty();
  }
  if (isRipple)
  {
    for (auto *rNote : stretchDrag.rippleNotes)
    {
      rNote->setClipWaveform({});
      if (rNote->hasSynthWaveform())
        rNote->markSynthDirty();
    }
  }

  // Targeted rebuild: regenerates basePitch from all notes but only
  // processes deltaPitch for the affected notes.
  // In ripple mode, include ALL shifted notes so their deltaPitch is
  // correctly placed at their new positions in audioData.deltaPitch.
  {
    std::vector<Note *> affectedNotes;
    if (stretchDrag.boundary.left)
      affectedNotes.push_back(stretchDrag.boundary.left);
    if (stretchDrag.boundary.right)
      affectedNotes.push_back(stretchDrag.boundary.right);
    if (isRipple)
    {
      affectedNotes.insert(affectedNotes.end(),
                           stretchDrag.rippleNotes.begin(),
                           stretchDrag.rippleNotes.end());
    }
    PitchCurveProcessor::rebuildBaseFromNotesForDrag(*owner_.project, affectedNotes);
  }
  owner_.invalidateBasePitchCache();

  // Recompose global waveform so background waveform and note-level
  // rendering both reflect the current note layout during drag.
  // Note bars with cleared clipWaveform will fall back to global waveform,
  // which has correct stretched/shifted audio from stretchFromOrig.
  owner_.project->composeGlobalWaveform();
  owner_.invalidateWaveformCache();

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();

  // NOTE: Do NOT set f0 dirty range during drag — it accumulates (union of
  // all ranges) and can grow to cover the entire file. The dirty range is
  // only needed after drag finishes, which is handled in finishStretchDrag().
}

void StretchHandler::finishStretchDrag()
{
  auto *project = owner_.project;
  if (!stretchDrag.active || !project)
  {
    stretchDrag = {};
    return;
  }

  if (!stretchDrag.changed)
  {
    cancelStretchDrag();
    return;
  }

  const bool isRipple = (stretchDrag.mode == StretchMode::Ripple);

  auto &audioData = project->getAudioData();
  const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
  const int currentBoundary = stretchDrag.currentBoundary;
  int rangeStart = std::clamp(stretchDrag.rangeStartFull, 0, totalFrames);
  int rangeEnd = std::clamp(stretchDrag.rangeEndFull, 0, totalFrames);
  if (rangeEnd <= rangeStart)
  {
    cancelStretchDrag();
    return;
  }

  // --- Compute final voicedMask ---
  // During position-only drag, voicedMask was not modified.
  // Resample stretched notes and place shifted notes at final positions.
  {
    // Clear affected range to unvoiced baseline
    for (int i = rangeStart; i < rangeEnd; ++i)
      audioData.voicedMask[static_cast<size_t>(i)] = false;

    auto smoothResampledVoiced = [](std::vector<bool> &mask)
    {
      if (mask.size() < 3)
        return;
      std::vector<bool> smoothed(mask);
      for (size_t i = 1; i + 1 < mask.size(); ++i)
      {
        const bool p = mask[i - 1];
        const bool c = mask[i];
        const bool n = mask[i + 1];
        if (!c && p && n)
          smoothed[i] = true;
        else if (c && !p && !n)
          smoothed[i] = false;
      }
      mask.swap(smoothed);
    };

    // Left note: resample voiced to new stretched length
    if (stretchDrag.boundary.left && !stretchDrag.leftVoiced.empty())
    {
      int leftLen = currentBoundary - stretchDrag.originalLeftStart;
      if (leftLen > 0)
      {
        auto resampled = CurveResampler::resampleNearest(
            stretchDrag.leftVoiced, leftLen);
        smoothResampledVoiced(resampled);
        int base = stretchDrag.originalLeftStart;
        for (int i = 0; i < leftLen && (base + i) < totalFrames; ++i)
          audioData.voicedMask[static_cast<size_t>(base + i)] =
              resampled[static_cast<size_t>(i)];
      }
    }

    // Right note
    if (stretchDrag.boundary.right)
    {
      if (isRipple && stretchDrag.boundary.left)
      {
        // Shifted (not stretched): write original voiced at new position
        int rStart = stretchDrag.boundary.right->getStartFrame();
        const auto &rv = stretchDrag.rightVoiced;
        for (int i = 0; i < static_cast<int>(rv.size()); ++i)
          if (rStart + i >= 0 && rStart + i < totalFrames)
            audioData.voicedMask[static_cast<size_t>(rStart + i)] =
                rv[static_cast<size_t>(i)];
      }
      else if (!stretchDrag.rightVoiced.empty())
      {
        // Stretched (absorb or free-left-edge ripple)
        int rightLen = stretchDrag.originalRightEnd - currentBoundary;
        if (rightLen > 0)
        {
          auto resampled = CurveResampler::resampleNearest(
              stretchDrag.rightVoiced, rightLen);
          smoothResampledVoiced(resampled);
          for (int i = 0; i < rightLen && (currentBoundary + i) < totalFrames;
               ++i)
            audioData.voicedMask[static_cast<size_t>(currentBoundary + i)] =
                resampled[static_cast<size_t>(i)];
        }
      }
    }

    // Ripple notes: write original voiced at shifted positions
    if (isRipple)
    {
      for (size_t ri = 0; ri < stretchDrag.rippleNotes.size(); ++ri)
      {
        int rStart = stretchDrag.rippleNotes[ri]->getStartFrame();
        const auto &rv = stretchDrag.rippleVoicedData[ri];
        for (int i = 0; i < static_cast<int>(rv.size()); ++i)
          if (rStart + i >= 0 && rStart + i < totalFrames)
            audioData.voicedMask[static_cast<size_t>(rStart + i)] =
                rv[static_cast<size_t>(i)];
      }
    }
  }

  std::vector<float> newDelta(audioData.deltaPitch.begin() + rangeStart,
                              audioData.deltaPitch.begin() + rangeEnd);
  std::vector<bool> newVoiced(audioData.voicedMask.begin() + rangeStart,
                              audioData.voicedMask.begin() + rangeEnd);
  std::vector<std::vector<float>> newMel;
  if (!audioData.melSpectrogram.empty() &&
      rangeEnd <= static_cast<int>(audioData.melSpectrogram.size()) &&
      audioData.waveform.getNumSamples() > 0 && centeredMelComputer)
  {
    // Only compute HQ mel for notes that were actually stretched (not in ripple shift)
    const int leftLen = stretchDrag.boundary.left
                            ? (currentBoundary - stretchDrag.originalLeftStart)
                            : 0;
    int rightLen = 0;
    if (stretchDrag.boundary.right)
    {
      if (isRipple && stretchDrag.boundary.left)
      {
        // In ripple mode with shared boundary, right note was NOT stretched,
        // it just shifted. No HQ mel recomputation needed for it.
        rightLen = 0;
      }
      else
      {
        rightLen = stretchDrag.originalRightEnd - currentBoundary;
      }
    }

    const float *globalAudio = audioData.waveform.getReadPointer(0);
    const int numSamples = audioData.waveform.getNumSamples();

    std::vector<std::vector<float>> newLeftMel;
    std::vector<std::vector<float>> newRightMel;

    if (leftLen > 0)
    {
      centeredMelComputer->computeTimeStretched(
          globalAudio, numSamples, stretchDrag.originalLeftStart,
          stretchDrag.originalLeftEnd, leftLen, newLeftMel);
    }

    if (rightLen > 0)
    {
      centeredMelComputer->computeTimeStretched(
          globalAudio, numSamples, stretchDrag.originalRightStart,
          stretchDrag.originalRightEnd, rightLen, newRightMel);
    }

    // Fallback to nearest neighbor if centered mel computation failed
    if (newLeftMel.empty() && leftLen > 0)
    {
      const int leftOffset =
          stretchDrag.originalLeftStart - stretchDrag.rangeStartFull;
      std::vector<std::vector<float>> leftMel;
      if (leftOffset >= 0 &&
          leftOffset + (stretchDrag.originalLeftEnd -
                        stretchDrag.originalLeftStart) <=
              static_cast<int>(stretchDrag.originalMelRangeFull.size()))
      {
        leftMel.assign(
            stretchDrag.originalMelRangeFull.begin() + leftOffset,
            stretchDrag.originalMelRangeFull.begin() + leftOffset +
                (stretchDrag.originalLeftEnd -
                 stretchDrag.originalLeftStart));
      }
      newLeftMel = CurveResampler::resampleNearest2D(leftMel, leftLen);
    }

    if (newRightMel.empty() && rightLen > 0)
    {
      const int rightOffset =
          stretchDrag.originalRightStart - stretchDrag.rangeStartFull;
      std::vector<std::vector<float>> rightMel;
      if (rightOffset >= 0 &&
          rightOffset + (stretchDrag.originalRightEnd -
                         stretchDrag.originalRightStart) <=
              static_cast<int>(stretchDrag.originalMelRangeFull.size()))
      {
        rightMel.assign(
            stretchDrag.originalMelRangeFull.begin() + rightOffset,
            stretchDrag.originalMelRangeFull.begin() + rightOffset +
                (stretchDrag.originalRightEnd -
                 stretchDrag.originalRightStart));
      }
      newRightMel = CurveResampler::resampleNearest2D(rightMel, rightLen);
    }

    // Write each note's HQ mel at its actual position in the global array,
    // rather than checking total size against range (which fails for single-note
    // and ripple cases where totalMelLen != rangeEnd - rangeStart).
    const int melSize = static_cast<int>(audioData.melSpectrogram.size());

    if (!newLeftMel.empty() && stretchDrag.boundary.left)
    {
      const int leftStart = stretchDrag.originalLeftStart;
      for (int i = 0; i < static_cast<int>(newLeftMel.size()); ++i)
      {
        const int globalIdx = leftStart + i;
        if (globalIdx >= 0 && globalIdx < melSize)
          audioData.melSpectrogram[static_cast<size_t>(globalIdx)] =
              newLeftMel[static_cast<size_t>(i)];
      }
    }

    if (!newRightMel.empty() && stretchDrag.boundary.right)
    {
      const int rightStart = currentBoundary;
      for (int i = 0; i < static_cast<int>(newRightMel.size()); ++i)
      {
        const int globalIdx = rightStart + i;
        if (globalIdx >= 0 && globalIdx < melSize)
          audioData.melSpectrogram[static_cast<size_t>(globalIdx)] =
              newRightMel[static_cast<size_t>(i)];
      }
    }

    // In ripple mode, write the right note's original mel at its shifted position,
    // and write each ripple note's original mel at its shifted position.
    if (isRipple)
    {
      if (stretchDrag.boundary.right && stretchDrag.boundary.left)
      {
        int rightStart = stretchDrag.boundary.right->getStartFrame();
        const int rightOffset =
            stretchDrag.originalRightStart - stretchDrag.rangeStartFull;
        int origRightLen = stretchDrag.originalRightEnd - stretchDrag.originalRightStart;
        if (rightOffset >= 0 &&
            rightOffset + origRightLen <=
                static_cast<int>(stretchDrag.originalMelRangeFull.size()))
        {
          for (int i = 0; i < origRightLen; ++i)
          {
            if (rightStart + i >= 0 && rightStart + i < melSize)
              audioData.melSpectrogram[static_cast<size_t>(rightStart + i)] =
                  stretchDrag.originalMelRangeFull[static_cast<size_t>(rightOffset + i)];
          }
        }
      }

      for (size_t ri = 0; ri < stretchDrag.rippleNotes.size(); ++ri)
      {
        auto *rNote = stretchDrag.rippleNotes[ri];
        const auto &rMel = stretchDrag.rippleMelData[ri];
        if (!rMel.empty())
        {
          int rStart = rNote->getStartFrame();
          for (int i = 0; i < static_cast<int>(rMel.size()); ++i)
          {
            if (rStart + i >= 0 && rStart + i < melSize)
              audioData.melSpectrogram[static_cast<size_t>(rStart + i)] =
                  rMel[static_cast<size_t>(i)];
          }
        }
      }
    }

    // Build newMel for undo data — must cover full rangeStart..rangeEnd
    // so that applyState()'s size check (mel.size() == rangeEnd - rangeStart) passes.
    newMel.assign(audioData.melSpectrogram.begin() + rangeStart,
                  audioData.melSpectrogram.begin() + std::min(rangeEnd, melSize));
  }

  // Resample clipWaveform to final stretched length (deferred from updateStretchDrag
  // for performance — only done once at drag finish instead of every mouse move frame).
  // Use srcClipWaveform (original, unmodified) when available to avoid compounding
  // quality degradation from re-resampling previously stretched clipWaveforms.
  if (stretchDrag.boundary.left)
  {
    const int leftLen = currentBoundary - stretchDrag.originalLeftStart;
    if (leftLen > 0)
    {
      const auto &sourceClip = stretchDrag.boundary.left->hasSrcClipWaveform()
                                   ? stretchDrag.boundary.left->getSrcClipWaveform()
                                   : stretchDrag.originalLeftClip;
      if (!sourceClip.empty())
      {
        const int newLeftSamples = leftLen * HOP_SIZE;
        auto newLeftClip = CurveResampler::resampleLinear(
            sourceClip, newLeftSamples);
        stretchDrag.boundary.left->setClipWaveform(std::move(newLeftClip));
      }
    }
  }
  if (stretchDrag.boundary.right)
  {
    if (isRipple && stretchDrag.boundary.left)
    {
      // Ripple mode with shared boundary: right note not stretched, restore original clip
      if (!stretchDrag.originalRightClip.empty())
        stretchDrag.boundary.right->setClipWaveform(stretchDrag.originalRightClip);
    }
    else
    {
      int rightLen = stretchDrag.originalRightEnd - currentBoundary;
      if (rightLen > 0)
      {
        const auto &sourceClip = stretchDrag.boundary.right->hasSrcClipWaveform()
                                     ? stretchDrag.boundary.right->getSrcClipWaveform()
                                     : stretchDrag.originalRightClip;
        if (!sourceClip.empty())
        {
          const int newRightSamples = rightLen * HOP_SIZE;
          auto newRightClip = CurveResampler::resampleLinear(
              sourceClip, newRightSamples);
          stretchDrag.boundary.right->setClipWaveform(std::move(newRightClip));
        }
      }
    }
  }

  // Regenerate clipWaveform for ripple-shifted notes from the final
  // composed global waveform so note bars show correct audio.
  if (isRipple && audioData.waveform.getNumSamples() > 0)
  {
    const float *wavPtr = audioData.waveform.getReadPointer(0);
    const int wavLen = audioData.waveform.getNumSamples();

    auto regenerateClip = [&](Note *note)
    {
      int s0 = note->getStartFrame() * HOP_SIZE;
      int s1 = note->getEndFrame() * HOP_SIZE;
      s0 = std::clamp(s0, 0, wavLen);
      s1 = std::clamp(s1, s0, wavLen);
      if (s1 > s0)
        note->setClipWaveform(std::vector<float>(wavPtr + s0, wavPtr + s1));
    };

    // Right note: shifted, not stretched (in between-notes boundary)
    if (stretchDrag.boundary.right && stretchDrag.boundary.left)
      regenerateClip(stretchDrag.boundary.right);

    for (auto *rNote : stretchDrag.rippleNotes)
      regenerateClip(rNote);
  }

  // Capture final note positions for undo
  int newLeftStart = stretchDrag.boundary.left
                         ? stretchDrag.boundary.left->getStartFrame()
                         : 0;
  int newLeftEnd = stretchDrag.boundary.left
                       ? stretchDrag.boundary.left->getEndFrame()
                       : 0;

  if (owner_.undoManager)
  {
    int capturedRangeStart = rangeStart;
    int capturedRangeEnd = rangeEnd;
    std::vector<float> oldDelta;
    std::vector<bool> oldVoiced;
    std::vector<std::vector<float>> oldMel;
    if (!stretchDrag.originalDeltaRangeFull.empty() &&
        !stretchDrag.originalVoicedRangeFull.empty())
    {
      int offset = rangeStart - stretchDrag.rangeStartFull;
      int count = rangeEnd - rangeStart;
      if (offset >= 0 &&
          offset + count <=
              static_cast<int>(
                  stretchDrag.originalDeltaRangeFull.size()))
      {
        oldDelta.assign(
            stretchDrag.originalDeltaRangeFull.begin() + offset,
            stretchDrag.originalDeltaRangeFull.begin() + offset + count);
        oldVoiced.assign(
            stretchDrag.originalVoicedRangeFull.begin() + offset,
            stretchDrag.originalVoicedRangeFull.begin() + offset + count);
      }
    }
    if (!stretchDrag.originalMelRangeFull.empty())
    {
      int offset = rangeStart - stretchDrag.rangeStartFull;
      int count = rangeEnd - rangeStart;
      if (offset >= 0 &&
          offset + count <=
              static_cast<int>(
                  stretchDrag.originalMelRangeFull.size()))
      {
        oldMel.assign(
            stretchDrag.originalMelRangeFull.begin() + offset,
            stretchDrag.originalMelRangeFull.begin() + offset + count);
      }
    }

    auto *ownerPtr = &owner_;

    if (isRipple)
    {
      // Ripple undo action: captures both the stretched note and all shifted notes.
      // The right boundary note also shifts in ripple mode, so include it
      // in the ripple notes list for proper undo/redo of its position.
      std::vector<Note *> allRippleNotes;
      std::vector<int> allOldStarts;
      std::vector<int> allOldEnds;
      std::vector<int> allNewStarts;
      std::vector<int> allNewEnds;

      // Prepend right note if it exists (its position shifted during ripple)
      if (stretchDrag.boundary.right)
      {
        allRippleNotes.push_back(stretchDrag.boundary.right);
        allOldStarts.push_back(stretchDrag.originalRightStart);
        allOldEnds.push_back(stretchDrag.originalRightEnd);
        allNewStarts.push_back(stretchDrag.boundary.right->getStartFrame());
        allNewEnds.push_back(stretchDrag.boundary.right->getEndFrame());
      }

      // Append the rest of the ripple notes
      for (size_t ri = 0; ri < stretchDrag.rippleNotes.size(); ++ri)
      {
        allRippleNotes.push_back(stretchDrag.rippleNotes[ri]);
        allOldStarts.push_back(stretchDrag.originalRippleStarts[ri]);
        allOldEnds.push_back(stretchDrag.originalRippleEnds[ri]);
        allNewStarts.push_back(stretchDrag.rippleNotes[ri]->getStartFrame());
        allNewEnds.push_back(stretchDrag.rippleNotes[ri]->getEndFrame());
      }

      auto action = std::make_unique<NoteTimingRippleAction>(
          stretchDrag.boundary.left, stretchDrag.boundary.right,
          std::move(allRippleNotes),
          &audioData.deltaPitch, &audioData.voicedMask,
          &audioData.melSpectrogram, capturedRangeStart, capturedRangeEnd,
          stretchDrag.originalLeftStart, stretchDrag.originalLeftEnd,
          newLeftStart, newLeftEnd,
          std::move(allOldStarts),
          std::move(allOldEnds),
          std::move(allNewStarts),
          std::move(allNewEnds),
          std::move(oldDelta), std::move(newDelta),
          std::move(oldVoiced), std::move(newVoiced),
          std::move(oldMel), std::move(newMel),
          [ownerPtr](int startFrame, int endFrame)
          {
            if (!ownerPtr->project)
              return;
            PitchCurveProcessor::rebuildBaseFromNotes(*ownerPtr->project);
            ownerPtr->invalidateBasePitchCache();
            const int f0Size =
                static_cast<int>(ownerPtr->project->getAudioData().f0.size());
            int smoothStart = std::max(0, startFrame - 60);
            int smoothEnd = std::min(f0Size, endFrame + 60);
            ownerPtr->project->setF0DirtyRange(smoothStart, smoothEnd);
            ownerPtr->project->composeGlobalWaveform();
          });
      owner_.undoManager->addAction(std::move(action));
    }
    else
    {
      // Absorb undo action
      auto action = std::make_unique<NoteTimingStretchAction>(
          stretchDrag.boundary.left, stretchDrag.boundary.right,
          &audioData.deltaPitch, &audioData.voicedMask,
          &audioData.melSpectrogram, capturedRangeStart, capturedRangeEnd,
          stretchDrag.originalLeftStart, stretchDrag.originalLeftEnd,
          stretchDrag.originalRightStart, stretchDrag.originalRightEnd,
          newLeftStart, newLeftEnd, currentBoundary,
          stretchDrag.originalRightEnd,
          std::move(oldDelta), std::move(newDelta), std::move(oldVoiced),
          std::move(newVoiced), std::move(oldMel), std::move(newMel),
          [ownerPtr](int startFrame, int endFrame)
          {
            if (!ownerPtr->project)
              return;
            PitchCurveProcessor::rebuildBaseFromNotes(*ownerPtr->project);
            ownerPtr->invalidateBasePitchCache();
            const int f0Size =
                static_cast<int>(ownerPtr->project->getAudioData().f0.size());
            int smoothStart = std::max(0, startFrame - 60);
            int smoothEnd = std::min(f0Size, endFrame + 60);
            ownerPtr->project->setF0DirtyRange(smoothStart, smoothEnd);
            ownerPtr->project->composeGlobalWaveform();
          });
      owner_.undoManager->addAction(std::move(action));
    }
  }

  // Full rebuild to ensure correctness after drag (the drag used a
  // lightweight targeted version that only updated affected notes).
  PitchCurveProcessor::rebuildBaseFromNotes(*project);
  owner_.invalidateBasePitchCache();

  // Mark affected notes' synthWaveforms as dirty so the next synthesis
  // cycle regenerates them. Then recompose the global waveform from
  // originalWaveform + per-note synthWaveforms — no memmove needed.
  if (stretchDrag.boundary.left)
    stretchDrag.boundary.left->markSynthDirty();
  if (stretchDrag.boundary.right)
    stretchDrag.boundary.right->markSynthDirty();
  // Only mark ripple notes synth-dirty if they were actually shifted.
  // Unshifted notes (e.g., left-edge-only boundary) keep valid synthWaveforms.
  if (stretchDrag.rippleDelta != 0)
  {
    for (auto *rNote : stretchDrag.rippleNotes)
      rNote->markSynthDirty();
  }
  project->composeGlobalWaveform();
  owner_.invalidateWaveformCache();
  owner_.repaint();

  // Compute tight dirty range based on actual affected note positions.
  int dirtyStart, dirtyEnd;
  if (isRipple)
  {
    // Ripple mode: only the stretched note needs resynthesis.
    // Shifted notes keep their synthWaveforms at new positions (compose handles placement).
    if (stretchDrag.boundary.left)
    {
      dirtyStart = stretchDrag.boundary.left->getStartFrame();
      dirtyEnd = stretchDrag.currentBoundary;
    }
    else
    {
      dirtyStart = stretchDrag.currentBoundary;
      dirtyEnd = stretchDrag.boundary.right->getEndFrame();
    }
    // Clear dirty marks on shifted notes so getDirtyFrameRange()
    // does not expand the synthesis range to cover them all.
    // Only clear right note's dirty if it was shifted (between-notes),
    // not when it was stretched (left-only boundary).
    if (stretchDrag.boundary.right && stretchDrag.boundary.left)
      stretchDrag.boundary.right->clearDirty();
    for (auto *rNote : stretchDrag.rippleNotes)
      rNote->clearDirty();
  }
  else
  {
    // Absorb mode
    if (stretchDrag.boundary.left && stretchDrag.boundary.right)
    {
      dirtyStart = stretchDrag.boundary.left->getStartFrame();
      dirtyEnd = stretchDrag.boundary.right->getEndFrame();
    }
    else if (stretchDrag.boundary.left)
    {
      dirtyStart = stretchDrag.boundary.left->getStartFrame();
      dirtyEnd = currentBoundary;
    }
    else
    {
      dirtyStart = currentBoundary;
      dirtyEnd = stretchDrag.boundary.right->getEndFrame();
    }
  }

  const int f0Size = static_cast<int>(audioData.f0.size());
  int smoothStart = std::max(0, dirtyStart - 60);
  int smoothEnd = std::min(f0Size, dirtyEnd + 60);
  project->setF0DirtyRange(smoothStart, smoothEnd);

  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();

  stretchDrag = {};
}

void StretchHandler::cancelStretchDrag()
{
  auto *project = owner_.project;
  if (!stretchDrag.active || !project)
  {
    stretchDrag = {};
    return;
  }

  auto &audioData = project->getAudioData();
  const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
  int rangeStart = std::clamp(stretchDrag.rangeStartFull, 0, totalFrames);
  int rangeEnd = std::clamp(stretchDrag.rangeEndFull, 0, totalFrames);

  if (rangeEnd > rangeStart &&
      stretchDrag.originalDeltaRangeFull.size() ==
          static_cast<size_t>(rangeEnd - rangeStart))
  {
    for (int i = rangeStart; i < rangeEnd; ++i)
      audioData.deltaPitch[static_cast<size_t>(i)] =
          stretchDrag.originalDeltaRangeFull[static_cast<size_t>(
              i - rangeStart)];
  }

  if (rangeEnd > rangeStart &&
      stretchDrag.originalVoicedRangeFull.size() ==
          static_cast<size_t>(rangeEnd - rangeStart))
  {
    for (int i = rangeStart; i < rangeEnd; ++i)
      audioData.voicedMask[static_cast<size_t>(i)] =
          stretchDrag.originalVoicedRangeFull[static_cast<size_t>(
              i - rangeStart)];
  }

  if (!stretchDrag.originalMelRangeFull.empty() && rangeStart < rangeEnd &&
      audioData.melSpectrogram.size() >=
          static_cast<size_t>(
              rangeStart + stretchDrag.originalMelRangeFull.size()))
  {
    for (size_t i = 0; i < stretchDrag.originalMelRangeFull.size(); ++i)
      audioData.melSpectrogram[static_cast<size_t>(rangeStart) + i] =
          stretchDrag.originalMelRangeFull[i];
  }

  if (stretchDrag.boundary.left)
  {
    stretchDrag.boundary.left->setStartFrame(stretchDrag.originalLeftStart);
    stretchDrag.boundary.left->setEndFrame(stretchDrag.originalLeftEnd);
    stretchDrag.boundary.left->markDirty();
    if (!stretchDrag.originalLeftClip.empty())
      stretchDrag.boundary.left->setClipWaveform(
          stretchDrag.originalLeftClip);
  }
  if (stretchDrag.boundary.right)
  {
    stretchDrag.boundary.right->setStartFrame(
        stretchDrag.originalRightStart);
    stretchDrag.boundary.right->setEndFrame(stretchDrag.originalRightEnd);
    stretchDrag.boundary.right->markDirty();
    if (!stretchDrag.originalRightClip.empty())
      stretchDrag.boundary.right->setClipWaveform(
          stretchDrag.originalRightClip);
  }

  // Restore ripple notes
  if (stretchDrag.mode == StretchMode::Ripple)
  {
    restoreRippleNotes();
  }

  PitchCurveProcessor::rebuildBaseFromNotes(*project);
  owner_.invalidateBasePitchCache();

  project->composeGlobalWaveform();
  owner_.invalidateWaveformCache();

  // Regenerate clipWaveform for notes whose clips were cleared during drag
  if (audioData.waveform.getNumSamples() > 0)
  {
    const float *wavPtr = audioData.waveform.getReadPointer(0);
    const int wavLen = audioData.waveform.getNumSamples();
    auto regenerateClip = [&](Note *note)
    {
      if (note->hasClipWaveform())
        return;
      int s0 = note->getStartFrame() * HOP_SIZE;
      int s1 = note->getEndFrame() * HOP_SIZE;
      s0 = std::clamp(s0, 0, wavLen);
      s1 = std::clamp(s1, s0, wavLen);
      if (s1 > s0)
        note->setClipWaveform(std::vector<float>(wavPtr + s0, wavPtr + s1));
    };
    if (stretchDrag.boundary.left && !stretchDrag.boundary.left->hasClipWaveform())
      regenerateClip(stretchDrag.boundary.left);
    if (stretchDrag.boundary.right && !stretchDrag.boundary.right->hasClipWaveform())
      regenerateClip(stretchDrag.boundary.right);
    if (stretchDrag.mode == StretchMode::Ripple)
    {
      for (auto *rNote : stretchDrag.rippleNotes)
        regenerateClip(rNote);
    }
  }

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();

  stretchDrag = {};
}

#endif // HACHITUNE_ENABLE_STRETCH
