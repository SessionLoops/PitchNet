// Polyphonic engine - Basic Pitch note evidence (ONNX Runtime). Port of basic_pitch.inference's windowing:
// 2 s windows, 30 overlapping frames, half of the overlap dropped on each side when unwrapping.

#include "BasicPitchEvidence.h"

#include <algorithm>
#include <cmath>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace polyphonic
{

struct BasicPitchEvidence::Impl
{
#ifdef HAVE_ONNXRUNTIME
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
#endif
};

BasicPitchEvidence::BasicPitchEvidence() : impl(std::make_unique<Impl>()) {}
BasicPitchEvidence::~BasicPitchEvidence() = default;

bool BasicPitchEvidence::isLoaded() const
{
#ifdef HAVE_ONNXRUNTIME
    return impl->session != nullptr;
#else
    return false;
#endif
}

bool BasicPitchEvidence::load(const std::filesystem::path& modelPath)
{
    error.clear();
#ifdef HAVE_ONNXRUNTIME
    try
    {
        impl->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "PitchNet-BasicPitch");
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

std::vector<float> BasicPitchEvidence::resample(const float* x, int numSamples, double fromRate, double toRate)
{
    if (numSamples <= 0 || fromRate <= 0 || toRate <= 0)
        return {};
    if (fromRate == toRate)
        return std::vector<float>(x, x + numSamples);
    constexpr double pi = 3.14159265358979323846;
    const double ratio = toRate / fromRate;
    const double fc = 0.5 * std::min(1.0, ratio) * 0.94;       // cutoff, cycles per input sample
    const int zeroCrossings = 24;
    const double half = zeroCrossings / (2.0 * fc);            // kernel half length, input samples
    const int phases = 1024;
    const int taps = static_cast<int>(std::ceil(half));
    // kernel table over [0, half] at `phases` points per input sample
    const double beta = 8.6;
    auto bessel0 = [](double v) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (v / (2.0 * k)) * (v / (2.0 * k));
            sum += term;
            if (term < 1e-12 * sum)
                break;
        }
        return sum;
    };
    const double i0b = bessel0(beta);
    std::vector<float> table(static_cast<size_t>((taps + 1) * phases + 2), 0.0f);
    for (size_t i = 0; i < table.size(); ++i)
    {
        const double d = static_cast<double>(i) / phases;
        if (d > half)
            break;
        const double r = d / half;
        const double win = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        const double arg = 2.0 * fc * d;
        const double sinc = arg == 0.0 ? 1.0 : std::sin(pi * arg) / (pi * arg);
        table[i] = static_cast<float>(2.0 * fc * sinc * win);
    }
    auto kernel = [&](double d) {
        d = std::fabs(d) * phases;
        const size_t i = static_cast<size_t>(d);
        if (i + 1 >= table.size())
            return 0.0f;
        const float u = static_cast<float>(d - static_cast<double>(i));
        return table[i] * (1.0f - u) + table[i + 1] * u;
    };
    const int outLen = static_cast<int>(std::ceil(numSamples * ratio));
    std::vector<float> y(static_cast<size_t>(outLen));
    for (int j = 0; j < outLen; ++j)
    {
        const double t = j / ratio;
        const int k0 = static_cast<int>(std::floor(t)) - taps, k1 = static_cast<int>(std::floor(t)) + taps + 1;
        double s = 0.0;
        for (int k = std::max(0, k0); k <= std::min(numSamples - 1, k1); ++k)
            s += static_cast<double>(x[k]) * kernel(t - k);
        y[static_cast<size_t>(j)] = static_cast<float>(s);
    }
    return y;
}

NoteEvidence BasicPitchEvidence::compute(const float* mono, int numSamples, double sampleRate, const std::atomic<bool>* cancel)
{
    NoteEvidence ev;
#ifdef HAVE_ONNXRUNTIME
    if (!impl->session || mono == nullptr || numSamples <= 0)
        return ev;
    try
    {
        auto audio = resample(mono, numSamples, sampleRate, kSampleRate);
        const int nOrig = static_cast<int>(audio.size());
        const int overlap = kOverlapFrames * kHop;
        audio.insert(audio.begin(), static_cast<size_t>(overlap / 2), 0.0f);
        const int winHop = kWindowSamples - overlap;
        std::vector<int> starts;
        for (int i = 0; i < static_cast<int>(audio.size()); i += winHop)
            starts.push_back(i);
        const int fps = kSampleRate / kHop;                     // 86, as in basic_pitch.constants
        const int nOut = static_cast<int>(std::floor(nOrig * (static_cast<double>(fps) / kSampleRate)));
        const int keep = kWindowFrames - kOverlapFrames;        // frames kept per window
        std::vector<float> note;
        note.reserve(static_cast<size_t>(starts.size()) * keep * 88);

        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inputNames[] = {"serving_default_input_2:0"};
        const char* outputNames[] = {"StatefulPartitionedCall:1"};   // the note posteriorgram
        constexpr int batch = 8;
        std::vector<float> input;
        for (size_t w0 = 0; w0 < starts.size(); w0 += batch)
        {
            if (cancel && cancel->load())
                return {};
            const int nb = static_cast<int>(std::min<size_t>(batch, starts.size() - w0));
            input.assign(static_cast<size_t>(nb) * kWindowSamples, 0.0f);
            for (int b = 0; b < nb; ++b)
            {
                const int s = starts[w0 + static_cast<size_t>(b)];
                const int n = std::min(kWindowSamples, static_cast<int>(audio.size()) - s);
                std::copy(audio.begin() + s, audio.begin() + s + n, input.begin() + static_cast<std::ptrdiff_t>(b) * kWindowSamples);
            }
            const int64_t shape[] = {nb, kWindowSamples, 1};
            auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape, 3);
            auto outputs = impl->session->Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
            const size_t expected = static_cast<size_t>(nb) * kWindowFrames * 88;
            if (outputs.size() != 1 || outputs[0].GetTensorTypeAndShapeInfo().GetElementCount() != expected)
            {
                error = "unexpected Basic Pitch output shape";
                return {};
            }
            const float* yn = outputs[0].GetTensorData<float>();
            for (int b = 0; b < nb; ++b)
                for (int f = kOverlapFrames / 2; f < kWindowFrames - kOverlapFrames / 2; ++f)
                {
                    const size_t off = (static_cast<size_t>(b) * kWindowFrames + static_cast<size_t>(f)) * 88;
                    note.insert(note.end(), yn + off, yn + off + 88);
                }
        }
        const int frames = std::min(nOut, static_cast<int>(note.size() / 88));
        note.resize(static_cast<size_t>(frames) * 88);
        // basic_pitch.note_creation.model_frames_to_time, including its per-window correction
        const double winOffset = (static_cast<double>(kHop) / kSampleRate) * (kWindowFrames - static_cast<double>(kWindowSamples) / kHop) + 0.0018;
        ev.frameTimes.resize(static_cast<size_t>(frames));
        for (int f = 0; f < frames; ++f)
            ev.frameTimes[static_cast<size_t>(f)] = static_cast<float>(f * static_cast<double>(kHop) / kSampleRate - winOffset * std::floor(f / static_cast<double>(kWindowFrames)));
        ev.note = std::move(note);
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
#endif
    return ev;
}

} // namespace polyphonic
