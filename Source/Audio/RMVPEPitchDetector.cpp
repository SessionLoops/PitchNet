#include "RMVPEPitchDetector.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

RMVPEPitchDetector::RMVPEPitchDetector()
{
  initMelFilterbank();
  initHannWindow();
}

RMVPEPitchDetector::~RMVPEPitchDetector() = default;

bool RMVPEPitchDetector::loadModel(const juce::File &modelPath,
                                   GPUProvider provider, int deviceId)
{
#ifdef HAVE_ONNXRUNTIME
  try
  {
    // Initialize ONNX Runtime
    onnxEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                         "RMVPEPitchDetector");

    Ort::SessionOptions sessionOptions;
    sessionOptions.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOptions.EnableCpuMemArena();

    if (provider == GPUProvider::CPU)
    {
      const int numThreads = std::max(1u, std::thread::hardware_concurrency()) / 2;
      sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
      sessionOptions.EnableMemPattern();
    }
    else
    {
      sessionOptions.SetIntraOpNumThreads(1);
      sessionOptions.SetInterOpNumThreads(1);
    }

    // Configure execution provider based on GPU settings
#if defined(_WIN32) && defined(USE_DIRECTML)
    if (provider == GPUProvider::DirectML)
    {
      try
      {
        const OrtApi &ortApi = Ort::GetApi();
        const OrtDmlApi *ortDmlApi = nullptr;
        Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
            "DML", ORT_API_VERSION,
            reinterpret_cast<const void **>(&ortDmlApi)));

        sessionOptions.DisableMemPattern();
        sessionOptions.SetExecutionMode(ORT_SEQUENTIAL);

        Ort::ThrowOnError(ortDmlApi->SessionOptionsAppendExecutionProvider_DML(
            sessionOptions, deviceId));
      }
      catch (const Ort::Exception &e)
      {
      }
    }
    else
#endif
#ifdef USE_CUDA
        if (provider == GPUProvider::CUDA)
    {
      try
      {
        OrtCUDAProviderOptions cudaOptions;
        cudaOptions.device_id = deviceId;
        cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
        cudaOptions.arena_extend_strategy = 1;
        sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
      }
      catch (const Ort::Exception &e)
      {
      }
    }
    else
#endif
        if (provider == GPUProvider::CoreML)
    {
      try
      {
        sessionOptions.AppendExecutionProvider("CoreML",
                                               {{"MLComputeUnits", "ALL"}});
      }
      catch (const Ort::Exception &e)
      {
      }
    }
    else
    {
      if (provider != GPUProvider::CPU)
      {
      }
    }

#ifdef _WIN32
    std::wstring modelPathW = modelPath.getFullPathName().toWideCharPointer();
    onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                                 sessionOptions);
#else
    std::string modelPathStr = modelPath.getFullPathName().toStdString();
    onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathStr.c_str(),
                                                 sessionOptions);
#endif

    allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();

    // Get input/output names
    size_t numInputs = onnxSession->GetInputCount();
    size_t numOutputs = onnxSession->GetOutputCount();

    inputNameStrings.clear();
    outputNameStrings.clear();
    inputNames.clear();
    outputNames.clear();

    for (size_t i = 0; i < numInputs; ++i)
    {
      auto namePtr = onnxSession->GetInputNameAllocated(i, *allocator);
      inputNameStrings.push_back(namePtr.get());
    }

    for (size_t i = 0; i < numOutputs; ++i)
    {
      auto namePtr = onnxSession->GetOutputNameAllocated(i, *allocator);
      outputNameStrings.push_back(namePtr.get());
    }

    for (const auto &name : inputNameStrings)
      inputNames.push_back(name.c_str());
    for (const auto &name : outputNameStrings)
      outputNames.push_back(name.c_str());

    usesMelInput = false;
    melInputChannelsFirst = true;
    if (numInputs > 0)
    {
      auto inputTypeInfo = onnxSession->GetInputTypeInfo(0);
      auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
      auto inputShape = inputTensorInfo.GetShape();
      if (inputShape.size() == 3)
      {
        usesMelInput = true;
        melInputChannelsFirst =
            inputShape[1] == N_MELS || inputShape[2] != N_MELS;
      }
    }

    inputTensorScratch.reserve(2);

    loaded = true;
    return true;
  }
  catch (const Ort::Exception &e)
  {
    loaded = false;
    return false;
  }
  catch (const std::exception &e)
  {
    loaded = false;
    return false;
  }
