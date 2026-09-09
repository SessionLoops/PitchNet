#pragma once

#include "../JuceHeader.h"

/**
 * Compute devices an ONNX Runtime execution provider exposes, shared by every
 * place that lets the user pick one (the Settings dialog and the Rendering
 * card).
 *
 * Enumeration is not free - DXGI needs a factory, CUDA needs its runtime DLL -
 * and adapters do not come and go while the app runs, so results are cached
 * per provider on first use.
 */
namespace GpuDeviceList
{
/**
 * Human-readable device names for a provider, in the order ONNX Runtime
 * numbers them: index i in this array is device id i.
 *
 * Empty for "CPU" and for providers that pick their own device (CoreML,
 * TensorRT) - there is nothing to choose between, so callers that still need
 * a row to show supply their own placeholder.
 */
juce::StringArray getDeviceNames(const juce::String &providerName);

/**
 * Device id to use when the user has never picked one.
 *
 * DirectML enumerates in DXGI order, which on the machines this matters for
 * puts the display adapter first and the compute-capable one last, so its
 * default is the last device rather than the first. Every other provider
 * defaults to 0.
 */
int getDefaultDeviceIndex(const juce::String &providerName);

/**
 * True when the provider exposes more than one device, i.e. when showing the
 * user a picker is worth the space.
 */
bool hasDeviceChoice(const juce::String &providerName);

/**
 * Names of the physical display adapters DXGI reports, with no provider
 * suffix, or empty off Windows and when DXGI reports none.
 *
 * This is the "is there hardware to run on" question, which a DirectML runtime
 * being installed does not answer: getDeviceNames("DirectML") falls back to a
 * fixed range when enumeration comes up empty, this does not.
 */
juce::StringArray getDisplayAdapterNames();
} // namespace GpuDeviceList
