#pragma once

#include "BreathSibilanceDetector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

/**
 * Where a note's audio goes when the note is made longer or shorter.
 *
 * A note plays source frames [sourceStart, sourceEnd) of the recording over
 * destination frames [destinationStart, destinationEnd) of the timeline.
 * Consonants keep their length: a singer holds the vowel longer and leaves
 * the consonants as they are, so the vowel - the voiced frames - takes up the
 * whole change, and everything unvoiced (consonants, closures, a breath) is
 * moved without being stretched. Squeezed, a note shortens its vowel first,
 * down to 30% of it; only past that do the consonants shrink with it. A note
 * without a vowel stretches evenly, as every note used to.
 *
 * Positions are continuous frame coordinates in which frame k spans
 * [k, k + 1). Destination frame d reads the source at sourceAt(d + 0.5) - 0.5,
 * the frame-centre convention the synthesizer already used. With whole-frame
 * bounds and consonants at full length, every consonant frame lands on a
 * whole destination frame and plays exactly one source frame
 * (unscaledSourceFrame), so it can keep the recording.
 *
 * JUCE-free. NoteTiming.h builds the map for a Note.
 */
namespace notetiming
{
struct Config
{
  // A squeezed note shortens its vowel first, down to this share of it (and
  // to no fewer than minVowelFrames); past that, the consonants shrink too.
  double minVowelShare = 0.3;
  // Voicing gaps this short inside a vowel stay part of it (as in NoteEdges).
  int dropoutFrames = 2;
  // Voiced runs shorter than this are a voicing blip, not a vowel to hold.
  int minVowelFrames = 3;
};

class TimeMap
{
public:
  struct Segment
  {
    double sourceStart = 0.0;
    double sourceEnd = 0.0;
    double destinationStart = 0.0;
    double destinationEnd = 0.0;
    bool vowel = false;
  };

  TimeMap() = default;

  /** The source range spread evenly over the destination range. */
  static TimeMap uniform(double sourceStart, double sourceEnd,
                         double destinationStart, double destinationEnd)
  {
    TimeMap map;
    map.segments_.push_back(
        {sourceStart, sourceEnd, destinationStart, destinationEnd, false});
    return map;
  }

  /**
   * @param voiced  per-frame voicing (AudioData::voicedMask), absolute frames
   * @param labels  breathsibilance labels per frame; may be empty. A voicing
   *                gap that holds noise is a consonant, not a dropout.
   */
  static TimeMap build(const std::vector<bool> &voiced,
                       const std::vector<std::uint8_t> &labels,
                       int sourceStart, int sourceEnd, double destinationStart,
                       double destinationEnd, const Config &config = {})
  {
    const int sourceLength = sourceEnd - sourceStart;
    const double destinationLength = destinationEnd - destinationStart;
    // Not stretched (in place, or only moved): every frame plays its own
    // source frame, vowel or not.
    if (sourceLength <= 0 || !(destinationLength > 0.0) ||
        destinationLength == static_cast<double>(sourceLength))
      return uniform(sourceStart, sourceEnd, destinationStart, destinationEnd);

    // Vowel frames: voiced, with short dropouts filled.
    std::vector<std::uint8_t> vowel(static_cast<size_t>(sourceLength), 0);
    for (int i = 0, last = -1; i < sourceLength; ++i)
    {
      const int frame = sourceStart + i;
      if (frame < 0 || frame >= static_cast<int>(voiced.size()) ||
          !voiced[static_cast<size_t>(frame)])
        continue;
      vowel[static_cast<size_t>(i)] = 1;
      const int gap = i - last - 1;
      if (last >= 0 && gap > 0 && gap <= config.dropoutFrames)
      {
        bool noise = false;
        for (int g = last + 1; g < i; ++g)
          noise = noise ||
                  breathsibilance::isNoise(
                      breathsibilance::labelAt(labels, sourceStart + g));
        if (!noise)
          std::fill(vowel.begin() + last + 1, vowel.begin() + i, 1);
      }
      last = i;
    }

    struct Run
    {
      int start = 0;
      int end = 0;
      bool vowel = false;
    };
    std::vector<Run> runs;
    for (int i = 0; i < sourceLength;)
    {
      const bool isVowel = vowel[static_cast<size_t>(i)] != 0;
      int j = i;
      while (j < sourceLength && (vowel[static_cast<size_t>(j)] != 0) == isVowel)
        ++j;
      // A blip of voicing is held like the consonants around it.
      const bool holds = isVowel && j - i >= config.minVowelFrames;
      if (!runs.empty() && runs.back().vowel == holds)
        runs.back().end = sourceStart + j;
      else
        runs.push_back({sourceStart + i, sourceStart + j, holds});
      i = j;
    }

    int vowelFrames = 0;
    for (const auto &run : runs)
      if (run.vowel)
        vowelFrames += run.end - run.start;
    const int consonantFrames = sourceLength - vowelFrames;
    if (vowelFrames == 0 || consonantFrames == 0)
      return uniform(sourceStart, sourceEnd, destinationStart, destinationEnd);

    double consonantLength = consonantFrames;
    double vowelLength = destinationLength - consonantLength;
    const double vowelFloor =
        std::max(vowelFrames * config.minVowelShare,
                 static_cast<double>(
                     std::min(vowelFrames, config.minVowelFrames)));
    if (vowelLength < vowelFloor)
    {
      // Squeezed past the vowel's floor: everything shrinks together.
      const double scale = destinationLength / (consonantFrames + vowelFloor);
      consonantLength = consonantFrames * scale;
      vowelLength = vowelFloor * scale;
    }
    const double consonantScale = consonantLength / consonantFrames;
    const double vowelScale = vowelLength / vowelFrames;

    std::vector<double> lengths(runs.size());
    for (size_t k = 0; k < runs.size(); ++k)
      lengths[k] = (runs[k].end - runs[k].start) *
                   (runs[k].vowel ? vowelScale : consonantScale);

    // Whole frames for every run where the bounds are whole and consonants
    // keep their length, so each consonant lands on whole frames: the vowel
    // runs share the vowel's length by largest remainder.
    const bool whole = consonantScale == 1.0 &&
                       destinationStart == std::floor(destinationStart) &&
                       destinationLength == std::floor(destinationLength);
    if (whole)
    {
      const long target = std::lround(vowelLength);
      long assigned = 0;
      std::vector<std::pair<double, size_t>> remainders;
      for (size_t k = 0; k < runs.size(); ++k)
      {
        if (!runs[k].vowel)
          continue;
        const double down = std::floor(lengths[k]);
        remainders.push_back({lengths[k] - down, k});
        lengths[k] = down;
        assigned += static_cast<long>(down);
      }
      std::stable_sort(remainders.begin(), remainders.end(),
                       [](const auto &a, const auto &b)
                       { return a.first > b.first; });
      for (size_t r = 0;
           r < remainders.size() && assigned < target; ++r, ++assigned)
        lengths[remainders[r].second] += 1.0;
    }

    TimeMap map;
    double position = destinationStart;
    for (size_t k = 0; k < runs.size(); ++k)
    {
      map.segments_.push_back({static_cast<double>(runs[k].start),
                               static_cast<double>(runs[k].end), position,
                               position + lengths[k], runs[k].vowel});
      position += lengths[k];
    }
    map.segments_.back().destinationEnd = destinationEnd;
    return map;
  }

