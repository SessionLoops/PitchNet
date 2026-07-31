#pragma once

#include "../Audio/EditorController.h"
#include "../Audio/ResampledLoopAudition.h"
#include "IMainView.h"
#include "../Audio/IO/AudioFileManager.h"
#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Undo/UndoActions.h"
#include "CustomMenuBarLookAndFeel.h"
#include "CustomTitleBar.h"
#include "Commands.h"
#include "Components/AppFont.h"
#include "Main/MenuHandler.h"
#include "Main/SettingsManager.h"
#include "ParameterPanel.h"
#include "PianoRollComponent.h"
#include "PianoRollWorkspaceView.h"
#include "SettingsOverlay.h"
#include "ToolbarComponent.h"
#include "Workspace/WorkspaceComponent.h"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <vector>

class AnalysisBackdrop : public juce::Component {
public:
  AnalysisBackdrop() {
    setInterceptsMouseClicks(true, true);
  }

  void paint(juce::Graphics &g) override {
    g.fillAll(juce::Colours::black.withAlpha(0.40f));
  }
};

class AnalysisProgressPopup : public juce::Component {
public:
  void setProgress(double newProgress) {
    progress = juce::jlimit(0.0, 1.0, newProgress);
    repaint();
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF333333u));
    g.fillRoundedRectangle(bounds, 7.0f);

    g.setColour(juce::Colour(0xFFEFEFEFu));
    g.setFont(AppFont::getFont(16.0f));
    g.drawText(title, getLocalBounds().withHeight(44).translated(0, 4),
               juce::Justification::centred, false);

    auto bar = juce::Rectangle<float>(29.0f, 53.0f, bounds.getWidth() - 58.0f, 20.0f);
    const auto barRadius = bar.getHeight() * 0.5f;
    g.setColour(juce::Colour(0xFF191717u));
    g.fillRoundedRectangle(bar, barRadius);

    auto fillBounds = bar.reduced(2.0f);
    auto fill = fillBounds.withWidth(fillBounds.getWidth() * static_cast<float>(progress));
    if (fill.getWidth() > 0.0f) {
      g.setColour(juce::Colour(0xFF9A9A9Au));
      g.fillRoundedRectangle(fill, fillBounds.getHeight() * 0.5f);
    }

    g.setColour(juce::Colour(0xFFEFEFEFu));
    g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
    g.drawText(juce::String(static_cast<int>(std::round(progress * 100.0))) + "%",
               bar.toNearestInt(), juce::Justification::centred, false);
  }

private:
  double progress = 0.0;
  static constexpr const char *title = "Analyzing Audio...";
};

