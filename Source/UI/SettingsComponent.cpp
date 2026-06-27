#include "SettingsComponent.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Localization.h"

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

#ifdef _WIN32
#include <dxgi1_2.h>
#include <windows.h>
#endif

namespace
{
  constexpr int kFollowSystemOutputId = 1;

  juce::String resolveDefaultOutputDeviceName(juce::AudioDeviceManager *deviceManager)
  {
    if (deviceManager == nullptr)
      return {};

    auto *currentType = deviceManager->getCurrentDeviceTypeObject();
    if (currentType == nullptr)
      return {};

    auto devices = currentType->getDeviceNames(false);
    const int defaultIndex = currentType->getDefaultDeviceIndex(false);
    if (juce::isPositiveAndBelow(defaultIndex, devices.size()))
      return devices[defaultIndex];

    if (devices.size() > 0)
      return devices[0];

    return {};
  }

#ifdef _WIN32
  juce::StringArray getDxgiAdapterNames()
  {
    juce::StringArray names;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void **>(&factory))) ||
        factory == nullptr)
    {
      return names;
    }

    for (UINT i = 0;; ++i)
    {
      IDXGIAdapter1 *adapter = nullptr;
      const auto hr = factory->EnumAdapters1(i, &adapter);
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(hr) || adapter == nullptr)
        continue;

      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)))
      {
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
          names.add(juce::String(desc.Description));
        }
      }
      adapter->Release();
    }

    factory->Release();
    return names;
  }
#endif
} // namespace

//==============================================================================
// SettingsComponent
//==============================================================================

