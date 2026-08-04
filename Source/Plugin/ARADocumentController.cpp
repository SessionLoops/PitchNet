#include "ARADocumentController.h"

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

  // An index is suitable for the on-disk archive, but not as the identity of a
  // live region: inserting a region can change indices and can make the new
  // object collide with a Project already owned by another region.  The host
  // ref is stable for the lifetime of the ARA object, including editor
  // close/reopen, so use it for the live Project/undo-history key.  Persistence
  // still maps this key to the index key in doStoreObjectsToStream().
  const auto hostRef = reinterpret_cast<std::uintptr_t>(region.getHostRef());
  const auto objectRef = reinterpret_cast<std::uintptr_t>(&region);
  const auto liveRef = hostRef != 0 ? hostRef : objectRef;
  return juce::String(modification->getPersistentID()) + ":live:" +
         juce::String::toHexString(static_cast<juce::int64>(liveRef));
}

juce::String
pitchnetArchivedRegionKey(const juce::ARAPlaybackRegion &region) {
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

namespace {
constexpr juce::int64 kPitchNetAraModificationArchiveMagic =
    -0x504E41524D4F444LL; // -PNARMOD
// Alpha archive layout: one document-level project archive, followed by
// per-modification region projects and processed audio.
constexpr int kPitchNetAraModificationArchiveVersion = 3;

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

  // Create readers for all playback regions
  for (auto *region : getPlaybackRegions()) {
    auto *source = region->getAudioModification()->getAudioSource();
    if (readers.find(source) == readers.end())
      readers.emplace(source,
                      std::make_unique<juce::ARAAudioSourceReader>(source));
  }
}

void PitchNetPlaybackRenderer::releaseResources() {
  readers.clear();
  rawResamplingStates.clear();
  processedResamplingStates.clear();
  tempBuffer.reset();
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
          renderedRegion = mixProcessedRegionAudio(
              buffer, *region, data->audio, data->sampleRate,
              data->startSampleInModification, modificationRate, sampleRate,
              timeInSamples, &processedResamplingStates[region]);
        }
      }
    }

    if (!renderedRegion && intersectsBlock) {
      auto *source = region->getAudioModification()->getAudioSource();
      if (source != nullptr) {
        auto it = readers.find(source);
        if (it == readers.end())
          it = readers
                   .emplace(source, std::make_unique<juce::ARAAudioSourceReader>(
                                        source))
                   .first;
        tempBuffer->clear();
        if (readPlaybackRegionIntoBlock(region, *it->second, sampleRate,
                                        timeInSamples, *tempBuffer,
                                        &rawResamplingStates[region])) {
          for (int ch = 0; ch < std::min(buffer.getNumChannels(),
                                         tempBuffer->getNumChannels());
               ++ch)
            buffer.addFrom(ch, 0, *tempBuffer, ch, 0, numSamples);
          renderedRegion = true;
        }
      }
    }

    renderedAny = renderedAny || renderedRegion;
  }

  return renderedAny;
}

