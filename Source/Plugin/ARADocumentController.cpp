#include "ARADocumentController.h"

#if JucePlugin_Enable_ARA

#include "../UI/IMainView.h"

#include <limits>
#include <utility>

namespace {
bool readPlaybackRegionIntoBlock(
    juce::ARAPlaybackRegion *region, juce::ARAAudioSourceReader &reader,
    double outputSampleRate, juce::int64 blockStartSample,
    juce::AudioBuffer<float> &buffer) {
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
  const int sourceLength =
      std::max(1, static_cast<int>(std::ceil(outputLength * ratio)) + 16);
  juce::AudioBuffer<float> sourceBuffer(buffer.getNumChannels(), sourceLength);
  sourceBuffer.clear();
  if (!reader.read(&sourceBuffer, 0, sourceLength, sourceStart, true,
                   sourceChannels > 1))
    return false;

  for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
    juce::LagrangeInterpolator interpolator;
    interpolator.process(
        ratio, sourceBuffer.getReadPointer(
                   std::min(ch, sourceBuffer.getNumChannels() - 1)),
        buffer.getWritePointer(ch, outputStart), outputLength);
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
  tempBuffer.reset();
}

bool PitchNetPlaybackRenderer::readFromARARegions(
    juce::AudioBuffer<float> &buffer, juce::int64 timeInSamples,
    int numSamples) {
  buffer.clear();
  bool didRender = false;

  for (auto *region : getPlaybackRegions()) {
    if (!region || !region->getAudioModification())
      continue;

    auto *source = region->getAudioModification()->getAudioSource();
    if (!source)
      continue;
    auto it = readers.find(source);
    if (it == readers.end())
      it = readers
               .emplace(source,
                        std::make_unique<juce::ARAAudioSourceReader>(source))
               .first;

    tempBuffer->clear();
    if (!readPlaybackRegionIntoBlock(region, *it->second, sampleRate,
                                     timeInSamples, *tempBuffer))
      continue;

    for (int ch = 0; ch < std::min(buffer.getNumChannels(),
                                   tempBuffer->getNumChannels()); ++ch)
      buffer.addFrom(ch, 0, *tempBuffer, ch, 0, numSamples);
    didRender = true;
  }

  return didRender;
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

  // Get processor from document controller (dynamic lookup)
  auto *realtimeProcessor = docCtrl ? docCtrl->getRealtimeProcessor() : nullptr;

  // Once the analysed timeline is ready, render from it directly.  Reading the
  // raw ARA source every block forces a second realtime resampling path when
  // the source rate differs from the project rate, which can produce boundary
  // clicks even for unedited regions.
  if (realtimeProcessor && realtimeProcessor->isReady()) {
    juce::AudioBuffer<float> silentInput(buffer.getNumChannels(), numSamples);
    silentInput.clear();
    if (realtimeProcessor->processBlock(silentInput, buffer, &posInfo))
      return true;
  }

  // Fallback while analysis is not ready: play the host ARA source directly.
  juce::AudioBuffer<float> inputBuffer(buffer.getNumChannels(), numSamples);
  bool didRender = readFromARARegions(inputBuffer, timeInSamples, numSamples);

  if (!didRender) {
    buffer.clear();
    return true;
  }

  buffer.makeCopyOf(inputBuffer);
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
  previewBuffer = std::make_unique<juce::AudioBuffer<float>>(
      numChannels, static_cast<int>(std::ceil(sampleRate)));
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
  if (!previewBuffer)
    return;

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
  if (!readPlaybackRangeIntoBuffer(previewRange, region, input)) {
    previewLoopRange = {};
    return;
  }

  auto *docCtrl = getDocController();
  auto *realtimeProcessor = docCtrl ? docCtrl->getRealtimeProcessor() : nullptr;
  if (realtimeProcessor && realtimeProcessor->isReady()) {
    juce::AudioPlayHead::PositionInfo previewPosition;
    const auto startSamples =
        static_cast<juce::int64>(std::llround(previewRange.getStart() *
                                              sampleRate));
    previewPosition.setTimeInSamples(startSamples);
    previewPosition.setTimeInSeconds(previewRange.getStart());
    previewPosition.setIsPlaying(false);
    if (!realtimeProcessor->processBlock(input, *previewBuffer,
                                         &previewPosition))
      previewBuffer->makeCopyOf(input);
  } else {
    previewBuffer->makeCopyOf(input);
  }

  previewLoopRange = juce::Range<juce::int64>::withStartAndLength(
      0, previewBuffer->getNumSamples());
  previewLoopPosition = previewLoopRange.getStart();
}

void PitchNetEditorRenderer::writePreviewOnce(
    juce::AudioBuffer<float> &buffer) {
  if (!previewBuffer || previewLoopRange.isEmpty()) {
    buffer.clear();
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

  if (written < buffer.getNumSamples())
    buffer.clear(written, buffer.getNumSamples() - written);
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

  if (!posInfo.getIsPlaying()) {
    const auto &previewState = docCtrl->getPreviewState();
    auto *previewRegion = previewState.previewedRegion.load();
    if (!previewRegion) {
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

  auto *realtimeProcessor = docCtrl->getRealtimeProcessor();
  if (!realtimeProcessor || !realtimeProcessor->isReady())
    return true;

  const int numSamples = buffer.getNumSamples();
  juce::AudioBuffer<float> processed(buffer.getNumChannels(), numSamples);
  processed.clear();

  juce::AudioBuffer<float> silentInput(buffer.getNumChannels(), numSamples);
  silentInput.clear();
  if (realtimeProcessor->processBlock(silentInput, processed, &posInfo)) {
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
      buffer.addFrom(ch, 0, processed, ch, 0, numSamples);
    return true;
  }

  const auto timeInSamples = posInfo.getTimeInSamples().orFallback(0);
  juce::AudioBuffer<float> inputBuffer(numChannels, numSamples);
  if (!readFromARARegions(inputBuffer, timeInSamples, numSamples))
    return true;

  processed.clear();
  if (!realtimeProcessor->processBlock(inputBuffer, processed, &posInfo))
    processed.makeCopyOf(inputBuffer);

  for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    buffer.addFrom(ch, 0, processed, ch, 0, numSamples);

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
        if (currentRegionSequence &&
            region->getRegionSequence() != currentRegionSequence)
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

        hashValue(reinterpret_cast<std::uintptr_t>(source));
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

  juce::Component::SafePointer<juce::Component> safeMain(
      mainComponent->getComponent());
  juce::MessageManager::callAsync([safeMain]() {
    if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
      view->clearHostAudio();
  });
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

  currentDocument = document;
  bool hasSource = false;
  currentPlaybackRegion = nullptr;
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
        if (!region ||
            (currentRegionSequence &&
             region->getRegionSequence() != currentRegionSequence))
          continue;
        currentPlaybackRegion = region;
        if (!currentRegionSequence)
          currentRegionSequence = region->getRegionSequence();
        break;
      }
    }
  }

  if (hasSource)
    processDocument(document);
  return hasSource;
}

bool PitchNetDocumentController::processPlaybackRegions(
    const std::vector<juce::ARAPlaybackRegion *> &playbackRegions,
    double projectSampleRate) {
  auto *firstRegion = playbackRegions.empty() ? nullptr : playbackRegions.front();
  if (!firstRegion || !firstRegion->getAudioModification())
    return false;

  currentPlaybackRegion = firstRegion;
  analysisTimelineSampleRate = projectSampleRate > 0.0 ? projectSampleRate
                                                       : 0.0;
  currentRegionSequence = firstRegion->getRegionSequence();
  currentAudioSource = firstRegion->getAudioModification()->getAudioSource();
  currentDocument = currentAudioSource ? currentAudioSource->getDocument()
                                       : nullptr;
  if (!currentDocument)
    return false;

  processDocument(currentDocument);
  return true;
}

void PitchNetDocumentController::willRemoveAudioSourceFromDocument(
    juce::ARADocument *document, juce::ARAAudioSource *audioSource) {
  if (!document || !audioSource)
    return;
  if (audioSource == currentAudioSource)
    currentAudioSource = nullptr;
  processDocument(document, nullptr, audioSource);
}

void PitchNetDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification)
    return;

  if (playbackRegion && currentRegionSequence &&
      playbackRegion->getRegionSequence() != currentRegionSequence)
    return;

  currentAudioSource = audioModification->getAudioSource();
  currentDocument = currentAudioSource ? currentAudioSource->getDocument()
                                       : currentDocument;
  currentPlaybackRegion = playbackRegion;
  currentRegionSequence = playbackRegion ? playbackRegion->getRegionSequence()
                                         : currentRegionSequence;
  processDocument(currentDocument);
}

void PitchNetDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification || !playbackRegion)
    return;

  if (currentRegionSequence &&
      playbackRegion->getRegionSequence() != currentRegionSequence)
    return;

  auto *audioSource = audioModification->getAudioSource();
  if (!audioSource)
    return;

  if (playbackRegion == currentPlaybackRegion) {
    previewState.previewedRegion.store(nullptr);
    currentPlaybackRegion = nullptr;
  }

  currentDocument = audioSource->getDocument();
  processDocument(currentDocument, playbackRegion);
}

void PitchNetDocumentController::didUpdatePlaybackRegionProperties(
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!playbackRegion)
    return;

  if (currentRegionSequence &&
      playbackRegion->getRegionSequence() != currentRegionSequence)
    return;

  if (auto *audioModification = playbackRegion->getAudioModification()) {
    currentAudioSource = audioModification->getAudioSource();
    currentDocument = currentAudioSource ? currentAudioSource->getDocument()
                                         : currentDocument;
    currentPlaybackRegion = playbackRegion;
    currentRegionSequence = playbackRegion->getRegionSequence();
    processDocument(currentDocument);
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

  currentPlaybackRegion = previewRegion;
  const double regionStart = previewRegion->getStartInPlaybackTime();
  const double regionEnd = previewRegion->getEndInPlaybackTime();
  const double start =
      juce::jlimit(regionStart, regionEnd, previewStartSeconds);
  const double end = juce::jlimit(regionStart, regionEnd, previewEndSeconds);
  if (end <= start)
    return;

  previewState.previewStartTime.store(start);
  previewState.previewEndTime.store(end);
  previewState.previewedRegion.store(previewRegion);
}

