#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/**
 * A plain tone for a detected note, like Melodyne's monitoring synth in Note
 * Assignment Mode: it follows the note's own pitch curve (Hz per analysis
 * frame) and level (per frame, any scale; it is normalised), so a candidate or
 * a proposed pitch can be heard before any separated sound exists. A few
 * harmonics (1, 1/2, 1/3, 1/4) under Nyquist, 10 ms fade in, 30 ms fade out,
 * peaking at `peakGain`. Mono, at `sampleRate`, `hop` samples per frame.
 */
namespace notetone
{

inline std::vector<float> render(const std::vector<float> &f0Hz, const std::vector<float> &level,
                                 double sampleRate, int hop, float peakGain = 0.2f)
{
  const int frames = static_cast<int>(f0Hz.size());
  if (frames == 0 || sampleRate <= 0.0 || hop <= 0)
    return {};
  const int release = static_cast<int>(sampleRate * 0.03);
  const int length = frames * hop + release;
  std::vector<float> out(static_cast<size_t>(length), 0.0f);

  float levelPeak = 0.0f;
  for (float v : level)
    levelPeak = std::max(levelPeak, v);
  // Quiet stretches of the note stay audible, a little.
  const auto gainAt = [&](double frame) {
    if (levelPeak <= 0.0f || level.empty())
      return 1.0f;
    const int k = std::clamp(static_cast<int>(frame), 0, static_cast<int>(level.size()) - 1);
    return 0.25f + 0.75f * level[static_cast<size_t>(k)] / levelPeak;
  };
  const auto hzAt = [&](double frame) {
    const double clamped = std::clamp(frame, 0.0, static_cast<double>(frames - 1));
    const int k = static_cast<int>(clamped);
    const int k1 = std::min(frames - 1, k + 1);
    const double t = clamped - k;
    return static_cast<double>(f0Hz[static_cast<size_t>(k)]) * (1.0 - t) +
           static_cast<double>(f0Hz[static_cast<size_t>(k1)]) * t;
  };

  constexpr double kTwoPi = 6.283185307179586;
  const float harmonicGain[] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f};
  const float norm = peakGain / (1.0f + 0.5f + 1.0f / 3.0f + 0.25f);
  double phase = 0.0;
  const int fadeIn = std::max(1, static_cast<int>(sampleRate * 0.01));
  const int body = frames * hop;
  for (int i = 0; i < length; ++i)
  {
    const double frame = static_cast<double>(std::min(i, body - 1)) / hop;
    const double hz = hzAt(frame);
    phase += kTwoPi * hz / sampleRate;
    if (phase > kTwoPi * 1024.0)
      phase -= kTwoPi * 1024.0;
    double v = 0.0;
    for (int h = 1; h <= 4; ++h)
      if (hz * h < 0.45 * sampleRate)
        v += harmonicGain[h - 1] * std::sin(phase * h);
    float envelope = gainAt(frame);
    if (i < fadeIn)
      envelope *= static_cast<float>(i) / fadeIn;
    if (i >= body)
      envelope *= 1.0f - static_cast<float>(i - body) / std::max(1, release);
    out[static_cast<size_t>(i)] = static_cast<float>(v) * norm * envelope;
  }
  return out;
}

} // namespace notetone
