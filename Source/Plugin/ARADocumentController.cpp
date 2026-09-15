#include "ARADocumentController.h"
#include "../Utils/AudioResampler.h"

#if JucePlugin_Enable_ARA

#include "../Models/ProjectSerializer.h"
#include "../Utils/Constants.h"
#include "PluginProcessor.h"
#include "PitchNetAudioModification.h"
#include "../UI/IMainView.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "AraDiagnostics.h"

juce::String pitchnetRegionKeyForIndex(const juce::String &modificationID,
                                       int regionIndex) {
  if (modificationID.isEmpty() || regionIndex < 0)
    return {};

  return modificationID + ":" + juce::String(regionIndex);
}

juce::String pitchnetRegionKey(const juce::ARAPlaybackRegion &region) {
  const auto *modification = region.getAudioModification();
  if (modification == nullptr)
    return {};

  // ARA gives persistent identity to audio modifications, not to playback
  // regions: ARAPlaybackRegionProperties carries no persistentID, and both
  // persistence filters expose only sources and modifications. Every region
  // referencing a modification is a window onto the same edit layer, so they
  // all resolve to one key. Splitting a clip therefore costs nothing - the new
  // regions already find the edits through the modification they share.
  return juce::String(modification->getPersistentID());
}

juce::String
pitchnetArchivedRegionKey(const juce::ARAPlaybackRegion &region) {
  const auto *modification = region.getAudioModification();
  if (modification == nullptr)
    return {};

  // Retained as a distinct spelling only so call sites keep reading clearly.
  // Region index is no longer part of any live identity.
  return juce::String(modification->getPersistentID());
}

namespace {
constexpr juce::int64 kPitchNetAraModificationArchiveMagic =
    -0x504E41524D4F444LL; // -PNARMOD
// Alpha archive layout: one document-level project archive, followed by
// per-modification region projects and processed audio.
// v4: projects are stored in AUDIO MODIFICATION time, not timeline time, and
// identity is the modification rather than the playback region. v3 payloads are
// read for stream alignment only and then discarded - their note frames and
// waveforms are timeline-anchored, so reusing them here would place edits and
// rendered audio at the wrong offset. Affected modifications simply re-analyse.
constexpr int kPitchNetAraModificationArchiveVersion = 4;
constexpr int kPitchNetAraFirstModificationTimeArchiveVersion = 4;

juce::AudioBuffer<float> resampleAuditionBuffer(
    const juce::AudioBuffer<float> &source, double sourceRate,
    double destinationRate) {
  if (source.getNumSamples() <= 0 || !std::isfinite(sourceRate) ||
      !std::isfinite(destinationRate) || sourceRate <= 0.0 ||
      destinationRate <= 0.0 ||
      juce::approximatelyEqual(sourceRate, destinationRate)) {
    juce::AudioBuffer<float> copy;
    copy.makeCopyOf(source);
    return copy;
  }

  const int outputSamples = std::max(
      1, static_cast<int>(std::lround(source.getNumSamples() *
                                       destinationRate / sourceRate)));
  juce::AudioBuffer<float> output(source.getNumChannels(), outputSamples);
  for (int channel = 0; channel < source.getNumChannels(); ++channel) {
    const auto *input = source.getReadPointer(channel);
    auto *destination = output.getWritePointer(channel);
    for (int sample = 0; sample < outputSamples; ++sample) {
      const double inputPosition = std::min(
          static_cast<double>(source.getNumSamples() - 1),
          sample * sourceRate / destinationRate);
      const int left = static_cast<int>(inputPosition);
      const int right = std::min(source.getNumSamples() - 1, left + 1);
      const float fraction = static_cast<float>(inputPosition - left);
      const float leftSample = std::isfinite(input[left]) ? input[left] : 0.0f;
      const float rightSample = std::isfinite(input[right]) ? input[right] : 0.0f;
      const float value = leftSample + fraction * (rightSample - leftSample);
      destination[sample] = std::isfinite(value) ? value : 0.0f;
    }
  }
  return output;
}

bool readPlaybackRegionIntoBlock(
    juce::ARAPlaybackRegion *region, juce::ARAAudioSourceReader &reader,
    double outputSampleRate, juce::int64 blockStartSample,
    juce::AudioBuffer<float> &buffer, AraResamplingState *state = nullptr) {
  if (!region || !region->getAudioModification() || outputSampleRate <= 0.0)
    return false;

  auto *source = region->getAudioModification()->getAudioSource();
  if (!source || source->getSampleRate() <= 0.0)
    return false;

  const juce::Range<double> blockRange(
      static_cast<double>(blockStartSample) / outputSampleRate,
      static_cast<double>(blockStartSample + buffer.getNumSamples()) /
          outputSampleRate);
  const auto intersection =
      juce::Range<double>(region->getStartInPlaybackTime(),
                          region->getEndInPlaybackTime())
          .getIntersectionWith(blockRange);
  if (intersection.isEmpty())
    return false;

  const double sourceRate = source->getSampleRate();
  const auto sourceStart = region->getStartInAudioModificationSamples() +
      static_cast<juce::int64>(std::llround(
          (intersection.getStart() - region->getStartInPlaybackTime()) *
          sourceRate));
  const int outputStart = static_cast<int>(std::llround(
      (intersection.getStart() - blockRange.getStart()) * outputSampleRate));
  const int outputLength = std::min(
      buffer.getNumSamples() - outputStart,
      static_cast<int>(std::llround(intersection.getLength() *
                                    outputSampleRate)));
  if (outputStart < 0 || outputLength <= 0)
    return false;

  const int sourceChannels = source->getChannelCount();
  if (juce::approximatelyEqual(sourceRate, outputSampleRate))
    return reader.read(&buffer, outputStart, outputLength, sourceStart, true,
                       sourceChannels > 1);

  const double ratio = sourceRate / outputSampleRate;

  // Persistent resampler: continue reading from state.nextSourceSample and keep
  // interpolator phase across blocks; reset only on ratio change or a playhead
  // jump. Falls back to a fresh interpolator per call when state is null.
  juce::int64 readStart = sourceStart;
  if (state != nullptr) {
    const auto absoluteOutputStart = blockStartSample + outputStart;
    if (!state->initialised || !juce::approximatelyEqual(state->ratio, ratio) ||
        state->lastRenderedOutputEnd != absoluteOutputStart) {
      state->interpolators.clear();
      state->interpolators.reserve(
          static_cast<size_t>(std::max(1, buffer.getNumChannels())));
      for (int i = 0; i < std::max(1, buffer.getNumChannels()); ++i)
        state->interpolators.emplace_back();
      state->nextSourceSample = sourceStart;
      state->lastRenderedOutputEnd = absoluteOutputStart;
      state->ratio = ratio;
      state->initialised = true;
    }
    readStart = state->nextSourceSample;
  }

  const int sourceLength =
      std::max(1, static_cast<int>(std::ceil(outputLength * ratio)) + 16);
  juce::AudioBuffer<float> sourceBuffer(buffer.getNumChannels(), sourceLength);
  sourceBuffer.clear();
  if (!reader.read(&sourceBuffer, 0, sourceLength, readStart, true,
                   sourceChannels > 1))
    return false;

  int inputSamplesUsed = 0;
  for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
    juce::LagrangeInterpolator localInterpolator;
    juce::LagrangeInterpolator &interpolator =
        state != nullptr ? state->interpolators[static_cast<size_t>(ch)]
                         : localInterpolator;
    const int used = interpolator.process(
        ratio,
        sourceBuffer.getReadPointer(
            std::min(ch, sourceBuffer.getNumChannels() - 1)),
        buffer.getWritePointer(ch, outputStart), outputLength);
    inputSamplesUsed = std::max(inputSamplesUsed, used);
  }
  if (state != nullptr) {
    state->nextSourceSample += inputSamplesUsed;
    state->lastRenderedOutputEnd = blockStartSample + outputStart + outputLength;
  }
  return true;
}

// Mix a region's pre-rendered PROCESSED audio into the output at its timeline
// position. Mirrors readPlaybackRegionIntoBlock but sources from an in-memory
// buffer and adds into the output so multiple regions sum. processed[0]
// corresponds to modification sample processedStartInModification (the
// region's start in its modification when the audio was rendered); if the
// region has since been trimmed or shifted within its modification, the read
// position is offset so the processed audio stays aligned with the source
// material instead of anchoring blindly to the region's current start (which
// played the wrong part of the take after a trim). Ranges the processed audio
// does not cover return false so the caller falls back to the raw source.
// Returns true if anything was mixed.
static bool mixProcessedRegionAudio(juce::AudioBuffer<float> &buffer,
                                    const juce::ARAPlaybackRegion &region,
                                    const juce::AudioBuffer<float> &processed,
                                    double processedRate,
                                    juce::int64 processedStartInModification,
                                    double modificationSampleRate,
                                    double outputSampleRate,
                                    juce::int64 blockStartSample,
                                    AraResamplingState *state = nullptr) {
  if (processedRate <= 0.0 || outputSampleRate <= 0.0 ||
      processed.getNumSamples() <= 0)
    return false;

  const juce::Range<double> blockRange(
      static_cast<double>(blockStartSample) / outputSampleRate,
      static_cast<double>(blockStartSample + buffer.getNumSamples()) /
          outputSampleRate);
  const auto intersection =
      juce::Range<double>(region.getStartInPlaybackTime(),
                          region.getEndInPlaybackTime())
          .getIntersectionWith(blockRange);
  if (intersection.isEmpty())
    return false;

  // Seconds (in modification time) between the region's current start and the
  // start the processed audio was rendered for. Positive when the region's
  // left edge was trimmed later into the take.
  const double modificationOffsetSeconds =
      modificationSampleRate > 0.0
          ? static_cast<double>(region.getStartInAudioModificationSamples() -
                                processedStartInModification) /
                modificationSampleRate
          : 0.0;

  const auto sourceStart = static_cast<juce::int64>(std::llround(
      ((intersection.getStart() - region.getStartInPlaybackTime()) +
       modificationOffsetSeconds) *
      processedRate));
  const int outputStart = static_cast<int>(std::llround(
      (intersection.getStart() - blockRange.getStart()) * outputSampleRate));
  const int outputLength = std::min(
      buffer.getNumSamples() - outputStart,
      static_cast<int>(std::llround(intersection.getLength() * outputSampleRate)));
  if (outputStart < 0 || outputLength <= 0 || sourceStart < 0 ||
      sourceStart >= processed.getNumSamples())
    return false;

  if (juce::approximatelyEqual(processedRate, outputSampleRate)) {
    const int samplesToCopy =
        std::min(outputLength,
                 processed.getNumSamples() - static_cast<int>(sourceStart));
    if (samplesToCopy <= 0)
      return false;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
      buffer.addFrom(ch, outputStart, processed,
                     std::min(ch, processed.getNumChannels() - 1),
                     static_cast<int>(sourceStart), samplesToCopy);
    return true;
  }

  const double ratio = processedRate / outputSampleRate;

  // Persistent resampler: continue from state.nextSourceSample and keep phase
  // across blocks; reset on ratio change or playhead jump. Fresh per call when
  // state is null.
  juce::int64 readStart = sourceStart;
  if (state != nullptr) {
    const auto absoluteOutputStart = blockStartSample + outputStart;
    if (!state->initialised || !juce::approximatelyEqual(state->ratio, ratio) ||
        state->lastRenderedOutputEnd != absoluteOutputStart) {
      state->interpolators.clear();
      state->interpolators.reserve(
          static_cast<size_t>(std::max(1, buffer.getNumChannels())));
      for (int i = 0; i < std::max(1, buffer.getNumChannels()); ++i)
        state->interpolators.emplace_back();
      state->nextSourceSample = sourceStart;
      state->lastRenderedOutputEnd = absoluteOutputStart;
      state->ratio = ratio;
      state->initialised = true;
    }
    readStart = state->nextSourceSample;
  }
  if (readStart < 0 || readStart >= processed.getNumSamples())
    return false;

  const int sourceLength =
      std::max(1, static_cast<int>(std::ceil(outputLength * ratio)) + 16);
  const int copyLen = std::min(
      sourceLength, processed.getNumSamples() - static_cast<int>(readStart));
  if (copyLen <= 0)
    return false;

  juce::AudioBuffer<float> sourceBuffer(std::max(1, processed.getNumChannels()),
                                        sourceLength);
  sourceBuffer.clear();
  for (int ch = 0; ch < sourceBuffer.getNumChannels(); ++ch)
    sourceBuffer.copyFrom(ch, 0, processed,
                          std::min(ch, processed.getNumChannels() - 1),
                          static_cast<int>(readStart), copyLen);

  juce::AudioBuffer<float> resampled(buffer.getNumChannels(), outputLength);
  resampled.clear();
  int inputSamplesUsed = 0;
  for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
    juce::LagrangeInterpolator localInterpolator;
    juce::LagrangeInterpolator &interpolator =
        state != nullptr ? state->interpolators[static_cast<size_t>(ch)]
                         : localInterpolator;
    const int used = interpolator.process(
        ratio,
        sourceBuffer.getReadPointer(
            std::min(ch, sourceBuffer.getNumChannels() - 1)),
        resampled.getWritePointer(ch), outputLength);
    inputSamplesUsed = std::max(inputSamplesUsed, used);
  }
  for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    buffer.addFrom(ch, outputStart, resampled, ch, 0, outputLength);
  if (state != nullptr) {
    state->nextSourceSample += inputSamplesUsed;
    state->lastRenderedOutputEnd = blockStartSample + outputStart + outputLength;
  }
  return true;
}
} // namespace

