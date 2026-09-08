#include "PluginProcessor.h"
#include "../Utils/AudioResampler.h"
#include "../Audio/EditorController.h"
#include "../Undo/PitchUndoManager.h"
#include "../Models/ProjectSerializer.h"
#include "../UI/IMainView.h"
#include "../Utils/Localization.h"
#include "../Utils/Constants.h"
#include "../Utils/MelSpectrogram.h"
#include "../Utils/OnnxRuntime.h"
#include "../Utils/OnnxRuntimeLoader.h"
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

#if JucePlugin_Enable_ARA

// Seed a timeline-anchored Project waveform from a region-local processed
// render. processed[0] corresponds to processedStartInModification; account
// for later trims by advancing into that render before placing it at the
// region's current playback position. Return false unless the processed audio
// covers the complete current region so hydration can safely leave the region
// source-backed when no compatible composite is available.
bool replaceTimelineRegionWithProcessedAudio(
    juce::AudioBuffer<float> &timeline, double timelineRate,
    const juce::AudioBuffer<float> &processed, double processedRate,
    juce::int64 processedStartInModification,
    juce::int64 currentStartInModification, double modificationRate,
    double regionStartSeconds, double regionEndSeconds) {
  if (timeline.getNumChannels() <= 0 || timeline.getNumSamples() <= 0 ||
      processed.getNumChannels() <= 0 || processed.getNumSamples() <= 0 ||
      !std::isfinite(timelineRate) || !std::isfinite(processedRate) ||
      !std::isfinite(modificationRate) || timelineRate <= 0.0 ||
      processedRate <= 0.0 || modificationRate <= 0.0 ||
      !std::isfinite(regionStartSeconds) ||
      !std::isfinite(regionEndSeconds) || regionStartSeconds < 0.0 ||
      regionEndSeconds <= regionStartSeconds)
    return false;

  const auto destinationStart64 = static_cast<juce::int64>(
      std::llround(regionStartSeconds * timelineRate));
  const auto destinationEnd64 = static_cast<juce::int64>(
      std::llround(regionEndSeconds * timelineRate));
  if (destinationStart64 < 0 || destinationEnd64 <= destinationStart64 ||
      destinationStart64 >= timeline.getNumSamples())
    return false;

  // The independently rounded timeline length can differ from the rounded
  // region end by one sample after host-rate -> project-rate conversion.
  constexpr juce::int64 kTimelineCoverageTolerance = 2;
  if (destinationEnd64 >
      static_cast<juce::int64>(timeline.getNumSamples()) +
          kTimelineCoverageTolerance)
    return false;

  const int destinationStart = static_cast<int>(destinationStart64);
  const int destinationEnd = static_cast<int>(std::min<juce::int64>(
      destinationEnd64, timeline.getNumSamples()));
  const int destinationSamples = destinationEnd - destinationStart;
  if (destinationSamples <= 0)
    return false;

  const double modificationOffsetSeconds =
      static_cast<double>(currentStartInModification -
                          processedStartInModification) /
      modificationRate;
  const double processedStart = modificationOffsetSeconds * processedRate;
  const double processedStep = processedRate / timelineRate;
  const double processedEnd =
      processedStart + static_cast<double>(destinationSamples) * processedStep;

  // Permit only rounding-sized edge discrepancies. A materially incomplete
  // processed render must not partially replace the pristine source because
  // that would expose an audible edited/original seam in the UI waveform.
  constexpr double kProcessedCoverageTolerance = 2.0;
  if (processedStart < -kProcessedCoverageTolerance ||
      processedEnd > static_cast<double>(processed.getNumSamples()) +
                         kProcessedCoverageTolerance)
    return false;

  const auto roundedProcessedStart =
      static_cast<juce::int64>(std::llround(processedStart));
  if (juce::approximatelyEqual(processedRate, timelineRate) &&
      std::abs(processedStart - static_cast<double>(roundedProcessedStart)) <
          1.0e-6 &&
      roundedProcessedStart >= 0 &&
      roundedProcessedStart + destinationSamples <=
          processed.getNumSamples()) {
    for (int channel = 0; channel < timeline.getNumChannels(); ++channel)
      timeline.copyFrom(
          channel, destinationStart, processed,
          std::min(channel, processed.getNumChannels() - 1),
          static_cast<int>(roundedProcessedStart), destinationSamples);
    return true;
  }

  for (int channel = 0; channel < timeline.getNumChannels(); ++channel) {
    const auto *source = processed.getReadPointer(
        std::min(channel, processed.getNumChannels() - 1));
    auto *destination = timeline.getWritePointer(channel, destinationStart);
    for (int sample = 0; sample < destinationSamples; ++sample) {
      const double sourcePosition = std::clamp(
          processedStart + static_cast<double>(sample) * processedStep, 0.0,
          static_cast<double>(processed.getNumSamples() - 1));
      const int left = static_cast<int>(sourcePosition);
      const int right = std::min(left + 1, processed.getNumSamples() - 1);
      const float fraction =
          static_cast<float>(sourcePosition - static_cast<double>(left));
      destination[sample] =
          source[left] + fraction * (source[right] - source[left]);
    }
  }
  return true;
}

// The UI's timeline (and its live playhead, driven from raw host transport
// time) is PLAYBACK-time anchored, so MainComponent::updateHostAudioTimelineOffset
// re-pads the whole displayed Project's buffers/frames to whatever region is
// active. The persistent per-modification master Project and the processed-
// audio render cache must instead stay MODIFICATION-time anchored (offset 0)
// so they mean the same thing regardless of which region is on screen. These
// two helpers convert between those coordinate systems -- ported from (and
// must stay behaviorally identical to) MainComponent::updateHostAudioTimelineOffset's
// repad, since PluginProcessor cannot call that UI method on an arbitrary,
// off-screen Project without disturbing what is actually being displayed.