SettingsComponent::SettingsComponent(
    SettingsManager *settingsMgr, juce::AudioDeviceManager *audioDeviceManager)
    : deviceManager(audioDeviceManager),
      pluginMode(audioDeviceManager == nullptr), settingsManager(settingsMgr)
{
  // Allow transparent corners for rounded window styling
  setOpaque(false);

  auto configureRowLabel = [](juce::Label &label)
  {
    label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    label.setFont(AppFont::getFont(15.0f));
    label.setJustificationType(juce::Justification::centredLeft);
  };

  // Title
  titleLabel.setText(TR("settings.title"), juce::dontSendNotification);
  titleLabel.setFont(AppFont::getBoldFont(20.0f));
  titleLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
  addAndMakeVisible(titleLabel);

  auto configureTabButton = [this](juce::TextButton &button)
  {
    button.setClickingTogglesState(false);
    button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    button.setLookAndFeel(&settingsLookAndFeel);
  };

  // Tabs
  generalTabButton.setButtonText(TR("settings.general"));
  configureTabButton(generalTabButton);
  generalTabButton.onClick = [this]()
  { setActiveTab(SettingsTab::General); };
  addAndMakeVisible(generalTabButton);

  audioTabButton.setButtonText(TR("settings.audio"));
  configureTabButton(audioTabButton);
  audioTabButton.onClick = [this]()
  { setActiveTab(SettingsTab::Audio); };
  addAndMakeVisible(audioTabButton);

  // Language selection
  languageLabel.setText(TR("settings.language"), juce::dontSendNotification);
  configureRowLabel(languageLabel);
  addAndMakeVisible(languageLabel);

  // Add "Auto" option first
  languageComboBox.addItem(TR("lang.auto"), 1);
  // Add available languages dynamically
  const auto &langs = Localization::getInstance().getAvailableLanguages();
  for (int i = 0; i < static_cast<int>(langs.size()); ++i)
    languageComboBox.addItem(langs[i].nativeName, i + 2); // IDs start at 2
  languageComboBox.addListener(this);
  languageComboBox.applyStyle();
  addAndMakeVisible(languageComboBox);

  // Device selection
  deviceLabel.setText(TR("settings.device"), juce::dontSendNotification);
  configureRowLabel(deviceLabel);
  addAndMakeVisible(deviceLabel);

  deviceComboBox.addListener(this);
  deviceComboBox.applyStyle();
  addAndMakeVisible(deviceComboBox);

  // GPU device ID selection
  gpuDeviceLabel.setText(TR("settings.gpu_device"), juce::dontSendNotification);
  configureRowLabel(gpuDeviceLabel);
  addAndMakeVisible(gpuDeviceLabel);

  // GPU device list will be populated dynamically based on available devices
  gpuDeviceComboBox.addListener(this);
  gpuDeviceComboBox.applyStyle();
  addAndMakeVisible(gpuDeviceComboBox);
  gpuDeviceLabel.setVisible(false);
  gpuDeviceComboBox.setVisible(false);

  // Pitch detector selection
  pitchDetectorLabel.setText(TR("settings.pitch_detector"),
                             juce::dontSendNotification);
  configureRowLabel(pitchDetectorLabel);
  addAndMakeVisible(pitchDetectorLabel);

  pitchDetectorComboBox.addItem("RMVPE", 1);
  pitchDetectorComboBox.addItem("FCPE", 2);
  pitchDetectorComboBox.setSelectedId(
      2, juce::dontSendNotification); // Default to FCPE
  pitchDetectorComboBox.addListener(this);
  pitchDetectorComboBox.applyStyle();
  addAndMakeVisible(pitchDetectorComboBox);

  gameChunksDebugLabel.setText("Show GAME chunks (debug)",
                               juce::dontSendNotification);
  configureRowLabel(gameChunksDebugLabel);
  addAndMakeVisible(gameChunksDebugLabel);

  segmentsDebugToggle.setButtonText("");
  segmentsDebugToggle.setClickingTogglesState(true);
  segmentsDebugToggle.onClick = [this]()
  {
    showSegmentsDebug = segmentsDebugToggle.getToggleState();
    if (settingsManager)
    {
      settingsManager->setShowSegmentsDebug(showSegmentsDebug);
      settingsManager->saveConfig();
    }
    if (onShowSegmentsDebugChanged)
      onShowSegmentsDebugChanged(showSegmentsDebug);
  };
  addAndMakeVisible(segmentsDebugToggle);

  gameValuesDebugLabel.setText("Show GAME values (debug)",
                               juce::dontSendNotification);
  configureRowLabel(gameValuesDebugLabel);
  addAndMakeVisible(gameValuesDebugLabel);

  gameValuesDebugToggle.setButtonText("");
  gameValuesDebugToggle.setClickingTogglesState(true);
  gameValuesDebugToggle.onClick = [this]()
  {
    showGameValuesDebug = gameValuesDebugToggle.getToggleState();
    if (settingsManager)
    {
      settingsManager->setShowGameValuesDebug(showGameValuesDebug);
      settingsManager->saveConfig();
    }
    if (onShowGameValuesDebugChanged)
      onShowGameValuesDebugChanged(showGameValuesDebug);
  };
  addAndMakeVisible(gameValuesDebugToggle);

  uvInterpolationDebugLabel.setText("Show UV interpolation (debug)",
                                    juce::dontSendNotification);
  configureRowLabel(uvInterpolationDebugLabel);
  addAndMakeVisible(uvInterpolationDebugLabel);

  uvInterpolationDebugToggle.setButtonText("");
  uvInterpolationDebugToggle.setClickingTogglesState(true);
  uvInterpolationDebugToggle.onClick = [this]()
  {
    showUvInterpolationDebug = uvInterpolationDebugToggle.getToggleState();
    if (settingsManager)
    {
      settingsManager->setShowUvInterpolationDebug(showUvInterpolationDebug);
      settingsManager->saveConfig();
    }
    if (onShowUvInterpolationDebugChanged)
      onShowUvInterpolationDebugChanged(showUvInterpolationDebug);
  };
  addAndMakeVisible(uvInterpolationDebugToggle);

  actualF0DebugLabel.setText("Show actual F0 (debug)",
                             juce::dontSendNotification);
  configureRowLabel(actualF0DebugLabel);
  addAndMakeVisible(actualF0DebugLabel);

  actualF0DebugToggle.setButtonText("");
  actualF0DebugToggle.setClickingTogglesState(true);
  actualF0DebugToggle.onClick = [this]()
  {
    showActualF0Debug = actualF0DebugToggle.getToggleState();
    if (settingsManager)
    {
      settingsManager->setShowActualF0Debug(showActualF0Debug);
      settingsManager->saveConfig();
    }
    if (onShowActualF0DebugChanged)
      onShowActualF0DebugChanged(showActualF0Debug);
  };
  addAndMakeVisible(actualF0DebugToggle);

  // Info label
  infoLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
  infoLabel.setFont(AppFont::getFont(15.0f));
  infoLabel.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(infoLabel);

  // Audio device settings (standalone mode only)
  if (!pluginMode && deviceManager != nullptr)
  {
    deviceManager->addChangeListener(this);
    startTimerHz(1);

    // Audio device type (driver)
    audioDeviceTypeLabel.setText(TR("settings.audio_driver"),
                                 juce::dontSendNotification);
    configureRowLabel(audioDeviceTypeLabel);
    addAndMakeVisible(audioDeviceTypeLabel);
    audioDeviceTypeComboBox.addListener(this);
    audioDeviceTypeComboBox.applyStyle();
    addAndMakeVisible(audioDeviceTypeComboBox);

    // Output device
    audioOutputLabel.setText(TR("settings.audio_output"),
                             juce::dontSendNotification);
    configureRowLabel(audioOutputLabel);
    addAndMakeVisible(audioOutputLabel);
    audioOutputComboBox.addListener(this);
    audioOutputComboBox.applyStyle();
    addAndMakeVisible(audioOutputComboBox);

    // Sample rate
    sampleRateLabel.setText(TR("settings.sample_rate"),
                            juce::dontSendNotification);
    configureRowLabel(sampleRateLabel);
    addAndMakeVisible(sampleRateLabel);
    sampleRateComboBox.addListener(this);
    sampleRateComboBox.applyStyle();
    addAndMakeVisible(sampleRateComboBox);

    // Buffer size
    bufferSizeLabel.setText(TR("settings.buffer_size"),
                            juce::dontSendNotification);
    configureRowLabel(bufferSizeLabel);
    addAndMakeVisible(bufferSizeLabel);
    bufferSizeComboBox.addListener(this);
    bufferSizeComboBox.applyStyle();
    addAndMakeVisible(bufferSizeComboBox);

    // Output channels
    outputChannelsLabel.setText(TR("settings.output_channels"),
                                juce::dontSendNotification);
    configureRowLabel(outputChannelsLabel);
    addAndMakeVisible(outputChannelsLabel);
    outputChannelsComboBox.addItem(TR("settings.mono"), 1);
    outputChannelsComboBox.addItem(TR("settings.stereo"), 2);
    outputChannelsComboBox.setSelectedId(2, juce::dontSendNotification);
    outputChannelsComboBox.addListener(this);
    outputChannelsComboBox.applyStyle();
    addAndMakeVisible(outputChannelsComboBox);

    updateAudioDeviceTypes();
  }

  // Load saved settings
  loadSettings();
  updateDeviceList();

  updateTabButtonStyles();
  updateTabVisibility();

  // Set size based on mode
  if (pluginMode)
    setSize(720, 420);
  else
    setSize(820, 620);
}

SettingsComponent::~SettingsComponent()
{
  stopTimer();
  if (!pluginMode && deviceManager != nullptr)
    deviceManager->removeChangeListener(this);
  generalTabButton.setLookAndFeel(nullptr);
  audioTabButton.setLookAndFeel(nullptr);
  languageComboBox.setLookAndFeel(nullptr);
  deviceComboBox.setLookAndFeel(nullptr);
  gpuDeviceComboBox.setLookAndFeel(nullptr);
  pitchDetectorComboBox.setLookAndFeel(nullptr);
  audioDeviceTypeComboBox.setLookAndFeel(nullptr);
  audioOutputComboBox.setLookAndFeel(nullptr);
  sampleRateComboBox.setLookAndFeel(nullptr);
  bufferSizeComboBox.setLookAndFeel(nullptr);
  outputChannelsComboBox.setLookAndFeel(nullptr);
  segmentsDebugToggle.setLookAndFeel(nullptr);
  gameValuesDebugToggle.setLookAndFeel(nullptr);
  uvInterpolationDebugToggle.setLookAndFeel(nullptr);
  actualF0DebugToggle.setLookAndFeel(nullptr);
}

void SettingsComponent::changeListenerCallback(
    juce::ChangeBroadcaster *source)
{
  if (source == deviceManager)
  {
    syncToSystemOutputIfNeeded();
    updateAudioOutputDevices(true);
  }
}