//==============================================================================
// PitchNetPlaybackRenderer
//==============================================================================

PitchNetDocumentController *
PitchNetPlaybackRenderer::getDocController() const {
  auto *docController = getDocumentController();
  return juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);
}

void PitchNetPlaybackRenderer::prepareToPlay(
    double sampleRateIn, int maxBlockSize, int numChannelsIn,
    juce::AudioProcessor::ProcessingPrecision,
    AlwaysNonRealtime alwaysNonRealtime) {
  sampleRate = sampleRateIn;
  numChannels = numChannelsIn;
  tempBuffer =
      std::make_unique<juce::AudioBuffer<float>>(numChannels, maxBlockSize);

  bool useBuffered = (alwaysNonRealtime == AlwaysNonRealtime::no);
  juce::ignoreUnused(useBuffered);

  if (auto *docCtrl = getDocController()) {
    docCtrl->ensureHeadlessPlaybackBinding();
    docCtrl->prepareDocumentPlayback(sampleRate, maxBlockSize);
    docCtrl->processPlaybackRegions(getPlaybackRegions(), sampleRate);
  }

  // Readers and resampler slots for the regions assigned so far. Regions the
  // host assigns later arrive via didAddPlaybackRegion(), also on the model
  // thread, so processBlock() never has to allocate.
  for (auto *region : getPlaybackRegions<juce::ARAPlaybackRegion>())
    ensureRenderResourcesFor(region);
}

void PitchNetPlaybackRenderer::releaseResources() {
  readers.clear();
  rawResamplingStates.clear();
  processedResamplingStates.clear();
  tempBuffer.reset();
}

void PitchNetPlaybackRenderer::ensureRenderResourcesFor(
    juce::ARAPlaybackRegion *region) {
  // Model thread only (prepareToPlay and region assignment). Allocating the
  // reader and both resampler slots here is what keeps processBlock()
  // allocation-free.
  if (region == nullptr)
    return;

  rawResamplingStates.try_emplace(region);
  processedResamplingStates.try_emplace(region);
#if PITCHNET_ARA_DIAGNOSTICS
  diagnosticRenderStates.try_emplace(region);
#endif

  auto *modification = region->getAudioModification();
  auto *source =
      modification != nullptr ? modification->getAudioSource() : nullptr;
  if (source != nullptr && readers.find(source) == readers.end())
    readers.emplace(source,
                    std::make_unique<juce::ARAAudioSourceReader>(source));

  ARA_DIAG("renderResources region=" + ARA_DIAG_PTR(region) +
           " mod=" + ARA_DIAG_PTR(modification) + " readerOk=" +
           juce::String(source != nullptr &&
                        readers.find(source) != readers.end() ? 1 : 0));
}

void PitchNetPlaybackRenderer::didAddPlaybackRegion(
    ARA::PlugIn::PlaybackRegion *playbackRegion) noexcept {
  ensureRenderResourcesFor(
      static_cast<juce::ARAPlaybackRegion *>(playbackRegion));
}

void PitchNetPlaybackRenderer::willRemovePlaybackRegion(
    ARA::PlugIn::PlaybackRegion *playbackRegion) noexcept {
  // Drop resampler state keyed by this pointer so a later region that reuses
  // the address cannot inherit it. Readers are keyed by audio source and are
  // cleared in releaseResources().
  auto *region = static_cast<juce::ARAPlaybackRegion *>(playbackRegion);
  rawResamplingStates.erase(region);
  processedResamplingStates.erase(region);
#if PITCHNET_ARA_DIAGNOSTICS
  diagnosticRenderStates.erase(region);
#endif
}

bool PitchNetPlaybackRenderer::renderProcessedRegions(
    juce::AudioBuffer<float> &buffer, juce::int64 timeInSamples,
    int numSamples) {
  // Per-region playback (VocalNet's model): a region that was CHANGED plays
  // the processed audio stored on its ARA modification; a region that was NOT
  // changed plays its raw ARA source. This is per-region and works with the
  // editor closed. The output buffer must already be cleared; regions sum into
  // it.
  bool renderedAny = false;

  const juce::Range<double> blockRange(
      static_cast<double>(timeInSamples) / sampleRate,
      static_cast<double>(timeInSamples + numSamples) / sampleRate);

  for (auto *region : getPlaybackRegions<juce::ARAPlaybackRegion>()) {
    if (!region || !region->getAudioModification())
      continue;

    const bool intersectsBlock =
        !juce::Range<double>(region->getStartInPlaybackTime(),
                             region->getEndInPlaybackTime())
             .getIntersectionWith(blockRange)
             .isEmpty();

    bool renderedRegion = false;

    if (auto *modification =
            region->getAudioModification<PitchNetAudioModification>()) {
      const auto lock = modification->tryLockProcessedAudio();
      if (lock.isLocked()) {
        // Strictly the region's OWN processed audio. Do NOT fall back to some
        // other region's processed data (the old getOnlyProcessedRegionData()
        // fallback): with several regions/tracks sharing one modification it
        // made every unedited region play the edited region's audio, so only
        // the region currently being edited played back correctly.
        const auto regionKey = pitchnetRegionKey(*region);
        const auto archivedKey = pitchnetArchivedRegionKey(*region);
        const auto *data = modification->getProcessedRegionData(regionKey);
        if (data == nullptr && archivedKey.isNotEmpty() &&
            archivedKey != regionKey)
          data = modification->getProcessedRegionData(archivedKey);
        if (data != nullptr && data->hasAudio()) {
          auto *source = modification->getAudioSource();
          const double modificationRate =
              source != nullptr ? source->getSampleRate() : 0.0;
          // Never operator[] here: it would insert and allocate on the audio
          // thread. A missing slot degrades resampling quality for a block
          // rather than dropping the region.
          auto processedState = processedResamplingStates.find(region);
          renderedRegion = mixProcessedRegionAudio(
              buffer, *region, data->audio, data->sampleRate,
              data->startSampleInModification, modificationRate, sampleRate,
              timeInSamples,
              processedState != processedResamplingStates.end()
                  ? &processedState->second
                  : nullptr);
        }
      }
    }

    bool rawAttempted = false;
    bool rawHadResources = false;

    if (!renderedRegion && intersectsBlock) {
      auto *source = region->getAudioModification()->getAudioSource();
      auto it = source != nullptr ? readers.find(source) : readers.end();
      auto rawState = rawResamplingStates.find(region);
      rawAttempted = true;
      rawHadResources =
          (it != readers.end() && rawState != rawResamplingStates.end());
      // Resources are created on the model thread by ensureRenderResourcesFor().
      // A missing resampler slot must not mean silence: readPlaybackRegionIntoBlock
      // accepts a null state and falls back to a fresh interpolator per call, which
      // allocates nothing. Only a missing reader forces us to skip.
      if (it != readers.end()) {
        AraResamplingState *rawStatePtr =
            rawState != rawResamplingStates.end() ? &rawState->second : nullptr;
        tempBuffer->clear();
        if (readPlaybackRegionIntoBlock(region, *it->second, sampleRate,
                                        timeInSamples, *tempBuffer,
                                        rawStatePtr)) {
          for (int ch = 0; ch < std::min(buffer.getNumChannels(),
                                         tempBuffer->getNumChannels());
               ++ch)
            buffer.addFrom(ch, 0, *tempBuffer, ch, 0, numSamples);
          renderedRegion = true;
        }
      }
    }

#if PITCHNET_ARA_DIAGNOSTICS
    // Fires only when this region's outcome changes - a move, a resize, or a
    // transition to/from silence. Never per block.
    if (intersectsBlock) {
      auto *mod = region->getAudioModification<PitchNetAudioModification>();
      const auto *rd =
          mod != nullptr
              ? mod->getProcessedRegionData(pitchnetRegionKey(*region))
              : nullptr;
      DiagnosticRenderState now{region->getStartInPlaybackTime(),
                                region->getStartInAudioModificationSamples(),
                                rd ? rd->audio.getNumSamples() : -1,
                                renderedRegion,
                                rd ? araDiagFingerprint(rd->audio) : 0.0f};
      auto previous = diagnosticRenderStates.find(region);
      if (previous != diagnosticRenderStates.end() &&
          !(previous->second == now)) {
        previous->second = now;
        ARA_DIAG("render region=" + ARA_DIAG_PTR(region) +
                 " mod=" + ARA_DIAG_PTR(mod) +
                 " id=" + (mod ? juce::String(mod->getPersistentID())
                               : juce::String("-")) +
                 " startInMod=" + juce::String(now.startInModification) +
                 " playbackStart=" + juce::String(now.playbackStart, 3) +
                 " blob=" + (rd ? juce::String(rd->audio.getNumSamples())
                                : juce::String("none")) +
                 " blobOffset=" + (rd ? juce::String(rd->startSampleInModification)
                                      : juce::String("-")) +
                 " renderedProcessed=" + juce::String(renderedRegion ? 1 : 0) +
                 " fp=" + juce::String(now.blobFingerprint, 6));
      }
    }
#endif

    renderedAny = renderedAny || renderedRegion;
  }

  return renderedAny;
}

bool PitchNetPlaybackRenderer::processBlock(
    juce::AudioBuffer<float> &buffer, juce::AudioProcessor::Realtime realtime,
    const juce::AudioPlayHead::PositionInfo &posInfo) noexcept {
  // Get document controller for accessing MainComponent
  auto *docCtrl = getDocController();
  if (!docCtrl) {
    buffer.clear();
    return true;
  }

  // Produce nothing while the host is mutating the model graph: any region
  // this block would read may be freed before the block finishes.
  //
  // "Nothing" has to mean silence, not passthrough. Every other exit from this
  // function clears, and neither processBlockForARA() nor the processor's ARA
  // branch clears afterwards, so returning early without clearing would hand
  // the host's own input buffer to the output for the length of an editing
  // cycle.
  const auto processingLock = docCtrl->getProcessingLock();
  if (!processingLock.isLocked()) {
    buffer.clear();
    return true;
  }

  auto timeInSamples = posInfo.getTimeInSamples().orFallback(0);
  bool isPlaying = posInfo.getIsPlaying();
  int numSamples = buffer.getNumSamples();
  const bool shouldSyncUi = (realtime == juce::AudioProcessor::Realtime::yes);
  const bool hasPlaybackRegions = !getPlaybackRegions().empty();

  syncHostLoopState(docCtrl, posInfo, shouldSyncUi);

  auto notifyHostStopped = [&]() {
    if (shouldSyncUi && docCtrl && docCtrl->getMainComponent()) {
      auto state = hostUiSyncState;
      if (!state->stoppedPending.exchange(true)) {
        juce::Component::SafePointer<juce::Component> safeMain(
            docCtrl->getMainComponent()->getComponent());
        juce::MessageManager::callAsync([safeMain, state]() {
          state->stoppedPending.store(false);
          if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->notifyHostStopped();
        });
      }
    }
  };

  auto notifyHostPlayState = [&](bool playing) {
    if (shouldSyncUi && docCtrl && docCtrl->getMainComponent()) {
      auto state = hostUiSyncState;
      if (state->latestPlaying.exchange(playing) == playing)
        return;

      if (!state->playStatePending.exchange(true)) {
        juce::Component::SafePointer<juce::Component> safeMain(
            docCtrl->getMainComponent()->getComponent());
        juce::MessageManager::callAsync([safeMain, state]() {
          state->playStatePending.store(false);
          if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->updateHostPlaybackState(state->latestPlaying.load());
        });
      }
    }
  };

  if (!isPlaying) {
    notifyHostPlayState(false);
    notifyHostStopped();
    buffer.clear();
    return true;
  }

  if (!hasPlaybackRegions) {
    notifyHostPlayState(true);
    buffer.clear();
    return true;
  }

  notifyHostPlayState(true);

  // ARA transport playback is strictly per-region (VocalNet's model): a region
  // plays either the processed audio stored on its AudioModification (if it
  // was changed) or its raw ARA source (if not). The realtime playback engine
  // is deliberately NOT part of this path — it holds a single (edited/composite)
  // project shared across all plugin instances, so voicing it here bled the
  // wrong audio across tracks and masked publication bugs.
  buffer.clear();
  renderProcessedRegions(buffer, timeInSamples, numSamples);
  return true;
}

