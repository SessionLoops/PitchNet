#pragma once

#include "../JuceHeader.h"

#include <mutex>

/**
 * Initialises the ONNX Runtime C++ API for this plug-in instance.
 *
 * On macOS a host may already have a different ONNX Runtime loaded. This
 * resolves the API entry point from PitchNet's bundled framework explicitly,
 * rather than allowing the host's exported OrtGetApiBase symbol to win.
 */
namespace OnnxRuntime
{
bool initialise(juce::String *errorMessage = nullptr);

// ONNX Runtime DirectML 1.18/1.19 can crash inside the GPU driver when
// sessions are created concurrently. Every DirectML session creation (pitch
// detectors and vocoder) must hold this lock.
inline std::mutex &getDirectMLSessionMutex()
{
  static std::mutex mutex;
  return mutex;
}
}
