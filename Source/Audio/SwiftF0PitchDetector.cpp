#include "SwiftF0PitchDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AudioResampler.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace
{
// Model receptive field: see LOOKAHEAD_FRAMES / LEFT_FRAMES in swift_f0/core.py.
constexpr int kLookaheadFrames = 10;
constexpr int kLeftFrames = 11;
constexpr int kWindowFrames = 1875;
// Digital silence makes the model emit random voiced frames; gate it.
constexpr float kSilencePeak = 1e-3f;
} // namespace

SwiftF0PitchDetector::SwiftF0PitchDetector() = default;
SwiftF0PitchDetector::~SwiftF0PitchDetector() = default;

bool SwiftF0PitchDetector::loadModel(const juce::File &modelPath)
{
#ifdef HAVE_ONNXRUNTIME
  lastError.clear();
  loaded = false;
  try
  {
    onnxEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                         "SwiftF0PitchDetector");
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.EnableCpuMemArena();
    options.EnableMemPattern();
    // More than ~4-6 threads does not help this model (per upstream notes).
    const int threads = static_cast<int>(
        std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
    options.SetIntraOpNumThreads(threads);
    // Let the pool sleep between calls; analysis is bursty.
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");

#ifdef _WIN32
    std::wstring path = modelPath.getFullPathName().toWideCharPointer();
#else
    std::string path = modelPath.getFullPathName().toStdString();
#endif
    onnxSession =
        std::make_unique<Ort::Session>(*onnxEnv, path.c_str(), options);

    loaded = true;
    return true;
  }
  catch (const std::exception &e)
  {
    lastError = juce::String(e.what());
    LOG("SwiftF0 model load failed for " + modelPath.getFullPathName() +
        ": " + lastError);
    return false;
  }
#else
  juce::ignoreUnused(modelPath);
  lastError = "ONNX Runtime is not available";
  return false;
#endif
}

std::vector<float> SwiftF0PitchDetector::resampleTo16k(const float *audio,
                                                       int numSamples,
                                                       int srcRate) const
{
  if (srcRate == SWIFT_SAMPLE_RATE)
    return std::vector<float>(audio, audio + numSamples);

  float *channels[1] = {const_cast<float *>(audio)};
  const juce::AudioBuffer<float> input(channels, 1, numSamples);
  auto output = AudioResampler::resample(input, static_cast<double>(srcRate),
                                         static_cast<double>(SWIFT_SAMPLE_RATE));
  const float *out = output.getReadPointer(0);
  return std::vector<float>(out, out + output.getNumSamples());
}

#ifdef HAVE_ONNXRUNTIME
bool SwiftF0PitchDetector::runWindow(const float *samples, int numSamples,
                                     float fmin, float fmax,
                                     std::vector<float> &pitch,
                                     std::vector<float> &confidence)
{
  auto memoryInfo =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  const std::array<int64_t, 2> audioShape{1, numSamples};
  std::array<Ort::Value, 3> inputs{
      Ort::Value::CreateTensor<float>(memoryInfo, const_cast<float *>(samples),
                                      static_cast<size_t>(numSamples),
                                      audioShape.data(), audioShape.size()),
      Ort::Value::CreateTensor<float>(memoryInfo, &fmin, 1, nullptr, 0),
      Ort::Value::CreateTensor<float>(memoryInfo, &fmax, 1, nullptr, 0)};

  const char *inputNames[] = {"audio", "fmin", "fmax"};
  const char *outputNames[] = {"pitch", "confidence"};
  auto outputs = onnxSession->Run(Ort::RunOptions{nullptr}, inputNames,
                                  inputs.data(), inputs.size(), outputNames, 2);

  auto readFloats = [](Ort::Value &value, std::vector<float> &dest)
  {
    auto info = value.GetTensorTypeAndShapeInfo();
    const auto count = info.GetElementCount();
    dest.resize(count);
    // Upstream exports pitch as float64 and confidence as float32; accept
    // either so a re-exported model keeps working.
    if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE)
    {
      const double *src = value.GetTensorData<double>();
      for (size_t i = 0; i < count; ++i)
        dest[i] = static_cast<float>(src[i]);
    }
    else
    {
      const float *src = value.GetTensorData<float>();
      std::copy(src, src + count, dest.begin());
    }
  };
  readFloats(outputs[0], pitch);
  readFloats(outputs[1], confidence);
  return pitch.size() == confidence.size();
}
#endif

std::vector<float> SwiftF0PitchDetector::extractF0(const float *audio,
                                                   int numSamples,
                                                   int sampleRate,
                                                   float threshold, float fmin,
                                                   float fmax)
{
  lastConfidence.clear();
#ifdef HAVE_ONNXRUNTIME
  if (!loaded || !onnxSession)
  {
    lastError = "SwiftF0 model is not loaded";
    return {};
  }
  if (audio == nullptr || numSamples <= 0 || sampleRate <= 0)
  {
    lastError = "SwiftF0: empty audio";
    return {};
  }
  lastError.clear();
  fmin = std::max(fmin, F0_MIN);
  fmax = std::min(fmax, F0_MAX);

  try
  {
    const auto signal = resampleTo16k(audio, numSamples, sampleRate);
    const int len = static_cast<int>(signal.size());
    if (len == 0)
    {
      lastError = "SwiftF0: audio too short to resample";
      return {};
    }

    const int n = std::max(1, len / HOP_SIZE);
    std::vector<float> f0(static_cast<size_t>(n), 0.0f);
    std::vector<float> conf(static_cast<size_t>(n), 0.0f);
    std::vector<float> pitch, confidence;

    for (int start = 0; start < n; start += kWindowFrames)
    {
      const int end = std::min(start + kWindowFrames, n);
      const int left = std::max(0, start - kLeftFrames);
      const int from = left * HOP_SIZE;
      const int to = end < n ? std::min(len, (end + kLookaheadFrames) * HOP_SIZE)
                             : len;
      if (!runWindow(signal.data() + from, to - from, fmin, fmax, pitch,
                     confidence))
      {
        lastError = "SwiftF0: unexpected model output";
        return {};
      }
      const int produced = static_cast<int>(confidence.size());
      for (int f = start; f < end; ++f)
      {
        const int local = f - left;
        if (local < 0 || local >= produced)
          continue;
        f0[static_cast<size_t>(f)] = pitch[static_cast<size_t>(local)];
        conf[static_cast<size_t>(f)] = confidence[static_cast<size_t>(local)];
      }
    }

    // Silence gate + voicing decision.
    for (int f = 0; f < n; ++f)
    {
      const int s0 = f * HOP_SIZE;
      const int s1 = std::min(len, s0 + HOP_SIZE);
      float peak = 0.0f;
      for (int s = s0; s < s1; ++s)
        peak = std::max(peak, std::abs(signal[static_cast<size_t>(s)]));
      if (peak < kSilencePeak)
        conf[static_cast<size_t>(f)] = 0.0f;

      const float p = f0[static_cast<size_t>(f)];
      const bool voiced = conf[static_cast<size_t>(f)] >= threshold &&
                          std::isfinite(p) && p > 0.0f;
      if (!voiced)
        f0[static_cast<size_t>(f)] = 0.0f;
    }

    lastConfidence = std::move(conf);
    return f0;
  }
  catch (const std::exception &e)
  {
    lastError = juce::String(e.what());
    LOG("SwiftF0 inference failed: " + lastError);
    return {};
  }
#else
  juce::ignoreUnused(audio, numSamples, sampleRate, threshold, fmin, fmax);
  lastError = "ONNX Runtime is not available";
  return {};
#endif
}