void SettingsComponent::timerCallback()
{
  if (!followSystemAudioOutput || pluginMode || deviceManager == nullptr)
    return;
  syncToSystemOutputIfNeeded();
}

void SettingsComponent::paint(juce::Graphics &g)
{
  juce::Path rounded;
  rounded.addRoundedRectangle(getLocalBounds().toFloat(), cornerRadius);
  g.reduceClipRegion(rounded);

  // Flat background fill
  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), cornerRadius);

  if (!sidebarBounds.isEmpty())
  {
    g.setColour(APP_COLOR_BORDER_SUBTLE);
    g.drawLine((float)sidebarBounds.getRight(), (float)sidebarBounds.getY(),
               (float)sidebarBounds.getRight(),
               (float)sidebarBounds.getBottom(), 1.0f);
  }

  if (!tabListBounds.isEmpty())
  {
    auto tabCard = tabListBounds.toFloat();
    g.setColour(APP_COLOR_SURFACE_RAISED);
    g.fillRoundedRectangle(tabCard, 10.0f);
    g.setColour(APP_COLOR_BORDER.withAlpha(0.55f));
    g.drawRoundedRectangle(tabCard.reduced(0.5f), 10.0f, 0.75f);
  }

  if (!cardBounds.isEmpty())
  {
    auto card = cardBounds.toFloat();
    g.setColour(APP_COLOR_SURFACE_RAISED);
    g.fillRoundedRectangle(card, 8.0f);

    g.setColour(APP_COLOR_BORDER.withAlpha(0.55f));
    g.drawRoundedRectangle(card.reduced(0.5f), 8.0f, 0.75f);

  }
}

void SettingsComponent::resized()
{
  auto bounds = getLocalBounds().reduced(16);
  tabListBounds = {};

  const int sidebarWidth = 140;
  sidebarBounds = bounds.removeFromLeft(sidebarWidth);

  auto tabAreaBounds = sidebarBounds.reduced(8, 10);
  const int tabHeight = 32;
  const int tabGap = 6;
  const int tabCount = audioTabButton.isVisible() ? 2 : 1;
  const int tabContainerHeight = 16 + tabCount * tabHeight + (tabCount - 1) * tabGap;
  tabListBounds =
      tabAreaBounds.withHeight(juce::jmin(tabAreaBounds.getHeight(), tabContainerHeight));

  auto tabArea = tabListBounds.reduced(8, 8);
  generalTabButton.setBounds(tabArea.removeFromTop(tabHeight));
  if (audioTabButton.isVisible())
  {
    tabArea.removeFromTop(tabGap);
    audioTabButton.setBounds(tabArea.removeFromTop(tabHeight));
  }

  bounds.removeFromLeft(10);

  auto titleArea = bounds.removeFromTop(34);
  titleLabel.setBounds(titleArea);
  bounds.removeFromTop(6);

  cardBounds = bounds;
  auto content = cardBounds.reduced(16, 12);

  const int rowHeight = 32;
  const int rowGap = 8;
  const int controlWidth =
      juce::jlimit(190, 300, content.getWidth() / 3);
  const int labelWidth =
      juce::jlimit(180, 320, content.getWidth() - controlWidth - 24);

  auto layoutRow = [&](juce::Label &label, juce::Component &control)
  {
    auto row = content.removeFromTop(rowHeight);
    auto labelArea = row.removeFromLeft(labelWidth);
    auto controlArea = row.removeFromRight(controlWidth);
    label.setBounds(labelArea);
    control.setBounds(controlArea.reduced(0, 2));
    content.removeFromTop(rowGap);
  };

  if (activeTab == SettingsTab::General)
  {
    layoutRow(languageLabel, languageComboBox);
    layoutRow(deviceLabel, deviceComboBox);

    if (gpuDeviceLabel.isVisible())
    {
      layoutRow(gpuDeviceLabel, gpuDeviceComboBox);
    }

    layoutRow(pitchDetectorLabel, pitchDetectorComboBox);
    layoutRow(gameChunksDebugLabel, segmentsDebugToggle);
    layoutRow(gameValuesDebugLabel, gameValuesDebugToggle);
    layoutRow(uvInterpolationDebugLabel, uvInterpolationDebugToggle);
    layoutRow(actualF0DebugLabel, actualF0DebugToggle);

    infoLabel.setBounds(content.removeFromTop(56));
    content.removeFromTop(12);
  }

  if (!pluginMode && deviceManager != nullptr &&
      activeTab == SettingsTab::Audio)
  {
    layoutRow(audioDeviceTypeLabel, audioDeviceTypeComboBox);
    layoutRow(audioOutputLabel, audioOutputComboBox);
    layoutRow(sampleRateLabel, sampleRateComboBox);
    layoutRow(bufferSizeLabel, bufferSizeComboBox);
    layoutRow(outputChannelsLabel, outputChannelsComboBox);
  }
}