bool PitchNetPlaybackRenderer::processBlock(
    juce::AudioBuffer<float> &buffer, juce::AudioProcessor::Realtime realtime,
    const juce::AudioPlayHead::PositionInfo &posInfo) noexcept {
  auto timeInSamples = posInfo.getTimeInSamples().orFallback(0);
  bool isPlaying = posInfo.getIsPlaying();
  int numSamples = buffer.getNumSamples();
  const bool shouldSyncUi = (realtime == juce::AudioProcessor::Realtime::yes);
  const bool hasPlaybackRegions = !getPlaybackRegions().empty();

  // Get document controller for accessing MainComponent
  auto *docCtrl = getDocController();
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

void PitchNetEditorRenderer::prepareToPlay(
    double sampleRateIn, int, int numChannelsIn,
    juce::AudioProcessor::ProcessingPrecision,
    AlwaysNonRealtime alwaysNonRealtime) {
  sampleRate = sampleRateIn;
  numChannels = numChannelsIn;
  previewBuffer = std::make_shared<juce::AudioBuffer<float>>(
      numChannels, static_cast<int>(std::ceil(sampleRate)));
  if (auto *docCtrl = getDocController())
    docCtrl->getPreviewState().editorRendererSampleRate.store(sampleRate);
  previewLoopRange = {};
  previewLoopPosition = 0;

  readers.clear();
  if (alwaysNonRealtime == AlwaysNonRealtime::yes)
    return;

  for (auto *region : getPlaybackRegions()) {
    if (!region || !region->getAudioModification())
      continue;

    auto *source = region->getAudioModification()->getAudioSource();
    if (source && readers.find(source) == readers.end())
      readers.emplace(source,
                      std::make_unique<juce::ARAAudioSourceReader>(source));
  }
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
  if (it == readers.end())
    it = readers.emplace(source, std::make_unique<juce::ARAAudioSourceReader>(
                                     source))
             .first;

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

  const int channelsToCopy =
      std::min(buffer.getNumChannels(), previewBuffer->getNumChannels());
  int written = 0;
  while (written < buffer.getNumSamples()) {
    const int available =
        static_cast<int>(previewLoopRange.getEnd() - previewLoopPosition);
    const int toCopy = std::min(buffer.getNumSamples() - written, available);
    for (int ch = 0; ch < channelsToCopy; ++ch)
      buffer.copyFrom(ch, written, *previewBuffer, ch,
                      static_cast<int>(previewLoopPosition), toCopy);
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

  const int channelsToCopy =
      std::min(buffer.getNumChannels(), previewBuffer->getNumChannels());
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
      float value = previewBuffer->getSample(ch, position);
      if (inCrossfade)
        value = value * tailGain +
                previewBuffer->getSample(ch, loopStart + position - crossfadeStart) *
                    headGain;
      if (previewTransitionRemaining > 0 && previousPreviewBuffer &&
          ch < previousPreviewBuffer->getNumChannels()) {
        const float oldValue = renderLoopSample(*previousPreviewBuffer,
                                                previousPreviewLoopPosition, ch);
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
        previousPreviewBuffer = std::move(previewBuffer);
        previousPreviewLoopPosition =
            previousPreviewBuffer && !previewLoopRange.isEmpty()
                ? previewLoopPosition
                : 0;
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

    const double previewStartTime = previewState.previewStartTime.load();
    const double previewEndTime = previewState.previewEndTime.load();
    if (!juce::approximatelyEqual(previewStartTime, lastPreviewStartTime) ||
        !juce::approximatelyEqual(previewEndTime, lastPreviewEndTime) ||
        previewRegion != lastPreviewRegion) {
      renderPreviewBuffer(previewRegion, previewStartTime, previewEndTime);
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
  stopAnalysisThread();
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

void PitchNetDocumentController::setAnalysisCallbacks(
    std::function<bool(
        std::uintptr_t, double,
        const std::vector<std::pair<double, double>> &)>
        attachCachedAnalysis,
    std::function<void(std::uintptr_t, const juce::AudioBuffer<float> &, double,
                       double,
                       const std::vector<std::pair<double, double>> &)>
        requestAnalysis) {
  attachCachedAnalysisCallback = std::move(attachCachedAnalysis);
  requestAnalysisCallback = std::move(requestAnalysis);
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
        return processor->serializePersistentProjectState(destData);
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
  return ProjectSerializer::toBinaryArchive(*documentProjectSnapshot, destData);
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

void PitchNetDocumentController::publishCompositeEditsToRegions(
    const Project &project) {
  if (currentDocument == nullptr)
    return;

  const auto &audioData = project.getAudioData();
  const auto &waveform = audioData.waveform;
  const double waveformRate =
      audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                               : 0.0;
  if (waveformRate <= 0.0 || waveform.getNumSamples() <= 0)
    return;

  // Timeline ranges (seconds) of the notes the user actually edited. Composite
  // projects bake timeline placement as leading silence, so waveform[0] is
  // timeline zero and note frames map directly to timeline seconds.
  const double frameSeconds = static_cast<double>(HOP_SIZE) / waveformRate;
  std::vector<juce::Range<double>> editedRanges;
  for (const auto &note : project.getNotes())
    if (note.hasRenderedEdit())
      editedRanges.push_back({note.getStartFrame() * frameSeconds,
                              note.getEndFrame() * frameSeconds});
  if (editedRanges.empty())
    return;

  // Collect the composite's regions first: the composite waveform is a MIX of
  // all of them, so a region's slice is only safe to publish when no OTHER
  // region overlaps it in time. Publishing an overlapped slice bakes the
  // neighbour's audio into this region's processed audio, and the neighbour
  // then plays its own audio as well — two voices at once (typical with
  // crossfaded/overlapping clips).
  struct CompositeRegion {
    PitchNetAudioModification *modification = nullptr;
    juce::ARAPlaybackRegion *region = nullptr;
    juce::Range<double> range;
  };
  std::vector<CompositeRegion> compositeRegions;

  for (auto *source : currentDocument->getAudioSources<juce::ARAAudioSource>()) {
    if (source == nullptr)
      continue;
    for (auto *modification :
         source->getAudioModifications<PitchNetAudioModification>()) {
      if (modification == nullptr)
        continue;
      for (auto *region :
           modification->getPlaybackRegions<juce::ARAPlaybackRegion>()) {
        if (region == nullptr || !shouldProcessPlaybackRegion(region))
          continue;
        compositeRegions.push_back(
            {modification, region,
             juce::Range<double>(region->getStartInPlaybackTime(),
                                 region->getEndInPlaybackTime())});
      }
    }
  }

  std::vector<PitchNetAudioModification *> changedModifications;

  for (const auto &entry : compositeRegions) {
    auto *modification = entry.modification;
    auto *region = entry.region;
    const auto &regionRange = entry.range;

        const bool touchesEdit = std::any_of(
            editedRanges.begin(), editedRanges.end(),
            [&regionRange](const juce::Range<double> &editedRange) {
              return !regionRange.getIntersectionWith(editedRange).isEmpty();
            });
        if (!touchesEdit)
          continue; // unchanged region: keep playing the original source

        const bool overlapsOtherRegion = std::any_of(
            compositeRegions.begin(), compositeRegions.end(),
            [&entry](const CompositeRegion &other) {
              return other.region != entry.region &&
                     !other.range.getIntersectionWith(entry.range).isEmpty();
            });
        if (overlapsOtherRegion)
          continue; // slice would contain the neighbour's audio too — skip
                    // rather than double voices; edit such regions via region
                    // selection (per-region canvas) instead

        const auto startSample = static_cast<juce::int64>(
            std::llround(regionRange.getStart() * waveformRate));
        if (startSample < 0 || startSample >= waveform.getNumSamples())
          continue;
        const int numSamples = static_cast<int>(std::min<juce::int64>(
            static_cast<juce::int64>(
                std::llround(regionRange.getLength() * waveformRate)),
            waveform.getNumSamples() - startSample));
        if (numSamples <= 0)
          continue;

        juce::AudioBuffer<float> slice(waveform.getNumChannels(), numSamples);
        for (int ch = 0; ch < waveform.getNumChannels(); ++ch)
          slice.copyFrom(ch, 0, waveform, ch, static_cast<int>(startSample),
                         numSamples);

        modification->setProcessedAudioForRegion(
            pitchnetRegionKey(*region), slice, waveformRate,
            region->getStartInAudioModificationSamples());

        if (std::find(changedModifications.begin(), changedModifications.end(),
                      modification) == changedModifications.end())
          changedModifications.push_back(modification);
  }

  for (auto *modification : changedModifications) {
    modification->notifyContentChanged(
        juce::ARAContentUpdateScopes::samplesAreAffected(), true);
    for (auto *region : modification->getPlaybackRegions())
      if (region != nullptr)
        region->notifyContentChanged(
            juce::ARAContentUpdateScopes::samplesAreAffected(), true);
  }
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

void PitchNetDocumentController::processDocument(
    juce::ARADocument *document, juce::ARAPlaybackRegion *excludedRegion,
    juce::ARAAudioSource *excludedSource) {
  if (!document)
    return;

  clearStaleRegionSequenceFilter(document);

  struct RegionRead {
    juce::ARAAudioSource *source = nullptr;
    double playbackStart = 0.0;
    double modificationStart = 0.0;
    double duration = 0.0;
  };

  std::vector<RegionRead> regions;
  std::vector<std::pair<double, double>> playbackRegionRanges;
  double timelineStart = std::numeric_limits<double>::max();
  double timelineEnd = 0.0;
  double compositeSampleRate = analysisTimelineSampleRate;
  int compositeChannels = 0;
  std::uintptr_t arrangementKey = static_cast<std::uintptr_t>(1469598103934665603ull);
  auto hashValue = [&arrangementKey](std::uint64_t value) {
    arrangementKey ^= static_cast<std::uintptr_t>(value);
    arrangementKey *= static_cast<std::uintptr_t>(1099511628211ull);
  };

  for (auto *source : document->getAudioSources<juce::ARAAudioSource>()) {
    if (!source || source == excludedSource || source->getSampleRate() <= 0.0 ||
        source->getChannelCount() <= 0)
      continue;

    for (auto *modification : source->getAudioModifications()) {
      if (!modification)
        continue;
      for (auto *region : modification->getPlaybackRegions()) {
        if (!region || region == excludedRegion)
          continue;
        if (!shouldProcessPlaybackRegion(region))
          continue;

        const double start = region->getStartInPlaybackTime();
        const double end = region->getEndInPlaybackTime();
        if (end <= start)
          continue;

        regions.push_back({source, start,
                           region->getStartInAudioModificationTime(),
                           end - start});
        playbackRegionRanges.emplace_back(std::max(0.0, start), end);
        timelineStart = std::min(timelineStart, start);
        timelineEnd = std::max(timelineEnd, end);
        if (analysisTimelineSampleRate <= 0.0)
          compositeSampleRate = std::max(compositeSampleRate,
                                         source->getSampleRate());
        compositeChannels = std::max(compositeChannels,
                                     source->getChannelCount());

        // Identify the source by its ARA persistent ID rather than its runtime
        // pointer. The pointer is reassigned every time the DAW reloads the
        // project, which changed arrangementKey and made attachCachedAraAnalysis
        // miss the restored analysis, so no region was recognised/analysed after
        // a reload (Bug 2). The persistent ID is stable across reloads.
        {
          const std::string &persistentID = source->getPersistentID();
          for (const char c : persistentID)
            hashValue(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
          // Separator so two sources' IDs cannot concatenate ambiguously.
          hashValue(static_cast<std::uint64_t>(0x1F));
        }
        hashValue(static_cast<std::uint64_t>(
            std::llround(region->getStartInAudioModificationTime() *
                         1000000.0)));
        hashValue(static_cast<std::uint64_t>(
            std::llround((end - start) * 1000000.0)));
      }
    }
  }

  if (regions.empty() || compositeSampleRate <= 0.0 ||
      compositeChannels <= 0) {
    clearMainComponentHostAudio();
    return;
  }

  // The project timeline cannot display negative time. Crop any part of a
  // region before zero, while retaining the exact spacing between regions.
  timelineStart = std::max(0.0, timelineStart);
  const double timelineOffsetSeconds = timelineStart;
  const auto compositeSamples64 = static_cast<juce::int64>(std::ceil(
      std::max(0.0, timelineEnd - timelineStart) * compositeSampleRate));
  if (compositeSamples64 <= 0 ||
      compositeSamples64 > std::numeric_limits<int>::max())
    return;

  // Include the layout in the cache identity. A source pointer alone is not
  // sufficient: trimming, splitting, or repeating a source changes the audio
  // that must be analysed.
  // Absolute DAW placement is deliberately excluded: moving the complete
  // arrangement only changes its timeline offset, not its analysed content.
  for (const auto &region : regions)
    hashValue(static_cast<std::uint64_t>(std::llround(
        (region.playbackStart - timelineStart) * 1000000.0)));
  hashValue(static_cast<std::uint64_t>(
      std::llround(compositeSampleRate * 1000.0)));
  hashValue(static_cast<std::uint64_t>(regions.size()));
  if (arrangementKey == 0)
    arrangementKey = 1;

  if (attachCachedAnalysisCallback &&
      attachCachedAnalysisCallback(arrangementKey, timelineOffsetSeconds,
                                   playbackRegionRanges))
    return;

  if (!requestAnalysisCallback && documentProjectSnapshot)
    return;

  stopAnalysisThread();
  if (!analysisState)
    analysisState = std::make_shared<AnalysisState>();
  analysisState->cancel.store(false);
  const auto jobId = analysisState->jobId.fetch_add(1) + 1;
  auto state = analysisState;
  auto requestAnalysis = requestAnalysisCallback;
  const int compositeSamples = static_cast<int>(compositeSamples64);

  analysisThread = std::thread(
      [regions = std::move(regions), state, jobId, compositeSamples,
       compositeChannels, compositeSampleRate, timelineStart,
       timelineOffsetSeconds, arrangementKey,
       playbackRegionRanges = std::move(playbackRegionRanges),
       requestAnalysis]() mutable {
        if (!state || state->cancel.load() || state->jobId.load() != jobId)
          return;

        juce::AudioBuffer<float> composite(compositeChannels,
                                           compositeSamples);
        composite.clear();

        for (const auto &region : regions) {
          if (state->cancel.load() || state->jobId.load() != jobId)
            return;

          const double visibleStart = std::max(region.playbackStart,
                                               timelineStart);
          const double visibleEnd = region.playbackStart + region.duration;
          if (visibleEnd <= visibleStart)
            continue;

          const int outputStart = static_cast<int>(std::llround(
              (visibleStart - timelineStart) * compositeSampleRate));
          const int outputLength = std::min(
              compositeSamples - outputStart,
              static_cast<int>(std::llround(
                  (visibleEnd - visibleStart) * compositeSampleRate)));
          if (outputStart < 0 || outputLength <= 0)
            continue;

          const double sourceRate = region.source->getSampleRate();
          const auto sourceStart = static_cast<juce::int64>(std::llround(
              (region.modificationStart + visibleStart -
               region.playbackStart) * sourceRate));
          const auto availableSourceSamples =
              std::max<juce::int64>(0, region.source->getSampleCount() -
                                           sourceStart);
          const int renderLength = std::min(
              outputLength, static_cast<int>(std::floor(
                                availableSourceSamples *
                                compositeSampleRate / sourceRate)));
          if (renderLength <= 0)
            continue;
          const int sourceLength = static_cast<int>(std::min<juce::int64>(
              availableSourceSamples,
              std::max(1, static_cast<int>(std::ceil(
                              renderLength * sourceRate /
                              compositeSampleRate)) + 2)));
          if (sourceLength <= 0)
            continue;
          juce::AudioBuffer<float> sourceBuffer(compositeChannels,
                                                 sourceLength);
          sourceBuffer.clear();
          juce::ARAAudioSourceReader reader(region.source);
          if (!reader.read(&sourceBuffer, 0, sourceLength, sourceStart, true,
                           region.source->getChannelCount() > 1))
            continue;

          juce::AudioBuffer<float> rendered(compositeChannels, renderLength);
          if (juce::approximatelyEqual(sourceRate, compositeSampleRate)) {
            for (int ch = 0; ch < compositeChannels; ++ch)
              rendered.copyFrom(ch, 0, sourceBuffer, ch, 0, renderLength);
          } else {
            const double ratio = sourceRate / compositeSampleRate;
            for (int ch = 0; ch < compositeChannels; ++ch) {
              juce::LagrangeInterpolator interpolator;
              interpolator.process(ratio, sourceBuffer.getReadPointer(ch),
                                   rendered.getWritePointer(ch), renderLength);
            }
          }

          // Playback regions may overlap, so match the renderer and mix them.
          for (int ch = 0; ch < compositeChannels; ++ch)
            composite.addFrom(ch, outputStart, rendered, ch, 0, renderLength);
        }

        if (state->cancel.load() || state->jobId.load() != jobId)
          return;

        // Existing UI code represents timeline placement as leading silence.
        const auto offsetSamples64 = static_cast<juce::int64>(std::llround(
            timelineOffsetSeconds * compositeSampleRate));
        if (offsetSamples64 > 0 &&
            offsetSamples64 <=
                static_cast<juce::int64>(std::numeric_limits<int>::max()) -
                    composite.getNumSamples()) {
          const int offsetSamples = static_cast<int>(offsetSamples64);
          juce::AudioBuffer<float> shifted(
              compositeChannels, offsetSamples + composite.getNumSamples());
          shifted.clear();
          for (int ch = 0; ch < compositeChannels; ++ch)
            shifted.copyFrom(ch, offsetSamples, composite, ch, 0,
                             composite.getNumSamples());
          composite = std::move(shifted);
        }

        juce::MessageManager::callAsync(
            [buffer = std::move(composite), compositeSampleRate,
             timelineOffsetSeconds, arrangementKey, state, jobId,
             playbackRegionRanges = std::move(playbackRegionRanges),
             requestAnalysis]() mutable {
              if (!state || state->jobId.load() != jobId)
                return;
              if (requestAnalysis)
                requestAnalysis(arrangementKey, buffer, compositeSampleRate,
                                timelineOffsetSeconds,
                                playbackRegionRanges);
            });
      });
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
  auto *processor = getRegionCanvasProcessor();
  if (region == nullptr || processor == nullptr)
    return;

  auto *modification = region->getAudioModification();
  auto *source = modification ? modification->getAudioSource() : nullptr;
  if (source == nullptr || !source->isSampleAccessEnabled() ||
      source->getSampleRate() <= 0.0)
    return; // Samples not ready yet; the sample-access handler will retry.

  const auto liveKey = pitchnetRegionKey(*region);
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

  const double sourceSampleRate = source->getSampleRate();
  const double lengthSeconds =
      region->getEndInPlaybackTime() - region->getStartInPlaybackTime();
  if (lengthSeconds <= 0.0)
    return;

  const int numSamples =
      juce::jmax(1, juce::roundToInt(lengthSeconds * sourceSampleRate));
  juce::AudioBuffer<float> regionBuffer(1, numSamples);
  regionBuffer.clear();

  juce::ARAAudioSourceReader reader(source);
  const auto blockStart = static_cast<juce::int64>(
      std::llround(region->getStartInPlaybackTime() * sourceSampleRate));
  if (!readPlaybackRegionIntoBlock(region, reader, sourceSampleRate, blockStart,
                                   regionBuffer))
    return;

  const auto timelineOffsetSamples64 = static_cast<juce::int64>(std::llround(
      std::max(0.0, region->getStartInPlaybackTime()) * sourceSampleRate));
  if (timelineOffsetSamples64 > std::numeric_limits<int>::max() - numSamples)
    return;

  const int timelineOffsetSamples = static_cast<int>(timelineOffsetSamples64);
  juce::AudioBuffer<float> buffer(1, timelineOffsetSamples + numSamples);
  buffer.clear();
  buffer.copyFrom(0, timelineOffsetSamples, regionBuffer, 0, 0, numSamples);

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
  processor->analyzeAraRegionForCanvas(
      liveKey, pitchModification,
      region->getStartInAudioModificationSamples(),
      region->getStartInPlaybackTime(), buffer, sourceSampleRate);
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

void PitchNetDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification)
    return;

  // Touch the persistent-ID/index key while the host object is known-valid.
  if (playbackRegion != nullptr)
    pitchnetRegionKey(*playbackRegion);

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
      playbackRegion != nullptr && processor != nullptr &&
      processor->getMainComponent() != nullptr)
    processor->setActiveAraRegion(playbackRegion);
}

void PitchNetDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification || !playbackRegion)
    return;

  if (!shouldProcessPlaybackRegion(playbackRegion))
    return;

  auto *audioSource = audioModification->getAudioSource();
  if (!audioSource)
    return;

  if (playbackRegion == currentPlaybackRegion) {
    previewState.previewedRegion.store(nullptr);
    currentPlaybackRegion = nullptr;
  }
  currentPlaybackRegions.erase(std::remove(currentPlaybackRegions.begin(),
                                           currentPlaybackRegions.end(),
                                           playbackRegion),
                               currentPlaybackRegions.end());

  // With persistent-ID/index keys, compute the key before the host removes the
  // The processor keeps each region's Project and undo manager together, so
  // both must be released when the host destroys the playback region.
  if (auto *processor = getRegionCanvasProcessor())
    processor->removeAraRegion(pitchnetRegionKey(*playbackRegion));

  currentDocument = audioSource->getDocument();
}

void PitchNetDocumentController::didUpdatePlaybackRegionProperties(
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!playbackRegion)
    return;

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
  if (playbackRegion == currentPlaybackRegion) {
    previewState.previewedRegion.store(nullptr);
    currentPlaybackRegion = nullptr;
  }
}

void PitchNetDocumentController::reanalyze() {
  if (currentDocument)
    processDocument(currentDocument);
}

void PitchNetDocumentController::startPreviewRange(double previewStartSeconds,
                                                   double previewEndSeconds) {
  if (!currentDocument)
    return;

  juce::ARAPlaybackRegion *previewRegion = nullptr;
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
        if (previewEndSeconds > region->getStartInPlaybackTime() &&
            previewStartSeconds < region->getEndInPlaybackTime()) {
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

  if (!previewRegion)
    return;

  const double regionStart = previewRegion->getStartInPlaybackTime();
  const double regionEnd = previewRegion->getEndInPlaybackTime();
  const double start =
      juce::jlimit(regionStart, regionEnd, previewStartSeconds);
  const double end = juce::jlimit(regionStart, regionEnd, previewEndSeconds);
  if (end <= start)
    return;

  previewState.previewStartTime.store(start);
  previewState.previewEndTime.store(end);
  previewState.previewClaimedRenderer.store(nullptr);
  previewState.previewedRegion.store(previewRegion);
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
  return new PitchNetAudioModification(audioSource, hostRef,
                                       optionalModificationToClone);
}

bool PitchNetDocumentController::doRestoreObjectsFromStream(
    juce::ARAInputStream &input,
    const juce::ARARestoreObjectsFilter *filter) noexcept {
  auto dataSize = input.readInt64();
  if (dataSize == kPitchNetAraModificationArchiveMagic) {
    const auto version = input.readInt();
    if (version != kPitchNetAraModificationArchiveVersion)
      return !input.failed();

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

        const auto regionKey =
            pitchnetRegionKeyForIndex(persistentID, regionIndex);

        restoreAraRegionProjectOrPend(regionKey, json.getData(),
                                      json.getSize());
        if (pitchModification != nullptr && regionKey.isNotEmpty() &&
            json.getSize() > 0)
          pitchModification->setProjectArchiveForRegion(
              regionKey, json.getData(), json.getSize());

        if (hasAudio != 0) {
          if (pitchModification != nullptr && regionKey.isNotEmpty()) {
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

  if (archiveData.getSize() == 0) {
    output.writeInt64(0);
    return true;
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
          if (pitchModification != nullptr && hasProject && json.getSize() > 0)
            pitchModification->setProjectArchiveForRegion(
                key, json.getData(), json.getSize());
          if (json.getSize() > 0)
            regionEntries.push_back(
                {static_cast<int>(r), key, liveKey, std::move(json)});
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
