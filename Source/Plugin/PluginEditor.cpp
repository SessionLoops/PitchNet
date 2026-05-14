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

  addAndMakeVisible(*mainView->getComponent());
  audioProcessor.setMainComponent(mainView.get());

#if JucePlugin_Enable_ARA
  setupARAMode();
#else
  setupNonARAMode();
#endif

  setupCallbacks();

  auto size = getDefaultMainViewSize(this);
  setSize(size.x, size.y);
  setResizable(true, true);

  // Grab keyboard focus on mainComponent when editor is shown
  mainView->getComponent()->grabKeyboardFocus();
}

PitchNetAudioProcessorEditor::~PitchNetAudioProcessorEditor() {
  audioProcessor.setMainComponent(nullptr);
  shutdownUiResources();
}

void PitchNetAudioProcessorEditor::setupARAMode() {
#if JucePlugin_Enable_ARA
  mainView->setARAMode(true);

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

  // Setup re-analyze callback
  mainView->setOnReanalyzeRequested([pitchDocController]() {
    pitchDocController->reanalyze();
  });

  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    audioProcessor.requestHostSeek(timeInSeconds);
  });

  // Check for existing audio sources
  auto *juceDocument = docController->getDocument();
  if (juceDocument) {
    auto &audioSources = juceDocument->getAudioSources<juce::ARAAudioSource>();

    if (!audioSources.empty()) {
      // Process first audio source
      auto *source = audioSources.front();
      if (source && source->getSampleCount() > 0) {
        juce::ARAAudioSourceReader reader(source);
        int numSamples = static_cast<int>(source->getSampleCount());
        int numChannels = source->getChannelCount();
        double sampleRate = source->getSampleRate();

        juce::AudioBuffer<float> buffer(numChannels, numSamples);
        if (reader.read(&buffer, 0, numSamples, 0, true, true)) {
          mainView->setHostAudio(buffer, sampleRate);
          return;
        }
      }
    }
  }
#endif
}

void PitchNetAudioProcessorEditor::setupNonARAMode() {
  mainView->setARAMode(false);

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
}

void PitchNetAudioProcessorEditor::visibilityChanged() {
  if (isVisible())
    mainView->getComponent()->grabKeyboardFocus();
}

void PitchNetAudioProcessorEditor::mouseDown(const juce::MouseEvent& e) {
  juce::ignoreUnused(e);
  mainView->getComponent()->grabKeyboardFocus();
}