void SettingsComponent::comboBoxChanged(juce::ComboBox *comboBox)
{
  if (isRefreshingAudioControls &&
      (comboBox == &audioDeviceTypeComboBox || comboBox == &audioOutputComboBox ||
       comboBox == &sampleRateComboBox || comboBox == &bufferSizeComboBox ||
       comboBox == &outputChannelsComboBox))
    return;

  if (comboBox == &languageComboBox)
  {
    int selectedId = languageComboBox.getSelectedId();
    if (selectedId == 1)
    {
      // Auto - detect system language
      Localization::detectSystemLanguage();
    }
    else if (selectedId >= 2)
    {
      // Get language code from index
      const auto &langs = Localization::getInstance().getAvailableLanguages();
      int langIndex = selectedId - 2;
      if (langIndex < static_cast<int>(langs.size()))
        Localization::getInstance().setLanguage(langs[langIndex].code);
    }
    saveSettings();

    if (onLanguageChanged)
      onLanguageChanged();
  }
  else if (comboBox == &deviceComboBox)
  {
    if (canChangeDevice && !canChangeDevice())
    {
      for (int i = 0; i < deviceComboBox.getNumItems(); ++i)
      {
        if (deviceComboBox.getItemText(i) == lastConfirmedDevice)
        {
          deviceComboBox.setSelectedItemIndex(i,
                                              juce::dontSendNotification);
          break;
        }
      }
      currentDevice = lastConfirmedDevice;
      updateGPUDeviceList(currentDevice);
      gpuDeviceComboBox.setSelectedId(lastConfirmedGpuDeviceId + 1,
                                      juce::dontSendNotification);
      infoLabel.setText(
          "Inference in progress. Stop it to switch device.",
          juce::dontSendNotification);
      updateTabVisibility();
      resized();
      return;
    }

    currentDevice = deviceComboBox.getText();

    // Show/hide GPU device selector based on device type
    bool showGpuDeviceList = shouldShowGpuDeviceList();
    if (showGpuDeviceList)
    {
      // Update GPU device list for the selected device type
      updateGPUDeviceList(currentDevice);
    }
    updateTabVisibility();
    resized();

    saveSettings();

    // Update info label
    if (currentDevice == "CPU")
    {
      infoLabel.setText(TR("settings.cpu_desc"), juce::dontSendNotification);
    }
    else if (currentDevice == "CUDA")
    {
      infoLabel.setText(TR("settings.cuda_desc"), juce::dontSendNotification);
    }
    else if (currentDevice == "DirectML")
    {
      infoLabel.setText(TR("settings.directml_desc"),
                        juce::dontSendNotification);
    }
    else if (currentDevice == "CoreML")
    {
      infoLabel.setText(TR("settings.coreml_desc"), juce::dontSendNotification);
    }

    if (onSettingsChanged)
      onSettingsChanged();

    lastConfirmedDevice = currentDevice;
    lastConfirmedGpuDeviceId = gpuDeviceId;
  }
  else if (comboBox == &gpuDeviceComboBox)
  {
    if (canChangeDevice && !canChangeDevice())
    {
      gpuDeviceComboBox.setSelectedId(lastConfirmedGpuDeviceId + 1,
                                      juce::dontSendNotification);
      infoLabel.setText(
          "Inference in progress. Stop it to switch device.",
          juce::dontSendNotification);
      return;
    }
    gpuDeviceId = gpuDeviceComboBox.getSelectedId() - 1;
    saveSettings();
    if (onSettingsChanged)
      onSettingsChanged();
    lastConfirmedGpuDeviceId = gpuDeviceId;
  }
  else if (comboBox == &pitchDetectorComboBox)
  {
    int selectedId = pitchDetectorComboBox.getSelectedId();
    if (selectedId == 1)
      pitchDetectorType = PitchDetectorType::RMVPE;
    else if (selectedId == 2)
      pitchDetectorType = PitchDetectorType::FCPE;

    saveSettings();

    if (onPitchDetectorChanged)
      onPitchDetectorChanged(pitchDetectorType);
  }
  else if (comboBox == &audioDeviceTypeComboBox)
  {
    int idx = audioDeviceTypeComboBox.getSelectedId() - 1;
    if (idx >= 0 && idx < audioDeviceTypeOrder.size())
    {
      const auto targetType = audioDeviceTypeOrder.getReference(idx)->getTypeName();
      const auto *currentType = deviceManager->getCurrentDeviceTypeObject();
      if (currentType == nullptr || currentType->getTypeName() != targetType)
        deviceManager->setCurrentAudioDeviceType(targetType, true);
      updateAudioOutputDevices(true);
    }
  }
  else if (comboBox == &audioOutputComboBox)
  {
    followSystemAudioOutput =
        (audioOutputComboBox.getSelectedId() == kFollowSystemOutputId);
    if (!followSystemAudioOutput && audioOutputComboBox.getSelectedId() > kFollowSystemOutputId)
      preferredAudioOutputDevice = audioOutputComboBox.getText();
    applyAudioSettings();
  }
  else if (comboBox == &sampleRateComboBox ||
           comboBox == &bufferSizeComboBox ||
           comboBox == &outputChannelsComboBox)
  {
    applyAudioSettings();
  }
}

bool SettingsComponent::shouldShowGpuDeviceList() const
{
  return currentDevice == "CUDA" || currentDevice == "DirectML";
}

void SettingsComponent::setActiveTab(SettingsTab tab)
{
  if (activeTab == tab)
    return;

  activeTab = tab;

  if (activeTab == SettingsTab::Audio && !pluginMode && deviceManager != nullptr)
    updateAudioOutputDevices(true);

  updateTabButtonStyles();
  updateTabVisibility();
  resized();
  repaint();
}

void SettingsComponent::updateTabButtonStyles()
{
  auto applyStyle = [&](juce::TextButton &button, bool isActive)
  {
    const auto activeBg = APP_COLOR_PRIMARY.withAlpha(0.22f);
    const auto inactiveBg = APP_COLOR_SURFACE_ALT.withAlpha(0.75f);
    const auto activeText = APP_COLOR_TEXT_PRIMARY;
    const auto inactiveText = APP_COLOR_TEXT_MUTED;

    if (isActive)
    {
      button.setColour(juce::TextButton::buttonColourId, activeBg);
      button.setColour(juce::TextButton::buttonOnColourId, activeBg);
      button.setColour(juce::TextButton::textColourOffId, activeText);
      button.setColour(juce::TextButton::textColourOnId, activeText);
    }
    else
    {
      button.setColour(juce::TextButton::buttonColourId, inactiveBg);
      button.setColour(juce::TextButton::buttonOnColourId, inactiveBg);
      button.setColour(juce::TextButton::textColourOffId, inactiveText);
      button.setColour(juce::TextButton::textColourOnId, inactiveText);
    }

    button.getProperties().set("isActiveTab", isActive);
  };

  applyStyle(generalTabButton, activeTab == SettingsTab::General);
  applyStyle(audioTabButton, activeTab == SettingsTab::Audio);
}

