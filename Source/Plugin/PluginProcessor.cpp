#include "PluginProcessor.h"
#include "../Utils/AudioResampler.h"
#include "../Audio/EditorController.h"
#include "../Undo/PitchUndoManager.h"
#include "../Models/ProjectModificationRanges.h"
#include "../Models/ProjectSerializer.h"
#include "../UI/IMainView.h"
#include "../Utils/Localization.h"
#include "../Utils/Constants.h"
#include "../Utils/MelSpectrogram.h"
#include "../Utils/OnnxRuntime.h"
#include "../Utils/OnnxRuntimeLoader.h"
#include "ARADocumentController.h"
#include "AraDiagnostics.h"
#include "PitchNetAudioModification.h"
#include "PluginEditor.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
// The project is stored in modification time and spans the whole modification.
// A region move must therefore change only which span is highlighted - never
// timelineOffsetSeconds, which would pad the synthesised waveform with leading
// silence and shift every published sample.
void stampSpanInModificationTime(Project &project,
                                 juce::int64 startSampleInModification,
                                 double lengthSeconds,
                                 double modificationRate) {
  auto &audioData = project.getAudioData();
  audioData.timelineOffsetSeconds = 0.0;
  if (modificationRate <= 0.0 || lengthSeconds <= 0.0) {
    audioData.playbackRegionRanges.clear();
    return;
  }

  const double start =
      static_cast<double>(startSampleInModification) / modificationRate;

  // Clamp to the audio that actually exists. The span marks which part of the
  // modification this region covers; it must never claim content past the end
  // of the take, because consumers size the canvas and the render from it. A
  // span reaching beyond the audio grows the rendered waveform by the excess,
  // which then publishes as leading silence and compounds on every edit.
  const double duration = audioData.getDuration();
  double end = start + lengthSeconds;
  if (duration > 0.0)
    end = std::min(end, duration);
  if (end <= start) {
    audioData.playbackRegionRanges.clear();
    return;
  }
  audioData.playbackRegionRanges = {{start, end}};
}

#if JucePlugin_Enable_ARA
void stampRegionSpanInModificationTime(Project &project,
                                       juce::ARAPlaybackRegion *region) {
  auto *modification = region != nullptr ? region->getAudioModification()
                                         : nullptr;
  auto *source = modification != nullptr ? modification->getAudioSource()
                                         : nullptr;
  stampSpanInModificationTime(
      project,
      region != nullptr ? region->getStartInAudioModificationSamples() : 0,
      region != nullptr ? std::max(0.0, region->getEndInPlaybackTime() -
                                            region->getStartInPlaybackTime())
                        : 0.0,
      source != nullptr ? source->getSampleRate() : 0.0);
}
#endif // JucePlugin_Enable_ARA
} // namespace

namespace {
constexpr std::uint32_t kPluginStateMagic = 0x504E5053u; // PNPS
constexpr int kPluginStateBinaryVersion = 1;

bool writeStateString(juce::OutputStream &out, const juce::String &text) {
  const auto bytes = text.getNumBytesAsUTF8();
  return out.writeInt64(static_cast<juce::int64>(bytes)) &&
         out.write(text.toRawUTF8(), bytes);
}

juce::String readStateString(juce::InputStream &in) {
  const auto bytes = in.readInt64();
  if (bytes < 0 || bytes > std::numeric_limits<int>::max())
    return {};

  juce::MemoryBlock data(static_cast<size_t>(bytes));
  if (bytes > 0 && in.read(data.getData(), static_cast<int>(bytes)) != bytes)
    return {};

  return juce::String(
      juce::CharPointer_UTF8(static_cast<const char *>(data.getData())),
      static_cast<size_t>(bytes));
}

#if JucePlugin_Enable_ARA
// Bring synthesized output back to the persistent region Project without
// replacing the Project, its note vector, or the F0 vectors referenced by undo
// actions. Analysis/edit data remains authoritative in the persistent object.
void mergeRenderedState(Project &target, const Project &rendered) {
  auto &targetAudio = target.getAudioData();
  const auto &renderedAudio = rendered.getAudioData();
  targetAudio.waveform.makeCopyOf(renderedAudio.waveform);
  targetAudio.timelineOffsetSeconds = renderedAudio.timelineOffsetSeconds;
  targetAudio.playbackRegionRanges = renderedAudio.playbackRegionRanges;

  auto &targetNotes = target.getNotes();
  const auto &renderedNotes = rendered.getNotes();
  if (targetNotes.size() == renderedNotes.size()) {
    for (size_t i = 0; i < targetNotes.size(); ++i) {
      const auto &renderedNote = renderedNotes[i];
      targetNotes[i].setRenderedEdit(renderedNote.hasRenderedEdit());
      targetNotes[i].setSynthDirty(renderedNote.isSynthDirty());
    }
  }
}

// Some AAX hosts deactivate an effect as soon as a rendered block contains an
// invalid or out-of-range sample. Keep this as the final ARA handoff so it
// covers both normal preview and temporary drag-audition output.
void sanitiseARAOutput(juce::AudioBuffer<float> &buffer) noexcept {
  for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
    auto *samples = buffer.getWritePointer(channel);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
      const float value = samples[sample];
      samples[sample] = std::isfinite(value)
                            ? juce::jlimit(-1.0f, 1.0f, value)
                            : 0.0f;
    }
  }
}

bool projectAppearsToCoverRegion(const Project &project, double regionStart,
                                 double regionEnd) {
  const auto &audioData = project.getAudioData();
  if (audioData.f0.empty())
    return false;

  constexpr double epsilon = 1.0e-3;
  if (std::abs(audioData.timelineOffsetSeconds - regionStart) <= epsilon)
    return true;

  for (const auto &[start, end] : audioData.playbackRegionRanges)
    if (std::abs(start - regionStart) <= epsilon &&
        std::abs(end - regionEnd) <= epsilon)
      return true;

  return false;
}

bool projectHasRestorableAnalysisData(const Project &project) {
  const auto &audioData = project.getAudioData();
  // Host-backed ARA region archives contain the analysis/edit shell but omit
  // project-level audio and mel buffers. Those are rebuilt from the ARA source
  // plus the processed-region render before the project becomes editable.
  if (audioData.f0.empty())
    return false;

  const int f0Size = static_cast<int>(audioData.f0.size());
  for (const auto &note : project.getNotes()) {
    if (note.getStartFrame() < 0 || note.getEndFrame() > f0Size)
      return false;
  }

  return true;
}

// archivedRegionKeyForLiveKey() lived here to bridge the two key families.
// There is one family now - the modification's live key - so every caller that
// tried the live slot and then the archived one collapses to a single lookup.

bool clearProcessedRegionAudio(PitchNetAudioModification *modification) {
  if (modification == nullptr)
    return false;

  const bool cleared = modification->hasProcessedAudio();
  modification->clearProcessedAudio();
  return cleared;
}
#endif
} // namespace

// ============================================================================
// Parameter Layout
// ============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout
PitchNetAudioProcessor::createParameterLayout() {
  std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

  // Bypass — standard host bypass
  params.push_back(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{PARAM_BYPASS, 1}, "Bypass", false));

  // Output Gain — post-processing volume in dB
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{PARAM_OUTPUT_GAIN, 1}, "Output Gain",
      juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f));

  // Dry/Wet — blend between original and processed (0-100%)
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{PARAM_DRY_WET, 1}, "Dry/Wet",
      juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));

  // Global Pitch Offset — semitone shift applied to entire project
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{PARAM_PITCH_OFFSET, 1}, "Pitch Offset",
      juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f));

  // Formant Shift — formant preservation shift in semitones
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{PARAM_FORMANT_SHIFT, 1}, "Formant Shift",
      juce::NormalisableRange<float>(-12.0f, 12.0f, 0.01f), 0.0f));

  return {params.begin(), params.end()};
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

PitchNetAudioProcessor::PitchNetAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
#else
    :
#endif
      apvts(*this, nullptr, "PitchNetParameters", createParameterLayout()) {
  // The standalone app does this at startup; a plug-in instance has no such
  // entry point, so the saved (or system) language is applied here instead.
  Localization::loadFromSettings();

  OnnxRuntimeLoader::ensureLoadedFromLocalDirectory();

  juce::String onnxRuntimeError;
  if (!OnnxRuntime::initialise(&onnxRuntimeError))
  {
    DBG("PitchNet: failed to initialise ONNX Runtime: " + onnxRuntimeError);
    jassertfalse;
  }

  // Cache raw parameter pointers for lock-free audio-thread access
  bypassParamValue = apvts.getRawParameterValue(PARAM_BYPASS);
  outputGainParamValue = apvts.getRawParameterValue(PARAM_OUTPUT_GAIN);
  dryWetParamValue = apvts.getRawParameterValue(PARAM_DRY_WET);
  pitchOffsetParamValue = apvts.getRawParameterValue(PARAM_PITCH_OFFSET);
  formantShiftParamValue = apvts.getRawParameterValue(PARAM_FORMANT_SHIFT);
  araAnalysisController = std::make_unique<EditorController>(false);
  undoManager = std::make_unique<PitchUndoManager>(100);
}

// Destructor is defined at the bottom of this file, after ARADocumentController.h
// is included, so the ARA build can detach the document-controller binding.

// ============================================================================
// AudioProcessor Info
// ============================================================================

const juce::String PitchNetAudioProcessor::getName() const {
  return JucePlugin_Name;
}

bool PitchNetAudioProcessor::acceptsMidi() const {
#if JucePlugin_WantsMidiInput
  return true;
#else
  return false;
#endif
}

bool PitchNetAudioProcessor::producesMidi() const {
#if JucePlugin_ProducesMidiOutput
  return true;
#else
  return false;
#endif
}

bool PitchNetAudioProcessor::isMidiEffect() const {
#if JucePlugin_IsMidiEffect
  return true;
#else
  return false;
#endif
}

#if JucePlugin_Enable_ARA
juce::AudioProcessorARAExtension *
PitchNetAudioProcessor::getARAClientExtensions() {
  return this;
}
#endif

// ============================================================================
// Prepare / Release
// ============================================================================

void PitchNetAudioProcessor::prepareToPlay(double sampleRate,
                                            int samplesPerBlock) {
  hostSampleRate = sampleRate;
  realtimeProcessor.prepareToPlay(sampleRate, samplesPerBlock);

  // Report zero latency — PitchNet uses pre-computed audio buffers,
  // so output at time T corresponds to input at time T (no analysis delay).
  setLatencySamples(0);

#if JucePlugin_Enable_ARA
  prepareToPlayForARA(sampleRate, samplesPerBlock,
                      getMainBusNumOutputChannels(), getProcessingPrecision());

  // Rebuild the headless playback buffer now that the host sample rate is known
  // and the ARA renderers exist. This makes UI-closed playback ready at the
  // correct rate (fixing the buzz that came from falling back to the raw ARA
  // source) regardless of whether state restore ran before or after this.
  if (isPlaybackRenderer())
    ensureHeadlessAraBinding();
#endif

  // Non-ARA capture controller.
  //
  // prepare() resizes the capture buffer and returns the controller to Idle,
  // which silently disarms a capture the user has already asked for. Hosts
  // call prepareToPlay whenever they (re)activate the plug-in, and several do
  // so between arming and the first block of audio - Audacity activates on
  // play, so an armed capture never saw a single sample there. Arming is the
  // user's intent, not DSP state, so carry it across.
  const bool wasArmed = isCaptureArmed();
  captureController->prepare(sampleRate, getMainBusNumOutputChannels(),
                             MAX_CAPTURE_SECONDS);
  if (wasArmed)
    captureController->resetToWaiting();
  lastCaptureUiState = captureController->getState();
  triggerAsyncUpdate();

  // Preparing the processor is a lifecycle operation, so this is the safe
  // place to rebuild a UI-closed playback buffer for the current host rate.
  // Never defer this to processBlock(): setProject() copies/resamples the
  // complete project waveform and takes a lock.
#if JucePlugin_Enable_ARA
  // ARA playback renderers were already handled by ensureHeadlessAraBinding()
  // above; this branch is for an ordinary/non-ARA instance of the same binary.
  if (!isPlaybackRenderer() && !mainComponent && araAnalysisProjectSnapshot)
    bindRealtimeProcessorHeadless();
#else
  if (!mainComponent && araAnalysisProjectSnapshot)
    bindRealtimeProcessorHeadless();
#endif
}

void PitchNetAudioProcessor::releaseResources() {
#if JucePlugin_Enable_ARA
  releaseResourcesForARA();
#endif
}

// ============================================================================
// Bus Layout
// ============================================================================

#if !JucePlugin_PreferredChannelConfigurations
bool PitchNetAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;
  auto out = layouts.getMainOutputChannelSet();
  return out == juce::AudioChannelSet::mono() ||
         out == juce::AudioChannelSet::stereo();
}
#endif

// ============================================================================
// Mode Detection
// ============================================================================

bool PitchNetAudioProcessor::isARAModeActive() const {
#if JucePlugin_Enable_ARA
  if (auto *editor = getActiveEditor()) {
    if (auto *araEditor =
            dynamic_cast<juce::AudioProcessorEditorARAExtension *>(editor)) {
      if (auto *editorView = araEditor->getARAEditorView()) {
        return editorView->getDocumentController() != nullptr;
      }
    }
  }
#endif
  return false;
}

HostCompatibility::HostInfo PitchNetAudioProcessor::getHostInfo() const {
  return HostCompatibility::detectHost(
      const_cast<PitchNetAudioProcessor *>(this));
}

juce::String PitchNetAudioProcessor::getHostStatusMessage() const {
  auto hostInfo = getHostInfo();
  bool araActive = isARAModeActive();

  if (hostInfo.type != HostCompatibility::HostType::Unknown) {
    if (araActive)
      return hostInfo.name + " - ARA Mode";
    if (hostInfo.supportsARA)
      return hostInfo.name + " - Non-ARA (ARA Available)";
    return hostInfo.name + " - Non-ARA Mode";
  }
  return araActive ? "ARA Mode" : "Non-ARA Mode";
}

// ============================================================================
// Output Processing (Bypass, Dry/Wet, Gain)
// ============================================================================

void PitchNetAudioProcessor::applyOutputProcessing(
    juce::AudioBuffer<float> &processedBuffer,
    const juce::AudioBuffer<float> &dryBuffer) {
  const int numSamples = processedBuffer.getNumSamples();
  const int numChannels = processedBuffer.getNumChannels();

  // Read parameters (lock-free atomic loads)
  const float dryWetPercent = dryWetParamValue->load();
  const float outputGainDb = outputGainParamValue->load();

  // Apply dry/wet mix
  const float wetAmount = dryWetPercent / 100.0f;
  if (wetAmount < 1.0f) {
    const float dryAmount = 1.0f - wetAmount;
    const int dryChannels =
        std::min(numChannels, dryBuffer.getNumChannels());
    const int drySamples =
        std::min(numSamples, dryBuffer.getNumSamples());

    for (int ch = 0; ch < dryChannels; ++ch) {
      // processed = dry * dryAmount + wet * wetAmount
      const float *dryData = dryBuffer.getReadPointer(ch);
      float *wetData = processedBuffer.getWritePointer(ch);
      for (int i = 0; i < drySamples; ++i)
        wetData[i] = dryData[i] * dryAmount + wetData[i] * wetAmount;
    }
  }

  // Apply output gain
  if (std::abs(outputGainDb) > 0.01f) {
    const float gainLinear = std::pow(10.0f, outputGainDb / 20.0f);
    processedBuffer.applyGain(gainLinear);
  }
}