  /** Source position played at destination position `destination`. */
  double sourceAt(double destination) const
  {
    if (segments_.empty())
      return destination;
    destination = std::clamp(destination, segments_.front().destinationStart,
                             segments_.back().destinationEnd);
    for (const auto &segment : segments_)
    {
      const double length = segment.destinationEnd - segment.destinationStart;
      if (length <= 0.0 || (destination >= segment.destinationEnd &&
                            &segment != &segments_.back()))
        continue;
      return segment.sourceStart + (destination - segment.destinationStart) *
                                       (segment.sourceEnd -
                                        segment.sourceStart) /
                                       length;
    }
    return segments_.back().sourceEnd;
  }

  /** Destination position at which source position `source` plays. */
  double destinationAt(double source) const
  {
    if (segments_.empty())
      return source;
    source = std::clamp(source, segments_.front().sourceStart,
                        segments_.back().sourceEnd);
    for (const auto &segment : segments_)
    {
      const double length = segment.sourceEnd - segment.sourceStart;
      if (length <= 0.0 ||
          (source >= segment.sourceEnd && &segment != &segments_.back()))
        continue;
      return segment.destinationStart +
             (source - segment.sourceStart) *
                 (segment.destinationEnd - segment.destinationStart) / length;
    }
    return segments_.back().destinationEnd;
  }

  /** Fractional source frame read for destination frame `frame`. */
  double sourceFrameAt(int frame) const
  {
    return sourceAt(static_cast<double>(frame) + 0.5) - 0.5;
  }

  /**
   * The source frame that destination frame `frame` plays one to one, or -1
   * where the note is stretched or squeezed there.
   */
  int unscaledSourceFrame(int frame) const
  {
    const double centre = static_cast<double>(frame) + 0.5;
    for (const auto &segment : segments_)
    {
      if (centre < segment.destinationStart || centre >= segment.destinationEnd)
        continue;
      if (segment.destinationEnd - segment.destinationStart !=
              segment.sourceEnd - segment.sourceStart ||
          segment.destinationStart != std::floor(segment.destinationStart))
        return -1;
      return static_cast<int>(segment.sourceStart) + frame -
             static_cast<int>(segment.destinationStart);
    }
    return -1;
  }

  const std::vector<Segment> &segments() const { return segments_; }

private:
  std::vector<Segment> segments_;
};
} // namespace notetiming
