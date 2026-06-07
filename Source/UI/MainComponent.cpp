#include "MainComponent.h"
#include "Main/ExportHelper.h"
#include "Main/MacMenuIconHelper.h"
#include "../Audio/RealtimePitchProcessor.h"
#include "../Audio/IO/MidiExporter.h"
#include "../Models/ProjectSerializer.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Localization.h"
#include "../Utils/PlatformPaths.h"
#include "../Utils/SHA256Utils.h"
#include "../Utils/UI/WindowSizing.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>

MainComponent::MainComponent(bool enableAudioDevice)
    : enableAudioDeviceFlag(enableAudioDevice), pianoRollView(pianoRoll)
{
  LOG("MainComponent: constructor start");
  setSize(WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight);
  setOpaque(true); // Required for native title bar

  LOG("MainComponent: creating core components...");
  // Initialize components
  editorController = std::make_unique<EditorController>(enableAudioDeviceFlag);
  if (enableAudioDeviceFlag)
  {
    if (auto *project = editorController->getProject())
      project->setTimelineDisplayMode(TimelineDisplayMode::Time);
  }
  undoManager = std::make_unique<PitchUndoManager>(100);
  commandManager = std::make_unique<juce::ApplicationCommandManager>();
  undoManager->onHistoryChanged = [this]()
  {
    if (commandManager)
      commandManager->commandStatusChanged();
  };

  // Initialize new modular components
  fileManager = std::make_unique<AudioFileManager>();
  menuHandler = std::make_unique<MenuHandler>();
  settingsManager = std::make_unique<SettingsManager>();

  LOG("MainComponent: loading ONNX models...");
  editorController->setPitchDetectorType(
      settingsManager->getPitchDetectorType());
  editorController->setDeviceConfig(settingsManager->getDevice(),
                                    settingsManager->getGPUDeviceId());
  editorController->reloadInferenceModels(false);

  LOG("MainComponent: wiring up components...");
  menuHandler->setUndoManager(undoManager.get());
  menuHandler->setCommandManager(commandManager.get());
  menuHandler->setPluginMode(isPluginMode());
  recentFiles = settingsManager->getRecentFiles();
  refreshRecentFilesMenu();
  menuHandler->setOnRecentFileSelected(
      [this](const juce::File &file)
      { openRecentFile(file); });
  settingsManager->setVocoder(editorController->getVocoder());

  // Load vocoder settings
  settingsManager->applySettings();

  LOG("MainComponent: initializing audio device...");
  // Initialize audio (standalone app only)
  if (auto *audioEngine = editorController->getAudioEngine())
    audioEngine->initializeAudio();
  LOG("MainComponent: audio initialized");

  LOG("MainComponent: setting up callbacks...");

  // Initialize view state from settings
  pianoRoll.setShowDeltaPitch(settingsManager->getShowDeltaPitch());
  pianoRoll.setShowBasePitch(settingsManager->getShowBasePitch());
  pianoRoll.setShowSegmentsDebug(
      settingsManager->getShowSegmentsDebug());
  pianoRoll.setShowGameValuesDebug(
      settingsManager->getShowGameValuesDebug());
  pianoRoll.setShowUvInterpolationDebug(
      settingsManager->getShowUvInterpolationDebug());
  pianoRoll.setShowActualF0Debug(
      settingsManager->getShowActualF0Debug());
  pianoRollView.setShowSegmentsDebug(
      settingsManager->getShowSegmentsDebug());

  // Add child components - macOS uses native menu, others use in-app menu bar
#if JUCE_MAC
#else
  menuBar.setModel(menuHandler.get());
  menuBar.setLookAndFeel(&menuBarLookAndFeel);
  addAndMakeVisible(menuBar);
#endif
  addAndMakeVisible(toolbar);
  addAndMakeVisible(workspace);
  addChildComponent(analysisBackdrop);
  addChildComponent(analysisProgressPopup);

  // Setup workspace with stacked piano roll + overview cards
  workspace.setMainContent(&pianoRollView);
  workspace.getMainCard().setBackgroundColour(juce::Colours::transparentBlack);
  workspace.getMainCard().setBorderColour(juce::Colours::transparentBlack);

  // Add parameter panel to workspace.
  workspace.addPanel("parameters", TR("panel.parameters"), &parameterPanel,
                     false);

  // Configure toolbar for plugin mode
  if (isPluginMode())
    toolbar.setPluginMode(true);
  toolbar.setTransportEnabled(isPluginMode());

  // Set undo manager for piano roll
  pianoRoll.setUndoManager(undoManager.get());
  parameterPanel.setUndoManager(undoManager.get());

  // Setup toolbar callbacks
  toolbar.onPlay = [this]()
  { play(); };
  toolbar.onPause = [this]()
  { pause(); };
  toolbar.onStop = [this]()
  { stop(); };
  toolbar.onGoToStart = [this]()
  { jumpTransport(false); };
  toolbar.onGoToEnd = [this]()
  { jumpTransport(true); };
  toolbar.onZoomChanged = [this](float pps)
  { onZoomChanged(pps); };
  toolbar.onEditModeChanged = [this](EditMode mode)
  { setEditMode(mode); };
  toolbar.onToggleLoop = [this](bool enabled)
  {
    if (auto *project = getProject())
    {
      project->setLoopEnabled(enabled);
      const auto &range = project->getLoopRange();
      const bool hasValidRange = range.endSeconds > range.startSeconds;
      toolbar.setLoopEnabled(enabled);
      pianoRoll.repaint();

      if (auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr)
      {
        if (hasValidRange)
          audioEngine->setLoopRange(range.startSeconds, range.endSeconds);
        audioEngine->setLoopEnabled(range.enabled);
      }
    }
    else
    {
      toolbar.setLoopEnabled(false);
      pianoRoll.repaint();
    }
  };
  toolbar.onToggleParameters = [this](bool visible)
  {
    workspace.showPanel("parameters", visible);
  };

  // Removed onRender callback - Melodyne-style: edits automatically trigger
  // real-time processing

  // Setup piano roll callbacks
  pianoRoll.onSeek = [this](double time)
  { seek(time); };
  pianoRoll.onPreviewRegionRequested = [this](int startFrame, int endFrame)
  { previewNoteRegion(startFrame, endFrame); };
  pianoRoll.onNoteSelected = [this](Note *note)
  { onNoteSelected(note); };
  pianoRoll.onPitchEdited = [this]()
  { onPitchEdited(); };
  pianoRoll.onPitchEditFinished = [this]()
  {
    resynthesizeIncremental();
    // Melodyne-style: trigger real-time processor update in plugin mode
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };
  pianoRoll.onZoomChanged = [this](float pps)
  {
    onZoomChanged(pps);
    pianoRollView.refreshOverview();
  };
  pianoRoll.onScrollChanged = [this](double x)
  {
    juce::ignoreUnused(x);
    pianoRollView.refreshOverview();
  };
  pianoRoll.onLoopRangeChanged = [this](const LoopRange &range)
  {
    toolbar.setLoopEnabled(range.enabled);
    pianoRoll.repaint();
    if (auto *audioEngine = editorController
                                ? editorController->getAudioEngine()
                                : nullptr)
    {
      audioEngine->setLoopRange(range.startSeconds, range.endSeconds);
      audioEngine->setLoopEnabled(range.enabled);
    }
  };

  // Setup parameter panel callbacks
  parameterPanel.onParameterChanged = [this]()
  { onPitchEdited(); };
  parameterPanel.onParameterEditFinished = [this]()
  {
    resynthesizeIncremental();
    // Melodyne-style: trigger real-time processor update in plugin mode
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };
  parameterPanel.onScaleRootChanged = [this](int rootNote)
  {
    pianoRoll.setScaleRootNote(rootNote);
  };
  parameterPanel.onScaleRootPreviewChanged = [this](std::optional<int> rootNote)
  {
    pianoRoll.setScaleRootPreview(rootNote);
  };
  parameterPanel.onScaleModeChanged = [this](ScaleMode mode)
  {
    pianoRoll.setScaleMode(mode);
  };
  parameterPanel.onScaleModePreviewChanged =
      [this](std::optional<ScaleMode> mode)
  {
    pianoRoll.setScaleModePreview(mode);
  };
  parameterPanel.onShowScaleColorsChanged = [this](bool enabled)
  {
    pianoRoll.setShowScaleColors(enabled);
  };
  parameterPanel.onSnapToSemitonesChanged = [this](bool enabled)
  {
    pianoRoll.setSnapToSemitoneDrag(enabled);
  };
  parameterPanel.onPitchReferenceChanged = [this](int hz)
  {
    pianoRoll.setPitchReferenceHz(hz);
  };
  parameterPanel.onDoubleClickSnapModeChanged =
      [this](DoubleClickSnapMode mode)
  {
    pianoRoll.setDoubleClickSnapMode(mode);
  };
  parameterPanel.onTimelineDisplayModeChanged =
      [this](TimelineDisplayMode mode)
  {
    pianoRoll.setTimelineDisplayMode(mode);
  };
  parameterPanel.onTimelineBeatSignatureChanged =
      [this](int numerator, int denominator)
  {
    pianoRoll.setTimelineBeatSignature(numerator, denominator);
  };
  parameterPanel.onTimelineTempoChanged = [this](double bpm)
  {
    pianoRoll.setTimelineTempoBpm(bpm);
  };
  parameterPanel.onTimelineGridDivisionChanged =
      [this](TimelineGridDivision division)
  {
    pianoRoll.setTimelineGridDivision(division);
  };
  parameterPanel.onTimelineSnapCycleChanged = [this](bool enabled)
  {
    pianoRoll.setTimelineSnapCycle(enabled);
  };
  parameterPanel.setProject(getProject());

  // Sync toolbar toggle with panel visibility
  toolbar.setParametersVisible(workspace.isPanelVisible("parameters"));
  workspace.onPanelVisibilityChanged = [this](const juce::String &id, bool visible)
  {
    if (id == "parameters")
      toolbar.setParametersVisible(visible);
  };

  // Setup audio engine callbacks
  if (auto *audioEngine = editorController->getAudioEngine())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);

    audioEngine->setPositionCallback([safeThis](double position)
                                     {
      if (safeThis == nullptr)
        return;
      // Throttle cursor updates - store position and let timer handle it
      safeThis->pendingCursorTime.store(position);
      safeThis->hasPendingCursorUpdate.store(true); });

    audioEngine->setFinishCallback([safeThis]()
                                   {
      if (safeThis == nullptr)
        return;
      safeThis->isPlaying = false;
      safeThis->toolbar.setPlaying(false);
      safeThis->seek(0.0); });
  }

  // Set initial project
  pianoRoll.setProject(editorController->getProject());
  pianoRollView.setProject(editorController->getProject());

  // Register commands with the command manager
  commandManager->registerAllCommandsForTarget(this);
  commandManager->setFirstCommandTarget(this);

  // Connect MenuHandler to ApplicationCommandManager for automatic menu updates
  // This is required for macOS native menu bar to reflect command states
  menuHandler->setApplicationCommandManagerToWatch(commandManager.get());

