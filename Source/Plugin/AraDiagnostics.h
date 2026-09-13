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

#else

#define ARA_DIAG(msg) ((void)0)
#define ARA_DIAG_PTR(p) juce::String()

#endif