// Small mirror of the repad below, for just a waveform buffer -- used right
// before caching audio for the renderer, which always expects modification-
// time (see mixProcessedRegionAudio() in ARADocumentController.cpp).
// shiftSeconds is how much leading padding is currently baked into
// shiftedBuffer beyond true modification-sample 0.
juce::AudioBuffer<float> unshiftWaveformToModificationTime(
    const juce::AudioBuffer<float> &shiftedBuffer, double shiftSeconds,
    double sampleRate) {
  if (shiftedBuffer.getNumSamples() <= 0 || sampleRate <= 0.0)
    return {};
  // Negative shift (a region whose modification-time start is LATER than its
  // playback-time start) is a known residual limitation: the forward re-pad
  // in MainComponent also clamps to 0, so there is no leading padding to
  // remove in that case either -- content stays where the forward pass left
  // it. Typical single-take audio has modification-start near 0, so this is
  // rarely hit in practice.
  const int shiftSamples =
      std::max(0, static_cast<int>(std::llround(shiftSeconds * sampleRate)));
  const int contentStart =
      std::min(shiftSamples, shiftedBuffer.getNumSamples());
  const int contentSamples = shiftedBuffer.getNumSamples() - contentStart;
  if (contentSamples <= 0)
    return {};
  juce::AudioBuffer<float> unshifted(shiftedBuffer.getNumChannels(),
                                     contentSamples);
  for (int ch = 0; ch < shiftedBuffer.getNumChannels(); ++ch)
    unshifted.copyFrom(ch, 0, shiftedBuffer, ch, contentStart, contentSamples);
  return unshifted;
}

// Full project re-pad (waveform, mel, every F0 variant, masks, notes, segment
// debug data), ported field-for-field from
// MainComponent::updateHostAudioTimelineOffset so a project can be moved
// between the modification-time master anchor (0) and a region's playback-
// time display anchor without touching the live UI. Used when persisting an
// edited display copy back into the per-modification master cache.
void repadProjectTimeline(Project &project, double oldOffsetSeconds,
                          double newOffsetSeconds) {
  auto &audioData = project.getAudioData();
  if (std::abs(newOffsetSeconds - oldOffsetSeconds) < 0.000001)
    return;

  const int sampleRate = audioData.sampleRate;
  if (sampleRate <= 0)
    return;
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

  auto repadBuffer = [&](juce::AudioBuffer<float> &buffer) {
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

  auto repadFloatVector = [&](std::vector<float> &values) {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<float> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)));
    std::copy(values.begin() + contentStart, values.end(),
              shifted.begin() + newOffsetFrames);
    values = std::move(shifted);
  };

  auto repadBoolVector = [&](std::vector<bool> &values) {
    const int oldSize = static_cast<int>(values.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    std::vector<bool> shifted(static_cast<size_t>(
        newOffsetFrames + std::max(0, oldSize - contentStart)), false);
    for (int i = contentStart; i < oldSize; ++i)
      shifted[static_cast<size_t>(newOffsetFrames + i - contentStart)] =
          values[static_cast<size_t>(i)];
    values = std::move(shifted);
  };

  auto repadMel = [&](std::vector<std::vector<float>> &frames) {
    const int oldSize = static_cast<int>(frames.size());
    const int contentStart = std::min(oldOffsetFrames, oldSize);
    const size_t numMels =
        oldSize > 0
            ? frames[static_cast<size_t>(std::min(contentStart, oldSize - 1))]
                  .size()
            : 0;
    std::vector<std::vector<float>> shifted(
        static_cast<size_t>(newOffsetFrames +
                            std::max(0, oldSize - contentStart)),
        std::vector<float>(numMels, 0.0f));
    std::move(frames.begin() + contentStart, frames.end(),
              shifted.begin() + newOffsetFrames);
    frames = std::move(shifted);
  };

  repadBuffer(audioData.waveform);
  repadBuffer(audioData.originalWaveform);
  repadMel(audioData.melSpectrogram);
  repadFloatVector(audioData.rawF0);
  repadFloatVector(audioData.cleanedF0);
  repadFloatVector(audioData.denseF0);
  repadFloatVector(audioData.f0);
  repadFloatVector(audioData.baseF0);
  repadFloatVector(audioData.basePitch);
  repadFloatVector(audioData.deltaPitch);
  repadBoolVector(audioData.voicedMask);
  repadBoolVector(audioData.vadMask);

  auto shiftFrame = [frameDelta](int frame) {
    return std::max(0, frame + frameDelta);
  };

  for (auto &note : project.getNotes()) {
    note.setStartFrame(shiftFrame(note.getStartFrame()));
    note.setEndFrame(shiftFrame(note.getEndFrame()));
    note.setSrcStartFrame(shiftFrame(note.getSrcStartFrame()));
    note.setSrcEndFrame(shiftFrame(note.getSrcEndFrame()));
  }

  for (auto &range : audioData.segmentChunkRanges) {
    range.first = shiftFrame(range.first);
    range.second = shiftFrame(range.second);
  }

  for (auto &chunk : audioData.segmentDebugChunks) {
    chunk.startFrame = shiftFrame(chunk.startFrame);
    chunk.endFrame = shiftFrame(chunk.endFrame);
    for (auto &event : chunk.events) {
      event.startFrame = shiftFrame(event.startFrame);
      event.endFrame = shiftFrame(event.endFrame);
      event.attachedStartFrame = shiftFrame(event.attachedStartFrame);
    }
  }

  audioData.timelineOffsetSeconds = newOffsetSeconds;
}

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

