#pragma once

#include "../Audio/PitchDetectorType.h"
#include "../JuceHeader.h"
#include "../Utils/Constants.h"
#include "Main/SettingsManager.h"
#include "StyledComponents.h"
#include <functional>

enum class Language; // Forward declaration

/**
 * Settings dialog for application configuration.
 * Includes device selection for ONNX inference.
 */
class SettingsComponent : public juce::Component,
                          public juce::ComboBox::Listener,
                          public juce::ChangeListener,
                          public juce::Timer
{
public:
  SettingsComponent(SettingsManager *settingsManager,
                    juce::AudioDeviceManager *audioDeviceManager = nullptr);
  ~SettingsComponent() override;

  void paint(juce::Graphics &g) override;
  void resized() override;

  // ComboBox::Listener
  void comboBoxChanged(juce::ComboBox *comboBox) override;

  // ChangeListener
  void changeListenerCallback(juce::ChangeBroadcaster *source) override;

  // Timer
  void timerCallback() override;

  // Get current settings
  juce::String getSelectedDevice() const { return currentDevice; }
  int getGPUDeviceId() const { return gpuDeviceId; }
  PitchDetectorType getPitchDetectorType() const { return pitchDetectorType; }

  // Plugin mode (disables audio device settings)
  bool isPluginMode() const { return pluginMode; }

  // Callbacks
  std::function<void()> onSettingsChanged;
  std::function<void()> onLanguageChanged;
  std::function<void(PitchDetectorType)> onPitchDetectorChanged;
  std::function<void(bool)> onShowSegmentsDebugChanged;
  std::function<void(bool)> onShowGameValuesDebugChanged;
  std::function<void(bool)> onShowUvInterpolationDebugChanged;
  std::function<void(bool)> onShowActualF0DebugChanged;
  std::function<bool()> canChangeDevice;

  // Load/save settings
  void loadSettings();
  void saveSettings();

  // Get available execution providers
  static juce::StringArray getAvailableDevices();

private:
  class SettingsLookAndFeel : public DarkLookAndFeel
  {
  public:
    juce::Font getTextButtonFont(juce::TextButton &, int) override
    {
      return AppFont::getFont(15.0f);
    }

    juce::Font getLabelFont(juce::Label &) override
    {
      return AppFont::getFont(15.0f);
    }

    juce::Font getComboBoxFont(juce::ComboBox &) override
    {
      return AppFont::getFont(15.0f);
    }

    juce::Font getPopupMenuFont() override { return AppFont::getFont(15.0f); }

    void drawButtonBackground(juce::Graphics &g, juce::Button &button,
                              const juce::Colour &backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
      auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
      auto fill = backgroundColour;

      if (shouldDrawButtonAsDown)
        fill = fill.brighter(0.08f);
      else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter(0.04f);

      g.setColour(fill);
      g.fillRoundedRectangle(bounds, 8.0f);

      const bool isActiveTab =
          static_cast<bool>(button.getProperties().getWithDefault("isActiveTab",
                                                                  false));
      g.setColour((isActiveTab ? APP_COLOR_PRIMARY : APP_COLOR_BORDER)
                      .withAlpha(isActiveTab ? 0.62f : 0.75f));
      g.drawRoundedRectangle(bounds, 8.0f, 1.0f);
    }
  };

  enum class SettingsTab
  {
    General,
    Audio
  };

  void updateDeviceList();
  void updateGPUDeviceList(const juce::String &deviceType);
  void updateAudioDeviceTypes();
  void updateAudioOutputDevices(bool force = false);
  void updateSampleRates();
  void updateBufferSizes();
  void applyAudioSettings();
  void syncToSystemOutputIfNeeded();
  void setActiveTab(SettingsTab tab);
  void updateTabButtonStyles();
  void updateTabVisibility();
  bool shouldShowGpuDeviceList() const;

  bool pluginMode = false;
  juce::AudioDeviceManager *deviceManager = nullptr;
  SettingsManager *settingsManager = nullptr;
  SettingsLookAndFeel settingsLookAndFeel;

  juce::Label titleLabel;
  juce::Label generalSectionLabel;

  juce::Label languageLabel;
  StyledComboBox languageComboBox;

  juce::Label deviceLabel;
  StyledComboBox deviceComboBox;
  juce::Label gpuDeviceLabel;
  StyledComboBox gpuDeviceComboBox;

  juce::Label pitchDetectorLabel;
  StyledComboBox pitchDetectorComboBox;
  juce::Label gameChunksDebugLabel;
  juce::ToggleButton segmentsDebugToggle;
  juce::Label gameValuesDebugLabel;
  juce::ToggleButton gameValuesDebugToggle;
  juce::Label uvInterpolationDebugLabel;
  juce::ToggleButton uvInterpolationDebugToggle;
  juce::Label actualF0DebugLabel;
  juce::ToggleButton actualF0DebugToggle;

  juce::Label infoLabel;

  // Audio device settings (standalone mode only)
  juce::Label audioSectionLabel;
  juce::Label audioDeviceTypeLabel;
  StyledComboBox audioDeviceTypeComboBox;
  juce::Array<juce::AudioIODeviceType *> audioDeviceTypeOrder;
  juce::Label audioOutputLabel;
  StyledComboBox audioOutputComboBox;
  juce::Label sampleRateLabel;
  StyledComboBox sampleRateComboBox;
  juce::Label bufferSizeLabel;
  StyledComboBox bufferSizeComboBox;
  juce::Label outputChannelsLabel;
  StyledComboBox outputChannelsComboBox;

  juce::String preferredAudioOutputDevice;

  juce::String currentDevice = "CPU";
  bool followSystemAudioOutput = true;
  bool isRefreshingAudioControls = false;
  bool hasLoadedSettings = false;
  int gpuDeviceId = 0;
  juce::String lastConfirmedDevice = "CPU";
  int lastConfirmedGpuDeviceId = 0;
  PitchDetectorType pitchDetectorType = PitchDetectorType::FCPE;
  bool showSegmentsDebug = false;
  bool showGameValuesDebug = false;
  bool showUvInterpolationDebug = false;
  bool showActualF0Debug = false;
  SettingsTab activeTab = SettingsTab::General;
  juce::TextButton generalTabButton;
  juce::TextButton audioTabButton;
  juce::Rectangle<int> cardBounds;
  juce::Rectangle<int> sidebarBounds;
  juce::Rectangle<int> tabListBounds;
  juce::Array<int> separatorYs;
  float cornerRadius = 10.0f;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsComponent)
};
