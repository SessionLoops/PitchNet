#pragma once

#include "../../JuceHeader.h"
#include "HostSyncService.h"
#include <atomic>
#include <functional>

/**
 * Plugin Transport Controller
 *
 * Provides bidirectional transport control between plugin and host.
 * Handles play/pause, stop, and seek requests from the UI,
 * and notifies the UI of host transport state changes.
 *
 * Design principles:
 * - Lock-free communication between audio and UI threads
 * - Unified interface for both ARA and non-ARA modes
 * - Graceful degradation when host doesn't support transport control
 *
 * Usage:
 *   // In PluginProcessor constructor:
 *   transportController = std::make_unique<PluginTransportController>();
 *
 *   // In processBlock:
 *   transportController->processBlock(getPlayHead(), getSampleRate());
 *
 *   // From UI thread:
 *   transportController->requestPlay(true);
 *   transportController->requestSeek(5.0); // seek to 5 seconds
 */
class PluginTransportController
{
public:
    // ========== Callbacks (UI thread) ==========

    using PlayStateCallback = std::function<void(bool isPlaying)>;
    using PositionCallback = std::function<void(double timeInSeconds)>;
    using TransportCallback = std::function<void(const HostSyncService::TransportState&)>;
    using TempoCallback = std::function<void(const HostSyncService::TempoInfo&)>;
    using LoopCallback = std::function<void(const HostSyncService::LoopInfo&)>;

    PluginTransportController();
    ~PluginTransportController();

    // ========== Audio Thread Methods ==========

    /**
     * Process pending transport requests and update sync state.
     * Call this from processBlock() on the audio thread.
     * @param playHead The host's AudioPlayHead (may be nullptr)
     * @param sampleRate Current sample rate
     */
    void processBlock(juce::AudioPlayHead* playHead, double sampleRate);

    /**
     * Process pending host transport requests without publishing host sync
     * callbacks. ARA playback renderers own UI playhead sync in ARA mode.
     */
    void processPendingRequestsOnly(juce::AudioPlayHead* playHead);

    /**
     * Get the current sync state (thread-safe snapshot).
     */
    HostSyncService::SyncState getCurrentState() const;

    /**
     * Check if host is currently playing.
     */
    bool isHostPlaying() const;

    /**
     * Get current playback position in seconds.
     */
    double getPositionSeconds() const;

    /**
     * Get current tempo in BPM.
     */
    double getTempoBpm() const;

    // ========== UI Thread Methods - Transport Control ==========

    /**
     * Request host to start/stop playback.
     * @param shouldPlay true to start, false to stop
     */
    void requestPlay(bool shouldPlay);

    /**
     * Request host to stop and rewind to beginning.
     */
    void requestStop();

    /**
     * Request host to seek to a specific position.
     * Note: Not all hosts support seeking. The position will be stored
     * and used for internal cursor tracking if host doesn't support it.
     * @param timeInSeconds Target position in seconds
     */
    void requestSeek(double timeInSeconds);

    /**
     * Toggle play/pause state.
     */
    void togglePlayPause();

    /**
     * Check if host supports transport control.
     * Updated after first processBlock call.
     */
    bool canControlTransport() const { return hostCanControlTransport.load(); }

    // ========== UI Thread Methods - Callbacks ==========

    /**
     * Set callback for play state changes.
     * Called on the message thread when play state changes.
     */
    void setPlayStateCallback(PlayStateCallback callback);

    /**
     * Set callback for position updates during playback.
     * Called on the message thread with throttled updates.
     */
    void setPositionCallback(PositionCallback callback);

    /**
     * Set callback for full transport state changes.
     */
    void setTransportCallback(TransportCallback callback);

    /**
     * Set callback for tempo changes.
     */
    void setTempoCallback(TempoCallback callback);

    /**
     * Set callback for host loop/cycle changes.
     */
    void setLoopCallback(LoopCallback callback);

    /**
     * Clear all callbacks.
     */
    void clearCallbacks();

    // ========== Configuration ==========

    /**
     * Set minimum interval between position callbacks (in milliseconds).
     * Default is 16ms (~60fps).
     */
    void setPositionUpdateInterval(int intervalMs);

    /**
     * Enable/disable position callbacks during playback.
     */
    void setPositionCallbackEnabled(bool enabled);

    /**
     * Get the underlying HostSyncService for advanced usage.
     */
    HostSyncService& getHostSyncService() { return hostSync; }

private:
    HostSyncService hostSync;

    // Transport control requests (UI -> Audio thread)
    std::atomic<bool> pendingPlayRequest{false};
    std::atomic<bool> requestedPlayState{false};
    std::atomic<bool> pendingStopRequest{false};
    std::atomic<bool> pendingSeekRequest{false};
    std::atomic<double> requestedSeekPosition{0.0};

    // Host capability (updated on audio thread)
    std::atomic<bool> hostCanControlTransport{false};

    // Internal state for seek tracking
    std::atomic<double> internalCursorPosition{0.0};
    std::atomic<bool> useInternalCursor{false};

    // Callbacks (stored with atomic operations for thread safety)
    std::shared_ptr<PlayStateCallback> playStateCallback;
    std::shared_ptr<PositionCallback> positionCallback;

    // Previous state for change detection
    bool previousPlayState = false;

    // Process pending requests on audio thread
    void processPendingRequests(juce::AudioPlayHead* playHead);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginTransportController)
};
