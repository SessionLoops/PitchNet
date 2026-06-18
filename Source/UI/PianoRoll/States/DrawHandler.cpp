#include "DrawHandler.h"
#include "../../PianoRollComponent.h"
#include "../../../Utils/Constants.h"
#include "../../../Utils/PitchCurveProcessor.h"

DrawHandler::DrawHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool DrawHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                            float worldY) {
  juce::ignoreUnused(e);

  isDrawing = false;
  isPendingDraw = true;
  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  drawCurves.clear();
  activeDrawCurve = nullptr;
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  pendingDrawStartX = worldX;
  pendingDrawStartY = worldY;
  return true;
}

bool DrawHandler::mouseDrag(const juce::MouseEvent &e, float worldX,
                            float worldY) {
  juce::ignoreUnused(e);

  if (isPendingDraw) {
    isPendingDraw = false;
    isDrawing = true;
    applyPitchDrawing(pendingDrawStartX, pendingDrawStartY);
  }

  if (!isDrawing)
    return false;

  applyPitchDrawing(worldX, worldY);

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();

  owner_.repaint();
  return true;
}

bool DrawHandler::mouseUp(const juce::MouseEvent &e, float worldX,
                          float worldY) {
  juce::ignoreUnused(e, worldX, worldY);

  if (isPendingDraw) {
    isPendingDraw = false;
    drawingEdits.clear();
    drawingEditIndexByFrame.clear();
    lastDrawFrame = -1;
    lastDrawValueCents = 0;
    activeDrawCurve = nullptr;
    drawCurves.clear();
    return true;
  }

  if (!isDrawing)
    return false;

  isDrawing = false;
  commitPitchDrawing();
  owner_.repaint();
  return true;
}

bool DrawHandler::isActive() const { return isDrawing || isPendingDraw; }