// ============================================================================
// Parameter Change Detection (Audio Thread -> Message Thread)
// ============================================================================

void PitchNetAudioProcessor::checkParameterChanges() {
  if (!mainComponent)
    return;

  const float pitchOffset = pitchOffsetParamValue->load();
  const float formantShift = formantShiftParamValue->load();

  const bool pitchChanged = std::abs(pitchOffset - cachedPitchOffset) > 0.001f;
  const bool formantChanged =
      std::abs(formantShift - cachedFormantShift) > 0.001f;

  if (!pitchChanged && !formantChanged)
    return;

  cachedPitchOffset = pitchOffset;
  cachedFormantShift = formantShift;

  // Store latest values and dispatch to message thread (coalesced)
  auto syncState = paramSyncState;
  syncState->pitchOffset.store(pitchOffset);
  syncState->formantShift.store(formantShift);
  syncState->needsResynth.store(true);

  if (!syncState->pending.exchange(true)) {
    juce::Component::SafePointer<juce::Component> safeMain(
        mainComponent->getComponent());
    juce::MessageManager::callAsync([safeMain, syncState]() {
      syncState->pending.store(false);
      if (!syncState->needsResynth.exchange(false))
        return;

      auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
      if (!view)
        return;
      auto *project = view->getProject();
      if (!project)
        return;

      // Apply parameter values to project
      const float po = syncState->pitchOffset.load();
      const float fs = syncState->formantShift.load();
      bool changed = false;

      if (std::abs(project->getGlobalPitchOffset() - po) > 0.001f) {
        project->setGlobalPitchOffset(po);
        changed = true;
      }
      if (std::abs(project->getFormantShift() - fs) > 0.001f) {
        project->setFormantShift(fs);
        changed = true;
      }

      // Trigger re-synthesis if values actually changed
      if (changed) {
        view->triggerResynthesis();
      }
    });
  }
}

// ============================================================================
// Process Block
// ============================================================================

void PitchNetAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                           juce::MidiBuffer &midiMessages) {
  juce::ignoreUnused(midiMessages);
  juce::ScopedNoDenormals noDenormals;
  bool didProcessHostSync = false;

  // Check bypass — if bypassed, pass through input unchanged
  const bool bypassed = bypassParamValue->load() >= 0.5f;
  if (bypassed) {
    // Transport sync still needs to run even when bypassed
    transportController.processBlock(getPlayHead(), hostSampleRate);
    return; // Input buffer passes through unchanged
  }

#if JucePlugin_Enable_ARA
  // ARA mode: let ARA renderer handle audio, then apply output processing.
  // Do not call isARAModeActive() here; it queries the editor and must only run
  // on the message thread.
  {
    transportController.processBlock(getPlayHead(), hostSampleRate);
    didProcessHostSync = true;
    checkParameterChanges();

    if (processBlockForARA(buffer, isRealtime(), getPlayHead())) {
      // ARA playback has no meaningful input/dry buffer. The playback renderer
      // replaces the buffer from ARA audio modifications or original sources,
      // so mixing against the host input can mute playback when that input is
      // silent. Keep only the final output gain here.
      const float outputGainDb = outputGainParamValue->load();
      if (std::abs(outputGainDb) > 0.01f)
        buffer.applyGain(std::pow(10.0f, outputGainDb / 20.0f));
      sanitiseARAOutput(buffer);
      return;
    }
  }
#endif

  // Process transport control requests and update sync state. ARA reaches this
  // point only when the ARA path did not handle the block.
  if (!didProcessHostSync)
    transportController.processBlock(getPlayHead(), hostSampleRate);

  // Check for parameter automation changes (pitch offset, formant shift)
  checkParameterChanges();

  // Non-ARA mode
  juce::AudioPlayHead::PositionInfo posInfo;
  if (auto *playHead = getPlayHead()) {
    if (auto info = playHead->getPosition())
      posInfo = *info;
  }

  processNonARAMode(buffer, posInfo,
                    isRealtime() == juce::AudioProcessor::Realtime::yes);
}

void PitchNetAudioProcessor::processBlockBypassed(
    juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
  juce::ignoreUnused(midiMessages);

  // Transport sync still runs when bypassed so cursor stays in sync
  transportController.processBlock(getPlayHead(), hostSampleRate);

  // Input passes through unchanged (buffer already contains input)
}

void PitchNetAudioProcessor::processNonARAMode(
    juce::AudioBuffer<float> &buffer,
    const juce::AudioPlayHead::PositionInfo &posInfo, bool isRealtime) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();
  const bool hostIsPlaying = posInfo.getIsPlaying();

  // Check if we have analyzed project ready for real-time processing. In
  // non-ARA hosts, state can be restored before the editor is opened, so the
  // processor-owned snapshot must be enough for playback.
  bool hasProject =
      (mainComponent && mainComponent->hasAnalyzedProject()) ||
      (araAnalysisReady && araAnalysisProjectSnapshot != nullptr);

  // Update UI cursor position from host playback position (only when we have
  // analyzed audio)
  if (isRealtime && mainComponent) {
    if (hostIsPlaying && hasProject) {
      // Only sync cursor after capture is complete and analyzed
      double timeInSeconds = 0.0;
      if (auto samples = posInfo.getTimeInSamples())
        timeInSeconds = static_cast<double>(*samples) / hostSampleRate;
      else if (auto time = posInfo.getTimeInSeconds())
        timeInSeconds = *time;

      auto state = hostUiSyncState;
      state->latestSeconds.store(timeInSeconds);

      // Never touch UI on the audio thread: coalesce to a single async update
      if (!state->posPending.exchange(true)) {
        juce::Component::SafePointer<juce::Component> safeMain(
            mainComponent->getComponent());
        juce::MessageManager::callAsync([safeMain, state]() {
          state->posPending.store(false);
          if (auto *view =
                  dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->updatePlaybackPosition(state->latestSeconds.load());
        });
      }
    } else if (!hostIsPlaying && hasProject) {
      auto state = hostUiSyncState;
      if (!state->stoppedPending.exchange(true)) {
        juce::Component::SafePointer<juce::Component> safeMain(
            mainComponent->getComponent());
        juce::MessageManager::callAsync([safeMain, state]() {
          state->stoppedPending.store(false);
          if (auto *view =
                  dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->notifyHostStopped();
        });
      }
    }
  }

  if (!hostIsPlaying) {
    // Still let the capture state machine observe transport stop so it can
    // finalize and dispatch analysis, but never output audio when stopped.
    const bool captureWasRunning =
        captureController->getState() ==
        NonAraCaptureController::State::Capturing;
    captureController->processBlock(buffer, false);

    if (captureController->shouldFinalize()) {
      NonAraCaptureController::FinalizeResult result;
      if (captureController->finalizeCapture(hostSampleRate, result) &&
          mainComponent) {
        auto controller = captureController;
        juce::MessageManager::callAsync([this, controller,
                                         samples = result.numSamples,
                                         sr = result.sampleRate,
                                         timelineOffset =
                                             captureTimelineOffsetSeconds]() mutable {
          if (!controller)
            return;
          auto trimmed = controller->copyCapturedAudio(samples);
          controller->onAnalysisDispatched();
          requestCapturedAudioAnalysis(trimmed, sr, timelineOffset);
        });
      }
    }

    if (captureWasRunning)
      disarmCaptureUi();

    // Audition the synthesized edit for the selected range while stopped.
    // Only fires when a project is analyzed and ready, so it never interferes
    // with capture finalization above.
    if (processPluginPreview(buffer))
      return;

    buffer.clear();
    return;
  }

  if (!isCaptureArmed() && hasProject) {
    if (!realtimeProcessor.isReady())
      return;

    // Save dry copy for dry/wet mixing
    juce::AudioBuffer<float> dryBuffer;
    const float dryWet = dryWetParamValue->load();
    if (dryWet < 99.9f)
      dryBuffer.makeCopyOf(buffer);

    // Real-time pitch correction mode
    juce::AudioBuffer<float> outputBuffer(numChannels, numSamples);
    if (realtimeProcessor.processBlock(buffer, outputBuffer, &posInfo)) {
      for (int ch = 0; ch < numChannels; ++ch)
        buffer.copyFrom(ch, 0, outputBuffer, ch, 0, numSamples);

      // Apply dry/wet mix and output gain
      applyOutputProcessing(buffer, dryBuffer);
    }
    return;
  }

  // Capture mode
  const auto stateBeforeCapture = captureController->getState();
  if (stateBeforeCapture == NonAraCaptureController::State::WaitingForAudio) {
    if (auto samples = posInfo.getTimeInSamples())
      captureTimelineOffsetSeconds =
          std::max(0.0, static_cast<double>(*samples) / hostSampleRate);
    else if (auto seconds = posInfo.getTimeInSeconds())
      captureTimelineOffsetSeconds = std::max(0.0, *seconds);
    else
      captureTimelineOffsetSeconds = 0.0;
    liveCaptureUiState->timelineOffsetSeconds.store(
        captureTimelineOffsetSeconds);
  }

  captureController->processBlock(buffer, hostIsPlaying);
  dispatchLiveCaptureUpdate();

  // UI: transition into recording
  auto currentState = captureController->getState();
  if (currentState != lastCaptureUiState) {
    if (currentState == NonAraCaptureController::State::Capturing &&
        mainComponent) {
      juce::Component::SafePointer<juce::Component> safeMain(
          mainComponent->getComponent());
      juce::MessageManager::callAsync([safeMain]() {
        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->setStatusMessage(TR("progress.recording"));
      });
    }
    lastCaptureUiState = currentState;
  }

  if (captureController->shouldFinalize()) {
    NonAraCaptureController::FinalizeResult result;
    if (captureController->finalizeCapture(hostSampleRate, result) &&
        mainComponent) {
      auto controller = captureController;
      juce::MessageManager::callAsync([this, controller,
                                       samples = result.numSamples,
                                       sr = result.sampleRate,
                                       timelineOffset =
                                           captureTimelineOffsetSeconds]() mutable {
        if (!controller)
          return;
        auto trimmed = controller->copyCapturedAudio(samples);
        controller->onAnalysisDispatched();
        requestCapturedAudioAnalysis(trimmed, sr, timelineOffset);
      });
    }
    disarmCaptureUi();
  }

  // Passthrough during capture
}

// ============================================================================
// Non-ARA Capture Control
// ============================================================================

void PitchNetAudioProcessor::handleAsyncUpdate() {
  if (wrapperType == wrapperType_AudioUnit)
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withTailLengthChanged(true));
}

void PitchNetAudioProcessor::startCapture() {
  auto state = liveCaptureUiState;
  state->generation.fetch_add(1);
  state->pending.store(false);
  state->latestSamples.store(0);
  state->sentSamples.store(0);
  state->timelineOffsetSeconds.store(0.0);
  captureTimelineOffsetSeconds = 0.0;
  captureController->resetToWaiting();
  handleAsyncUpdate();
}

void PitchNetAudioProcessor::stopCapture() {
  captureController->stop();
  handleAsyncUpdate();
}

void PitchNetAudioProcessor::bindRealtimeProcessorHeadless() {
  if (mainComponent != nullptr)
    return; // an open editor drives the binding
  if (!araAnalysisProjectSnapshot)
    return; // nothing analyzed yet
  realtimeProcessor.setProject(araAnalysisProjectSnapshot.get());
}

void PitchNetAudioProcessor::startPluginPreview(double startSeconds,
                                                double endSeconds) {
  pluginPreview.startSeconds.store(std::max(0.0, startSeconds));
  pluginPreview.endSeconds.store(std::max(0.0, endSeconds));
  pluginPreview.restart.store(true);
  pluginPreview.active.store(true);
}

void PitchNetAudioProcessor::stopPluginPreview() {
  pluginPreview.active.store(false);
}

void PitchNetAudioProcessor::startPluginAudition(
    const juce::AudioBuffer<float> &buffer, double sampleRate) {
  if (buffer.getNumSamples() <= 0)
    return;

  // Drag and piano-key audition buffers are produced at the Project's sample
  // rate, which is not necessarily the current host rate (for example after a
  // session-rate change or when restoring captured state). The audio-thread
  // loop below deliberately uses integer cursors, so publish host-rate audio
  // here rather than letting one source sample incorrectly equal one host
  // sample.
  const double sourceRate =
      std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
  const double targetRate =
      std::isfinite(hostSampleRate) && hostSampleRate > 0.0
          ? hostSampleRate
          : sourceRate;
  auto preview = std::make_shared<juce::AudioBuffer<float>>();

  if (juce::approximatelyEqual(sourceRate, targetRate)) {
    preview->makeCopyOf(buffer);
  } else {
    const int sourceSamples = buffer.getNumSamples();
    const auto scaledLength = static_cast<juce::int64>(std::llround(
        static_cast<double>(sourceSamples) * targetRate / sourceRate));
    const int outputSamples = static_cast<int>(juce::jlimit<juce::int64>(
        1, std::numeric_limits<int>::max(), scaledLength));
    preview->setSize(buffer.getNumChannels(), outputSamples);

    const double sourceStep = sourceRate / targetRate;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
      const auto *source = buffer.getReadPointer(channel);
      auto *output = preview->getWritePointer(channel);
      for (int sample = 0; sample < outputSamples; ++sample) {
        const double sourcePosition = std::min(
            static_cast<double>(sourceSamples - 1), sample * sourceStep);
        const int left = static_cast<int>(sourcePosition);
        const int right = std::min(sourceSamples - 1, left + 1);
        const float fraction = static_cast<float>(sourcePosition - left);
        output[sample] =
            source[left] + fraction * (source[right] - source[left]);
      }
    }
  }

  std::atomic_store(&pluginAuditionBuffer, std::move(preview));
  pluginPreview.restart.store(true);
  pluginPreview.active.store(true);
}

void PitchNetAudioProcessor::stopPluginAudition() {
  std::atomic_store(&pluginAuditionBuffer,
                    std::shared_ptr<juce::AudioBuffer<float>>{});
  stopPluginPreview();
}

