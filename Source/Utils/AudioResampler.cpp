#include "AudioResampler.h"

#include <CDSPResampler.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace AudioResampler {
namespace {

constexpr int maxInputBlockSize = 65536;
constexpr double transitionBandPercent = 2.5;

int calculateOutputLength(int inputSamples, double sourceSampleRate,
                          double targetSampleRate) {
  const double length = static_cast<double>(inputSamples) * targetSampleRate /
                        sourceSampleRate;
  if (!std::isfinite(length) || length <= 0.0 ||
      length > static_cast<double>(std::numeric_limits<int>::max()))
    return 0;

  return std::max(1, static_cast<int>(std::llround(length)));
}

} // namespace

juce::AudioBuffer<float>
resample(const juce::AudioBuffer<float> &input, double sourceSampleRate,
         double targetSampleRate, int targetNumSamples) {
  const int channels = input.getNumChannels();
  const int inputSamples = input.getNumSamples();
  if (channels <= 0 || inputSamples <= 0)
    return input;

  if (!std::isfinite(sourceSampleRate) || sourceSampleRate <= 0.0 ||
      !std::isfinite(targetSampleRate) || targetSampleRate <= 0.0)
    return input;

  const int outputSamples =
      targetNumSamples >= 0
          ? targetNumSamples
          : calculateOutputLength(inputSamples, sourceSampleRate,
                                  targetSampleRate);
  if (outputSamples <= 0)
    return {};

  if (juce::approximatelyEqual(sourceSampleRate, targetSampleRate) &&
      outputSamples == inputSamples)
    return input;

  juce::AudioBuffer<float> output(channels, outputSamples);
  output.clear();

  // r8brain's one-shot API takes a mutable input pointer, although it only
  // reads it. Keep a channel scratch copy so callers' buffers remain const.
  std::vector<float> channelInput(static_cast<size_t>(inputSamples));
  const int blockSize = std::max(1, std::min(inputSamples, maxInputBlockSize));

  for (int channel = 0; channel < channels; ++channel) {
    std::copy_n(input.getReadPointer(channel), inputSamples,
                channelInput.data());
    r8b::CDSPResampler24 converter(sourceSampleRate, targetSampleRate,
                                   blockSize, transitionBandPercent);
    converter.oneshot(channelInput.data(), inputSamples,
                      output.getWritePointer(channel), outputSamples);
  }

  return output;
}

} // namespace AudioResampler
