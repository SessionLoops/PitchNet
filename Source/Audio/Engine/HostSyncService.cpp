#include "HostSyncService.h"

HostSyncService::HostSyncService()
    : atomicState(std::make_shared<AtomicSyncState>())
{
}

HostSyncService::~HostSyncService()
{
    clearCallbacks();
}

// ========== Audio Thread Methods ==========

void HostSyncService::updateFromPlayHead(juce::AudioPlayHead* playHead, double sampleRate)
{
    if (playHead == nullptr)
        return;

    auto posInfo = playHead->getPosition();
    if (posInfo.hasValue())
        updateFromPositionInfo(*posInfo, sampleRate);
}

void HostSyncService::updateFromPositionInfo(const juce::AudioPlayHead::PositionInfo& info, double sampleRate)
{
    currentState.sampleRate = sampleRate;

    // Update transport state
    TransportState newTransport;
    newTransport.isPlaying = info.getIsPlaying();
    newTransport.isRecording = info.getIsRecording();
    newTransport.isLooping = info.getIsLooping();

    // Update position info
    PositionInfo newPosition;

    if (auto timeInSeconds = info.getTimeInSeconds())
    {
        newPosition.timeInSeconds = *timeInSeconds;
        newPosition.hasTimeInSeconds = true;
        newPosition.timeInSamples = static_cast<juce::int64>(*timeInSeconds * sampleRate);
        newPosition.hasTimeInSamples = true;
    }
    else if (auto timeInSamples = info.getTimeInSamples())
    {
        newPosition.timeInSamples = *timeInSamples;
        newPosition.hasTimeInSamples = true;
        newPosition.timeInSeconds = static_cast<double>(*timeInSamples) / sampleRate;
        newPosition.hasTimeInSeconds = true;
    }

    if (auto ppq = info.getPpqPosition())
    {
        newPosition.ppqPosition = *ppq;
        newPosition.hasPpqPosition = true;
    }

    if (auto ppqLastBar = info.getPpqPositionOfLastBarStart())
    {
        newPosition.ppqPositionOfLastBarStart = *ppqLastBar;
        newPosition.hasPpqPositionOfLastBarStart = true;
    }

    if (auto barCount = info.getBarCount())
    {
        newPosition.barCount = static_cast<int>(*barCount);
        newPosition.hasBarCount = true;
    }

    // Update tempo info
    TempoInfo newTempo;

    if (auto bpm = info.getBpm())
    {
        newTempo.bpm = *bpm;
        newTempo.hasBpm = true;
    }

    if (auto timeSig = info.getTimeSignature())
    {
        newTempo.timeSigNumerator = timeSig->numerator;
        newTempo.timeSigDenominator = timeSig->denominator;
        newTempo.hasTimeSignature = true;
    }

    // Update loop info
    LoopInfo newLoop;
    newLoop.isLoopEnabled = info.getIsLooping();

    if (auto loopPoints = info.getLoopPoints())
    {
        newLoop.loopStartPpq = loopPoints->ppqStart;
        newLoop.loopEndPpq = loopPoints->ppqEnd;
        newLoop.hasLoopPoints = true;

        // Convert to seconds if we have tempo
        if (newTempo.hasBpm && newTempo.bpm > 0.0)
        {
            newLoop.loopStartSeconds = newLoop.loopStartPpq * 60.0 / newTempo.bpm;
            newLoop.loopEndSeconds = newLoop.loopEndPpq * 60.0 / newTempo.bpm;
        }
    }

    // Store in current state
    currentState.transport = newTransport;
    currentState.position = newPosition;
    currentState.tempo = newTempo;
    currentState.loop = newLoop;

    // Update atomic state for UI thread
    auto state = atomicState;
    state->positionSeconds.store(newPosition.timeInSeconds, std::memory_order_relaxed);
    state->bpm.store(newTempo.bpm, std::memory_order_relaxed);
    state->isPlaying.store(newTransport.isPlaying, std::memory_order_relaxed);
    state->isRecording.store(newTransport.isRecording, std::memory_order_relaxed);
    state->isLooping.store(newTransport.isLooping, std::memory_order_relaxed);

    // Notify UI thread of changes (coalesced)

    // Transport change notification
    if (newTransport != previousTransport)
    {
        previousTransport = newTransport;
        notifyTransportChange(newTransport);
    }

    // Tempo change notification
    bool tempoChanged = (newTempo.hasBpm && std::abs(newTempo.bpm - previousTempo.bpm) > 0.001) ||
                        (newTempo.hasTimeSignature &&
                         (newTempo.timeSigNumerator != previousTempo.timeSigNumerator ||
                          newTempo.timeSigDenominator != previousTempo.timeSigDenominator));

    if (tempoChanged)
    {
        previousTempo = newTempo;
        notifyTempoChange(newTempo);
    }

    if (newLoop != previousLoop)
    {
        previousLoop = newLoop;
        notifyLoopChange(newLoop);
    }

    const bool positionChanged =
        newPosition.hasTimeInSeconds &&
        (!previousPosition.hasTimeInSeconds ||
         std::abs(newPosition.timeInSeconds - previousPosition.timeInSeconds) >
             0.0001);

    // Position update notification. Playback is throttled; stopped host seek
    // moves are emitted on change so the UI follows manual playhead edits.
    if (positionCallbackEnabled && newPosition.hasTimeInSeconds)
    {
        auto now = juce::Time::getMillisecondCounter();
        const bool shouldNotifyPlaying =
            newTransport.isPlaying &&
            now - lastPositionCallbackTime >=
                static_cast<juce::int64>(positionUpdateIntervalMs);
        const bool shouldNotifyStoppedSeek =
            !newTransport.isPlaying && positionChanged;

        if (shouldNotifyPlaying || shouldNotifyStoppedSeek)
        {
            lastPositionCallbackTime = now;
            notifyPositionUpdate(newPosition.timeInSeconds);
        }
    }

    previousPosition = newPosition;
}