#if JUCE_MAC
  if (!isPluginMode())
  {
    macExtraAppleMenuItems = menuHandler->getMacExtraAppleMenu();
    juce::MenuBarModel::setMacMainMenu(menuHandler.get(), &macExtraAppleMenuItems);
    MacMenuIconHelper::applySettingsMenuIcon(TR("command.settings"));
  }
#endif

  // Add command manager key mappings as a KeyListener
  // This enables automatic keyboard shortcut dispatch
  addKeyListener(commandManager->getKeyMappings());
  setWantsKeyboardFocus(true);

  // Load config
  if (enableAudioDeviceFlag)
    settingsManager->loadConfig();

  LOG("MainComponent: starting timer...");
  // Start timer for UI updates
  startTimerHz(30);
  LOG("MainComponent: constructor complete");
}

void MainComponent::reloadInferenceModels(bool async)
{
  if (!settingsManager || !editorController)
    return;

  editorController->setDeviceConfig(settingsManager->getDevice(),
                                    settingsManager->getGPUDeviceId());
  editorController->reloadInferenceModels(async);
}

bool MainComponent::isInferenceBusy() const
{
  if (isLoadingAudio.load())
    return true;
  if (editorController && editorController->isLoading())
    return true;
  if (editorController && editorController->isRendering())
    return true;
  if (editorController && editorController->isInferenceBusy())
    return true;
  return false;
}

MainComponent::~MainComponent()
{
#if JUCE_MAC
  juce::MenuBarModel::setMacMainMenu(nullptr);
#else
  menuBar.setModel(nullptr);
  menuBar.setLookAndFeel(nullptr);
#endif
  removeKeyListener(commandManager->getKeyMappings());
  stopTimer();

  if (auto *audioEngine = editorController->getAudioEngine())
  {
    audioEngine->clearCallbacks();
    audioEngine->shutdownAudio();
  }

  if (enableAudioDeviceFlag)
    settingsManager->saveConfig();
}

juce::Point<int> MainComponent::getSavedWindowSize() const
{
  if (settingsManager)
    return {settingsManager->getWindowWidth(),
            settingsManager->getWindowHeight()};
  return {WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight};
}

void MainComponent::paint(juce::Graphics &g)
{
  g.fillAll(APP_COLOR_BACKGROUND);
}

void MainComponent::paintOverChildren(juce::Graphics &g)
{
  if (settingsOverlay != nullptr && settingsOverlay->isVisible())
    return;

  constexpr int scrollBarWidth = 8;
  const int y = toolbar.getBottom();
  if (y >= getHeight())
    return;

  g.setColour(juce::Colour(0xFF3C3C3Cu));
  g.fillRect(0, y, juce::jmax(0, getWidth() - scrollBarWidth), 1);
}

void MainComponent::resized()
{
  auto bounds = getLocalBounds();

#if !JUCE_MAC
  // Menu bar at top for non-mac platforms
  menuBar.setBounds(bounds.removeFromTop(24));
#endif

  // Toolbar
  toolbar.setBounds(bounds.removeFromTop(60));

  // Workspace takes remaining space (includes piano roll, panels, and sidebar)
  workspace.setBounds(bounds);

  if (settingsOverlay)
    settingsOverlay->setBounds(getLocalBounds());

  analysisBackdrop.setBounds(getLocalBounds());
  analysisProgressPopup.setBounds(getLocalBounds().withSizeKeepingCentre(399, 98));

  if (enableAudioDeviceFlag && settingsManager)
    settingsManager->setWindowSize(getWidth(), getHeight());
}

void MainComponent::mouseDown(const juce::MouseEvent &e)
{
  juce::ignoreUnused(e);
}

void MainComponent::mouseDrag(const juce::MouseEvent &e)
{
  juce::ignoreUnused(e);
}

void MainComponent::mouseDoubleClick(const juce::MouseEvent &e)
{
  juce::ignoreUnused(e);
}

void MainComponent::timerCallback()
{
  // Handle throttled cursor updates (30Hz max)
  if (hasPendingCursorUpdate.load())
  {
    double position = pendingCursorTime.load();
    hasPendingCursorUpdate.store(false);

    if (previewRegionActive)
    {
      if (position >= previewRegionEndTime)
        finishPreviewRegion(true);
      else
        pianoRoll.setPreviewPlaybackPosition(position);
      return;
    }

    pianoRoll.setCursorTime(position);
    toolbar.setCurrentTime(position);

    // Follow playback: scroll to keep cursor visible
    if (isPlaying && toolbar.isFollowPlayback())
    {
      float cursorX =
          static_cast<float>(position * pianoRoll.getPixelsPerSecond());
      float viewWidth = static_cast<float>(pianoRoll.getVisibleContentWidth());
      float scrollX = static_cast<float>(pianoRoll.getScrollX());

      // If cursor is outside visible area, scroll to center it
      if (cursorX < scrollX || cursorX > scrollX + viewWidth)
      {
        double newScrollX =
            std::max(0.0, static_cast<double>(cursorX - viewWidth * 0.3f));
        pianoRoll.setScrollX(newScrollX);
      }
    }
  }

  if (isLoadingAudio.load())
  {
    const auto progress = static_cast<float>(loadingProgress.load());

    juce::String msg;
    {
      const juce::ScopedLock sl(loadingMessageLock);
      msg = loadingMessage;
    }

    toolbar.hideProgress();
    showAnalysisProgress(progress);
    lastLoadingMessage = msg;

    return;
  }

  if (lastLoadingMessage.isNotEmpty())
  {
    toolbar.hideProgress();
    hideAnalysisProgress();
    lastLoadingMessage.clear();
  }
}

bool MainComponent::keyPressed(const juce::KeyPress &key,
                               juce::Component * /*originatingComponent*/)
{
  // All keyboard shortcuts are now handled by ApplicationCommandManager
  // This method is kept for potential future non-command keyboard handling
  juce::ignoreUnused(key);
  return false;
}

