#include "PluginTransportController.h"

PluginTransportController::PluginTransportController()
{
    // Set up internal callbacks from HostSyncService
    hostSync.setTransportCallback([this](const HostSyncService::TransportState& state) {
        // Notify play state change
        if (state.isPlaying != previousPlayState)
        {
            previousPlayState = state.isPlaying;

            auto cb = std::atomic_load(&playStateCallback);
            if (cb && *cb)
            {
                bool playing = state.isPlaying;
                juce::MessageManager::callAsync([cb, playing]() {
                    if (*cb)
                        (*cb)(playing);
                });
            }
        }
    });

    hostSync.setPositionCallback([this](double timeInSeconds) {
        auto cb = std::atomic_load(&positionCallback);
        if (cb && *cb)
        {
            juce::MessageManager::callAsync([cb, timeInSeconds]() {
                if (*cb)
                    (*cb)(timeInSeconds);
            });
        }
    });
}

PluginTransportController::~PluginTransportController()
{
    clearCallbacks();
}

// ========== Audio Thread Methods ==========

void PluginTransportController::processBlock(juce::AudioPlayHead* playHead, double sampleRate)
{
    // Update host capability flag
    if (playHead != nullptr)
        hostCanControlTransport.store(playHead->canControlTransport(), std::memory_order_relaxed);

    // Process pending transport requests
    processPendingRequests(playHead);

    // Update sync state from host
    hostSync.updateFromPlayHead(playHead, sampleRate);

    // If we have a pending seek and host is not playing, update internal cursor
    if (useInternalCursor.load(std::memory_order_relaxed))
    {
        auto state = hostSync.getCurrentState();
        if (!state.transport.isPlaying)
        {
            // Use internal cursor position when host is stopped
            internalCursorPosition.store(requestedSeekPosition.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
        }
        else
        {
            // Host is playing, disable internal cursor
            useInternalCursor.store(false, std::memory_order_relaxed);
        }
    }
}

void PluginTransportController::processPendingRequestsOnly(juce::AudioPlayHead* playHead)
{
    if (playHead != nullptr)
        hostCanControlTransport.store(playHead->canControlTransport(), std::memory_order_relaxed);

    processPendingRequests(playHead);
}

void PluginTransportController::processPendingRequests(juce::AudioPlayHead* playHead)
{
    if (playHead == nullptr)
        return;

    bool canControl = playHead->canControlTransport();

    // Process stop request first
    if (pendingStopRequest.exchange(false, std::memory_order_acquire))
    {
        if (canControl)
        {
            playHead->transportPlay(false);
            playHead->transportRewind();
        }
        // Reset internal cursor to beginning
        internalCursorPosition.store(0.0, std::memory_order_relaxed);
        useInternalCursor.store(true, std::memory_order_relaxed);
    }

    // Process seek request
    if (pendingSeekRequest.exchange(false, std::memory_order_acquire))
    {
        double seekPos = requestedSeekPosition.load(std::memory_order_relaxed);

        // JUCE's AudioPlayHead transport control can only request rewind, not an
        // arbitrary timeline seek. Still ask the host when the target is the
        // start; otherwise keep PitchNet's cursor in sync locally.
        if (canControl && seekPos <= 0.0001)
            playHead->transportRewind();

        // Store for internal tracking (host may not support seek)
        // Note: We do NOT stop playback when seeking - just update internal cursor
        // The host controls the actual playback position
        internalCursorPosition.store(seekPos, std::memory_order_relaxed);
        useInternalCursor.store(true, std::memory_order_relaxed);

        // Notify UI of the seek position
        auto cb = std::atomic_load(&positionCallback);
        if (cb && *cb)
        {
            juce::MessageManager::callAsync([cb, seekPos]() {
                if (*cb)
                    (*cb)(seekPos);
            });
        }
    }

    // Process play request
    if (pendingPlayRequest.exchange(false, std::memory_order_acquire))
    {
        bool shouldPlay = requestedPlayState.load(std::memory_order_relaxed);
        if (canControl)
        {
            playHead->transportPlay(shouldPlay);
        }

        // If starting playback, disable internal cursor (host will provide position)
        if (shouldPlay)
        {
            useInternalCursor.store(false, std::memory_order_relaxed);
        }
    }
}

HostSyncService::SyncState PluginTransportController::getCurrentState() const
{
    return hostSync.getCurrentState();
}

bool PluginTransportController::isHostPlaying() const
{
    return hostSync.isHostPlaying();
}

double PluginTransportController::getPositionSeconds() const
{
    // Return internal cursor if active, otherwise host position
    if (useInternalCursor.load(std::memory_order_relaxed))
        return internalCursorPosition.load(std::memory_order_relaxed);
    return hostSync.getPositionSeconds();
}

double PluginTransportController::getTempoBpm() const
{
    return hostSync.getTempoBpm();
}

// ========== UI Thread Methods - Transport Control ==========

void PluginTransportController::requestPlay(bool shouldPlay)
{
    requestedPlayState.store(shouldPlay, std::memory_order_relaxed);
    pendingPlayRequest.store(true, std::memory_order_release);
}

void PluginTransportController::requestStop()
{
    pendingStopRequest.store(true, std::memory_order_release);
}

void PluginTransportController::requestSeek(double timeInSeconds)
{
    requestedSeekPosition.store(std::max(0.0, timeInSeconds), std::memory_order_relaxed);
    pendingSeekRequest.store(true, std::memory_order_release);
}

void PluginTransportController::togglePlayPause()
{
    bool currentlyPlaying = hostSync.isHostPlaying();
    requestPlay(!currentlyPlaying);
}

// ========== UI Thread Methods - Callbacks ==========

void PluginTransportController::setPlayStateCallback(PlayStateCallback callback)
{
    std::atomic_store(&playStateCallback, std::make_shared<PlayStateCallback>(std::move(callback)));
    hostSync.setTransportCallback([this](const HostSyncService::TransportState& state) {
        if (state.isPlaying != previousPlayState)
        {
            previousPlayState = state.isPlaying;

            auto cb = std::atomic_load(&playStateCallback);
            if (cb && *cb)
            {
                bool playing = state.isPlaying;
                juce::MessageManager::callAsync([cb, playing]() {
                    if (*cb)
                        (*cb)(playing);
                });
            }
        }
    });
}

void PluginTransportController::setPositionCallback(PositionCallback callback)
{
    std::atomic_store(&positionCallback, std::make_shared<PositionCallback>(std::move(callback)));
    hostSync.setPositionCallback([this](double timeInSeconds) {
        auto cb = std::atomic_load(&positionCallback);
        if (cb && *cb)
        {
            juce::MessageManager::callAsync([cb, timeInSeconds]() {
                if (*cb)
                    (*cb)(timeInSeconds);
            });
        }
    });
}

void PluginTransportController::setTransportCallback(TransportCallback callback)
{
    hostSync.setTransportCallback(std::move(callback));
}

void PluginTransportController::setTempoCallback(TempoCallback callback)
{
    hostSync.setTempoCallback(std::move(callback));
}

void PluginTransportController::setLoopCallback(LoopCallback callback)
{
    hostSync.setLoopCallback(std::move(callback));
}

void PluginTransportController::clearCallbacks()
{
    std::atomic_store(&playStateCallback, std::shared_ptr<PlayStateCallback>());
    std::atomic_store(&positionCallback, std::shared_ptr<PositionCallback>());
    hostSync.clearCallbacks();
}

// ========== Configuration ==========

void PluginTransportController::setPositionUpdateInterval(int intervalMs)
{
    hostSync.setPositionUpdateInterval(intervalMs);
}

void PluginTransportController::setPositionCallbackEnabled(bool enabled)
{
    hostSync.setPositionCallbackEnabled(enabled);
}