bool PitchNetAudioProcessor::processPluginPreview(
    juce::AudioBuffer<float> &buffer) {
  if (!pluginPreview.active.load())
    return false;

  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  // A fresh preview request restarts playback from the start of the range.
  if (pluginPreview.restart.exchange(false))
    pluginPreviewCursor = 0;

  if (auto audition = std::atomic_load(&pluginAuditionBuffer)) {
    buffer.clear();
    if (audition != activePluginAuditionBuffer) {
      previousPluginAuditionBuffer = activePluginAuditionBuffer;
      previousPluginAuditionCursor = pluginAuditionCursor;
      activePluginAuditionBuffer = audition;
      pluginAuditionCursor = 0;
      pluginAuditionTransitionTotal = 4096;
      pluginAuditionTransitionRemaining =
          previousPluginAuditionBuffer ? pluginAuditionTransitionTotal : 0;
    }

    const int sourceSamples = activePluginAuditionBuffer->getNumSamples();
    const int sourceChannels = activePluginAuditionBuffer->getNumChannels();
    if (sourceSamples <= 0 || sourceChannels <= 0)
      return true;

    // Audition snippets are normally mono.  A host still supplies a stereo
    // (or wider) output buffer, so mirror a mono snippet to every output
    // channel instead of leaving everything after channel 0 silent.
    const int channels = sourceChannels == 1
                             ? numChannels
                             : std::min(numChannels, sourceChannels);

    auto renderLoopSample = [](const juce::AudioBuffer<float> &source,
                               juce::int64 cursor, int channel) {
      const int length = source.getNumSamples();
      const int overlap = std::min(8192, std::max(1, length / 2));
      const int overlapStart = length - overlap;
      const int position = static_cast<int>(cursor);
      float value = source.getSample(channel, position);
      if (position >= overlapStart) {
        const float t = static_cast<float>(position - overlapStart) / overlap;
        value = value * std::cos(t * juce::MathConstants<float>::halfPi) +
                source.getSample(channel, position - overlapStart) *
                    std::sin(t * juce::MathConstants<float>::halfPi);
      }
      return value;
    };
    auto advanceLoopCursor = [](const juce::AudioBuffer<float> &source,
                                juce::int64 &cursor) {
      const int length = source.getNumSamples();
      const int overlap = std::min(8192, std::max(1, length / 2));
      if (++cursor >= length)
        cursor -= length - overlap;
    };

    for (int sample = 0; sample < numSamples; ++sample) {
      for (int ch = 0; ch < channels; ++ch) {
        const int sourceChannel = sourceChannels == 1 ? 0 : ch;
        float value = renderLoopSample(*activePluginAuditionBuffer,
                                       pluginAuditionCursor, sourceChannel);
        if (pluginAuditionTransitionRemaining > 0 &&
            previousPluginAuditionBuffer &&
            (previousPluginAuditionBuffer->getNumChannels() == 1 ||
             ch < previousPluginAuditionBuffer->getNumChannels())) {
          const int previousSourceChannel =
              previousPluginAuditionBuffer->getNumChannels() == 1 ? 0 : ch;
          const float oldValue = renderLoopSample(
              *previousPluginAuditionBuffer, previousPluginAuditionCursor,
              previousSourceChannel);
          const float t = 1.0f - static_cast<float>(pluginAuditionTransitionRemaining) /
                                      static_cast<float>(pluginAuditionTransitionTotal);
          value = oldValue * std::cos(t * juce::MathConstants<float>::halfPi) +
                  value * std::sin(t * juce::MathConstants<float>::halfPi);
        }
        buffer.setSample(ch, sample, value);
      }
      advanceLoopCursor(*activePluginAuditionBuffer, pluginAuditionCursor);
      if (pluginAuditionTransitionRemaining > 0 && previousPluginAuditionBuffer)
        advanceLoopCursor(*previousPluginAuditionBuffer,
                          previousPluginAuditionCursor);
      if (pluginAuditionTransitionRemaining > 0)
        --pluginAuditionTransitionRemaining;
    }
    return true;
  }

  if (!realtimeProcessor.isReady())
    return false;

  const double sr = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
  const auto startSample = static_cast<juce::int64>(
      std::llround(pluginPreview.startSeconds.load() * sr));
  const auto endSample = static_cast<juce::int64>(
      std::llround(pluginPreview.endSeconds.load() * sr));
  const juce::int64 total = endSample - startSample;

  // Active but nothing (more) to play: hold silence until re-triggered or
  // stopped. This mirrors the ARA editor renderer's play-once behaviour.
  buffer.clear();
  if (total <= 0 || pluginPreviewCursor >= total)
    return true;

  // Read the synthesized timeline at the absolute preview position. isPlaying
  // is false so the realtime processor treats this as a one-shot render and
  // does not disturb its streaming cursor.
  const juce::int64 readStart = startSample + pluginPreviewCursor;
  juce::AudioPlayHead::PositionInfo pos;
  pos.setTimeInSamples(readStart);
  pos.setTimeInSeconds(static_cast<double>(readStart) / sr);
  pos.setIsPlaying(false);

  juce::AudioBuffer<float> silentInput(numChannels, numSamples);
  silentInput.clear();
  juce::AudioBuffer<float> rendered(numChannels, numSamples);
  rendered.clear();
  if (!realtimeProcessor.processBlock(silentInput, rendered, &pos))
    return true; // not ready / no data -> silence

  const int toCopy = static_cast<int>(
      std::min<juce::int64>(numSamples, total - pluginPreviewCursor));
  for (int ch = 0; ch < numChannels; ++ch)
    buffer.copyFrom(ch, 0, rendered, ch, 0, toCopy);

  pluginPreviewCursor += toCopy;
  return true;
}

void PitchNetAudioProcessor::disarmCaptureUi() {
  // One-shot message-thread notification, not a polling timer.
  triggerAsyncUpdate();
  if (!mainComponent)
    return;

  juce::Component::SafePointer<juce::Component> safeMain(
      mainComponent->getComponent());
  juce::MessageManager::callAsync([safeMain]() {
    if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
      view->updateRecordArmState(false);
  });
}

void PitchNetAudioProcessor::dispatchLiveCaptureUpdate() {
  if (!mainComponent)
    return;

  auto state = liveCaptureUiState;
  const int latest = captureController->getCapturedSampleCount();
  state->latestSamples.store(latest);
  if (latest <= state->sentSamples.load() || state->pending.exchange(true))
    return;

  const auto generation = state->generation.load();
  auto controller = captureController;
  juce::Component::SafePointer<juce::Component> safeMain(
      mainComponent->getComponent());
  juce::MessageManager::callAsync(
      [safeMain, controller, state, generation,
       sampleRate = hostSampleRate]() {
        if (generation != state->generation.load()) {
          state->pending.store(false);
          return;
        }

        const int start = state->sentSamples.load();
        const int end = state->latestSamples.load();
        if (end > start) {
          auto chunk = controller->copyCapturedAudioRange(start, end - start);
          if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent())) {
            if (start == 0)
              view->beginLiveRecording(sampleRate,
                                       state->timelineOffsetSeconds.load());
            view->appendLiveRecordingAudio(chunk);
          }
          state->sentSamples.store(end);
        }
        state->pending.store(false);
      });
}
void PitchNetAudioProcessor::requestCapturedAudioAnalysis(
    const juce::AudioBuffer<float> &buffer, double sampleRate,
    double timelineOffsetSeconds) {
  if (buffer.getNumSamples() <= 0 || sampleRate <= 0.0)
    return;

  // Analysis is asynchronous. Preserve the completed regions now rather than
  // trying to recover them from mutable UI/controller state in the callback.
  std::shared_ptr<Project> previousCapturedProject;
  if (mainComponent) {
    if (auto *current = mainComponent->getProject();
        current && current->getAudioData().waveform.getNumSamples() > 0)
      previousCapturedProject = std::make_shared<Project>(*current);
  }
  if (!previousCapturedProject && araAnalysisProjectSnapshot &&
      araAnalysisProjectSnapshot->getAudioData().waveform.getNumSamples() > 0)
    previousCapturedProject =
        std::make_shared<Project>(*araAnalysisProjectSnapshot);

  undoManager->clear();

  if (!araAnalysisController)
    araAnalysisController = std::make_unique<EditorController>(false);

  araAnalysisSourceKey = 0;
  araAnalysisLoading = true;
  araAnalysisReady = false;
  araAnalysisTimelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
  araAnalysisProjectSnapshot.reset();
  invalidateAraAnalysisProjectJson();

  if (mainComponent) {
    mainComponent->setStatusMessage(TR("progress.analyzing"));
    mainComponent->showAnalysisProgress(0.0);
  }

  araAnalysisController->setHostAudioAsync(
      buffer, sampleRate,
      [this](double progress, const juce::String &msg) {
        juce::Component::SafePointer<juce::Component> safeMain(
            mainComponent ? mainComponent->getComponent() : nullptr);
        juce::MessageManager::callAsync([safeMain, progress, msg]() {
          if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent())) {
            view->setStatusMessage(msg);
            view->showAnalysisProgress(progress);
          }
        });
      },
      [this, timelineOffsetSeconds,
       previousCapturedProject](const juce::AudioBuffer<float> &) {
        if (!araAnalysisController)
          return;

        auto *analyzedProject = araAnalysisController->getProject();
        if (!analyzedProject)
          return;

        std::unique_ptr<Project> completedProject;
        const bool hasPreviousCapture =
            previousCapturedProject &&
            previousCapturedProject->getAudioData().waveform.getNumSamples() >
                0;

        if (hasPreviousCapture) {
          completedProject =
              std::make_unique<Project>(*previousCapturedProject);
          auto &dst = completedProject->getAudioData();
          const auto &src = analyzedProject->getAudioData();
          const int dstRate = std::max(1, dst.sampleRate);
          const int sampleOffset = std::max(
              0, static_cast<int>(std::llround(timelineOffsetSeconds *
                                               dstRate)));
          const int frameOffset = std::max(
              0, static_cast<int>(std::llround(
                     timelineOffsetSeconds * dstRate /
                     static_cast<double>(HOP_SIZE))));
          const int captureFrames = std::max(
              1, static_cast<int>(std::ceil(
                     src.waveform.getNumSamples() /
                     static_cast<double>(HOP_SIZE))));
          const int captureEndFrame = frameOffset + captureFrames;

          auto mergeBuffer = [sampleOffset](juce::AudioBuffer<float> &to,
                                             const juce::AudioBuffer<float> &from) {
            const int channels =
                std::max(to.getNumChannels(), from.getNumChannels());
            const int required = sampleOffset + from.getNumSamples();
            if (to.getNumChannels() < channels ||
                to.getNumSamples() < required)
              to.setSize(channels, std::max(to.getNumSamples(), required),
                         true, true, false);
            for (int ch = 0; ch < from.getNumChannels(); ++ch)
              to.copyFrom(ch, sampleOffset, from, ch, 0,
                          from.getNumSamples());
          };
          mergeBuffer(dst.waveform, src.waveform);
          mergeBuffer(dst.originalWaveform, src.originalWaveform);

          auto mergeFloats = [frameOffset](std::vector<float> &to,
                                            const std::vector<float> &from) {
            to.resize(std::max(to.size(), static_cast<size_t>(frameOffset) +
                                             from.size()),
                      0.0f);
            std::copy(from.begin(), from.end(), to.begin() + frameOffset);
          };
          auto mergeBools = [frameOffset](std::vector<bool> &to,
                                           const std::vector<bool> &from) {
            to.resize(std::max(to.size(), static_cast<size_t>(frameOffset) +
                                             from.size()),
                      false);
            for (size_t i = 0; i < from.size(); ++i)
              to[static_cast<size_t>(frameOffset) + i] = from[i];
          };
          mergeFloats(dst.rawF0, src.rawF0);
          mergeFloats(dst.cleanedF0, src.cleanedF0);
          mergeFloats(dst.denseF0, src.denseF0);
          mergeFloats(dst.f0, src.f0);
          mergeFloats(dst.baseF0, src.baseF0);
          mergeFloats(dst.basePitch, src.basePitch);
          mergeFloats(dst.deltaPitch, src.deltaPitch);
          mergeBools(dst.voicedMask, src.voicedMask);
          mergeBools(dst.vadMask, src.vadMask);
          dst.melSpectrogram.resize(
              std::max(dst.melSpectrogram.size(),
                       static_cast<size_t>(frameOffset) +
                           src.melSpectrogram.size()));
          std::copy(src.melSpectrogram.begin(), src.melSpectrogram.end(),
                    dst.melSpectrogram.begin() + frameOffset);

          auto &notes = completedProject->getNotes();
          notes.erase(
              std::remove_if(notes.begin(), notes.end(),
                             [frameOffset, captureEndFrame](const Note &note) {
                               return note.getSrcStartFrame() < captureEndFrame &&
                                      note.getSrcEndFrame() > frameOffset;
                             }),
              notes.end());
          for (auto note : analyzedProject->getNotes()) {
            note.setStartFrame(note.getStartFrame() + frameOffset);
            note.setEndFrame(note.getEndFrame() + frameOffset);
            note.setSrcStartFrame(note.getSrcStartFrame() + frameOffset);
            note.setSrcEndFrame(note.getSrcEndFrame() + frameOffset);
            completedProject->addNote(std::move(note));
          }

          dst.segmentChunkRanges.erase(
              std::remove_if(dst.segmentChunkRanges.begin(),
                             dst.segmentChunkRanges.end(),
                             [frameOffset, captureEndFrame](const auto &range) {
                               return range.first < captureEndFrame &&
                                      range.second > frameOffset;
                             }),
              dst.segmentChunkRanges.end());
          for (auto range : src.segmentChunkRanges)
            dst.segmentChunkRanges.emplace_back(range.first + frameOffset,
                                                range.second + frameOffset);

          dst.segmentDebugChunks.erase(
              std::remove_if(dst.segmentDebugChunks.begin(),
                             dst.segmentDebugChunks.end(),
                             [frameOffset, captureEndFrame](const auto &chunk) {
                               return chunk.startFrame < captureEndFrame &&
                                      chunk.endFrame > frameOffset;
                             }),
              dst.segmentDebugChunks.end());
          for (auto chunk : src.segmentDebugChunks) {
            chunk.startFrame += frameOffset;
            chunk.endFrame += frameOffset;
            for (auto &event : chunk.events) {
              event.startFrame += frameOffset;
              event.endFrame += frameOffset;
              event.attachedStartFrame += frameOffset;
            }
            dst.segmentDebugChunks.push_back(std::move(chunk));
          }

          dst.timelineOffsetSeconds =
              std::min(std::max(0.0, dst.timelineOffsetSeconds),
                       std::max(0.0, timelineOffsetSeconds));
        } else {
          completedProject = std::make_unique<Project>(*analyzedProject);
        }

        araAnalysisProjectSnapshot =
            std::make_unique<Project>(*completedProject);
        invalidateAraAnalysisProjectJson();
        araAnalysisLoading = false;
        araAnalysisReady = araAnalysisProjectSnapshot != nullptr;

        if (mainComponent && araAnalysisReady) {
          mainComponent->restoreProjectSnapshot(*araAnalysisProjectSnapshot);
          // The first capture is local to zero and still needs positioning.
          // Later captures are merged directly at their absolute DAW offset.
          if (!hasPreviousCapture)
            mainComponent->updateHostAudioTimelineOffset(
                araAnalysisTimelineOffsetSeconds);
          // Keep the processor-owned snapshot in the same absolute timeline
          // representation as the UI project for the next capture.
          if (auto *positionedProject = mainComponent->getProject()) {
            araAnalysisProjectSnapshot =
                std::make_unique<Project>(*positionedProject);
            invalidateAraAnalysisProjectJson();
          }
          if (araAnalysisProjectSnapshot)
            publishPersistentProjectSnapshot(*araAnalysisProjectSnapshot);
          mainComponent->bindRealtimeProcessor(realtimeProcessor);
          mainComponent->hideAnalysisProgress();
        } else if (mainComponent) {
          mainComponent->hideAnalysisProgress();
        } else if (araAnalysisProjectSnapshot) {
          // Capture analysis may outlive the editor that started it. Keep
          // headless playback current without doing any project-sized work in
          // processBlock().
          publishPersistentProjectSnapshot(*araAnalysisProjectSnapshot);
          bindRealtimeProcessorHeadless();
        }
      });
}

