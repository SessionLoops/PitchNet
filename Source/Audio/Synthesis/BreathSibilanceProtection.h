#pragma once

#include "../BreathSibilanceDetector.h"
#include "../NoteTimeMap.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace breathsibilance
{
/**
 * Keep detected breaths and sibilants on the original side of the
 * synthesizer's blend mask (per sample, 1 = synthesized, 0 = original).
 *
 * They carry no pitch to correct, so the recording is kept at any run length
 * rather than regenerated: the vocoder reproduces an "s" closely but not
 * exactly (about 1 dB duller above 3.5 kHz on vocadito, see
 * docs/breath-sibilance-detection.md).
 *
 * `mask` covers frames [startFrame, startFrame + numFrames) at hopSize
 * samples per frame; `labels` is indexed by absolute frame. A label run is
 * aligned to its frames' centres (frame k covers k +- 1/2 hop, as analysed)
 * and both fades lie inside it, so the crossfade only mixes noise with
 * noise - never the original voice with the shifted one, which combs. A run
 * cut by the range continues beyond it and is not faded at that end.
 */
inline void protectNoiseInBlendMask(std::vector<float> &mask,
                                    const std::vector<std::uint8_t> &labels,
                                    int startFrame, int numFrames, int hopSize,
                                    int fadeSamples = 256)
{
  if (labels.empty() || numFrames <= 0 || hopSize <= 0)
    return;

  const int numSamples = static_cast<int>(mask.size());
  const int halfHop = hopSize / 2;
  int frame = 0;
  while (frame < numFrames)
  {
    if (!isNoise(labelAt(labels, startFrame + frame)))
    {
      ++frame;
      continue;
    }
    const int runStart = frame;
    while (frame < numFrames && isNoise(labelAt(labels, startFrame + frame)))
      ++frame;
    const int runEnd = frame;

    const bool fadeIn = runStart > 0;
    const bool fadeOut = runEnd < numFrames;
    const int first =
        fadeIn ? std::clamp(runStart * hopSize - halfHop, 0, numSamples) : 0;
    const int last =
        fadeOut ? std::clamp(runEnd * hopSize - halfHop, 0, numSamples)
                : numSamples;
    const int length = last - first;
    if (length <= 0)
      continue;

    const int fade = std::min(fadeSamples, length / 4);
    for (int sample = first; sample < last; ++sample)
    {
      float keepOriginal = 1.0f;
      if (fadeIn && fade > 0 && sample < first + fade)
        keepOriginal = static_cast<float>(sample - first + 1) /
                       static_cast<float>(fade + 1);
      if (fadeOut && fade > 0 && sample >= last - fade)
        keepOriginal =
            std::min(keepOriginal, static_cast<float>(last - sample) /
                                       static_cast<float>(fade + 1));
      mask[static_cast<size_t>(sample)] *= 1.0f - keepOriginal;
    }
  }
}

/**
 * The same for a note that was moved or stretched in time, which otherwise
 * goes through the synthesizer whole.
 *
 * Its breaths and sibilants are not stretched (NoteTimeMap.h): each of those
 * destination frames plays one recorded frame, so the recording is copied
 * from where it was recorded into `original` - the original side of the
 * blend, the samples of frames [startFrame, startFrame + numFrames) - and the
 * mask keeps it, fading inside each run as protectNoiseInBlendMask does.
 * Frames a squeezed note shrinks, and everything voiced, stay synthesized.
 *
 * @param map        the note's map; [noteStart, noteEnd) its destination
 * @param recording  the source recording, indexed by absolute sample
 */
inline void protectMovedNoiseInBlend(std::vector<float> &mask,
                                     std::vector<float> &original,
                                     const float *recording,
                                     int recordingSamples,
                                     const std::vector<std::uint8_t> &labels,
                                     const notetiming::TimeMap &map,
                                     int noteStart, int noteEnd,
                                     int startFrame, int numFrames,
                                     int hopSize, int fadeSamples = 256)
{
  if (recording == nullptr || labels.empty() || numFrames <= 0 ||
      hopSize <= 0)
    return;

  const int numSamples =
      static_cast<int>(std::min(mask.size(), original.size()));
  const int halfHop = hopSize / 2;
  const int rangeEnd = startFrame + numFrames;
  const int firstFrame = std::max(noteStart, startFrame);
  const int lastFrame = std::min(noteEnd, rangeEnd);
  // The note's own samples, relative to the range: a run is kept inside them.
  const int noteFirstSample = (noteStart - startFrame) * hopSize;
  const int noteLastSample = (noteEnd - startFrame) * hopSize;

  // Destination frame -> the recorded noise frame it plays, or -1.
  const auto noiseSource = [&](int frame)
  {
    const int source = map.unscaledSourceFrame(frame);
    return source >= 0 && isNoise(labelAt(labels, source)) ? source : -1;
  };

  int frame = firstFrame;
  while (frame < lastFrame)
  {
    const int source = noiseSource(frame);
    if (source < 0)
    {
      ++frame;
      continue;
    }
    const int shift = frame - source;
    const int runStart = frame;
    while (frame < lastFrame && noiseSource(frame) == frame - shift)
      ++frame;
    const int runEnd = frame;

    // Frame k covers k * hop +- hop / 2, as analysed (see above).
    const bool fadeIn = runStart > startFrame;
    const bool fadeOut = runEnd < rangeEnd;
    const int first = std::clamp(
        fadeIn ? std::max((runStart - startFrame) * hopSize - halfHop,
                          noteFirstSample)
               : 0,
        0, numSamples);
    const int last = std::clamp(
        fadeOut ? std::min((runEnd - startFrame) * hopSize - halfHop,
                           noteLastSample)
                : numSamples,
        0, numSamples);
    const int length = last - first;
    if (length <= 0)
      continue;

    const int fade = std::min(fadeSamples, length / 4);
    const long long offset =
        static_cast<long long>(startFrame - shift) * hopSize;
    for (int sample = first; sample < last; ++sample)
    {
      const long long recorded = offset + sample;
      if (recorded < 0 || recorded >= recordingSamples)
        continue;
      float keepOriginal = 1.0f;
      if (fadeIn && fade > 0 && sample < first + fade)
        keepOriginal = static_cast<float>(sample - first + 1) /
                       static_cast<float>(fade + 1);
      if (fadeOut && fade > 0 && sample >= last - fade)
        keepOriginal =
            std::min(keepOriginal, static_cast<float>(last - sample) /
                                       static_cast<float>(fade + 1));
      original[static_cast<size_t>(sample)] =
          recording[static_cast<size_t>(recorded)];
      mask[static_cast<size_t>(sample)] *= 1.0f - keepOriginal;
    }
  }
}
} // namespace breathsibilance