//==============================================================================
// PitchNetEditorRenderer
//==============================================================================

PitchNetDocumentController *PitchNetEditorRenderer::getDocController() const {
  auto *docController = getDocumentController();
  return juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);
}

PitchNetEditorRenderer::~PitchNetEditorRenderer() {
  for (auto *sequence : listenedRegionSequences)
    sequence->removeListener(this);
}

void PitchNetEditorRenderer::prepareToPlay(
    double sampleRateIn, int, int numChannelsIn,
    juce::AudioProcessor::ProcessingPrecision,
    AlwaysNonRealtime alwaysNonRealtime) {
  sampleRate = sampleRateIn;
  numChannels = numChannelsIn;
  previewBuffer = std::make_shared<juce::AudioBuffer<float>>(
      numChannels, static_cast<int>(std::ceil(sampleRate)));
  previewBuffer->clear();
  previousPreviewBuffer.reset();
  lastAuditionBuffer.reset();
  previousPreviewLoopPosition = 0;
  previewTransitionRemaining = 0;
  previewTransitionTotal = 0;
  lastPreviewStartTime = -1.0;
  lastPreviewEndTime = -1.0;
  lastPreviewRegion = nullptr;
  wasPreviewing = false;
  if (auto *docCtrl = getDocController())
    docCtrl->getPreviewState().editorRendererSampleRate.store(sampleRate);
  previewLoopRange = {};
  previewLoopPosition = 0;

  readers.clear();
  mayCreateReadersWhileRendering = (alwaysNonRealtime == AlwaysNonRealtime::yes);
  if (alwaysNonRealtime == AlwaysNonRealtime::yes)
    return;

  for (auto *region : getPlaybackRegions<juce::ARAPlaybackRegion>())
    ensureReaderFor(region);
}

void PitchNetEditorRenderer::ensureReaderFor(juce::ARAPlaybackRegion *region) {
  // Model thread only (prepareToPlay and region assignment).
  if (region == nullptr || region->getAudioModification() == nullptr)
    return;

  auto *source = region->getAudioModification()->getAudioSource();
  if (source != nullptr && readers.find(source) == readers.end())
    readers.emplace(source,
                    std::make_unique<juce::ARAAudioSourceReader>(source));
}

void PitchNetEditorRenderer::didAddPlaybackRegion(
    ARA::PlugIn::PlaybackRegion *) noexcept {
  // Do not touch the reader map here: this call is allowed while rendering, so
  // processBlock() may be reading it right now. Defer to configure().
  asyncConfigCallback.startConfigure();
}

void PitchNetEditorRenderer::didAddRegionSequence(
    ARA::PlugIn::RegionSequence *regionSequence) noexcept {
  auto *sequence = static_cast<juce::ARARegionSequence *>(regionSequence);
  if (sequence != nullptr && listenedRegionSequences.insert(sequence).second)
    sequence->addListener(this);
  asyncConfigCallback.startConfigure();
}

void PitchNetEditorRenderer::willRemoveRegionSequence(
    ARA::PlugIn::RegionSequence *regionSequence) noexcept {
  auto *sequence = static_cast<juce::ARARegionSequence *>(regionSequence);
  if (sequence != nullptr && listenedRegionSequences.erase(sequence) > 0)
    sequence->removeListener(this);
}

void PitchNetEditorRenderer::didAddPlaybackRegionToRegionSequence(
    juce::ARARegionSequence *, juce::ARAPlaybackRegion *) {
  // A region appearing in a sequence this renderer already previews needs a
  // reader too, and arrives without didAddPlaybackRegion() being called.
  asyncConfigCallback.startConfigure();
}

void PitchNetEditorRenderer::configure() {
  // Message thread, holding asyncConfigCallback's lock exclusively, so
  // processBlock() cannot be inside the reader map while it is rewritten.
  if (mayCreateReadersWhileRendering)
    return;

  forEachAssignedPlaybackRegion([this](juce::ARAPlaybackRegion *region) {
    ensureReaderFor(region);
    return true;
  });

  // REAPER can ask for a preview of a region it never assigned to this
  // renderer, and processBlock() honours that (see previewRegionIsAssigned).
  // Such a region is in neither list above, so prepare its reader explicitly.
  if (auto *docCtrl = getDocController())
    ensureReaderFor(docCtrl->getPreviewState().previewedRegion.load());
}

void PitchNetEditorRenderer::releaseResources() {
  readers.clear();
  previewBuffer.reset();
}

bool PitchNetEditorRenderer::readPlaybackRangeIntoBuffer(
    juce::Range<double> playbackRange, juce::ARAPlaybackRegion *region,
    juce::AudioBuffer<float> &buffer) {
  if (!region || !region->getAudioModification())
    return false;

  auto *audioModification = region->getAudioModification();
  auto *source = audioModification->getAudioSource();
  if (!source || source->getSampleRate() <= 0.0 || source->getChannelCount() <= 0)
    return false;

  auto sourceRange = juce::Range<double>(region->getStartInPlaybackTime(),
                                         region->getEndInPlaybackTime())
                         .getIntersectionWith(playbackRange);
  if (sourceRange.isEmpty())
    return false;

  auto it = readers.find(source);
  if (it == readers.end()) {
    // Reached from processBlock() via renderPreviewBuffer(). On a realtime
    // thread a miss means this region has no reader yet: give up rather than
    // allocate. Offline rendering is not realtime and has no pre-made readers,
    // so it is allowed to create one here.
    if (!mayCreateReadersWhileRendering)
      return false;
    it = readers.emplace(source, std::make_unique<juce::ARAAudioSourceReader>(
                                     source))
             .first;
  }

  const auto sourceSampleRate = source->getSampleRate();
  const int sourceChannels = source->getChannelCount();
  const auto inputOffset =
      static_cast<juce::int64>(std::llround(
          (sourceRange.getStart() - region->getStartInPlaybackTime()) *
          sourceSampleRate)) +
      region->getStartInAudioModificationSamples();
  const auto outputOffset = static_cast<juce::int64>(std::llround(
      (sourceRange.getStart() - playbackRange.getStart()) * sampleRate));
  const auto readLength = std::min(
      static_cast<juce::int64>(buffer.getNumSamples()) - outputOffset,
      static_cast<juce::int64>(
          std::llround(sourceRange.getLength() * sampleRate)));

  if (readLength <= 0 || outputOffset < 0)
    return false;

  if (juce::approximatelyEqual(sourceSampleRate, sampleRate)) {
    return it->second->read(&buffer, static_cast<int>(outputOffset),
                            static_cast<int>(readLength), inputOffset, true,
                            sourceChannels > 1);
  }

  const double ratio = sourceSampleRate / sampleRate;
  const int sourceSamplesToRead =
      std::max(1, static_cast<int>(std::ceil(readLength * ratio)) + 16);
  juce::AudioBuffer<float> sourceBuffer(buffer.getNumChannels(),
                                        sourceSamplesToRead);
  sourceBuffer.clear();
  if (!it->second->read(&sourceBuffer, 0, sourceSamplesToRead, inputOffset,
                        true, sourceChannels > 1))
    return false;

  for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
    juce::LagrangeInterpolator interpolator;
    interpolator.process(
        ratio, sourceBuffer.getReadPointer(std::min(ch, sourceChannels - 1)),
        buffer.getWritePointer(ch, static_cast<int>(outputOffset)),
        static_cast<int>(readLength));
  }

  return true;
}

void PitchNetEditorRenderer::renderPreviewBuffer(
    juce::ARAPlaybackRegion *region, double previewStartTime,
    double previewEndTime) {
  if (!previewBuffer) {
    if (region == nullptr || region->getAudioModification() == nullptr)
      return;

    if (auto *source = region->getAudioModification()->getAudioSource()) {
      if (source->getSampleRate() > 0.0)
        sampleRate = source->getSampleRate();
      if (source->getChannelCount() > 0)
        numChannels = source->getChannelCount();
    }

    previewBuffer = std::make_shared<juce::AudioBuffer<float>>(
        std::max(1, numChannels),
        static_cast<int>(std::ceil(std::max(1.0, sampleRate))));
    previewLoopRange = {};
    previewLoopPosition = 0;
  }

  const juce::Range<double> regionRange(region->getStartInPlaybackTime(),
                                        region->getEndInPlaybackTime());
  const auto previewRange =
      regionRange.getIntersectionWith({previewStartTime, previewEndTime});
  if (previewRange.isEmpty()) {
    previewLoopRange = {};
    return;
  }

  const auto previewSamples = static_cast<int>(
      std::max<juce::int64>(1, std::llround(previewRange.getLength() *
                                            sampleRate)));
  previewBuffer->setSize(numChannels, previewSamples, false, false, true);
  previewBuffer->clear();

  juce::AudioBuffer<float> input(numChannels, previewBuffer->getNumSamples());
  input.clear();

  bool renderedPreview = false;
  if (auto *modification =
          region->getAudioModification<PitchNetAudioModification>()) {
    const auto lock = modification->tryLockProcessedAudio();
    if (lock.isLocked()) {
      const auto regionKey = pitchnetRegionKey(*region);
      const auto archivedKey = pitchnetArchivedRegionKey(*region);
      const auto *data = modification->getProcessedRegionData(regionKey);
      if (data == nullptr && archivedKey.isNotEmpty() &&
          archivedKey != regionKey)
        data = modification->getProcessedRegionData(archivedKey);
      if (data != nullptr && data->hasAudio()) {
        auto *source = modification->getAudioSource();
        const double modificationRate =
            source != nullptr ? source->getSampleRate() : 0.0;
        const auto previewStartSample = static_cast<juce::int64>(
            std::llround(previewRange.getStart() * sampleRate));
        renderedPreview = mixProcessedRegionAudio(
            input, *region, data->audio, data->sampleRate,
            data->startSampleInModification, modificationRate, sampleRate,
            previewStartSample);
      }
    }
  }

  if (!renderedPreview &&
      !readPlaybackRangeIntoBuffer(previewRange, region, input)) {
    previewLoopRange = {};
    return;
  }

  previewBuffer->makeCopyOf(input);

  previewLoopRange = juce::Range<juce::int64>::withStartAndLength(
      0, previewBuffer->getNumSamples());
  previewLoopPosition = previewLoopRange.getStart();
}

void PitchNetEditorRenderer::writePreviewOnce(
    juce::AudioBuffer<float> &buffer) {
  buffer.clear();

  if (!previewBuffer || previewLoopRange.isEmpty()) {
    return;
  }

  const int sourceChannels = previewBuffer->getNumChannels();
  const int channelsToCopy = sourceChannels == 1
                                 ? buffer.getNumChannels()
                                 : std::min(buffer.getNumChannels(), sourceChannels);
  int written = 0;
  while (written < buffer.getNumSamples()) {
    const int available =
        static_cast<int>(previewLoopRange.getEnd() - previewLoopPosition);
    const int toCopy = std::min(buffer.getNumSamples() - written, available);
    for (int ch = 0; ch < channelsToCopy; ++ch) {
      const int sourceChannel = sourceChannels == 1 ? 0 : ch;
      buffer.copyFrom(ch, written, *previewBuffer, sourceChannel,
                      static_cast<int>(previewLoopPosition), toCopy);
    }
    written += toCopy;
    previewLoopPosition += toCopy;
    if (previewLoopPosition >= previewLoopRange.getEnd()) {
      previewLoopRange = {};
      break;
    }
  }

  juce::ignoreUnused(written);
}