void PitchNetAudioProcessor::requestPluginProjectRender(
    const Project &projectToRender) {
  bool renderActiveAraRegion = false;
#if JucePlugin_Enable_ARA
  renderActiveAraRegion =
      canvasShowsActiveAraRegion && activeRegionKey.isNotEmpty();
#endif

  if (renderActiveAraRegion) {
    if (!regionCanvasController)
      regionCanvasController = std::make_unique<EditorController>(false);
  } else if (!araAnalysisController) {
    araAnalysisController = std::make_unique<EditorController>(false);
  }

  auto *controller = renderActiveAraRegion ? regionCanvasController.get()
                                           : araAnalysisController.get();
  if (controller == nullptr)
    return;

  auto *backendProject = controller->getProject();
  if (!backendProject)
  {
    controller->setProject(std::make_unique<Project>(projectToRender));
    backendProject = controller->getProject();
  }
  else if (backendProject != &projectToRender)
  {
    *backendProject = projectToRender;
  }

  juce::Component::SafePointer<juce::Component> safeMain(
      mainComponent ? mainComponent->getComponent() : nullptr);

  const auto renderRegionKey = activeRegionKey;
  const auto renderRegionRevision =
      renderActiveAraRegion ? araRegions[renderRegionKey].revision : 0;
  auto *renderModification = activeModification;
  const auto renderStartSampleInModification = activeStartSampleInModification;
  const double renderRegionStartSeconds = activeRegionStartSeconds;
  const double renderRegionEndSeconds = activeRegionEndSeconds;
  std::vector<juce::Range<int>> renderChangedSampleRanges;
#if JucePlugin_Enable_ARA
  if (renderActiveAraRegion) {
    const auto &audioData = projectToRender.getAudioData();
    const double renderRate =
        audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                                 : hostSampleRate;
    renderChangedSampleRanges =
        collectDirtyModificationSampleRanges(projectToRender, renderRate);
  }
#endif
  auto &pendingRerun =
      renderActiveAraRegion ? regionCanvasRenderPendingRerun
                            : araRenderPendingRerun;

  controller->resynthesizeIncrementalAsync(
      *backendProject,
      [safeMain](const juce::String &message) {
        juce::MessageManager::callAsync([safeMain, message]() {
          if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->setStatusMessage(message);
        });
      },
      [this, controller, renderActiveAraRegion, renderRegionKey, renderRegionRevision,
       renderModification, renderStartSampleInModification,
       renderRegionStartSeconds, renderRegionEndSeconds,
       renderChangedSampleRanges](bool success) {
        if (renderActiveAraRegion) {
          const auto current = araRegions.find(renderRegionKey);
          if (current == araRegions.end() ||
              current->second.revision != renderRegionRevision) {
            regionCanvasRenderPendingRerun.store(false);
            return; // The host split/replaced this project's note and audio data.
          }
        }
        auto *renderedProject = controller != nullptr ? controller->getProject()
                                                     : nullptr;

        if (success && renderedProject) {
#if JucePlugin_Enable_ARA
          if (renderActiveAraRegion && renderRegionKey.isNotEmpty()) {
            auto *renderSource = renderModification != nullptr
                                     ? renderModification->getAudioSource()
                                     : nullptr;
            stampSpanInModificationTime(
                *renderedProject, renderStartSampleInModification,
                std::max(0.0,
                         renderRegionEndSeconds - renderRegionStartSeconds),
                renderSource != nullptr ? renderSource->getSampleRate() : 0.0);
            auto &audioData = renderedProject->getAudioData();

            Project *persistentProject = nullptr;
            if (renderRegionKey == activeRegionKey &&
                canvasShowsActiveAraRegion && mainComponent != nullptr)
              persistentProject = mainComponent->getProject();
            if (persistentProject == nullptr)
              persistentProject = araRegions[renderRegionKey].project.get();

            if (persistentProject != nullptr)
              mergeRenderedState(*persistentProject, *renderedProject);
            else
              araRegions[renderRegionKey].project =
                  std::make_unique<Project>(*renderedProject);
            publishPersistentProjectSnapshot(*renderedProject);

            if (renderModification != nullptr) {
              juce::MemoryBlock projectArchive;
              if (serializeAraRegionProject(renderRegionKey, projectArchive))
                renderModification->setProjectArchive(
                    projectArchive.getData(), projectArchive.getSize());
            }

            if (renderModification != nullptr &&
                projectHasRegionEdits(*renderedProject) &&
                audioData.waveform.getNumSamples() > 0) {
              const double processedRate =
                  audioData.sampleRate > 0
                      ? static_cast<double>(audioData.sampleRate)
                      : hostSampleRate;
              // The project is stored in modification time and spans the
              // whole modification, so publish it whole. Each region maps its
              // own span out of it via getStartInAudioModificationSamples().
              juce::AudioBuffer<float> processedSlice;
              processedSlice.makeCopyOf(audioData.waveform);
              if (!renderChangedSampleRanges.empty()) {
                juce::AudioBuffer<float> previousProcessed;
                double previousRate = 0.0;
                juce::int64 previousStart = 0;
                const bool hasPrevious =
                    renderModification->copyProcessedAudio(
                        previousProcessed, previousRate, previousStart);
                if (hasPrevious)
                  preserveProcessedAudioOutsideRanges(
                      processedSlice, processedRate,
                      /*startSampleInModification*/ 0, previousProcessed,
                      previousRate, previousStart, renderChangedSampleRanges);
              }
              ARA_DIAG("publish[render] mod=" + ARA_DIAG_PTR(renderModification) +
                       " key=" + renderRegionKey + " samples=" +
                       juce::String(processedSlice.getNumSamples()) +
                       " rate=" + juce::String(processedRate, 1) + " offset=0" +
                       " fp=" + juce::String(araDiagFingerprint(processedSlice), 6) +
                       " changedRanges=" +
                       juce::String(static_cast<int>(renderChangedSampleRanges.size())));
              renderModification->setProcessedAudio(
                  processedSlice, processedRate,
                  /*startSampleInModification*/ 0);
              renderModification->notifyContentChanged(
                  juce::ARAContentUpdateScopes::samplesAreAffected(), true);
              for (auto *region : renderModification->getPlaybackRegions())
                if (region != nullptr)
                  region->notifyContentChanged(
                      juce::ARAContentUpdateScopes::samplesAreAffected(), true);
            } else if (ARA_DIAG("clear[render] mod=" +
                                ARA_DIAG_PTR(renderModification) + " key=" +
                                renderRegionKey),
                       clearProcessedRegionAudio(renderModification)) {
              renderModification->notifyContentChanged(
                  juce::ARAContentUpdateScopes::samplesAreAffected(), true);
              for (auto *region : renderModification->getPlaybackRegions())
                if (region != nullptr)
                  region->notifyContentChanged(
                      juce::ARAContentUpdateScopes::samplesAreAffected(), true);
            }
          } else
#endif
          {
            renderedProject->getAudioData().timelineOffsetSeconds =
                araAnalysisTimelineOffsetSeconds;
            araAnalysisProjectSnapshot =
                std::make_unique<Project>(*renderedProject);
            invalidateAraAnalysisProjectJson();
            araAnalysisReady = true;
            publishPersistentProjectSnapshot(*renderedProject);
          }
        }

        juce::Component::SafePointer<juce::Component> renderSafeMain(
            mainComponent ? mainComponent->getComponent() : nullptr);
        juce::MessageManager::callAsync(
            [this, renderSafeMain, success, renderActiveAraRegion,
             renderRegionStartSeconds]() {
              if (auto *view =
                      dynamic_cast<IMainView *>(renderSafeMain.getComponent())) {
                if (success) {
                  // updateHostAudioTimelineOffset() REPADS the project's
                  // buffers to relocate content. ARA projects are stored in
                  // modification time and must never be padded: passing the
                  // region's timeline start here re-padded on every render,
                  // and since the stamp resets the field to zero in between,
                  // the waveform grew by the region position each cycle.
                  view->updateHostAudioTimelineOffset(
                      renderActiveAraRegion ? 0.0
                                            : araAnalysisTimelineOffsetSeconds);
                  view->bindRealtimeProcessor(realtimeProcessor);
                }
                view->finishBackendRender(success);
                view->setStatusMessage({});
              } else if (success && !renderActiveAraRegion &&
                         araAnalysisProjectSnapshot) {
                // The render finished after its editor closed. Refresh once on
                // the message thread; bindRealtimeProcessorHeadless() will
                // no-op if a replacement editor has since attached.
                bindRealtimeProcessorHeadless();
              }
            });
      },
      pendingRerun, true);
}

void PitchNetAudioProcessor::updateProjectStateFromEditor(
    const Project &project) {
  std::unique_ptr<Project> araRegionScopedProject;
#if JucePlugin_Enable_ARA
  if (canvasShowsActiveAraRegion && activeRegionKey.isNotEmpty()) {
    araRegionScopedProject = std::make_unique<Project>(project);
    stampActiveRegionSpan(*araRegionScopedProject);

    if (mainComponent != nullptr) {
      if (auto *uiProject = mainComponent->getProject()) {
        stampActiveRegionSpan(*uiProject);
        if (auto *component = mainComponent->getComponent())
          component->repaint();
      }
    }
  }
#endif

  const Project &stateProject =
      araRegionScopedProject != nullptr ? *araRegionScopedProject : project;

  araAnalysisProjectSnapshot = std::make_unique<Project>(stateProject);
  invalidateAraAnalysisProjectJson();
  araAnalysisReady =
      stateProject.getAudioData().waveform.getNumSamples() > 0 &&
      !stateProject.getAudioData().f0.empty();
  cachedPitchOffset = stateProject.getGlobalPitchOffset();
  cachedFormantShift = stateProject.getFormantShift();
  publishPersistentProjectSnapshot(stateProject);

#if JucePlugin_Enable_ARA
  // Publish per-region state only when the canvas actually holds the ACTIVE
  // REGION's own project. This callback also fires when the composite/document
  // analysis lands in the canvas; caching or publishing that project under the
  // region's key stored the whole-timeline waveform as the region's processed
  // audio, so the renderer played the composite's leading silence at the
  // region position (the old realtime-processor safety net masked this).
  if (canvasShowsActiveAraRegion && activeRegionKey.isNotEmpty()) {
    if (activeModification != nullptr) {
      juce::MemoryBlock projectArchive;
      if (serializeAraRegionProject(activeRegionKey, projectArchive))
        activeModification->setProjectArchive(projectArchive.getData(),
                                              projectArchive.getSize());
    }

    // Resynth-on-edit: republish the active region's freshly synthesised
    // waveform onto its modification so per-region data (headless playback,
    // persistence, timeline clips) reflects the edit instead of the
    // analysis-time audio. Only for regions that were actually CHANGED —
    // an unedited region stores nothing and keeps playing its original source.
    if (activeModification != nullptr && projectHasRegionEdits(stateProject)) {
      const auto &processed = stateProject.getAudioData().waveform;
      const double processedRate =
          stateProject.getAudioData().sampleRate > 0
              ? static_cast<double>(stateProject.getAudioData().sampleRate)
              : hostSampleRate;
      if (processed.getNumSamples() > 0) {
        // Modification-scoped project: publish it whole at offset zero.
        ARA_DIAG("publish[edit] mod=" + ARA_DIAG_PTR(activeModification) +
                 " key=" + activeRegionKey + " samples=" +
                 juce::String(processed.getNumSamples()) + " rate=" +
                 juce::String(processedRate, 1) + " offset=0" +
                 " fp=" + juce::String(araDiagFingerprint(processed), 6));
        activeModification->setProcessedAudio(processed, processedRate,
                                              /*startSampleInModification*/ 0);

        // Tell the host the rendered samples changed (ARAPluginDemo pattern).
        // ARA hosts prefetch/pre-render playback-renderer output ahead of the
        // playhead; without this notification they keep playing the stale
        // pre-edit render, so edits seemed to "not take effect" until the
        // host happened to re-render on its own.
        activeModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected(), true);
        for (auto *region : activeModification->getPlaybackRegions())
          if (region != nullptr)
            region->notifyContentChanged(
                juce::ARAContentUpdateScopes::samplesAreAffected(), true);
      }
    } else if (activeModification != nullptr) {
      ARA_DIAG("clear[edit] mod=" + ARA_DIAG_PTR(activeModification) +
               " key=" + activeRegionKey + " reason=noRegionEdits");
      if (clearProcessedRegionAudio(activeModification)) {
        activeModification->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected(), true);
        for (auto *region : activeModification->getPlaybackRegions())
          if (region != nullptr)
            region->notifyContentChanged(
                juce::ARAContentUpdateScopes::samplesAreAffected(), true);
      }
    }
  }
#endif
}

#if JucePlugin_Enable_ARA
bool PitchNetAudioProcessor::projectHasRegionEdits(const Project &project) {
  if (project.getGlobalPitchOffset() != 0.0f ||
      project.getFormantShift() != 0.0f)
    return true;

  // Compare each note against the original rather than trusting
  // hasRenderedEdit(). That flag is historical: it records that a note once
  // contributed to the persisted composite, and it is only refreshed for
  // commit anchors, so resetting one note cleared its own flag while every
  // other previously-edited note kept the whole region on the resynthesis
  // path. A fully reset region therefore never fell back to the untouched ARA
  // source, and stayed on a re-synthesised blob whose splice boundaries are
  // audible as a click on every syllable.
  //
  // isNeutralForOriginalWaveform() checks timing, pitch, baked delta, offset,
  // formant, volume, tilt, vibrato, drift, smoothing and delta scale, so a
  // note it calls neutral is one the original audio already represents
  // exactly.
  for (const auto &note : project.getNotes())
    if (!note.isRest() && !note.isNeutralForOriginalWaveform())
      return true;

  return false;
}
#endif
const Project *PitchNetAudioProcessor::projectForPersistentState() const {
  if (mainComponent) {
    if (auto *project = mainComponent->getProject())
      return project;
  }

  if (araAnalysisProjectSnapshot)
    return araAnalysisProjectSnapshot.get();

  if (araAnalysisController && araAnalysisController->getProject())
    return araAnalysisController->getProject();

  return nullptr;
}

bool PitchNetAudioProcessor::serializePersistentProjectState(
    juce::MemoryBlock &destData, bool hostBackedARA) const {
  destData.setSize(0);
  juce::MemoryOutputStream out(destData, false);
  return serializePersistentProjectState(out, hostBackedARA);
}

