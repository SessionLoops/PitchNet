#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace VisualWaveformEnvelope
{
inline float calculateRms(const std::vector<double> &sumSquares,
                          int prefixStartSample, int startSample,
                          int endSample)
{
  const int localStart = startSample - prefixStartSample;
  const int localEnd = endSample - prefixStartSample;
  const double totalSquares =
      sumSquares[static_cast<size_t>(localEnd)] -
      sumSquares[static_cast<size_t>(localStart)];
  const int sampleCount = std::max(1, endSample - startSample);

  return static_cast<float>(std::sqrt(totalSquares / sampleCount));
}

inline void rejectSinglePointSpikes(std::vector<float> &values)
{
  if (values.size() < 5)
    return;

  std::vector<float> filtered(values);
  for (size_t i = 2; i + 2 < values.size(); ++i)
  {
    float window[5] = {values[i - 2], values[i - 1], values[i],
                       values[i + 1], values[i + 2]};
    std::sort(std::begin(window), std::end(window));
    filtered[i] = window[2];
  }

  values.swap(filtered);
}

inline void smoothAttackRelease(std::vector<float> &values)
{
  if (values.size() < 2)
    return;

  constexpr float attack = 0.7f;
  constexpr float release = 0.32f;

  float envelope = values.front();
  for (auto &value : values)
  {
    const float coefficient = value > envelope ? attack : release;
    envelope += (value - envelope) * coefficient;
    value = envelope;
  }

  envelope = values.back();
  for (auto it = values.rbegin(); it != values.rend(); ++it)
  {
    const float coefficient = *it > envelope ? attack : release;
    envelope += (*it - envelope) * coefficient;
    *it = (*it + envelope) * 0.5f;
  }
}

inline std::vector<float> build(const float *samples, int totalSamples,
                                int startSample, int endSample, int pointCount,
                                float displayWidthPixels, double sampleRate,
                                float pixelsPerSecond)
{
  std::vector<float> values(static_cast<size_t>(std::max(0, pointCount)), 0.0f);
  if (!samples || totalSamples <= 0 || pointCount <= 0 || endSample <= startSample ||
      displayWidthPixels <= 0.0f || sampleRate <= 0.0 || pixelsPerSecond <= 0.0f)
    return values;

  startSample = std::max(0, std::min(startSample, totalSamples - 1));
  endSample = std::max(startSample + 1, std::min(endSample, totalSamples));

  const double samplesPerPixel =
      static_cast<double>(endSample - startSample) / displayWidthPixels;
  const int windowSamples = std::max(
      1, static_cast<int>(std::round(std::max(sampleRate * 0.012,
                                              samplesPerPixel * 10.0))));
  const int halfWindowSamples = windowSamples / 2;
  const int prefixStartSample = std::max(0, startSample - halfWindowSamples - 1);
  const int prefixEndSample = std::min(totalSamples, endSample + halfWindowSamples + 1);

  std::vector<double> sumSquares(
      static_cast<size_t>(prefixEndSample - prefixStartSample + 1), 0.0);
  for (int sample = prefixStartSample; sample < prefixEndSample; ++sample)
  {
    const double value = static_cast<double>(samples[sample]);
    const auto localIndex = static_cast<size_t>(sample - prefixStartSample + 1);
    sumSquares[localIndex] = sumSquares[localIndex - 1] + value * value;
  }

  const float denominator = static_cast<float>(std::max(1, pointCount - 1));
  for (int point = 0; point < pointCount; ++point)
  {
    const double samplePosition =
        startSample +
        (static_cast<double>(point) / denominator) * (endSample - startSample);
    const int centerSample = static_cast<int>(std::round(samplePosition));
    const int windowStart =
        std::max(prefixStartSample, centerSample - halfWindowSamples);
    const int windowEnd =
        std::min(prefixEndSample, centerSample + halfWindowSamples + 1);

    values[static_cast<size_t>(point)] =
        std::min(1.0f, calculateRms(sumSquares, prefixStartSample, windowStart,
                                    windowEnd) *
                           1.75f);
  }

  rejectSinglePointSpikes(values);
  smoothAttackRelease(values);

  return values;
}
} // namespace VisualWaveformEnvelope
