#pragma once

#include "PolyEngine.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace polyphonic
{

/**
 * Neural note evidence for the polyphonic engine's purifier from Spotify's Basic Pitch model
 * (Bittner et al., "A Lightweight Instrument-Agnostic Model for Polyphonic Note
 * Transcription and Multipitch Estimation", ICASSP 2022; Apache-2.0).
 *
 * Runs the ~230 kB "nmp" ONNX model on the CPU over 2-second windows at
 * 22050 Hz and returns its 88-key note posteriorgram. With this
 * evidence the purifier separates real notes from harmonic "ghosts" far more
 * reliably than from spectral cues alone (see docs/polyphonic-editing.md).
 *
 * Needs ONNX Runtime (HAVE_ONNXRUNTIME). Without it, or without the model file,
 * load() fails and analysis falls back to the DSP-only purifier. On macOS call
 * OnnxRuntime::initialise() first, as for every other model in PitchNet.
 */
class BasicPitchEvidence
{
public:
    static constexpr int kSampleRate = 22050;
    static constexpr int kHop = 256;
    static constexpr int kWindowSamples = kSampleRate * 2 - kHop;   // 43844
    static constexpr int kWindowFrames = 172;
    static constexpr int kOverlapFrames = 30;

    BasicPitchEvidence();
    ~BasicPitchEvidence();

    bool load(const std::filesystem::path& modelPath);
    bool isLoaded() const;
    const std::string& lastError() const { return error; }

    /** Posteriorgrams for mono audio at any sample rate. Empty on failure or cancellation. */
    NoteEvidence compute(const float* mono, int numSamples, double sampleRate,
                         const std::atomic<bool>* cancel = nullptr);

    /** Windowed-sinc (Kaiser) resampler used for the model input; exposed for tests. */
    static std::vector<float> resample(const float* x, int numSamples, double fromRate, double toRate);

    /** Times of the retained model frames for numSamples at kSampleRate, excluding padded audio. */
    static std::vector<float> frameTimesForSamples(int numSamples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::string error;
};

} // namespace polyphonic
