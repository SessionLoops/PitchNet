#include "PluginProcessor.h"
#include "../Audio/EditorController.h"
#include "../Undo/PitchUndoManager.h"
#include "../Models/ProjectSerializer.h"
#include "../UI/IMainView.h"
#include "../Utils/Localization.h"
<<<<<<< HEAD
#include "../Utils/OnnxRuntime.h"
=======
#include "../Utils/OnnxRuntimeLoader.h"
>>>>>>> 52705bffccd9c74cfd6682e3021031781150e668
#include "ARADocumentController.h"
#include "PitchNetAudioModification.h"
#include "PluginEditor.h"
#include <cmath>
#include <cstdint>
#include <limits>

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

juce::AudioBuffer<float> copyTimelineSlice(const juce::AudioBuffer<float> &src,
                                           double sampleRate,
                                           double startSeconds,
                                           double endSeconds) {
  if (src.getNumSamples() <= 0 || sampleRate <= 0.0 ||
      endSeconds <= startSeconds)
    return {};

  const auto startSample64 = static_cast<juce::int64>(
      std::llround(std::max(0.0, startSeconds) * sampleRate));
  const auto endSample64 = static_cast<juce::int64>(
      std::llround(std::max(startSeconds, endSeconds) * sampleRate));
  const int startSample = static_cast<int>(
      juce::jlimit<juce::int64>(0, src.getNumSamples(), startSample64));
  const int endSample = static_cast<int>(
      juce::jlimit<juce::int64>(startSample, src.getNumSamples(),
                                endSample64));
  const int numSamples = endSample - startSample;
  if (numSamples <= 0)
    return {};

  juce::AudioBuffer<float> slice(src.getNumChannels(), numSamples);
  for (int ch = 0; ch < src.getNumChannels(); ++ch)
    slice.copyFrom(ch, 0, src, ch, startSample, numSamples);
  return slice;
}

#if JucePlugin_Enable_ARA
using SampleRange = juce::Range<int>;

juce::String archivedRegionKeyForLiveRegion(
    const juce::ARAPlaybackRegion &region) {
  const auto *modification = region.getAudioModification();
  if (modification == nullptr)
    return {};

  const auto &regions =
      modification->getPlaybackRegions<juce::ARAPlaybackRegion>();
  for (size_t i = 0; i < regions.size(); ++i)
    if (regions[i] == &region)
      return pitchnetRegionKeyForIndex(modification->getPersistentID(),
                                       static_cast<int>(i));

  return {};
}

bool projectAppearsToCoverRegion(const Project &project, double regionStart,
                                 double regionEnd) {
  const auto &audioData = project.getAudioData();
  if (audioData.waveform.getNumSamples() <= 0 || audioData.f0.empty())
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

bool projectHasEditableAnalysisData(const Project &project) {
  const auto &audioData = project.getAudioData();
  if (audioData.waveform.getNumSamples() <= 0 ||
      audioData.originalWaveform.getNumSamples() <= 0 || audioData.f0.empty())
    return false;

  const int f0Size = static_cast<int>(audioData.f0.size());
  for (const auto &note : project.getNotes()) {
    if (note.getStartFrame() < 0 || note.getEndFrame() > f0Size)
      return false;
  }

  return true;
}

std::vector<SampleRange> collectDirtyRegionSampleRanges(
    const Project &project, double sampleRate, double regionStartSeconds) {
  std::vector<SampleRange> ranges;
  if (sampleRate <= 0.0)
    return ranges;

  const auto toRegionSample = [sampleRate, regionStartSeconds](int frame) {
    const double absoluteSeconds =
        static_cast<double>(frame) * static_cast<double>(HOP_SIZE) / sampleRate;
    double regionSeconds = absoluteSeconds - regionStartSeconds;
    if (regionSeconds < 0.0)
      regionSeconds = absoluteSeconds;
    return static_cast<int>(std::llround(std::max(0.0, regionSeconds) *
                                         sampleRate));
  };

  for (const auto &note : project.getNotes()) {
    if (!note.isDirty())
      continue;
    const int start = toRegionSample(note.getStartFrame());
    const int end = toRegionSample(note.getEndFrame());
    if (end > start)
      ranges.emplace_back(start, end);
  }

  if (project.hasF0DirtyRange()) {
    const auto [startFrame, endFrame] = project.getF0DirtyRange();
    const int start = toRegionSample(startFrame);
    const int end = toRegionSample(endFrame);
    if (end > start)
      ranges.emplace_back(start, end);
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const auto &a, const auto &b) {
              return a.getStart() < b.getStart();
            });

  std::vector<SampleRange> merged;
  for (const auto &range : ranges) {
    if (merged.empty() || range.getStart() > merged.back().getEnd()) {
      merged.push_back(range);
      continue;
    }
    merged.back() = merged.back().getUnionWith(range);
  }

  return merged;
}

