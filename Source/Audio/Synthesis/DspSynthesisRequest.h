#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * One render request, shared by every non-model synthesis engine.
 *
 * Both DSP engines answer the same question - "produce the edited audio for
 * this frame range" - and both answer it by reading the original waveform
 * rather than regenerating it, so they take identical input. Two per-output-
 * frame curves describe the whole edit:
 *
 *   sourceFrame  where in the ORIGINAL audio this output frame reads from.
 *                Identity for a pitch-only edit; a note's retiming shows up
 *                here as a stretched or displaced run of values, which is how
 *                one pass follows an arbitrary time map.
 *   pitchRatio   target F0 over the source F0 at that same position.
 *
 * The remaining fields describe the source itself. PSOLA needs them to place
 * pitch marks; the phase vocoder ignores them.
 */
struct DspSynthesisRequest
{
  /// A slice of the original waveform (mono), wide enough to cover every
  /// analysis window or grain the curves ask for.
  std::vector<float> source;

  /// Sample index in the original audio that source[0] corresponds to.
  /// Positions in sourceFrame are absolute, so a retimed note can read
  /// outside the rendered range; this is what makes the slice addressable.
  int sourceStartSample = 0;

  /// Per output frame: fractional frame index into the original audio.
  std::vector<double> sourceFrame;

  /// Per output frame: linear frequency ratio (1.0 = unchanged).
  std::vector<float> pitchRatio;

  /// Per SOURCE frame, absolute indexing: the original pitch in Hz, dense
  /// (unvoiced gaps interpolated). PSOLA derives its period from this.
  std::vector<float> sourceF0;

  /// Per SOURCE frame, absolute indexing: whether that frame is voiced.
  /// PSOLA runs pitch-synchronous grains only inside voiced runs and falls
  /// back to fixed-grain overlap-add elsewhere, where a period is meaningless.
  std::vector<std::uint8_t> sourceVoiced;

  /// Frame grid the per-output-frame curves are expressed on (the mel hop).
  int hopSize = 512;

  /// Length of the render. Output is exactly numOutputFrames * hopSize.
  int numOutputFrames = 0;

  int sampleRate = 44100;

  /// Preserve formants rather than letting them ride the pitch shift.
  /// The phase vocoder counter-shifts its cepstral envelope to honour this;
  /// PSOLA gets it inherently and ignores the flag.
  bool preserveFormants = true;
};

namespace dspSynthesis {

/// Sample a per-output-frame source-frame curve at a fractional frame index,
/// extending past both ends by continuing the identity mapping. Frames of
/// pre- and post-roll need positions too, and an abrupt clamp there would
/// fold the read position back on itself and smear the region edges.
inline double sampleSourceFrame(const std::vector<double> &curve, double frame,
                                int numFrames)
{
  if (curve.empty() || numFrames <= 0)
    return frame;

  const int last = numFrames - 1;
  if (frame <= 0.0)
    return curve[0] + frame;
  if (frame >= static_cast<double>(last))
    return curve[static_cast<size_t>(last)] + (frame - static_cast<double>(last));

  const int left = static_cast<int>(std::floor(frame));
  const int right = std::min(left + 1, last);
  const double amount = frame - static_cast<double>(left);
  return curve[static_cast<size_t>(left)] * (1.0 - amount) +
         curve[static_cast<size_t>(right)] * amount;
}

/// Same idea for the ratio curve, but held flat outside the range: the
/// pre-roll should carry the region's own shift, not drift back to unity.
inline float samplePitchRatio(const std::vector<float> &curve, double frame,
                              int numFrames)
{
  if (curve.empty() || numFrames <= 0)
    return 1.0f;

  const int last = numFrames - 1;
  if (frame <= 0.0)
    return curve[0];
  if (frame >= static_cast<double>(last))
    return curve[static_cast<size_t>(last)];

  const int left = static_cast<int>(std::floor(frame));
  const int right = std::min(left + 1, last);
  const float amount = static_cast<float>(frame - static_cast<double>(left));
  return curve[static_cast<size_t>(left)] * (1.0f - amount) +
         curve[static_cast<size_t>(right)] * amount;
}

} // namespace dspSynthesis
