#include "PluginEditor.h"
#include "HostCompatibility.h"

#if JucePlugin_Enable_ARA
#include "ARADocumentController.h"
#endif

PitchNetAudioProcessorEditor::PitchNetAudioProcessorEditor(
    PitchNetAudioProcessor &p)
    : AudioProcessorEditor(&p), audioProcessor(p),
      mainView(createMainView(false))
#if JucePlugin_Enable_ARA
      ,
      AudioProcessorEditorARAExtension(&p)
#endif
{
  // Initialize UI resources
  initializeUiResources();

  // Enable keyboard focus for the editor
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);

  addAndMakeVisible(*mainView->getComponent());
  mainView->getComponent()->setWantsKeyboardFocus(true);
  mainView->getComponent()->setMouseClickGrabsKeyboardFocus(true);
  audioProcessor.setMainComponent(mainView.get());
  addMouseListener(this, true);

#if JucePlugin_Enable_ARA
  setupARAMode();
#else
  setupNonARAMode();
#endif

  setupCallbacks();

  auto size = getDefaultMainViewSize(this);
  setSize(size.x, size.y);
  setResizable(true, true);

  // Keyboard focus is requested once the host has actually shown the editor.
  requestMainViewKeyboardFocusAsync();
}

PitchNetAudioProcessorEditor::~PitchNetAudioProcessorEditor() {
  removeMouseListener(this);
  audioProcessor.getTransportController().clearCallbacks();
  audioProcessor.setMainComponent(nullptr);
  shutdownUiResources();
}

void PitchNetAudioProcessorEditor::setupARAMode() {
#if JucePlugin_Enable_ARA
  auto *editorView = getARAEditorView();
  if (!editorView) {
    setupNonARAMode();
    return;
  }

  auto *docController = editorView->getDocumentController();
  if (!docController) {
    setupNonARAMode();
    return;
  }

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);

  if (!pitchDocController) {
    setupNonARAMode();
    return;
  }

  // Connect ARA controller to UI
  pitchDocController->setMainComponent(mainView.get());
  pitchDocController->setRealtimeProcessor(
      &audioProcessor.getRealtimeProcessor());

  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    if (auto *editorView = getARAEditorView()) {
      if (auto *docController = editorView->getDocumentController()) {
        if (auto *playbackController =
                docController->getHostPlaybackController()) {
          playbackController->requestSetPlaybackPosition(timeInSeconds);
          return;
        }
      }
    }

    audioProcessor.requestHostSeek(timeInSeconds);
  });

  setupHostTransportUiSync(true);

  // Check for existing audio sources
  auto *juceDocument = docController->getDocument();
  if (pitchDocController->processExistingAudioSources(
          static_cast<juce::ARADocument *>(juceDocument)))
    return;
#endif
}

void PitchNetAudioProcessorEditor::setupNonARAMode() {
  // Setup host transport control callbacks for non-ARA mode
  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    audioProcessor.requestHostSeek(timeInSeconds);
  });

  setupHostTransportUiSync(true);
}

void PitchNetAudioProcessorEditor::setupHostTransportUiSync(
    bool includePlayState) {
  auto safeMain =
      juce::Component::SafePointer<juce::Component>(mainView->getComponent());

  if (includePlayState) {
    audioProcessor.getTransportController().setPlayStateCallback(
        [safeMain](bool isPlaying) {
          auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
          if (!view)
            return;

          view->updateHostPlaybackState(isPlaying);
        });
  } else {
    audioProcessor.getTransportController().setPlayStateCallback(nullptr);
  }

  audioProcessor.getTransportController().setPositionCallback(
      [safeMain](double timeInSeconds) {
        auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
        if (!view)
          return;

        view->updatePlaybackPosition(timeInSeconds);
      });

  audioProcessor.getTransportController().setLoopCallback(
      [safeMain](const HostSyncService::LoopInfo &loop) {
        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->updateHostLoopRange(loop.loopStartSeconds, loop.loopEndSeconds,
                                    loop.isLoopEnabled, loop.hasLoopPoints);
      });
}

void PitchNetAudioProcessorEditor::setupCallbacks() {
  // When project data changes (analysis complete or synthesis complete)
  mainView->setOnProjectDataChanged([this]() {
    mainView->bindRealtimeProcessor(audioProcessor.getRealtimeProcessor());
    audioProcessor.getRealtimeProcessor().invalidate();
  });

  // onPitchEditFinished is handled by onProjectDataChanged (called after async
  // synthesis completes) No need for separate callback here
}

void PitchNetAudioProcessorEditor::paint(juce::Graphics &) {
  // MainComponent handles all painting
}

void PitchNetAudioProcessorEditor::resized() {
  mainView->getComponent()->setBounds(getLocalBounds());
  requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocus() {
  auto *component = mainView ? mainView->getComponent() : nullptr;
  if (!component || (!component->isShowing() && !component->isOnDesktop()))
    return;

  if (auto *focused = juce::Component::getCurrentlyFocusedComponent()) {
    if (focused == this || isParentOf(focused) || focused == component ||
        component->isParentOf(focused))
      return;
  }

  if (auto *peer = getPeer())
    peer->grabFocus();

  grabKeyboardFocus();
  component->grabKeyboardFocus();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocusAsync() {
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safeThis(this);
  juce::MessageManager::callAsync([safeThis]() {
    if (safeThis != nullptr)
      safeThis->requestMainViewKeyboardFocus();
  });
}

void PitchNetAudioProcessorEditor::visibilityChanged() {
  if (isVisible())
    requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::mouseDown(const juce::MouseEvent& e) {
  juce::ignoreUnused(e);
  requestMainViewKeyboardFocusAsync();
}
