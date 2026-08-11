#include "FCPEPitchDetector.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>

FCPEPitchDetector::FCPEPitchDetector()
{
  initMelFilterbank();
  initHannWindow();
  initCentTable();
}

FCPEPitchDetector::~FCPEPitchDetector() = default;

#ifdef HAVE_ONNXRUNTIME
namespace {
Ort::SessionOptions createCpuFallbackSessionOptions()
{
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  sessionOptions.EnableCpuMemArena();

  const int numThreads =
      std::max(1u, std::thread::hardware_concurrency()) / 2;
  sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
  sessionOptions.EnableMemPattern();
  return sessionOptions;
}
} // namespace
#endif

void FCPEPitchDetector::initMelFilterbank()
{
  // Create mel filterbank matching librosa's implementation (Slaney
  // normalization)
  const int numBins = N_FFT / 2 + 1; // 513

  // Hz to Mel conversion (HTK formula used by librosa by default when htk=False
  // is NOT used) Actually librosa uses Slaney by default, but the mel function
  // with htk=False uses HTK For compatibility with FCPE, we use the HTK formula
  auto hzToMel = [](float hz) -> float
  {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
  };

  auto melToHz = [](float mel) -> float
  {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
  };

  float melMin = hzToMel(FMIN);
  float melMax = hzToMel(FMAX);

  // Create mel points (N_MELS + 2 for triangular filters)
  std::vector<float> melPoints(N_MELS + 2);
  for (int i = 0; i <= N_MELS + 1; ++i)
  {
    melPoints[i] = melMin + (melMax - melMin) * i / (N_MELS + 1);
  }

  // Convert to Hz
  std::vector<float> hzPoints(N_MELS + 2);
  for (int i = 0; i <= N_MELS + 1; ++i)
  {
    hzPoints[i] = melToHz(melPoints[i]);
  }

  // Create filterbank with Slaney normalization (sparse representation)
  melFilterbank.resize(N_MELS);
  for (int m = 0; m < N_MELS; ++m)
  {
    float fLow = hzPoints[m];
    float fCenter = hzPoints[m + 1];
    float fHigh = hzPoints[m + 2];

    // Slaney normalization: 2 / (fHigh - fLow)
    float enorm = 2.0f / (fHigh - fLow);

    // Find the range of bins that fall within [fLow, fHigh]
    int firstBin = numBins;
    int lastBin = -1;

    for (int k = 0; k < numBins; ++k)
    {
      float freq = static_cast<float>(k) * FCPE_SAMPLE_RATE / N_FFT;
      if (freq >= fLow && freq <= fHigh)
      {
        if (k < firstBin)
          firstBin = k;
        if (k > lastBin)
          lastBin = k;
      }
    }

    MelBand &band = melFilterbank[m];
    if (lastBin < firstBin)
    {
      band.startBin = 0;
      band.endBin = 0;
      continue;
    }

    band.startBin = firstBin;
    band.endBin = lastBin + 1; // exclusive
    band.weights.resize(band.endBin - band.startBin, 0.0f);

    for (int k = firstBin; k <= lastBin; ++k)
    {
      float freq = static_cast<float>(k) * FCPE_SAMPLE_RATE / N_FFT;

      if (freq >= fLow && freq < fCenter)
      {
        band.weights[k - firstBin] = enorm * (freq - fLow) / (fCenter - fLow);
      }
      else if (freq >= fCenter && freq <= fHigh)
      {
        band.weights[k - firstBin] = enorm * (fHigh - freq) / (fHigh - fCenter);
      }
    }
  }
}

void FCPEPitchDetector::initHannWindow()
{
  // Create Hann window (numpy.hanning style - symmetric)
  hannWindow.resize(WIN_SIZE);
  for (int i = 0; i < WIN_SIZE; ++i)
  {
    hannWindow[i] =
        0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i /
                                (WIN_SIZE - 1)));
  }
}

void FCPEPitchDetector::initCentTable()
{
  // Create cent table matching PyTorch model
  centTable.resize(OUT_DIMS);
  float centMin = f0ToCent(F0_MIN);
  float centMax = f0ToCent(F0_MAX);

  for (int i = 0; i < OUT_DIMS; ++i)
  {
    centTable[i] = centMin + (centMax - centMin) * i / (OUT_DIMS - 1);
  }
}