bool PitchNetAudioProcessor::serializePersistentProjectState(
    juce::OutputStream &out, bool hostBackedARA) const {
  const auto archiveMode =
      hostBackedARA
          ? ProjectSerializer::BinaryArchiveMode::hostBackedARA
          : ProjectSerializer::BinaryArchiveMode::selfContained;

  if (const auto *project = projectForPersistentState())
    return ProjectSerializer::toBinaryArchive(*project, out, archiveMode);

  // Deliberately outside projectForPersistentState(): there is no Project here
  // to fingerprint, so computePersistentStateKey() returns its "nothing
  // cacheable" sentinel and this path is written fresh every time.
  if (pendingStateJson.isNotEmpty()) {
    Project pendingProject;
    if (ProjectSerializer::fromJson(
            pendingProject, juce::JSON::parse(pendingStateJson)))
      return ProjectSerializer::toBinaryArchive(pendingProject, out,
                                                archiveMode);

    return out.write(pendingStateJson.toRawUTF8(),
                     pendingStateJson.getNumBytesAsUTF8());
  }

  return false;
}

std::uint64_t PitchNetAudioProcessor::computePersistentStateKey(
    const juce::String &parametersXml, bool hostBackedARA) const {
  const Project *project = projectForPersistentState();
  if (project == nullptr)
    return 0;

  std::uint64_t hash = 0xcbf29ce484222325ull;

  const auto hashBytes = [&hash](const void *data, size_t bytes) {
    const auto *cursor = static_cast<const unsigned char *>(data);
    size_t i = 0;
    for (; i + sizeof(std::uint64_t) <= bytes; i += sizeof(std::uint64_t)) {
      std::uint64_t word = 0;
      std::memcpy(&word, cursor + i, sizeof(word));
      hash = (hash ^ word) * 0x100000001b3ull;
    }
    for (; i < bytes; ++i)
      hash = (hash ^ cursor[i]) * 0x100000001b3ull;
  };
  const auto hashValue = [&hash](std::uint64_t value) {
    hash = (hash ^ value) * 0x100000001b3ull;
  };
  const auto hashFloats = [&hashValue,
                           &hashBytes](const std::vector<float> &values) {
    hashValue(values.size());
    if (!values.empty())
      hashBytes(values.data(), values.size() * sizeof(float));
  };
  const auto hashBools = [&hashValue](const std::vector<bool> &values) {
    hashValue(values.size());
    for (bool value : values)
      hashValue(value ? 1u : 0u);
  };

  hashValue(hostBackedARA ? 1u : 0u);
  hashBytes(parametersXml.toRawUTF8(), parametersXml.getNumBytesAsUTF8());

  // Everything the archive's metadata carries - global parameters, note
  // positions, gains, macros, and the modification-time region span - in
  // exactly the shape toBinaryArchive() writes it, so this cannot drift as
  // fields are added.
  const auto metadataJson = juce::JSON::toString(
      ProjectSerializer::toJson(*project, false, false), false);
  hashBytes(metadataJson.toRawUTF8(), metadataJson.getNumBytesAsUTF8());

  // The pitch curves are hashed in full: they are ~1MB for a long take and
  // every edit moves one of them. The waveform buffers are deliberately
  // fingerprinted by shape only. They are tens of MB, originalWaveform never
  // changes after capture, and the rendered waveform cannot change without a
  // resynthesis that moved the curves or metadata hashed above.
  const auto &audioData = project->getAudioData();
  hashValue(static_cast<std::uint64_t>(audioData.sampleRate));
  hashBytes(&audioData.timelineOffsetSeconds,
            sizeof(audioData.timelineOffsetSeconds));
  for (const auto *buffer : {&audioData.waveform, &audioData.originalWaveform}) {
    hashValue(static_cast<std::uint64_t>(buffer->getNumChannels()));
    hashValue(static_cast<std::uint64_t>(buffer->getNumSamples()));
  }

  // A self-contained archive writes every sample of BOTH buffers, so the key
  // has to represent their contents, not just their shape. Shape alone is not
  // enough: an asynchronous resynthesis can land after the edit that triggered
  // it, leaving curves and metadata identical across two saves while the
  // rendered samples differ - the second save would then be handed the first
  // one's blob and persist pre-render audio. Host-backed archives omit both
  // buffers, so they are not scanned there.
  //
  // This is a full memory scan, deliberately. It is still cheaper than the
  // re-archive it avoids, and the alternative - a revision counter bumped at
  // every waveform mutation - would be a correctness invariant that has to be
  // found and maintained at every future write site.
  if (!hostBackedARA) {
    for (const auto *buffer :
         {&audioData.waveform, &audioData.originalWaveform})
      for (int channel = 0; channel < buffer->getNumChannels(); ++channel)
        hashBytes(buffer->getReadPointer(channel),
                  static_cast<size_t>(buffer->getNumSamples()) * sizeof(float));
  }

  hashFloats(audioData.f0);
  hashFloats(audioData.rawF0);
  hashFloats(audioData.cleanedF0);
  hashFloats(audioData.denseF0);
  hashFloats(audioData.baseF0);
  hashFloats(audioData.basePitch);
  hashFloats(audioData.deltaPitch);
  hashBools(audioData.voicedMask);
  hashBools(audioData.vadMask);

  // toBinaryArchive() writes the segment chunks as their own binary section,
  // outside the metadata JSON hashed above, so they need hashing explicitly -
  // otherwise a re-segmentation could be served from the cache as unchanged.
  hashValue(audioData.segmentDebugChunks.size());
  for (const auto &chunk : audioData.segmentDebugChunks) {
    hashValue(static_cast<std::uint64_t>(chunk.chunkIndex));
    hashValue(static_cast<std::uint64_t>(chunk.startFrame));
    hashValue(static_cast<std::uint64_t>(chunk.endFrame));
    hashValue(static_cast<std::uint64_t>(chunk.shortRestThreshold));
    hashValue(chunk.events.size());
    for (const auto &event : chunk.events) {
      hashValue(static_cast<std::uint64_t>(event.startFrame));
      hashValue(static_cast<std::uint64_t>(event.endFrame));
      hashValue(static_cast<std::uint64_t>(event.attachedStartFrame));
      hashBytes(&event.midiNote, sizeof(event.midiNote));
      hashValue(event.isRest ? 1u : 0u);
      hashBytes(&event.durationSeconds, sizeof(event.durationSeconds));
      hashValue(static_cast<std::uint64_t>(event.durationFrames));
    }
  }

  for (const auto &note : project->getNotes()) {
    hashFloats(note.getOriginalDeltaPitch());
    hashFloats(note.getDeltaPitch());
    hashFloats(note.getBakedDeltaPitch());
    hashFloats(note.getF0Values());
  }

  // 0 is the "nothing to cache" sentinel.
  return hash == 0 ? 1 : hash;
}

const juce::String &PitchNetAudioProcessor::getAraAnalysisProjectJson() const {
  if (!araAnalysisProjectJsonValid) {
    araAnalysisProjectJson =
        araAnalysisProjectSnapshot
            ? juce::JSON::toString(
                  ProjectSerializer::toJson(*araAnalysisProjectSnapshot), false)
            : juce::String();
    araAnalysisProjectJsonValid = true;
  }
  return araAnalysisProjectJson;
}

void PitchNetAudioProcessor::invalidateAraAnalysisProjectJson() {
  araAnalysisProjectJson.clear();
  araAnalysisProjectJsonValid = false;
}

void PitchNetAudioProcessor::setAraAnalysisProjectJson(juce::String json) {
  araAnalysisProjectJson = std::move(json);
  araAnalysisProjectJsonValid = true;
}

bool PitchNetAudioProcessor::restoreProjectJsonToProcessorState(
    const juce::String &projectJson) {
  if (projectJson.isEmpty())
    return false;

  auto parsed = juce::JSON::parse(projectJson);
  if (!parsed.isObject())
    return false;

  auto restoredProject = std::make_unique<Project>();
  if (!ProjectSerializer::fromJson(*restoredProject, parsed))
    return false;
  adoptMacroParameters(*restoredProject);

  araAnalysisTimelineOffsetSeconds =
      restoredProject->getAudioData().timelineOffsetSeconds;
  // The JSON this project was restored from, so
  // there is no need to rebuild it from the
  // snapshot on the next read.
  setAraAnalysisProjectJson(projectJson);
  araAnalysisProjectSnapshot = std::make_unique<Project>(*restoredProject);
  araAnalysisLoading = false;
  araAnalysisReady =
      restoredProject->getAudioData().waveform.getNumSamples() > 0 &&
      !restoredProject->getAudioData().f0.empty();

  if (!araAnalysisController)
    araAnalysisController = std::make_unique<EditorController>(false);
  araAnalysisController->setProject(std::move(restoredProject));
  if (araAnalysisProjectSnapshot)
    publishPersistentProjectSnapshot(*araAnalysisProjectSnapshot);
  return true;
}

bool PitchNetAudioProcessor::restorePersistentProjectState(
    const void *data, size_t sizeInBytes) {
  if (!data || sizeInBytes == 0)
    return false;

  auto restoredProject = std::make_unique<Project>();
  if (ProjectSerializer::fromBinaryArchive(*restoredProject, data,
                                            sizeInBytes)) {
    adoptMacroParameters(*restoredProject);
    pendingStateJson.clear();
    araAnalysisTimelineOffsetSeconds =
        restoredProject->getAudioData().timelineOffsetSeconds;
    invalidateAraAnalysisProjectJson();
    araAnalysisProjectSnapshot = std::make_unique<Project>(*restoredProject);
    araAnalysisLoading = false;
    araAnalysisReady =
        restoredProject->getAudioData().waveform.getNumSamples() > 0 &&
        !restoredProject->getAudioData().f0.empty();
    cachedPitchOffset = restoredProject->getGlobalPitchOffset();
    cachedFormantShift = restoredProject->getFormantShift();

    if (!araAnalysisController)
      araAnalysisController = std::make_unique<EditorController>(false);
    araAnalysisController->setProject(
        std::make_unique<Project>(*restoredProject));
    publishPersistentProjectSnapshot(*restoredProject);

    if (mainComponent) {
      mainComponent->restoreProjectSnapshot(*restoredProject);
      mainComponent->bindRealtimeProcessor(realtimeProcessor);
    } else {
      // Loaded with the UI closed: bind the realtime processor to the restored
      // snapshot so ARA playback works without ever opening the editor. The
      // document-controller pointer is established in didBindToARA().
      bindRealtimeProcessorHeadless();
    }
    return true;
  }

  juce::String projectJson(
      juce::CharPointer_UTF8(static_cast<const char *>(data)), sizeInBytes);
  if (!restoreProjectJsonToProcessorState(projectJson))
    return false;

  pendingStateJson = projectJson;

  if (mainComponent && mainComponent->restoreProjectJson(projectJson)) {
    pendingStateJson.clear();
    if (auto *project = mainComponent->getProject()) {
      cachedPitchOffset = project->getGlobalPitchOffset();
      cachedFormantShift = project->getFormantShift();
      araAnalysisProjectSnapshot = std::make_unique<Project>(*project);
      invalidateAraAnalysisProjectJson();
      publishPersistentProjectSnapshot(*project);
    }
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
  } else if (araAnalysisProjectSnapshot) {
    // JSON state can also be restored before an editor is ever opened.
    bindRealtimeProcessorHeadless();
  }

  return true;
}

// ============================================================================
// Editor Connection
// ============================================================================

void PitchNetAudioProcessor::setMainComponent(IMainView *mc) {
  if (mainComponent != nullptr && mainComponent != mc) {
    if (auto *project = mainComponent->getProject())
      updateProjectStateFromEditor(*project);
    viewportState = mainComponent->getViewportState();
#if JucePlugin_Enable_ARA
    if (canvasShowsActiveAraRegion && activeRegionKey.isNotEmpty())
      araRegions[activeRegionKey].project =
          mainComponent->exchangeProject(nullptr);
#endif
    mainComponent->bindUndoManager(nullptr);
    mainComponent->bindBackendController(nullptr);
  }

  // A new (or no) canvas starts without the active region's project loaded.
  if (mainComponent != mc)
    canvasShowsActiveAraRegion = false;

  mainComponent = mc;
  if (mc) {
    if (!araAnalysisController)
      araAnalysisController = std::make_unique<EditorController>(false);
    mc->bindBackendController(araAnalysisController.get());
    mc->bindUndoManager(undoManager.get());
    mc->bindRealtimeProcessor(realtimeProcessor);

    bool restoredPersistentProject = false;
#if JucePlugin_Enable_ARA
    if (regionCanvasAnalysisPending.load()) {
      // An ARA host may replace the editor while region analysis is running.
      // Recreate the same empty/modal state in the replacement editor and let
      // subsequent progress callbacks target this current binding.
      auto displaced = mc->exchangeProject(nullptr);
      juce::ignoreUnused(displaced);
      mc->setStatusMessage(TR("progress.analyzing"));
      mc->showAnalysisProgress(0.0);
      restoredPersistentProject = true;
    } else if (activeRegionKey.isNotEmpty()) {
      auto activeIt = araRegions.find(activeRegionKey);
      if (activeIt != araRegions.end() && activeIt->second.project) {
        auto displaced =
            mc->exchangeProject(std::move(activeIt->second.project));
        juce::ignoreUnused(displaced);
        mc->bindUndoManager(activeIt->second.ensureUndoManager());
        mc->bindRealtimeProcessor(realtimeProcessor);
        canvasShowsActiveAraRegion = true;
        restoredPersistentProject = true;
      }
    }
#endif
    if (!restoredPersistentProject && pendingStateJson.isNotEmpty() &&
        mc->restoreProjectJson(pendingStateJson)) {
      pendingStateJson.clear();
      restoredPersistentProject = true;
    } else if (!restoredPersistentProject && araAnalysisProjectSnapshot) {
      restoredPersistentProject =
          mc->restoreProjectSnapshot(*araAnalysisProjectSnapshot);
    } else if (!restoredPersistentProject &&
               getAraAnalysisProjectJson().isNotEmpty()) {
      restoredPersistentProject =
          mc->restoreProjectJson(getAraAnalysisProjectJson());
    }

    // Sync current APVTS parameter values to project
    if (auto *project = mc->getProject()) {
      if (restoredPersistentProject)
        adoptMacroParameters(*project);
      else
        attachMacroParameters(*project);

      const float po = pitchOffsetParamValue->load();
      const float fs = formantShiftParamValue->load();
      if (std::abs(po) > 0.001f)
        project->setGlobalPitchOffset(po);
      if (std::abs(fs) > 0.001f)
        project->setFormantShift(fs);

      if (restoredPersistentProject) {
        apvts.getParameter(PARAM_PITCH_OFFSET)
            ->setValueNotifyingHost(apvts.getParameter(PARAM_PITCH_OFFSET)
                                       ->convertTo0to1(
                                           project->getGlobalPitchOffset()));
        apvts.getParameter(PARAM_FORMANT_SHIFT)
            ->setValueNotifyingHost(apvts.getParameter(PARAM_FORMANT_SHIFT)
                                       ->convertTo0to1(
                                           project->getFormantShift()));
        araAnalysisProjectSnapshot = std::make_unique<Project>(*project);
        invalidateAraAnalysisProjectJson();
        araAnalysisReady =
            project->getAudioData().waveform.getNumSamples() > 0 &&
            !project->getAudioData().f0.empty();
        publishPersistentProjectSnapshot(*project);
      }
    }

    mc->restoreViewportState(viewportState);

#if JucePlugin_Enable_ARA
    // The active region is normally set before the editor exists, so every
    // push on that path found mainComponent null and returned without doing
    // anything. Push once here so the ruler lines up with the host timeline
    // as soon as the canvas opens, rather than only after the clip is moved.
    pushTimelineDisplayOffset();
#endif
  } else {
    // The editor is closing, but ARA playback/bounce must keep working
    // headlessly. Re-point the realtime processor at the persistent backend
    // project snapshot (which holds the edited, synthesized waveform) and the
    // processor-owned vocoder, instead of nulling it and falling back to the
    // raw, unedited ARA source (which would lose edits and buzz from per-block
    // resampling).
    if (araAnalysisProjectSnapshot) {
      bindRealtimeProcessorHeadless();
    } else {
      realtimeProcessor.setProject(nullptr);
    }
  }
}

