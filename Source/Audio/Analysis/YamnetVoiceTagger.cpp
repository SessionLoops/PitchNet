// YAMNet voice score (ONNX Runtime). See YamnetVoiceTagger.h.

#include "YamnetVoiceTagger.h"

#include "../Polyphonic/BasicPitchEvidence.h"

#include <algorithm>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace inputtype
{

struct YamnetVoiceTagger::Impl
{
#ifdef HAVE_ONNXRUNTIME
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
#endif
};

const std::vector<int>& YamnetVoiceTagger::voiceClasses()
{
    // yamnet_class_map.csv: 0-5 Speech, Child speech, Conversation, Narration,
    // Babbling, Speech synthesizer; 24-32 Singing, Choir, Yodeling, Chant, Mantra,
    // Child singing, Synthetic singing, Rapping, Humming.
    static const std::vector<int> classes = {0, 1, 2, 3, 4, 5, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    return classes;
}

YamnetVoiceTagger::YamnetVoiceTagger() : impl(std::make_unique<Impl>()) {}
YamnetVoiceTagger::~YamnetVoiceTagger() = default;

bool YamnetVoiceTagger::isLoaded() const
{
#ifdef HAVE_ONNXRUNTIME
    return impl->session != nullptr;
#else
    return false;
#endif
}

bool YamnetVoiceTagger::load(const std::filesystem::path& modelPath)
{
    error.clear();
#ifdef HAVE_ONNXRUNTIME
    try
    {
        impl->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "PitchNet-YAMNet");
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetIntraOpNumThreads(2);
        impl->session = std::make_unique<Ort::Session>(*impl->env, modelPath.c_str(), options);
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        impl->session.reset();
        return false;
    }
#else
    (void) modelPath;
    error = "ONNX Runtime is not available";
    return false;
#endif
}

std::vector<float> YamnetVoiceTagger::scores(const float* mono, int numSamples, double sampleRate,
                                             const std::atomic<bool>* cancel)
{
    error.clear();
#ifdef HAVE_ONNXRUNTIME
    if (!impl->session || mono == nullptr || numSamples <= 0 || sampleRate <= 0)
        return {};
    try
    {
        auto audio = polyphonic::BasicPitchEvidence::resample(mono, numSamples, sampleRate, kSampleRate);
        if (audio.empty() || (cancel && cancel->load()))
            return {};
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[] = {static_cast<int64_t>(audio.size())};
        auto tensor = Ort::Value::CreateTensor<float>(memory, audio.data(), audio.size(), shape, 1);
        const char* inputNames[] = {"waveform"};
        const char* outputNames[] = {"scores"};
        auto outputs = impl->session->Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
        if (outputs.size() != 1)
        {
            error = "unexpected YAMNet output count";
            return {};
        }
        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto dims = info.GetShape();
        if (dims.size() != 2 || dims[1] != kNumClasses || dims[0] < 1)
        {
            error = "unexpected YAMNet output shape";
            return {};
        }
        const float* s = outputs[0].GetTensorData<float>();
        const auto patches = static_cast<size_t>(dims[0]);
        std::vector<float> mean(kNumClasses, 0.0f);
        for (size_t p = 0; p < patches; ++p)
            for (size_t c = 0; c < static_cast<size_t>(kNumClasses); ++c)
                mean[c] += s[p * kNumClasses + c];
        for (auto& v : mean)
            v /= static_cast<float>(patches);
        return mean;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return {};
    }
#else
    (void) mono;
    (void) numSamples;
    (void) sampleRate;
    (void) cancel;
    error = "ONNX Runtime is not available";
    return {};
#endif
}

float YamnetVoiceTagger::voiceScore(const std::vector<float>& scores)
{
    if (scores.size() != static_cast<size_t>(kNumClasses))
        return -1.0f;
    float best = 0.0f;
    for (int c : voiceClasses())
        best = std::max(best, scores[static_cast<size_t>(c)]);
    return best;
}

float YamnetVoiceTagger::voiceScore(const float* mono, int numSamples, double sampleRate,
                                    const std::atomic<bool>* cancel)
{
    return voiceScore(scores(mono, numSamples, sampleRate, cancel));
}

} // namespace inputtype