void MainComponent::saveProject()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->saveProject(); });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  auto ensureAudioSha = [project]()
  {
    auto audioFile = project->getFilePath();
    if (audioFile.existsAsFile())
      project->setAudioSha256(SHA256Utils::fileSHA256(audioFile));
  };

  auto target = project->getProjectFilePath();
  if (target == juce::File{})
  {
    // Prevent re-triggering while dialog is open
    if (fileChooser != nullptr)
      return;

    // Default next to audio if possible
    auto audio = project->getFilePath();
    if (audio.existsAsFile())
      target = audio.withFileExtension("htpx");
    else
      target =
          juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
              .getChildFile("Untitled.htpx");

#if JUCE_WINDOWS && JUCE_MODAL_LOOPS_PERMITTED
    juce::FileChooser chooser(TR("dialog.save_project"), target, "*.htpx",
                              true, false, this);
    if (!chooser.browseForFileToSave(true))
      return;

    auto file = chooser.getResult();
    if (file == juce::File{})
      return;

    if (file.getFileExtension().isEmpty())
      file = file.withFileExtension("htpx");

    toolbar.showProgress(TR("progress.saving"));
    toolbar.setProgress(-1.0f);

    ensureAudioSha();
    const bool ok = ProjectSerializer::saveToFile(*project, file);
    if (ok)
      project->setProjectFilePath(file);

    toolbar.hideProgress();
    return;
#else
    fileChooser = std::make_unique<juce::FileChooser>(
        TR("dialog.save_project"), target, "*.htpx");

    auto chooserFlags = juce::FileBrowserComponent::saveMode |
                        juce::FileBrowserComponent::canSelectFiles |
                        juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    fileChooser->launchAsync(chooserFlags, [safeThis](
                                               const juce::FileChooser &fc)
                             {
      if (safeThis == nullptr)
        return;

      auto file = fc.getResult();
      safeThis->fileChooser
          .reset(); // Allow next dialog to open (must be after getResult)

      if (file == juce::File{})
        return;

      if (file.getFileExtension().isEmpty())
        file = file.withFileExtension("htpx");

      safeThis->toolbar.showProgress(TR("progress.saving"));
      safeThis->toolbar.setProgress(-1.0f);

      auto *project = safeThis ? safeThis->getProject() : nullptr;
      if (!project) {
        safeThis->toolbar.hideProgress();
        return;
      }
      auto audioFile = project->getFilePath();
      if (audioFile.existsAsFile())
        project->setAudioSha256(SHA256Utils::fileSHA256(audioFile));
      const bool ok = ProjectSerializer::saveToFile(*project, file);
      if (ok)
        project->setProjectFilePath(file);

      safeThis->toolbar.hideProgress(); });

    return;
#endif
  }

  toolbar.showProgress(TR("progress.saving"));
  toolbar.setProgress(-1.0f);

  ensureAudioSha();
  const bool ok = ProjectSerializer::saveToFile(*project, target);
  juce::ignoreUnused(ok);

  toolbar.hideProgress();
}

void MainComponent::openFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->openFile(); });
    return;
  }

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.select_audio"), juce::File{},
      "*.htpx;*.wav;*.mp3;*.flac;*.aiff");

  auto chooserFlags = juce::FileBrowserComponent::openMode |
                      juce::FileBrowserComponent::canSelectFiles;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc)
      {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser.reset(); // Allow next dialog to open
        if (file.existsAsFile()) {
          safeThis->addRecentFile(file);
          if (file.hasFileExtension("htpx") || file.hasFileExtension(".htpx"))
            safeThis->openProjectFile(file);
          else
            safeThis->loadAudioFile(file);
        } });
}

void MainComponent::refreshRecentFilesMenu()
{
  if (menuHandler)
    menuHandler->setRecentFiles(recentFiles);
  if (menuHandler)
    menuHandler->menuItemsChanged();
}

void MainComponent::addRecentFile(const juce::File &file)
{
  if (!file.existsAsFile())
    return;

  const auto fullPath = file.getFullPathName();
  recentFiles.removeString(fullPath);
  recentFiles.insert(0, fullPath);

  constexpr int kMaxRecentFiles = 10;
  while (recentFiles.size() > kMaxRecentFiles)
    recentFiles.remove(recentFiles.size() - 1);

  if (settingsManager)
  {
    settingsManager->setLastFilePath(file);
    settingsManager->setRecentFiles(recentFiles);
    settingsManager->saveConfig();
  }
  refreshRecentFilesMenu();
}

void MainComponent::openRecentFile(const juce::File &file)
{
  if (!file.existsAsFile())
  {
    StyledMessageBox::show(this, "Recent file missing",
                           "File not found:\n" + file.getFullPathName(),
                           StyledMessageBox::WarningIcon);
    recentFiles.removeString(file.getFullPathName());
    if (settingsManager)
    {
      settingsManager->setRecentFiles(recentFiles);
      settingsManager->saveConfig();
    }
    refreshRecentFilesMenu();
    return;
  }

  if (file.hasFileExtension("htpx") || file.hasFileExtension(".htpx"))
    openProjectFile(file);
  else
    loadAudioFile(file);
}

// openProjectFile and loadAudioFile implementations are in Main/MainComponent_ProjectIO.cpp

void MainComponent::analyzeAudio()
{
  auto *project = getProject();
  if (!project || !editorController)
    return;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->analyzeAudioAsync(
      [safeThis](Project &projectRef)
      {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.setProject(&projectRef);
        safeThis->pianoRollView.setProject(&projectRef);
        safeThis->fitAnalyzedPitchRangeToView(projectRef);
        safeThis->pianoRoll.repaint();
      },
      [safeThis]()
      {
        if (safeThis == nullptr)
          return;
        safeThis->notifyProjectDataChanged();
      });
}

void MainComponent::analyzeAudio(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress,
    std::function<void()> onComplete)
{
  if (editorController)
  {
    editorController->analyzeAudio(targetProject, onProgress, onComplete);
  }
}

void MainComponent::fitAnalyzedPitchRangeToView(Project &project)
{
  float minMidi = std::numeric_limits<float>::max();
  float maxMidi = std::numeric_limits<float>::lowest();

  for (const auto &note : project.getNotes())
  {
    if (note.isRest())
      continue;

    const float midi = note.getAdjustedMidiNote();
    if (!std::isfinite(midi))
      continue;

    minMidi = std::min(minMidi, midi);
    maxMidi = std::max(maxMidi, midi);
  }

  if (minMidi == std::numeric_limits<float>::max())
  {
    const auto &f0 = project.getAudioData().f0;
    for (float freq : f0)
    {
      if (freq <= 50.0f || !std::isfinite(freq))
        continue;

      const float midi = freqToMidi(freq);
      if (!std::isfinite(midi))
        continue;

      minMidi = std::min(minMidi, midi);
      maxMidi = std::max(maxMidi, midi);
    }
  }

  if (minMidi != std::numeric_limits<float>::max() && maxMidi >= minMidi)
  {
    maxMidi = std::min(maxMidi + 1.0f, static_cast<float>(MAX_MIDI_NOTE));
    pianoRoll.fitPitchRangeToView(minMidi, maxMidi);
  }
}

