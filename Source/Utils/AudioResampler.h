#pragma once

#include "../JuceHeader.h"

namespace AudioResampler {

// High-quality, band-limited sample-rate conversion for complete buffers.
// This function may allocate and must not be called from the audio thread.
juce::AudioBuffer<float>
resample(const juce::AudioBuffer<float> &input, double sourceSampleRate,
         double targetSampleRate, int targetNumSamples = -1);

} // namespace AudioResampler