#else
  return false;
#endif
}

void RMVPEPitchDetector::initMelFilterbank()
{
  const int numBins = WINDOW_LENGTH / 2 + 1;
  melFilterbank.assign(N_MELS, std::vector<float>(numBins, 0.0f));

  auto hzToMel = [](float hz) -> float
  {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
  };

  auto melToHz = [](float mel) -> float
  {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
  };

  const float melMin = hzToMel(static_cast<float>(MEL_FMIN));
  const float melMax = hzToMel(static_cast<float>(MEL_FMAX));

  std::vector<float> melPoints(N_MELS + 2);
  std::vector<float> hzPoints(N_MELS + 2);
  for (int i = 0; i <= N_MELS + 1; ++i)
  {
    melPoints[i] = melMin + (melMax - melMin) * i / (N_MELS + 1);
    hzPoints[i] = melToHz(melPoints[i]);
  }

  for (int m = 0; m < N_MELS; ++m)
  {
    const float fLow = hzPoints[m];
    const float fCenter = hzPoints[m + 1];
    const float fHigh = hzPoints[m + 2];

    for (int k = 0; k < numBins; ++k)
    {
      const float freq = static_cast<float>(k) * SAMPLE_RATE / WINDOW_LENGTH;
      if (freq >= fLow && freq < fCenter)
        melFilterbank[m][k] = (freq - fLow) / (fCenter - fLow);
      else if (freq >= fCenter && freq <= fHigh)
        melFilterbank[m][k] = (fHigh - freq) / (fHigh - fCenter);
    }
  }
}

void RMVPEPitchDetector::initHannWindow()
{
  hannWindow.resize(WINDOW_LENGTH);
  for (int i = 0; i < WINDOW_LENGTH; ++i)
  {
    hannWindow[i] =
        0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i /
                                (WINDOW_LENGTH - 1)));
  }
}

std::vector<float> RMVPEPitchDetector::resampleTo16k(const float *audio,
                                                     int numSamples,
                                                     int srcRate)
{
  if (srcRate == SAMPLE_RATE)
  {
    return std::vector<float>(audio, audio + numSamples);
  }

  // Linear interpolation resampling
  double ratio = static_cast<double>(SAMPLE_RATE) / srcRate;
  int outSamples = static_cast<int>(numSamples * ratio);

  std::vector<float> resampled(outSamples);

  for (int i = 0; i < outSamples; ++i)
  {
    double srcPos = i / ratio;
    int srcIdx = static_cast<int>(srcPos);
    double frac = srcPos - srcIdx;

    if (srcIdx + 1 < numSamples)
    {
      resampled[i] = static_cast<float>(audio[srcIdx] * (1.0 - frac) +
                                        audio[srcIdx + 1] * frac);
    }
    else if (srcIdx < numSamples)
    {
      resampled[i] = audio[srcIdx];
    }
  }

  return resampled;
}

