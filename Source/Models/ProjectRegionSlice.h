#pragma once

#include "Project.h"
#include "../Utils/Constants.h"
#include <algorithm>

// Projects use absolute timeline frames. Preserve analysis and source buffers
// so time-edited notes can still reference their original source material.
inline void clipProjectToPlaybackRange(Project &project, double start, double end) {
  auto &audio = project.getAudioData();
  const int firstFrame = secondsToFrames(static_cast<float>(start));
  const int lastFrame = secondsToFrames(static_cast<float>(end));
  auto &notes = project.getNotes();
  notes.erase(std::remove_if(notes.begin(), notes.end(), [&](Note &note) {
    const int oldStart = note.getStartFrame(), oldEnd = note.getEndFrame();
    const int left = std::max(firstFrame, oldStart);
    const int right = std::min(lastFrame, oldEnd);
    if (right <= left)
      return true;
    if (left == oldStart && right == oldEnd)
      return false;
    const auto slice = [&](const std::vector<float> &values) {
      const int a = std::clamp(left - oldStart, 0, static_cast<int>(values.size()));
      const int b = std::clamp(right - oldStart, a, static_cast<int>(values.size()));
      return std::vector<float>(values.begin() + a, values.begin() + b);
    };
    const double scale = static_cast<double>(note.getSrcDurationFrames()) / (oldEnd - oldStart);
    const int srcStart = note.getSrcStartFrame();
    note.setSrcStartFrame(srcStart + static_cast<int>(std::lround((left - oldStart) * scale)));
    note.setSrcEndFrame(srcStart + static_cast<int>(std::lround((right - oldStart) * scale)));
    note.setDeltaPitch(slice(note.getDeltaPitch()));
    note.setOriginalDeltaPitch(slice(note.getOriginalDeltaPitch()));
    note.setBakedDeltaPitch(slice(note.getBakedDeltaPitch()));
    note.setF0Values(slice(note.getF0Values()));
    note.setStartFrame(left);
    note.setEndFrame(right);
    note.clearTimingPreview();
    note.setRenderedEdit(note.hasRenderedEdit());
    return false;
  }), notes.end());
  // Keep absolute analysis/source coordinates, including source material
  // referenced by time-edited notes. Only the output is bounded here.
  audio.timelineOffsetSeconds = start;
  audio.playbackRegionRanges = {{start, end}};
  if (audio.waveform.getNumSamples() > 0) {
    const int endSample = std::min(audio.waveform.getNumSamples(),
                                  juce::roundToInt(end * audio.sampleRate));
    audio.waveform.setSize(audio.waveform.getNumChannels(), endSample, true);
    audio.waveform.clear(0, std::clamp(juce::roundToInt(start * audio.sampleRate), 0, endSample));
  }
}