void preserveProcessedAudioOutsideRanges(
    juce::AudioBuffer<float> &replacement, double replacementRate,
    juce::int64 replacementStartInModification,
    const juce::AudioBuffer<float> &previous, double previousRate,
    juce::int64 previousStartInModification,
    const std::vector<SampleRange> &changedRanges) {
  if (replacement.getNumSamples() <= 0 || previous.getNumSamples() <= 0 ||
      replacementRate <= 0.0 || previousRate <= 0.0 || changedRanges.empty())
    return;

  const int channels =
      std::min(replacement.getNumChannels(), previous.getNumChannels());
  if (channels <= 0)
    return;

  const auto isChanged = [&changedRanges](int replacementSample) {
    for (const auto &range : changedRanges)
      if (range.contains(replacementSample))
        return true;
    return false;
  };

  const double rateRatio = previousRate / replacementRate;
  for (int dst = 0; dst < replacement.getNumSamples(); ++dst) {
    if (isChanged(dst))
      continue;

    const auto modificationSample =
        replacementStartInModification + static_cast<juce::int64>(dst);
    const double previousSamplePosition =
        static_cast<double>(modificationSample - previousStartInModification) *
        rateRatio;
    const int previousSample =
        static_cast<int>(std::llround(previousSamplePosition));
    if (previousSample < 0 || previousSample >= previous.getNumSamples())
      continue;

    for (int ch = 0; ch < channels; ++ch)
      replacement.setSample(ch, dst, previous.getSample(ch, previousSample));
  }
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
<<<<<<< HEAD
  juce::String onnxRuntimeError;
  if (!OnnxRuntime::initialise(&onnxRuntimeError))
  {
    DBG("PitchNet: failed to initialise ONNX Runtime: " + onnxRuntimeError);
    jassertfalse;
  }
=======
  OnnxRuntimeLoader::ensureLoadedFromLocalDirectory();
>>>>>>> 52705bffccd9c74cfd6682e3021031781150e668

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

  // Non-ARA capture controller
  captureController->prepare(sampleRate, getMainBusNumOutputChannels(),
                             MAX_CAPTURE_SECONDS);
  lastCaptureUiState = captureController->getState();
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
    if (!mainComponent && araAnalysisProjectSnapshot)
      bindRealtimeProcessorHeadless();

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

void PitchNetAudioProcessor::startCapture() {
  auto state = liveCaptureUiState;
  state->generation.fetch_add(1);
  state->pending.store(false);
  state->latestSamples.store(0);
  state->sentSamples.store(0);
  state->timelineOffsetSeconds.store(0.0);
  captureTimelineOffsetSeconds = 0.0;
  captureController->resetToWaiting();
}

void PitchNetAudioProcessor::stopCapture() { captureController->stop(); }

void PitchNetAudioProcessor::bindRealtimeProcessorHeadless() {
  if (mainComponent != nullptr)
    return; // an open editor drives the binding
  if (!araAnalysisProjectSnapshot)
    return; // nothing analyzed yet
  realtimeProcessor.setVocoder(
      araAnalysisController ? araAnalysisController->getVocoder() : nullptr);
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

bool PitchNetAudioProcessor::processPluginPreview(
    juce::AudioBuffer<float> &buffer) {
  if (!pluginPreview.active.load() || !realtimeProcessor.isReady())
    return false;

  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  // A fresh preview request restarts playback from the start of the range.
  if (pluginPreview.restart.exchange(false))
    pluginPreviewCursor = 0;

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

bool PitchNetAudioProcessor::attachCachedAraAnalysis(
    std::uintptr_t sourceKey, double timelineOffsetSeconds,
    const std::vector<std::pair<double, double>> &playbackRegionRanges) {
  if (sourceKey == 0)
    return false;

  if (sourceKey != araAnalysisSourceKey) {
    // ARA source keys are runtime object identities, so they are not stable
    // across DAW project reloads. If we restored a complete analyzed snapshot
    // from the saved project before the host reports its new source key, adopt
    // the new key instead of discarding the snapshot and starting analysis over.
    if (araAnalysisSourceKey == 0 && araAnalysisReady &&
        araAnalysisProjectSnapshot)
      araAnalysisSourceKey = sourceKey;
    else
      return false;
  }

  araPlaybackRegionRanges = playbackRegionRanges;
  araAnalysisTimelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);

  if (mainComponent && araAnalysisReady && araAnalysisProjectSnapshot) {
    if (!mainComponent->hasAnalyzedProject()) {
      undoManager->clear();
      mainComponent->restoreProjectSnapshot(*araAnalysisProjectSnapshot);
      canvasShowsActiveAraRegion = false; // canvas now holds the composite
    }
    mainComponent->updateHostAudioTimelineOffset(araAnalysisTimelineOffsetSeconds);
    if (auto *project = mainComponent->getProject()) {
      project->getAudioData().timelineOffsetSeconds =
          araAnalysisTimelineOffsetSeconds;
      project->getAudioData().playbackRegionRanges = araPlaybackRegionRanges;
      araAnalysisProjectSnapshot = std::make_unique<Project>(*project);
      araAnalysisProjectJson =
          juce::JSON::toString(ProjectSerializer::toJson(*project), false);
    }
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
    mainComponent->hideAnalysisProgress();
  } else if (mainComponent && araAnalysisReady &&
             araAnalysisProjectJson.isNotEmpty()) {
    undoManager->clear();
    mainComponent->restoreProjectJson(araAnalysisProjectJson);
    canvasShowsActiveAraRegion = false; // canvas now holds the composite
    mainComponent->updateHostAudioTimelineOffset(araAnalysisTimelineOffsetSeconds);
    if (auto *project = mainComponent->getProject()) {
      project->getAudioData().timelineOffsetSeconds =
          araAnalysisTimelineOffsetSeconds;
      project->getAudioData().playbackRegionRanges = araPlaybackRegionRanges;
      araAnalysisProjectSnapshot = std::make_unique<Project>(*project);
      araAnalysisProjectJson =
          juce::JSON::toString(ProjectSerializer::toJson(*project), false);
    }
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
    mainComponent->hideAnalysisProgress();
  } else if (mainComponent && araAnalysisLoading) {
    mainComponent->setStatusMessage(TR("progress.analyzing"));
    mainComponent->showAnalysisProgress(0.0);
  }

  return araAnalysisReady || araAnalysisLoading;
}

void PitchNetAudioProcessor::requestAraSourceAnalysis(
    std::uintptr_t sourceKey, const juce::AudioBuffer<float> &buffer,
    double sampleRate, double timelineOffsetSeconds,
    const std::vector<std::pair<double, double>> &playbackRegionRanges) {
  if (sourceKey == 0 || buffer.getNumSamples() <= 0 || sampleRate <= 0.0)
    return;

  const auto previousRanges = araPlaybackRegionRanges;
  auto findMissingRanges = [](const auto &from, const auto &in) {
    std::vector<std::pair<double, double>> missing;
    std::vector<bool> matched(in.size(), false);
    for (const auto &candidate : from) {
      bool found = false;
      for (size_t i = 0; i < in.size(); ++i) {
        if (!matched[i] &&
            std::abs(candidate.first - in[i].first) < 0.000001 &&
            std::abs(candidate.second - in[i].second) < 0.000001) {
          matched[i] = true;
          found = true;
          break;
        }
      }
      if (!found)
        missing.push_back(candidate);
    }
    return missing;
  };

  if (araAnalysisReady && mainComponent && mainComponent->hasAnalyzedProject()) {
    const auto removed = findMissingRanges(previousRanges, playbackRegionRanges);
    const auto added = findMissingRanges(playbackRegionRanges, previousRanges);
    if (removed.size() == 1 && added.empty()) {
      removeAraRegionFromProject(sourceKey, removed.front(),
                                playbackRegionRanges);
      return;
    }
    if (added.size() == 1 && removed.empty()) {
      analyzeAndMergeAraRegion(sourceKey, buffer, sampleRate, added.front(),
                               playbackRegionRanges);
      return;
    }
  }

  araPlaybackRegionRanges = playbackRegionRanges;
  if (attachCachedAraAnalysis(sourceKey, timelineOffsetSeconds,
                              playbackRegionRanges))
    return;

  undoManager->clear();

  if (!araAnalysisController)
    araAnalysisController = std::make_unique<EditorController>(false);

  araAnalysisSourceKey = sourceKey;
  araAnalysisLoading = true;
  araAnalysisReady = false;
  araAnalysisTimelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
  araAnalysisProjectSnapshot.reset();
  araAnalysisProjectJson.clear();

  if (mainComponent)
  {
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
      [this, sourceKey](const juce::AudioBuffer<float> &) {
        if (sourceKey != araAnalysisSourceKey || !araAnalysisController)
          return;

        auto *project = araAnalysisController->getProject();
        if (!project)
          return;

        project->getAudioData().timelineOffsetSeconds =
            araAnalysisTimelineOffsetSeconds;
        project->getAudioData().playbackRegionRanges = araPlaybackRegionRanges;
        araAnalysisProjectSnapshot = std::make_unique<Project>(*project);
        araAnalysisProjectJson =
            juce::JSON::toString(ProjectSerializer::toJson(*project), false);
        araAnalysisLoading = false;
        araAnalysisReady = araAnalysisProjectSnapshot != nullptr;

        if (mainComponent && araAnalysisReady) {
          mainComponent->restoreProjectSnapshot(*araAnalysisProjectSnapshot);
          canvasShowsActiveAraRegion = false; // canvas now holds the composite
          mainComponent->updateHostAudioTimelineOffset(
              araAnalysisTimelineOffsetSeconds);
          if (auto *positionedProject = mainComponent->getProject()) {
            araAnalysisProjectSnapshot =
                std::make_unique<Project>(*positionedProject);
            araAnalysisProjectJson = juce::JSON::toString(
                ProjectSerializer::toJson(*positionedProject), false);
          }
          if (araAnalysisProjectSnapshot)
            publishPersistentProjectSnapshot(*araAnalysisProjectSnapshot);
          mainComponent->bindRealtimeProcessor(realtimeProcessor);
          mainComponent->hideAnalysisProgress();
        } else if (mainComponent) {
          mainComponent->hideAnalysisProgress();
        }
      });
}

void PitchNetAudioProcessor::removeAraRegionFromProject(
    std::uintptr_t newSourceKey,
    const std::pair<double, double> &removedRange,
    const std::vector<std::pair<double, double>> &remainingRanges) {
  auto *project = mainComponent ? mainComponent->getProject() : nullptr;
  if (!project)
    return;

  auto updated = std::make_unique<Project>(*project);
  auto &audio = updated->getAudioData();
  const int sampleRate = std::max(1, audio.sampleRate);
  const int firstSample = juce::jlimit(
      0, audio.waveform.getNumSamples(),
      static_cast<int>(std::floor(removedRange.first * sampleRate)));
  const int lastSample = juce::jlimit(
      firstSample, audio.waveform.getNumSamples(),
      static_cast<int>(std::ceil(removedRange.second * sampleRate)));
  const int firstFrame = std::max(
      0, static_cast<int>(std::floor(removedRange.first * sampleRate /
                                     static_cast<double>(HOP_SIZE))));
  const int lastFrame = std::max(
      firstFrame, static_cast<int>(std::ceil(
                      removedRange.second * sampleRate /
                      static_cast<double>(HOP_SIZE))));

  auto clearBuffer = [firstSample, lastSample](juce::AudioBuffer<float> &b) {
    const int end = std::min(lastSample, b.getNumSamples());
    if (end > firstSample)
      b.clear(firstSample, end - firstSample);
  };
  clearBuffer(audio.waveform);
  clearBuffer(audio.originalWaveform);

  auto clearFloats = [firstFrame, lastFrame](std::vector<float> &v) {
    const int end = std::min(lastFrame, static_cast<int>(v.size()));
    if (end > firstFrame)
      std::fill(v.begin() + firstFrame, v.begin() + end, 0.0f);
  };
  auto clearBools = [firstFrame, lastFrame](std::vector<bool> &v) {
    const int end = std::min(lastFrame, static_cast<int>(v.size()));
    for (int i = firstFrame; i < end; ++i)
      v[static_cast<size_t>(i)] = false;
  };
  clearFloats(audio.f0);
  clearFloats(audio.baseF0);
  clearFloats(audio.basePitch);
  clearFloats(audio.deltaPitch);
  clearBools(audio.voicedMask);
  clearBools(audio.vadMask);
  for (int i = firstFrame;
       i < std::min(lastFrame, static_cast<int>(audio.melSpectrogram.size()));
       ++i)
    std::fill(audio.melSpectrogram[static_cast<size_t>(i)].begin(),
              audio.melSpectrogram[static_cast<size_t>(i)].end(), 0.0f);

  auto &notes = updated->getNotes();
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [firstFrame, lastFrame](const Note &note) {
                               const int midpoint =
                                   (note.getSrcStartFrame() +
                                    note.getSrcEndFrame()) /
                                   2;
                               return midpoint >= firstFrame &&
                                      midpoint < lastFrame;
                             }),
              notes.end());
  audio.segmentChunkRanges.erase(
      std::remove_if(audio.segmentChunkRanges.begin(),
                     audio.segmentChunkRanges.end(),
                     [firstFrame, lastFrame](const auto &range) {
                       return range.first < lastFrame &&
                              range.second > firstFrame;
                     }),
      audio.segmentChunkRanges.end());
  audio.segmentDebugChunks.erase(
      std::remove_if(audio.segmentDebugChunks.begin(),
                     audio.segmentDebugChunks.end(),
                     [firstFrame, lastFrame](const auto &chunk) {
                       return chunk.startFrame < lastFrame &&
                              chunk.endFrame > firstFrame;
                     }),
      audio.segmentDebugChunks.end());

  audio.playbackRegionRanges = remainingRanges;
  audio.timelineOffsetSeconds = remainingRanges.empty()
                                    ? 0.0
                                    : std::max(0.0, std::min_element(
                                          remainingRanges.begin(),
                                          remainingRanges.end())->first);
  araPlaybackRegionRanges = remainingRanges;
  araAnalysisTimelineOffsetSeconds = audio.timelineOffsetSeconds;
  araAnalysisSourceKey = newSourceKey;
  araAnalysisProjectSnapshot = std::make_unique<Project>(*updated);
  araAnalysisProjectJson =
      juce::JSON::toString(ProjectSerializer::toJson(*updated), false);
  mainComponent->restoreProjectSnapshot(*updated);
  mainComponent->bindRealtimeProcessor(realtimeProcessor);
}