std::vector<std::vector<float>>
RMVPEPitchDetector::extractMel(const std::vector<float> &audio)
{
  constexpr int frameMultiple = 32;
  const int numBins = WINDOW_LENGTH / 2 + 1;
  const int numFrames =
      std::max(1, static_cast<int>(audio.size()) / HOP_SIZE + 1);
  const int alignedFrames =
      ((numFrames + frameMultiple - 1) / frameMultiple) * frameMultiple;

  std::vector<std::vector<float>> mel(alignedFrames,
                                      std::vector<float>(N_MELS, 0.0f));
  std::vector<float> fftBuffer(WINDOW_LENGTH * 2, 0.0f);
  std::vector<float> mag(numBins, 0.0f);
  juce::dsp::FFT fft(static_cast<int>(std::log2(WINDOW_LENGTH)));

  const int padLeft = WINDOW_LENGTH / 2;
  for (int frame = 0; frame < numFrames; ++frame)
  {
    const int start = frame * HOP_SIZE - padLeft;

    std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
    for (int i = 0; i < WINDOW_LENGTH; ++i)
    {
      const int srcIdx = start + i;
      if (srcIdx >= 0 && srcIdx < static_cast<int>(audio.size()))
        fftBuffer[i] = audio[static_cast<size_t>(srcIdx)] * hannWindow[i];
    }

    fft.performRealOnlyForwardTransform(fftBuffer.data());

    for (int k = 0; k < numBins; ++k)
    {
      const float real = fftBuffer[k * 2];
      const float imag = fftBuffer[k * 2 + 1];
      mag[k] = std::sqrt(real * real + imag * imag + 1e-9f);
    }

    for (int m = 0; m < N_MELS; ++m)
    {
      float sum = 0.0f;
      for (int k = 0; k < numBins; ++k)
        sum += mag[k] * melFilterbank[m][k];

      mel[frame][m] = std::log(std::max(sum, 1e-5f));
    }
  }

  return mel;
}

std::vector<float> RMVPEPitchDetector::decodeF0(const float *hidden,
                                                int numFrames,
                                                float threshold)
{
  // Decode hidden states to F0 values
  // This matches the Python decode function in export.py
  std::vector<float> f0(numFrames, 0.0f);

  for (int t = 0; t < numFrames; ++t)
  {
    const float *frame = hidden + t * N_CLASS;

    // Find max value and index
    int center = 0;
    float maxVal = frame[0];
    for (int i = 1; i < N_CLASS; ++i)
    {
      if (frame[i] > maxVal)
      {
        maxVal = frame[i];
        center = i;
      }
    }

    // Check threshold (unvoiced detection)
    if (maxVal < threshold)
    {
      f0[t] = 0.0f;
      continue;
    }

    // Local weighted average around center (±4 bins)
    int start = std::max(0, center - 4);
    int end = std::min(N_CLASS, center + 5);

    float weightedSum = 0.0f;
    float weightSum = 0.0f;

    for (int i = start; i < end; ++i)
    {
      // idx_cents = idx * 20 + RMVPE_CONST
      float idxCents = i * 20.0f + RMVPE_CONST;
      weightedSum += frame[i] * idxCents;
      weightSum += frame[i];
    }

    if (weightSum > 0.0f)
    {
      float cents = weightedSum / weightSum;
      // f0 = 10 * 2^(cents/1200)
      f0[t] = 10.0f * std::pow(2.0f, cents / 1200.0f);
    }
    else
    {
      f0[t] = 0.0f;
    }
  }

  return f0;
}

std::vector<float> RMVPEPitchDetector::extractF0(const float *audio,
                                                 int numSamples, int sampleRate,
                                                 float threshold)
{
#ifdef HAVE_ONNXRUNTIME
  if (!loaded)
  {
    return {};
  }

  try
  {
    // Step 1: Resample to 16kHz
    auto audio16k = resampleTo16k(audio, numSamples, sampleRate);

    // Process in chunks to avoid stack overflow for long audio
    // Max chunk: 30 seconds at 16kHz = 480000 samples
    constexpr int MAX_CHUNK_SAMPLES = 16000 * 30;
    constexpr int OVERLAP_SAMPLES = 16000; // 1 second overlap

    if (static_cast<int>(audio16k.size()) <= MAX_CHUNK_SAMPLES)
    {
      // Short audio: process directly
      return extractF0Chunk(audio16k.data(), static_cast<int>(audio16k.size()),
                            threshold);
    }

    // Long audio: process in chunks
    std::vector<float> allF0;
    int pos = 0;
    int totalSamples = static_cast<int>(audio16k.size());

    while (pos < totalSamples)
    {
      int chunkEnd = std::min(pos + MAX_CHUNK_SAMPLES, totalSamples);
      int chunkSize = chunkEnd - pos;

      auto chunkF0 =
          extractF0Chunk(audio16k.data() + pos, chunkSize, threshold);
      if (chunkF0.empty())
        return {};

      if (pos == 0)
      {
        // First chunk: use all frames
        allF0 = std::move(chunkF0);
      }
      else
      {
        // Subsequent chunks: skip overlap frames
        int overlapFrames = OVERLAP_SAMPLES / HOP_SIZE;
        if (static_cast<int>(chunkF0.size()) > overlapFrames)
        {
          allF0.insert(allF0.end(), chunkF0.begin() + overlapFrames,
                       chunkF0.end());
        }
      }

      pos += MAX_CHUNK_SAMPLES - OVERLAP_SAMPLES;
    }

    return allF0;
  }
  catch (const Ort::Exception &e)
  {
    return {};
  }
  catch (const std::exception &e)
  {
    return {};
  }
#else
  return {};
#endif
}

