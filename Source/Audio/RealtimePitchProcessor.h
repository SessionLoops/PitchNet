#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "Vocoder.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

/**
 * Real-time pitch correction processor
 * Pre-computes processed audio in background, provides real-time playback
 */
class RealtimePitchProcessor {
public:
    RealtimePitchProcessor();
    ~RealtimePitchProcessor();

    void setProject(Project* proj);
    void setVocoder(Vocoder* voc);
    void prepareToPlay(double sampleRate, int samplesPerBlock);

    /**
     * Process audio block
     * @return true if processed audio was used, false if passthrough
     */
    bool processBlock(juce::AudioBuffer<float>& input,
                      juce::AudioBuffer<float>& output,
                      const juce::AudioPlayHead::PositionInfo* positionInfo);

    /**
     * Trigger re-computation (call when project data changes)
     */
    void invalidate();

    bool isReady() const { return ready.load(); }
    double getPosition() const { return position.load(); }
    void setPosition(double positionSeconds) { position.store(positionSeconds); }

private:
    void startComputation();
    void computeInBackground();

    /// Install a newly built cache. Keeps the outgoing buffer alive so
    /// processBlock() can ramp between the two instead of cutting.
    /// Caller must hold bufferLock.
    void publishProcessedBuffer(juce::AudioBuffer<float>&& buffer);

    Project* project = nullptr;
    Vocoder* vocoder = nullptr;
    double sampleRate = 44100.0;

    // Length of the ramp between an outgoing and an incoming cache. A
    // resynthesis commit that lands mid-playback used to drop straight to the
    // dry input until the new cache was built, which is an abrupt jump between
    // two unrelated signals at both edges.
    static constexpr int kBufferSwapFadeSamples = 512;

    juce::AudioBuffer<float> processedBuffer;
    juce::AudioBuffer<float> previousProcessedBuffer;
    int swapFadeRemaining = 0;
    std::atomic<bool> ready{false};
    std::atomic<bool> computing{false};
    std::atomic<bool> cancelCompute{false};
    std::atomic<std::uint64_t> invalidateGeneration{0};
    std::atomic<double> position{0.0};

    // Continuous read cursor, touched only on the audio thread. Lets playback
    // follow the host position contiguously and ignore sub-sample jitter in the
    // host's reported time, which would otherwise cause boundary clicks.
    juce::int64 readPosition = 0;
    bool readPositionValid = false;

    juce::CriticalSection bufferLock;
    std::unique_ptr<std::thread> computeThread;
};