juce::AudioProcessorEditor *PitchNetAudioProcessor::createEditor() {
  return new PitchNetAudioProcessorEditor(*this);
}

// ============================================================================
// State Save / Load (versioned envelope)
// ============================================================================

void PitchNetAudioProcessor::getStateInformation(
    juce::MemoryBlock &destData) {
  destData.setSize(0);
  juce::MemoryOutputStream out(destData, false);

  auto apvtsState = apvts.copyState();
  auto apvtsXml = apvtsState.createXml();
  const juce::String parametersXml = apvtsXml ? apvtsXml->toString()
                                              : juce::String();

  bool hostBackedARA = false;
#if JucePlugin_Enable_ARA
  // When this processor is attached to an ARA document, the host already owns
  // the immutable source and the ARA object stream persists ProcessedRegionData
  // for edited playback. Keep ordinary/non-ARA plugin state self-contained.
  hostBackedARA = araDocumentController != nullptr;
#endif

  // Hosts ask for plug-in state far more often than the project changes: every
  // project save, every track duplicate, editor close and offline bounce.
  // Re-archiving a multi-minute take each time is what makes saving stall, so
  // hand back the previous blob when nothing moved. The key is a content
  // fingerprint rather than Project::isModified(), which not every edit path
  // sets - a missed invalidation here would silently persist stale audio.
  const auto stateKey = computePersistentStateKey(parametersXml, hostBackedARA);
  if (stateKey != 0 && stateKey == cachedPluginStateKey &&
      cachedPluginStateBlock.getSize() > 0) {
    destData = cachedPluginStateBlock;
    return;
  }

  // Write the archive straight into the host's block and back-patch its length
  // rather than staging it in a second MemoryBlock first, which doubled both
  // the peak footprint and the copying.
  out.writeInt(static_cast<int>(kPluginStateMagic));
  out.writeInt(kPluginStateBinaryVersion);
  writeStateString(out, parametersXml);

  const auto lengthFieldPosition = out.getPosition();
  out.writeInt64(0);
  const auto archiveStart = out.getPosition();
  serializePersistentProjectState(out, hostBackedARA);
  const auto archiveEnd = out.getPosition();

  out.setPosition(lengthFieldPosition);
  out.writeInt64(archiveEnd - archiveStart);
  out.setPosition(archiveEnd);
  out.flush();

  cachedPluginStateKey = stateKey;
  cachedPluginStateBlock = destData;
}

void PitchNetAudioProcessor::setStateInformation(const void *data,
                                                   int sizeInBytes) {
  if (!data || sizeInBytes <= 0)
    return;

  const auto clearUndoHistories = [this]() {
    // Incoming state replaces whatever this processor held, so the blob
    // cached for the previous project must not outlive it - a later save
    // could otherwise hand the host the state we just replaced.
    cachedPluginStateKey = 0;
    cachedPluginStateBlock.reset();

    undoManager->clear();
    for (auto &[regionKey, regionState] : araRegions) {
      juce::ignoreUnused(regionKey);
      if (regionState.undoManager)
        regionState.undoManager->clear();
    }
    araRegions.clear();
#if JucePlugin_Enable_ARA
    activeRegionKey.clear();
  activeRegionSelector.clear();
    activeModification = nullptr;
    canvasShowsActiveAraRegion = false;
#endif
    if (mainComponent)
      mainComponent->bindUndoManager(undoManager.get());
  };

  {
    juce::MemoryInputStream in(data, static_cast<size_t>(sizeInBytes), false);
    if (static_cast<std::uint32_t>(in.readInt()) == kPluginStateMagic) {
      if (in.readInt() != kPluginStateBinaryVersion)
        return;

      clearUndoHistories();

      auto parametersXml = readStateString(in);
      if (parametersXml.isNotEmpty()) {
        auto xml = juce::parseXML(parametersXml);
        if (xml) {
          auto tree = juce::ValueTree::fromXml(*xml);
          if (tree.isValid())
            apvts.replaceState(tree);
        }
      }

      const auto projectBytes = in.readInt64();
      if (projectBytes > 0 &&
          projectBytes <= std::numeric_limits<int>::max()) {
        juce::MemoryBlock projectArchive(static_cast<size_t>(projectBytes));
        if (in.read(projectArchive.getData(), static_cast<int>(projectBytes)) ==
            projectBytes)
          restorePersistentProjectState(projectArchive.getData(),
                                        projectArchive.getSize());
      }
      return;
    }
  }

  juce::String rawString(
      juce::CharPointer_UTF8(static_cast<const char *>(data)),
      static_cast<size_t>(sizeInBytes));

  auto parsed = juce::JSON::parse(rawString);
  if (!parsed.isObject())
    return;

  clearUndoHistories();

  // Check if this is a versioned envelope or legacy project JSON
  if (parsed.hasProperty("pluginStateVersion")) {
    // New versioned format
    // Restore APVTS parameters
    auto parametersXml = parsed.getProperty("parametersXml", "").toString();
    if (parametersXml.isNotEmpty()) {
      auto xml = juce::parseXML(parametersXml);
      if (xml) {
        auto tree = juce::ValueTree::fromXml(*xml);
        if (tree.isValid())
          apvts.replaceState(tree);
      }
    }

    // Restore project state
    auto projectState = parsed.getProperty("projectState", {});
    if (projectState.isObject()) {
      auto projectJson = juce::JSON::toString(projectState, false);
      if (restorePersistentProjectState(projectJson.toRawUTF8(),
                                        projectJson.getNumBytesAsUTF8())) {
        // Sync project values to cached state
        if (auto *project =
                mainComponent ? mainComponent->getProject()
                              : araAnalysisController->getProject()) {
          cachedPitchOffset = project->getGlobalPitchOffset();
          cachedFormantShift = project->getFormantShift();
        }
        return;
      }
    }
  } else {
    // Legacy format: raw project JSON (backward compatibility)
    if (restorePersistentProjectState(rawString.toRawUTF8(),
                                      rawString.getNumBytesAsUTF8())) {
      // Sync legacy project values to APVTS
      if (auto *project =
              mainComponent ? mainComponent->getProject()
                            : araAnalysisController->getProject()) {
        apvts.getParameter(PARAM_PITCH_OFFSET)
            ->setValueNotifyingHost(apvts.getParameter(PARAM_PITCH_OFFSET)
                                       ->convertTo0to1(
                                           project->getGlobalPitchOffset()));
        apvts.getParameter(PARAM_FORMANT_SHIFT)
            ->setValueNotifyingHost(apvts.getParameter(PARAM_FORMANT_SHIFT)
                                       ->convertTo0to1(
                                           project->getFormantShift()));
        cachedPitchOffset = project->getGlobalPitchOffset();
        cachedFormantShift = project->getFormantShift();
      }
      return;
    }
  }
}

// ============================================================================
// Plugin Filter Factory
// ============================================================================

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new PitchNetAudioProcessor();
}

#if JucePlugin_Enable_ARA

const ARA::ARAFactory *JUCE_CALLTYPE createARAFactory() {
  return juce::ARADocumentControllerSpecialisation::createARAFactory<
      PitchNetDocumentController>();
}

std::unique_ptr<Project> PitchNetAudioProcessor::copyAraRegionProject(
    const juce::String &key) const {
  if (key == activeRegionKey && canvasShowsActiveAraRegion && mainComponent)
    if (auto *project = mainComponent->getProject())
      return std::make_unique<Project>(*project);
  const auto it = araRegions.find(key);
  return it != araRegions.end() && it->second.project
             ? std::make_unique<Project>(*it->second.project) : nullptr;
}

void PitchNetAudioProcessor::installAraRegionProject(
    juce::ARAPlaybackRegion *region, std::unique_ptr<Project> project) {
  const auto key = pitchnetRegionKey(*region);
  if (pendingRegionCanvasAnalysisKey == key) {
    // Host edits can finish while the editor is closed. Installing preserved
    // state must retire a pending job even when no canvas can be attached.
    regionCanvasAnalysisGeneration.fetch_add(1);
    if (regionCanvasController)
      regionCanvasController->requestCancelLoading();
    pendingRegionCanvasAnalysisKey.clear();
    regionCanvasAnalysisPending.store(false);
  }
  auto &state = araRegions[key];
  ++state.revision;
  // Old undo actions hold pointers to the previous notes and must not survive
  // replacement of that project. The host owns undo of region split/glue.
  state.ensureUndoManager()->clear();
  if (key == activeRegionKey && mainComponent && canvasShowsActiveAraRegion) {
    auto previous = mainComponent->exchangeProject(nullptr);
    canvasShowsActiveAraRegion = false;
  }
  state.project = std::move(project);
  if (key == activeRegionKey) {
    updateActiveAraRegionProperties(region);
    showAraRegionProjectIfActive(key);
  }
}

void PitchNetAudioProcessor::setActiveAraRegion(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr)
    return;

  const auto key = pitchnetRegionKey(*region);
  if (key.isEmpty())
    return;

  const auto selector = pitchnetRegionSelector(*region);

  if (key == activeRegionKey) {
    // Studio One Event FX can bind and select the playback region before the
    // editor exists. That headless call records activeRegionKey but cannot
    // attach a Project or request region-canvas analysis. When the editor later
    // selects the same region, resume the missing UI initialisation instead of
    // treating the matching key as a completed activation.
    const bool analysisAlreadyPendingForRegion =
        regionCanvasAnalysisPending.load() &&
        pendingRegionCanvasAnalysisKey == key;
    if (mainComponent == nullptr || canvasShowsActiveAraRegion ||
        analysisAlreadyPendingForRegion) {
      // Same modification, so ownership is unchanged and the Project stays
      // bound - but a different window onto it needs its placement refreshed,
      // or the ruler, playhead and stamped span keep describing the sibling
      // that was selected before.
      if (selector != activeRegionSelector) {
        activeRegionSelector = selector;
        updateActiveAraRegionProperties(region);
      }
      return;
    }
  }

  // Return the outgoing region's actual Project to its store. Undo actions
  // retain pointers into this object, so copying a snapshot here would leave
  // that region's history pointing at destroyed notes and F0 arrays.
  if (activeRegionKey.isNotEmpty() && mainComponent != nullptr &&
      canvasShowsActiveAraRegion) {
    auto outgoing = mainComponent->exchangeProject(nullptr);
    if (outgoing) {
      stampActiveRegionSpan(*outgoing);
      araRegions[activeRegionKey].project = std::move(outgoing);
    }
  }

  activeRegionKey = key;
  activeRegionSelector = selector;
  activeModification = region->getAudioModification<PitchNetAudioModification>();
  activeRegionStartSeconds = std::max(0.0, region->getStartInPlaybackTime());
  activeRegionEndSeconds = std::max(activeRegionStartSeconds,
                                    region->getEndInPlaybackTime());
  activeStartSampleInModification =
      region->getStartInAudioModificationSamples();

  auto &incomingState = araRegions[key];
  auto *incomingUndoManager = incomingState.ensureUndoManager();
  if (mainComponent != nullptr)
    mainComponent->bindUndoManager(incomingUndoManager);

  // Show the incoming region's project. A project analysed in this session is
  // ready immediately. A restored ARA archive has all analysis/edit data but
  // deliberately lacks project waveforms and mel, so it takes the source-read
  // path below to rebuild them without neural analysis.
  if (mainComponent != nullptr) {
    auto it = araRegions.find(key);
    if ((it == araRegions.end() || !it->second.project) &&
        activeModification != nullptr) {
      juce::MemoryBlock archive;
      if (activeModification->copyProjectArchive(archive)) {
        restoreAraRegionProject(key, archive.getData(), archive.getSize());
        // Restoring the archive can synchronously hydrate and move the Project
        // into the canvas. Its map slot is then empty by design, not evidence
        // that anything further needs loading.
        if (canvasShowsActiveAraRegion && mainComponent->getProject() != nullptr)
          return;
        it = araRegions.find(key);
      }
    }
    // The archived-key fallback that stood here loaded state filed under the
    // modification's persistent ID when the live lookup missed. Restored state
    // is now filed under the live key in the first place, so a miss here means
    // there is genuinely nothing to show and analysis should run.

    const bool needsSourceHydration =
        it != araRegions.end() && it->second.project &&
        araRegionProjectNeedsSourceHydration(key);
    if (it != araRegions.end() && it->second.project &&
        !needsSourceHydration) {
      regionCanvasAnalysisGeneration.fetch_add(1);
      if (regionCanvasController)
        regionCanvasController->requestCancelLoading();
      pendingRegionCanvasAnalysisKey.clear();
      regionCanvasAnalysisPending.store(false);
      attachMacroParameters(*it->second.project);
      auto displaced =
          mainComponent->exchangeProject(std::move(it->second.project));
      juce::ignoreUnused(displaced);
      mainComponent->updateHostAudioTimelineOffset(0.0);
      pushTimelineDisplayOffset();
      if (auto *shown = mainComponent->getProject())
        stampActiveRegionSpan(*shown);
      mainComponent->bindRealtimeProcessor(realtimeProcessor);
      canvasShowsActiveAraRegion = true;
      mainComponent->hideAnalysisProgress();
    } else if (araDocumentController != nullptr) {
      // The host selected a region that either has no project yet or has a
      // restored project waiting for its pristine ARA source. Do not expose a
      // partially hydrated project to editing.
      if (mainComponent->getProject() != nullptr) {
        auto displaced = mainComponent->exchangeProject(nullptr);
        juce::ignoreUnused(displaced);
      }
      canvasShowsActiveAraRegion = false;
      pendingRegionCanvasAnalysisKey = key;
      regionCanvasAnalysisPending.store(true);
      mainComponent->setStatusMessage(TR("progress.analyzing"));
      mainComponent->showAnalysisProgress(0.0);
      // requestRegionCanvasAnalysis() hydrates restored state when possible,
      // otherwise it starts analysis. It may have to wait for sample access.
      araDocumentController->requestRegionCanvasAnalysis(region);
    }
  }
}