void SettingsComponent::updateTabVisibility()
{
  const bool showGeneral = (activeTab == SettingsTab::General);
  const bool showAudio =
      (!pluginMode && deviceManager != nullptr &&
       activeTab == SettingsTab::Audio);
  const bool showGpuDeviceList = shouldShowGpuDeviceList();

  languageLabel.setVisible(showGeneral);
  languageComboBox.setVisible(showGeneral);
  deviceLabel.setVisible(showGeneral);
  deviceComboBox.setVisible(showGeneral);
  gpuDeviceLabel.setVisible(showGeneral && showGpuDeviceList);
  gpuDeviceComboBox.setVisible(showGeneral && showGpuDeviceList);
  pitchDetectorLabel.setVisible(showGeneral);
  pitchDetectorComboBox.setVisible(showGeneral);
  gameChunksDebugLabel.setVisible(showGeneral);
  segmentsDebugToggle.setVisible(showGeneral);
  gameValuesDebugLabel.setVisible(showGeneral);
  gameValuesDebugToggle.setVisible(showGeneral);
  uvInterpolationDebugLabel.setVisible(showGeneral);
  uvInterpolationDebugToggle.setVisible(showGeneral);
  actualF0DebugLabel.setVisible(showGeneral);
  actualF0DebugToggle.setVisible(showGeneral);
  infoLabel.setVisible(showGeneral);

  audioDeviceTypeLabel.setVisible(showAudio);
  audioDeviceTypeComboBox.setVisible(showAudio);
  audioOutputLabel.setVisible(showAudio);
  audioOutputComboBox.setVisible(showAudio);
  sampleRateLabel.setVisible(showAudio);
  sampleRateComboBox.setVisible(showAudio);
  bufferSizeLabel.setVisible(showAudio);
  bufferSizeComboBox.setVisible(showAudio);
  outputChannelsLabel.setVisible(showAudio);
  outputChannelsComboBox.setVisible(showAudio);

  audioTabButton.setVisible(!pluginMode && deviceManager != nullptr);

  if (pluginMode || deviceManager == nullptr)
    setActiveTab(SettingsTab::General);
}

void SettingsComponent::updateDeviceList()
{
  deviceComboBox.clear();

  auto devices = getAvailableDevices();
  int selectedIndex = 0;

  // Auto-select based on compile-time flags (first run only)
  if (!hasLoadedSettings && currentDevice == "CPU")
  {
#ifdef USE_DIRECTML
    // If DirectML is compiled in, default to DirectML
    for (int i = 0; i < devices.size(); ++i)
    {
      if (devices[i] == "DirectML")
      {
        selectedIndex = i;
        currentDevice = devices[i];
        break;
      }
    }
#elif defined(USE_CUDA)
    // If CUDA is compiled in, default to CUDA
    for (int i = 0; i < devices.size(); ++i)
    {
      if (devices[i] == "CUDA")
      {
        selectedIndex = i;
        currentDevice = devices[i];
        break;
      }
    }
#else
    // No GPU compiled in, stay on CPU
#endif
  }

  for (int i = 0; i < devices.size(); ++i)
  {
    deviceComboBox.addItem(devices[i], i + 1);
    if (devices[i] == currentDevice)
      selectedIndex = i;
  }

  deviceComboBox.setSelectedItemIndex(selectedIndex,
                                      juce::dontSendNotification);

  // Update info for initially selected device
  comboBoxChanged(&deviceComboBox);
}

void SettingsComponent::updateGPUDeviceList(const juce::String &deviceType)
{
  gpuDeviceComboBox.clear();

  if (deviceType == "CPU")
  {
    // No GPU devices for CPU
    return;
  }

#ifdef HAVE_ONNXRUNTIME
  if (deviceType == "CUDA")
  {
#ifdef USE_CUDA
    int deviceCount = 0;
    bool devicesDetected = false;
    juce::StringArray cudaDeviceNames;

    // Try to load CUDA runtime library to get actual device count and names
#ifdef _WIN32
    const char *cudaDllNames[] = {
        "cudart64_12.dll", // CUDA 12.x
        "cudart64_11.dll", // CUDA 11.x
        "cudart64_10.dll", // CUDA 10.x
        "cudart64.dll"     // Generic
    };

    HMODULE cudaLib = nullptr;
    for (const char *dllName : cudaDllNames)
    {
      cudaLib = LoadLibraryA(dllName);
      if (cudaLib)
      {
        break;
      }
    }

    if (cudaLib)
    {
      typedef int (*cudaGetDeviceCountFunc)(int *);
      typedef int (*cudaGetDevicePropertiesFunc)(void *, int);

      auto cudaGetDeviceCount =
          (cudaGetDeviceCountFunc)GetProcAddress(cudaLib, "cudaGetDeviceCount");

      if (cudaGetDeviceCount)
      {
        int result = cudaGetDeviceCount(&deviceCount);
        if (result == 0 && deviceCount > 0)
        {

          // Try to get device properties for names
          auto cudaGetDeviceProperties =
              (cudaGetDevicePropertiesFunc)GetProcAddress(
                  cudaLib, "cudaGetDeviceProperties");

          for (int deviceId = 0; deviceId < deviceCount; ++deviceId)
          {
            juce::String deviceName = "GPU " + juce::String(deviceId);

            // Try to get device name from properties
            if (cudaGetDeviceProperties)
            {
              // Allocate full cudaDeviceProp structure (it's large, ~1KB)
              // We can't use the actual struct without CUDA headers, so
              // allocate enough space
              char propBuffer[2048]; // Large enough for cudaDeviceProp
              memset(propBuffer, 0, sizeof(propBuffer));

              if (cudaGetDeviceProperties(propBuffer, deviceId) == 0)
              {
                // Device name is at the start of the structure
                char *name = propBuffer;
                if (name[0] != '\0')
                {
                  deviceName = juce::String(name);
                }
              }
            }

            cudaDeviceNames.add(deviceName + " (CUDA)");
          }
          devicesDetected = true;
        }
        else
        {
        }
      }
      FreeLibrary(cudaLib);
    }
    else
    {
    }
#endif

    if (devicesDetected && cudaDeviceNames.size() > 0)
    {
      for (int i = 0; i < cudaDeviceNames.size(); ++i)
        gpuDeviceComboBox.addItem(cudaDeviceNames[i], i + 1);
    }
    else
    {
#ifdef _WIN32
      auto dxgiNames = getDxgiAdapterNames();
      if (dxgiNames.size() > 0)
      {
        for (int i = 0; i < dxgiNames.size(); ++i)
          gpuDeviceComboBox.addItem(dxgiNames[i] + " (DXGI)", i + 1);
      }
#endif
    }

    // If no devices detected, add default
    if (gpuDeviceComboBox.getNumItems() == 0)
    {
      gpuDeviceComboBox.addItem("GPU 0 (CUDA)", 1);
    }
#else
    // CUDA not compiled in, but provider is available
    // This shouldn't happen, but add default option
    gpuDeviceComboBox.addItem("GPU 0 (CUDA)", 1);
#endif
  }
  else if (deviceType == "DirectML")
  {
#ifdef USE_DIRECTML
    bool addedFromDxgi = false;
#ifdef _WIN32
    auto dxgiNames = getDxgiAdapterNames();
    if (dxgiNames.size() > 0)
    {
      for (int i = 0; i < dxgiNames.size(); ++i)
        gpuDeviceComboBox.addItem(dxgiNames[i] + " (DirectML)", i + 1);
      addedFromDxgi = true;
    }
#endif
    if (!addedFromDxgi)
    {
      // DirectML fallback: provide a small default list
      for (int deviceId = 0; deviceId < 4; ++deviceId)
      {
        gpuDeviceComboBox.addItem(
            "GPU " + juce::String(deviceId) + " (DirectML)", deviceId + 1);
      }
    }
#else
    // DirectML not compiled in
    gpuDeviceComboBox.addItem("GPU 0 (DirectML)", 1);
#endif
  }
  else
  {
    // Other GPU providers (CoreML, TensorRT) - use default device
    gpuDeviceComboBox.addItem(TR("settings.default_gpu"), 1);
  }

  // Set default selection
  if (gpuDeviceComboBox.getNumItems() > 0)
  {
    // Try to restore saved selection, or use first device
    int savedId = gpuDeviceId + 1;
    if (savedId > 0 && savedId <= gpuDeviceComboBox.getNumItems())
      gpuDeviceComboBox.setSelectedId(savedId, juce::dontSendNotification);
    else
      gpuDeviceComboBox.setSelectedId(1, juce::dontSendNotification);
  }
#endif
}

