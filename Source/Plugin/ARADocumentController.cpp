#include "ARADocumentController.h"

#if JucePlugin_Enable_ARA

#include "../UI/IMainView.h"

#include <limits>
#include <utility>

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
  bool didRender = false;
  auto blockRange =
      juce::Range<juce::int64>::withStartAndLength(timeInSamples, numSamples);

  for (auto *region : getPlaybackRegions()) {
    auto playbackRange = region->getSampleRange(
        sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
    auto renderRange = blockRange.getIntersectionWith(playbackRange);

    if (renderRange.isEmpty())
      continue;

    // Get modification range
    juce::Range<juce::int64> modRange{
        region->getStartInAudioModificationSamples(),
        region->getEndInAudioModificationSamples()};
    auto modOffset = modRange.getStart() - playbackRange.getStart();
    renderRange = renderRange.getIntersectionWith(
        modRange.movedToStartAt(playbackRange.getStart()));

    if (renderRange.isEmpty())
      continue;

    // Get reader
    auto *source = region->getAudioModification()->getAudioSource();
    auto it = readers.find(const_cast<juce::ARAAudioSource *>(source));
    if (it == readers.end())
      continue;

    int samplesToRead = static_cast<int>(renderRange.getLength());
    int bufferOffset =
        static_cast<int>(renderRange.getStart() - blockRange.getStart());
    auto sourceStart = renderRange.getStart() + modOffset;

    auto &readBuffer = didRender ? *tempBuffer : buffer;
    if (!it->second->read(&readBuffer, bufferOffset, samplesToRead, sourceStart,
                          true, true))
      continue;

    if (didRender) {
      // Mix with existing
      for (int ch = 0; ch < numChannels; ++ch)
        buffer.addFrom(ch, bufferOffset, *tempBuffer, ch, bufferOffset,
                       samplesToRead);
    } else {
      // Clear areas outside render range
      if (bufferOffset > 0)
        buffer.clear(0, bufferOffset);
      int endOffset = bufferOffset + samplesToRead;
      if (endOffset < numSamples)
        buffer.clear(endOffset, numSamples - endOffset);
      didRender = true;
    }
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

  // Read from ARA regions
  juce::AudioBuffer<float> inputBuffer(buffer.getNumChannels(), numSamples);
  bool didRender = readFromARARegions(inputBuffer, timeInSamples, numSamples);

  if (!didRender) {
    buffer.clear();
    return true;
  }

  // Get processor from document controller (dynamic lookup)
  auto *realtimeProcessor = docCtrl ? docCtrl->getRealtimeProcessor() : nullptr;

  // Apply pitch correction if processor available and ready
  if (realtimeProcessor && realtimeProcessor->isReady()) {
    if (realtimeProcessor->processBlock(inputBuffer, buffer, &posInfo)) {
      return true;
    } else {
    }
  } else {
    // Log why we're not using the processor
    if (!realtimeProcessor) {
    } else if (!realtimeProcessor->isReady()) {
    }
  }

  // Fallback: copy input to output
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
  auto blockRange =
      juce::Range<juce::int64>::withStartAndLength(timeInSamples, numSamples);

  for (auto *region : getPlaybackRegions()) {
    if (!region || !region->getAudioModification())
      continue;

    auto playbackRange = region->getSampleRange(
        sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
    auto renderRange = blockRange.getIntersectionWith(playbackRange);
    if (renderRange.isEmpty())
      continue;

    juce::Range<juce::int64> modRange{
        region->getStartInAudioModificationSamples(),
        region->getEndInAudioModificationSamples()};
    auto modOffset = modRange.getStart() - playbackRange.getStart();
    renderRange = renderRange.getIntersectionWith(
        modRange.movedToStartAt(playbackRange.getStart()));
    if (renderRange.isEmpty())
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

    const int samplesToRead = static_cast<int>(renderRange.getLength());
    const int bufferOffset =
        static_cast<int>(renderRange.getStart() - blockRange.getStart());
    const auto sourceStart = renderRange.getStart() + modOffset;

    juce::AudioBuffer<float> regionBuffer(buffer.getNumChannels(), numSamples);
    regionBuffer.clear();
    if (!it->second->read(&regionBuffer, bufferOffset, samplesToRead,
                          sourceStart, true, true))
      continue;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
      buffer.addFrom(ch, bufferOffset, regionBuffer, ch, bufferOffset,
                     samplesToRead);
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

  const auto timeInSamples = posInfo.getTimeInSamples().orFallback(0);
  const int numSamples = buffer.getNumSamples();
  juce::AudioBuffer<float> inputBuffer(numChannels, numSamples);
  if (!readFromARARegions(inputBuffer, timeInSamples, numSamples))
    return true;

  juce::AudioBuffer<float> processed(buffer.getNumChannels(), numSamples);
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
    std::function<bool(std::uintptr_t, double)> attachCachedAnalysis,
    std::function<void(std::uintptr_t, const juce::AudioBuffer<float> &, double,
                       double)>
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

void PitchNetDocumentController::processAudioSource(
    juce::ARAAudioSource *source) {
  if (!source)
    return;

  auto numSamples = static_cast<int>(source->getSampleCount());
  auto numChannels = source->getChannelCount();
  auto sourceSampleRate = source->getSampleRate();
  const double timelineOffsetSeconds =
      std::max(0.0, getTimelineOffsetSecondsForSource(source).value_or(0.0));
  const auto sourceKey = reinterpret_cast<std::uintptr_t>(source);

  if (attachCachedAnalysisCallback &&
      attachCachedAnalysisCallback(sourceKey, timelineOffsetSeconds))
    return;

  if (numSamples <= 0 || numChannels <= 0 || sourceSampleRate <= 0)
    return;

  stopAnalysisThread();
  if (!analysisState)
    analysisState = std::make_shared<AnalysisState>();
  analysisState->cancel.store(false);
  const auto jobId = analysisState->jobId.fetch_add(1) + 1;

  auto *sourcePtr = source;
  auto state = analysisState;
  auto requestAnalysis = requestAnalysisCallback;
  analysisThread = std::thread([sourcePtr, state, jobId, numSamples,
                                numChannels, sourceSampleRate,
                                timelineOffsetSeconds, sourceKey,
                                requestAnalysis]() mutable {
    if (!state)
      return;
    if (state->cancel.load() || state->jobId.load() != jobId)
      return;

    juce::ARAAudioSourceReader reader(sourcePtr);
    juce::AudioBuffer<float> sourceBuffer(numChannels, numSamples);
    if (!reader.read(&sourceBuffer, 0, numSamples, 0, true, true))
      return;

    const auto offsetSamples64 = static_cast<juce::int64>(
        std::llround(timelineOffsetSeconds * sourceSampleRate));
    if (offsetSamples64 > 0 &&
        offsetSamples64 <=
            static_cast<juce::int64>(std::numeric_limits<int>::max()) -
                static_cast<juce::int64>(numSamples)) {
      const int offsetSamples = static_cast<int>(offsetSamples64);
      juce::AudioBuffer<float> shifted(numChannels,
                                       offsetSamples + numSamples);
      shifted.clear();
      for (int ch = 0; ch < numChannels; ++ch)
        shifted.copyFrom(ch, offsetSamples, sourceBuffer, ch, 0, numSamples);
      sourceBuffer = std::move(shifted);
    }

    if (state->cancel.load() || state->jobId.load() != jobId)
      return;

    juce::MessageManager::callAsync([buffer = std::move(sourceBuffer),
                                     sourceSampleRate, timelineOffsetSeconds,
                                     state, jobId, sourceKey,
                                     requestAnalysis]() mutable {
      if (!state)
        return;
      if (state->jobId.load() != jobId)
        return;
      if (requestAnalysis)
        requestAnalysis(sourceKey, buffer, sourceSampleRate,
                        timelineOffsetSeconds);
    });
  });
}

void PitchNetDocumentController::updateAudioSourceTimelineOffset(
    juce::ARAAudioSource *source, juce::ARAPlaybackRegion *excludedRegion) {
  if (!mainComponent || !source)
    return;

  const auto timelineOffset =
      getTimelineOffsetSecondsForSource(source, excludedRegion);
  if (!timelineOffset.has_value()) {
    clearMainComponentHostAudio();
    return;
  }

  const double timelineOffsetSeconds = std::max(0.0, *timelineOffset);

  auto updateTimelineOffset = timelineOffsetCallback;
  juce::Component::SafePointer<juce::Component> safeMain(
      mainComponent->getComponent());
  juce::MessageManager::callAsync([safeMain, timelineOffsetSeconds,
                                   updateTimelineOffset]() {
    if (updateTimelineOffset) {
      updateTimelineOffset(timelineOffsetSeconds);
      return;
    }

    if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
      view->updateHostAudioTimelineOffset(timelineOffsetSeconds);
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

std::optional<double> PitchNetDocumentController::
    getTimelineOffsetSecondsForSource(
        juce::ARAAudioSource *source,
        juce::ARAPlaybackRegion *excludedRegion) const {
  if (!source)
    return std::nullopt;

  std::optional<double> earliestOffset;
  for (auto *audioModification : source->getAudioModifications()) {
    if (!audioModification)
      continue;

    for (auto *region : audioModification->getPlaybackRegions()) {
      if (!region || region == excludedRegion)
        continue;

      const double offset = region->getStartInPlaybackTime() -
                            region->getStartInAudioModificationTime();
      earliestOffset = earliestOffset.has_value()
                           ? std::min(*earliestOffset, offset)
                           : std::optional<double>(offset);
    }
  }

  return earliestOffset;
}

void PitchNetDocumentController::didAddAudioSourceToDocument(
    juce::ARADocument *, juce::ARAAudioSource *audioSource) {
  currentAudioSource = audioSource;
  processAudioSource(audioSource);
}

bool PitchNetDocumentController::processExistingAudioSources(
    juce::ARADocument *document) {
  if (!document)
    return false;

  juce::ARAAudioSource *fallbackSource = nullptr;
  for (auto *source : document->getAudioSources<juce::ARAAudioSource>()) {
    if (!source || source->getSampleCount() <= 0 ||
        source->getChannelCount() <= 0 || source->getSampleRate() <= 0.0)
      continue;

    if (!fallbackSource)
      fallbackSource = source;

    if (getTimelineOffsetSecondsForSource(source).has_value()) {
      currentAudioSource = source;
      currentPlaybackRegion = nullptr;
      for (auto *audioModification : source->getAudioModifications()) {
        if (!audioModification)
          continue;
        const auto &regions = audioModification->getPlaybackRegions();
        if (!regions.empty()) {
          currentPlaybackRegion = regions.front();
          break;
        }
      }
      if (mainComponent && mainComponent->hasAnalyzedProject())
        updateAudioSourceTimelineOffset(source);
      else
        processAudioSource(source);
      return true;
    }
  }

  if (fallbackSource) {
    currentAudioSource = fallbackSource;
    if (!mainComponent || !mainComponent->hasAnalyzedProject())
      processAudioSource(fallbackSource);
    return true;
  }

  return false;
}

void PitchNetDocumentController::willRemoveAudioSourceFromDocument(
    juce::ARADocument *, juce::ARAAudioSource *audioSource) {
  if (audioSource && audioSource == currentAudioSource)
    clearMainComponentHostAudio();
}

void PitchNetDocumentController::didAddPlaybackRegionToAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification)
    return;

  currentAudioSource = audioModification->getAudioSource();
  currentPlaybackRegion = playbackRegion;
  if (mainComponent && mainComponent->hasAnalyzedProject())
    updateAudioSourceTimelineOffset(currentAudioSource);
  else
    processAudioSource(currentAudioSource);
}

void PitchNetDocumentController::willRemovePlaybackRegionFromAudioModification(
    juce::ARAAudioModification *audioModification,
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!audioModification || !playbackRegion)
    return;

  auto *audioSource = audioModification->getAudioSource();
  if (!audioSource || audioSource != currentAudioSource)
    return;

  if (playbackRegion == currentPlaybackRegion) {
    previewState.previewedRegion.store(nullptr);
    currentPlaybackRegion = nullptr;
  }

  updateAudioSourceTimelineOffset(audioSource, playbackRegion);
}

void PitchNetDocumentController::didUpdatePlaybackRegionProperties(
    juce::ARAPlaybackRegion *playbackRegion) {
  if (!playbackRegion)
    return;

  if (auto *audioModification = playbackRegion->getAudioModification()) {
    currentAudioSource = audioModification->getAudioSource();
    currentPlaybackRegion = playbackRegion;
    if (mainComponent && mainComponent->hasAnalyzedProject())
      updateAudioSourceTimelineOffset(currentAudioSource);
    else
      processAudioSource(currentAudioSource);
  }
}

void PitchNetDocumentController::reanalyze() {
  if (currentAudioSource)
    processAudioSource(currentAudioSource);
}

void PitchNetDocumentController::startPreviewRange(double previewStartSeconds,
                                                   double previewEndSeconds) {
  if (!currentPlaybackRegion)
    return;

  const double regionStart = currentPlaybackRegion->getStartInPlaybackTime();
  const double regionEnd = currentPlaybackRegion->getEndInPlaybackTime();
  const double start =
      juce::jlimit(regionStart, regionEnd, previewStartSeconds);
  const double end = juce::jlimit(regionStart, regionEnd, previewEndSeconds);
  if (end <= start)
    return;

  previewState.previewStartTime.store(start);
  previewState.previewEndTime.store(end);
  previewState.previewedRegion.store(currentPlaybackRegion);
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
