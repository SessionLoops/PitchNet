#pragma once

#include "../JuceHeader.h"

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
}