bool FCPEPitchDetector::loadModel(const juce::File &modelPath,
                                  const juce::File &melFilterbankPath,
                                  const juce::File &centTablePath,
                                  GPUProvider provider, int deviceId)
{
#ifdef HAVE_ONNXRUNTIME
  lastError.clear();
  try
  {
    // Load mel filterbank from file if provided (dense binary → sparse)
    if (melFilterbankPath.existsAsFile())
    {
      juce::FileInputStream stream(melFilterbankPath);
      if (stream.openedOk())
      {
        const int numBins = N_FFT / 2 + 1;
        std::vector<float> data(N_MELS * numBins);
        stream.read(data.data(), data.size() * sizeof(float));

        melFilterbank.resize(N_MELS);
        for (int m = 0; m < N_MELS; ++m)
        {
          // Find non-zero bounds
          int firstBin = numBins;
          int lastBin = -1;
          for (int k = 0; k < numBins; ++k)
          {
            if (data[m * numBins + k] != 0.0f)
            {
              if (k < firstBin)
                firstBin = k;
              lastBin = k;
            }
          }

          MelBand &band = melFilterbank[m];
          if (lastBin < firstBin)
          {
            band.startBin = 0;
            band.endBin = 0;
            continue;
          }

          band.startBin = firstBin;
          band.endBin = lastBin + 1;
          band.weights.resize(band.endBin - band.startBin);
          for (int k = firstBin; k <= lastBin; ++k)
          {
            band.weights[k - firstBin] = data[m * numBins + k];
          }
        }
      }
    }

    // Load cent table from file if provided
    if (centTablePath.existsAsFile())
    {
      juce::FileInputStream stream(centTablePath);
      if (stream.openedOk())
      {
        centTable.resize(OUT_DIMS);
        stream.read(centTable.data(), centTable.size() * sizeof(float));
      }
    }

    // Initialize ONNX Runtime
    onnxEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                         "FCPEPitchDetector");

    Ort::SessionOptions sessionOptions;
    sessionOptions.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOptions.EnableCpuMemArena();

    if (provider == GPUProvider::CPU)
    {
      const int numThreads =
          std::max(1u, std::thread::hardware_concurrency()) / 2;
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
        LOG("FCPE DirectML provider setup failed: " + juce::String(e.what()));
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
        LOG("FCPE CUDA provider setup failed: " + juce::String(e.what()));
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
        LOG("FCPE CoreML provider setup failed: " + juce::String(e.what()));
      }
    }
    else
    {
      // CPU fallback - do nothing, CPU is default
      if (provider != GPUProvider::CPU)
      {
      }
    }

#ifdef _WIN32
    std::wstring modelPathW = modelPath.getFullPathName().toWideCharPointer();
    try
    {
      onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                                   sessionOptions);
    }
    catch (const Ort::Exception &e)
    {
      if (provider == GPUProvider::CPU)
        throw;

      LOG("FCPE session creation failed with requested provider: " +
          juce::String(e.what()) + "; retrying with CPU");
      auto cpuOptions = createCpuFallbackSessionOptions();
      onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                                   cpuOptions);
    }
#else
    std::string modelPathStr = modelPath.getFullPathName().toStdString();
    try
    {
      onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathStr.c_str(),
                                                   sessionOptions);
    }
    catch (const Ort::Exception &e)
    {
      if (provider == GPUProvider::CPU)
        throw;

      LOG("FCPE session creation failed with requested provider: " +
          juce::String(e.what()) + "; retrying with CPU");
      auto cpuOptions = createCpuFallbackSessionOptions();
      onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathStr.c_str(),
                                                   cpuOptions);
    }
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

    inputTensorScratch.reserve(1);

    loaded = true;
    return true;
  }
  catch (const Ort::Exception &e)
  {
    lastError = juce::String(e.what());
    LOG("FCPE model load failed for " + modelPath.getFullPathName() + ": " +
        lastError);
    loaded = false;
    return false;
  }
  catch (const std::exception &e)
  {
    lastError = juce::String(e.what());
    LOG("FCPE model load failed for " + modelPath.getFullPathName() + ": " +
        lastError);
    loaded = false;
    return false;
  }
