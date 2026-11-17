#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace inputtype
{

/**
 * "Is this a voice?" from Google's YAMNet AudioSet tagger (Apache-2.0;
 * tensorflow/models research/audioset/yamnet), exported to ONNX with its log-mel
 * frontend inside the graph (Resources/tools/README.md).
 *
 * The model takes mono 16 kHz audio and scores 521 AudioSet classes for each
 * 0.96 s patch (0.48 s hop; shorter input is zero-padded to one patch). The
 * voice score is the patch-averaged score's maximum over the speech classes
 * (Speech .. Speech synthesizer) and the singing family (Singing, Choir,
 * Yodeling, Chant, Mantra, Child singing, Synthetic singing, Rapping, Humming),
 * as measured in docs/testing/vocal-detection-research.md. AudioSet scores are
 * not calibrated across classes, so the threshold applied to it is our own.
 *
 * Needs ONNX Runtime (HAVE_ONNXRUNTIME); without it load() fails. On macOS call
 * OnnxRuntime::initialise() first, as for every other model in PitchNet.
 */
class YamnetVoiceTagger
{
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kNumClasses = 521;

    /** Output columns that count as a voice. */
    static const std::vector<int>& voiceClasses();

    YamnetVoiceTagger();
    ~YamnetVoiceTagger();

    bool load(const std::filesystem::path& modelPath);
    bool isLoaded() const;
    const std::string& lastError() const { return error; }

    /** Patch-averaged scores [kNumClasses] for mono audio at any rate. Empty on failure or cancellation. */
    std::vector<float> scores(const float* mono, int numSamples, double sampleRate,
                              const std::atomic<bool>* cancel = nullptr);

    /** Maximum of `scores` over voiceClasses(); -1 for empty scores. */
    static float voiceScore(const std::vector<float>& scores);

    /** voiceScore(scores(...)); -1 on failure. */
    float voiceScore(const float* mono, int numSamples, double sampleRate,
                     const std::atomic<bool>* cancel = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::string error;
};

} // namespace inputtype