void DrawHandler::cancel() {
  if (isPendingDraw) {
    isPendingDraw = false;
    drawingEdits.clear();
    drawingEditIndexByFrame.clear();
    lastDrawFrame = -1;
    lastDrawValueCents = 0;
    activeDrawCurve = nullptr;
    drawCurves.clear();
    owner_.repaint();
    return;
  }

  if (!isDrawing)
    return;

  // Restore original F0 values from drawing edits
  if (owner_.project && !drawingEdits.empty()) {
    auto &audioData = owner_.project->getAudioData();
    for (const auto &e : drawingEdits) {
      if (e.idx >= 0 && e.idx < static_cast<int>(audioData.f0.size())) {
        audioData.f0[e.idx] = e.oldF0;
      }
      if (e.idx >= 0 &&
          e.idx < static_cast<int>(audioData.deltaPitch.size())) {
        audioData.deltaPitch[e.idx] = e.oldDelta;
      }
      if (e.idx >= 0 &&
          e.idx < static_cast<int>(audioData.voicedMask.size())) {
        audioData.voicedMask[e.idx] = e.oldVoiced;
      }
    }
  }

  isDrawing = false;
  isPendingDraw = false;
  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  activeDrawCurve = nullptr;
  drawCurves.clear();

  owner_.repaint();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void DrawHandler::applyPitchDrawing(float x, float y) {
  if (!owner_.project)
    return;

  auto &audioData = owner_.project->getAudioData();
  if (audioData.f0.empty())
    return;

  double time = owner_.xToTime(x);
  // Compensate for centering offset used in display
  float midi = owner_.yToMidi(y - owner_.pixelsPerSemitone * 0.5f);
  // Remove global pitch offset so drawing maps to what is shown on screen
  if (owner_.project)
    midi -= owner_.project->getGlobalPitchOffset();
  int frameIndex =
      static_cast<int>(secondsToFrames(static_cast<float>(time)));
  int midiCents = static_cast<int>(std::round(midi * 100.0f));
  applyPitchPoint(frameIndex, midiCents);
}

void DrawHandler::commitPitchDrawing() {
  if (drawingEdits.empty())
    return;

  // Calculate the dirty frame range from the changes
  int minFrame = std::numeric_limits<int>::max();
  int maxFrame = std::numeric_limits<int>::min();
  for (const auto &e : drawingEdits) {
    minFrame = std::min(minFrame, e.idx);
    maxFrame = std::max(maxFrame, e.idx);
  }

  // Clear deltaPitch for notes in the edited range so they use the drawn F0
  if (owner_.project && minFrame <= maxFrame) {
    const int maxFrameExclusive = maxFrame + 1;
    auto &notes = owner_.project->getNotes();
    for (auto &note : notes) {
      if (note.getEndFrame() > minFrame &&
          note.getStartFrame() < maxFrameExclusive) {
        if (note.hasDeltaPitch()) {
          note.setDeltaPitch(std::vector<float>());
        }
      }
    }
  }

  // Set F0 dirty range in project for incremental synthesis
  if (owner_.project && minFrame <= maxFrame) {
    owner_.project->setF0DirtyRange(minFrame, maxFrame + 1);
  }

  // Create undo action
  if (owner_.undoManager && owner_.project) {
    auto &audioData = owner_.project->getAudioData();
    auto *projectPtr = owner_.project;
    auto action = std::make_unique<F0EditAction>(
        &audioData.f0, &audioData.deltaPitch, &audioData.voicedMask,
        drawingEdits, [projectPtr](int minFrame, int maxFrame) {
          if (projectPtr)
            projectPtr->setF0DirtyRange(minFrame, maxFrame + 1);
        });
    owner_.undoManager->addAction(std::move(action));
  }

  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  activeDrawCurve = nullptr;
  drawCurves.clear();

  // Trigger synthesis
  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();
}

void DrawHandler::applyPitchPoint(int frameIndex, int midiCents) {
  if (!owner_.project)
    return;

  auto &audioData = owner_.project->getAudioData();
  if (audioData.f0.empty())
    return;

  const int f0Size = static_cast<int>(audioData.f0.size());
  if (audioData.deltaPitch.size() < audioData.f0.size())
    audioData.deltaPitch.resize(audioData.f0.size(), 0.0f);
  if (audioData.basePitch.size() < audioData.f0.size())
    audioData.basePitch.resize(audioData.f0.size(), 0.0f);
  if (frameIndex < 0 || frameIndex >= f0Size)
    return;

  // Only start a new curve if there's no active curve (first point of drawing)
  if (!activeDrawCurve) {
    startNewPitchCurve(frameIndex, midiCents);
    // First point of the new curve: apply and exit
    auto applyFrameFirst = [&](int idx, int cents) {
      const float newFreq = midiToFreq(static_cast<float>(cents) / 100.0f);
      const float oldF0 = audioData.f0[idx];
      const float oldDelta =
          (idx < static_cast<int>(audioData.deltaPitch.size()))
              ? audioData.deltaPitch[idx]
              : 0.0f;
      const bool oldVoiced =
          (idx < static_cast<int>(audioData.voicedMask.size()))
              ? audioData.voicedMask[idx]
              : false;

      float baseMidi = (idx < static_cast<int>(audioData.basePitch.size()))
                           ? audioData.basePitch[static_cast<size_t>(idx)]
                           : 0.0f;
      float newMidi = static_cast<float>(cents) / 100.0f;
      float newDelta = newMidi - baseMidi;

      auto it = drawingEditIndexByFrame.find(idx);
      if (it == drawingEditIndexByFrame.end()) {
        drawingEditIndexByFrame.emplace(idx, drawingEdits.size());
        drawingEdits.push_back(F0FrameEdit{idx, oldF0, newFreq, oldDelta,
                                           newDelta, oldVoiced, true});
        // Clear deltaPitch for any note containing this frame
        auto &notes = owner_.project->getNotes();
        for (auto &note : notes) {
          if (note.getStartFrame() <= idx && note.getEndFrame() > idx &&
              note.hasDeltaPitch()) {
            note.setDeltaPitch(std::vector<float>());
            break;
          }
        }
      } else {
        auto &e = drawingEdits[it->second];
        e.newF0 = newFreq;
        e.newDelta = newDelta;
        e.newVoiced = true;
      }

      audioData.f0[idx] = newFreq;
      if (idx < static_cast<int>(audioData.deltaPitch.size())) {
        audioData.deltaPitch[static_cast<size_t>(idx)] = newDelta;
      }
      if (idx < static_cast<int>(audioData.voicedMask.size()))
        audioData.voicedMask[idx] = true;
    };
    applyFrameFirst(frameIndex, midiCents);
    return;
  }

  auto applyFrame = [&](int idx, int cents) {
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
    if (it == drawingEditIndexByFrame.end()) {
      drawingEditIndexByFrame.emplace(idx, drawingEdits.size());
      drawingEdits.push_back(F0FrameEdit{idx, oldF0, newFreq, oldDelta,
                                         newDelta, oldVoiced, true});

      // Clear deltaPitch for any note containing this frame
      auto &notes = owner_.project->getNotes();
      for (auto &note : notes) {
        if (note.getStartFrame() <= idx && note.getEndFrame() > idx &&
            note.hasDeltaPitch()) {
          note.setDeltaPitch(std::vector<float>());
          break;
        }
      }
    } else {
      auto &e = drawingEdits[it->second];
      e.newF0 = newFreq;
      e.newDelta = newDelta;
      e.newVoiced = true;
    }

    audioData.f0[idx] = newFreq;
    if (idx < static_cast<int>(audioData.deltaPitch.size())) {
      audioData.deltaPitch[static_cast<size_t>(idx)] = newDelta;
    }
    if (idx < static_cast<int>(audioData.voicedMask.size()))
      audioData.voicedMask[idx] = true;
  };

  auto appendValue = [&](int idx, int cents) {
    if (!activeDrawCurve)
      return;

    const int curveStart = activeDrawCurve->localStart();
    auto &vals = activeDrawCurve->mutableValues();

    // Handle backward drawing: prepend values if idx < curveStart
    if (idx < curveStart) {
      const int prependCount = curveStart - idx;
      std::vector<int> newVals(static_cast<size_t>(prependCount), cents);
      newVals.insert(newVals.end(), vals.begin(), vals.end());
      activeDrawCurve->setValues(std::move(newVals));
      activeDrawCurve->setLocalStart(idx);
      return;
    }

    const int offset = idx - curveStart;
    if (offset < static_cast<int>(vals.size())) {
      vals[static_cast<std::size_t>(offset)] = cents;
      return;
    }

    while (static_cast<int>(vals.size()) < offset) {
      int fill = vals.empty() ? cents : vals.back();
      vals.push_back(fill);
    }
    vals.push_back(cents);
  };

  if (lastDrawFrame < 0) {
    appendValue(frameIndex, midiCents);
    applyFrame(frameIndex, midiCents);
  } else {
    int start = lastDrawFrame;
    int end = frameIndex;
    int startVal = lastDrawValueCents;
    int endVal = midiCents;

    if (start == end) {
      appendValue(frameIndex, midiCents);
      applyFrame(frameIndex, midiCents);
    } else {
      int step = (end > start) ? 1 : -1;
      int length = std::abs(end - start);
      for (int i = 0; i <= length; ++i) {
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

void DrawHandler::startNewPitchCurve(int frameIndex, int midiCents) {
  drawCurves.push_back(std::make_unique<DrawCurve>(frameIndex, 1));
  activeDrawCurve = drawCurves.back().get();
  activeDrawCurve->appendValue(midiCents);
  lastDrawFrame = frameIndex;
  lastDrawValueCents = midiCents;
}
