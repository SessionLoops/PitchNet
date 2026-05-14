#pragma once

#include "../JuceHeader.h"
#include "GPUProvider.h"
#include <vector>
#include <array>
#include <memory>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

/**
 * RMVPE (Robust Model for Vocal Pitch Estimation) - Deep learning based pitch detector.
 * Uses ONNX Runtime for inference.
 *
 * Based on the RMVPE paper and implementation.
 * Input: Raw waveform at 16kHz
 * Output: F0 values in Hz
 */
class RMVPEPitchDetector
{
public:
    // RMVPE configuration constants (from constants.py)
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int N_CLASS = 360;
    static constexpr int N_MELS = 128;
    static constexpr int MEL_FMIN = 30;
    static constexpr int MEL_FMAX = SAMPLE_RATE / 2; // 8000
    static constexpr int WINDOW_LENGTH = 1024;
    static constexpr int HOP_SIZE = 160;
    static constexpr float RMVPE_CONST = 1997.3794084376191f; // Renamed from CONST to avoid Windows macro conflict
    static constexpr float DEFAULT_THRESHOLD = 0.03f;

    RMVPEPitchDetector();
    ~RMVPEPitchDetector();

    /**
     * Load RMVPE model from ONNX file.
     * @param modelPath Path to rmvpe.onnx
     * @param provider GPU provider (CPU, CUDA, or DirectML)
     * @param deviceId GPU device ID (0 = first GPU)
     * @return true if successful
     */
    bool loadModel(const juce::File &modelPath,
                   GPUProvider provider = GPUProvider::CPU,
                   int deviceId = 0);

    /**
     * Check if model is loaded.
     */
    bool isLoaded() const { return loaded; }

    /**
     * Extract F0 from audio buffer.
     * The audio will be resampled to 16kHz internally.
     *
     * @param audio Audio samples
     * @param numSamples Number of samples
     * @param sampleRate Original sample rate
     * @param threshold Confidence threshold (default 0.03)
     * @return F0 values in Hz (0 for unvoiced frames)
     */
    std::vector<float> extractF0(const float *audio, int numSamples,
                                 int sampleRate, float threshold = DEFAULT_THRESHOLD);

    /**
     * Extract F0 with progress callback.
     */
    std::vector<float> extractF0WithProgress(const float *audio, int numSamples,
                                             int sampleRate, float threshold,
                                             std::function<void(double)> progressCallback);

    /**
     * Get the number of F0 frames that will be produced for given audio length.
     */
    int getNumFrames(int numSamples, int sampleRate) const;

    /**
     * Get the time in seconds for a given frame index.
     */
    float getTimeForFrame(int frameIndex) const;

    /**
     * Get hop size in original sample rate.
     */
    int getHopSizeForSampleRate(int sampleRate) const;

private:
    bool loaded = false;
    bool usesMelInput = false;
    bool melInputChannelsFirst = true;

    std::vector<std::vector<float>> melFilterbank;
    std::vector<float> hannWindow;

    // Resample audio to 16kHz
    std::vector<float> resampleTo16k(const float *audio, int numSamples, int srcRate);

    void initMelFilterbank();
    void initHannWindow();
    std::vector<std::vector<float>> extractMel(const std::vector<float> &audio);

    // Process a single chunk of 16kHz audio
    std::vector<float> extractF0Chunk(const float *audio16k, int numSamples, float threshold);

    // Decode hidden states to F0 (matching Python decode function)
    std::vector<float> decodeF0(const float *hidden, int numFrames, float threshold);

#ifdef HAVE_ONNXRUNTIME
    std::unique_ptr<Ort::Env> onnxEnv;
    std::unique_ptr<Ort::Session> onnxSession;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;

    std::vector<const char *> inputNames;
    std::vector<const char *> outputNames;
    std::vector<std::string> inputNameStrings;
    std::vector<std::string> outputNameStrings;

    // Reused per-inference buffers to reduce hot-path allocations.
    std::array<int64_t, 2> waveformShapeScratch{1, 0};
    std::array<int64_t, 3> melShapeScratch{1, N_MELS, 0};
    std::array<int64_t, 1> thresholdShapeScratch{1};
    std::array<float, 1> thresholdScratch{DEFAULT_THRESHOLD};
    std::vector<float> melInputScratch;
    std::vector<float> latentScratch;
    std::vector<Ort::Value> inputTensorScratch;
#endif
};
