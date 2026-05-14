#include "ARADocumentController.h"

#if JucePlugin_Enable_ARA

#include "../UI/IMainView.h"

#include <limits>

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

  // Get document controller for accessing MainComponent
  auto *docCtrl = getDocController();

  // Update UI cursor position from host playback position
  if (shouldSyncUi && docCtrl && docCtrl->getMainComponent()) {
    if (isPlaying) {
      double timeInSeconds = static_cast<double>(timeInSamples) / sampleRate;
      auto state = hostUiSyncState;
      state->latestSeconds.store(timeInSeconds);

      if (!state->posPending.exchange(true)) {
        juce::Component::SafePointer<juce::Component> safeMain(
            docCtrl->getMainComponent()->getComponent());
        juce::MessageManager::callAsync([safeMain, state]() {
          state->posPending.store(false);
          if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->updatePlaybackPosition(state->latestSeconds.load());
        });
      }
    } else {
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
  }

  if (!isPlaying) {
    buffer.clear();
    return true;
  }

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
    // Map host timeline position to audio-modification sample time.
    // ARAAudioSourceReader reads in audio-modification sample coordinates,
    // so the realtime processor must use the same time base.
    juce::int64 modOffsetSamples = 0;
    auto blockRange = juce::Range<juce::int64>::withStartAndLength(
        timeInSamples, static_cast<juce::int64>(numSamples));

    for (auto *region : getPlaybackRegions()) {
      auto playbackRange = region->getSampleRange(
          sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
      if (blockRange.getIntersectionWith(playbackRange).isEmpty())
        continue;

      const juce::int64 modStart = region->getStartInAudioModificationSamples();
      modOffsetSamples = modStart - playbackRange.getStart();
      break;
    }

    juce::AudioPlayHead::PositionInfo adjustedPosInfo = posInfo;
    if (auto s = posInfo.getTimeInSamples())
      adjustedPosInfo.setTimeInSamples(*s + modOffsetSamples);
    if (auto t = posInfo.getTimeInSeconds())
      adjustedPosInfo.setTimeInSeconds(
          *t + (static_cast<double>(modOffsetSamples) / sampleRate));

    if (realtimeProcessor->processBlock(inputBuffer, buffer,
                                        &adjustedPosInfo)) {
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
// PitchNetDocumentController
//==============================================================================

PitchNetDocumentController::~PitchNetDocumentController() {
  stopAnalysisThread();
  if (analysisThread.joinable())
    analysisThread.join();
  if (analysisJoinerThread.joinable())
    analysisJoinerThread.join();
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
  if (!mainComponent || !source)
    return;

  stopAnalysisThread();
  if (!analysisState)
    analysisState = std::make_shared<AnalysisState>();
  analysisState->cancel.store(false);
  const auto jobId = analysisState->jobId.fetch_add(1) + 1;

  auto numSamples = static_cast<int>(source->getSampleCount());
  auto numChannels = source->getChannelCount();
  auto sourceSampleRate = source->getSampleRate();

  if (numSamples <= 0 || numChannels <= 0 || sourceSampleRate <= 0)
    return;

  juce::Component::SafePointer<juce::Component> safeMain(
      mainComponent->getComponent());
  auto *sourcePtr = source;
  auto state = analysisState;
  analysisThread = std::thread([safeMain, sourcePtr, state, jobId, numSamples,
                                numChannels, sourceSampleRate]() mutable {
    auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
    if (!view)
      return;
    if (!state)
      return;
    if (state->cancel.load() || state->jobId.load() != jobId)
      return;

    juce::ARAAudioSourceReader reader(sourcePtr);
    juce::AudioBuffer<float> buffer(numChannels, numSamples);
    if (!reader.read(&buffer, 0, numSamples, 0, true, true))
      return;

    if (state->cancel.load() || state->jobId.load() != jobId)
      return;

    juce::MessageManager::callAsync([safeMain, buffer = std::move(buffer),
                                     sourceSampleRate, state, jobId]() mutable {
      if (safeMain == nullptr)
        return;
      auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
      if (!view)
        return;
      if (!state)
        return;
      if (state->jobId.load() != jobId)
        return;
      view->setStatusMessage("ARA Mode - Analyzing...");
      view->setHostAudio(buffer, sourceSampleRate);
    });
  });
}

void PitchNetDocumentController::didAddAudioSourceToDocument(
    juce::ARADocument *, juce::ARAAudioSource *audioSource) {
  currentAudioSource = audioSource;
  processAudioSource(audioSource);
}

void PitchNetDocumentController::reanalyze() {
  if (currentAudioSource)
    processAudioSource(currentAudioSource);
}

juce::ARAPlaybackRenderer *
PitchNetDocumentController::doCreatePlaybackRenderer() noexcept {
  return new PitchNetPlaybackRenderer(
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