void PitchNetEditorRenderer::writePreviewLoop(
    juce::AudioBuffer<float> &buffer) {
  buffer.clear();
  if (!previewBuffer || previewLoopRange.isEmpty())
    return;

  const int sourceChannels = previewBuffer->getNumChannels();
  const int channelsToCopy = sourceChannels == 1
                                 ? buffer.getNumChannels()
                                 : std::min(buffer.getNumChannels(), sourceChannels);
  const int loopStart = static_cast<int>(previewLoopRange.getStart());
  const int loopEnd = static_cast<int>(previewLoopRange.getEnd());
  const int loopLength = loopEnd - loopStart;
  const int crossfade = std::min(8192, std::max(1, loopLength / 2));
  const int crossfadeStart = loopEnd - crossfade;
  auto renderLoopSample = [](const juce::AudioBuffer<float> &source,
                             juce::int64 position, int channel) {
    const int length = source.getNumSamples();
    if (length <= 0 || channel < 0 || channel >= source.getNumChannels())
      return 0.0f;
    position %= length;
    if (position < 0)
      position += length;
    const int overlap = std::min(8192, std::max(1, length / 2));
    const int overlapStart = length - overlap;
    float value = source.getSample(channel, static_cast<int>(position));
    if (position >= overlapStart) {
      const float t = static_cast<float>(position - overlapStart) / overlap;
      value = value * std::cos(t * juce::MathConstants<float>::halfPi) +
              source.getSample(channel, static_cast<int>(position - overlapStart)) *
                  std::sin(t * juce::MathConstants<float>::halfPi);
    }
    return value;
  };
  auto advanceLoopPosition = [](const juce::AudioBuffer<float> &source,
                                juce::int64 &position) {
    const int length = source.getNumSamples();
    if (length <= 0) {
      position = 0;
      return;
    }
    position %= length;
    if (position < 0)
      position += length;
    const int overlap = std::min(8192, std::max(1, length / 2));
    if (++position >= length)
      position -= length - overlap;
  };
  for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
    const int position = static_cast<int>(previewLoopPosition);
    const bool inCrossfade = position >= crossfadeStart;
    const float t = inCrossfade
                        ? static_cast<float>(position - crossfadeStart) /
                              static_cast<float>(crossfade)
                        : 0.0f;
    const float tailGain = std::cos(t * juce::MathConstants<float>::halfPi);
    const float headGain = std::sin(t * juce::MathConstants<float>::halfPi);
    for (int ch = 0; ch < channelsToCopy; ++ch) {
      const int sourceChannel = sourceChannels == 1 ? 0 : ch;
      float value = previewBuffer->getSample(sourceChannel, position);
      if (inCrossfade)
        value = value * tailGain +
                previewBuffer->getSample(sourceChannel,
                                         loopStart + position - crossfadeStart) *
                    headGain;
      if (previewTransitionRemaining > 0 && previousPreviewBuffer &&
          (previousPreviewBuffer->getNumChannels() == 1 ||
           ch < previousPreviewBuffer->getNumChannels())) {
        const int previousSourceChannel =
            previousPreviewBuffer->getNumChannels() == 1 ? 0 : ch;
        const float oldValue = renderLoopSample(*previousPreviewBuffer,
                                                previousPreviewLoopPosition,
                                                previousSourceChannel);
        const float handoff = 1.0f - static_cast<float>(previewTransitionRemaining) /
                                           static_cast<float>(previewTransitionTotal);
        value = oldValue * std::cos(handoff * juce::MathConstants<float>::halfPi) +
                value * std::sin(handoff * juce::MathConstants<float>::halfPi);
      }
      buffer.setSample(ch, sample, std::isfinite(value) ? value : 0.0f);
    }
    ++previewLoopPosition;
    if (previewLoopPosition >= loopEnd)
      previewLoopPosition -= loopLength - crossfade;
    if (previewTransitionRemaining > 0 && previousPreviewBuffer &&
        previousPreviewBuffer->getNumSamples() > 0) {
      advanceLoopPosition(*previousPreviewBuffer, previousPreviewLoopPosition);
      --previewTransitionRemaining;
    }
  }
}

bool PitchNetEditorRenderer::readFromARARegions(
    juce::AudioBuffer<float> &buffer, juce::int64 timeInSamples,
    int numSamples) {
  buffer.clear();
  bool didRender = false;
  for (auto *region : getPlaybackRegions()) {
    if (!region || !region->getAudioModification())
      continue;

    auto *source = region->getAudioModification()->getAudioSource();
    auto it = readers.find(const_cast<juce::ARAAudioSource *>(source));
    if (it == readers.end() && source)
      it = readers.emplace(const_cast<juce::ARAAudioSource *>(source),
                           std::make_unique<juce::ARAAudioSourceReader>(
                               const_cast<juce::ARAAudioSource *>(source)))
               .first;
    if (it == readers.end())
      continue;

    juce::AudioBuffer<float> regionBuffer(buffer.getNumChannels(), numSamples);
    regionBuffer.clear();
    if (!readPlaybackRegionIntoBlock(region, *it->second, sampleRate,
                                     timeInSamples, regionBuffer))
      continue;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
      buffer.addFrom(ch, 0, regionBuffer, ch, 0, numSamples);
    didRender = true;
  }

  return didRender;
}

bool PitchNetEditorRenderer::processBlock(
    juce::AudioBuffer<float> &buffer, juce::AudioProcessor::Realtime realtime,
    const juce::AudioPlayHead::PositionInfo &posInfo) noexcept {
  juce::ignoreUnused(realtime);

  auto *docCtrl = getDocController();
  if (!docCtrl)
    return true;

  // See PitchNetPlaybackRenderer::processBlock(): the editor renderer reads
  // the same regions and needs the same exclusion during host graph edits.
  const auto processingLock = docCtrl->getProcessingLock();
  if (!processingLock.isLocked())
    return true;

  // Editor-renderer region assignment is allowed while rendering, so the
  // editing lock above does not cover it (see AraAsyncConfigurationCallback).
  // This second try-lock excludes the reader rebuild in configure().
  //
  // Deliberately no buffer.clear() on either early return: the playback
  // renderer has already written this block into the same buffer
  // (processBlockForARA calls it first), and clearing would silence it.
  const auto configLock = asyncConfigCallback.tryLockForRender();
  if (!configLock.isLocked())
    return true;

  auto &previewState = docCtrl->getPreviewState();
  const bool previewRequested = previewState.auditionActive.load() ||
                                previewState.previewedRegion.load() != nullptr;
  // Some ARA hosts momentarily report a playing transport while asking the
  // editor renderer for a stopped-preview block. Honour an explicit preview
  // request in that case; normal transport playback still takes the path
  // below when no preview is active.
  if (!posInfo.getIsPlaying() || previewRequested) {
    auto audition = std::atomic_load(&previewState.auditionBuffer);
    if (previewState.auditionActive.load() && audition) {
      auto *claimedRenderer = previewState.previewClaimedRenderer.load();
      if (claimedRenderer == nullptr) {
        PitchNetEditorRenderer *expected = nullptr;
        previewState.previewClaimedRenderer.compare_exchange_strong(expected,
                                                                     this);
        claimedRenderer = previewState.previewClaimedRenderer.load();
      }
      if (claimedRenderer != this) {
        buffer.clear();
        return true;
      }
      if (audition != lastAuditionBuffer) {
        // Allocated storage is not necessarily rendered audio. Only crossfade
        // from a preview that is still playing, never the initial scratch
        // buffer or a completed/stopped preview.
        const bool hasActivePreview = wasPreviewing && previewBuffer &&
                                      !previewLoopRange.isEmpty();
        previousPreviewBuffer = hasActivePreview ? std::move(previewBuffer)
                                                 : nullptr;
        previousPreviewLoopPosition = hasActivePreview ? previewLoopPosition : 0;
        previewTransitionTotal = 4096;
        previewTransitionRemaining = previousPreviewBuffer ? previewTransitionTotal : 0;
        previewBuffer = audition;
        previewLoopRange = juce::Range<juce::int64>::withStartAndLength(
            0, previewBuffer->getNumSamples());
        previewLoopPosition = previewLoopRange.getStart();
        lastAuditionBuffer = audition;
      }
      writePreviewLoop(buffer);
      if (!std::exchange(wasPreviewing, true))
        buffer.applyGainRamp(0, std::min(50, buffer.getNumSamples()), 0.0f,
                             1.0f);
      return true;
    }
    auto *previewRegion = previewState.previewedRegion.load();
    const auto previewRegionIsAssigned = [&]() {
      if (previewRegion == nullptr)
        return false;

      const auto &assignedRegions = getPlaybackRegions();
      // REAPER can request an editor preview before assigning this renderer a
      // region. The document-level preview selection is authoritative there.
      if (assignedRegions.empty())
        return true;

      for (auto *region : assignedRegions)
        if (region == previewRegion)
          return true;

      return false;
    }();

    if (!previewRegion || !previewRegionIsAssigned) {
      buffer.clear();
      lastPreviewStartTime = -1.0;
      lastPreviewEndTime = -1.0;
      lastPreviewRegion = nullptr;
      previewLoopRange = {};
      previewLoopPosition = 0;
      if (std::exchange(wasPreviewing, false))
        buffer.applyGainRamp(0, std::min(50, buffer.getNumSamples()), 1.0f,
                             0.0f);
      return true;
    }

    auto *claimedRenderer = previewState.previewClaimedRenderer.load();
    if (claimedRenderer != this) {
      if (claimedRenderer != nullptr) {
        buffer.clear();
        return true;
      }

      PitchNetEditorRenderer *expected = nullptr;
      if (!previewState.previewClaimedRenderer.compare_exchange_strong(
              expected, this)) {
        buffer.clear();
        return true;
      }
    }

    const auto previewGeneration =
        previewState.previewGeneration.load(std::memory_order_acquire);
    const double previewStartTime = previewState.previewStartTime.load();
    const double previewEndTime = previewState.previewEndTime.load();
    if (previewGeneration != lastPreviewGeneration ||
        !juce::approximatelyEqual(previewStartTime, lastPreviewStartTime) ||
        !juce::approximatelyEqual(previewEndTime, lastPreviewEndTime) ||
        previewRegion != lastPreviewRegion) {
      renderPreviewBuffer(previewRegion, previewStartTime, previewEndTime);
      lastPreviewGeneration = previewGeneration;
      lastPreviewStartTime = previewStartTime;
      lastPreviewEndTime = previewEndTime;
      lastPreviewRegion = previewRegion;
    }

    writePreviewOnce(buffer);
    if (!std::exchange(wasPreviewing, true))
      buffer.applyGainRamp(0, std::min(50, buffer.getNumSamples()), 0.0f,
                           1.0f);
    return true;
  }

  // During host playback the PlaybackRenderer already renders the program into
  // this buffer. The host calls both renderers with the same buffer
  // (processBlockForARA: playbackRenderer->processBlock then
  // editorRenderer->processBlock), so if we render and add the program here as
  // well, the two identical copies sum and the output is +6 dB too loud (for a
  // stereo source each channel doubles, which sounds like L and R were summed).
  // The editor renderer only auditions edits via the preview path above while
  // the transport is stopped; during playback it must contribute nothing.
  lastPreviewStartTime = -1.0;
  lastPreviewEndTime = -1.0;
  lastPreviewRegion = nullptr;
  previewLoopRange = {};
  previewLoopPosition = 0;
  wasPreviewing = false;
  return true;
}

void PitchNetPlaybackRenderer::syncHostLoopState(
    PitchNetDocumentController *docCtrl,
    const juce::AudioPlayHead::PositionInfo &posInfo, bool shouldSyncUi) {
  if (!shouldSyncUi || !docCtrl || !docCtrl->getMainComponent())
    return;

  HostLoopState loopState;
  loopState.enabled = posInfo.getIsLooping();

  if (auto loopPoints = posInfo.getLoopPoints()) {
    if (auto bpm = posInfo.getBpm()) {
      if (*bpm > 0.0) {
        loopState.startSeconds = loopPoints->ppqStart * 60.0 / *bpm;
        loopState.endSeconds = loopPoints->ppqEnd * 60.0 / *bpm;
        loopState.hasRange = loopState.endSeconds > loopState.startSeconds;
      }
    }
  }

  if (hasPreviousLoopState && loopState == previousLoopState)
    return;

  previousLoopState = loopState;
  hasPreviousLoopState = true;
  auto state = hostUiSyncState;
  state->latestLoopStartSeconds.store(loopState.startSeconds);
  state->latestLoopEndSeconds.store(loopState.endSeconds);
  state->latestLoopEnabled.store(loopState.enabled);
  state->latestLoopHasRange.store(loopState.hasRange);

  if (!state->loopPending.exchange(true)) {
    juce::Component::SafePointer<juce::Component> safeMain(
        docCtrl->getMainComponent()->getComponent());
    juce::MessageManager::callAsync([safeMain, state]() {
      state->loopPending.store(false);
      if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent())) {
        view->updateHostLoopRange(
            state->latestLoopStartSeconds.load(),
            state->latestLoopEndSeconds.load(),
            state->latestLoopEnabled.load(),
            state->latestLoopHasRange.load());
      }
    });
  }
}

//==============================================================================
// PitchNetDocumentController
//==============================================================================

PitchNetDocumentController::~PitchNetDocumentController() {
  mainComponent = nullptr;
  editorProcessor = nullptr;
  realtimeProcessor = nullptr;
  currentAudioSource = nullptr;
  // Final teardown joins directly instead of starting another joiner thread.
  if (analysisState)
    analysisState->cancel.store(true);
  if (analysisThread.joinable())
    analysisThread.join();
  if (analysisJoinerThread.joinable())
    analysisJoinerThread.join();
}

