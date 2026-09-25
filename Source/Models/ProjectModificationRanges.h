#pragma once

#include "../JuceHeader.h"
#include "../Utils/Constants.h"
#include "Project.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Sample ranges within an ARA audio modification.
//
// An ARA project is stored in MODIFICATION time: sample zero is the start of
// the audio modification, not the start of the playback region and not the
// start of the host timeline. A modification may be referenced by several
// playback regions at different timeline positions, so region-relative and
// timeline-relative sample indices are both ambiguous here - only modification
// samples identify a position unambiguously.
//
// Everything below therefore speaks modification samples, and the two
// functions must agree on that or edits land in the wrong place.

using SampleRange = juce::Range<int>;

// Frames dirtied by editing, converted to modification samples and merged into
// disjoint ranges.
//
// This previously subtracted the playback region's timeline start, producing
// region-relative samples, while its consumer below indexes a buffer that
// spans the whole modification. The ranges were displaced by the region's
// offset, so a freshly synthesised edit was preserved where nothing had
// changed and the actually edited samples were overwritten with stale audio.
// A fallback ("if the region-relative time is negative, use the absolute
// time") hid this for edits before the region start, which is why only the
// right-hand slice of a split ever misbehaved.
inline std::vector<SampleRange>
collectDirtyModificationSampleRanges(const Project &project,
                                     double sampleRate) {
  std::vector<SampleRange> ranges;
  if (sampleRate <= 0.0)
    return ranges;

  const auto toModificationSample = [](int frame) {
    return static_cast<int>(std::max<juce::int64>(
        0,
        static_cast<juce::int64>(frame) * static_cast<juce::int64>(HOP_SIZE)));
  };

  for (const auto &note : project.getNotes()) {
    if (!note.isDirty())
      continue;
    const int start = toModificationSample(note.getStartFrame());
    const int end = toModificationSample(note.getEndFrame());
    if (end > start)
      ranges.emplace_back(start, end);
  }

  if (project.hasF0DirtyRange()) {
    const auto [startFrame, endFrame] = project.getF0DirtyRange();
    const int start = toModificationSample(startFrame);
    const int end = toModificationSample(endFrame);
    if (end > start)
      ranges.emplace_back(start, end);
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const auto &a, const auto &b) {
              return a.getStart() < b.getStart();
            });

  std::vector<SampleRange> merged;
  for (const auto &range : ranges) {
    if (merged.empty() || range.getStart() > merged.back().getEnd()) {
      merged.push_back(range);
      continue;
    }
    merged.back() = merged.back().getUnionWith(range);
  }

  return merged;
}

// Carry unchanged audio forward from the previously published render, so a
// partial resynthesis does not discard edits outside the ranges it rebuilt.
//
// changedRanges is indexed in the REPLACEMENT buffer's own samples; both
// buffers are located in modification time by their start offsets, which is
// what lets the two be at different rates or offsets.
inline void preserveProcessedAudioOutsideRanges(
    juce::AudioBuffer<float> &replacement, double replacementRate,
    juce::int64 replacementStartInModification,
    const juce::AudioBuffer<float> &previous, double previousRate,
    juce::int64 previousStartInModification,
    const std::vector<SampleRange> &changedRanges) {
  if (replacement.getNumSamples() <= 0 || previous.getNumSamples() <= 0 ||
      replacementRate <= 0.0 || previousRate <= 0.0 || changedRanges.empty())
    return;

  const int channels =
      std::min(replacement.getNumChannels(), previous.getNumChannels());
  if (channels <= 0)
    return;

  const auto isChanged = [&changedRanges](int replacementSample) {
    for (const auto &range : changedRanges)
      if (range.contains(replacementSample))
        return true;
    return false;
  };

  const double rateRatio = previousRate / replacementRate;
  for (int dst = 0; dst < replacement.getNumSamples(); ++dst) {
    if (isChanged(dst))
      continue;

    const auto modificationSample =
        replacementStartInModification + static_cast<juce::int64>(dst);
    const double previousSamplePosition =
        static_cast<double>(modificationSample - previousStartInModification) *
        rateRatio;
    const int previousSample =
        static_cast<int>(std::llround(previousSamplePosition));
    if (previousSample < 0 || previousSample >= previous.getNumSamples())
      continue;

    for (int ch = 0; ch < channels; ++ch)
      replacement.setSample(ch, dst, previous.getSample(ch, previousSample));
  }
}