void MainComponent::exportFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->exportFile(); });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  const int inputSampleRate = project->getAudioData().sampleRate;
  juce::Component::SafePointer<MainComponent> safeThis(this);
  ExportHelper::showExportSettingsDialogAsync(
      this, inputSampleRate,
      [safeThis](std::optional<ExportHelper::ExportSettings> exportSettings)
      {
        if (safeThis == nullptr || !exportSettings.has_value())
          return;

        auto performExport = [safeThis, exportSettings](const juce::File &targetFile)
        {
          if (safeThis == nullptr)
            return;

          auto *activeProject = safeThis->getProject();
          if (!activeProject)
            return;

          auto file = targetFile;
          const auto extension = ExportHelper::getFormatExtension(exportSettings->format);
          if (file.getFileExtension().isEmpty())
            file = file.withFileExtension(extension);

          auto &audioData = activeProject->getAudioData();
          juce::AudioBuffer<float> sourceBuffer;
          sourceBuffer.makeCopyOf(audioData.waveform);
          const int sourceRate = audioData.sampleRate;

          safeThis->toolbar.showProgress(TR("progress.exporting_audio"));
          safeThis->toolbar.setProgress(-1.0f);
          safeThis->toolbar.setEnabled(false);

          std::thread([safeThis, settings = *exportSettings, file = std::move(file),
                       sourceBuffer = std::move(sourceBuffer), sourceRate]() mutable
                      {
            juce::String error;
            bool success = false;

            do {
              juce::AudioBuffer<float> exportBuffer =
                  ExportHelper::convertChannels(sourceBuffer, settings.channels);
              exportBuffer = ExportHelper::resampleAudio(exportBuffer, sourceRate, settings.sampleRate);

              if (file.existsAsFile() && !file.deleteFile()) {
                error = TR("dialog.failed_delete") + "\n" + file.getFullPathName();
                break;
              }

              auto fileStream = std::make_unique<juce::FileOutputStream>(file);
              if (!fileStream || !fileStream->openedOk()) {
                error = TR("dialog.failed_open") + "\n" + file.getFullPathName();
                break;
              }
              std::unique_ptr<juce::OutputStream> outputStream = std::move(fileStream);

              juce::AudioFormatManager formatManager;
              formatManager.registerBasicFormats();
              auto *format = ExportHelper::findFormatForExtension(
                  formatManager, ExportHelper::getFormatExtension(settings.format));
              if (!format) {
                error = "No exporter is available for format: " +
                        ExportHelper::getFormatDisplayName(settings.format);
                break;
              }

              auto writerOptions = juce::AudioFormatWriterOptions{}
                                       .withSampleRate(settings.sampleRate)
                                       .withNumChannels(exportBuffer.getNumChannels())
                                       .withBitsPerSample(settings.bitsPerSample);
              if (format->isCompressed()) {
                writerOptions = writerOptions.withQualityOptionIndex(
                    ExportHelper::chooseQualityIndex(format->getQualityOptions(),
                                       settings.bitrateKbps));
              }

              auto writer = format->createWriterFor(outputStream, writerOptions);
              if (!writer) {
                error = TR("dialog.failed_create_writer") + "\n" +
                        file.getFullPathName();
                break;
              }

              success = writer->writeFromAudioSampleBuffer(
                  exportBuffer, 0, exportBuffer.getNumSamples());
              writer->flush();
              writer.reset();
              if (!success) {
                error = TR("dialog.failed_write") + "\n" + file.getFullPathName();
                break;
              }
            } while (false);

            juce::MessageManager::callAsync([safeThis, success, error, file]() {
              if (safeThis == nullptr)
                return;
              safeThis->toolbar.setEnabled(true);
              safeThis->toolbar.hideProgress();
              if (success) {
                StyledMessageBox::show(
                    safeThis.getComponent(), TR("dialog.export_complete"),
                    TR("dialog.audio_exported") + "\n" + file.getFullPathName(),
                    StyledMessageBox::InfoIcon);
              } else {
                StyledMessageBox::show(
                    safeThis.getComponent(), TR("dialog.export_failed"), error,
                    StyledMessageBox::WarningIcon);
              }
            }); })
              .detach();
        };

        if (safeThis->fileChooser != nullptr)
          return;

        safeThis->fileChooser = std::make_unique<juce::FileChooser>(
            TR("dialog.save_audio"), juce::File{},
            ExportHelper::getFormatWildcard(exportSettings->format));

        auto chooserFlags = juce::FileBrowserComponent::saveMode |
                            juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting;

        safeThis->fileChooser->launchAsync(
            chooserFlags, [safeThis, performExport](const juce::FileChooser &fc)
            {
              if (safeThis == nullptr)
                return;
              const auto file = fc.getResult();
              safeThis->fileChooser.reset();
              if (file == juce::File{})
                return;
              performExport(file); });
      });
}

void MainComponent::exportMidiFile()
{
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]()
                                    {
      if (safeThis != nullptr)
        safeThis->exportMidiFile(); });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  const auto &notes = project->getNotes();
  if (notes.empty())
  {
    StyledMessageBox::show(this, TR("dialog.export_failed"),
                           TR("dialog.no_notes_to_export"),
                           StyledMessageBox::WarningIcon);
    return;
  }

  // Suggest filename based on project or audio file
  juce::File defaultFile;
  if (project->getFilePath().existsAsFile())
  {
    defaultFile = project->getFilePath().withFileExtension("mid");
  }
  else if (project->getProjectFilePath().existsAsFile())
  {
    defaultFile = project->getProjectFilePath().withFileExtension("mid");
  }

#if JUCE_WINDOWS && JUCE_MODAL_LOOPS_PERMITTED
  juce::FileChooser chooser(TR("dialog.export_midi"), defaultFile,
                            "*.mid;*.midi", true, false, this);
  if (!chooser.browseForFileToSave(true))
    return;

  auto file = chooser.getResult();
  if (file == juce::File{})
    return;

  // Ensure .mid extension
  if (file.getFileExtension().isEmpty())
    file = file.withFileExtension("mid");

  // Show progress
  toolbar.showProgress(TR("progress.exporting_midi"));
  toolbar.setProgress(0.3f);

  // Configure export options
  MidiExporter::ExportOptions options;
  options.tempo = static_cast<float>(project->getTimelineTempoBpm());
  options.ticksPerQuarterNote = 480;
  options.velocity = 100;
  options.quantizePitch = true; // Round to nearest semitone

  toolbar.setProgress(0.6f);

  // Export using adjusted pitch data (includes user edits)
  bool success = MidiExporter::exportToFile(project->getNotes(), file, options);

  toolbar.setProgress(1.0f);
  toolbar.hideProgress();

  if (success)
  {
    StyledMessageBox::show(
        this, TR("dialog.export_complete"),
        TR("dialog.midi_exported") + "\n" + file.getFullPathName(),
        StyledMessageBox::InfoIcon);
  }
  else
  {
    StyledMessageBox::show(
        this, TR("dialog.export_failed"),
        TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
        StyledMessageBox::WarningIcon);
  }
#else
  fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.export_midi"), defaultFile, "*.mid;*.midi");

  auto chooserFlags = juce::FileBrowserComponent::saveMode |
                      juce::FileBrowserComponent::canSelectFiles |
                      juce::FileBrowserComponent::warnAboutOverwriting;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc)
      {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser
            .reset(); // Allow next dialog to open (must be after getResult)

        if (file == juce::File{})
          return;

        // Ensure .mid extension
        if (file.getFileExtension().isEmpty())
          file = file.withFileExtension("mid");

        // Show progress
        safeThis->toolbar.showProgress(TR("progress.exporting_midi"));
        safeThis->toolbar.setProgress(0.3f);

        auto *project = safeThis ? safeThis->getProject() : nullptr;
        if (!project)
          return;

        // Configure export options
        MidiExporter::ExportOptions options;
        options.tempo = static_cast<float>(project->getTimelineTempoBpm());
        options.ticksPerQuarterNote = 480;
        options.velocity = 100;
        options.quantizePitch = true; // Round to nearest semitone

        safeThis->toolbar.setProgress(0.6f);

        // Export using adjusted pitch data (includes user edits)
        bool success =
            MidiExporter::exportToFile(project->getNotes(), file, options);

        safeThis->toolbar.setProgress(1.0f);
        safeThis->toolbar.hideProgress();

        if (success) {
          StyledMessageBox::show(
              safeThis.getComponent(), TR("dialog.export_complete"),
              TR("dialog.midi_exported") + "\n" + file.getFullPathName(),
              StyledMessageBox::InfoIcon);
        } else {
          StyledMessageBox::show(
              safeThis.getComponent(), TR("dialog.export_failed"),
              TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
              StyledMessageBox::WarningIcon);
        } });
#endif
}

