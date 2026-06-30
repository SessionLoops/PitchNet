#pragma once

#include "../../Audio/PitchDetectorType.h"
#include "../../Audio/Vocoder.h"
#include "../../JuceHeader.h"
#include "../../Utils/PlatformPaths.h"
#include <functional>

/**
 * Manages application settings and configuration persistence.
 */
class SettingsManager
{
public:
  SettingsManager();
  ~SettingsManager() = default;

  void setVocoder(Vocoder *v) { vocoder = v; }

  void loadSettings();
  void applySettings();
  juce::String getDevice() const { return device; }
  void setDevice(const juce::String &d) { device = d; }
  int getThreads() const { return threads; }
  void setThreads(int t) { threads = t; }
  PitchDetectorType getPitchDetectorType() const { return pitchDetectorType; }
  void setPitchDetectorType(PitchDetectorType t) { pitchDetectorType = t; }
  int getGPUDeviceId() const { return gpuDeviceId; }
  void setGPUDeviceId(int id) { gpuDeviceId = id; }
  juce::String getLanguage() const { return language; }
  void setLanguage(const juce::String &lang) { language = lang; }

  // Config (config.json - window state, last file)
  void loadConfig();
  void saveConfig();
  void setLastFilePath(const juce::File &file) { lastFilePath = file; }
  juce::File getLastFilePath() const { return lastFilePath; }
  void setRecentFiles(const juce::StringArray &files) { recentFiles = files; }
  const juce::StringArray &getRecentFiles() const { return recentFiles; }
  void setWindowSize(int w, int h)
  {
    windowWidth = w;
    windowHeight = h;
  }
  int getWindowWidth() const { return windowWidth; }
  int getWindowHeight() const { return windowHeight; }
  bool getFollowSystemAudioOutput() const { return followSystemAudioOutput; }
  void setFollowSystemAudioOutput(bool follow)
  {
    followSystemAudioOutput = follow;
  }
  juce::String getPreferredAudioOutputDevice() const
  {
    return preferredAudioOutputDevice;
  }
  void setPreferredAudioOutputDevice(const juce::String &name)
  {
    preferredAudioOutputDevice = name;
  }

  // View settings
  void setShowDeltaPitch(bool show) { showDeltaPitch = show; }
  void setShowBasePitch(bool show) { showBasePitch = show; }
  void setShowSegmentsDebug(bool show) { showSegmentsDebug = show; }
  void setShowGameValuesDebug(bool show) { showGameValuesDebug = show; }
  void setShowUvInterpolationDebug(bool show) { showUvInterpolationDebug = show; }
  void setShowActualF0Debug(bool show) { showActualF0Debug = show; }
  bool getShowDeltaPitch() const { return showDeltaPitch; }
  bool getShowBasePitch() const { return showBasePitch; }
  bool getShowSegmentsDebug() const { return showSegmentsDebug; }
  bool getShowGameValuesDebug() const { return showGameValuesDebug; }
  bool getShowUvInterpolationDebug() const { return showUvInterpolationDebug; }
  bool getShowActualF0Debug() const { return showActualF0Debug; }

  // Callbacks
  std::function<void()> onSettingsChanged;

private:
  static juce::File getSettingsFile();
  static juce::File getConfigFile();

  Vocoder *vocoder = nullptr;

  // Settings
  juce::String device = "CPU";
  int threads = 0;
  PitchDetectorType pitchDetectorType = PitchDetectorType::FCPE;
  int gpuDeviceId = 0;
  juce::String language = "en";

  // Config
  juce::File lastFilePath;
  juce::StringArray recentFiles;
  int windowWidth = 1000;
  int windowHeight = 628;
  bool showDeltaPitch = true;
  bool showBasePitch = false;
  bool showSegmentsDebug = false;
  bool showGameValuesDebug = false;
  bool showUvInterpolationDebug = false;
  bool showActualF0Debug = false;
  bool followSystemAudioOutput = true;
  juce::String preferredAudioOutputDevice;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsManager)
};
