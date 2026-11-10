#pragma once

#include "Project.h"
#include "../Audio/NoteTiming.h"
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
    // The recorded frames the note plays at the cuts, through its time map
    // (a stretched note holds its vowel), and where each curve stored with
    // the note divides there: at the cut for a curve written at the note's
    // current length, at the recorded frame otherwise.
    const auto map = notetiming::forNote(audio, note);
    const int sourceLeft = static_cast<int>(std::lround(map.sourceAt(left)));
    const int sourceRight = static_cast<int>(std::lround(map.sourceAt(right)));
    const auto slice = [&](const std::vector<float> &values) {
      const int size = static_cast<int>(values.size());
      const auto indexAt = [&](int frame, int sourceFrame) {
        return size == oldEnd - oldStart
                   ? frame - oldStart
                   : notetiming::curveIndexAtSource(audio, note, size, sourceFrame);
      };
      const int a = std::clamp(indexAt(left, sourceLeft), 0, size);
      const int b = std::clamp(indexAt(right, sourceRight), a, size);
      return std::vector<float>(values.begin() + a, values.begin() + b);
    };
    note.setDeltaPitch(slice(note.getDeltaPitch()));
    note.setOriginalDeltaPitch(slice(note.getOriginalDeltaPitch()));
    note.setBakedDeltaPitch(slice(note.getBakedDeltaPitch()));
    note.setF0Values(slice(note.getF0Values()));
    note.setSrcStartFrame(sourceLeft);
    note.setSrcEndFrame(sourceRight);
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