std::pair<double, double>
PitchNetAudioProcessor::activeRegionSpanInModificationTime() const {
  auto *source = activeModification != nullptr
                     ? activeModification->getAudioSource()
                     : nullptr;
  const double rate = source != nullptr ? source->getSampleRate() : 0.0;
  const double length =
      std::max(0.0, activeRegionEndSeconds - activeRegionStartSeconds);
  if (rate <= 0.0)
    return {0.0, length};
  const double start =
      static_cast<double>(activeStartSampleInModification) / rate;
  return {start, start + length};
}

void PitchNetAudioProcessor::pushTimelineDisplayOffset() const {
  if (mainComponent == nullptr)
    return;

  // regionStartInPlaybackTime - regionStartInModificationTime. Deliberately
  // unclamped: a region windowing the back of a take, placed near the start of
  // the timeline, legitimately yields a negative offset.
  const auto span = activeRegionSpanInModificationTime();
  const double offset =
      activeModification != nullptr ? activeRegionStartSeconds - span.first
                                    : 0.0;
  mainComponent->setTimelineDisplayOffset(offset);
}

void PitchNetAudioProcessor::stampActiveRegionSpan(Project &project) const {
  auto *source = activeModification != nullptr
                     ? activeModification->getAudioSource()
                     : nullptr;
  stampSpanInModificationTime(
      project, activeStartSampleInModification,
      std::max(0.0, activeRegionEndSeconds - activeRegionStartSeconds),
      source != nullptr ? source->getSampleRate() : 0.0);
}

void PitchNetAudioProcessor::updateActiveAraRegionProperties(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr)
    return;

  const auto key = pitchnetRegionKey(*region);
  const auto newStart = std::max(0.0, region->getStartInPlaybackTime());
  if (key.isEmpty() || key != activeRegionKey)
    return;
  // Siblings share the modification key, so the key alone would let an
  // unselected sibling's move or resize overwrite the selected region's
  // placement - and reselecting the selected region then returns early in
  // setActiveAraRegion(), leaving the ruler and seek mapping on the sibling.
  // Only the selected window updates the placement.
  if (activeRegionSelector.isNotEmpty() &&
      pitchnetRegionSelector(*region) != activeRegionSelector)
    return;

  activeModification = region->getAudioModification<PitchNetAudioModification>();
  activeRegionStartSeconds = newStart;
  activeRegionEndSeconds =
      std::max(activeRegionStartSeconds, region->getEndInPlaybackTime());
  activeStartSampleInModification =
      region->getStartInAudioModificationSamples();

  if (mainComponent != nullptr && canvasShowsActiveAraRegion) {
    mainComponent->updateHostAudioTimelineOffset(0.0);
    pushTimelineDisplayOffset();

    if (auto *project = mainComponent->getProject()) {
      stampRegionSpanInModificationTime(*project, region);

      publishPersistentProjectSnapshot(*project);

      if (araAnalysisReady)
        mainComponent->bindRealtimeProcessor(realtimeProcessor);
    }
  } else if (auto it = araRegions.find(key);
             it != araRegions.end() && it->second.project) {
    stampRegionSpanInModificationTime(*it->second.project, region);
  }
}

void PitchNetAudioProcessor::analyzeAraRegionForCanvas(
    const juce::String &regionKey, PitchNetAudioModification *modification,
    juce::int64 startSampleInModification, double timelineOffsetSeconds,
    const juce::AudioBuffer<float> &buffer, double sampleRate) {
  if (regionKey.isEmpty() || buffer.getNumSamples() <= 0 || sampleRate <= 0.0)
    return;

  // No audio is published at analysis time (unedited regions play their raw
  // source), but the editable analysis project is cached on the ARA
  // modification so Cubase can archive it using the same object-level model as
  // JUCE's ARAPluginDemo/VocalNet.
  if (!regionCanvasController)
    regionCanvasController = std::make_unique<EditorController>(false);

  const auto analysisGeneration =
      regionCanvasAnalysisGeneration.fetch_add(1) + 1;
  pendingRegionCanvasAnalysisKey = regionKey;
  regionCanvasAnalysisPending.store(true);

  if (mainComponent) {
    mainComponent->setStatusMessage(TR("progress.analyzing"));
    mainComponent->showAnalysisProgress(0.0);
  }

  regionCanvasController->setHostAudioAsync(
      buffer, sampleRate,
      [this, analysisGeneration](double progress, const juce::String &msg) {
        if (regionCanvasAnalysisGeneration.load() != analysisGeneration)
          return;
        juce::MessageManager::callAsync(
            [this, analysisGeneration, progress, msg]() {
              if (regionCanvasAnalysisGeneration.load() !=
                      analysisGeneration ||
                  !regionCanvasAnalysisPending.load() ||
                  mainComponent == nullptr)
                return;

              // Resolve the current editor only after reaching the message
              // thread. The editor that launched this job may have been
              // replaced by the host, but mainComponent now points at the live
              // replacement and cannot be destroyed concurrently here.
              mainComponent->setStatusMessage(msg);
              mainComponent->showAnalysisProgress(progress);
            });
      },
      [this, regionKey, modification, startSampleInModification,
       analysisGeneration, sampleRate,
       timelineOffsetSeconds](const juce::AudioBuffer<float> &) {
        if (!regionCanvasController ||
            regionCanvasAnalysisGeneration.load() != analysisGeneration)
          return;
        auto *project = regionCanvasController->getProject();
        if (!project)
          return;

        // Analysis is modification-scoped now: the project covers the
        // whole modification and the caller passes zero for the offset.
        juce::ignoreUnused(timelineOffsetSeconds);
        // Length, not absolute end: the span starts at the region's offset
        // into the modification, so pass what remains of the take from there.
        stampSpanInModificationTime(
            *project, startSampleInModification,
            std::max(0.0, project->getAudioData().getDuration() -
                              static_cast<double>(startSampleInModification) /
                                  std::max(1.0, sampleRate)),
            sampleRate);

        attachMacroParameters(*project);

        // Cache this region's analysis so re-selecting it is instant and its
        // edits persist across switches.
        auto &regionState = araRegions[regionKey];
        regionState.project = std::make_unique<Project>(*project);
        auto *regionUndoManager = regionState.ensureUndoManager();
        regionUndoManager->clear();
        const bool completedPendingRegion =
            pendingRegionCanvasAnalysisKey == regionKey;

        // State/editor reconstruction can clear activeRegionKey while leaving
        // this exact region analysis pending.  In that case the pending key is
        // still the authoritative selection, so restore its active placement
        // before deciding whether to attach the completed Project.
        if (completedPendingRegion && activeRegionKey.isEmpty()) {
          activeRegionKey = regionKey;
          activeModification = modification;
          // Placement is the live region's, in host time, exactly as
          // setActiveAraRegion() records it. The analysis offset is always
          // zero and the project spans the whole modification, so neither can
          // stand in for the region's position or length.
          juce::ARAPlaybackRegion *placement = nullptr;
          if (araDocumentController != nullptr) {
            auto *current = araDocumentController->getCurrentPlaybackRegion();
            if (current != nullptr && pitchnetRegionKey(*current) == regionKey)
              placement = current;
          }
          if (placement == nullptr && modification != nullptr) {
            const auto &regions =
                modification->getPlaybackRegions<juce::ARAPlaybackRegion>();
            if (!regions.empty())
              placement = regions.front();
          }
          if (placement != nullptr) {
            activeRegionSelector = pitchnetRegionSelector(*placement);
            activeRegionStartSeconds =
                std::max(0.0, placement->getStartInPlaybackTime());
            activeRegionEndSeconds = std::max(
                activeRegionStartSeconds, placement->getEndInPlaybackTime());
            activeStartSampleInModification =
                placement->getStartInAudioModificationSamples();
          } else {
            activeRegionStartSeconds = 0.0;
            activeRegionEndSeconds = project->getAudioData().getDuration();
            activeStartSampleInModification = startSampleInModification;
          }
        }

        // Paint it onto the canvas if it is still the active region.
        if (regionKey == activeRegionKey && mainComponent) {
          // The canvas is in modification time; the placement is host time.
          const auto latestSpan = activeRegionSpanInModificationTime();
          auto displaced = mainComponent->exchangeProject(
              std::move(araRegions[regionKey].project));
          juce::ignoreUnused(displaced);
          mainComponent->bindUndoManager(regionUndoManager);
          // The host may move the region while analysis is running. The
          // analyzed Project is anchored at the position captured at launch;
          // repad every waveform/F0/note array to the latest host position
          // before exposing it, so content and boundary move together.
          mainComponent->updateHostAudioTimelineOffset(0.0);
          pushTimelineDisplayOffset();
          if (auto *positionedProject = mainComponent->getProject())
            stampActiveRegionSpan(*positionedProject);
          mainComponent->bindRealtimeProcessor(realtimeProcessor);
          canvasShowsActiveAraRegion = true;

          // Once the newly analysed project is on the canvas, bring its ARA
          // region into view instead of leaving the viewport at the prior one.
          mainComponent->focusTimelineRange(latestSpan.first, latestSpan.second);
        }

        if (modification != nullptr) {
          juce::MemoryBlock projectArchive;
          if (serializeAraRegionProject(regionKey, projectArchive))
            modification->setProjectArchive(projectArchive.getData(),
                                            projectArchive.getSize());
        }

        // No audio is published on analysis: an unedited region plays its raw
        // ARA source (VocalNet's model). resynth-on-edit publishes processed
        // audio only for regions the user actually edits — no full re-synthesis
        // of unchanged audio.
        if (completedPendingRegion) {
          pendingRegionCanvasAnalysisKey.clear();
          regionCanvasAnalysisPending.store(false);
        }
        if (mainComponent &&
            (regionKey == activeRegionKey || completedPendingRegion))
          mainComponent->hideAnalysisProgress();
      });
}

void PitchNetAudioProcessor::releaseAraModificationCanvas(
    PitchNetAudioModification *modification) {
  if (modification == nullptr || activeModification != modification)
    return;

  if (mainComponent != nullptr && canvasShowsActiveAraRegion &&
      activeRegionKey.isNotEmpty()) {
    // Deactivation is reversible - redo can reactivate this same object - so
    // the edited project has to survive in the region cache. Discarding it here
    // lost every edit made since the last save whenever a track version was
    // deactivated.
    araRegions[activeRegionKey].project =
        mainComponent->exchangeProject(nullptr);
    mainComponent->bindUndoManager(undoManager.get());
  }

  activeModification = nullptr;
  activeRegionKey.clear();
  activeRegionSelector.clear();
  canvasShowsActiveAraRegion = false;
}

// The host is destroying the selected region. Its selector would outlive it,
// and the sibling guard in updateActiveAraRegionProperties() would then ignore
// every surviving sibling, leaving the ruler, seek mapping and highlighted span
// on the deleted slice. Nothing is selected afterwards, as when a host deletes
// a selected event; the edits stay with the modification.
void PitchNetAudioProcessor::forgetAraPlaybackRegion(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr || activeRegionSelector.isEmpty() ||
      pitchnetRegionSelector(*region) != activeRegionSelector)
    return;

  releaseAraModificationCanvas(
      region->getAudioModification<PitchNetAudioModification>());
}

void PitchNetAudioProcessor::forgetAraModification(
    PitchNetAudioModification *modification) {
  if (modification == nullptr)
    return;

  releaseAraModificationCanvas(modification);

  // Invalidate pending analysis BEFORE erasing. analyzeAraRegionForCanvas()
  // captures a raw PitchNetAudioModification* in its completion, so a callback
  // landing after this point would otherwise refile state under a dead key or
  // touch a destroyed object. This is the only destruction path, so bumping the
  // generation here is what makes that captured pointer safe: every completion
  // compares the generation before dereferencing.
  regionCanvasAnalysisGeneration.fetch_add(1);
  if (regionCanvasController)
    regionCanvasController->requestCancelLoading();
  pendingRegionCanvasAnalysisKey.clear();
  regionCanvasAnalysisPending.store(false);

  // Edit state is filed under the modification's live key - one entry, not a
  // prefixed family. This previously matched on a "<id>:" prefix left over from
  // the per-region key scheme, which the modification-owned key never matches,
  // so nothing was ever erased and every destroyed modification leaked its
  // Project and undo history. The object is still alive here, so its live key
  // is safe to read.
  const auto key = pitchnetModificationKey(*modification);
  const auto it = araRegions.find(key);
  if (it != araRegions.end()) {
    if (it->second.undoManager != nullptr)
      it->second.undoManager->clear();
    araRegions.erase(it);
  }
}

bool PitchNetAudioProcessor::serializeAraRegionProject(
    const juce::String &regionKey, juce::MemoryBlock &out) const {
  out.setSize(0);

  const Project *project = nullptr;
  if (regionKey == activeRegionKey && canvasShowsActiveAraRegion &&
      mainComponent != nullptr)
    project = mainComponent->getProject();
  if (project == nullptr) {
    const auto it = araRegions.find(regionKey);
    if (it != araRegions.end())
      project = it->second.project.get();
  }
  if (project == nullptr)
    return false;

  return ProjectSerializer::toBinaryArchive(
             *project, out,
             ProjectSerializer::BinaryArchiveMode::hostBackedARA) &&
         out.getSize() > 0;
}

bool PitchNetAudioProcessor::hasAraRegionProject(
    const juce::String &regionKey) const {
  if (regionKey == activeRegionKey && canvasShowsActiveAraRegion &&
      mainComponent != nullptr && mainComponent->getProject() != nullptr)
    return true;
  const auto it = araRegions.find(regionKey);
  return it != araRegions.end() && it->second.project != nullptr;
}

bool PitchNetAudioProcessor::araRegionProjectNeedsSourceHydration(
    const juce::String &regionKey) const {
  const Project *project = nullptr;
  if (regionKey == activeRegionKey && canvasShowsActiveAraRegion &&
      mainComponent != nullptr)
    project = mainComponent->getProject();
  if (project == nullptr) {
    const auto it = araRegions.find(regionKey);
    if (it != araRegions.end())
      project = it->second.project.get();
  }

  if (project == nullptr)
    return false;

  const auto &audioData = project->getAudioData();
  return audioData.waveform.getNumSamples() <= 0 ||
         audioData.originalWaveform.getNumSamples() <= 0 ||
         audioData.melSpectrogram.empty();
}