void PitchNetAudioProcessor::analyzeAndMergeAraRegion(
    std::uintptr_t newSourceKey, const juce::AudioBuffer<float> &buffer,
    double sampleRate, const std::pair<double, double> &addedRange,
    const std::vector<std::pair<double, double>> &allRanges) {
  const int firstSample = juce::jlimit(
      0, buffer.getNumSamples(),
      static_cast<int>(std::floor(addedRange.first * sampleRate)));
  const int lastSample = juce::jlimit(
      firstSample, buffer.getNumSamples(),
      static_cast<int>(std::ceil(addedRange.second * sampleRate)));
  if (lastSample <= firstSample)
    return;

  juce::AudioBuffer<float> region(buffer.getNumChannels(),
                                  lastSample - firstSample);
  for (int ch = 0; ch < region.getNumChannels(); ++ch)
    region.copyFrom(ch, 0, buffer, ch, firstSample, region.getNumSamples());

  araIncrementalAnalysisController = std::make_unique<EditorController>(false);
  auto *controller = araIncrementalAnalysisController.get();
  mainComponent->setStatusMessage(TR("progress.analyzing"));
  mainComponent->showAnalysisProgress(0.0);
  controller->setHostAudioAsync(
      region, sampleRate,
      [this](double progress, const juce::String &message) {
        juce::Component::SafePointer<juce::Component> safeMain(
            mainComponent ? mainComponent->getComponent() : nullptr);
        juce::MessageManager::callAsync(
            [safeMain, progress, message]() {
              if (auto *view =
                      dynamic_cast<IMainView *>(safeMain.getComponent())) {
                view->setStatusMessage(message);
                view->showAnalysisProgress(progress);
              }
            });
      },
      [this, controller, newSourceKey, addedRange,
       allRanges](const juce::AudioBuffer<float> &) {
        if (!araIncrementalAnalysisController ||
            araIncrementalAnalysisController.get() != controller ||
            !mainComponent)
          return;
        auto *regionProject = controller->getProject();
        auto *currentProject = mainComponent->getProject();
        if (!regionProject || !currentProject)
          return;

        auto merged = std::make_unique<Project>(*currentProject);
        auto &dst = merged->getAudioData();
        const auto &src = regionProject->getAudioData();
        const int dstRate = std::max(1, dst.sampleRate);
        const int sampleOffset = static_cast<int>(
            std::llround(addedRange.first * dstRate));
        const int frameOffset = static_cast<int>(std::llround(
            addedRange.first * dstRate / static_cast<double>(HOP_SIZE)));

        auto mergeBuffer = [sampleOffset](juce::AudioBuffer<float> &to,
                                          const juce::AudioBuffer<float> &from) {
          const int channels = std::max(to.getNumChannels(),
                                        from.getNumChannels());
          const int required = sampleOffset + from.getNumSamples();
          if (to.getNumChannels() < channels || to.getNumSamples() < required)
            to.setSize(channels, std::max(to.getNumSamples(), required), true,
                       true, false);
          for (int ch = 0; ch < from.getNumChannels(); ++ch)
            to.copyFrom(ch, sampleOffset, from, ch, 0, from.getNumSamples());
        };
        mergeBuffer(dst.waveform, src.waveform);
        mergeBuffer(dst.originalWaveform, src.originalWaveform);

        auto mergeFloats = [frameOffset](std::vector<float> &to,
                                         const std::vector<float> &from) {
          to.resize(std::max(to.size(), static_cast<size_t>(frameOffset) +
                                           from.size()), 0.0f);
          std::copy(from.begin(), from.end(), to.begin() + frameOffset);
        };
        auto mergeBools = [frameOffset](std::vector<bool> &to,
                                        const std::vector<bool> &from) {
          to.resize(std::max(to.size(), static_cast<size_t>(frameOffset) +
                                           from.size()), false);
          for (size_t i = 0; i < from.size(); ++i)
            to[static_cast<size_t>(frameOffset) + i] = from[i];
        };
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

        for (auto note : regionProject->getNotes()) {
          note.setStartFrame(note.getStartFrame() + frameOffset);
          note.setEndFrame(note.getEndFrame() + frameOffset);
          note.setSrcStartFrame(note.getSrcStartFrame() + frameOffset);
          note.setSrcEndFrame(note.getSrcEndFrame() + frameOffset);
          merged->addNote(std::move(note));
        }
        for (auto range : src.segmentChunkRanges)
          dst.segmentChunkRanges.emplace_back(range.first + frameOffset,
                                              range.second + frameOffset);
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

        dst.playbackRegionRanges = allRanges;
        dst.timelineOffsetSeconds = allRanges.empty()
                                        ? 0.0
                                        : std::max(0.0, std::min_element(
                                              allRanges.begin(),
                                              allRanges.end())->first);
        araPlaybackRegionRanges = allRanges;
        araAnalysisTimelineOffsetSeconds = dst.timelineOffsetSeconds;
        araAnalysisSourceKey = newSourceKey;
        araAnalysisProjectSnapshot = std::make_unique<Project>(*merged);
        araAnalysisProjectJson =
            juce::JSON::toString(ProjectSerializer::toJson(*merged), false);
        mainComponent->restoreProjectSnapshot(*merged);
        mainComponent->bindRealtimeProcessor(realtimeProcessor);
        mainComponent->hideAnalysisProgress();
        mainComponent->setStatusMessage({});
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
  araPlaybackRegionRanges.clear();
  araAnalysisProjectSnapshot.reset();
  araAnalysisProjectJson.clear();

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
        araAnalysisProjectJson =
            juce::JSON::toString(ProjectSerializer::toJson(*completedProject),
                                 false);
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
            araAnalysisProjectJson = juce::JSON::toString(
                ProjectSerializer::toJson(*positionedProject), false);
          }
          if (araAnalysisProjectSnapshot)
            publishPersistentProjectSnapshot(*araAnalysisProjectSnapshot);
          mainComponent->bindRealtimeProcessor(realtimeProcessor);
          mainComponent->hideAnalysisProgress();
        } else if (mainComponent) {
          mainComponent->hideAnalysisProgress();
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
    renderChangedSampleRanges = collectDirtyRegionSampleRanges(
        projectToRender, renderRate, renderRegionStartSeconds);
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
      [this, controller, renderActiveAraRegion, renderRegionKey,
       renderModification, renderStartSampleInModification,
       renderRegionStartSeconds, renderRegionEndSeconds,
       renderChangedSampleRanges](bool success) {
        auto *renderedProject = controller != nullptr ? controller->getProject()
                                                     : nullptr;

        if (success && renderedProject) {
#if JucePlugin_Enable_ARA
          if (renderActiveAraRegion && renderRegionKey.isNotEmpty()) {
            auto &audioData = renderedProject->getAudioData();
            audioData.timelineOffsetSeconds = renderRegionStartSeconds;
            if (renderRegionEndSeconds > renderRegionStartSeconds)
              audioData.playbackRegionRanges = {
                  {renderRegionStartSeconds, renderRegionEndSeconds}};

            araRegionProjects[renderRegionKey] =
                std::make_unique<Project>(*renderedProject);
            publishPersistentProjectSnapshot(*renderedProject);

            if (renderModification != nullptr) {
              juce::MemoryBlock projectArchive;
              if (serializeAraRegionProject(renderRegionKey, projectArchive))
                renderModification->setProjectArchiveForRegion(
                    renderRegionKey, projectArchive.getData(),
                    projectArchive.getSize());
            }

            if (renderModification != nullptr &&
                projectHasRegionEdits(*renderedProject) &&
                audioData.waveform.getNumSamples() > 0) {
              const double processedRate =
                  audioData.sampleRate > 0
                      ? static_cast<double>(audioData.sampleRate)
                      : hostSampleRate;
              auto processedSlice = copyTimelineSlice(
                  audioData.waveform, processedRate, renderRegionStartSeconds,
                  renderRegionEndSeconds);
              if (processedSlice.getNumSamples() <= 0)
                processedSlice.makeCopyOf(audioData.waveform);
              if (!renderChangedSampleRanges.empty()) {
                juce::AudioBuffer<float> previousProcessed;
                double previousRate = 0.0;
                juce::int64 previousStart = 0;
                if (renderModification->copyProcessedAudioForRegion(
                        renderRegionKey, previousProcessed, previousRate,
                        previousStart))
                  preserveProcessedAudioOutsideRanges(
                      processedSlice, processedRate,
                      renderStartSampleInModification, previousProcessed,
                      previousRate, previousStart, renderChangedSampleRanges);
              }
              renderModification->setProcessedAudioForRegion(
                  renderRegionKey, processedSlice, processedRate,
                  renderStartSampleInModification);
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
            araAnalysisProjectJson =
                juce::JSON::toString(
                    ProjectSerializer::toJson(*renderedProject), false);
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
                  view->updateHostAudioTimelineOffset(
                      renderActiveAraRegion ? renderRegionStartSeconds
                                            : araAnalysisTimelineOffsetSeconds);
                  view->bindRealtimeProcessor(realtimeProcessor);
                }
                view->finishBackendRender(success);
                view->setStatusMessage({});
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
    auto &audioData = araRegionScopedProject->getAudioData();
    audioData.timelineOffsetSeconds = activeRegionStartSeconds;
    if (activeRegionEndSeconds > activeRegionStartSeconds)
      audioData.playbackRegionRanges = {
          {activeRegionStartSeconds, activeRegionEndSeconds}};

    if (mainComponent != nullptr) {
      if (auto *uiProject = mainComponent->getProject()) {
        auto &uiAudioData = uiProject->getAudioData();
        uiAudioData.timelineOffsetSeconds = activeRegionStartSeconds;
        if (activeRegionEndSeconds > activeRegionStartSeconds)
          uiAudioData.playbackRegionRanges = {
              {activeRegionStartSeconds, activeRegionEndSeconds}};
        if (auto *component = mainComponent->getComponent())
          component->repaint();
      }
    }
  }
#endif

  const Project &stateProject =
      araRegionScopedProject != nullptr ? *araRegionScopedProject : project;

  araAnalysisProjectSnapshot = std::make_unique<Project>(stateProject);
  araAnalysisProjectJson =
      juce::JSON::toString(ProjectSerializer::toJson(stateProject), false);
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
    // Keep the active region's cached project current so a switch-away or a
    // save (per-region persistence) captures the edit, not the pre-edit
    // project.
    araRegionProjects[activeRegionKey] =
        std::make_unique<Project>(stateProject);

    if (activeModification != nullptr) {
      juce::MemoryBlock projectArchive;
      if (serializeAraRegionProject(activeRegionKey, projectArchive))
        activeModification->setProjectArchiveForRegion(
            activeRegionKey, projectArchive.getData(),
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
        auto processedSlice =
            copyTimelineSlice(processed, processedRate, activeRegionStartSeconds,
                              activeRegionEndSeconds);
        if (processedSlice.getNumSamples() <= 0)
          processedSlice.makeCopyOf(processed);
        activeModification->setProcessedAudioForRegion(
            activeRegionKey, processedSlice, processedRate,
            activeStartSampleInModification);

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
    }
  } else if (araDocumentController != nullptr &&
             projectHasRegionEdits(stateProject)) {
    // The canvas holds the COMPOSITE project (no region selected) and the user
    // edited it. ARA playback is strictly modification-or-original — there is
    // no realtime-engine path — so slice the composite waveform per edited
    // region and store the slices on the modifications; unedited regions stay
    // unpublished and keep playing their original source.
    araDocumentController->publishCompositeEditsToRegions(stateProject);
  }
#endif
}

#if JucePlugin_Enable_ARA
bool PitchNetAudioProcessor::projectHasRegionEdits(const Project &project) {
  if (project.getGlobalPitchOffset() != 0.0f ||
      project.getFormantShift() != 0.0f)
    return true;

  // Any note with a synthesised waveform means the user edited it (synthesis
  // only runs for edited notes); a freshly analysed, untouched project has
  // none.
  for (const auto &note : project.getNotes())
    if (note.hasSynthWaveform())
      return true;

  return false;
}
#endif

void PitchNetAudioProcessor::updateAraTimelineOffset(
    double timelineOffsetSeconds) {
  araAnalysisTimelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);

  if (!mainComponent)
    return;

  mainComponent->updateHostAudioTimelineOffset(araAnalysisTimelineOffsetSeconds);

  auto *project = mainComponent->getProject();
  if (!project)
    return;

  project->getAudioData().timelineOffsetSeconds =
      araAnalysisTimelineOffsetSeconds;
  araAnalysisProjectSnapshot = std::make_unique<Project>(*project);
  araAnalysisProjectJson =
      juce::JSON::toString(ProjectSerializer::toJson(*project), false);
  publishPersistentProjectSnapshot(*project);

  if (araAnalysisReady)
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
}

bool PitchNetAudioProcessor::serializePersistentProjectState(
    juce::MemoryBlock &destData) const {
  destData.setSize(0);

  if (mainComponent) {
    if (auto *project = mainComponent->getProject())
      return ProjectSerializer::toBinaryArchive(*project, destData);
  }

  if (araAnalysisProjectSnapshot)
    return ProjectSerializer::toBinaryArchive(*araAnalysisProjectSnapshot,
                                              destData);

  if (araAnalysisController && araAnalysisController->getProject())
    return ProjectSerializer::toBinaryArchive(
        *araAnalysisController->getProject(), destData);

  if (pendingStateJson.isNotEmpty()) {
    Project pendingProject;
    if (ProjectSerializer::fromJson(
            pendingProject, juce::JSON::parse(pendingStateJson)))
      return ProjectSerializer::toBinaryArchive(pendingProject, destData);

    destData.append(pendingStateJson.toRawUTF8(),
                    pendingStateJson.getNumBytesAsUTF8());
    return destData.getSize() > 0;
  }

  return false;
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

  // Rebuild the global waveform from the persisted per-note synthesis so
  // headless playback reflects saved edits (see binary branch above).
  restoredProject->recomposeFromSynthIfPresent();

  araAnalysisTimelineOffsetSeconds =
      restoredProject->getAudioData().timelineOffsetSeconds;
  araPlaybackRegionRanges =
      restoredProject->getAudioData().playbackRegionRanges;
  araAnalysisProjectJson = projectJson;
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
    // The global waveform stored in the archive can lag the authoritative
    // per-note synthesis (it's only refreshed when the editor recomposes).
    // Rebuild it from originalWaveform + the persisted per-note synthWaveforms
    // so headless playback reflects the saved edits without needing the editor
    // open. Guarded so it never reverts a project that has no per-note synthesis.
    restoredProject->recomposeFromSynthIfPresent();
    pendingStateJson.clear();
    araAnalysisTimelineOffsetSeconds =
        restoredProject->getAudioData().timelineOffsetSeconds;
    araPlaybackRegionRanges =
        restoredProject->getAudioData().playbackRegionRanges;
    araAnalysisProjectJson =
        juce::JSON::toString(ProjectSerializer::toJson(*restoredProject),
                             false);
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
      araAnalysisProjectJson =
          juce::JSON::toString(ProjectSerializer::toJson(*project), false);
      publishPersistentProjectSnapshot(*project);
    }
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
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
    if (pendingStateJson.isNotEmpty() &&
        mc->restoreProjectJson(pendingStateJson)) {
      pendingStateJson.clear();
      restoredPersistentProject = true;
    } else if (araAnalysisProjectSnapshot) {
      restoredPersistentProject =
          mc->restoreProjectSnapshot(*araAnalysisProjectSnapshot);
    } else if (araAnalysisProjectJson.isNotEmpty()) {
      restoredPersistentProject = mc->restoreProjectJson(araAnalysisProjectJson);
    }

    // Sync current APVTS parameter values to project
    if (auto *project = mc->getProject()) {
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
        araAnalysisProjectJson =
            juce::JSON::toString(ProjectSerializer::toJson(*project), false);
        araAnalysisReady =
            project->getAudioData().waveform.getNumSamples() > 0 &&
            !project->getAudioData().f0.empty();
        publishPersistentProjectSnapshot(*project);
      }
    }

    mc->restoreViewportState(viewportState);
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
      realtimeProcessor.setVocoder(nullptr);
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

  juce::MemoryBlock projectArchive;
  serializePersistentProjectState(projectArchive);

  out.writeInt(static_cast<int>(kPluginStateMagic));
  out.writeInt(kPluginStateBinaryVersion);
  writeStateString(out, parametersXml);
  out.writeInt64(static_cast<juce::int64>(projectArchive.getSize()));
  if (projectArchive.getSize() > 0)
    out.write(projectArchive.getData(), projectArchive.getSize());
}