void MainComponent::play()
{
  auto *project = getProject();

  // In plugin mode, playback is controlled by the host. Update immediately for
  // responsiveness; host/ARA callbacks will keep the state in sync.
  if (isPluginMode())
  {
    if (onRequestHostPlayState)
      onRequestHostPlayState(true);
    isPlaying = true;
    toolbar.setPlaying(true);
    return;
  }

  if (!project || project->getAudioData().waveform.getNumSamples() == 0)
  {
    isPlaying = false;
    toolbar.setPlaying(false);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;

  const auto &loopRange = project->getLoopRange();
  if (loopRange.isValid())
  {
    double position = audioEngine->getPosition();
    if (position < loopRange.startSeconds || position >= loopRange.endSeconds)
    {
      audioEngine->seek(loopRange.startSeconds);
      pianoRoll.setCursorTime(loopRange.startSeconds);
      toolbar.setCurrentTime(loopRange.startSeconds);
    }
  }

  isPlaying = true;
  toolbar.setPlaying(true);
  audioEngine->play();
}

void MainComponent::pause()
{
  if (previewRegionActive)
  {
    finishPreviewRegion(true);
    return;
  }

  // In plugin mode, playback is controlled by the host
  if (isPluginMode())
  {
    if (onRequestHostPlayState)
      onRequestHostPlayState(false);
    isPlaying = false;
    toolbar.setPlaying(false);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  isPlaying = false;
  toolbar.setPlaying(false);
  audioEngine->pause();
}

void MainComponent::stop()
{
  if (previewRegionActive)
  {
    finishPreviewRegion(false);
  }

  // In plugin mode, playback is controlled by the host
  if (isPluginMode())
  {
    if (onRequestHostStop)
      onRequestHostStop();
    isPlaying = false;
    toolbar.setPlaying(false);
    seek(0.0);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  isPlaying = false;
  toolbar.setPlaying(false);
  audioEngine->stop();
  seek(0.0);
}

void MainComponent::seek(double time)
{
  // In plugin mode, request a host seek when the host supports it and update
  // the UI cursor immediately for responsiveness.
  if (isPluginMode())
  {
    if (onRequestHostSeek)
      onRequestHostSeek(time);
    pendingCursorTime.store(time);
    pianoRoll.setCursorTime(time);
    toolbar.setCurrentTime(time);
    return;
  }

  // Standalone mode: use AudioEngine for seeking
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  audioEngine->seek(time);
  pendingCursorTime.store(time);
  pianoRoll.setCursorTime(time);
  toolbar.setCurrentTime(time);

  // Scroll view to make cursor visible
  float cursorX = static_cast<float>(time * pianoRoll.getPixelsPerSecond());
  float viewWidth = static_cast<float>(pianoRoll.getVisibleContentWidth());
  float scrollX = static_cast<float>(pianoRoll.getScrollX());

  // If cursor is outside visible area, scroll to show it
  if (cursorX < scrollX || cursorX > scrollX + viewWidth)
  {
    // Center cursor in view, or scroll to start if time is 0
    double newScrollX;
    if (time < 0.001)
    {
      newScrollX = 0.0; // Go to start
    }
    else
    {
      newScrollX =
          std::max(0.0, static_cast<double>(cursorX - viewWidth * 0.3));
    }
    pianoRoll.setScrollX(newScrollX);
  }
}

void MainComponent::previewNoteRegion(int startFrame, int endFrame)
{
  if (endFrame <= startFrame)
    return;

  const double startTime = framesToSeconds(startFrame);
  const double endTime = framesToSeconds(endFrame);
  if (endTime <= startTime)
    return;

  if (isPluginMode())
    return;

  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;

  previewRegionActive = true;
  previewRegionEndTime = endTime;
  previewRegionReturnTime = pianoRoll.getCursorTime();
  previewRegionWasProjectPlaying = isPlaying;
  pianoRoll.setPreviewPlaybackState(true, startFrame, endFrame);
  pianoRoll.setPreviewPlaybackPosition(startTime);

  audioEngine->seek(startTime);
  audioEngine->play();
}

void MainComponent::finishPreviewRegion(bool restorePosition)
{
  if (!previewRegionActive)
    return;

  const double returnTime = previewRegionReturnTime;
  const bool shouldResumeProjectPlayback =
      previewRegionWasProjectPlaying && restorePosition;
  previewRegionActive = false;
  previewRegionWasProjectPlaying = false;
  pianoRoll.setPreviewPlaybackState(false, 0, 0);

  if (auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr)
  {
    audioEngine->pause();
    if (restorePosition)
      audioEngine->seek(returnTime);
    if (shouldResumeProjectPlayback)
      audioEngine->play();
  }

  pendingCursorTime.store(returnTime);
  hasPendingCursorUpdate.store(false);
}

void MainComponent::jumpTransport(bool forward)
{
  auto *project = getProject();
  if (!project)
    return;

  double currentTime = pendingCursorTime.load();
  if (!isPluginMode())
  {
    if (auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr)
      currentTime = audioEngine->getPosition();
  }

  const double duration = project->getAudioData().getDuration();
  double targetTime = currentTime;

  if (project->getTimelineDisplayMode() == TimelineDisplayMode::Beats)
  {
    const double bpm = juce::jlimit(20.0, 300.0, project->getTimelineTempoBpm());
    const int numerator = juce::jmax(1, project->getTimelineBeatNumerator());
    const int denominator = juce::jmax(1, project->getTimelineBeatDenominator());
    const double beatSeconds = (60.0 / bpm) * (4.0 / static_cast<double>(denominator));
    const double barSeconds = beatSeconds * static_cast<double>(numerator);

    if (barSeconds > 1.0e-6)
    {
      constexpr double epsilon = 1.0e-6;
      const double barPosition = currentTime / barSeconds;
      targetTime = forward
                       ? (std::floor(barPosition + epsilon) + 1.0) * barSeconds
                       : (std::ceil(barPosition - epsilon) - 1.0) * barSeconds;
    }
  }
  else
  {
    constexpr double epsilon = 1.0e-6;
    targetTime = forward
                     ? (std::floor(currentTime / 2.0 + epsilon) + 1.0) * 2.0
                     : (std::ceil(currentTime / 2.0 - epsilon) - 1.0) * 2.0;
  }

  seek(juce::jlimit(0.0, duration, targetTime));
}

void MainComponent::resynthesizeIncremental()
{

  auto *project = getProject();
  if (!project || !editorController)
  {
    return;
  }

  toolbar.showProgress(TR("progress.synthesizing"));
  toolbar.setProgress(-1.0f);
  toolbar.setEnabled(false);

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->resynthesizeIncrementalAsync(
      *project,
      [safeThis](const juce::String &message)
      {
        if (safeThis == nullptr)
          return;
        safeThis->toolbar.showProgress(message);
      },
      [safeThis](bool success)
      {
        if (safeThis == nullptr)
          return;

        safeThis->toolbar.setEnabled(true);
        safeThis->toolbar.hideProgress();

        if (!success)
        {
          return;
        }

        safeThis->pianoRoll.invalidateWaveformCache();
        safeThis->pianoRoll.repaint();
        if (safeThis->isPluginMode())
          safeThis->notifyProjectDataChanged();
      },
      pendingIncrementalResynth,
      isPluginMode());
}

void MainComponent::onNoteSelected(Note *note)
{
  parameterPanel.setSelectedNote(note);
}

void MainComponent::onPitchEdited()
{
  pianoRoll.repaint();
  pianoRollView.refreshOverview();
  parameterPanel.updateFromNote();
}

void MainComponent::onZoomChanged(float pixelsPerSecond)
{
  if (isSyncingZoom)
    return;

  isSyncingZoom = true;

  // Update all components with zoom centered on cursor
  pianoRoll.setPixelsPerSecond(pixelsPerSecond, true);
  toolbar.setZoom(pixelsPerSecond);
  pianoRollView.refreshOverview();

  isSyncingZoom = false;
}

void MainComponent::notifyProjectDataChanged()
{
  if (onProjectDataChanged)
    onProjectDataChanged();
}

void MainComponent::undo()
{
  // Cancel any in-progress drawing first
  pianoRoll.cancelDrawing();

  if (undoManager && undoManager->canUndo())
  {
    undoManager->undo();
    parameterPanel.updateFromNote();
    pianoRoll.invalidateBasePitchCache(); // Refresh cache after note split etc.
    pianoRoll.repaint();
    pianoRollView.refreshOverview();

    if (getProject())
    {
      // Don't mark all notes as dirty - let undo action callbacks handle
      // the specific dirty range. This avoids synthesizing the entire project.
      // The undo action's callback will set the correct F0 dirty range.
      resynthesizeIncremental();
    }

    // Update command states (undo/redo availability changed)
    if (commandManager)
      commandManager->commandStatusChanged();
  }
}

void MainComponent::redo()
{
  if (undoManager && undoManager->canRedo())
  {
    undoManager->redo();
    parameterPanel.updateFromNote();
    pianoRoll.invalidateBasePitchCache(); // Refresh cache after note split etc.
    pianoRoll.repaint();
    pianoRollView.refreshOverview();

    if (getProject())
    {
      // Don't mark all notes as dirty - let redo action callbacks handle
      // the specific dirty range. This avoids synthesizing the entire project.
      // The redo action's callback will set the correct F0 dirty range.
      resynthesizeIncremental();
    }

    // Update command states (undo/redo availability changed)
    if (commandManager)
      commandManager->commandStatusChanged();
  }
}

void MainComponent::setEditMode(EditMode mode)
{
  pianoRoll.setEditMode(mode);
  toolbar.setEditMode(mode);

  // Update command states (draw mode toggle state changed)
  if (commandManager)
    commandManager->commandStatusChanged();
}

void MainComponent::segmentIntoNotes()
{
  auto *project = getProject();
  if (!project || !editorController)
    return;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->segmentIntoNotesAsync(
      [safeThis](Project &projectRef)
      {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.setProject(&projectRef);
        safeThis->pianoRollView.setProject(&projectRef);
      },
      [safeThis]()
      {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.invalidateBasePitchCache();
        safeThis->pianoRoll.repaint();
      });
}

void MainComponent::segmentIntoNotes(Project &targetProject)
{
  if (editorController)
  {
    editorController->segmentIntoNotes(targetProject, [this]()
                                       {
      pianoRoll.invalidateBasePitchCache();
      pianoRoll.repaint(); },
                                       [this](double progress)
                                       { showAnalysisProgress(progress); });
    hideAnalysisProgress();
  }
}

void MainComponent::showAnalysisProgress(double progress)
{
  analysisProgressPopup.setProgress(progress);
  if (!analysisBackdrop.isVisible())
    analysisBackdrop.setVisible(true);
  analysisBackdrop.toFront(false);

  if (!analysisProgressPopup.isVisible())
    analysisProgressPopup.setVisible(true);
  analysisProgressPopup.toFront(false);
}

void MainComponent::hideAnalysisProgress()
{
  if (analysisProgressPopup.isVisible())
    analysisProgressPopup.setVisible(false);
  if (analysisBackdrop.isVisible())
    analysisBackdrop.setVisible(false);
}

void MainComponent::showSettings()
{
  if (!settingsOverlay)
  {
    // Pass AudioDeviceManager only in standalone mode
    juce::AudioDeviceManager *deviceMgr = nullptr;
    if (!isPluginMode() && editorController &&
        editorController->getAudioEngine())
      deviceMgr = &editorController->getAudioEngine()->getDeviceManager();

    settingsOverlay =
        std::make_unique<SettingsOverlay>(settingsManager.get(), deviceMgr);
    addAndMakeVisible(settingsOverlay.get());
    settingsOverlay->setVisible(false);
    settingsOverlay->onClose = [this]()
    {
      if (settingsOverlay)
        settingsOverlay->setVisible(false);
    };

    settingsOverlay->getSettingsComponent()->onSettingsChanged = [this]()
    {
      settingsManager->applySettings();
      reloadInferenceModels(true);
    };
    settingsOverlay->getSettingsComponent()->canChangeDevice = [this]()
    {
      return !isInferenceBusy();
    };
    settingsOverlay->getSettingsComponent()->onPitchDetectorChanged =
        [this](PitchDetectorType type)
    {
      if (editorController)
        editorController->setPitchDetectorType(type);
    };
    settingsOverlay->getSettingsComponent()->onShowSegmentsDebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowSegmentsDebug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowSegmentsDebug(show);
      pianoRollView.setShowSegmentsDebug(show);
      pianoRollView.refreshOverview();
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowGameValuesDebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowGameValuesDebug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowGameValuesDebug(show);
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowUvInterpolationDebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowUvInterpolationDebug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowUvInterpolationDebug(show);
      pianoRoll.repaint();
    };
    settingsOverlay->getSettingsComponent()->onShowActualF0DebugChanged =
        [this](bool show)
    {
      if (settingsManager)
      {
        settingsManager->setShowActualF0Debug(show);
        settingsManager->saveConfig();
      }
      pianoRoll.setShowActualF0Debug(show);
      pianoRoll.repaint();
    };
  }

  settingsOverlay->setBounds(getLocalBounds());
  settingsOverlay->setVisible(true);
  settingsOverlay->toFront(true);
  settingsOverlay->grabKeyboardFocus();
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray &files)
{
  for (const auto &file : files)
  {
    if (file.endsWithIgnoreCase(".htpx") || file.endsWithIgnoreCase(".wav") ||
        file.endsWithIgnoreCase(".mp3") ||
        file.endsWithIgnoreCase(".flac") || file.endsWithIgnoreCase(".aiff") ||
        file.endsWithIgnoreCase(".ogg") || file.endsWithIgnoreCase(".m4a"))
      return true;
  }
  return false;
}

void MainComponent::filesDropped(const juce::StringArray &files, int /*x*/,
                                 int /*y*/)
{
  if (files.isEmpty())
    return;

  juce::File audioFile(files[0]);
  if (!audioFile.existsAsFile())
    return;

  if (audioFile.hasFileExtension("htpx") || audioFile.hasFileExtension(".htpx"))
    openProjectFile(audioFile);
  else
    loadAudioFile(audioFile);
}

void MainComponent::setHostAudio(const juce::AudioBuffer<float> &buffer,
                                 double sampleRate,
                                 double timelineOffsetSeconds)
{
  if (!isPluginMode())
    return;

  if (!editorController)
    return;

  showAnalysisProgress(0.0);

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->setHostAudioAsync(
      buffer, sampleRate,
      [safeThis](double p, const juce::String &msg)
      {
        if (safeThis == nullptr)
          return;
        juce::MessageManager::callAsync([safeThis, p, msg]()
                                        {
          if (safeThis == nullptr)
            return;

          safeThis->toolbar.hideProgress();
          safeThis->showAnalysisProgress(p); });
      },
      [safeThis, timelineOffsetSeconds](const juce::AudioBuffer<float> &original)
      {
        if (safeThis == nullptr)
          return;

        if (safeThis->undoManager)
          safeThis->undoManager->clear();

        auto *project = safeThis->getProject();
        if (!project)
          return;

        project->getAudioData().timelineOffsetSeconds =
            std::max(0.0, timelineOffsetSeconds);

        safeThis->pianoRoll.setProject(project);
        safeThis->pianoRollView.setProject(project);
        safeThis->parameterPanel.setProject(project);
        safeThis->toolbar.setTotalTime(project->getAudioData().getDuration());
        safeThis->toolbar.setTransportEnabled(true);

        safeThis->fitAnalyzedPitchRangeToView(*project);

        auto *vocoder = safeThis->editorController
                            ? safeThis->editorController->getVocoder()
                            : nullptr;
        if (vocoder && !vocoder->isLoaded())
        {
          auto modelPath = PlatformPaths::getVocoderModelFile();
          if (modelPath.exists())
          {
            if (!vocoder->loadModel(modelPath))
            {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::AlertWindow::WarningIcon, "Inference failed",
                  "Failed to load vocoder model at:\n" +
                      modelPath.getFullPathName() +
                      "\n\nPlease check your model installation and try again.");
              safeThis->toolbar.hideProgress();
              return;
            }
          }
          else
          {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Missing model file",
                "The vocoder model was not found at:\n" +
                    modelPath.getFullPathName() +
                    "\n\nPlease install the required model files and try again.");
            safeThis->toolbar.hideProgress();
            return;
          }
        }

        safeThis->repaint();

        safeThis->notifyProjectDataChanged();

        safeThis->hideAnalysisProgress();
        safeThis->toolbar.hideProgress();
      });
}