void PitchNetDocumentController::setMainComponent(IMainView *mc) {
  if (mc == nullptr && mainComponent != nullptr) {
    stopAnalysisThread();
    currentAudioSource = nullptr;
  }

  mainComponent = mc;
  if (mainComponent && pendingRestoredProjectData.getSize() > 0 &&
      !restoreProjectStateCallback) {
    juce::String jsonString(
        juce::CharPointer_UTF8(
            static_cast<const char *>(pendingRestoredProjectData.getData())),
        pendingRestoredProjectData.getSize());
    mainComponent->restoreProjectJson(jsonString);
  }
}
void PitchNetDocumentController::setPersistenceCallbacks(
    std::function<bool(juce::MemoryBlock &)> serializeProjectState,
    std::function<bool(const void *, size_t)> restoreProjectState) {
  serializeProjectStateCallback = std::move(serializeProjectState);
  restoreProjectStateCallback = std::move(restoreProjectState);
  if (restoreProjectStateCallback && pendingRestoredProjectData.getSize() > 0) {
    if (restoreProjectStateCallback(pendingRestoredProjectData.getData(),
                                    pendingRestoredProjectData.getSize()))
      pendingRestoredProjectData.setSize(0);
  }
}

void PitchNetDocumentController::restoreAraRegionProjectOrPend(
    const juce::String &regionKey, const void *data, size_t sizeInBytes) {
  if (regionKey.isEmpty() || data == nullptr || sizeInBytes == 0)
    return;

  if (auto *processor = getRegionCanvasProcessor()) {
    processor->restoreAraRegionProject(regionKey, data, sizeInBytes);
    return;
  }

  for (auto &pending : pendingRestoredRegionProjects) {
    if (pending.regionKey == regionKey) {
      pending.data.replaceAll(data, sizeInBytes);
      return;
    }
  }

  juce::MemoryBlock copy(data, sizeInBytes);
  pendingRestoredRegionProjects.push_back({regionKey, std::move(copy)});
}

void PitchNetDocumentController::flushPendingAraRegionProjects() {
  auto *processor = getRegionCanvasProcessor();
  if (processor == nullptr || pendingRestoredRegionProjects.empty())
    return;

  for (const auto &pending : pendingRestoredRegionProjects) {
    processor->restoreAraRegionProject(pending.regionKey,
                                       pending.data.getData(),
                                       pending.data.getSize());
  }

  pendingRestoredRegionProjects.clear();
}

RealtimePitchProcessor *PitchNetDocumentController::getRealtimeProcessor() {
  // Prefer the live (editor/processor) realtime processor, but ONLY when it is
  // actually ready. Headless — before any editor has bound its project — the
  // processor's realtime processor is attached but not ready, because the
  // restored project landed in documentProjectSnapshot. Returning the not-ready
  // processor here would force the playback renderer into its raw-ARA-source
  // fallback, so headless playback would lose all edits and differ from what
  // the editor shows. In that case use the document's own realtime processor,
  // which is restored from the saved project and prepared at the host rate.
  if (realtimeProcessor && realtimeProcessor->isReady())
    return realtimeProcessor;
  if (documentProjectSnapshot)
    return &documentRealtimeProcessor;
  return realtimeProcessor;
}

void PitchNetDocumentController::setOwningProcessor(
    PitchNetAudioProcessor *processor) {
  if (processor == nullptr) {
    owningProcessor = nullptr;
    setRealtimeProcessor(nullptr);
    setPersistenceCallbacks(nullptr, nullptr);
    return;
  }

  owningProcessor = processor;
  ensureHeadlessPlaybackBinding();
  flushPendingAraRegionProjects();
}

void PitchNetDocumentController::releaseOwningProcessor(
    PitchNetAudioProcessor *processor) {
  if (processor == nullptr || owningProcessor != processor)
    return;

  if (realtimeProcessor == &processor->getRealtimeProcessor())
    setRealtimeProcessor(nullptr);

  owningProcessor = nullptr;
  setPersistenceCallbacks(nullptr, nullptr);
}

void PitchNetDocumentController::setEditorProcessor(
    PitchNetAudioProcessor *processor) {
  editorProcessor = processor;
  flushPendingAraRegionProjects();
}

void PitchNetDocumentController::releaseEditorProcessor(
    PitchNetAudioProcessor *processor) {
  if (processor != nullptr && editorProcessor == processor)
    editorProcessor = nullptr;
}

PitchNetAudioProcessor *
PitchNetDocumentController::getRegionCanvasProcessor() const {
  if (editorProcessor != nullptr &&
      editorProcessor->getMainComponent() != nullptr)
    return editorProcessor;
  return owningProcessor;
}

void PitchNetDocumentController::ensureHeadlessPlaybackBinding() {
  if (!owningProcessor)
    return;

  setRealtimeProcessor(&owningProcessor->getRealtimeProcessor());
  setPersistenceCallbacks(
      [processor = owningProcessor](juce::MemoryBlock &destData) {
        return processor->serializePersistentProjectState(destData, true);
      },
      [processor = owningProcessor](const void *data, size_t sizeInBytes) {
        return processor->restorePersistentProjectState(data, sizeInBytes);
      });
}

bool PitchNetDocumentController::restoreProjectStateToDocument(
    const void *data, size_t sizeInBytes) {
  if (!data || sizeInBytes == 0)
    return false;

  auto restoredProject = std::make_unique<Project>();
  if (ProjectSerializer::fromBinaryArchive(*restoredProject, data,
                                           sizeInBytes)) {
    documentProjectSnapshot = std::move(restoredProject);
    documentRealtimeProcessor.setProject(documentProjectSnapshot.get());
    return true;
  }

  juce::String projectJson(
      juce::CharPointer_UTF8(static_cast<const char *>(data)), sizeInBytes);
  auto parsed = juce::JSON::parse(projectJson);
  if (!parsed.isObject() ||
      !ProjectSerializer::fromJson(*restoredProject, parsed))
    return false;

  documentProjectSnapshot = std::move(restoredProject);
  documentRealtimeProcessor.setProject(documentProjectSnapshot.get());
  return true;
}

bool PitchNetDocumentController::serializeDocumentProjectState(
    juce::MemoryBlock &destData) const {
  destData.setSize(0);
  if (!documentProjectSnapshot)
    return false;
  // An ARA document archive can always recover immutable audio from the host.
  // Edited playback is persisted separately as ProcessedRegionData, so storing
  // either project waveform or the source-derived mel here only duplicates the
  // largest payloads in the DAW project.
  return ProjectSerializer::toBinaryArchive(
      *documentProjectSnapshot, destData,
      ProjectSerializer::BinaryArchiveMode::hostBackedARA);
}

void PitchNetDocumentController::prepareDocumentPlayback(double sampleRate,
                                                         int maxBlockSize) {
  documentRealtimeProcessor.prepareToPlay(sampleRate, maxBlockSize);
  if (documentProjectSnapshot)
    documentRealtimeProcessor.setProject(documentProjectSnapshot.get());
}

void PitchNetDocumentController::setDocumentProjectSnapshot(
    const Project &project, bool notifyHost) {
  documentProjectSnapshot = std::make_unique<Project>(project);
  documentRealtimeProcessor.setProject(documentProjectSnapshot.get());
  if (notifyHost)
    notifyAudioModificationContentChanged(true);
}

void PitchNetDocumentController::notifyAudioModificationContentChanged(
    bool notifyHost) {
  auto notifyModification = [notifyHost](juce::ARAAudioModification *mod) {
    if (!mod)
      return;
    mod->notifyContentChanged(
        juce::ARAContentUpdateScopes::samplesAreAffected(), notifyHost);
    for (auto *region : mod->getPlaybackRegions())
      if (region)
        region->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected(), notifyHost);
  };

  if (currentDocument) {
    for (auto *source : currentDocument->getAudioSources<juce::ARAAudioSource>()) {
      if (!source)
        continue;
      for (auto *modification : source->getAudioModifications()) {
        if (!modification)
          continue;

        bool belongsToCurrentSequence = currentRegionSequence == nullptr;
        for (auto *region : modification->getPlaybackRegions()) {
          if (region && region->getRegionSequence() == currentRegionSequence) {
            belongsToCurrentSequence = true;
            break;
          }
        }

        if (belongsToCurrentSequence)
          notifyModification(modification);
      }
    }
    return;
  }

  if (currentPlaybackRegion)
    notifyModification(currentPlaybackRegion->getAudioModification());
}

void PitchNetDocumentController::stopAnalysisThread() {
  if (analysisState)
    analysisState->cancel.store(true);
  if (analysisThread.joinable()) {
    if (analysisJoinerThread.joinable())
      analysisJoinerThread.join();
    auto old = std::move(analysisThread);
    analysisJoinerThread = std::thread([t = std::move(old)]() mutable {
      if (t.joinable())
        t.join();
    });
  }
}
void PitchNetDocumentController::clearMainComponentHostAudio() {
  if (!mainComponent)
    return;

  stopAnalysisThread();
  currentAudioSource = nullptr;
  currentPlaybackRegions.clear();

  juce::Component::SafePointer<juce::Component> safeMain(
      mainComponent->getComponent());
  juce::MessageManager::callAsync([safeMain]() {
    if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
      view->clearHostAudio();
  });
}

void PitchNetDocumentController::clearStaleRegionSequenceFilter(
    juce::ARADocument *document) {
  if (!document)
    return;

  if (!currentPlaybackRegions.empty()) {
    std::vector<juce::ARAPlaybackRegion *> liveRegions;
    for (auto *source : document->getAudioSources<juce::ARAAudioSource>()) {
      if (!source)
        continue;
      for (auto *modification : source->getAudioModifications()) {
        if (!modification)
          continue;
        for (auto *region : modification->getPlaybackRegions()) {
          if (std::find(currentPlaybackRegions.begin(),
                        currentPlaybackRegions.end(),
                        region) != currentPlaybackRegions.end())
            liveRegions.push_back(region);
        }
      }
    }

    currentPlaybackRegions = std::move(liveRegions);
    if (currentPlaybackRegions.empty())
      currentPlaybackRegion = nullptr;
  }

  if (!currentRegionSequence)
    return;

  for (auto *regionSequence :
       document->getRegionSequences<juce::ARARegionSequence>()) {
    if (regionSequence == currentRegionSequence)
      return;
  }

  // Pro Tools can keep the document controller alive while replacing the ARA
  // model objects during plugin unload/reload. If we keep filtering by the old
  // sequence pointer, every restored region is ignored.
  currentRegionSequence = nullptr;
  currentPlaybackRegion = nullptr;
}

bool PitchNetDocumentController::shouldProcessPlaybackRegion(
    juce::ARAPlaybackRegion *region) const {
  if (!region)
    return false;

  // The editor selection is more recent and more specific than the renderer's
  // cached region list. Some hosts do not rebuild that list when selection
  // moves between already-existing regions, so rejecting currentPlaybackRegion
  // here drops its subsequent position updates until the editor is reopened.
  if (region == currentPlaybackRegion)
    return true;

  if (!currentPlaybackRegions.empty())
    return std::find(currentPlaybackRegions.begin(),
                     currentPlaybackRegions.end(),
                     region) != currentPlaybackRegions.end();

  return !currentRegionSequence ||
         region->getRegionSequence() == currentRegionSequence;
}

void PitchNetDocumentController::didAddAudioSourceToDocument(
    juce::ARADocument *document, juce::ARAAudioSource *audioSource) {
  currentDocument = document;
  currentAudioSource = audioSource;
}

bool PitchNetDocumentController::processExistingAudioSources(
    juce::ARADocument *document) {
  if (!document)
    return false;

  clearStaleRegionSequenceFilter(document);

  currentDocument = document;
  bool hasSource = false;
  currentPlaybackRegion = nullptr;
  currentPlaybackRegions.clear();
  for (auto *source : document->getAudioSources<juce::ARAAudioSource>()) {
    if (!source || source->getSampleCount() <= 0 ||
        source->getChannelCount() <= 0 || source->getSampleRate() <= 0.0)
      continue;

    if (!hasSource) {
      hasSource = true;
      currentAudioSource = source;
    }
    for (auto *modification : source->getAudioModifications()) {
      if (!modification || currentPlaybackRegion)
        continue;
      for (auto *region : modification->getPlaybackRegions()) {
        if (!shouldProcessPlaybackRegion(region))
          continue;
        currentPlaybackRegion = region;
        if (!currentRegionSequence)
          currentRegionSequence = region->getRegionSequence();
        break;
      }
    }
  }

  // A valid source alone is not enough to initialise a region-based editor.
  // Some hosts (notably Studio One Event FX) expose the source before a usable
  // playback region is found and do not send an initial selection callback.
  return currentPlaybackRegion != nullptr;
}