class MainComponent : public juce::Component,
                      public juce::Timer,
                      public juce::KeyListener,
                      public juce::FileDragAndDropTarget,
                      public juce::ApplicationCommandTarget,
                      public IMainView {
public:
  using juce::Component::keyPressed;
  explicit MainComponent(bool enableAudioDevice = true);
  ~MainComponent() override;

  void paint(juce::Graphics &g) override;
  void paintOverChildren(juce::Graphics &g) override;
  void resized() override;

  void timerCallback() override;

  // KeyListener
  bool keyPressed(const juce::KeyPress &key,
                  juce::Component *originatingComponent) override;

  // ApplicationCommandTarget interface
  juce::ApplicationCommandTarget* getNextCommandTarget() override;
  void getAllCommands(juce::Array<juce::CommandID>& commands) override;
  void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
  bool perform(const ApplicationCommandTarget::InvocationInfo& info) override;

  // Mouse handling for window dragging on macOS
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;

  // FileDragAndDropTarget
  bool isInterestedInFileDrag(const juce::StringArray &files) override;
  void filesDropped(const juce::StringArray &files, int x, int y) override;

  // Plugin mode
  bool isPluginMode() const { return !enableAudioDeviceFlag; }
  juce::Component *getComponent() override { return this; }
  Project *getProject() const override {
    return editorController ? editorController->getProject() : nullptr;
  }
  std::unique_ptr<Project>
  exchangeProject(std::unique_ptr<Project> project) override;
  Vocoder *getVocoder() const override {
    return editorController ? editorController->getVocoder() : nullptr;
  }
  void bindBackendController(EditorController *controller) override;
  void bindUndoManager(PitchUndoManager *manager) override;
  MainViewViewportState getViewportState() const override;
  void restoreViewportState(const MainViewViewportState &state) override;
  bool hasAnalyzedProject() const override;
  void bindRealtimeProcessor(RealtimePitchProcessor &processor) override;
  juce::String serializeProjectJson() const override;
  bool restoreProjectJson(const juce::String &json) override;
  bool restoreProjectSnapshot(const Project &project) override;
  void setStatusMessage(const juce::String &message) override {
    toolbar.setStatusMessage(message);
  }
  void showAnalysisProgress(double progress) override;
  void hideAnalysisProgress() override;
  void finishBackendRender(bool success) override;
  void setOnProjectDataChanged(std::function<void()> callback) override {
    onProjectDataChanged = std::move(callback);
  }
  void setOnPitchEditFinished(std::function<void()> callback) override {
    onPitchEditFinished = std::move(callback);
  }
  void setOnRequestBackendRender(
      std::function<void(const Project &)> callback) override {
    onRequestBackendRender = std::move(callback);
  }
  void setOnRequestBackendPreview(
      std::function<void(const Project &, int, int)> callback) override {
    onRequestBackendPreview = std::move(callback);
  }
  void setOnStopBackendPreview(std::function<void()> callback) override {
    onStopBackendPreview = std::move(callback);
  }
  void setOnRequestDragAudition(
      std::function<void(const juce::AudioBuffer<float> &, double)> callback) override {
    onRequestDragAudition = std::move(callback);
  }
  void setOnStopDragAudition(std::function<void()> callback) override {
    onStopDragAudition = std::move(callback);
  }
  void setOnRequestHostPlayState(
      std::function<void(bool)> callback) override {
    onRequestHostPlayState = std::move(callback);
  }
  void setOnRequestHostStop(std::function<void()> callback) override {
    onRequestHostStop = std::move(callback);
  }
  void setOnRequestHostSeek(
      std::function<void(double)> callback) override {
    onRequestHostSeek = std::move(callback);
  }
  void setOnRequestHostLoopRange(
      std::function<void(double, double, bool, bool)> callback) override {
    onRequestHostLoopRange = std::move(callback);
  }
  void setOnRecordArmChanged(std::function<void(bool)> callback) override {
    onRecordArmChanged = std::move(callback);
  }
  void setRecordControlVisible(bool visible) override {
    toolbar.setRecordControlVisible(visible);
  }
  void updateRecordArmState(bool armed) override {
    toolbar.setRecordArmed(armed);
  }
  void beginLiveRecording(double sampleRate,
                          double timelineOffsetSeconds) override;
  void appendLiveRecordingAudio(
      const juce::AudioBuffer<float> &buffer) override;
  juce::Point<int> getSavedWindowSize() const;

  // Check if ARA mode is active.
  bool isARAModeActive() const;

  // Plugin mode - host audio handling
  void setHostAudio(const juce::AudioBuffer<float> &buffer,
                    double sampleRate,
                    double timelineOffsetSeconds = 0.0) override;
  void updateHostAudioTimelineOffset(double timelineOffsetSeconds) override;
  void clearHostAudio() override;
  void focusTimelineRange(double startSeconds, double endSeconds) override;
  void renderProcessedAudio();

  // Plugin mode callbacks
  std::function<void()>
      onProjectDataChanged; // Called when project data is ready or changed
  std::function<void()>
      onPitchEditFinished; // Called when pitch editing is finished
                           // (Melodyne-style: triggers real-time update)
  std::function<void(const Project &)> onRequestBackendRender;
  std::function<void(const Project &, int, int)> onRequestBackendPreview;
  std::function<void()> onStopBackendPreview;
  std::function<void(const juce::AudioBuffer<float> &, double)> onRequestDragAudition;
  std::function<void()> onStopDragAudition;

  // Plugin mode - request host transport control (optional; only works if host
  // supports it)
  std::function<void(bool shouldPlay)> onRequestHostPlayState;
  std::function<void()> onRequestHostStop;
  std::function<void(double timeInSeconds)> onRequestHostSeek;
  std::function<void(double startSeconds, double endSeconds, bool enabled,
                     bool hasRange)>
      onRequestHostLoopRange;
  std::function<void(bool armed)> onRecordArmChanged;

  // Plugin mode - update playback position from host
  void updatePlaybackPosition(double timeSeconds) override;
  void updateHostPlaybackState(bool isPlaying) override;
  void updateHostTimelineState(double bpm, int numerator,
                               int denominator) override;
  void updateHostLoopRange(double startSeconds, double endSeconds,
                           bool enabled, bool hasRange) override;
  void notifyHostStopped() override; // Called when host stops playback
  void triggerResynthesis() override; // Triggered by DAW parameter automation

private:
  void openFile();
  void exportFile();
  void exportMidiFile();
  void play();
  void pause();
  void stop();
  void seek(double time);
  void previewNoteRegion(int startFrame, int endFrame);
  void finishPreviewRegion(bool restorePosition);
  void auditionDraggedNote(const Note &note);
  void finishDraggedNoteAudition();
  void dispatchPendingDragAudition();
  void auditionPianoKey(int midiNote);
  void finishPianoKeyAudition();
  void jumpTransport(bool forward);
  void resynthesizeIncremental(); // Incremental synthesis on edit
  void showSettings();
  void openRecentFile(const juce::File &file);
  void addRecentFile(const juce::File &file);
  void refreshRecentFilesMenu();

  void onNoteSelected(Note *note);
  void onPitchEdited();
  void onZoomChanged(float pixelsPerSecond);
  void reinterpolateUV(int startFrame,
                       int endFrame); // Re-infer UV regions using FCPE
  void notifyProjectDataChanged();
  void fitAnalyzedPitchRangeToView(Project &project);

  void reloadInferenceModels(bool async = false);
  bool isInferenceBusy() const;
  void applyCachedHostLoopRange();
  void checkForUpdatesOnLaunch();
  void showUpdateAvailablePopup(const juce::String &latestVersion,
                                const juce::String &releaseNotes);
  void skipUpdateVersion(const juce::String &version);

  void loadAudioFile(const juce::File &file);
  void clearProjectForNewLoad();
  void openProjectFile(const juce::File &file);
  void analyzeAudio();
  void analyzeAudio(
      Project &targetProject,
      const std::function<void(double, const juce::String &)> &onProgress,
      std::function<void()> onComplete = nullptr);
  void segmentIntoNotes();
  void segmentIntoNotes(Project &targetProject);

  void saveProject(bool saveAs = false);

  void undo();
  void redo();
  void setEditMode(EditMode mode);

  std::unique_ptr<EditorController> ownedEditorController;
  EditorController *editorController = nullptr;
  std::unique_ptr<PitchUndoManager> ownedUndoManager;
  PitchUndoManager *undoManager = nullptr;
  std::unique_ptr<juce::ApplicationCommandManager> commandManager;
  bool previewRegionActive = false;
  double previewRegionEndTime = 0.0;
  double previewRegionReturnTime = 0.0;
  double previewRegionStartTime = 0.0;
  double previewRegionWallClockStartMs = 0.0;
  bool previewRegionWasProjectPlaying = false;
  std::unique_ptr<ResampledLoopAudition> resampledLoopAudition;
  bool dragAuditionEnabled = false;
  bool noteDragAuditionActive = false;
  bool noteDragAuditionBackendPreviewStarted = false;
  bool noteDragAuditionPending = false;
  bool noteDragAuditionWasPlaying = false;
  double noteDragAuditionReturnTime = 0.0;
  int noteDragAuditionStartFrame = 0;
  int noteDragAuditionEndFrame = 0;
  juce::int64 noteDragAuditionLastInputMs = 0;
  float pendingNoteDragAuditionShift = 0.0f;
  float pendingNoteDragAuditionMidiNote = 60.0f;
  std::vector<float> pendingNoteDragAuditionSource;
  static constexpr juce::int64 noteDragAuditionDebounceMs = 20;

  // New modular components
  std::unique_ptr<AudioFileManager> fileManager;
  std::unique_ptr<MenuHandler> menuHandler;
  std::unique_ptr<SettingsManager> settingsManager;

  const bool enableAudioDeviceFlag;

  CustomMenuBarLookAndFeel menuBarLookAndFeel;
  juce::MenuBarComponent menuBar;
#if JUCE_MAC
  juce::PopupMenu macExtraAppleMenuItems;
#endif
  ToolbarComponent toolbar;
  WorkspaceComponent workspace;
  PianoRollComponent pianoRoll;
  PianoRollWorkspaceView pianoRollView;
  ParameterPanel parameterPanel;
  AnalysisBackdrop analysisBackdrop;
  AnalysisProgressPopup analysisProgressPopup;

  // A desktop tooltip window lets tooltip clients work in both standalone and
  // plug-in hosts, whose editor component is owned externally.
  std::unique_ptr<juce::TooltipWindow> tooltipWindow;

  std::unique_ptr<SettingsOverlay> settingsOverlay;

  std::unique_ptr<juce::FileChooser> fileChooser;
  juce::StringArray recentFiles;

  bool isPlaying = false;
  bool liveRecordingActive = false;
  bool pianoKeyAuditionActive = false;
  bool pianoKeyAuditionWasPlaying = false;
  double pianoKeyAuditionReturnTime = 0.0;

  // Sync flag to prevent infinite loops
  bool isSyncingZoom = false;

  // Async load state
  std::atomic<bool> isLoadingAudio{false};
  std::atomic<double> loadingProgress{0.0};
  juce::CriticalSection loadingMessageLock;
  juce::String loadingMessage;
  juce::String lastLoadingMessage;

  // Async render state (plugin mode) is managed by EditorController
  // Incremental synthesis coalescing
  std::atomic<bool> pendingIncrementalResynth{false};

  // Cursor update throttling
  std::atomic<double> pendingCursorTime{0.0};
  std::atomic<bool> hasPendingCursorUpdate{false};

  bool hasCachedHostLoopRange = false;
  double cachedHostLoopStartSeconds = 0.0;
  double cachedHostLoopEndSeconds = 0.0;
  bool cachedHostLoopEnabled = false;
  bool cachedHostLoopHasRange = false;
  double suppressPlaybackFollowUntilMs = 0.0;
  juce::int64 lastCursorUpdateTime = 0;

#if JUCE_MAC
  juce::ComponentDragger dragger;
#endif

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
