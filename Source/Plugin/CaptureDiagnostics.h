#pragma once

#include "../JuceHeader.h"
#include "../Utils/AppLogger.h"
#include "NonAraCaptureController.h"

// Non-ARA capture lifecycle diagnostics (issue #19: Ableton freeze/undo
// capture). Compiled out unless the build is configured with
// -DPITCHNET_CAPTURE_DIAGNOSTICS=ON in a configuration that has
// PITCHNET_LOGGING_ENABLED (Debug / RelWithDebInfo, e.g. the pitchnet-dev
// preset). Writes through AppLogger:
//   macOS:   ~/Library/Logs/PitchNet/debug_<session>.log
//   Windows: %APPDATA%/PitchNet/Logs/debug_<session>.log
//
// AppLogger opens the file per call, so this is NOT realtime-safe. Audio-thread
// call sites only log when an observed state CHANGES, never per block.

#if PITCHNET_CAPTURE_DIAGNOSTICS && PITCHNET_LOGGING_ENABLED

#define CAPTURE_DIAG(msg) AppLogger::log(juce::String("[Capture] ") + (msg))

inline const char *captureDiagStateName(NonAraCaptureController::State s) {
  switch (s) {
  case NonAraCaptureController::State::Idle: return "Idle";
  case NonAraCaptureController::State::WaitingForAudio: return "WaitingForAudio";
  case NonAraCaptureController::State::Capturing: return "Capturing";
  case NonAraCaptureController::State::Complete: return "Complete";
  }
  return "?";
}

#define CAPTURE_DIAG_ID(p)                                                     \
  juce::String::toHexString((juce::int64)(juce::pointer_sized_int)(p))

#else

#define CAPTURE_DIAG(msg) ((void)0)

#endif
