#include "PitchToolOperations.h"
#include "Constants.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace PitchToolOperations {

std::vector<float> tiltDeltaPitch(const std::vector<float>& deltaPitch,
                                  float pivotPosition,
                                  float amount) {
  if (deltaPitch.empty()) {
    return {};
  }

  std::vector<float> result(deltaPitch);
  if (deltaPitch.size() == 1) {
    return result;
  }

  const float clampedPivot = std::clamp(pivotPosition, 0.0f, 1.0f);
  const float maxDistance = std::max(clampedPivot, 1.0f - clampedPivot);
  if (maxDistance <= 0.0f) {
    return result;
  }

  const float invLastIndex = 1.0f / static_cast<float>(deltaPitch.size() - 1);
  for (size_t i = 0; i < deltaPitch.size(); ++i) {
    const float normalizedPosition = static_cast<float>(i) * invLastIndex;
    // Normalize by furthest edge distance so `amount` means a full-end shift.
    const float normalizedDistance =
        (normalizedPosition - clampedPivot) / maxDistance;
    result[i] = deltaPitch[i] + normalizedDistance * amount;
  }

  return result;
}

std::vector<float> scaleVibrato(const std::vector<float>& deltaPitch,
                                float factor) {
  if (deltaPitch.empty()) {
    return {};
  }

  std::vector<float> result(deltaPitch.size(), 0.0f);
  std::transform(deltaPitch.begin(), deltaPitch.end(), result.begin(),
                 [factor](float value) {
                   return value * factor;
                 });

  return result;
}

std::vector<float> smoothBoundary(const std::vector<float>& deltaPitch,
                                  int side,
                                  int transitionFrames,
                                  float targetPitch) {
  if (deltaPitch.empty()) {
    return {};
  }

  std::vector<float> result(deltaPitch);
  if (transitionFrames <= 0 || (side != 0 && side != 1)) {
    return result;
  }

  const int clampedFrames = std::max(
      1, std::min(transitionFrames, static_cast<int>(deltaPitch.size())));

  // Gaussian kernel: sigma = transitionFrames / 2.0
  // Weight at boundary = 1.0 (full target), weight at edge of transition = ~0.14
  const float sigma = static_cast<float>(clampedFrames) / 2.0f;
  const float invTwoSigmaSq = 1.0f / (2.0f * sigma * sigma);

  if (side == 0) {
    // Left boundary: blend FROM targetPitch TO note's internal curve
    for (int i = 0; i < clampedFrames; ++i) {
      // Distance from boundary (frame 0)
      const float dist = static_cast<float>(i);
      // Gaussian weight: 1.0 at boundary, decreasing toward interior
      const float gaussWeight = std::exp(-dist * dist * invTwoSigmaSq);
      // Blend: high gaussWeight = more targetPitch, low = more original
      result[static_cast<size_t>(i)] =
          targetPitch * gaussWeight + deltaPitch[static_cast<size_t>(i)] * (1.0f - gaussWeight);
    }
  } else {
    // Right boundary: blend FROM note's internal curve TO targetPitch
    const size_t startIndex = deltaPitch.size() - static_cast<size_t>(clampedFrames);
    for (int i = 0; i < clampedFrames; ++i) {
      // Distance from boundary (last frame)
      const float dist = static_cast<float>(clampedFrames - 1 - i);
      // Gaussian weight: 1.0 at boundary, decreasing toward interior
      const float gaussWeight = std::exp(-dist * dist * invTwoSigmaSq);
      const size_t index = startIndex + static_cast<size_t>(i);
      // Blend: high gaussWeight = more targetPitch, low = more original
      result[index] =
          targetPitch * gaussWeight + deltaPitch[index] * (1.0f - gaussWeight);
    }
  }

  return result;
}

std::vector<float> scalePitchDrift(const std::vector<float>& deltaPitch, float factor) {
  if (deltaPitch.size() < 2 || factor == 1.0f)
    return deltaPitch;

  // A symmetric Gaussian avoids phase delay. Its -3 dB cutoff is 2 Hz.
  // Normalize the truncated kernel at note edges instead of padding with zeros.
  const double sigma = std::sqrt(std::log(2.0)) / (2.0 * 3.141592653589793 * 2.0)
                     * SAMPLE_RATE / HOP_SIZE;
  const int radius = static_cast<int>(std::ceil(3.0 * sigma));
  std::vector<double> weights(static_cast<size_t>(radius + 1));
  for (int j = 0; j <= radius; ++j)
    weights[static_cast<size_t>(j)] = std::exp(-0.5 * j * j / (sigma * sigma));
  std::vector<float> trend(deltaPitch.size());
  for (int i = 0; i < static_cast<int>(deltaPitch.size()); ++i) {
    double sum = 0.0, weight = 0.0;
    for (int j = std::max(0, i - radius);
         j <= std::min(static_cast<int>(deltaPitch.size()) - 1, i + radius); ++j) {
      const double w = weights[static_cast<size_t>(std::abs(j - i))];
      sum += w * deltaPitch[static_cast<size_t>(j)];
      weight += w;
    }
    trend[static_cast<size_t>(i)] = static_cast<float>(sum / weight);
  }
  const float mean = computeMean(trend);
  auto result = deltaPitch;
  for (size_t i = 0; i < result.size(); ++i)
    result[i] += (factor - 1.0f) * (trend[i] - mean);
  return result;
}

float computeMean(const std::vector<float>& deltaPitch) {
  if (deltaPitch.empty()) {
    return 0.0f;
  }

  const float sum =
      std::accumulate(deltaPitch.begin(), deltaPitch.end(), 0.0f);
  return sum / static_cast<float>(deltaPitch.size());
}

std::vector<float> applyAllTransformations(const std::vector<float>& originalDelta,
                                           float tiltLeft,
                                           float tiltRight,
                                           float vibrato,
                                           int smoothLeftFrames,
                                           int smoothRightFrames,
                                           const AdjacentNoteContext& adjacentContext,
                                           float pitchDrift) {
  if (originalDelta.empty()) {
    return {};
  }

  // Start with the original pristine curve
  std::vector<float> result = originalDelta;

  // 1. Apply modulation before tilt, preserving the tilt ramp at 0%.
  if (std::abs(vibrato - 1.0f) > 0.001f) {
    result = scaleVibrato(result, vibrato);
  }

  // Adjust the slow trend after the existing whole-contour vibrato scaling.
  // This retains the established 0% vibrato = flat behavior.
  result = scalePitchDrift(result, pitchDrift);

  // 2. Apply tilt transformations (combined left + right)
  // TiltLeft: pivot at right (1.0), negative amount
  if (std::abs(tiltLeft) > 0.001f) {
    result = tiltDeltaPitch(result, 1.0f, -tiltLeft);
  }
  
  // TiltRight: pivot at left (0.0), positive amount
  if (std::abs(tiltRight) > 0.001f) {
    result = tiltDeltaPitch(result, 0.0f, tiltRight);
  }

  // 3. Apply boundary smoothing
  if (smoothLeftFrames > 0) {
    const float leftTarget = adjacentContext.hasLeft ? adjacentContext.leftBoundaryDelta : 0.0f;
    result = smoothBoundary(result, 0, smoothLeftFrames, leftTarget);
  }
  
  if (smoothRightFrames > 0) {
    const float rightTarget = adjacentContext.hasRight ? adjacentContext.rightBoundaryDelta : 0.0f;
    result = smoothBoundary(result, 1, smoothRightFrames, rightTarget);
  }

  return result;
}

} // namespace PitchToolOperations