juce::StringArray SettingsComponent::getAvailableDevices()
{
  juce::StringArray devices;

  // CPU is always available
  devices.add("CPU");

#ifdef HAVE_ONNXRUNTIME
  try
  {
    // Get providers that are compiled into the ONNX Runtime library
    auto availableProviders = Ort::GetAvailableProviders();

    // Check which providers are available
    bool hasCuda = false, hasDml = false, hasCoreML = false,
         hasTensorRT = false;

    for (const auto &provider : availableProviders)
    {
      juce::String providerStr(provider);

      if (providerStr == "CUDAExecutionProvider")
        hasCuda = true;
      else if (providerStr == "DmlExecutionProvider")
        hasDml = true;
      else if (providerStr == "CoreMLExecutionProvider")
        hasCoreML = true;
      else if (providerStr == "TensorrtExecutionProvider")
        hasTensorRT = true;
    }

    // Add available GPU providers based on compile-time flags
    // DML and CUDA are mutually exclusive
#ifdef USE_DIRECTML
    if (hasDml)
    {
      devices.add("DirectML");
    }
#elif defined(USE_CUDA)
    if (hasCuda)
    {
      devices.add("CUDA");
    }
#else
    // No GPU compiled in, but show available providers for information
    if (hasCuda)
    {
      devices.add("CUDA");
    }
    if (hasDml)
    {
      devices.add("DirectML");
    }
#endif
    if (hasCoreML)
    {
      devices.add("CoreML");
    }
    if (hasTensorRT)
    {
      devices.add("TensorRT");
    }

    // If no GPU providers found, show info about how to enable them
    if (!hasCuda && !hasDml && !hasCoreML && !hasTensorRT)
    {
    }
  }
  catch (const std::exception &e)
  {
  }
#else
#endif

  return devices;
}

void SettingsComponent::loadSettings()
{
  const auto &langs = Localization::getInstance().getAvailableLanguages();

  if (settingsManager)
    settingsManager->loadConfig();

  if (settingsManager)
  {
    currentDevice = settingsManager->getDevice();
    gpuDeviceId = settingsManager->getGPUDeviceId();
    pitchDetectorType = settingsManager->getPitchDetectorType();
    followSystemAudioOutput = settingsManager->getFollowSystemAudioOutput();
    preferredAudioOutputDevice = settingsManager->getPreferredAudioOutputDevice();
    showSegmentsDebug = settingsManager->getShowSegmentsDebug();
    showGameValuesDebug = settingsManager->getShowGameValuesDebug();
    showUvInterpolationDebug = settingsManager->getShowUvInterpolationDebug();
    showActualF0Debug = settingsManager->getShowActualF0Debug();

    auto langCode = settingsManager->getLanguage();
    if (langCode == "auto")
    {
      Localization::detectSystemLanguage();
      languageComboBox.setSelectedId(1, juce::dontSendNotification);
    }
    else
    {
      Localization::getInstance().setLanguage(langCode);
      for (int i = 0; i < static_cast<int>(langs.size()); ++i)
      {
        if (langs[i].code == langCode)
        {
          languageComboBox.setSelectedId(i + 2, juce::dontSendNotification);
          break;
        }
      }
    }
  }

  // Update the ComboBox selection to match loaded settings
  for (int i = 0; i < deviceComboBox.getNumItems(); ++i)
  {
    if (deviceComboBox.getItemText(i) == currentDevice)
    {
      deviceComboBox.setSelectedItemIndex(i, juce::dontSendNotification);
      break;
    }
  }

  // Update GPU device ID and visibility
  bool showGpuDeviceList =
      (currentDevice == "CUDA" || currentDevice == "DirectML");
  if (showGpuDeviceList)
  {
    // Update GPU device list for the loaded device type
    updateGPUDeviceList(currentDevice);
  }
  gpuDeviceComboBox.setSelectedId(gpuDeviceId + 1, juce::dontSendNotification);
  gpuDeviceLabel.setVisible(showGpuDeviceList);
  gpuDeviceComboBox.setVisible(showGpuDeviceList);

  // Update pitch detector combo box
  if (pitchDetectorType == PitchDetectorType::RMVPE)
    pitchDetectorComboBox.setSelectedId(1, juce::dontSendNotification);
  else if (pitchDetectorType == PitchDetectorType::FCPE)
    pitchDetectorComboBox.setSelectedId(2, juce::dontSendNotification);

  segmentsDebugToggle.setToggleState(showSegmentsDebug,
                                     juce::dontSendNotification);
  gameValuesDebugToggle.setToggleState(showGameValuesDebug,
                                       juce::dontSendNotification);
  uvInterpolationDebugToggle.setToggleState(showUvInterpolationDebug,
                                            juce::dontSendNotification);
  actualF0DebugToggle.setToggleState(showActualF0Debug,
                                     juce::dontSendNotification);

  hasLoadedSettings = true;
  lastConfirmedDevice = currentDevice;
  lastConfirmedGpuDeviceId = gpuDeviceId;

  if (!pluginMode && deviceManager != nullptr)
    updateAudioOutputDevices(true);
}