void MainComponent::updateHostAudioTimelineOffset(double timelineOffsetSeconds)
{
  if (!isPluginMode())
    return;

  auto *project = getProject();
  if (!project)
    return;

  auto &audioData = project->getAudioData();
  if (audioData.waveform.getNumSamples() == 0 || audioData.sampleRate <= 0)
  {
    audioData.timelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
    return;
  }

  const double newOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
  const double oldOffsetSeconds = std::max(0.0, audioData.timelineOffsetSeconds);
  if (std::abs(newOffsetSeconds - oldOffsetSeconds) < 0.000001)
    return;

  const int sampleRate = audioData.sampleRate;
  const int oldOffsetSamples = std::max(
      0, static_cast<int>(std::llround(oldOffsetSeconds * sampleRate)));
  const int newOffsetSamples = std::max(
      0, static_cast<int>(std::llround(newOffsetSeconds * sampleRate)));
  const int oldOffsetFrames = std::max(
      0, static_cast<int>(std::llround(oldOffsetSeconds * sampleRate /
                                       static_cast<double>(HOP_SIZE))));
  const int newOffsetFrames = std::max(
      0, static_cast<int>(std::llround(newOffsetSeconds * sampleRate /
                                       static_cast<double>(HOP_SIZE))));
  const int frameDelta = newOffsetFrames - oldOffsetFrames;

  auto repadBuffer = [&](juce::AudioBuffer<float> &buffer)
  {
    const int oldSamples = buffer.getNumSamples();
    if (oldSamples <= 0)
      return;

    const int contentStart = std::min(oldOffsetSamples, oldSamples);
    const int contentSamples = oldSamples - contentStart;
    const int newSamples = newOffsetSamples + contentSamples;
    juce::AudioBuffer<float> shifted(buffer.getNumChannels(), newSamples);
    shifted.clear();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
      shifted.copyFrom(ch, newOffsetSamples, buffer, ch, contentStart,
                       contentSamples);
    buffer = std::move(shifted);
  };

  auto repadFloatVector = [&](std::vector<float> &values)
  {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<float> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)));
    std::copy(values.begin() + contentStart, values.end(),
              shifted.begin() + newOffsetFrames);
    values = std::move(shifted);
  };

  auto repadBoolVector = [&](std::vector<bool> &values)
  {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<bool> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)), false);
    for (int i = contentStart; i < oldSize; ++i)
      shifted[static_cast<size_t>(newOffsetFrames + i - contentStart)] =
          values[static_cast<size_t>(i)];
    values = std::move(shifted);
  };

  auto repadMel = [&](std::vector<std::vector<float>> &frames)
  {
    const int oldSize = static_cast<int>(frames.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    const size_t numMels = oldSize > 0 ? frames[static_cast<size_t>(
                                             std::min(contentStart, oldSize - 1))]
                                             .size()
                                       : 0;
    std::vector<std::vector<float>> shifted(
        static_cast<size_t>(newOffsetFrames + std::max(0, oldSize - contentStart)),
        std::vector<float>(numMels, 0.0f));
    std::move(frames.begin() + contentStart, frames.end(),
              shifted.begin() + newOffsetFrames);
    frames = std::move(shifted);
  };

  repadBuffer(audioData.waveform);
  repadBuffer(audioData.originalWaveform);
  repadMel(audioData.melSpectrogram);
  repadFloatVector(audioData.f0);
  repadFloatVector(audioData.baseF0);
  repadFloatVector(audioData.basePitch);
  repadFloatVector(audioData.deltaPitch);
  repadBoolVector(audioData.voicedMask);
  repadBoolVector(audioData.vadMask);

  auto shiftFrame = [frameDelta](int frame)
  {
    return std::max(0, frame + frameDelta);
  };

  for (auto &note : project->getNotes())
  {
    note.setStartFrame(shiftFrame(note.getStartFrame()));
    note.setEndFrame(shiftFrame(note.getEndFrame()));
    note.setSrcStartFrame(shiftFrame(note.getSrcStartFrame()));
    note.setSrcEndFrame(shiftFrame(note.getSrcEndFrame()));
  }

  for (auto &range : audioData.segmentChunkRanges)
  {
    range.first = shiftFrame(range.first);
    range.second = shiftFrame(range.second);
  }

  for (auto &chunk : audioData.segmentDebugChunks)
  {
    chunk.startFrame = shiftFrame(chunk.startFrame);
    chunk.endFrame = shiftFrame(chunk.endFrame);
    for (auto &event : chunk.events)
    {
      event.startFrame = shiftFrame(event.startFrame);
      event.endFrame = shiftFrame(event.endFrame);
      event.attachedStartFrame = shiftFrame(event.attachedStartFrame);
    }
  }

  audioData.timelineOffsetSeconds = newOffsetSeconds;
  toolbar.setTotalTime(audioData.getDuration());
  pianoRoll.setProject(project);
  pianoRollView.setProject(project);
  parameterPanel.setProject(project);
  repaint();
  notifyProjectDataChanged();
}

