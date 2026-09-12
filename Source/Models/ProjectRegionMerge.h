#pragma once

#include "ProjectRegionSlice.h"

struct ProjectPlaybackPart {
  const Project *project;
  double start, end;
};

// Merge only a complete, non-overlapping partition. All projects are already
// anchored to host time; concatenating their padded buffers would shift notes.
inline std::unique_ptr<Project> mergeProjectPlaybackParts(
    const std::vector<ProjectPlaybackPart> &parts, double start, double end) {
  if (parts.size() < 2 || !parts.front().project)
    return nullptr;
  const int rate = parts.front().project->getAudioData().sampleRate;
  double next = start;
  for (const auto &part : parts) {
    if (!part.project || part.project->getAudioData().sampleRate != rate ||
        std::abs(part.start - next) > 1.0e-6 || part.end <= part.start)
      return nullptr;
    next = part.end;
  }
  if (rate <= 0 || start < 0 || std::abs(next - end) > 1.0e-6)
    return nullptr;

  auto result = std::make_unique<Project>(*parts.front().project);
  result->clearNotes();
  result->clearF0DirtyRange();
  auto &output = result->getAudioData();
  output.segmentChunkRanges.clear();
  output.segmentDebugChunks.clear();
  bool hasWaveform = true, hasOriginal = true, hasMel = true;
  for (const auto &part : parts) {
    Project clipped = *part.project;
    clipProjectToPlaybackRange(clipped, part.start, part.end);
    for (auto note : clipped.getNotes()) {
      // Per-region pitch/gain settings become per-note differences relative
      // to the surviving project's controls. Rendered audio is copied as-is.
      note.setPitchOffset(note.getPitchOffset() + part.project->getGlobalPitchOffset()
                          - result->getGlobalPitchOffset());
      note.setVolumeDb(note.getVolumeDb() + part.project->getVolume() - result->getVolume());
      result->addNote(std::move(note));
    }
    const auto &input = part.project->getAudioData();
    const int first = secondsToFrames(static_cast<float>(part.start));
    const int last = secondsToFrames(static_cast<float>(part.end));
    const auto overlay = [first, last](auto &dst, const auto &src) {
      const int limit = std::min(last, static_cast<int>(src.size()));
      if (limit <= first) return;
      dst.resize(std::max(dst.size(), static_cast<size_t>(limit)));
      std::copy(src.begin() + first, src.begin() + limit, dst.begin() + first);
    };
    overlay(output.rawF0, input.rawF0);
    overlay(output.cleanedF0, input.cleanedF0);
    overlay(output.denseF0, input.denseF0);
    overlay(output.f0, input.f0);
    overlay(output.baseF0, input.baseF0);
    overlay(output.basePitch, input.basePitch);
    overlay(output.deltaPitch, input.deltaPitch);
    overlay(output.voicedMask, input.voicedMask);
    overlay(output.vadMask, input.vadMask);
    overlay(output.melSpectrogram, input.melSpectrogram);
    hasMel = hasMel && !input.melSpectrogram.empty();
    const int begin = juce::roundToInt(part.start * rate);
    const int finish = juce::roundToInt(part.end * rate);
    const auto copyAudio = [begin, finish](auto &dst, const auto &src) {
      if (src.getNumSamples() < finish || src.getNumChannels() == 0) return false;
      dst.setSize(std::max(dst.getNumChannels(), src.getNumChannels()),
                  std::max(dst.getNumSamples(), finish), true, true);
      for (int ch = 0; ch < dst.getNumChannels(); ++ch)
        dst.copyFrom(ch, begin, src, std::min(ch, src.getNumChannels() - 1), begin, finish - begin);
      return true;
    };
    hasWaveform = copyAudio(output.waveform, input.waveform) && hasWaveform;
    hasOriginal = copyAudio(output.originalWaveform, input.originalWaveform) && hasOriginal;
    for (const auto &range : input.segmentChunkRanges)
      if (range.second > first && range.first < last)
        output.segmentChunkRanges.emplace_back(std::max(first, range.first), std::min(last, range.second));
    for (const auto &chunk : input.segmentDebugChunks)
      if (chunk.endFrame > first && chunk.startFrame < last)
        output.segmentDebugChunks.push_back(chunk);
    if (part.project->hasF0DirtyRange()) {
      const auto dirty = part.project->getF0DirtyRange();
      if (dirty.second > first && dirty.first < last)
        result->setF0DirtyRange(std::max(first, dirty.first), std::min(last, dirty.second));
    }
  }
  // A restored archive can be source-less. Keep its analysis and edits, then
  // let the normal ARA hydration path rebuild source buffers without analysis.
  if (!hasWaveform) output.waveform.setSize(0, 0);
  if (!hasOriginal) output.originalWaveform.setSize(0, 0);
  if (!hasMel) output.melSpectrogram.clear();
  output.timelineOffsetSeconds = start;
  output.playbackRegionRanges = {{start, end}};
  result->setModified(true);
  return result;
}
