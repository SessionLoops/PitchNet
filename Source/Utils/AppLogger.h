#pragma once

#include "../JuceHeader.h"
#include "PlatformPaths.h"

/**
 * Simple file logger for debugging.
 * Logs to %APPDATA%/PitchNet/debug.log
 */
class AppLogger {
public:
  static void init() {
    (void)getSessionId();
    (void)getLogFile();
  }

  static void log(const juce::String &message) {
    auto logFile = getLogFile();
    auto timestamp =
        juce::Time::getCurrentTime().toString(true, true, true, true);
    logFile.appendText("[" + timestamp + "] " + message + "\n");
  }

  static void clear() {
    auto logFile = getLogFile();
    logFile.deleteFile();
  }

  static juce::File getLogFile() {
    return PlatformPaths::getLogFile("debug_" + getSessionId() + ".log");
  }

  static juce::String getSessionId() {
    static const juce::String sessionId =
        juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    return sessionId;
  }
};

#define LOG(msg) AppLogger::log(msg)