// STEP 5: under the whole-source model (Step 1), a project that has been
// analyzed at all (has f0 data) covers EVERY region built from that
// modification -- there is no more "this region's own slice" to check for,
// since the buffer is never truncated to one region's span. regionStart/End
// are kept as parameters so call sites don't need to change, but coverage no
// longer depends on them.
bool projectAppearsToCoverRegion(const Project &project, double regionStart,
                                 double regionEnd) {
  juce::ignoreUnused(regionStart, regionEnd);
  return !project.getAudioData().f0.empty();
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

void setPlaybackRegionRangeToMaterialExtent(Project &project) {
  auto &audioData = project.getAudioData();
  const double start = std::max(0.0, audioData.timelineOffsetSeconds);
  const double end = static_cast<double>(audioData.getDuration());

  if (end > start)
    audioData.playbackRegionRanges = {{start, end}};
  else
    audioData.playbackRegionRanges.clear();
}

juce::String archivedRegionKeyForLiveKey(
    PitchNetAudioModification *modification, const juce::String &liveKey) {
  if (modification == nullptr || liveKey.isEmpty())
    return {};

  for (auto *region :
       modification->getPlaybackRegions<juce::ARAPlaybackRegion>())
    if (region != nullptr && pitchnetRegionKey(*region) == liveKey)
      return pitchnetArchivedRegionKey(*region);

  return {};
}

bool clearProcessedRegionAudio(PitchNetAudioModification *modification,
                               const juce::String &liveKey,
                               const juce::String &archivedKey) {
  if (modification == nullptr || liveKey.isEmpty())
    return false;

  bool cleared = modification->hasProcessedAudioForRegion(liveKey);
  modification->clearProcessedAudioForRegion(liveKey);
  if (archivedKey.isNotEmpty() && archivedKey != liveKey) {
    cleared = modification->hasProcessedAudioForRegion(archivedKey) || cleared;
    modification->clearProcessedAudioForRegion(archivedKey);
  }
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

  // Non-ARA capture controller
  captureController->prepare(sampleRate, getMainBusNumOutputChannels(),
                             MAX_CAPTURE_SECONDS);
  lastCaptureUiState = captureController->getState();

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
  if (auto *hostPlayHead = getPlayHead()) {
    if (auto info = hostPlayHead->getPosition())
      posInfo = *info;
  }

  processNonARAMode(buffer, posInfo,
                    isRealtime() == juce::AudioProcessor::Realtime::yes);
}

void PitchNetAudioProcessor::processBlockBypassed(
    juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
  juce::ignoreUnused(buffer, midiMessages);

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

  // Composite/document analysis may finish after the user has selected a
  // region. Never overwrite the active region Project: its undo history owns
  // pointers into that exact object.
  if (mainComponent &&
      (canvasShowsActiveAraRegion || regionCanvasAnalysisPending.load())) {
    // A region-local analysis owns the empty canvas and its popup until its
    // Project is ready. Composite analysis may continue for playback/cache,
    // but must not replace that UI state or hide its progress.
    if (!regionCanvasAnalysisPending.load())
      mainComponent->hideAnalysisProgress();
    return true;
  }

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

  if (mainComponent && !regionCanvasAnalysisPending.load())
  {
    mainComponent->setStatusMessage(TR("progress.analyzing"));
    mainComponent->showAnalysisProgress(0.0);
  }

  araAnalysisController->setHostAudioAsync(
      buffer, sampleRate,
      [this](double progress, const juce::String &msg) {
        if (regionCanvasAnalysisPending.load())
          return;
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

        if (mainComponent && araAnalysisReady &&
            !canvasShowsActiveAraRegion &&
            !regionCanvasAnalysisPending.load()) {
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
        } else if (mainComponent && !regionCanvasAnalysisPending.load()) {
          mainComponent->hideAnalysisProgress();
        } else if (!mainComponent && araAnalysisProjectSnapshot) {
          // Analysis can finish after the editor has closed. Publish and bind
          // the completed snapshot once here, rather than rebuilding it from
          // every subsequent audio callback.
          publishPersistentProjectSnapshot(*araAnalysisProjectSnapshot);
          bindRealtimeProcessorHeadless();
        }
      });
}

void PitchNetAudioProcessor::removeAraRegionFromProject(
    std::uintptr_t newSourceKey,
    const std::pair<double, double> &removedRange,
    const std::vector<std::pair<double, double>> &remainingRanges) {
  auto *project = canvasShowsActiveAraRegion
                      ? araAnalysisProjectSnapshot.get()
                      : (mainComponent ? mainComponent->getProject() : nullptr);
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
  clearFloats(audio.rawF0);
  clearFloats(audio.cleanedF0);
  clearFloats(audio.denseF0);
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
  if (!canvasShowsActiveAraRegion) {
    mainComponent->restoreProjectSnapshot(*updated);
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
  }
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
  if (!regionCanvasAnalysisPending.load()) {
    mainComponent->setStatusMessage(TR("progress.analyzing"));
    mainComponent->showAnalysisProgress(0.0);
  }
  controller->setHostAudioAsync(
      region, sampleRate,
      [this](double progress, const juce::String &message) {
        if (regionCanvasAnalysisPending.load())
          return;
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
        auto *currentProject = canvasShowsActiveAraRegion
                                   ? araAnalysisProjectSnapshot.get()
                                   : mainComponent->getProject();
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
        if (!canvasShowsActiveAraRegion) {
          mainComponent->restoreProjectSnapshot(*merged);
          mainComponent->bindRealtimeProcessor(realtimeProcessor);
        }
        if (!regionCanvasAnalysisPending.load()) {
          mainComponent->hideAnalysisProgress();
          mainComponent->setStatusMessage({});
        }
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
  const auto renderPlaybackRegionKey = activePlaybackRegionKey;
  auto *renderModification = activeModification;
  const double renderRegionStartSeconds = activeRegionStartSeconds;
  const double renderRegionEndSeconds = activeRegionEndSeconds;
  // The Project being rendered (projectToRender/backendProject) is whatever
  // the UI currently has loaded, which is playback-time shifted by this
  // amount (see updateActiveRegionTiming()). The renderer/master storage need
  // it back at modification-time (shift -> 0); the live display stays as-is.
  const double renderRegionTimelineShiftSeconds =
      activeRegionTimelineShiftSeconds;
  // STEP 3: no per-region dirty-range slicing anymore -- the resynthesised
  // waveform is published to the modification whole, at offset 0, so the
  // old changed-sample-range/merge machinery is no longer needed here.
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
       renderPlaybackRegionKey, renderModification,
       renderRegionStartSeconds, renderRegionEndSeconds,
       renderRegionTimelineShiftSeconds](bool success) {
        auto *renderedProject = controller != nullptr ? controller->getProject()
                                                     : nullptr;

        if (success && renderedProject) {
#if JucePlugin_Enable_ARA
          if (renderActiveAraRegion && renderRegionKey.isNotEmpty()) {
            // renderedProject mirrors whatever the UI had loaded, so its
            // audioData is already playback-time shifted by
            // renderRegionTimelineShiftSeconds -- do NOT relabel
            // timelineOffsetSeconds here, that would mismatch label vs.
            // content. The editor highlight should cover the loaded material,
            // not the selected host slice that happened to trigger rendering.
            setPlaybackRegionRangeToMaterialExtent(*renderedProject);

            // renderRegionKey alone is not enough: sibling regions from a
            // slice/track-version-duplicate share it (it's modification/
            // source-scoped), so it stays equal to activeRegionKey even
            // after the user has switched to a DIFFERENT sibling region
            // while this async render was in flight. A plain slice doesn't
            // move or restretch audio, so its two halves share the exact
            // same playback-vs-modification-time shift -- comparing the
            // ACTUAL REGION OBJECT (juce::ARAPlaybackRegion*) would also
            // reject that harmless case if the host ever recreates the
            // region's wrapper object for unrelated reasons between dispatch
            // and completion, silently skipping the merge below even though
            // nothing unsafe happened. Comparing the shift itself instead
            // only rejects the genuinely unsafe case -- a sibling at a
            // materially different timeline position (e.g. a track-version
            // duplicate placed elsewhere) -- without being sensitive to
            // object identity that isn't actually load-bearing here.
            const bool targetIsLiveDisplay =
                renderRegionKey == activeRegionKey &&
                renderPlaybackRegionKey == activePlaybackRegionKey &&
                juce::approximatelyEqual(activeRegionTimelineShiftSeconds,
                                         renderRegionTimelineShiftSeconds) &&
                canvasShowsActiveAraRegion && mainComponent != nullptr;

            Project storageProject(*renderedProject);
            repadProjectTimeline(storageProject, renderRegionTimelineShiftSeconds,
                                 0.0);

            if (targetIsLiveDisplay && mainComponent->getProject() != nullptr) {
              // Live display: target is in the SAME playback-time-shifted
              // coordinates as renderedProject -- merge directly.
              mergeRenderedState(*mainComponent->getProject(), *renderedProject);
            }

            // ARA storage/rendering must stay anchored in audio-modification
            // time. The live UI project may be playback-time shifted, so never
            // publish or archive that display copy as persistent ARA state.
            auto &storedState = araRegions[renderRegionKey];
            storedState.project = std::make_unique<Project>(storageProject);

            Project &storedProject = storageProject;
            publishPersistentProjectSnapshot(storedProject);

            if (renderModification != nullptr) {
              juce::MemoryBlock projectArchive;
              if (ProjectSerializer::toBinaryArchive(
                      storedProject, projectArchive,
                      ProjectSerializer::BinaryArchiveMode::hostBackedARA) &&
                  projectArchive.getSize() > 0)
                renderModification->setProjectArchiveForRegion(
                    renderRegionKey, projectArchive.getData(),
                    projectArchive.getSize());
            }

            // Publish the WHOLE rendered material under the SHARED
            // modification/source key (renderRegionKey), not
            // renderPlaybackRegionKey (a per-region identity). This buffer is
            // the whole-source, modification-time-anchored project; every
            // sibling region shares it and finds its own portion via
            // mixProcessedRegionAudio()'s offset math -- publishing it under
            // a per-region key instead means only the one sibling whose own
            // key happens to match would ever find it, leaving edits made
            // while any OTHER sibling was active silently unheard until that
            // specific region was reselected. This storage project is
            // modification-time anchored; the composite publisher expects
            // timeline-anchored data, so do not route this path through
            // publishCompositeEditsToRegions().
            auto &storedAudioData = storedProject.getAudioData();
            if (renderModification != nullptr &&
                projectHasRegionEdits(storedProject) &&
                storedAudioData.waveform.getNumSamples() > 0) {
              const double processedRate =
                  storedAudioData.sampleRate > 0
                      ? static_cast<double>(storedAudioData.sampleRate)
                      : hostSampleRate;
              renderModification->setProcessedAudioForRegion(
                  renderRegionKey, storedAudioData.waveform, processedRate,
                  0);
              renderModification->notifyContentChanged(
                  juce::ARAContentUpdateScopes::samplesAreAffected(), true);
              for (auto *region : renderModification->getPlaybackRegions())
                if (region != nullptr)
                  region->notifyContentChanged(
                      juce::ARAContentUpdateScopes::samplesAreAffected(), true);
            } else if (clearProcessedRegionAudio(
                           renderModification, renderRegionKey,
                           juce::String{})) {
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
             renderRegionKey, renderPlaybackRegionKey,
             renderRegionTimelineShiftSeconds]() {
              if (auto *view =
                      dynamic_cast<IMainView *>(renderSafeMain.getComponent())) {
                if (success) {
                  // Only re-pad the display if this render's region is still
                  // the one on screen (same guard as targetIsLiveDisplay
                  // above -- a track-version duplicate elsewhere on the
                  // timeline has a materially different shift and must not
                  // be applied to whatever the user has since switched to).
                  // This must pass the SHIFT, not renderRegionStartSeconds
                  // (raw playback time) -- passing playback time here mis-
                  // pads the display for any region whose modification-time
                  // start isn't near zero, i.e. most regions past the first.
                  const bool stillLiveDisplay =
                      !renderActiveAraRegion ||
                      (renderRegionKey == activeRegionKey &&
                       renderPlaybackRegionKey == activePlaybackRegionKey &&
                       juce::approximatelyEqual(
                           activeRegionTimelineShiftSeconds,
                           renderRegionTimelineShiftSeconds));
                  if (stillLiveDisplay)
                    view->updateHostAudioTimelineOffset(
                        renderActiveAraRegion ? renderRegionTimelineShiftSeconds
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
    // `project` mirrors the live UI canvas, which is playback-time shifted by
    // activeRegionTimelineShiftSeconds. Build a SEPARATE, properly re-padded
    // copy anchored back at modification-time (0) for caching/persistence --
    // do NOT force timelineOffsetSeconds to 0 without moving the content, and
    // do NOT touch the live uiProject's own data here at all: it is already
    // correctly shifted by updateHostAudioTimelineOffset(), and stomping its
    // label without repadding its content corrupts the very state that
    // function relies on for every subsequent region switch.
    araRegionScopedProject = std::make_unique<Project>(project);
    repadProjectTimeline(*araRegionScopedProject,
                         activeRegionTimelineShiftSeconds, 0.0);

    if (mainComponent != nullptr) {
      if (auto *uiProject = mainComponent->getProject()) {
        setPlaybackRegionRangeToMaterialExtent(*uiProject);
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
  // REGION's own project.
  if (canvasShowsActiveAraRegion && activeRegionKey.isNotEmpty()) {
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
    // stateProject is already the modification-time-anchored (offset 0) copy
    // built above, so its waveform can be published as-is: every sibling
    // region sharing this modification finds its own portion inside this same
    // buffer via mixProcessedRegionAudio()'s offset math.
    if (activeModification != nullptr && projectHasRegionEdits(stateProject)) {
      const auto &processed = stateProject.getAudioData().waveform;
      const double processedRate =
          stateProject.getAudioData().sampleRate > 0
              ? static_cast<double>(stateProject.getAudioData().sampleRate)
              : hostSampleRate;
      if (processed.getNumSamples() > 0) {
        // Publish under the SHARED modification/source key (activeRegionKey),
        // not activePlaybackRegionKey (a per-region identity). `processed` is
        // the whole-source, modification-time-anchored buffer -- storing it
        // under a per-region key means only that one sibling's lookup (which
        // checks its own per-region key as a fallback) would ever find it;
        // every other sibling reading via the shared key would miss it and
        // silently fall back to raw/unedited audio. See the (still-accurate)
        // comment above this block: every sibling must find this same entry.
        activeModification->setProcessedAudioForRegion(
            activeRegionKey, processed, processedRate, 0);

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
      // Clear the same shared key the write path above publishes to.
      if (clearProcessedRegionAudio(activeModification, activeRegionKey,
                                    juce::String{})) {
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

  // The rendered-edit flag records that a note contributed to the persisted
  // composite without retaining a duplicate per-note audio buffer.
  for (const auto &note : project.getNotes())
    if (note.hasRenderedEdit())
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
    juce::MemoryBlock &destData, bool hostBackedARA) const {
  destData.setSize(0);

  const auto archiveMode =
      hostBackedARA
          ? ProjectSerializer::BinaryArchiveMode::hostBackedARA
          : ProjectSerializer::BinaryArchiveMode::selfContained;

  if (mainComponent) {
    if (auto *project = mainComponent->getProject())
      return ProjectSerializer::toBinaryArchive(*project, destData,
                                                archiveMode);
  }

  if (araAnalysisProjectSnapshot)
    return ProjectSerializer::toBinaryArchive(*araAnalysisProjectSnapshot,
                                              destData, archiveMode);

  if (araAnalysisController && araAnalysisController->getProject())
    return ProjectSerializer::toBinaryArchive(
        *araAnalysisController->getProject(), destData, archiveMode);

  if (pendingStateJson.isNotEmpty()) {
    Project pendingProject;
    if (ProjectSerializer::fromJson(
            pendingProject, juce::JSON::parse(pendingStateJson)))
      return ProjectSerializer::toBinaryArchive(pendingProject, destData,
                                                archiveMode);

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
  adoptMacroParameters(*restoredProject);

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
    adoptMacroParameters(*restoredProject);
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
               araAnalysisProjectJson.isNotEmpty()) {
      restoredPersistentProject = mc->restoreProjectJson(araAnalysisProjectJson);
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
  bool hostBackedARA = false;
#if JucePlugin_Enable_ARA
  // When this processor is attached to an ARA document, the host already owns
  // the immutable source and the ARA object stream persists ProcessedRegionData
  // for edited playback. Keep ordinary/non-ARA plugin state self-contained.
  hostBackedARA = araDocumentController != nullptr;
#endif
  serializePersistentProjectState(projectArchive, hostBackedARA);

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

  const auto clearUndoHistories = [this]() {
    undoManager->clear();
    for (auto &[regionKey, regionState] : araRegions) {
      juce::ignoreUnused(regionKey);
      if (regionState.undoManager)
        regionState.undoManager->clear();
    }
    araRegions.clear();
#if JucePlugin_Enable_ARA
    activeRegionKey.clear();
    activePlaybackRegionKey.clear();
    activePlaybackRegion = nullptr;
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

void PitchNetAudioProcessor::setActiveAraRegion(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr)
    return;

  const auto key = pitchnetRegionKey(*region);
  if (key.isEmpty())
    return;
  const auto playbackRegionKey = pitchnetProcessedRegionKey(*region);

  if (key == activeRegionKey && playbackRegionKey == activePlaybackRegionKey) {
    updateActiveAraRegionProperties(region);
    // Studio One Event FX can bind and select the playback region before the
    // editor exists. That headless call records activeRegionKey but cannot
    // attach a Project or request region-canvas analysis. When the editor later
    // selects the same region, resume the missing UI initialisation instead of
    // treating the matching key as a completed activation.
    const bool analysisAlreadyPendingForRegion =
        regionCanvasAnalysisPending.load() &&
        pendingRegionCanvasAnalysisKey == key;
    if (mainComponent == nullptr || canvasShowsActiveAraRegion ||
        analysisAlreadyPendingForRegion)
      return;
  }

  // Return the outgoing region's actual Project to its store. Undo actions
  // retain pointers into this object, so copying a snapshot here would leave
  // that region's history pointing at destroyed notes and F0 arrays.
  if (activeRegionKey.isNotEmpty() && mainComponent != nullptr &&
      canvasShowsActiveAraRegion) {
    auto outgoing = mainComponent->exchangeProject(nullptr);
    if (outgoing) {
      // outgoing IS the retained, undo-tracked object (undo actions hold
      // pointers into its notes/F0 vectors per the comment above) -- it must
      // NOT be repadded/reallocated here. Its timelineOffsetSeconds is
      // already kept correct by updateHostAudioTimelineOffset() (called
      // whenever this region was activated/updated), so leave it alone;
      // only playbackRegionRanges (a plain label, not referenced by undo) is
      // safe to refresh directly. Whichever region reactivates this cached
      // entry next -- same region or a sibling at a different playback
      // position -- goes through updateHostAudioTimelineOffset() again,
      // which re-pads correctly from whatever offset is actually stored here.
      if (activeRegionEndSeconds > activeRegionStartSeconds)
        setPlaybackRegionRangeToMaterialExtent(*outgoing);
      araRegions[activeRegionKey].project = std::move(outgoing);
    }
  }

  activeRegionKey = key;
  activePlaybackRegionKey = playbackRegionKey;
  activePlaybackRegion = region;
  activeModification = region->getAudioModification<PitchNetAudioModification>();
  // activeRegionStartSeconds/EndSeconds are PLAYBACK time (arrangement
  // position) -- what the UI's timeline ruler and live playhead use.
  // activeRegionTimelineShiftSeconds is the separate playback-vs-modification
  // shift needed to re-anchor the shared (modification-time) buffer for
  // display. See updateActiveRegionTiming().
  updateActiveRegionTiming(region);

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
      if (activeModification->copyProjectArchiveForRegion(key, archive)) {
        restoreAraRegionProject(key, archive.getData(), archive.getSize());
        it = araRegions.find(key);
      }
    }
    if (it == araRegions.end() || !it->second.project) {
      const auto archivedKey = pitchnetArchivedRegionKey(*region);
      if (archivedKey.isNotEmpty() && archivedKey != key) {
        if (activeModification != nullptr) {
          juce::MemoryBlock archive;
          if (activeModification->copyProjectArchiveForRegion(archivedKey,
                                                              archive)) {
            restoreAraRegionProject(archivedKey, archive.getData(),
                                    archive.getSize());
          }
        }
        // restoreAraRegionProject() may synchronously migrate, hydrate, and
        // display the archived state under the live key when ARA sample access
        // is already enabled. Do not replace that hydrated Project below with
        // another copy of the source-less archived shell.
        if (canvasShowsActiveAraRegion &&
            mainComponent->getProject() != nullptr)
          return;
        if (const auto archivedIt = araRegions.find(archivedKey);
            archivedIt != araRegions.end() && archivedIt->second.project &&
            projectAppearsToCoverRegion(*archivedIt->second.project,
                                        activeRegionStartSeconds,
                                        activeRegionEndSeconds) &&
            (it == araRegions.end() || !it->second.project)) {
          araRegions[key].project =
              std::make_unique<Project>(*archivedIt->second.project);
          it = araRegions.find(key);
        }
      }
    }

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
      mainComponent->updateHostAudioTimelineOffset(
          activeRegionTimelineShiftSeconds);
      if (auto *project = mainComponent->getProject()) {
        setPlaybackRegionRangeToMaterialExtent(*project);
      }
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

void PitchNetAudioProcessor::updateActiveRegionTiming(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr)
    return;

  activeStartSampleInModification =
      region->getStartInAudioModificationSamples();
  activeRegionStartSeconds = std::max(0.0, region->getStartInPlaybackTime());
  activeRegionEndSeconds =
      std::max(activeRegionStartSeconds, region->getEndInPlaybackTime());

  auto *modification =
      region->getAudioModification<PitchNetAudioModification>();
  auto *rateSource = modification ? modification->getAudioSource() : nullptr;
  const double modRate = rateSource ? rateSource->getSampleRate() : 0.0;
  const double modificationStartSeconds =
      modRate > 0.0
          ? static_cast<double>(activeStartSampleInModification) / modRate
          : activeRegionStartSeconds;
  activeRegionTimelineShiftSeconds =
      activeRegionStartSeconds - modificationStartSeconds;
}

void PitchNetAudioProcessor::updateActiveAraRegionProperties(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr)
    return;

  const auto key = pitchnetRegionKey(*region);
  const auto playbackRegionKey = pitchnetProcessedRegionKey(*region);
  if (key.isEmpty() || key != activeRegionKey ||
      (region != activePlaybackRegion &&
       playbackRegionKey != activePlaybackRegionKey))
    return;

  activePlaybackRegionKey = playbackRegionKey;
  activePlaybackRegion = region;
  activeModification = region->getAudioModification<PitchNetAudioModification>();
  // This runs when a DIFFERENT ARAPlaybackRegion object shares the active key
  // (e.g. the sibling region a slice creates) -- recompute timing for THIS
  // region instead of leaving the previous region's timing in place.
  updateActiveRegionTiming(region);

  if (mainComponent != nullptr && canvasShowsActiveAraRegion) {
    // Re-pads the shared (modification-time-anchored) buffer to this
    // region's playback-time shift; also sets audioData.timelineOffsetSeconds
    // to the shift internally, so it must NOT be set again below.
    mainComponent->updateHostAudioTimelineOffset(
        activeRegionTimelineShiftSeconds);

    if (auto *project = mainComponent->getProject()) {
      // Keep the editor highlight tied to the loaded source/material, not to
      // the current Cubase slice wrapper.
      setPlaybackRegionRangeToMaterialExtent(*project);

      publishPersistentProjectSnapshot(*project);

      if (araAnalysisReady)
        mainComponent->bindRealtimeProcessor(realtimeProcessor);
    }
  } else if (auto it = araRegions.find(key);
             it != araRegions.end() && it->second.project) {
    // Stored-but-not-displayed: leave timelineOffsetSeconds as whatever it
    // actually is (its content is not being moved here); only the label-only
    // playbackRegionRanges is safe to refresh without a repad.
    setPlaybackRegionRangeToMaterialExtent(*it->second.project);
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
       analysisGeneration,
       timelineOffsetSeconds](const juce::AudioBuffer<float> &) {
        if (!regionCanvasController ||
            regionCanvasAnalysisGeneration.load() != analysisGeneration)
          return;
        auto *project = regionCanvasController->getProject();
        if (!project)
          return;

        project->getAudioData().timelineOffsetSeconds =
            std::max(0.0, timelineOffsetSeconds);
        setPlaybackRegionRangeToMaterialExtent(*project);

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
          // Rare recovery path (editor was torn down mid-analysis): no actual
          // ARAPlaybackRegion object is available here to read real
          // playback-time placement from, so fall back to "no shift" rather
          // than reusing a stale value from whatever region was active
          // before. The canvas will show the whole buffer at its own anchor
          // until a real selection event calls updateActiveRegionTiming().
          activeRegionStartSeconds = std::max(0.0, timelineOffsetSeconds);
          activeRegionEndSeconds = project->getAudioData().getDuration();
          activeStartSampleInModification = startSampleInModification;
          activeRegionTimelineShiftSeconds = 0.0;
        }

        // Paint it onto the canvas if it is still the active region.
        if (regionKey == activeRegionKey && mainComponent) {
          const double latestStart = activeRegionStartSeconds;
          const double latestEnd = activeRegionEndSeconds;
          auto displaced = mainComponent->exchangeProject(
              std::move(araRegions[regionKey].project));
          juce::ignoreUnused(displaced);
          mainComponent->bindUndoManager(regionUndoManager);
          // The host may move the region while analysis is running. The
          // analyzed Project is anchored at modification sample 0; re-pad it
          // to the active region's CURRENT playback-time shift (kept fresh by
          // updateActiveRegionTiming()) before exposing it, so content and
          // boundary move together.
          mainComponent->updateHostAudioTimelineOffset(
              activeRegionTimelineShiftSeconds);
          if (auto *positionedProject = mainComponent->getProject())
            setPlaybackRegionRangeToMaterialExtent(*positionedProject);
          mainComponent->bindRealtimeProcessor(realtimeProcessor);
          canvasShowsActiveAraRegion = true;

          // Once the newly analysed project is on the canvas, bring its ARA
          // region into view instead of leaving the viewport at the prior one.
          mainComponent->focusTimelineRange(latestStart, latestEnd);
        }

        if (modification != nullptr) {
          juce::MemoryBlock projectArchive;
          if (serializeAraRegionProject(regionKey, projectArchive))
            modification->setProjectArchiveForRegion(
                regionKey, projectArchive.getData(), projectArchive.getSize());
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

void PitchNetAudioProcessor::removeAraRegion(
    const juce::String &regionKey) {
  if (regionKey.isEmpty())
    return;

  if (regionKey == activeRegionKey) {
    if (mainComponent != nullptr && canvasShowsActiveAraRegion &&
        activeRegionKey.isNotEmpty()) {
      auto removedProject = mainComponent->exchangeProject(nullptr);
      juce::ignoreUnused(removedProject);
      mainComponent->bindUndoManager(undoManager.get());
    }
    activeModification = nullptr;
    activeRegionKey.clear();
    activePlaybackRegionKey.clear();
    activePlaybackRegion = nullptr;
    canvasShowsActiveAraRegion = false;
  }

  araRegions.erase(regionKey);
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
  if (audioData.waveform.getNumSamples() > 0 &&
      audioData.originalWaveform.getNumSamples() > 0 &&
      !audioData.melSpectrogram.empty())
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

  // STEP 1 follow-up: hydratedSource is now the WHOLE audio source (Step 1),
  // not a per-region padded buffer, so the old check (does this match the
  // archived region's OWN end in playbackRegionRanges) no longer applies --
  // playbackRegionRanges is just the focus window now, usually far shorter
  // than the source. Sanity-check against the analyzed frame count instead:
  // f0 was computed from this same whole-source waveform originally.
  if (!audioData.f0.empty()) {
    const auto expectedSamples = static_cast<juce::int64>(audioData.f0.size()) *
                                 static_cast<juce::int64>(HOP_SIZE);
    const auto actualSamples =
        static_cast<juce::int64>(hydratedSource->getNumSamples());
    if (std::abs(expectedSamples - actualSamples) >
        static_cast<juce::int64>(HOP_SIZE))
      return false;
  }

  audioData.sampleRate = juce::roundToInt(projectSampleRate);
  if (audioData.originalWaveform.getNumSamples() <= 0)
    audioData.originalWaveform.makeCopyOf(*hydratedSource);

  if (audioData.melSpectrogram.empty()) {
    const auto &sourceWaveform = audioData.originalWaveform;
    MelSpectrogram melComputer(audioData.sampleRate, N_FFT, HOP_SIZE,
                               NUM_MELS, FMIN, FMAX);
    audioData.melSpectrogram = melComputer.compute(
        sourceWaveform.getReadPointer(0), sourceWaveform.getNumSamples());
    if (audioData.melSpectrogram.empty())
      return false;
  }

  // Prefer the separately persisted region render as the authoritative
  // composite waveform. It already contains every saved edit, including
  // global processing.
  // The project remains timeline-anchored, so start with the pristine source
  // (including its leading timeline padding) and replace the current region.
  if (audioData.waveform.getNumSamples() <= 0) {
    audioData.waveform.makeCopyOf(*hydratedSource);

    if (regionKey == activeRegionKey && activeModification != nullptr) {
      juce::AudioBuffer<float> processedAudio;
      double processedRate = 0.0;
      juce::int64 processedStartInModification = 0;
      bool hasProcessedAudio =
          activeModification->copyProcessedAudioForRegion(
              regionKey, processedAudio, processedRate,
              processedStartInModification);
      if (!hasProcessedAudio) {
        const auto archivedKey =
            archivedRegionKeyForLiveKey(activeModification, regionKey);
        if (archivedKey.isNotEmpty() && archivedKey != regionKey)
          hasProcessedAudio =
              activeModification->copyProcessedAudioForRegion(
                  archivedKey, processedAudio, processedRate,
                  processedStartInModification);
      }

      if (hasProcessedAudio)
        replaceTimelineRegionWithProcessedAudio(
            audioData.waveform, projectSampleRate, processedAudio,
            processedRate, processedStartInModification,
            activeStartSampleInModification, sourceSampleRate,
            activeRegionStartSeconds, activeRegionEndSeconds);
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
  mainComponent->updateHostAudioTimelineOffset(
      activeRegionTimelineShiftSeconds);
  if (auto *project = mainComponent->getProject())
    setPlaybackRegionRangeToMaterialExtent(*project);
  mainComponent->bindRealtimeProcessor(realtimeProcessor);
  canvasShowsActiveAraRegion = true;
  mainComponent->hideAnalysisProgress();
  mainComponent->focusTimelineRange(activeRegionStartSeconds,
                                    activeRegionEndSeconds);
  return true;
}

void PitchNetAudioProcessor::restoreAraRegionProject(const juce::String &regionKey,
                                                     const void *data,
                                                     size_t sizeInBytes) {
  if (regionKey.isEmpty() || data == nullptr || sizeInBytes == 0)
    return;

  auto installRestoredProject = [this, &regionKey](
                                    std::unique_ptr<Project> project) {
    attachMacroParameters(*project);

    // ARA restores region archives under their stable modification/index key,
    // while the live editor uses the host-object key. The active region may
    // already have started a fresh analysis before its indexed archive arrives.
    // Migrate a late archive immediately and invalidate that analysis so its
    // original notes cannot overwrite the restored edits.
    juce::String liveKey;
    if (regionKey == activeRegionKey) {
      liveKey = activeRegionKey;
    } else if (activeModification != nullptr && activeRegionKey.isNotEmpty() &&
               archivedRegionKeyForLiveKey(activeModification,
                                           activeRegionKey) == regionKey &&
               projectAppearsToCoverRegion(*project,
                                           activeRegionStartSeconds,
                                           activeRegionEndSeconds)) {
      liveKey = activeRegionKey;
    }

    const bool needsLiveKeyMigration =
        liveKey.isNotEmpty() && liveKey != regionKey;
    auto &archivedState = araRegions[regionKey];
    archivedState.project = needsLiveKeyMigration
                                ? std::make_unique<Project>(*project)
                                : std::move(project);
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
    if (needsLiveKeyMigration)
      liveState.project = std::move(project);
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
    mainComponent->updateHostAudioTimelineOffset(
        activeRegionTimelineShiftSeconds);
    if (auto *positionedProject = mainComponent->getProject())
      setPlaybackRegionRangeToMaterialExtent(*positionedProject);
    mainComponent->bindRealtimeProcessor(realtimeProcessor);
    canvasShowsActiveAraRegion = true;
    mainComponent->hideAnalysisProgress();
    mainComponent->focusTimelineRange(activeRegionStartSeconds,
                                      activeRegionEndSeconds);
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

PitchNetAudioProcessor::~PitchNetAudioProcessor() = default;
#endif