#else
  lastError = "ONNX Runtime is not available";
  return false;
#endif
}

std::vector<float> FCPEPitchDetector::resampleTo16k(const float *audio,
                                                    int numSamples,
                                                    int srcRate)
{
  if (srcRate == FCPE_SAMPLE_RATE)
  {
    return std::vector<float>(audio, audio + numSamples);
  }

  // Simple linear interpolation resampling
  // For production, consider using a proper resampler (e.g., libsamplerate)
  double ratio = static_cast<double>(FCPE_SAMPLE_RATE) / srcRate;
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
FCPEPitchDetector::extractMel(const std::vector<float> &audio)
{
  const int numBins = N_FFT / 2 + 1;

  // Pad audio (same as PyTorch FCPE)
  int padLeft = (WIN_SIZE - HOP_SIZE) / 2;
  int padRight = std::max((WIN_SIZE - HOP_SIZE + 1) / 2,
                          WIN_SIZE - static_cast<int>(audio.size()) - padLeft);

  std::vector<float> paddedAudio;

  // Reflect padding if possible, otherwise zero padding
  if (padRight < static_cast<int>(audio.size()))
  {
    // Reflect padding
    paddedAudio.reserve(padLeft + audio.size() + padRight);

    // Left reflection
    for (int i = padLeft; i > 0; --i)
    {
      paddedAudio.push_back(
          audio[std::min(i, static_cast<int>(audio.size()) - 1)]);
    }

    // Original audio
    paddedAudio.insert(paddedAudio.end(), audio.begin(), audio.end());

    // Right reflection
    int audioSize = static_cast<int>(audio.size());
    for (int i = 0; i < padRight; ++i)
    {
      int idx = audioSize - 2 - i;
      if (idx < 0)
        idx = 0;
      paddedAudio.push_back(audio[idx]);
    }
  }
  else
  {
    // Zero padding
    paddedAudio.resize(padLeft + audio.size() + padRight, 0.0f);
    std::copy(audio.begin(), audio.end(), paddedAudio.begin() + padLeft);
  }

  // Calculate number of frames
  int numFrames =
      1 + (static_cast<int>(paddedAudio.size()) - WIN_SIZE) / HOP_SIZE;
  if (numFrames < 1)
    numFrames = 1;

  std::vector<std::vector<float>> mel(numFrames,
                                      std::vector<float>(N_MELS, 0.0f));

  // FFT buffer (real + imaginary interleaved for JUCE FFT)
  std::vector<float> fftBuffer(N_FFT * 2, 0.0f);
  std::vector<float> mag(numBins, 0.0f);
  juce::dsp::FFT fft(static_cast<int>(std::log2(N_FFT)));

  for (int frame = 0; frame < numFrames; ++frame)
  {
    int start = frame * HOP_SIZE;

    // Apply window and prepare FFT input
    std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
    for (int i = 0;
         i < WIN_SIZE && start + i < static_cast<int>(paddedAudio.size());
         ++i)
    {
      fftBuffer[i] = paddedAudio[start + i] * hannWindow[i];
    }

    // Perform FFT
    fft.performRealOnlyForwardTransform(fftBuffer.data());

    // Compute magnitude spectrum
    for (int k = 0; k < numBins; ++k)
    {
      float real = fftBuffer[k * 2];
      float imag = fftBuffer[k * 2 + 1];
      mag[k] = std::sqrt(real * real + imag * imag + 1e-9f);
    }

    // Apply sparse mel filterbank
    for (int m = 0; m < N_MELS; ++m)
    {
      const auto &band = melFilterbank[m];
      float sum = 0.0f;
      for (int k = band.startBin; k < band.endBin; ++k)
      {
        sum += mag[k] * band.weights[k - band.startBin];
      }

      // Dynamic range compression (log)
      mel[frame][m] = std::log(std::max(sum, CLIP_VAL));
    }
  }

  return mel;
}

std::vector<float> FCPEPitchDetector::decodeF0(const float *latent,
                                               int numFrames,
                                               float threshold)
{
  std::vector<float> f0(numFrames, 0.0f);

  for (int t = 0; t < numFrames; ++t)
  {
    const float *frame = latent + static_cast<size_t>(t) * OUT_DIMS;

    // Find max index and confidence
    int maxIdx = 0;
    float maxVal = frame[0];
    for (int i = 1; i < OUT_DIMS; ++i)
    {
      if (frame[i] > maxVal)
      {
        maxVal = frame[i];
        maxIdx = i;
      }
    }

    // Check confidence threshold
    if (maxVal <= threshold)
    {
      f0[t] = 0.0f;
      continue;
    }

    // Local argmax decoder: weighted average around max index
    int localStart = std::max(0, maxIdx - 4);
    int localEnd = std::min(OUT_DIMS - 1, maxIdx + 4);

    float weightedSum = 0.0f;
    float weightSum = 0.0f;

    for (int i = localStart; i <= localEnd; ++i)
    {
      weightedSum += centTable[i] * frame[i];
      weightSum += frame[i];
    }

    if (weightSum > 1e-9f)
    {
      float cent = weightedSum / weightSum;
      f0[t] = centToF0(cent);
    }
    else
    {
      f0[t] = 0.0f;
    }
  }

  return f0;
}

std::vector<float> FCPEPitchDetector::extractF0(const float *audio,
                                                int numSamples, int sampleRate,
                                                float threshold)
{
#ifdef HAVE_ONNXRUNTIME
  lastError.clear();
  if (!loaded)
  {
    lastError = "FCPE model is not loaded";
    return {};
  }

  try
  {
    // Step 1: Resample to 16kHz
    auto audio16k = resampleTo16k(audio, numSamples, sampleRate);

    // Step 2: Extract mel spectrogram
    auto mel = extractMel(audio16k);

    if (mel.empty())
    {
      lastError = "FCPE mel extraction produced no frames";
      return {};
    }

    // Step 3: Prepare input tensor [1, T, N_MELS]
    int numFrames = static_cast<int>(mel.size());
    melInputScratch.resize(static_cast<size_t>(numFrames) * N_MELS);

    for (int t = 0; t < numFrames; ++t)
    {
      for (int m = 0; m < N_MELS; ++m)
      {
        melInputScratch[static_cast<size_t>(t) * N_MELS + m] = mel[t][m];
      }
    }

    inputShapeScratch[1] = numFrames;

    static const auto memoryInfo = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
    static const Ort::RunOptions runOptions{nullptr};

    inputTensorScratch.clear();
    inputTensorScratch.emplace_back(Ort::Value::CreateTensor<float>(
        memoryInfo, melInputScratch.data(), melInputScratch.size(),
        inputShapeScratch.data(), inputShapeScratch.size()));

    // Step 4: Run inference
    auto t0 = std::chrono::high_resolution_clock::now();
    auto outputTensors = onnxSession->Run(runOptions, inputNames.data(),
                                          inputTensorScratch.data(),
                                          inputTensorScratch.size(),
                                          outputNames.data(), 1);
    auto t1 = std::chrono::high_resolution_clock::now();
    LOG("FCPE inference: " + juce::String(std::chrono::duration<double, std::milli>(t1 - t0).count(), 1) + " ms");

    // Step 5: Get output [1, T, OUT_DIMS]
    float *outputData = outputTensors[0].GetTensorMutableData<float>();
    const size_t outputCount =
        outputTensors[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (outputCount < static_cast<size_t>(OUT_DIMS))
    {
      lastError = "FCPE returned an unexpected output tensor";
      return {};
    }
    const int outFrames = static_cast<int>(outputCount / OUT_DIMS);

    // Step 6: Decode to F0
    return decodeF0(outputData, outFrames, threshold);
  }
  catch (const Ort::Exception &e)
  {
    lastError = juce::String(e.what());
    LOG("FCPE inference failed: " + lastError);
    return {};
  }
  catch (const std::exception &e)
  {
    lastError = juce::String(e.what());
    LOG("FCPE inference failed: " + lastError);
    return {};
  }
#else
  lastError = "ONNX Runtime is not available";
  return {};
#endif
}

std::vector<float> FCPEPitchDetector::extractF0WithProgress(
    const float *audio, int numSamples, int sampleRate, float threshold,
    std::function<void(double)> progressCallback)
{
#ifdef HAVE_ONNXRUNTIME
  lastError.clear();
  if (!loaded)
  {
    lastError = "FCPE model is not loaded";
    return {};
  }

  try
  {
    if (progressCallback)
      progressCallback(0.1);

    // Step 1: Resample to 16kHz
    auto audio16k = resampleTo16k(audio, numSamples, sampleRate);

    if (progressCallback)
      progressCallback(0.3);

    // Step 2: Extract mel spectrogram
    auto mel = extractMel(audio16k);

    if (mel.empty())
    {
      lastError = "FCPE mel extraction produced no frames";
      return {};
    }

    if (progressCallback)
      progressCallback(0.5);

    // Step 3: Prepare input tensor [1, T, N_MELS]
    int numFrames = static_cast<int>(mel.size());
    melInputScratch.resize(static_cast<size_t>(numFrames) * N_MELS);

    for (int t = 0; t < numFrames; ++t)
    {
      for (int m = 0; m < N_MELS; ++m)
      {
        melInputScratch[static_cast<size_t>(t) * N_MELS + m] = mel[t][m];
      }
    }

    inputShapeScratch[1] = numFrames;

    static const auto memoryInfo = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
    static const Ort::RunOptions runOptions{nullptr};

    inputTensorScratch.clear();
    inputTensorScratch.emplace_back(Ort::Value::CreateTensor<float>(
        memoryInfo, melInputScratch.data(), melInputScratch.size(),
        inputShapeScratch.data(), inputShapeScratch.size()));

    if (progressCallback)
      progressCallback(0.6);

    // Step 4: Run inference
    auto t0 = std::chrono::high_resolution_clock::now();
    auto outputTensors = onnxSession->Run(runOptions, inputNames.data(),
                                          inputTensorScratch.data(),
                                          inputTensorScratch.size(),
                                          outputNames.data(), 1);
    auto t1 = std::chrono::high_resolution_clock::now();
    LOG("FCPE inference (progress): " + juce::String(std::chrono::duration<double, std::milli>(t1 - t0).count(), 1) + " ms");

    if (progressCallback)
      progressCallback(0.8);

    // Step 5: Get output [1, T, OUT_DIMS]
    float *outputData = outputTensors[0].GetTensorMutableData<float>();
    const size_t outputCount =
        outputTensors[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (outputCount < static_cast<size_t>(OUT_DIMS))
    {
      lastError = "FCPE returned an unexpected output tensor";
      return {};
    }
    const int outFrames = static_cast<int>(outputCount / OUT_DIMS);

    if (progressCallback)
      progressCallback(0.9);

    // Step 6: Decode to F0
    auto result = decodeF0(outputData, outFrames, threshold);

    if (progressCallback)
      progressCallback(1.0);

    return result;
  }
  catch (const Ort::Exception &e)
  {
    lastError = juce::String(e.what());
    LOG("FCPE inference failed: " + lastError);
    return {};
  }
  catch (const std::exception &e)
  {
    lastError = juce::String(e.what());
    LOG("FCPE inference failed: " + lastError);
    return {};
  }
#else
  lastError = "ONNX Runtime is not available";
  return {};
#endif
}

int FCPEPitchDetector::getNumFrames(int numSamples, int sampleRate) const
{
  // Convert to 16kHz sample count
  int samples16k = static_cast<int>(
      numSamples * static_cast<double>(FCPE_SAMPLE_RATE) / sampleRate);

  // Calculate padding
  int padLeft = (WIN_SIZE - HOP_SIZE) / 2;
  int padRight =
      std::max((WIN_SIZE - HOP_SIZE + 1) / 2, WIN_SIZE - samples16k - padLeft);

  int paddedLen = samples16k + padLeft + padRight;
  return 1 + (paddedLen - WIN_SIZE) / HOP_SIZE;
}

float FCPEPitchDetector::getTimeForFrame(int frameIndex) const
{
  return static_cast<float>(frameIndex * HOP_SIZE) / FCPE_SAMPLE_RATE;
}

int FCPEPitchDetector::getHopSizeForSampleRate(int sampleRate) const
{
  return static_cast<int>(HOP_SIZE * static_cast<double>(sampleRate) /
                          FCPE_SAMPLE_RATE);
}
