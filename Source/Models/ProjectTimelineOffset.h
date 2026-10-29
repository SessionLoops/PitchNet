#pragma once

#include "Project.h"
#include "../Utils/Constants.h"
#include <algorithm>
#include <cmath>
#include <vector>

// Non-ARA captures are stored in absolute host-timeline coordinates: sample 0
// of every buffer (and frame 0 of every per-frame vector) is the song start,
// and timelineOffsetSeconds records where the captured content begins. A fresh
// analysis comes back local to zero, so it must be re-padded to the capture's
// host position before it is shown, played or persisted.
//
// Moves the content from the project's current offset to newOffsetSeconds by
// re-padding leading silence. Returns false when nothing changed. Pure data -
// safe to call with no editor attached (message thread, not the audio thread).
inline bool repadProjectToTimelineOffset(Project &project,
                                         double timelineOffsetSeconds) {
  auto &audioData = project.getAudioData();
  if (audioData.waveform.getNumSamples() == 0 || audioData.sampleRate <= 0) {
    audioData.timelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
    return false;
  }

  const double newOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
  const double oldOffsetSeconds = std::max(0.0, audioData.timelineOffsetSeconds);
  if (std::abs(newOffsetSeconds - oldOffsetSeconds) < 0.000001)
    return false;

  const int sampleRate = audioData.sampleRate;
  const int oldOffsetSamples = std::max(
      0, static_cast<int>(std::llround(oldOffsetSeconds * sampleRate)));
  const int newOffsetSamples = std::max(
      0, static_cast<int>(std::llround(newOffsetSeconds * sampleRate)));
  const int oldOffsetFrames = std::max(
      0, static_cast<int>(std::llround(oldOffsetSeconds * sampleRate /
                                       static_cast<double>(HOP_SIZE))));
  const int newOffsetFrames = std::max(
      0, static_cast<int>(std::llround(newOffsetSeconds * sampleRate /
                                       static_cast<double>(HOP_SIZE))));
  const int frameDelta = newOffsetFrames - oldOffsetFrames;

  auto repadBuffer = [&](juce::AudioBuffer<float> &buffer)
  {
    const int oldSamples = buffer.getNumSamples();
    if (oldSamples <= 0)
      return;

    const int contentStart = std::min(oldOffsetSamples, oldSamples);
    const int contentSamples = oldSamples - contentStart;
    const int newSamples = newOffsetSamples + contentSamples;
    juce::AudioBuffer<float> shifted(buffer.getNumChannels(), newSamples);
    shifted.clear();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
      shifted.copyFrom(ch, newOffsetSamples, buffer, ch, contentStart,
                       contentSamples);
    buffer = std::move(shifted);
  };

  auto repadFloatVector = [&](std::vector<float> &values)
  {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<float> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)));
    std::copy(values.begin() + contentStart, values.end(),
              shifted.begin() + newOffsetFrames);
    values = std::move(shifted);
  };

  auto repadBoolVector = [&](std::vector<bool> &values)
  {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<bool> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)), false);
    for (int i = contentStart; i < oldSize; ++i)
      shifted[static_cast<size_t>(newOffsetFrames + i - contentStart)] =
          values[static_cast<size_t>(i)];
    values = std::move(shifted);
  };

  auto repadMel = [&](std::vector<std::vector<float>> &frames)
  {
    const int oldSize = static_cast<int>(frames.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    const size_t numMels = oldSize > 0 ? frames[static_cast<size_t>(
                                             std::min(contentStart, oldSize - 1))]
                                             .size()
                                       : 0;
    std::vector<std::vector<float>> shifted(
        static_cast<size_t>(newOffsetFrames + std::max(0, oldSize - contentStart)),
        std::vector<float>(numMels, 0.0f));
    std::move(frames.begin() + contentStart, frames.end(),
              shifted.begin() + newOffsetFrames);
    frames = std::move(shifted);
  };

  repadBuffer(audioData.waveform);
  repadBuffer(audioData.originalWaveform);
  repadMel(audioData.melSpectrogram);
  repadFloatVector(audioData.rawF0);
  repadFloatVector(audioData.cleanedF0);
  repadFloatVector(audioData.denseF0);
  repadFloatVector(audioData.f0);
  repadFloatVector(audioData.baseF0);
  repadFloatVector(audioData.basePitch);
  repadFloatVector(audioData.deltaPitch);
  repadBoolVector(audioData.voicedMask);
  repadBoolVector(audioData.vadMask);

  auto shiftFrame = [frameDelta](int frame)
  {
    return std::max(0, frame + frameDelta);
  };

  for (auto &note : project.getNotes())
  {
    note.setStartFrame(shiftFrame(note.getStartFrame()));
    note.setEndFrame(shiftFrame(note.getEndFrame()));
    note.setSrcStartFrame(shiftFrame(note.getSrcStartFrame()));
    note.setSrcEndFrame(shiftFrame(note.getSrcEndFrame()));
    note.setRenderedFrames(shiftFrame(note.getRenderedStartFrame()),
                           shiftFrame(note.getRenderedEndFrame()));
  }

  for (auto &range : audioData.segmentChunkRanges)
  {
    range.first = shiftFrame(range.first);
    range.second = shiftFrame(range.second);
  }

  for (auto &chunk : audioData.segmentDebugChunks)
  {
    chunk.startFrame = shiftFrame(chunk.startFrame);
    chunk.endFrame = shiftFrame(chunk.endFrame);
    for (auto &event : chunk.events)
    {
      event.startFrame = shiftFrame(event.startFrame);
      event.endFrame = shiftFrame(event.endFrame);
      event.attachedStartFrame = shiftFrame(event.attachedStartFrame);
    }
  }

  audioData.timelineOffsetSeconds = newOffsetSeconds;
  return true;
}