bool PitchNetAudioProcessor::hydrateAraRegionProject(
    const juce::String &regionKey,
    const juce::AudioBuffer<float> &sourceBuffer,
    double sourceSampleRate) {
  if (regionKey.isEmpty() || sourceBuffer.getNumChannels() <= 0 ||
      sourceBuffer.getNumSamples() <= 0 || sourceSampleRate <= 0.0)
    return false;

  Project *project = nullptr;
  if (regionKey == activeRegionKey && canvasShowsActiveAraRegion &&
      mainComponent != nullptr)
    project = mainComponent->getProject();
  if (project == nullptr) {
    const auto it = araRegions.find(regionKey);
    if (it != araRegions.end())
      project = it->second.project.get();
  }
  if (project == nullptr)
    return false;

  auto &audioData = project->getAudioData();

  // A freshly split clone arrives with everything already populated, so
  // hydration is a no-op for it.
  const bool takesEarlyOut = audioData.waveform.getNumSamples() > 0 &&
                             audioData.originalWaveform.getNumSamples() > 0 &&
                             !audioData.melSpectrogram.empty();
  if (takesEarlyOut)
    return true;

  // Region analysis is stored at PitchNet's working rate, which can differ
  // from the ARA source rate. Recreate the same resampled pristine buffer that
  // EditorController produced during the original analysis.
  const double projectSampleRate =
      audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                               : sourceSampleRate;
  juce::AudioBuffer<float> resampledSource;
  const juce::AudioBuffer<float> *hydratedSource = &sourceBuffer;
  if (!juce::approximatelyEqual(projectSampleRate, sourceSampleRate)) {
    resampledSource = AudioResampler::resample(
        sourceBuffer, sourceSampleRate, projectSampleRate);
    if (resampledSource.getNumSamples() <= 0)
      return false;
    hydratedSource = &resampledSource;
  }

  // A timeline-era length check lived here: it compared the end of
  // playbackRegionRanges against the source buffer length, on the assumption
  // that both ran from timeline zero. Ranges are now in modification time and
  // the buffer is always the whole modification, so that comparison only
  // passed when a region happened to end at the end of the take - and sent
  // every other region into a needless full re-analysis, discarding its edits.

  audioData.sampleRate = juce::roundToInt(projectSampleRate);
  if (audioData.originalWaveform.getNumSamples() <= 0)
    audioData.originalWaveform.makeCopyOf(*hydratedSource);

  if (audioData.melSpectrogram.empty()) {
    const auto &sourceWaveform = audioData.originalWaveform;
    MelSpectrogram melComputer(audioData.sampleRate, N_FFT, HOP_SIZE,
                               NUM_MELS, FMIN, FMAX);
    audioData.melSpectrogram = melComputer.compute(
        sourceWaveform.getReadPointer(0), sourceWaveform.getNumSamples());
    if (audioData.melSpectrogram.empty()) {
      ARA_DIAG("hydrateFailed key=" + regionKey + " reason=melComputeFailed");
      return false;
    }
  }

  // Prefer the separately persisted render as the authoritative waveform: it
  // already contains every saved edit, including global processing. Start from
  // the pristine source - the whole modification, in modification time, with no
  // timeline padding - and let the processed audio replace it where present.
  if (audioData.waveform.getNumSamples() <= 0) {
    audioData.waveform.makeCopyOf(*hydratedSource);

    if (regionKey == activeRegionKey && activeModification != nullptr) {
      juce::AudioBuffer<float> processedAudio;
      double processedRate = 0.0;
      juce::int64 processedStartInModification = 0;
      const bool hasProcessedAudio = activeModification->copyProcessedAudio(
          processedAudio, processedRate, processedStartInModification);

      // Processed audio is modification-scoped and stored at offset zero, so
      // it already IS the edited waveform for the whole take. The previous
      // timeline splice rebuilt the buffer starting at the region's offset
      // into the modification, which then got published at offset zero - so
      // every region read its own offset into an already-offset buffer and
      // played the wrong part of the take.
      if (hasProcessedAudio && processedAudio.getNumSamples() > 0) {
        jassert(processedStartInModification == 0);
        if (!juce::approximatelyEqual(processedRate, projectSampleRate)) {
          auto converted = AudioResampler::resample(
              processedAudio, processedRate, projectSampleRate);
          if (converted.getNumSamples() > 0)
            audioData.waveform.makeCopyOf(converted);
        } else {
          audioData.waveform.makeCopyOf(processedAudio);
        }
      }
    }
  }

  // Untouched regions and processed renders that no longer cover a
  // trimmed/extended region remain source-backed. There is intentionally no
  // per-note waveform reconstruction path: the composite render is now the
  // sole persisted edited-audio representation.
  if (pendingRegionCanvasAnalysisKey == regionKey) {
    pendingRegionCanvasAnalysisKey.clear();
    regionCanvasAnalysisPending.store(false);
  }
  return true;
}

bool PitchNetAudioProcessor::araRegionProjectAppearsToCover(
    const juce::String &regionKey, double regionStart, double regionEnd) const {
  const Project *project = nullptr;
  if (regionKey == activeRegionKey && canvasShowsActiveAraRegion &&
      mainComponent != nullptr)
    project = mainComponent->getProject();
  if (project == nullptr) {
    const auto it = araRegions.find(regionKey);
    if (it != araRegions.end())
      project = it->second.project.get();
  }
  return project != nullptr &&
         projectAppearsToCoverRegion(*project, regionStart, regionEnd);
}

bool PitchNetAudioProcessor::restoredAraProjectAppearsToCover(
    double regionStart, double regionEnd) const {
  return araAnalysisProjectSnapshot != nullptr &&
         projectAppearsToCoverRegion(*araAnalysisProjectSnapshot, regionStart,
                                     regionEnd);
}

bool PitchNetAudioProcessor::showAraRegionProjectIfActive(
    const juce::String &regionKey) {
  if (regionKey.isEmpty() || regionKey != activeRegionKey ||
      mainComponent == nullptr)
    return false;

  if (canvasShowsActiveAraRegion && mainComponent->getProject() != nullptr) {
    const auto &audioData = mainComponent->getProject()->getAudioData();
    return audioData.waveform.getNumSamples() > 0 &&
           audioData.originalWaveform.getNumSamples() > 0 &&
           !audioData.melSpectrogram.empty();
  }

  auto it = araRegions.find(regionKey);
  if (it == araRegions.end() || !it->second.project)
    return false;
  if (araRegionProjectNeedsSourceHydration(regionKey))
    return false;

  regionCanvasAnalysisGeneration.fetch_add(1);
  if (regionCanvasController)
    regionCanvasController->requestCancelLoading();
  if (pendingRegionCanvasAnalysisKey == regionKey)
    pendingRegionCanvasAnalysisKey.clear();
  regionCanvasAnalysisPending.store(false);
  auto *regionUndoManager = it->second.ensureUndoManager();
  attachMacroParameters(*it->second.project);
  auto displaced =
      mainComponent->exchangeProject(std::move(it->second.project));
  juce::ignoreUnused(displaced);
  mainComponent->bindUndoManager(regionUndoManager);
  mainComponent->updateHostAudioTimelineOffset(0.0);
  pushTimelineDisplayOffset();
  if (auto *shown = mainComponent->getProject())
    stampActiveRegionSpan(*shown);
  mainComponent->bindRealtimeProcessor(realtimeProcessor);
  canvasShowsActiveAraRegion = true;
  mainComponent->hideAnalysisProgress();
  {
    const auto span = activeRegionSpanInModificationTime();
    mainComponent->focusTimelineRange(span.first, span.second);
  }
  return true;
}

void PitchNetAudioProcessor::restoreAraRegionProject(const juce::String &regionKey,
                                                     const void *data,
                                                     size_t sizeInBytes) {
  ARA_DIAG("restoreProject key=" + regionKey + " bytes=" +
           juce::String((int)sizeInBytes) + " activeKey=" + activeRegionKey);
  if (regionKey.isEmpty() || data == nullptr || sizeInBytes == 0)
    return;

  auto installRestoredProject = [this, &regionKey](
                                    std::unique_ptr<Project> project) {
    attachMacroParameters(*project);

    // Restored state is filed under the live key by the document controller, so
    // there is no archived-to-live migration left to do. What stood here
    // matched an incoming archived key against the active region's archived key
    // and moved the Project across; both keys are now the same family.
    //
    // The active region may still have started a fresh analysis before its
    // archive arrived, so that analysis is invalidated below to stop its
    // original notes overwriting the restored edits.
    const juce::String liveKey =
        regionKey == activeRegionKey ? activeRegionKey : juce::String();

    auto &archivedState = araRegions[regionKey];
    archivedState.project = std::move(project);
    archivedState.ensureUndoManager()->clear();

    if (liveKey.isEmpty())
      return;

    regionCanvasAnalysisGeneration.fetch_add(1);
    if (regionCanvasController)
      regionCanvasController->requestCancelLoading();
    if (pendingRegionCanvasAnalysisKey == liveKey)
      pendingRegionCanvasAnalysisKey.clear();
    regionCanvasAnalysisPending.store(false);

    auto &liveState = araRegions[liveKey];
    auto *liveUndoManager = liveState.ensureUndoManager();
    liveUndoManager->clear();

    if (mainComponent == nullptr || liveKey != activeRegionKey)
      return;

    if (liveState.project != nullptr &&
        (liveState.project->getAudioData().waveform.getNumSamples() <= 0 ||
         liveState.project->getAudioData().originalWaveform.getNumSamples() <=
             0 ||
         liveState.project->getAudioData().melSpectrogram.empty())) {
      // Keep the restored analysis shell out of the editor until its immutable
      // source, source-derived mel, and rendered waveform have been rebuilt.
      // Otherwise an edit could synthesize without all required inputs.
      if (mainComponent->getProject() != nullptr) {
        auto displaced = mainComponent->exchangeProject(nullptr);
        juce::ignoreUnused(displaced);
      }
      mainComponent->bindUndoManager(liveUndoManager);
      canvasShowsActiveAraRegion = false;
      pendingRegionCanvasAnalysisKey.clear();
      regionCanvasAnalysisPending.store(false);
      mainComponent->setStatusMessage(TR("progress.analyzing"));
      mainComponent->showAnalysisProgress(0.0);

      if (araDocumentController != nullptr && activeModification != nullptr) {
        for (auto *region : activeModification->getPlaybackRegions<
                 juce::ARAPlaybackRegion>()) {
          if (region == nullptr || pitchnetRegionKey(*region) != liveKey)
            continue;
          pendingRegionCanvasAnalysisKey = liveKey;
          regionCanvasAnalysisPending.store(true);
          araDocumentController->setCurrentPlaybackRegion(region);
          araDocumentController->requestRegionCanvasAnalysis(region);
          break;
        }
      }
      return;
    }

    auto displaced =
        mainComponent->exchangeProject(std::move(liveState.project));
    juce::ignoreUnused(displaced);
    mainComponent->bindUndoManager(liveUndoManager);
    mainComponent->updateHostAudioTimelineOffset(0.0);
    pushTimelineDisplayOffset();
    if (auto *shown = mainComponent->getProject())
      stampActiveRegionSpan(*shown);
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
    canvasShowsActiveAraRegion = true;
    mainComponent->hideAnalysisProgress();
    {
      const auto span = activeRegionSpanInModificationTime();
      mainComponent->focusTimelineRange(span.first, span.second);
    }
  };

  auto project = std::make_unique<Project>();
  if (ProjectSerializer::fromBinaryArchive(*project, data, sizeInBytes) &&
      projectHasRestorableAnalysisData(*project)) {
    installRestoredProject(std::move(project));
    return;
  }

  const juce::String json(juce::CharPointer_UTF8(static_cast<const char *>(data)),
                          sizeInBytes);
  const auto parsed = juce::JSON::parse(json);
  if (!parsed.isObject())
    return;

  project = std::make_unique<Project>();
  if (ProjectSerializer::fromJson(*project, parsed) &&
      projectHasRestorableAnalysisData(*project))
    installRestoredProject(std::move(project));
}

void PitchNetAudioProcessor::attachMacroParameters(Project &project) {
  project.setMacroParameters(macroParameters);
}

void PitchNetAudioProcessor::adoptMacroParameters(Project &project) {
  if (const auto restored = project.getMacroParameters())
    *macroParameters = *restored;
  project.setMacroParameters(macroParameters);
}

void PitchNetAudioProcessor::setAraDocumentController(
    PitchNetDocumentController *dc) {
  if (araDocumentController != nullptr && araDocumentController != dc)
    araDocumentController->releaseOwningProcessor(this);

  araDocumentController = dc;
  if (dc) {
    dc->setOwningProcessor(this);
    dc->setRealtimeProcessor(&realtimeProcessor);
    if (araAnalysisProjectSnapshot)
      dc->setDocumentProjectSnapshot(*araAnalysisProjectSnapshot, false);
  }
}

void PitchNetAudioProcessor::publishPersistentProjectSnapshot(
    const Project &project) {
  if (araDocumentController)
    araDocumentController->setDocumentProjectSnapshot(project);
}

void PitchNetAudioProcessor::ensureHeadlessAraBinding() {
  // Establish the document-controller binding (not only when the editor opens)
  // so loading a saved project with the UI closed still wires the playback
  // renderer to the realtime processor and can restore project state.
  if (auto *pr = getPlaybackRenderer<PitchNetPlaybackRenderer>()) {
    if (auto *dc = pr->getDocController()) {
      setAraDocumentController(dc);

      // Own the persistence callbacks at the processor (capturing the
      // processor, which outlives the editor) so an ARA archive can be restored
      // headlessly. If restore data arrived earlier, the controller applies it
      // synchronously here.
      dc->setPersistenceCallbacks(
          [this](juce::MemoryBlock &destData) {
            return serializePersistentProjectState(destData, true);
          },
          [this](const void *data, size_t sizeInBytes) {
            return restorePersistentProjectState(data, sizeInBytes);
          });
    }
  }

  // Build the headless playback buffer. Called from prepareToPlay too, so the
  // realtime processor's sample rate is already the host rate and invalidate()
  // resamples the snapshot correctly (otherwise playback walks off a wrong-rate
  // buffer and falls back to the buzzing raw-source path).
  bindRealtimeProcessorHeadless();
}

void PitchNetAudioProcessor::didBindToARA() noexcept {
  juce::AudioProcessorARAExtension::didBindToARA();
  ensureHeadlessAraBinding();
}

PitchNetAudioProcessor::~PitchNetAudioProcessor() {
  // Cancel every backend before member destruction starts joining workers.
  if (araAnalysisController)
    araAnalysisController->requestShutdown();
  if (araIncrementalAnalysisController)
    araIncrementalAnalysisController->requestShutdown();
  if (regionCanvasController)
    regionCanvasController->requestShutdown();

  cancelPendingUpdate();
  // The document controller can outlive this processor while the host releases
  // ARA objects. Drop only the binding that belongs to this processor so later
  // callbacks cannot use stale raw pointers or processor-capturing lambdas.
  if (araDocumentController) {
    araDocumentController->releaseEditorProcessor(this);
    araDocumentController->releaseOwningProcessor(this);
  }
}
#else
void PitchNetAudioProcessor::publishPersistentProjectSnapshot(
    const Project &) {}

PitchNetAudioProcessor::~PitchNetAudioProcessor() {
  if (araAnalysisController)
    araAnalysisController->requestShutdown();
  if (araIncrementalAnalysisController)
    araIncrementalAnalysisController->requestShutdown();
  if (regionCanvasController)
    regionCanvasController->requestShutdown();
  cancelPendingUpdate();
}
#endif