bool PitchNetDocumentController::processPlaybackRegions(
    const std::vector<juce::ARAPlaybackRegion *> &playbackRegions,
    double projectSampleRate) {
  ensureHeadlessPlaybackBinding();

  auto *firstRegion = playbackRegions.empty() ? nullptr : playbackRegions.front();
  if (!firstRegion || !firstRegion->getAudioModification())
    return false;

  currentPlaybackRegion = firstRegion;
  currentPlaybackRegions = playbackRegions;
  analysisTimelineSampleRate = projectSampleRate > 0.0 ? projectSampleRate
                                                       : 0.0;
  currentRegionSequence = firstRegion->getRegionSequence();
  currentAudioSource = firstRegion->getAudioModification()->getAudioSource();
  currentDocument = currentAudioSource ? currentAudioSource->getDocument()
                                       : nullptr;
  if (!currentDocument)
    return false;

  return true;
}

void PitchNetDocumentController::willRemoveAudioSourceFromDocument(
    juce::ARADocument *document, juce::ARAAudioSource *audioSource) {
  if (!document || !audioSource)
    return;
  if (audioSource == currentAudioSource)
    currentAudioSource = nullptr;
  currentPlaybackRegions.erase(
      std::remove_if(currentPlaybackRegions.begin(),
                     currentPlaybackRegions.end(),
                     [audioSource](auto *region) {
                       auto *modification =
                           region ? region->getAudioModification() : nullptr;
                       return modification &&
                              modification->getAudioSource() == audioSource;
                     }),
      currentPlaybackRegions.end());
}

void PitchNetDocumentController::didEnableAudioSourceSamplesAccess(
    juce::ARAAudioSource *audioSource, bool enable) {
  // The analysis path reads the ARA source samples directly. On unload/reload
  // (e.g. removing and re-inserting the plugin), the host re-creates the audio
  // sources with sample access DISABLED and only enables it afterwards. Any
  // analysis kicked off before that point read silence and produced nothing.
  // When samples become readable, retry only the currently selected region.
  // Saved/restored region projects should be used as-is; we do not rebuild the
  // whole document just because the host toggled sample access.
  if (!enable || !audioSource)
    return;

  auto *document = audioSource->getDocument();
  if (!document)
    document = currentDocument;
  if (!document)
    return;

  clearStaleRegionSequenceFilter(document);
  currentAudioSource = audioSource;
  currentDocument = document;

  if (currentPlaybackRegion) {
    if (auto *modification = currentPlaybackRegion->getAudioModification();
        modification != nullptr &&
        modification->getAudioSource() == audioSource) {
      requestRegionCanvasAnalysis(currentPlaybackRegion);
    }
  }
}

void PitchNetDocumentController::requestRegionCanvasAnalysis(
    juce::ARAPlaybackRegion *region) {
  if (hostEditing)
    return;
  auto *processor = getRegionCanvasProcessor();
  if (region == nullptr || processor == nullptr)
    return;

  auto *modification = region->getAudioModification();
  auto *source = modification ? modification->getAudioSource() : nullptr;
  if (source == nullptr || !source->isSampleAccessEnabled() ||
      source->getSampleRate() <= 0.0)
    return; // Samples not ready yet; the sample-access handler will retry.

  const auto liveKey = pitchnetRegionKey(*region);
  ARA_DIAG("canvasRequest key=" + liveKey + " hasProject=" +
           juce::String(processor->hasAraRegionProject(liveKey) ? 1 : 0) +
           " needsHydration=" +
           juce::String(processor->araRegionProjectNeedsSourceHydration(liveKey)
                            ? 1 : 0));
  if (processor->hasAraRegionProject(liveKey) &&
      !processor->araRegionProjectNeedsSourceHydration(liveKey)) {
    processor->showAraRegionProjectIfActive(liveKey);
    return;
  }
  if (!processor->hasAraRegionProject(liveKey)) {
    auto *pitchModification =
        region->getAudioModification<PitchNetAudioModification>();
    if (pitchModification != nullptr) {
      juce::MemoryBlock archive;
      if (pitchModification->copyProjectArchiveForRegion(liveKey, archive)) {
        processor->restoreAraRegionProject(liveKey, archive.getData(),
                                           archive.getSize());
        if (!processor->araRegionProjectNeedsSourceHydration(liveKey) &&
            processor->showAraRegionProjectIfActive(liveKey))
          return;
      }
    }
  }

  // Analyse the whole audio modification rather than one region's slice. The
  // edits belong to the modification and every region referencing it is a
  // window onto the same material, so the project is stored in MODIFICATION
  // time with no leading timeline padding. Timeline placement is applied when
  // rendering and when drawing, not baked into the analysis.
  const double sourceSampleRate = source->getSampleRate();
  const auto sourceSampleCount = source->getSampleCount();
  if (sourceSampleCount <= 0 ||
      sourceSampleCount > std::numeric_limits<int>::max())
    return;

  const int numSamples = static_cast<int>(sourceSampleCount);
  juce::AudioBuffer<float> buffer(1, numSamples);
  buffer.clear();

  juce::ARAAudioSourceReader reader(source);
  if (!reader.read(&buffer, 0, numSamples, 0, true, false))
    return;

  // ARA region archives retain edit data, but not project waveforms or mel.
  // Reattach the source and rebuild mel/rendered audio now, without running
  // pitch detection or note segmentation again.
  if (processor->hasAraRegionProject(liveKey) &&
      processor->hydrateAraRegionProject(liveKey, buffer,
                                         sourceSampleRate)) {
    processor->showAraRegionProjectIfActive(liveKey);
    return;
  }

  auto *pitchModification =
      region->getAudioModification<PitchNetAudioModification>();
  ARA_DIAG("canvasFallThroughToAnalyse key=" + liveKey + " hasProject=" +
           juce::String(processor->hasAraRegionProject(liveKey) ? 1 : 0) +
           " needsHydration=" +
           juce::String(processor->araRegionProjectNeedsSourceHydration(liveKey)
                            ? 1 : 0) +
           " sourceSamples=" + juce::String(numSamples));
  processor->analyzeAraRegionForCanvas(liveKey, pitchModification,
                                       /*startSampleInModification*/ 0,
                                       /*timelineOffsetSeconds*/ 0.0, buffer,
                                       sourceSampleRate);
}

void PitchNetDocumentController::setCurrentPlaybackRegion(
    juce::ARAPlaybackRegion *region) {
  if (region == nullptr || region->getAudioModification() == nullptr)
    return;

  currentPlaybackRegion = region;
  currentRegionSequence = region->getRegionSequence();
  currentAudioSource = region->getAudioModification()->getAudioSource();
  currentDocument = currentAudioSource ? currentAudioSource->getDocument()
                                       : currentDocument;
}

void PitchNetDocumentController::willDestroyAudioSource(
    juce::ARAAudioSource *audioSource) {
  if (audioSource == currentAudioSource)
    currentAudioSource = nullptr;
}

void PitchNetDocumentController::didAddPlaybackRegionToRegionSequence(
    juce::ARARegionSequence *regionSequence,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!regionSequence || !playbackRegion)
    return;

  if (!currentPlaybackRegions.empty() ||
      (currentRegionSequence && currentRegionSequence != regionSequence))
    return;

  currentRegionSequence = regionSequence;
  currentPlaybackRegion = playbackRegion;
  currentDocument = regionSequence->getDocument();
}

void PitchNetDocumentController::willDestroyRegionSequence(
    juce::ARARegionSequence *regionSequence) {
  if (regionSequence != currentRegionSequence)
    return;

  currentRegionSequence = nullptr;
  currentPlaybackRegion = nullptr;
}

juce::ScopedTryReadLock PitchNetDocumentController::getProcessingLock() {
  return juce::ScopedTryReadLock{processBlockLock};
}

void PitchNetDocumentController::willBeginEditing(juce::ARADocument *document) {
  juce::ignoreUnused(document);
  hostEditing = true;
  deferredRegionUpdates.clear();

  // Exclude the audio thread for the duration of the host's graph edit. Paired
  // with exitWrite() in didEndEditing(); both must run exactly once per cycle.
  //
  // There is no longer anything to snapshot here. Edits belong to the audio
  // modification, so regions the host creates during this cycle - split parts,
  // glued events, duplicates - already resolve to the state they should have
  // through the modification they reference.
  ARA_DIAG("editBegin");
  processBlockLock.enterWrite();
}

void PitchNetDocumentController::didEndEditing(juce::ARADocument *document) {
  juce::ignoreUnused(document);
  hostEditing = false;

  // The graph is stable again. Let the audio thread back in before the tail
  // below, which reads source audio and dispatches analysis and must not run
  // with the render path locked out.
  processBlockLock.exitWrite();
  ARA_DIAG("editEnd");

  auto updates = std::move(deferredRegionUpdates);
  deferredRegionUpdates.clear();
  for (auto *region : updates)
    didUpdatePlaybackRegionProperties(region);

  if (auto *processor = getRegionCanvasProcessor();
      processor != nullptr && processor->getMainComponent() != nullptr &&
      currentPlaybackRegion != nullptr) {
    processor->setActiveAraRegion(currentPlaybackRegion);
    requestRegionCanvasAnalysis(currentPlaybackRegion);
  }
}

void PitchNetDocumentController::snapshotRegionState(
    juce::ARAPlaybackRegion &region) {
  auto *modification = region.getAudioModification<PitchNetAudioModification>();
  if (modification == nullptr)
    return;
  const auto liveKey = pitchnetRegionKey(region);
  const auto archiveKey = pitchnetArchivedRegionKey(region);
  if (archiveKey.isEmpty())
    return;
  juce::MemoryBlock archive;
  auto *processor = getRegionCanvasProcessor();
  if ((processor && processor->serializeAraRegionProject(liveKey, archive)) ||
      modification->copyProjectArchiveForRegion(liveKey, archive))
    modification->setProjectArchiveForRegion(archiveKey, archive.getData(),
                                             archive.getSize());
  juce::AudioBuffer<float> audio;
  double rate = 0.0;
  juce::int64 start = 0;
  if (modification->copyProcessedAudioForRegion(liveKey, audio, rate, start))
    modification->setProcessedAudioForRegion(archiveKey, audio, rate, start);
}

void PitchNetDocumentController::didUpdateAudioModificationProperties(
    juce::ARAAudioModification *audioModification) {
  if (auto *modification =
          dynamic_cast<PitchNetAudioModification *>(audioModification)) {
    const auto pendingClone = modification->getPendingClonedPersistentID();
    modification->adoptClonedRegionState();
    ARA_DIAG("modProps mod=" + ARA_DIAG_PTR(modification) +
             " id=" + juce::String(modification->getPersistentID()) +
             " pendingCloneId=" +
             (pendingClone.isEmpty() ? juce::String("none") : pendingClone) +
             " archiveKeys=" +
             juce::String((int)modification->getProjectArchiveRegionIDs().size()));
  }
}

void PitchNetDocumentController::willDeactivateAudioModificationForUndoHistory(
    juce::ARAAudioModification *audioModification, bool deactivate) {
  // ARA requires the host to destroy every playback region before deactivating
  // a modification, and reading or updating a deactivated one is invalid. Drop
  // the canvas binding but keep the cached state: redo can reactivate this same
  // object, and the host is not required to re-restore it from an archive.
  if (!deactivate)
    return;

  if (auto *modification =
          dynamic_cast<PitchNetAudioModification *>(audioModification))
    if (auto *processor = getRegionCanvasProcessor())
      processor->releaseAraModificationCanvas(modification);
}

void PitchNetDocumentController::willDestroyAudioModification(
    juce::ARAAudioModification *audioModification) {
  auto *modification =
      dynamic_cast<PitchNetAudioModification *>(audioModification);
  if (modification == nullptr)
    return;

  if (currentPlaybackRegion != nullptr &&
      currentPlaybackRegion->getAudioModification() == audioModification) {
    if (previewState.previewedRegion.load() == currentPlaybackRegion)
      previewState.previewedRegion.store(nullptr);
    currentPlaybackRegion = nullptr;
  }
  currentPlaybackRegions.erase(
      std::remove_if(currentPlaybackRegions.begin(),
                     currentPlaybackRegions.end(),
                     [audioModification](auto *region) {
                       return region == nullptr ||
                              region->getAudioModification() ==
                                  audioModification;
                     }),
      currentPlaybackRegions.end());

  if (auto *processor = getRegionCanvasProcessor())
    processor->forgetAraModification(modification);
}

void PitchNetDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification)
    return;

  if (playbackRegion != nullptr)
    ARA_DIAG("regionAdd region=" + ARA_DIAG_PTR(playbackRegion) +
             " mod=" + ARA_DIAG_PTR(audioModification) +
             " id=" + juce::String(audioModification->getPersistentID()) +
             " startInMod=" +
             juce::String(playbackRegion->getStartInAudioModificationSamples()) +
             " playback=[" +
             juce::String(playbackRegion->getStartInPlaybackTime(), 3) + "," +
             juce::String(playbackRegion->getEndInPlaybackTime(), 3) + "]");

  auto *audioSource = audioModification->getAudioSource();
  auto *document = audioSource ? audioSource->getDocument() : currentDocument;
  clearStaleRegionSequenceFilter(document);

  // Do not use shouldProcessPlaybackRegion() here.  Once the controller has
  // any tracked regions that predicate is a membership test, and a genuinely
  // new region cannot be a member until this callback inserts it below.  That
  // circular check made every live add get ignored until reopening the editor
  // rebuilt currentPlaybackRegions from the host.
  if (playbackRegion != nullptr && currentRegionSequence != nullptr &&
      playbackRegion->getRegionSequence() != currentRegionSequence)
    return;

  currentAudioSource = audioSource;
  currentDocument = document;
  currentPlaybackRegion = playbackRegion;
  if (playbackRegion &&
      std::find(currentPlaybackRegions.begin(), currentPlaybackRegions.end(),
                playbackRegion) == currentPlaybackRegions.end())
    currentPlaybackRegions.push_back(playbackRegion);
  currentRegionSequence = playbackRegion ? playbackRegion->getRegionSequence()
                                         : currentRegionSequence;

  // A region added while the editor is open is also the region the user is
  // creating, so make it the active canvas region immediately.  Do this only
  // with a live editor: the headless add/restore path already establishes its
  // selection when the editor is constructed, and pre-selecting it here would
  // make that later selection look like a no-op.
  if (auto *processor = getRegionCanvasProcessor();
      !hostEditing && playbackRegion != nullptr && processor != nullptr &&
      processor->getMainComponent() != nullptr)
    processor->setActiveAraRegion(playbackRegion);
}

void PitchNetDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification || !playbackRegion)
    return;

  snapshotRegionState(*playbackRegion);

  if (!shouldProcessPlaybackRegion(playbackRegion))
    return;

  auto *audioSource = audioModification->getAudioSource();
  if (!audioSource)
    return;

  deferredRegionUpdates.erase(std::remove(deferredRegionUpdates.begin(),
      deferredRegionUpdates.end(), playbackRegion), deferredRegionUpdates.end());
  if (playbackRegion == currentPlaybackRegion) {
    previewState.previewedRegion.store(nullptr);
    currentPlaybackRegion = nullptr;
  }
  currentPlaybackRegions.erase(std::remove(currentPlaybackRegions.begin(),
                                           currentPlaybackRegions.end(),
                                           playbackRegion),
                               currentPlaybackRegions.end());

  // snapshotRegionState() preserves an archive for version reactivation.
  // Release the live Project and undo manager in willDestroyPlaybackRegion().
  currentDocument = audioSource->getDocument();
}

void PitchNetDocumentController::didUpdatePlaybackRegionProperties(
    juce::ARAPlaybackRegion *playbackRegion) {
  if (playbackRegion != nullptr)
    ARA_DIAG("regionProps region=" + ARA_DIAG_PTR(playbackRegion) +
             " mod=" + ARA_DIAG_PTR(playbackRegion->getAudioModification()) +
             " startInMod=" +
             juce::String(playbackRegion->getStartInAudioModificationSamples()) +
             " playback=[" +
             juce::String(playbackRegion->getStartInPlaybackTime(), 3) + "," +
             juce::String(playbackRegion->getEndInPlaybackTime(), 3) + "]" +
             " hostEditing=" + juce::String(hostEditing ? 1 : 0));
  if (!playbackRegion)
    return;

  if (hostEditing) {
    deferredRegionUpdates.push_back(playbackRegion);
    return;
  }

  const auto updatedKey = pitchnetRegionKey(*playbackRegion);
  if (!shouldProcessPlaybackRegion(playbackRegion))
    return;

  // ARA hosts may update several regions in one edit transaction. While a new
  // region is being analysed, a property update from an older region must not
  // steal its canvas. Once analysis is idle, however, some hosts report a
  // manual region drag only as a property update (without a preceding editor
  // selection notification). In that case the moved region must become active
  // here, otherwise its boundary moves while its cached notes/waveform remain
  // at the old position until the editor is reopened.
  auto *processor = getRegionCanvasProcessor();
  const bool isProcessorActiveRegion =
      processor != nullptr && updatedKey == processor->getActiveAraRegionKey();
  const bool hasNoProcessorSelection =
      processor == nullptr || processor->getActiveAraRegionKey().isEmpty();
  const bool canFollowMovedRegion =
      processor != nullptr && !isProcessorActiveRegion &&
      !processor->isAraRegionCanvasAnalysisPending();

  if (isProcessorActiveRegion || hasNoProcessorSelection ||
      canFollowMovedRegion) {
    if (auto *audioModification = playbackRegion->getAudioModification()) {
      currentAudioSource = audioModification->getAudioSource();
      currentDocument = currentAudioSource ? currentAudioSource->getDocument()
                                           : currentDocument;
      currentPlaybackRegion = playbackRegion;
      currentRegionSequence = playbackRegion->getRegionSequence();
    }
  }

  if (canFollowMovedRegion)
    processor->setActiveAraRegion(playbackRegion);
  else if (processor != nullptr)
    processor->updateActiveAraRegionProperties(playbackRegion);
}

void PitchNetDocumentController::willDestroyPlaybackRegion(
    juce::ARAPlaybackRegion *playbackRegion) {
  deferredRegionUpdates.erase(std::remove(deferredRegionUpdates.begin(),
      deferredRegionUpdates.end(), playbackRegion), deferredRegionUpdates.end());
  if (playbackRegion == currentPlaybackRegion) {
    previewState.previewedRegion.store(nullptr);
    currentPlaybackRegion = nullptr;
  }

  // The removal callback has already cleared this region from the tracked
  // selection. Destruction must release its Project and undo manager even
  // when other regions remain selected.
  if (playbackRegion == nullptr)
    return;
  if (auto *processor = getRegionCanvasProcessor())
    processor->removeAraRegion(pitchnetRegionKey(*playbackRegion));
}
// Takes MODIFICATION seconds, because that is the space the Project - and so
// the piano roll the user is selecting in - lives in. Previously it took
// playback seconds while its only caller passed modification seconds, so the
// two agreed only while a region sat at timeline zero. Move the event and
// auditioning a span before the region start found no region at all (silence),
// while a span after it played audio offset by the region's timeline position.
void PitchNetDocumentController::startPreviewRange(
    double previewStartInModificationSeconds,
    double previewEndInModificationSeconds) {
  if (!currentDocument)
    return;

  // No time-stretching is supported, so a region's modification span and its
  // playback span have the same length.
  const auto regionSpanInModification = [](const juce::ARAPlaybackRegion *r) {
    const double start = r->getStartInAudioModificationTime();
    return std::pair<double, double>{
        start, start + (r->getEndInPlaybackTime() - r->getStartInPlaybackTime())};
  };

  const auto coversRequestedSpan = [&](const juce::ARAPlaybackRegion *r) {
    if (r == nullptr)
      return false;
    if (currentRegionSequence && r->getRegionSequence() != currentRegionSequence)
      return false;
    const auto [startInMod, endInMod] = regionSpanInModification(r);
    return previewEndInModificationSeconds > startInMod &&
           previewStartInModificationSeconds < endInMod;
  };

  // Prefer the region the user actually selected in the host. Splitting an
  // event produces several playback regions aliasing ONE audio modification,
  // so they all share a persistent ID and cannot be told apart by key - but the
  // host reports the selection through ARAViewSelection, and the editor stores
  // the resolved region here. Without this the search below simply takes the
  // first region that happens to overlap, which for duplicates means auditioning
  // through whichever copy the document enumerates first rather than the one
  // being edited.
  juce::ARAPlaybackRegion *previewRegion = nullptr;
  if (auto *selected = getCurrentPlaybackRegion(); coversRequestedSpan(selected))
    previewRegion = selected;

  // Nothing selected, or the selection does not cover the requested span: fall
  // back to whichever region overlaps it.
  if (previewRegion == nullptr) {
    for (auto *source : currentDocument->getAudioSources<juce::ARAAudioSource>()) {
      if (!source)
        continue;
      for (auto *modification : source->getAudioModifications()) {
        if (!modification)
          continue;
        for (auto *region : modification->getPlaybackRegions()) {
          if (!region ||
              (currentRegionSequence &&
               region->getRegionSequence() != currentRegionSequence))
            continue;
          const auto [regionStartInMod, regionEndInMod] =
              regionSpanInModification(region);
          if (previewEndInModificationSeconds > regionStartInMod &&
              previewStartInModificationSeconds < regionEndInMod) {
            previewRegion = region;
            break;
          }
        }
        if (previewRegion)
          break;
      }
      if (previewRegion)
        break;
    }
  }

  if (!previewRegion)
    return;

  // Clamp in modification space, then convert to the playback time the render
  // path downstream expects.
  const auto [regionStartInMod, regionEndInMod] =
      regionSpanInModification(previewRegion);
  const double startInMod = juce::jlimit(regionStartInMod, regionEndInMod,
                                         previewStartInModificationSeconds);
  const double endInMod = juce::jlimit(regionStartInMod, regionEndInMod,
                                       previewEndInModificationSeconds);
  if (endInMod <= startInMod)
    return;

  const double modificationToPlayback =
      previewRegion->getStartInPlaybackTime() - regionStartInMod;
  previewState.previewStartTime.store(startInMod + modificationToPlayback);
  previewState.previewEndTime.store(endInMod + modificationToPlayback);
  previewState.previewClaimedRenderer.store(nullptr);
  previewState.previewedRegion.store(previewRegion);
  // Last, so a render thread that observes the bump also sees the new range.
  previewState.previewGeneration.fetch_add(1, std::memory_order_release);

  // Model thread. A host may preview a region it never assigned to an editor
  // renderer, in which case no assignment callback fires and the renderer has
  // no reader for it. Ask for one now; the renderer does the thread bridging.
  for (auto *renderer :
       getDocumentController()->getEditorRenderers<PitchNetEditorRenderer>())
    if (renderer != nullptr)
      renderer->requestReaderConfiguration();
}

void PitchNetDocumentController::startPreviewAudio(
    const juce::AudioBuffer<float> &buffer, double sampleRate) {
  if (buffer.getNumSamples() <= 0)
    return;
  const double sourceRate = sampleRate > 0.0 ? sampleRate : 44100.0;
  const double rendererRate = previewState.editorRendererSampleRate.load();
  auto preview = std::make_shared<juce::AudioBuffer<float>>(
      resampleAuditionBuffer(buffer, sourceRate,
                             rendererRate > 0.0 ? rendererRate : sourceRate));
  if (preview->getNumSamples() <= 0 || preview->getNumChannels() <= 0)
    return;
  std::atomic_store(&previewState.auditionBuffer, std::move(preview));
  // startPreviewRange() has already selected a region and caused the host's
  // normal preview signal flow to run. Keep that selection intact while the
  // editor renderer substitutes this temporary resampled audition buffer.
  // Do not release the selected renderer for each drag update: another ARA
  // renderer may claim it and leave this editor's output silent.
  if (!previewState.auditionActive.exchange(true))
    previewState.previewClaimedRenderer.store(nullptr);
}

void PitchNetDocumentController::stopPreview() {
  previewState.auditionActive.store(false);
  std::atomic_store(&previewState.auditionBuffer,
                    std::shared_ptr<juce::AudioBuffer<float>>{});
  previewState.previewStartTime.store(0.0);
  previewState.previewEndTime.store(0.0);
  previewState.previewedRegion.store(nullptr);
  previewState.previewClaimedRenderer.store(nullptr);
}

juce::ARAPlaybackRenderer *
PitchNetDocumentController::doCreatePlaybackRenderer() noexcept {
  ensureHeadlessPlaybackBinding();
  return new PitchNetPlaybackRenderer(
      ARADocumentControllerSpecialisation::getDocumentController());
}

juce::ARAEditorRenderer *PitchNetDocumentController::doCreateEditorRenderer() {
  return new PitchNetEditorRenderer(
      ARADocumentControllerSpecialisation::getDocumentController());
}