void SettingsComponent::saveSettings()
{
  // Don't save if combo box not initialized yet
  if (languageComboBox.getSelectedId() == 0)
    return;

  // Save language code
  int langId = languageComboBox.getSelectedId();
  juce::String langCode = "auto";
  if (langId >= 2)
  {
    const auto &langs = Localization::getInstance().getAvailableLanguages();
    int langIndex = langId - 2;
    if (langIndex < static_cast<int>(langs.size()))
      langCode = langs[langIndex].code;
  }

  if (settingsManager)
  {
    settingsManager->setDevice(currentDevice);
    settingsManager->setGPUDeviceId(gpuDeviceId);
    settingsManager->setPitchDetectorType(pitchDetectorType);
    settingsManager->setLanguage(langCode);
    settingsManager->setShowSegmentsDebug(showSegmentsDebug);
    settingsManager->setShowGameValuesDebug(showGameValuesDebug);
    settingsManager->setShowUvInterpolationDebug(showUvInterpolationDebug);
    settingsManager->setShowActualF0Debug(showActualF0Debug);
    settingsManager->setFollowSystemAudioOutput(followSystemAudioOutput);
    settingsManager->setPreferredAudioOutputDevice(preferredAudioOutputDevice);
    settingsManager->saveConfig();
  }
}

void SettingsComponent::updateAudioDeviceTypes()
{
  if (deviceManager == nullptr)
    return;

  const juce::ScopedValueSetter<bool> refreshingAudioUi(
      isRefreshingAudioControls, true);

  audioDeviceTypeComboBox.clear(juce::dontSendNotification);
  audioDeviceTypeOrder.clear();

  auto &types = deviceManager->getAvailableDeviceTypes();
  juce::AudioIODeviceType *asioType = nullptr;
  for (int i = 0; i < types.size(); ++i)
  {
    if (types[i]->getTypeName() == "ASIO")
    {
      asioType = types[i];
    }
    else
    {
      audioDeviceTypeOrder.add(types[i]);
    }
  }

  if (asioType != nullptr)
    audioDeviceTypeOrder.insert(0, asioType);

  for (int i = 0; i < audioDeviceTypeOrder.size(); ++i)
    audioDeviceTypeComboBox.addItem(
        audioDeviceTypeOrder[i]->getTypeName(), i + 1);

  if (auto *currentType = deviceManager->getCurrentDeviceTypeObject())
  {
    for (int i = 0; i < audioDeviceTypeOrder.size(); ++i)
    {
      if (audioDeviceTypeOrder[i] == currentType)
      {
        audioDeviceTypeComboBox.setSelectedId(i + 1,
                                              juce::dontSendNotification);
        break;
      }
    }
  }
  updateAudioOutputDevices(true);
}

void SettingsComponent::updateAudioOutputDevices(bool force)
{
  juce::ignoreUnused(force);
  if (deviceManager == nullptr)
    return;

  const juce::ScopedValueSetter<bool> refreshingAudioUi(
      isRefreshingAudioControls, true);

  auto *currentType = deviceManager->getCurrentDeviceTypeObject();
  if (currentType != nullptr)
  {
    auto devices = currentType->getDeviceNames(false); // false = output devices

    juce::String currentName;
    if (auto *audioDevice = deviceManager->getCurrentAudioDevice())
      currentName = audioDevice->getName();

    audioOutputComboBox.clear(juce::dontSendNotification);
    audioOutputComboBox.addItem("System Default", kFollowSystemOutputId);
    for (int i = 0; i < devices.size(); ++i)
      audioOutputComboBox.addItem(devices[i], i + 2);

    int targetId = kFollowSystemOutputId;
    if (!followSystemAudioOutput)
    {
      int preferredIdx = devices.indexOf(preferredAudioOutputDevice);
      if (preferredIdx >= 0)
      {
        targetId = preferredIdx + 2;
      }
      else
      {
        int currentIdx = devices.indexOf(currentName);
        if (currentIdx >= 0)
        {
          targetId = currentIdx + 2;
          preferredAudioOutputDevice = currentName;
        }
        else if (devices.size() > 0)
        {
          targetId = 2;
          preferredAudioOutputDevice = devices[0];
        }
        else
        {
          followSystemAudioOutput = true;
          targetId = kFollowSystemOutputId;
        }
      }
    }

    audioOutputComboBox.setSelectedId(targetId, juce::dontSendNotification);
    sampleRateComboBox.setEnabled(!followSystemAudioOutput);
    bufferSizeComboBox.setEnabled(!followSystemAudioOutput);
    outputChannelsComboBox.setEnabled(!followSystemAudioOutput);

    auto setup = deviceManager->getAudioDeviceSetup();
    int currentOutputChannels = 2;
    if (setup.useDefaultOutputChannels)
    {
      if (auto *audioDevice = deviceManager->getCurrentAudioDevice())
        currentOutputChannels =
            juce::jlimit(1, 2,
                         audioDevice->getActiveOutputChannels().countNumberOfSetBits());
    }
    else
    {
      currentOutputChannels =
          juce::jlimit(1, 2, setup.outputChannels.countNumberOfSetBits());
    }
    outputChannelsComboBox.setSelectedId(currentOutputChannels,
                                         juce::dontSendNotification);
  }

  updateSampleRates();
  updateBufferSizes();
}

void SettingsComponent::updateSampleRates()
{
  if (deviceManager == nullptr)
    return;

  const juce::ScopedValueSetter<bool> refreshingAudioUi(
      isRefreshingAudioControls, true);

  sampleRateComboBox.clear(juce::dontSendNotification);
  if (auto *device = deviceManager->getCurrentAudioDevice())
  {
    auto rates = device->getAvailableSampleRates();
    double currentRate = device->getCurrentSampleRate();
    for (int i = 0; i < rates.size(); ++i)
    {
      sampleRateComboBox.addItem(
          juce::String(static_cast<int>(rates[i])) + " Hz", i + 1);
      if (std::abs(rates[i] - currentRate) < 1.0)
        sampleRateComboBox.setSelectedId(i + 1, juce::dontSendNotification);
    }
  }
  else
  {
    auto setup = deviceManager->getAudioDeviceSetup();
    if (setup.sampleRate > 0.0)
    {
      sampleRateComboBox.addItem(
          juce::String(static_cast<int>(setup.sampleRate)) + " Hz", 1);
      sampleRateComboBox.setSelectedId(1, juce::dontSendNotification);
    }
  }
}

