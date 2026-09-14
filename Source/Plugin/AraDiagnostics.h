#pragma once

#include "../JuceHeader.h"

// ARA model-graph and render diagnostics, compiled out unless the build is
// configured with -DPITCHNET_ARA_DIAGNOSTICS=ON.
//
// Routes through the project's existing AppLogger so every event lands in one
// chronological file:
//   %APPDATA%/PitchNet/Logs/debug_<session>.log
//
// AppLogger reopens the file on every call, so this is NOT realtime-safe.
// Model-graph events are low frequency by nature. Render-path reporting must
// only fire when an outcome CHANGES, never per block.

#if PITCHNET_ARA_DIAGNOSTICS

#include "../Utils/AppLogger.h"

#define ARA_DIAG(msg) AppLogger::log(juce::String("[ARA] ") + (msg))
#define ARA_DIAG_PTR(p)                                                        \
  juce::String::toHexString((juce::int64)(juce::pointer_sized_int)(p))

// TEMPORARY: cheap content fingerprint of an audio buffer.
//
// The render diagnostic tracks a blob's dimensions, which never change on an
// edit - the project spans the whole modification, so every republish is the
// same length. That makes a content change invisible. This gives publish and
// render sites a comparable value so a blob can be followed from where it is
// written to where it is served.
//
// Must be identical at every call site or the comparison means nothing, hence
// one definition here. Samples a fixed number of taps rather than the whole
// buffer: the render site runs on the audio thread and must stay O(1) in the
// blob length.
inline float araDiagFingerprint(const juce::AudioBuffer<float> &buffer) {
  const int n = buffer.getNumSamples();
  if (n <= 0 || buffer.getNumChannels() <= 0)
    return 0.0f;
  const float *p = buffer.getReadPointer(0);
  constexpr int kTaps = 16;
  float sum = 0.0f;
  for (int i = 0; i < kTaps; ++i) {
    const int idx =
        n == 1 ? 0
               : static_cast<int>(static_cast<juce::int64>(i) * (n - 1) /
                                  (kTaps - 1));
    sum += p[idx] * static_cast<float>(i + 1);
  }
  return sum;
}

#else

#define ARA_DIAG(msg) ((void)0)
#define ARA_DIAG_PTR(p) juce::String()

#endif