juce::ARAAudioModification *PitchNetDocumentController::doCreateAudioModification(
    juce::ARAAudioSource *audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification *optionalModificationToClone) {
  if (optionalModificationToClone != nullptr)
    for (auto *region : optionalModificationToClone
                            ->getPlaybackRegions<juce::ARAPlaybackRegion>())
      if (region != nullptr)
        snapshotRegionState(*region);

  auto *created = new PitchNetAudioModification(audioSource, hostRef,
                                               optionalModificationToClone);
  ARA_DIAG("createMod mod=" + ARA_DIAG_PTR(created) +
           " src=" + ARA_DIAG_PTR(audioSource) + " clonedFrom=" +
           (optionalModificationToClone
                ? ARA_DIAG_PTR(optionalModificationToClone) + " cloneSrcId=" +
                      juce::String(
                          optionalModificationToClone->getPersistentID())
                : juce::String("none")) +
           " idAtCreate=" + juce::String(created->getPersistentID()));
  return created;
}

bool PitchNetDocumentController::doRestoreObjectsFromStream(
    juce::ARAInputStream &input,
    const juce::ARARestoreObjectsFilter *filter) noexcept {
  auto dataSize = input.readInt64();
  if (dataSize == kPitchNetAraModificationArchiveMagic) {
    const auto version = input.readInt();
    if (version < 3 || version > kPitchNetAraModificationArchiveVersion)
      return !input.failed();

    // See the version constant: pre-v4 payloads are timeline-anchored and
    // cannot be mapped into the modification-time model, so they are consumed
    // and dropped rather than restored.
    const bool payloadIsUsable =
        version >= kPitchNetAraFirstModificationTimeArchiveVersion;

    auto restoreProjectArchive = [this](const juce::MemoryBlock &data) {
      if (data.getSize() == 0)
        return;

      restoreProjectStateToDocument(data.getData(), data.getSize());

      if (restoreProjectStateCallback)
        restoreProjectStateCallback(data.getData(), data.getSize());
      else if (mainComponent) {
        juce::String jsonString(
            juce::CharPointer_UTF8(static_cast<const char *>(data.getData())),
            data.getSize());
        mainComponent->restoreProjectJson(jsonString);
      } else {
        pendingRestoredProjectData = data;
      }
    };

    const auto documentArchiveSize = input.readInt64();
    if (documentArchiveSize < 0 ||
        documentArchiveSize > std::numeric_limits<int>::max())
      return false;

    juce::MemoryBlock documentData(static_cast<size_t>(documentArchiveSize));
    if (documentArchiveSize > 0 &&
        input.read(documentData.getData(),
                   static_cast<int>(documentArchiveSize)) !=
            documentArchiveSize)
      return false;

    if (payloadIsUsable)
      restoreProjectArchive(documentData);

    const auto numAudioModifications = input.readInt64();
    for (juce::int64 i = 0; i < numAudioModifications; ++i) {
      const auto persistentID = input.readString();

      // Match the modification first so per-region audio (which has no size
      // prefix) can be read or skipped inline, keeping the stream aligned.
      auto *audioModification =
          filter ? filter->getAudioModificationToRestoreStateWithID<
                       juce::ARAAudioModification>(
                       persistentID.getCharPointer())
                 : nullptr;
      auto *pitchModification =
          dynamic_cast<PitchNetAudioModification *>(audioModification);

      ARA_DIAG("restoreMod archivedId=" + persistentID +
               " matched=" + juce::String(audioModification != nullptr ? 1 : 0) +
               " version=" + juce::String(version) +
               " payloadUsable=" + juce::String(payloadIsUsable ? 1 : 0));

      // Per region: a project JSON and, optionally, rendered processed audio.
      // Read in stream order; restore when the region is matched, otherwise
      // consume the bytes so the stream stays aligned.
      const int numRegions = input.readInt();
      for (int r = 0; r < numRegions; ++r) {
        const int regionIndex = input.readInt();
        const auto jsonSize = input.readInt64();
        if (jsonSize < 0 || jsonSize > std::numeric_limits<int>::max())
          return false;
        juce::MemoryBlock json(static_cast<size_t>(jsonSize));
        if (jsonSize > 0 &&
            input.read(json.getData(), static_cast<int>(jsonSize)) != jsonSize)
          return false;
        const int hasAudio = input.readInt();

        // Legacy archives carry one entry per region index; live identity is
        // the modification, so every entry restores into the same slot.
        juce::ignoreUnused(regionIndex);
        const auto regionKey =
            audioModification != nullptr
                ? juce::String(audioModification->getPersistentID())
                : persistentID;

        ARA_DIAG("restoreEntry id=" + persistentID +
                 " regionIndex=" + juce::String(regionIndex) +
                 " key=" + regionKey + " jsonBytes=" +
                 juce::String((int)json.getSize()) + " hasAudio=" +
                 juce::String(hasAudio));
        if (payloadIsUsable && audioModification != nullptr)
          restoreAraRegionProjectOrPend(regionKey, json.getData(),
                                        json.getSize());
        if (payloadIsUsable && pitchModification != nullptr &&
            regionKey.isNotEmpty() && json.getSize() > 0)
          pitchModification->setProjectArchiveForRegion(
              regionKey, json.getData(), json.getSize());

        if (hasAudio != 0) {
          if (payloadIsUsable && pitchModification != nullptr &&
              regionKey.isNotEmpty()) {
            if (!pitchModification->readProcessedAudioForRegionFromStream(
                    regionKey, input))
              return false;
          } else if (!PitchNetAudioModification::skipProcessedAudioFromStream(
                         input)) {
            return false;
          }
        }
      }

      if (!audioModification)
        continue;

      audioModification->notifyContentChanged(
          juce::ARAContentUpdateScopes::samplesAreAffected(), false);
      for (auto *region : audioModification->getPlaybackRegions())
        if (region)
          region->notifyContentChanged(
              juce::ARAContentUpdateScopes::samplesAreAffected(), false);
    }

    return !input.failed();
  }

  if (dataSize <= 0)
    return true;

  juce::MemoryBlock data;
  data.setSize(static_cast<size_t>(dataSize));
  input.read(data.getData(), static_cast<int>(dataSize));

  restoreProjectStateToDocument(data.getData(), data.getSize());

  if (restoreProjectStateCallback)
    restoreProjectStateCallback(data.getData(), data.getSize());
  else if (mainComponent) {
    juce::String jsonString(
        juce::CharPointer_UTF8(static_cast<const char *>(data.getData())),
        data.getSize());
    mainComponent->restoreProjectJson(jsonString);
  }
  else
    pendingRestoredProjectData = data;

  return !input.failed();
}

bool PitchNetDocumentController::doStoreObjectsToStream(
    juce::ARAOutputStream &output,
    const juce::ARAStoreObjectsFilter *filter) noexcept {
  juce::MemoryBlock archiveData;
  if (serializeProjectStateCallback)
    serializeProjectStateCallback(archiveData);
  else
    serializeDocumentProjectState(archiveData);

  if (archiveData.getSize() == 0)
    serializeDocumentProjectState(archiveData);

  if (archiveData.getSize() == 0 && mainComponent) {
    auto jsonString = mainComponent->serializeProjectJson();
    archiveData.append(jsonString.toRawUTF8(),
                       jsonString.getNumBytesAsUTF8());
  }

  if (filter) {
    const auto &audioModificationsToPersist =
        filter->getAudioModificationsToStore<juce::ARAAudioModification>();

    if (!audioModificationsToPersist.empty()) {
      if (!output.writeInt64(kPitchNetAraModificationArchiveMagic))
        return false;
      if (!output.writeInt(kPitchNetAraModificationArchiveVersion))
        return false;
      if (!output.writeInt64(static_cast<juce::int64>(archiveData.getSize())))
        return false;
      if (archiveData.getSize() > 0 &&
          !output.write(archiveData.getData(), archiveData.getSize()))
        return false;
      if (!output.writeInt64(
              static_cast<juce::int64>(audioModificationsToPersist.size())))
        return false;

      for (auto *audioModification : audioModificationsToPersist) {
        if (!audioModification)
          continue;
        if (!output.writeString(audioModification->getPersistentID()))
          return false;

        // One entry per region that has its own analysed/edited project, keyed
        // by its index in this modification. Each entry carries the region's
        // project JSON and, when present, its rendered processed audio so reload
        // playback is pitch-corrected without re-analysing/re-rendering.
        const auto *pitchModification =
            dynamic_cast<const PitchNetAudioModification *>(audioModification);
        struct RegionEntry {
          int index;
          juce::String key;
          juce::String audioKey;
          juce::MemoryBlock json;
        };
        std::vector<RegionEntry> regionEntries;
        const auto &regions =
            audioModification->getPlaybackRegions<juce::ARAPlaybackRegion>();
        for (size_t r = 0; r < regions.size(); ++r) {
          if (regions[r] == nullptr)
            continue;
          const auto key = pitchnetRegionKeyForIndex(
              audioModification->getPersistentID(), static_cast<int>(r));
          const auto liveKey = pitchnetRegionKey(*regions[r]);
          juce::MemoryBlock json;
          const bool hasDistinctLiveKey =
              liveKey.isNotEmpty() && liveKey != key;
          auto *processor = getRegionCanvasProcessor();
          // The processor owns the live Project. Prefer serialising it at the
          // instant the host asks us to save; the modification cache can lag
          // behind an edit or an asynchronous resynthesis callback.
          bool hasProject = false;
          if (processor != nullptr && hasDistinctLiveKey)
            hasProject =
                processor->serializeAraRegionProject(liveKey, json);
          if (!hasProject && pitchModification != nullptr &&
              hasDistinctLiveKey)
            hasProject =
                pitchModification->copyProjectArchiveForRegion(liveKey, json);
          if (!hasProject && pitchModification != nullptr)
            hasProject =
                pitchModification->copyProjectArchiveForRegion(key, json);
          if (!hasProject && processor != nullptr)
            hasProject = processor->serializeAraRegionProject(key, json);
          ARA_DIAG("store mod=" + ARA_DIAG_PTR(pitchModification) +
                   " id=" + juce::String(audioModification->getPersistentID()) +
                   " regionIndex=" + juce::String((int)r) +
                   " liveKey=" + liveKey + " indexKey=" + key +
                   " hasProject=" + juce::String(hasProject ? 1 : 0) +
                   " bytes=" + juce::String((int)json.getSize()));
          if (pitchModification != nullptr && hasProject && json.getSize() > 0)
            pitchModification->setProjectArchiveForRegion(
                key, json.getData(), json.getSize());
          if (json.getSize() > 0)
            regionEntries.push_back(
                {static_cast<int>(r), key, liveKey, std::move(json)});
        }

        // Inactive track versions can keep their modification while the host
        // removes all playback regions. Persist the saved slots in that case.
        if (regions.empty() && pitchModification != nullptr) {
          const auto prefix =
              juce::String(audioModification->getPersistentID()) + ":";
          for (const auto &key : pitchModification->getProjectArchiveRegionIDs()) {
            if (!key.startsWith(prefix))
              continue;
            const auto suffix = key.substring(prefix.length());
            if (suffix.isEmpty() || !suffix.containsOnly("0123456789"))
              continue;
            juce::MemoryBlock json;
            if (pitchModification->copyProjectArchiveForRegion(key, json))
              regionEntries.push_back(
                  {suffix.getIntValue(), key, key, std::move(json)});
          }
        }

        if (!output.writeInt(static_cast<int>(regionEntries.size())))
          return false;
        for (const auto &entry : regionEntries) {
          if (!output.writeInt(entry.index))
            return false;
          if (!output.writeInt64(static_cast<juce::int64>(entry.json.getSize())))
            return false;
          if (entry.json.getSize() > 0 &&
              !output.write(entry.json.getData(), entry.json.getSize()))
            return false;

          // Prefer the live key because it contains any edits made after this
          // project was restored. The stable index key is the same region's
          // persisted fallback and may still contain an older render.
          const bool hasLiveAudio =
              pitchModification != nullptr && entry.audioKey != entry.key &&
              pitchModification->hasProcessedAudioForRegion(entry.audioKey);
          const bool hasArchivedAudio =
              pitchModification != nullptr &&
              pitchModification->hasProcessedAudioForRegion(entry.key);
          const bool hasAudio = hasLiveAudio || hasArchivedAudio;
          if (!output.writeInt(hasAudio ? 1 : 0))
            return false;
          if (hasAudio) {
            const auto &audioKey = hasLiveAudio ? entry.audioKey : entry.key;
            if (!pitchModification->writeProcessedAudioForRegionToStream(
                    audioKey, output))
              return false;
          }
        }
      }
      return true;
    }
  }

  output.writeInt64(static_cast<juce::int64>(archiveData.getSize()));
  return output.write(archiveData.getData(), archiveData.getSize());
}

#endif // JucePlugin_Enable_ARA