void PitchNetAudioProcessor::setStateInformation(const void *data,
                                                   int sizeInBytes) {
  if (!data || sizeInBytes <= 0)
    return;

  {
    juce::MemoryInputStream in(data, static_cast<size_t>(sizeInBytes), false);
    if (static_cast<std::uint32_t>(in.readInt()) == kPluginStateMagic) {
      if (in.readInt() != kPluginStateBinaryVersion)
        return;

      undoManager->clear();

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

  undoManager->clear();

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

void PitchNetAudioProcessor::setActiveAraRegion(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr)
    return;

  const auto key = pitchnetRegionKey(*region);
  if (key.isEmpty() || key == activeRegionKey)
    return;

  // Preserve the outgoing region's edits: copy the current canvas project back
  // into its per-region slot (only if that region is already tracked, so we
  // never overwrite a region with another region's data before Stage 2 fills
  // the map).
  if (activeRegionKey.isNotEmpty() && mainComponent != nullptr &&
      araRegionProjects.count(activeRegionKey) > 0) {
    if (auto *current = mainComponent->getProject()) {
      auto outgoing = std::make_unique<Project>(*current);
      auto &audioData = outgoing->getAudioData();
      audioData.timelineOffsetSeconds = activeRegionStartSeconds;
      if (activeRegionEndSeconds > activeRegionStartSeconds)
        audioData.playbackRegionRanges = {
            {activeRegionStartSeconds, activeRegionEndSeconds}};
      araRegionProjects[activeRegionKey] = std::move(outgoing);
    }
  }

  activeRegionKey = key;
  activeModification = region->getAudioModification<PitchNetAudioModification>();
  activeRegionStartSeconds = std::max(0.0, region->getStartInPlaybackTime());
  activeRegionEndSeconds = std::max(activeRegionStartSeconds,
                                    region->getEndInPlaybackTime());
  activeStartSampleInModification =
      region->getStartInAudioModificationSamples();

  // Show the incoming region's project. Priority:
  //  1) a per-region project we already have (analysed this session, or restored
  //     from the saved project) -> instant switch, no re-analysis;
  //  2) otherwise analyse THIS region from its own source (per-region correct;
  //     renders its processed audio). Reload no longer re-analyses because
  //     visited regions are restored into (1) by per-region persistence.
  if (mainComponent != nullptr) {
    auto it = araRegionProjects.find(key);
    if (it == araRegionProjects.end() && activeModification != nullptr) {
      juce::MemoryBlock archive;
      if (activeModification->copyProjectArchiveForRegion(key, archive)) {
        restoreAraRegionProject(key, archive.getData(), archive.getSize());
        it = araRegionProjects.find(key);
      }
    }
    if (it == araRegionProjects.end()) {
      const auto archivedKey = archivedRegionKeyForLiveRegion(*region);
      if (archivedKey.isNotEmpty() && archivedKey != key) {
        if (activeModification != nullptr) {
          juce::MemoryBlock archive;
          if (activeModification->copyProjectArchiveForRegion(archivedKey,
                                                              archive)) {
            restoreAraRegionProject(archivedKey, archive.getData(),
                                    archive.getSize());
          }
        }
        if (const auto archivedIt = araRegionProjects.find(archivedKey);
            archivedIt != araRegionProjects.end() && archivedIt->second &&
            projectAppearsToCoverRegion(*archivedIt->second,
                                        activeRegionStartSeconds,
                                        activeRegionEndSeconds)) {
          araRegionProjects[key] =
              std::make_unique<Project>(*archivedIt->second);
          it = araRegionProjects.find(key);
        }
      }
    }

    if (it != araRegionProjects.end() && it->second) {
      mainComponent->restoreProjectSnapshot(*it->second);
      mainComponent->updateHostAudioTimelineOffset(activeRegionStartSeconds);
      if (auto *project = mainComponent->getProject()) {
        project->getAudioData().playbackRegionRanges = {
            {activeRegionStartSeconds, activeRegionEndSeconds}};
        araRegionProjects[key] = std::make_unique<Project>(*project);
      }
      canvasShowsActiveAraRegion = true;
    } else if (araDocumentController != nullptr) {
      if (auto *current = mainComponent->getProject();
          current != nullptr &&
          projectAppearsToCoverRegion(*current, activeRegionStartSeconds,
                                      activeRegionEndSeconds)) {
        auto restoredRegionProject = std::make_unique<Project>(*current);
        auto &audioData = restoredRegionProject->getAudioData();
        audioData.timelineOffsetSeconds = activeRegionStartSeconds;
        audioData.playbackRegionRanges = {
            {activeRegionStartSeconds, activeRegionEndSeconds}};
        araRegionProjects[key] = std::move(restoredRegionProject);
        mainComponent->updateHostAudioTimelineOffset(activeRegionStartSeconds);
        canvasShowsActiveAraRegion = true;
        return;
      }

      if (araAnalysisProjectSnapshot != nullptr &&
          projectAppearsToCoverRegion(*araAnalysisProjectSnapshot,
                                      activeRegionStartSeconds,
                                      activeRegionEndSeconds)) {
        auto restoredRegionProject =
            std::make_unique<Project>(*araAnalysisProjectSnapshot);
        auto &audioData = restoredRegionProject->getAudioData();
        audioData.timelineOffsetSeconds = activeRegionStartSeconds;
        audioData.playbackRegionRanges = {
            {activeRegionStartSeconds, activeRegionEndSeconds}};
        araRegionProjects[key] = std::move(restoredRegionProject);
        mainComponent->restoreProjectSnapshot(*araRegionProjects[key]);
        mainComponent->updateHostAudioTimelineOffset(activeRegionStartSeconds);
        mainComponent->bindRealtimeProcessor(realtimeProcessor);
        canvasShowsActiveAraRegion = true;
        return;
      }

      // Canvas keeps its previous (possibly composite) content until the
      // region's analysis completes and repaints it.
      canvasShowsActiveAraRegion = false;
      araDocumentController->requestRegionCanvasAnalysis(region);
    }
  }
}

void PitchNetAudioProcessor::updateActiveAraRegionProperties(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr)
    return;

  const auto key = pitchnetRegionKey(*region);
  if (key.isEmpty() || key != activeRegionKey)
    return;

  activeModification = region->getAudioModification<PitchNetAudioModification>();
  activeRegionStartSeconds = std::max(0.0, region->getStartInPlaybackTime());
  activeRegionEndSeconds =
      std::max(activeRegionStartSeconds, region->getEndInPlaybackTime());
  activeStartSampleInModification =
      region->getStartInAudioModificationSamples();

  if (mainComponent != nullptr && canvasShowsActiveAraRegion) {
    mainComponent->updateHostAudioTimelineOffset(activeRegionStartSeconds);

    if (auto *project = mainComponent->getProject()) {
      auto &audioData = project->getAudioData();
      audioData.timelineOffsetSeconds = activeRegionStartSeconds;
      audioData.playbackRegionRanges = {
          {activeRegionStartSeconds, activeRegionEndSeconds}};

      araRegionProjects[key] = std::make_unique<Project>(*project);
      publishPersistentProjectSnapshot(*project);

      if (araAnalysisReady)
        mainComponent->bindRealtimeProcessor(realtimeProcessor);
    }
  } else if (auto it = araRegionProjects.find(key);
             it != araRegionProjects.end() && it->second) {
    auto &audioData = it->second->getAudioData();
    audioData.timelineOffsetSeconds = activeRegionStartSeconds;
    audioData.playbackRegionRanges = {
        {activeRegionStartSeconds, activeRegionEndSeconds}};
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
  juce::ignoreUnused(startSampleInModification);

  if (!regionCanvasController)
    regionCanvasController = std::make_unique<EditorController>(false);

  if (mainComponent) {
    mainComponent->setStatusMessage(TR("progress.analyzing"));
    mainComponent->showAnalysisProgress(0.0);
  }

  regionCanvasController->setHostAudioAsync(
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
      [this, regionKey, modification,
       timelineOffsetSeconds](const juce::AudioBuffer<float> &) {
        if (!regionCanvasController)
          return;
        auto *project = regionCanvasController->getProject();
        if (!project)
          return;

        project->getAudioData().timelineOffsetSeconds =
            std::max(0.0, timelineOffsetSeconds);
        if (project->getAudioData().getDuration() >
            std::max(0.0, timelineOffsetSeconds))
          project->getAudioData().playbackRegionRanges = {
              {std::max(0.0, timelineOffsetSeconds),
               project->getAudioData().getDuration()}};

        // Cache this region's analysis so re-selecting it is instant and its
        // edits persist across switches.
        araRegionProjects[regionKey] = std::make_unique<Project>(*project);
        if (modification != nullptr) {
          juce::MemoryBlock projectArchive;
          if (serializeAraRegionProject(regionKey, projectArchive))
            modification->setProjectArchiveForRegion(
                regionKey, projectArchive.getData(), projectArchive.getSize());
        }

        // Paint it onto the canvas if it is still the active region.
        if (regionKey == activeRegionKey && mainComponent) {
          mainComponent->restoreProjectSnapshot(*araRegionProjects[regionKey]);
          mainComponent->updateHostAudioTimelineOffset(
              std::max(0.0, timelineOffsetSeconds));
          mainComponent->bindRealtimeProcessor(realtimeProcessor);
          canvasShowsActiveAraRegion = true;
        }

        // No audio is published on analysis: an unedited region plays its raw
        // ARA source (VocalNet's model). resynth-on-edit publishes processed
        // audio only for regions the user actually edits — no full re-synthesis
        // of unchanged audio.
        if (mainComponent)
          mainComponent->hideAnalysisProgress();
      });
}

void PitchNetAudioProcessor::clearActiveAraRegionIfModification(
    PitchNetAudioModification *modification) {
  if (modification != nullptr && modification == activeModification) {
    activeModification = nullptr;
    activeRegionKey.clear();
    canvasShowsActiveAraRegion = false;
  }
}

bool PitchNetAudioProcessor::serializeAraRegionProject(
    const juce::String &regionKey, juce::MemoryBlock &out) const {
  out.setSize(0);

  const auto it = araRegionProjects.find(regionKey);
  if (it == araRegionProjects.end() || it->second == nullptr)
    return false;

  return ProjectSerializer::toBinaryArchive(*it->second, out) &&
         out.getSize() > 0;
}

bool PitchNetAudioProcessor::hasAraRegionProject(
    const juce::String &regionKey) const {
  const auto it = araRegionProjects.find(regionKey);
  return it != araRegionProjects.end() && it->second != nullptr;
}

bool PitchNetAudioProcessor::araRegionProjectAppearsToCover(
    const juce::String &regionKey, double regionStart, double regionEnd) const {
  const auto it = araRegionProjects.find(regionKey);
  return it != araRegionProjects.end() && it->second != nullptr &&
         projectAppearsToCoverRegion(*it->second, regionStart, regionEnd);
}

bool PitchNetAudioProcessor::restoredAraProjectAppearsToCover(
    double regionStart, double regionEnd) const {
  return araAnalysisProjectSnapshot != nullptr &&
         projectAppearsToCoverRegion(*araAnalysisProjectSnapshot, regionStart,
                                     regionEnd);
}

void PitchNetAudioProcessor::restoreAraRegionProject(const juce::String &regionKey,
                                                     const void *data,
                                                     size_t sizeInBytes) {
  if (data == nullptr || sizeInBytes == 0)
    return;

  auto project = std::make_unique<Project>();
  if (ProjectSerializer::fromBinaryArchive(*project, data, sizeInBytes) &&
      projectHasEditableAnalysisData(*project)) {
    project->recomposeFromSynthIfPresent();
    araRegionProjects[regionKey] = std::move(project);
    return;
  }

  const juce::String json(juce::CharPointer_UTF8(static_cast<const char *>(data)),
                          sizeInBytes);
  const auto parsed = juce::JSON::parse(json);
  if (!parsed.isObject())
    return;

  project = std::make_unique<Project>();
  if (ProjectSerializer::fromJson(*project, parsed) &&
      projectHasEditableAnalysisData(*project)) {
    project->recomposeFromSynthIfPresent();
    araRegionProjects[regionKey] = std::move(project);
  }
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
            return serializePersistentProjectState(destData);
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
  // The document controller can outlive this processor while the host releases
  // ARA objects. Drop only the binding that belongs to this processor so later
  // callbacks cannot use stale raw pointers or processor-capturing lambdas.
  if (araDocumentController)
    araDocumentController->releaseOwningProcessor(this);
}
#else
void PitchNetAudioProcessor::publishPersistentProjectSnapshot(
    const Project &) {}

PitchNetAudioProcessor::~PitchNetAudioProcessor() = default;
#endif