void SettingsComponent::updateBufferSizes()
{
  if (deviceManager == nullptr)
    return;

  const juce::ScopedValueSetter<bool> refreshingAudioUi(
      isRefreshingAudioControls, true);

  bufferSizeComboBox.clear(juce::dontSendNotification);
  if (auto *device = deviceManager->getCurrentAudioDevice())
  {
    auto sizes = device->getAvailableBufferSizes();
    int currentSize = device->getCurrentBufferSizeSamples();
    for (int i = 0; i < sizes.size(); ++i)
    {
      bufferSizeComboBox.addItem(juce::String(sizes[i]) + " samples", i + 1);
      if (sizes[i] == currentSize)
        bufferSizeComboBox.setSelectedId(i + 1, juce::dontSendNotification);
    }
  }
  else
  {
    auto setup = deviceManager->getAudioDeviceSetup();
    if (setup.bufferSize > 0)
    {
      bufferSizeComboBox.addItem(juce::String(setup.bufferSize) + " samples", 1);
      bufferSizeComboBox.setSelectedId(1, juce::dontSendNotification);
    }
  }
}

void SettingsComponent::applyAudioSettings()
{
  if (deviceManager == nullptr)
    return;

  auto setup = deviceManager->getAudioDeviceSetup();
  const auto originalSetup = setup;
  followSystemAudioOutput =
      (audioOutputComboBox.getSelectedId() == kFollowSystemOutputId);

  juce::String targetOutputName;
  if (followSystemAudioOutput)
  {
    targetOutputName = resolveDefaultOutputDeviceName(deviceManager);

    if (targetOutputName.isEmpty())
    {
      if (auto *audioDevice = deviceManager->getCurrentAudioDevice())
        targetOutputName = audioDevice->getName();
    }
  }
  else
  {
    if (audioOutputComboBox.getSelectedId() > kFollowSystemOutputId)
      targetOutputName = audioOutputComboBox.getText();
    else
      targetOutputName = preferredAudioOutputDevice;

    if (targetOutputName.isNotEmpty())
      preferredAudioOutputDevice = targetOutputName;
  }

  if (targetOutputName.isEmpty())
  {
    auto initError = deviceManager->initialiseWithDefaultDevices(0, 2);
    if (initError.isNotEmpty())
    {
      return;
    }

    followSystemAudioOutput = true;
    updateAudioOutputDevices(true);
    return;
  }

  const bool outputDeviceChanged = (setup.outputDeviceName != targetOutputName);
  setup.outputDeviceName = targetOutputName;
  const bool useDefaultOutputChannels = followSystemAudioOutput || outputDeviceChanged;

  // When following system default or switching output device, let JUCE pick a
  // valid sample-rate/buffer pair for the destination device.
  if (followSystemAudioOutput || outputDeviceChanged)
  {
    setup.sampleRate = 0.0;
    setup.bufferSize = 0;
  }
  else if (auto *device = deviceManager->getCurrentAudioDevice())
  {
    auto rates = device->getAvailableSampleRates();
    int rateIdx = sampleRateComboBox.getSelectedId() - 1;
    if (rateIdx >= 0 && rateIdx < rates.size())
      setup.sampleRate = rates[rateIdx];

    auto sizes = device->getAvailableBufferSizes();
    int sizeIdx = bufferSizeComboBox.getSelectedId() - 1;
    if (sizeIdx >= 0 && sizeIdx < sizes.size())
      setup.bufferSize = sizes[sizeIdx];
  }

  if (useDefaultOutputChannels)
  {
    setup.useDefaultOutputChannels = true;
    setup.outputChannels.clear();
  }
  else
  {
    setup.useDefaultOutputChannels = false;
    const int channels = juce::jlimit(1, 2, outputChannelsComboBox.getSelectedId());
    setup.outputChannels.setRange(0, channels, true);
  }

  if (setup == originalSetup)
  {
    return;
  }

  auto error = deviceManager->setAudioDeviceSetup(setup, true);
  if (error.isNotEmpty() && (setup.sampleRate != 0.0 || setup.bufferSize != 0))
  {
    auto fallback = setup;
    fallback.sampleRate = 0.0;
    fallback.bufferSize = 0;
    fallback.useDefaultOutputChannels = true;
    fallback.outputChannels.clear();
    error = deviceManager->setAudioDeviceSetup(fallback, true);
  }

  if (error.isNotEmpty())
  {
    updateAudioOutputDevices(true);
    return;
  }

  if (settingsManager)
  {
    settingsManager->setFollowSystemAudioOutput(followSystemAudioOutput);
    settingsManager->setPreferredAudioOutputDevice(preferredAudioOutputDevice);
    settingsManager->saveConfig();
  }

  updateAudioOutputDevices(true);
}

void SettingsComponent::syncToSystemOutputIfNeeded()
{
  if (!followSystemAudioOutput || deviceManager == nullptr)
    return;

  const auto targetOutputName = resolveDefaultOutputDeviceName(deviceManager);
  if (targetOutputName.isEmpty())
    return;

  juce::String currentName;
  if (auto *audioDevice = deviceManager->getCurrentAudioDevice())
    currentName = audioDevice->getName();
  if (currentName == targetOutputName)
    return;

  auto setup = deviceManager->getAudioDeviceSetup();
  setup.outputDeviceName = targetOutputName;
  setup.sampleRate = 0.0;
  setup.bufferSize = 0;
  setup.useDefaultOutputChannels = true;
  setup.outputChannels.clear();

  auto error = deviceManager->setAudioDeviceSetup(setup, true);
  if (error.isNotEmpty())
  {
    auto initError = deviceManager->initialiseWithDefaultDevices(0, 2);
    juce::ignoreUnused(initError);
  }
}

// SettingsOverlay and SettingsDialog implementations are in SettingsOverlay.cpp