void MainComponent::clearHostAudio()
{
  if (!isPluginMode())
    return;

  clearProjectForNewLoad();
  hideAnalysisProgress();
  toolbar.hideProgress();
  notifyProjectDataChanged();
}

void MainComponent::updatePlaybackPosition(double timeSeconds)
{
  if (!isPluginMode())
    return;

  auto *project = getProject();
  double displayTime = std::max(0.0, timeSeconds);

  // Clamp only when project audio exists. Empty ARA tracks still need to follow
  // the host playhead position.
  if (project && project->getAudioData().waveform.getNumSamples() > 0)
  {
    double duration = project->getAudioData().getDuration();
    displayTime = std::min(displayTime, static_cast<double>(duration));
  }

  // Update cursor position using the same mechanism as AudioEngine
  pendingCursorTime.store(displayTime);
  hasPendingCursorUpdate.store(true);
}

void MainComponent::updateHostPlaybackState(bool hostIsPlaying)
{
  if (!isPluginMode())
    return;

  isPlaying = hostIsPlaying;
  toolbar.setPlaying(hostIsPlaying);
}

void MainComponent::updateHostLoopRange(double startSeconds, double endSeconds,
                                        bool enabled, bool hasRange)
{
  auto *project = getProject();
  if (!project)
    return;

  if (hasRange && endSeconds > startSeconds)
  {
    project->setLoopRange(startSeconds, endSeconds);
    project->setLoopEnabled(enabled);
  }
  else
  {
    if (hasRange)
      project->clearLoopRange();
    else
      project->setLoopEnabled(false);
  }

  toolbar.setLoopEnabled(enabled);
  pianoRoll.repaint();
}

bool MainComponent::hasAnalyzedProject() const
{
  if (auto *project = getProject())
  {
    auto &audioData = project->getAudioData();
    return audioData.waveform.getNumSamples() > 0 && !audioData.f0.empty();
  }
  return false;
}

void MainComponent::bindRealtimeProcessor(RealtimePitchProcessor &processor)
{
  processor.setProject(getProject());
  processor.setVocoder(editorController ? editorController->getVocoder()
                                        : nullptr);
}

juce::String MainComponent::serializeProjectJson() const
{
  if (auto *project = getProject())
  {
    auto json = ProjectSerializer::toJson(*project);
    return juce::JSON::toString(json, false);
  }
  return {};
}

bool MainComponent::restoreProjectJson(const juce::String &jsonString)
{
  if (jsonString.isEmpty())
    return false;
  if (auto *project = getProject())
  {
    auto json = juce::JSON::parse(jsonString);
    if (json.isObject())
    {
      ProjectSerializer::fromJson(*project, json);
      return true;
    }
  }
  return false;
}

void MainComponent::notifyHostStopped()
{
  if (!isPluginMode())
    return;

  updateHostPlaybackState(false);
}

void MainComponent::triggerResynthesis()
{
  if (!isPluginMode())
    return;

  // Triggered by DAW parameter automation (pitch offset, formant shift).
  // Follows the same flow as parameterPanel.onParameterEditFinished.
  resynthesizeIncremental();
  notifyProjectDataChanged();
  if (onPitchEditFinished)
    onPitchEditFinished();
}

bool MainComponent::isARAModeActive() const
{
  // Check if we're in plugin mode and have project data from ARA
  // ARA mode is indicated by having project data but no manual capture
  // In ARA mode, audio comes from ARA document controller, not from
  // processBlock
  if (!isPluginMode())
    return false;

  // If we have project data and it wasn't captured manually, it's likely from
  // ARA This is a heuristic - in a real implementation, we'd track this
  // explicitly
  if (auto *project = getProject();
      project && project->getAudioData().waveform.getNumSamples() > 0)
  {
    // Check if we're not currently capturing (which would indicate non-ARA
    // mode) Note: This requires access to PluginProcessor, which we don't have
    // here A better approach would be to set a flag when ARA audio is received
    return true; // Assume ARA if we have project data in plugin mode
  }

  return false;
}

void MainComponent::renderProcessedAudio()
{
  auto *project = getProject();
  if (!isPluginMode() || !project ||
      project->getAudioData().originalWaveform.getNumSamples() == 0)
    return;

  // Show progress
  toolbar.showProgress(TR("progress.rendering"));
  auto *vocoder = editorController ? editorController->getVocoder() : nullptr;
  if (!vocoder)
  {
    toolbar.hideProgress();
    return;
  }

  const float globalOffset = project->getGlobalPitchOffset();
  juce::Component::SafePointer<MainComponent> safeThis(this);

  if (editorController)
  {
    editorController->renderProcessedAudioAsync(
        *project, globalOffset,
        [safeThis](bool ok)
        {
          if (safeThis == nullptr)
            return;
          safeThis->toolbar.hideProgress();
          if (ok)
            safeThis->notifyProjectDataChanged();
        });
  }
}

// ApplicationCommandTarget interface implementations
juce::ApplicationCommandTarget *MainComponent::getNextCommandTarget()
{
  return nullptr;
}