void PitchNetDocumentController::stopPreview() {
  previewState.previewStartTime.store(0.0);
  previewState.previewEndTime.store(0.0);
  previewState.previewedRegion.store(nullptr);
}

juce::ARAPlaybackRenderer *
PitchNetDocumentController::doCreatePlaybackRenderer() noexcept {
  return new PitchNetPlaybackRenderer(
      ARADocumentControllerSpecialisation::getDocumentController());
}

juce::ARAEditorRenderer *PitchNetDocumentController::doCreateEditorRenderer() {
  return new PitchNetEditorRenderer(
      ARADocumentControllerSpecialisation::getDocumentController());
}

bool PitchNetDocumentController::doRestoreObjectsFromStream(
    juce::ARAInputStream &input,
    const juce::ARARestoreObjectsFilter *) noexcept {
  auto dataSize = input.readInt64();
  if (dataSize <= 0)
    return true;

  juce::MemoryBlock data;
  data.setSize(static_cast<size_t>(dataSize));
  input.read(data.getData(), static_cast<int>(dataSize));

  if (mainComponent) {
    juce::String jsonString(
        juce::CharPointer_UTF8(static_cast<const char *>(data.getData())),
        data.getSize());
    mainComponent->restoreProjectJson(jsonString);
  }

  return !input.failed();
}

bool PitchNetDocumentController::doStoreObjectsToStream(
    juce::ARAOutputStream &output,
    const juce::ARAStoreObjectsFilter *) noexcept {
  if (!mainComponent) {
    output.writeInt64(0);
    return true;
  }

  auto jsonString = mainComponent->serializeProjectJson();
  if (jsonString.isEmpty()) {
    output.writeInt64(0);
    return true;
  }

  output.writeInt64(static_cast<juce::int64>(jsonString.getNumBytesAsUTF8()));
  return output.write(jsonString.toRawUTF8(),
                      static_cast<int>(jsonString.getNumBytesAsUTF8()));
}

#endif // JucePlugin_Enable_ARA
