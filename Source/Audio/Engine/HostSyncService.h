#pragma once

#include "../../JuceHeader.h"
#include <atomic>
#include <functional>

/**
 * Host Synchronization Service
 *
 * Provides a unified interface for synchronizing with DAW hosts.
 * Handles transport state, position, tempo, time signature, and loop regions.
 *
 * Design principles:
 * - Lock-free communication between audio and UI threads
 * - Coalesced updates to avoid flooding the message thread
 * - Safe pointer handling to prevent dangling references
 * - Decoupled from UI components (pure service layer)
 *
 * Thread safety:
 * - All public methods are thread-safe
 * - Audio thread methods are real-time safe (no allocations, no locks)
 * - UI thread methods use MessageManager::callAsync for safe updates
 */
class HostSyncService
{
public:
    // ========== Transport State ==========

    struct TransportState
    {
        bool isPlaying = false;
        bool isRecording = false;
        bool isLooping = false;

        bool operator==(const TransportState& other) const
        {
            return isPlaying == other.isPlaying &&
                   isRecording == other.isRecording &&
                   isLooping == other.isLooping;
        }

        bool operator!=(const TransportState& other) const { return !(*this == other); }
    };

    // ========== Position Info ==========

    struct PositionInfo
    {
        double timeInSeconds = 0.0;
        juce::int64 timeInSamples = 0;
        double ppqPosition = 0.0;      // Position in quarter notes (PPQ)
        double ppqPositionOfLastBarStart = 0.0;
        int barCount = 0;              // Current bar number

        bool hasTimeInSeconds = false;
        bool hasTimeInSamples = false;
        bool hasPpqPosition = false;
        bool hasPpqPositionOfLastBarStart = false;
        bool hasBarCount = false;
    };

    // ========== Tempo Info ==========

    struct TempoInfo
    {
        double bpm = 120.0;
        int timeSigNumerator = 4;
        int timeSigDenominator = 4;

        bool hasBpm = false;
        bool hasTimeSignature = false;

        double getSecondsPerBeat() const { return 60.0 / bpm; }
        double getSecondsPerBar() const { return getSecondsPerBeat() * timeSigNumerator; }
        double getBeatsPerBar() const { return static_cast<double>(timeSigNumerator); }
    };

    // ========== Loop Info ==========

    struct LoopInfo
    {
        double loopStartPpq = 0.0;
        double loopEndPpq = 0.0;
        double loopStartSeconds = 0.0;
        double loopEndSeconds = 0.0;

        bool hasLoopPoints = false;
        bool isLoopEnabled = false;

        bool operator==(const LoopInfo& other) const
        {
            constexpr double epsilon = 0.0001;
            return std::abs(loopStartPpq - other.loopStartPpq) < epsilon &&
                   std::abs(loopEndPpq - other.loopEndPpq) < epsilon &&
                   std::abs(loopStartSeconds - other.loopStartSeconds) < epsilon &&
                   std::abs(loopEndSeconds - other.loopEndSeconds) < epsilon &&
                   hasLoopPoints == other.hasLoopPoints &&
                   isLoopEnabled == other.isLoopEnabled;
        }

        bool operator!=(const LoopInfo& other) const { return !(*this == other); }
    };

    // ========== Combined Sync State ==========

    struct SyncState
    {
        TransportState transport;
        PositionInfo position;
        TempoInfo tempo;
        LoopInfo loop;
        double sampleRate = 44100.0;

        // Convert PPQ to seconds using current tempo
        double ppqToSeconds(double ppq) const
        {
            if (tempo.bpm <= 0.0) return 0.0;
            return ppq * 60.0 / tempo.bpm;
        }

        // Convert seconds to PPQ using current tempo
        double secondsToPpq(double seconds) const
        {
            if (tempo.bpm <= 0.0) return 0.0;
            return seconds * tempo.bpm / 60.0;
        }

        // Get current bar and beat position
        void getBarBeatPosition(int& bar, double& beat) const
        {
            if (!position.hasPpqPosition || !tempo.hasTimeSignature)
            {
                bar = 1;
                beat = 1.0;
                return;
            }

            double beatsPerBar = tempo.getBeatsPerBar();
            double totalBeats = position.ppqPosition;

            bar = static_cast<int>(totalBeats / beatsPerBar) + 1;
            beat = std::fmod(totalBeats, beatsPerBar) + 1.0;
        }
    };

    // ========== Callbacks ==========

    using TransportCallback = std::function<void(const TransportState&)>;
    using PositionCallback = std::function<void(double timeInSeconds)>;
    using TempoCallback = std::function<void(const TempoInfo&)>;
    using LoopCallback = std::function<void(const LoopInfo&)>;
    using FullSyncCallback = std::function<void(const SyncState&)>;

    // ========== Constructor / Destructor ==========

    HostSyncService();
    ~HostSyncService();