void MainComponent::getAllCommands(juce::Array<juce::CommandID> &commands)
{
  // Register all application commands that MainComponent handles
  const juce::CommandID commandArray[] = {
      // File commands
      CommandIDs::openFile,
      CommandIDs::saveProject,
      CommandIDs::exportAudio,
      CommandIDs::exportMidi,
      CommandIDs::quit,

      // Edit commands
      CommandIDs::undo,
      CommandIDs::redo,
      CommandIDs::selectAll,

      // View commands
      CommandIDs::showSettings,
      CommandIDs::showDeltaPitch,
      CommandIDs::showBasePitch,

      // Transport commands
      CommandIDs::playPause,
      CommandIDs::stop,
      CommandIDs::goToStart,
      CommandIDs::goToEnd,

      // Edit mode commands
      CommandIDs::toggleDrawMode,
      CommandIDs::exitDrawMode};

  commands.addArray(commandArray, sizeof(commandArray) / sizeof(commandArray[0]));
}

void MainComponent::getCommandInfo(juce::CommandID commandID,
                                   juce::ApplicationCommandInfo &result)
{
  const auto primaryModifier =
#if JUCE_MAC
      juce::ModifierKeys::commandModifier;
#else
      juce::ModifierKeys::ctrlModifier;
#endif
  auto *project = getProject();
  switch (commandID)
  {
  // File commands
  case CommandIDs::openFile:
    result.setInfo(TR("command.open_audio"), TR("command.open_audio.desp"), "File", 0);
    result.addDefaultKeypress('o', primaryModifier);
    break;

  case CommandIDs::saveProject:
    result.setInfo(TR("command.save_project"), TR("command.save_project.desp"), "File", 0);
    result.addDefaultKeypress('s', primaryModifier);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::exportAudio:
    result.setInfo(TR("command.export_audio"), TR("command.export_audio.desp"), "File", 0);
    result.addDefaultKeypress('e', primaryModifier);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::exportMidi:
    result.setInfo(TR("command.export_midi"), TR("command.export_midi.desp"), "File", 0);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::quit:
    result.setInfo(TR("command.quit"), TR("command.quit.desp"), "File", 0);
    result.addDefaultKeypress('q', primaryModifier);
    result.setActive(!isPluginMode());
    break;

  // Edit commands
  case CommandIDs::undo:
    result.setInfo(TR("command.undo"), TR("command.undo.desp"), "Edit", 0);
    result.addDefaultKeypress('z', primaryModifier);
    result.setActive(isPluginMode() || (undoManager != nullptr && undoManager->canUndo()));
    break;

  case CommandIDs::redo:
    result.setInfo(TR("command.redo"), TR("command.redo.desp"), "Edit", 0);
#if JUCE_MAC
    result.addDefaultKeypress('z', primaryModifier | juce::ModifierKeys::shiftModifier);
#else
    result.addDefaultKeypress('y', primaryModifier);
#endif
    result.setActive(isPluginMode() || (undoManager != nullptr && undoManager->canRedo()));
    break;

  case CommandIDs::selectAll:
    result.setInfo(TR("command.select_all"), TR("command.select_all.desp"), "Edit", 0);
    result.addDefaultKeypress('a', primaryModifier);
    result.setActive(project != nullptr);
    break;

  // View commands
  case CommandIDs::showSettings:
    result.setInfo(TR("command.settings"), TR("command.settings.desp"), "View", 0);
    result.addDefaultKeypress(',', primaryModifier);
    break;

  case CommandIDs::showDeltaPitch:
    result.setInfo(TR("command.show_delta_pitch"), TR("command.show_delta_pitch.desp"), "View", 0);
    result.addDefaultKeypress('d', primaryModifier | juce::ModifierKeys::shiftModifier);
    result.setTicked(settingsManager->getShowDeltaPitch());
    break;

  case CommandIDs::showBasePitch:
    result.setInfo(TR("command.show_base_pitch"), TR("command.show_base_pitch.desp"), "View", 0);
    result.addDefaultKeypress('b', primaryModifier | juce::ModifierKeys::shiftModifier);
    result.setTicked(settingsManager->getShowBasePitch());
    break;

  // Transport commands
  case CommandIDs::playPause:
    result.setInfo(TR("command.play_pause"), TR("command.play_pause.desp"), "Transport", 0);
    if (!isPluginMode())
      result.addDefaultKeypress(juce::KeyPress::spaceKey, juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::stop:
    result.setInfo(TR("command.stop"), TR("command.stop.desp"), "Transport", 0);
    result.addDefaultKeypress(juce::KeyPress::escapeKey, juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr && isPlaying);
    break;

  case CommandIDs::goToStart:
    result.setInfo(TR("command.go_to_start"), TR("command.go_to_start.desp"), "Transport", 0);
    result.addDefaultKeypress(juce::KeyPress::homeKey, juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr);
    break;

  case CommandIDs::goToEnd:
    result.setInfo(TR("command.go_to_end"), TR("command.go_to_end.desp"), "Transport", 0);
    result.addDefaultKeypress(juce::KeyPress::endKey, juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr);
    break;

  // Edit mode commands
  case CommandIDs::toggleDrawMode:
    result.setInfo(TR("command.toggle_draw"), TR("command.toggle_draw.desp"), "Edit Mode", 0);
    result.addDefaultKeypress('d', juce::ModifierKeys::noModifiers);
    result.setActive(project != nullptr);
    result.setTicked(pianoRoll.getEditMode() == EditMode::Draw);
    break;

  case CommandIDs::exitDrawMode:
    result.setInfo(TR("command.exit_draw"), TR("command.exit_draw.desp"), "Edit Mode", 0);
    result.addDefaultKeypress(juce::KeyPress::escapeKey, juce::ModifierKeys::noModifiers);
    result.setActive(pianoRoll.getEditMode() == EditMode::Draw);
    break;

  default:
    break;
  }
}

bool MainComponent::perform(const ApplicationCommandTarget::InvocationInfo &info)
{
  switch (info.commandID)
  {
  // File commands
  case CommandIDs::openFile:
    this->openFile();
    return true;

  case CommandIDs::saveProject:
    this->saveProject();
    return true;

  case CommandIDs::exportAudio:
    exportFile();
    return true;

  case CommandIDs::exportMidi:
    exportMidiFile();
    return true;

  case CommandIDs::quit:
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
    return true;

  // Edit commands
  case CommandIDs::undo:
    if (isPluginMode())
    {
      if (undoManager != nullptr && undoManager->canUndo())
        this->undo();
      return true; // Consume in plugin mode to avoid host-level undo conflicts
    }
    this->undo();
    return true;

  case CommandIDs::redo:
    if (isPluginMode())
    {
      if (undoManager != nullptr && undoManager->canRedo())
        this->redo();
      return true; // Consume in plugin mode to avoid host-level redo conflicts
    }
    this->redo();
    return true;

  case CommandIDs::selectAll:
    if (auto *project = getProject())
    {
      project->selectAllNotes();
      pianoRoll.repaint();
    }
    return true;

  // View commands
  case CommandIDs::showSettings:
    showSettings();
    return true;

  case CommandIDs::showDeltaPitch:
  {
    bool newState = !settingsManager->getShowDeltaPitch();
    pianoRoll.setShowDeltaPitch(newState);
    settingsManager->setShowDeltaPitch(newState);
    settingsManager->saveConfig();
    commandManager->commandStatusChanged();
    return true;
  }

  case CommandIDs::showBasePitch:
  {
    bool newState = !settingsManager->getShowBasePitch();
    pianoRoll.setShowBasePitch(newState);
    settingsManager->setShowBasePitch(newState);
    settingsManager->saveConfig();
    commandManager->commandStatusChanged();
    return true;
  }

  // Transport commands
  case CommandIDs::playPause:
    if (isPlaying)
      pause();
    else
      play();
    return true;

  case CommandIDs::stop:
    this->stop();
    return true;

  case CommandIDs::goToStart:
    if (auto *project = getProject())
      seek(std::max(0.0, project->getAudioData().timelineOffsetSeconds));
    return true;

  case CommandIDs::goToEnd:
    if (auto *project = getProject())
    {
      seek(project->getAudioData().getDuration());
    }
    return true;

  // Edit mode commands
  case CommandIDs::toggleDrawMode:
    if (pianoRoll.getEditMode() == EditMode::Draw)
      setEditMode(EditMode::Select);
    else
      setEditMode(EditMode::Draw);
    return true;

  case CommandIDs::exitDrawMode:
    if (pianoRoll.getEditMode() == EditMode::Draw)
    {
      setEditMode(EditMode::Select);
    }
    return true;

  default:
    return false;
  }
}