std::vector<float> RMVPEPitchDetector::extractF0Chunk(const float *audio16k,
                                                      int numSamples,
                                                      float threshold)
{
#ifdef HAVE_ONNXRUNTIME
  if (!onnxSession || numSamples <= 0)
    return {};

  thresholdScratch[0] = threshold;

  static const auto memoryInfo = Ort::MemoryInfo::CreateCpu(
      OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
  static const Ort::RunOptions runOptions{nullptr};

  inputTensorScratch.clear();

  std::vector<std::vector<float>> mel;
  if (usesMelInput)
  {
    mel = extractMel(std::vector<float>(audio16k, audio16k + numSamples));
    if (mel.empty())
      return {};

    const int numFrames = static_cast<int>(mel.size());
    melInputScratch.resize(static_cast<size_t>(numFrames) * N_MELS);
    if (melInputChannelsFirst)
    {
      melShapeScratch = {1, N_MELS, numFrames};
      for (int m = 0; m < N_MELS; ++m)
      {
        for (int t = 0; t < numFrames; ++t)
          melInputScratch[static_cast<size_t>(m) * numFrames + t] = mel[t][m];
      }
    }
    else
    {
      melShapeScratch = {1, numFrames, N_MELS};
      for (int t = 0; t < numFrames; ++t)
      {
        for (int m = 0; m < N_MELS; ++m)
          melInputScratch[static_cast<size_t>(t) * N_MELS + m] = mel[t][m];
      }
    }

    inputTensorScratch.emplace_back(Ort::Value::CreateTensor<float>(
        memoryInfo, melInputScratch.data(), melInputScratch.size(),
        melShapeScratch.data(), melShapeScratch.size()));
  }
  else
  {
    waveformShapeScratch[1] = static_cast<int64_t>(numSamples);
    inputTensorScratch.emplace_back(Ort::Value::CreateTensor<float>(
        memoryInfo, const_cast<float *>(audio16k), static_cast<size_t>(numSamples),
        waveformShapeScratch.data(), waveformShapeScratch.size()));
  }

  if (inputNames.size() > inputTensorScratch.size())
  {
    inputTensorScratch.emplace_back(Ort::Value::CreateTensor<float>(
        memoryInfo, thresholdScratch.data(), thresholdScratch.size(),
        thresholdShapeScratch.data(), thresholdShapeScratch.size()));
  }

  auto t0 = std::chrono::high_resolution_clock::now();
  auto outputTensors = onnxSession->Run(
      runOptions, inputNames.data(), inputTensorScratch.data(),
      inputTensorScratch.size(), outputNames.data(), outputNames.size());
  auto t1 = std::chrono::high_resolution_clock::now();
  LOG("RMVPE chunk inference: " + juce::String(std::chrono::duration<double, std::milli>(t1 - t0).count(), 1) + " ms" + " (" + juce::String(numSamples) + " samples)");

  float *f0Data = outputTensors[0].GetTensorMutableData<float>();
  auto outputInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
  auto f0Shape = outputInfo.GetShape();
  const auto outputCount = outputInfo.GetElementCount();

  if (f0Shape.size() == 2 && f0Shape[1] > 0)
  {
    int numFrames = static_cast<int>(f0Shape[1]);
    return std::vector<float>(f0Data, f0Data + numFrames);
  }

  if (f0Shape.size() == 3)
  {
    if (f0Shape[2] == N_CLASS)
    {
      int numFrames = static_cast<int>(f0Shape[1]);
      return decodeF0(f0Data, numFrames, threshold);
    }

    if (f0Shape[1] == N_CLASS)
    {
      int numFrames = static_cast<int>(f0Shape[2]);
      latentScratch.resize(static_cast<size_t>(numFrames) * N_CLASS);
      for (int t = 0; t < numFrames; ++t)
      {
        for (int c = 0; c < N_CLASS; ++c)
          latentScratch[static_cast<size_t>(t) * N_CLASS + c] =
              f0Data[static_cast<size_t>(c) * numFrames + t];
      }
      return decodeF0(latentScratch.data(), numFrames, threshold);
    }
  }

  if (outputCount > 0 && outputCount % N_CLASS == 0)
  {
    int numFrames = static_cast<int>(outputCount / N_CLASS);
    return decodeF0(f0Data, numFrames, threshold);
  }

  return {};
#else
  return {};
#endif
}

std::vector<float> RMVPEPitchDetector::extractF0WithProgress(
    const float *audio, int numSamples, int sampleRate, float threshold,
    std::function<void(double)> progressCallback)
{
#ifdef HAVE_ONNXRUNTIME
  if (!loaded)
  {
    return {};
  }

  try
  {
    if (progressCallback)
      progressCallback(0.1);

    // Step 1: Resample to 16kHz
    auto audio16k = resampleTo16k(audio, numSamples, sampleRate);

    if (progressCallback)
      progressCallback(0.25);

    // Process in chunks to avoid huge one-shot inference on long audio.
    constexpr int MAX_CHUNK_SAMPLES = 16000 * 30;
    constexpr int OVERLAP_SAMPLES = 16000; // 1 second overlap

    std::vector<float> allF0;
    int pos = 0;
    const int totalSamples = static_cast<int>(audio16k.size());

    while (pos < totalSamples)
    {
      const int chunkEnd = std::min(pos + MAX_CHUNK_SAMPLES, totalSamples);
      const int chunkSize = chunkEnd - pos;

      auto chunkF0 = extractF0Chunk(audio16k.data() + pos, chunkSize, threshold);
      if (chunkF0.empty())
        return {};

      if (pos == 0)
      {
        allF0 = std::move(chunkF0);
      }
      else
      {
        const int overlapFrames = OVERLAP_SAMPLES / HOP_SIZE;
        if (static_cast<int>(chunkF0.size()) > overlapFrames)
        {
          allF0.insert(allF0.end(), chunkF0.begin() + overlapFrames,
                       chunkF0.end());
        }
      }

      pos += MAX_CHUNK_SAMPLES - OVERLAP_SAMPLES;
      if (progressCallback)
      {
        const double processedRatio =
            totalSamples > 0
                ? static_cast<double>(std::min(pos, totalSamples)) /
                      static_cast<double>(totalSamples)
                : 1.0;
        progressCallback(0.25 + 0.70 * processedRatio);
      }
    }

    if (progressCallback)
      progressCallback(1.0);

    return allF0;
  }
  catch (const Ort::Exception &e)
  {
    return {};
  }
  catch (const std::exception &e)
  {
    return {};
  }
#else
  return {};
#endif
}

int RMVPEPitchDetector::getNumFrames(int numSamples, int sampleRate) const
{
  // Convert to 16kHz sample count
  int samples16k = static_cast<int>(
      numSamples * static_cast<double>(SAMPLE_RATE) / sampleRate);
  // n_frames = T // hop_length + 1
  return samples16k / HOP_SIZE + 1;
}

float RMVPEPitchDetector::getTimeForFrame(int frameIndex) const
{
  return static_cast<float>(frameIndex * HOP_SIZE) / SAMPLE_RATE;
}

int RMVPEPitchDetector::getHopSizeForSampleRate(int sampleRate) const
{
  return static_cast<int>(HOP_SIZE * static_cast<double>(sampleRate) /
                          SAMPLE_RATE);
}