    // ========== Audio Thread Methods (Real-time safe) ==========

    /**
     * Update sync state from host's AudioPlayHead.
     * Call this from processBlock() on the audio thread.
     * @param playHead The host's AudioPlayHead (may be nullptr)
     * @param sampleRate Current sample rate
     */
    void updateFromPlayHead(juce::AudioPlayHead* playHead, double sampleRate);

    /**
     * Update sync state from JUCE PositionInfo.
     * Call this from processBlock() on the audio thread.
     */
    void updateFromPositionInfo(const juce::AudioPlayHead::PositionInfo& info, double sampleRate);

    /**
     * Get the current sync state (thread-safe snapshot).
     */
    SyncState getCurrentState() const;

    /**
     * Check if host is currently playing.
     */
    bool isHostPlaying() const { return currentState.transport.isPlaying; }

    /**
     * Get current playback position in seconds.
     */
    double getPositionSeconds() const { return currentState.position.timeInSeconds; }

    /**
     * Get current tempo in BPM.
     */
    double getTempoBpm() const { return currentState.tempo.bpm; }

    // ========== UI Thread Methods ==========

    /**
     * Set callback for transport state changes.
     * Called on the message thread when transport state changes.
     */
    void setTransportCallback(TransportCallback callback);

    /**
     * Set callback for position updates.
     * Called on the message thread during playback.
     */
    void setPositionCallback(PositionCallback callback);

    /**
     * Set callback for tempo changes.
     * Called on the message thread when tempo or time signature changes.
     */
    void setTempoCallback(TempoCallback callback);

    /**
     * Set callback for loop region changes.
     */
    void setLoopCallback(LoopCallback callback);

    /**
     * Set callback for full sync state updates.
     * Called on the message thread with complete sync state.
     */
    void setFullSyncCallback(FullSyncCallback callback);

    /**
     * Clear all callbacks.
     */
    void clearCallbacks();

    // ========== Host Transport Control (Best-effort) ==========

    /**
     * Request host to start/stop playback.
     * Note: Not all hosts support this.
     */
    void requestPlay(bool shouldPlay);

    /**
     * Request host to stop and rewind.
     */
    void requestStop();

    /**
     * Check if there's a pending play request.
     */
    bool hasPendingPlayRequest() const { return pendingPlayRequest.load(); }

    /**
     * Check if there's a pending stop request.
     */
    bool hasPendingStopRequest() const { return pendingStopRequest.load(); }

    /**
     * Process pending transport requests.
     * Call this from processBlock() with the host's playHead.
     */
    void processPendingRequests(juce::AudioPlayHead* playHead);

    // ========== Configuration ==========

    /**
     * Set minimum interval between position callbacks (in milliseconds).
     * Default is 16ms (~60fps).
     */
    void setPositionUpdateInterval(int intervalMs) { positionUpdateIntervalMs = intervalMs; }

    /**
     * Enable/disable position callbacks during playback.
     */
    void setPositionCallbackEnabled(bool enabled) { positionCallbackEnabled = enabled; }

private:
    // Current sync state (updated on audio thread)
    SyncState currentState;

    // Atomic state for lock-free UI updates
    struct AtomicSyncState
    {
        std::atomic<double> positionSeconds{0.0};
        std::atomic<double> bpm{120.0};
        std::atomic<bool> isPlaying{false};
        std::atomic<bool> isRecording{false};
        std::atomic<bool> isLooping{false};
        std::atomic<bool> positionPending{false};
        std::atomic<bool> transportPending{false};
        std::atomic<bool> tempoPending{false};
    };
    std::shared_ptr<AtomicSyncState> atomicState;

    // Callbacks (stored with atomic_load/store for thread safety)
    std::shared_ptr<TransportCallback> transportCallback;
    std::shared_ptr<PositionCallback> positionCallback;
    std::shared_ptr<TempoCallback> tempoCallback;
    std::shared_ptr<LoopCallback> loopCallback;
    std::shared_ptr<FullSyncCallback> fullSyncCallback;

    // Transport control requests
    std::atomic<bool> pendingPlayRequest{false};
    std::atomic<bool> requestedPlayState{false};
    std::atomic<bool> pendingStopRequest{false};

    // Configuration
    int positionUpdateIntervalMs = 16;
    bool positionCallbackEnabled = true;

    // Previous state for change detection
    TransportState previousTransport;
    PositionInfo previousPosition;
    TempoInfo previousTempo;
    LoopInfo previousLoop;

    // Throttling
    juce::int64 lastPositionCallbackTime = 0;

    // Internal methods
    void notifyTransportChange(const TransportState& state);
    void notifyPositionUpdate(double seconds);
    void notifyTempoChange(const TempoInfo& tempo);
    void notifyLoopChange(const LoopInfo& loop);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostSyncService)
};