HostSyncService::SyncState HostSyncService::getCurrentState() const
{
    return currentState;
}

// ========== UI Thread Methods ==========

void HostSyncService::setTransportCallback(TransportCallback callback)
{
    std::atomic_store(&transportCallback, std::make_shared<TransportCallback>(std::move(callback)));
}

void HostSyncService::setPositionCallback(PositionCallback callback)
{
    std::atomic_store(&positionCallback, std::make_shared<PositionCallback>(std::move(callback)));
}

void HostSyncService::setTempoCallback(TempoCallback callback)
{
    std::atomic_store(&tempoCallback, std::make_shared<TempoCallback>(std::move(callback)));
}

void HostSyncService::setLoopCallback(LoopCallback callback)
{
    std::atomic_store(&loopCallback, std::make_shared<LoopCallback>(std::move(callback)));
}

void HostSyncService::setFullSyncCallback(FullSyncCallback callback)
{
    std::atomic_store(&fullSyncCallback, std::make_shared<FullSyncCallback>(std::move(callback)));
}

void HostSyncService::clearCallbacks()
{
    std::atomic_store(&transportCallback, std::shared_ptr<TransportCallback>());
    std::atomic_store(&positionCallback, std::shared_ptr<PositionCallback>());
    std::atomic_store(&tempoCallback, std::shared_ptr<TempoCallback>());
    std::atomic_store(&loopCallback, std::shared_ptr<LoopCallback>());
    std::atomic_store(&fullSyncCallback, std::shared_ptr<FullSyncCallback>());
}

// ========== Host Transport Control ==========

void HostSyncService::requestPlay(bool shouldPlay)
{
    requestedPlayState.store(shouldPlay, std::memory_order_relaxed);
    pendingPlayRequest.store(true, std::memory_order_release);
}

void HostSyncService::requestStop()
{
    pendingStopRequest.store(true, std::memory_order_release);
}

void HostSyncService::processPendingRequests(juce::AudioPlayHead* playHead)
{
    if (playHead == nullptr)
        return;

    if (!playHead->canControlTransport())
        return;

    // Process stop request first
    if (pendingStopRequest.exchange(false, std::memory_order_acquire))
    {
        playHead->transportPlay(false);
        playHead->transportRewind();
    }

    // Process play request
    if (pendingPlayRequest.exchange(false, std::memory_order_acquire))
    {
        bool shouldPlay = requestedPlayState.load(std::memory_order_relaxed);
        playHead->transportPlay(shouldPlay);
    }
}

// ========== Internal Notification Methods ==========

void HostSyncService::notifyTransportChange(const TransportState& state)
{
    auto state_ptr = atomicState;

    if (!state_ptr->transportPending.exchange(true, std::memory_order_acq_rel))
    {
        auto cb = std::atomic_load(&transportCallback);
        if (cb && *cb)
        {
            TransportState stateCopy = state;
            juce::MessageManager::callAsync([cb, stateCopy, state_ptr]()
            {
                state_ptr->transportPending.store(false, std::memory_order_release);
                if (*cb)
                    (*cb)(stateCopy);
            });
        }
        else
        {
            state_ptr->transportPending.store(false, std::memory_order_release);
        }
    }
}

void HostSyncService::notifyPositionUpdate(double seconds)
{
    juce::ignoreUnused(seconds);
    auto state_ptr = atomicState;

    if (!state_ptr->positionPending.exchange(true, std::memory_order_acq_rel))
    {
        auto cb = std::atomic_load(&positionCallback);
        if (cb && *cb)
        {
            juce::MessageManager::callAsync([cb, state_ptr]()
            {
                state_ptr->positionPending.store(false, std::memory_order_release);
                // Use latest position from atomic state
                double latestSeconds = state_ptr->positionSeconds.load(std::memory_order_relaxed);
                if (*cb)
                    (*cb)(latestSeconds);
            });
        }
        else
        {
            state_ptr->positionPending.store(false, std::memory_order_release);
        }
    }
}

void HostSyncService::notifyTempoChange(const TempoInfo& tempo)
{
    auto state_ptr = atomicState;

    if (!state_ptr->tempoPending.exchange(true, std::memory_order_acq_rel))
    {
        auto cb = std::atomic_load(&tempoCallback);
        if (cb && *cb)
        {
            TempoInfo tempoCopy = tempo;
            juce::MessageManager::callAsync([cb, tempoCopy, state_ptr]()
            {
                state_ptr->tempoPending.store(false, std::memory_order_release);
                if (*cb)
                    (*cb)(tempoCopy);
            });
        }
        else
        {
            state_ptr->tempoPending.store(false, std::memory_order_release);
        }
    }
}

void HostSyncService::notifyLoopChange(const LoopInfo& loop)
{
    auto cb = std::atomic_load(&loopCallback);
    if (cb && *cb)
    {
        LoopInfo loopCopy = loop;
        juce::MessageManager::callAsync([cb, loopCopy]()
        {
            if (*cb)
                (*cb)(loopCopy);
        });
    }
}
