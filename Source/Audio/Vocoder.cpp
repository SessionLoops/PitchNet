#include "Vocoder.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Constants.h"
#include "../Utils/PlatformPaths.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace
{
  constexpr float kMelMinClamp = -15.0f;
  constexpr float kMelMaxClamp = 5.0f;
  constexpr float kF0MinValid = 20.0f;
  constexpr float kF0MaxValid = 2000.0f;

  // Maximum frames per single ONNX inference call.
  // Large tensors can cause "Error in building plan" on CoreML and OOM on other
  // providers.  512 frames ≈ 5.9 s at 44100/512 and is safe across all backends.
  constexpr size_t kMaxChunkFrames = 512;
  // Overlap between adjacent chunks for crossfade (in frames).
  constexpr size_t kOverlapFrames = 16;

  bool isVerboseInferLogEnabled()
  {
    static const bool enabled = []()
    {
      const auto value =
          juce::SystemStats::getEnvironmentVariable("PITCHNET_VOCODER_TRACE",
                                                    {})
              .trim()
              .toLowerCase();
      return value == "1" || value == "true" || value == "yes";
    }();
    return enabled;
  }
} // namespace

Vocoder::Vocoder()
{
  // Open log file in platform-appropriate logs directory
  auto logPath = PlatformPaths::getLogFile("vocoder_" +
                                           AppLogger::getSessionId() + ".txt");
  logFile = std::make_unique<std::ofstream>(
      logPath.getFullPathName().toStdString(), std::ios::app);

  if (logFile && logFile->is_open())
  {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    *logFile << "\n========== Vocoder Session Started at " << std::ctime(&time)
             << " ==========\n";
    logFile->flush();
  }

#ifdef HAVE_ONNXRUNTIME
  // Initialize ONNX Runtime environment
  try
  {
    onnxEnv =
        std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "PitchNet");
    allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();
    log("ONNX Runtime initialized successfully");
  }
  catch (const Ort::Exception &e)
  {
    log("Failed to initialize ONNX Runtime: " + std::string(e.what()));
  }
#endif

  // Start async worker thread for inferAsync()
  asyncWorker = std::thread([this]()
                            {
    for (;;) {
      AsyncTask task;
      {
        std::unique_lock<std::mutex> lock(asyncMutex);
        asyncCondition.wait(lock, [this]() {
          return isShuttingDown.load() || !asyncQueue.empty();
        });

        if (isShuttingDown.load() && asyncQueue.empty())
          return;

        task = std::move(asyncQueue.front());
        asyncQueue.pop_front();
      }

      // Skip work if shutting down
      if (isShuttingDown.load()) {
        if (activeAsyncTasks.fetch_sub(1) == 1) {
          std::lock_guard<std::mutex> lock(asyncMutex);
          asyncCondition.notify_all();
        }
        continue;
      }

      // If canceled, still invoke callback (with empty result) so callers can
      // clear state and potentially schedule a rerun.
      if (task.cancelFlag && task.cancelFlag->load()) {
        // Mark task done
        if (activeAsyncTasks.fetch_sub(1) == 1) {
          std::lock_guard<std::mutex> lock(asyncMutex);
          asyncCondition.notify_all();
        }

        auto cb = std::move(task.callback);
        juce::MessageManager::callAsync([cb]() mutable {
          if (cb)
            cb({});
        });
        continue;
      }

      auto result = infer(task.mel, task.f0);

      // Mark task done
      if (activeAsyncTasks.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lock(asyncMutex);
        asyncCondition.notify_all();
      }

      // If shutting down, skip callback
      if (isShuttingDown.load())
        continue;

      // Call callback on message thread
      auto cb = std::move(task.callback);
      juce::MessageManager::callAsync(
          [cb = std::move(cb), result = std::move(result)]() mutable {
        if (cb)
          cb(std::move(result));
      });
    } });
}

Vocoder::~Vocoder()
{
  // Signal shutdown
  isShuttingDown.store(true);

  // Wake worker and join
  {
    std::lock_guard<std::mutex> lock(asyncMutex);
    asyncCondition.notify_all();
  }
  if (asyncWorker.joinable())
    asyncWorker.join();

#ifdef HAVE_ONNXRUNTIME
  onnxSession.reset();
  onnxEnv.reset();
#endif
  if (logFile && logFile->is_open())
  {
    log("Vocoder session ended");
    logFile->close();
  }
}

void Vocoder::log(const std::string &message)
{
  if (logFile && logFile->is_open())
  {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif

    *logFile << std::put_time(&tm_buf, "%H:%M:%S") << "." << std::setfill('0')
             << std::setw(3) << ms.count() << " | " << message << "\n";
    logFile->flush();
  }
}

bool Vocoder::isOnnxRuntimeAvailable()
{
#ifdef HAVE_ONNXRUNTIME
  return true;
#else
  return false;
#endif
}

bool Vocoder::loadModel(const juce::File &modelPath)
{
#ifdef HAVE_ONNXRUNTIME
  if (!onnxEnv)
  {
    log("ONNX Runtime not initialized");
    return false;
  }

  if (!modelPath.existsAsFile())
  {
    log("Vocoder: Model file not found: " +
        modelPath.getFullPathName().toStdString());
    return false;
  }

  try
  {
    // Validate ONNX environment
    if (!onnxEnv)
    {
      log("ONNX Runtime environment is null");
      return false;
    }

    // Create session with current settings
    log("Creating session options...");
    Ort::SessionOptions sessionOptions = createSessionOptions();

    // Create session
#ifdef _WIN32
    // Safely convert path to wide string
    juce::String pathStr = modelPath.getFullPathName();
    if (pathStr.isEmpty())
    {
      log("Model path is empty");
      return false;
    }

    // Convert to wide string safely
    const wchar_t *pathWChar = pathStr.toWideCharPointer();
    if (pathWChar == nullptr)
    {
      log("Failed to convert model path to wide string");
      return false;
    }
    std::wstring modelPathW(pathWChar);

    // Validate path length (Windows MAX_PATH is 260, but extended paths can be
    // longer)
    if (modelPathW.length() == 0 || modelPathW.length() > 32767)
    {
      log("Invalid model path length: " + std::to_string(modelPathW.length()));
      return false;
    }

    log("Loading model from: " + pathStr.toStdString());
    log("Path length: " + std::to_string(modelPathW.length()) + " characters");

    // Create the session - this is where the exception might occur
    onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                                 sessionOptions);
#else
    std::string modelPathStr = modelPath.getFullPathName().toStdString();
    if (modelPathStr.empty())
    {
      log("Model path is empty");
      return false;
    }
    log("Loading model from: " + modelPathStr);
    onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathStr.c_str(),
                                                 sessionOptions);
#endif

    ioBinding.reset();
    if (executionDevice != "CPU")
    {
      try
      {
        ioBinding = std::make_unique<Ort::IoBinding>(*onnxSession);
      }
      catch (const Ort::Exception &e)
      {
        log("Failed to create IO binding (falling back to standard Run API): " +
            std::string(e.what()));
      }
    }

    // Get input names
    size_t numInputs = onnxSession->GetInputCount();
    inputNameStrings.clear();
    inputNames.clear();

    for (size_t i = 0; i < numInputs; ++i)
    {
      auto namePtr = onnxSession->GetInputNameAllocated(i, *allocator);
      inputNameStrings.push_back(namePtr.get());
    }
    for (auto &name : inputNameStrings)
    {
      inputNames.push_back(name.c_str());
    }

    // Get output names
    size_t numOutputs = onnxSession->GetOutputCount();
    outputNameStrings.clear();
    outputNames.clear();

    for (size_t i = 0; i < numOutputs; ++i)
    {
      auto namePtr = onnxSession->GetOutputNameAllocated(i, *allocator);
      outputNameStrings.push_back(namePtr.get());
    }
    for (auto &name : outputNameStrings)
    {
      outputNames.push_back(name.c_str());
    }

    // Pre-size scratch containers used by infer() hot path.
    melShapeScratch.resize(3);
    f0ShapeScratch.resize(2);
    inputTensorScratch.reserve(2);
    outputTensorScratch.clear();
    outputTensorScratch.reserve(outputNames.size());
    for (size_t i = 0; i < outputNames.size(); ++i)
    {
      outputTensorScratch.emplace_back(nullptr);
    }

    log("Vocoder: ONNX model loaded successfully");
    log("  Input names: " +
        std::string(inputNames.size() > 0 ? inputNames[0] : "none"));
    log("  Output names: " +
        std::string(outputNames.size() > 0 ? outputNames[0] : "none"));

    modelFile = modelPath;
    loaded = true;
    return true;
  }
  catch (const Ort::Exception &e)
  {
    log("Failed to load ONNX model: " + std::string(e.what()));
    loaded = false;
    return false;
  }
#else
  // Without ONNX Runtime, try to load config from same directory
  auto configPath = modelPath.getParentDirectory().getChildFile("config.json");
  if (configPath.existsAsFile())
  {
    auto configText = configPath.loadFileAsString();
    auto config = juce::JSON::parse(configText);

    if (config.isObject())
    {
      auto configObj = config.getDynamicObject();
      if (configObj)
      {
        sampleRate = configObj->getProperty("sampling_rate");
        hopSize = configObj->getProperty("hop_size");
        numMels = configObj->getProperty("num_mels");
        pitchControllable = configObj->getProperty("pc_aug");
      }
    }
  }

  log("Vocoder: ONNX Runtime not available, using sine fallback");
  loaded = true; // Allow "loaded" state for fallback
  return true;
#endif
}

std::vector<float> Vocoder::infer(const std::vector<std::vector<float>> &mel,
                                  const std::vector<float> &f0)
{
  if (!loaded || mel.empty() || f0.empty())
    return {};

  // Lock to ensure thread-safe access to ONNX session
  std::lock_guard<std::mutex> lock(inferenceMutex);

  const size_t numFrames = std::min(mel.size(), f0.size());
  if (numFrames == 0)
    return {};
  const bool verboseInferLog = isVerboseInferLogEnabled();
  const auto startTotal = std::chrono::high_resolution_clock::now();

#ifdef HAVE_ONNXRUNTIME
  if (!onnxSession || inputNames.empty() || outputNames.empty())
  {
    log("ONNX session not available, using fallback");
    return generateSineFallback(f0);
  }

  try
  {
    // -----------------------------------------------------------
    // Short input: single-pass inference (no chunking overhead)
    // -----------------------------------------------------------
    if (numFrames <= kMaxChunkFrames)
    {
      auto result = inferChunkLocked(mel, f0, numFrames);
      if (result.empty())
      {
        log("Single-chunk inference failed, using fallback");
        return generateSineFallback(f0);
      }
      if (verboseInferLog)
      {
        const auto endTotal = std::chrono::high_resolution_clock::now();
        const auto totalMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(endTotal -
                                                                  startTotal)
                .count();
        log("Vocoder infer [" + std::to_string(numFrames) +
            " frames] total=" + std::to_string(totalMs) + "ms");
      }
      return result;
    }

    // -----------------------------------------------------------
    // Long input: chunked inference with crossfade
    // -----------------------------------------------------------
    const size_t step = kMaxChunkFrames - kOverlapFrames;
    const size_t overlapSamples =
        kOverlapFrames * static_cast<size_t>(hopSize);
    const size_t totalSamples = numFrames * static_cast<size_t>(hopSize);

    size_t numChunks = 0;
    for (size_t off = 0; off < numFrames; off += step)
      ++numChunks;

    log("Chunked inference: " + std::to_string(numFrames) + " frames -> " +
        std::to_string(numChunks) + " chunks (max " +
        std::to_string(kMaxChunkFrames) + " frames, overlap " +
        std::to_string(kOverlapFrames) + ")");

    std::vector<float> waveform(totalSamples, 0.0f);
    size_t waveformWriteEnd = 0;

    for (size_t chunkIdx = 0, frameOff = 0; frameOff < numFrames;
         ++chunkIdx, frameOff += step)
    {
      const size_t chunkEnd =
          std::min(frameOff + kMaxChunkFrames, numFrames);
      const size_t chunkFrames = chunkEnd - frameOff;

      // Build sub-ranges for this chunk
      std::vector<std::vector<float>> chunkMel(
          mel.begin() + static_cast<ptrdiff_t>(frameOff),
          mel.begin() + static_cast<ptrdiff_t>(chunkEnd));
      std::vector<float> chunkF0(
          f0.begin() + static_cast<ptrdiff_t>(frameOff),
          f0.begin() + static_cast<ptrdiff_t>(chunkEnd));

      auto chunkWav = inferChunkLocked(chunkMel, chunkF0, chunkFrames);
      if (chunkWav.empty())
      {
        log("Chunk " + std::to_string(chunkIdx) + " inference failed");
        return generateSineFallback(f0);
      }

      const size_t dstOffset =
          frameOff * static_cast<size_t>(hopSize);
      const size_t chunkSamples = chunkWav.size();

      if (chunkIdx == 0)
      {
        // First chunk: straight copy
        const size_t copyLen = std::min(chunkSamples, totalSamples);
        std::copy_n(chunkWav.begin(), copyLen, waveform.begin());
        waveformWriteEnd = copyLen;
      }
      else
      {
        // Crossfade the overlap region
        const size_t prevTail =
            (waveformWriteEnd > dstOffset) ? (waveformWriteEnd - dstOffset) : 0;
        const size_t fadeSamples =
            std::min({overlapSamples, prevTail, chunkSamples});

        for (size_t i = 0; i < fadeSamples; ++i)
        {
          const float t =
              static_cast<float>(i) / static_cast<float>(fadeSamples);
          waveform[dstOffset + i] =
              waveform[dstOffset + i] * (1.0f - t) + chunkWav[i] * t;
        }

        // Copy non-overlapping tail
        const size_t dstTailStart = dstOffset + fadeSamples;
        const size_t srcTailLen =
            (chunkSamples > fadeSamples) ? (chunkSamples - fadeSamples) : 0;
        const size_t safeTailLen =
            std::min(srcTailLen, totalSamples - dstTailStart);
        if (safeTailLen > 0)
        {
          std::copy_n(chunkWav.begin() + static_cast<ptrdiff_t>(fadeSamples),
                      safeTailLen,
                      waveform.begin() + static_cast<ptrdiff_t>(dstTailStart));
        }
        waveformWriteEnd =
            std::min(dstTailStart + safeTailLen, totalSamples);
      }

      if (verboseInferLog)
      {
        log("  chunk " + std::to_string(chunkIdx) + "/" +
            std::to_string(numChunks) + " frames [" +
            std::to_string(frameOff) + ".." + std::to_string(chunkEnd) +
            ") -> " + std::to_string(chunkSamples) + " samples");
      }
    }

    waveform.resize(waveformWriteEnd);

    // Final clamp
    for (auto &s : waveform)
      s = std::clamp(s, -1.0f, 1.0f);

    if (verboseInferLog)
    {
      const auto endTotal = std::chrono::high_resolution_clock::now();
      const auto totalMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(endTotal -
                                                                startTotal)
              .count();
      log("Vocoder chunked infer [" + std::to_string(numFrames) +
          " frames, " + std::to_string(numChunks) + " chunks] total=" +
          std::to_string(totalMs) + "ms");
    }

    return waveform;
  }
  catch (const Ort::Exception &e)
  {
    log("ONNX inference failed: " + std::string(e.what()));
    return generateSineFallback(f0);
  }
#else
  return generateSineFallback(f0);
#endif
}

#ifdef HAVE_ONNXRUNTIME
std::vector<float>
Vocoder::inferChunkLocked(const std::vector<std::vector<float>> &mel,
                          const std::vector<float> &f0, size_t numFrames)
{
  // Caller must hold inferenceMutex and have verified onnxSession is valid.
  const auto startPrep = std::chrono::high_resolution_clock::now();
  const bool verboseInferLog = isVerboseInferLogEnabled();

  // Prepare mel input: [batch=1, num_mels, frames]
  melShapeScratch[0] = 1;
  melShapeScratch[1] = static_cast<int64_t>(numMels);
  melShapeScratch[2] = static_cast<int64_t>(numFrames);

  const size_t melElementCount =
      static_cast<size_t>(numMels) * numFrames;
  melScratch.resize(melElementCount);

  for (size_t frame = 0; frame < numFrames; ++frame)
  {
    const auto &sourceFrame = mel[frame];
    const int melCount =
        juce::jmin(numMels, static_cast<int>(sourceFrame.size()));
    size_t dstIndex = frame;
    int m = 0;
    for (; m < melCount; ++m, dstIndex += numFrames)
    {
      melScratch[dstIndex] = std::clamp(
          sourceFrame[static_cast<size_t>(m)], kMelMinClamp, kMelMaxClamp);
    }
    for (; m < numMels; ++m, dstIndex += numFrames)
    {
      melScratch[dstIndex] = 0.0f;
    }
  }

  // Prepare f0 input: [batch=1, frames]
  f0ShapeScratch[0] = 1;
  f0ShapeScratch[1] = static_cast<int64_t>(numFrames);

  f0Scratch.resize(numFrames);
  for (size_t i = 0; i < numFrames; ++i)
  {
    const float freq = f0[i];
    f0Scratch[i] =
        (freq > 0.0f) ? std::clamp(freq, kF0MinValid, kF0MaxValid) : 0.0f;
  }

  static const auto cpuMemoryInfo =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  inputTensorScratch.clear();
  inputTensorScratch.emplace_back(Ort::Value::CreateTensor<float>(
      cpuMemoryInfo, melScratch.data(), melScratch.size(),
      melShapeScratch.data(), melShapeScratch.size()));
  inputTensorScratch.emplace_back(Ort::Value::CreateTensor<float>(
      cpuMemoryInfo, f0Scratch.data(), f0Scratch.size(),
      f0ShapeScratch.data(), f0ShapeScratch.size()));

  const auto startInfer = std::chrono::high_resolution_clock::now();

  Ort::Value *outputTensor = nullptr;
  std::vector<Ort::Value> ioBoundOutputs;
  static const Ort::RunOptions runOptions{nullptr};

  bool ranWithIoBinding = false;
  const bool canUseIoBinding =
      ioBinding && executionDevice != "CPU" &&
      inputTensorScratch.size() == inputNames.size() &&
      outputNames.size() == 1;

  if (canUseIoBinding)
  {
    try
    {
      ioBinding->ClearBoundInputs();
      ioBinding->ClearBoundOutputs();
      for (size_t i = 0; i < inputNames.size(); ++i)
      {
        ioBinding->BindInput(inputNames[i], inputTensorScratch[i]);
      }
      ioBinding->BindOutput(outputNames.front(), cpuMemoryInfo);
      onnxSession->Run(runOptions, *ioBinding);
      ioBoundOutputs = ioBinding->GetOutputValues();
      if (!ioBoundOutputs.empty())
      {
        outputTensor = &ioBoundOutputs.front();
        ranWithIoBinding = true;
      }
    }
    catch (const Ort::Exception &e)
    {
      if (verboseInferLog)
      {
        log("IO binding run failed; fallback to standard run path: " +
            std::string(e.what()));
      }
    }
  }

  if (!ranWithIoBinding)
  {
    if (outputTensorScratch.size() != outputNames.size())
    {
      outputTensorScratch.clear();
      outputTensorScratch.reserve(outputNames.size());
      for (size_t i = 0; i < outputNames.size(); ++i)
      {
        outputTensorScratch.emplace_back(nullptr);
      }
    }
    for (auto &outputValue : outputTensorScratch)
    {
      outputValue = Ort::Value{nullptr};
    }

    onnxSession->Run(runOptions, inputNames.data(),
                     inputTensorScratch.data(),
                     inputTensorScratch.size(), outputNames.data(),
                     outputTensorScratch.data(),
                     outputTensorScratch.size());

    if (!outputTensorScratch.empty())
    {
      outputTensor = &outputTensorScratch.front();
    }
  }

  const auto endInfer = std::chrono::high_resolution_clock::now();

  if (outputTensor == nullptr || !outputTensor->HasValue())
  {
    log("ONNX inference returned no output");
    return {};
  }

  auto typeInfo = outputTensor->GetTensorTypeAndShapeInfo();
  const size_t outputSize = typeInfo.GetElementCount();

  if (verboseInferLog)
  {
    const size_t expectedSamples =
        numFrames * static_cast<size_t>(hopSize);
    if (outputSize != expectedSamples)
    {
      log("WARNING: Output length mismatch! Expected " +
          std::to_string(expectedSamples) + " samples (" +
          std::to_string(numFrames) + " frames * " +
          std::to_string(hopSize) + " hop), but got " +
          std::to_string(outputSize) + " samples");
    }
    const auto prepMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(startInfer -
                                                              startPrep)
            .count();
    const auto inferMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endInfer -
                                                              startInfer)
            .count();
    log("  chunk prep=" + std::to_string(prepMs) +
        "ms infer=" + std::to_string(inferMs) + "ms");
  }

  // Copy and clamp output
  float *outputData = outputTensor->GetTensorMutableData<float>();
  std::vector<float> waveform(outputSize);
  for (size_t i = 0; i < outputSize; ++i)
  {
    waveform[i] = std::clamp(outputData[i], -1.0f, 1.0f);
  }

  return waveform;
}
#endif

std::vector<float>
Vocoder::inferWithPitchShift(const std::vector<std::vector<float>> &mel,
                             const std::vector<float> &f0,
                             float pitchShiftSemitones)
{
  if (pitchShiftSemitones == 0.0f)
    return infer(mel, f0);

  // Shift F0
  float ratio = std::pow(2.0f, pitchShiftSemitones / 12.0f);
  std::vector<float> shiftedF0 = f0;

  for (auto &freq : shiftedF0)
  {
    if (freq > 0.0f)
      freq *= ratio;
  }

  return infer(mel, shiftedF0);
}

void Vocoder::inferAsync(std::vector<std::vector<float>> mel,
                         std::vector<float> f0,
                         std::function<void(std::vector<float>)> callback,
                         std::shared_ptr<std::atomic<bool>> cancelFlag)
{
  // Check if shutting down
  if (isShuttingDown.load())
  {
    log("inferAsync: Vocoder is shutting down, skipping request");
    return;
  }

  // Increment active task count
  activeAsyncTasks.fetch_add(1);

  {
    std::lock_guard<std::mutex> lock(asyncMutex);
    asyncQueue.emplace_back(
        AsyncTask{std::move(mel), std::move(f0), std::move(callback),
                  std::move(cancelFlag)});
    asyncCondition.notify_one();
  }
}

std::vector<float> Vocoder::generateSineFallback(const std::vector<float> &f0)
{
  // Fallback: Generate simple sine wave based on F0
  size_t numFrames = f0.size();
  size_t numSamples = numFrames * hopSize;

  std::vector<float> waveform(numSamples, 0.0f);

  float phase = 0.0f;
  for (size_t frame = 0; frame < numFrames; ++frame)
  {
    float freq = f0[frame];
    if (freq <= 0.0f)
      freq = 0.0f; // Unvoiced

    for (int s = 0; s < hopSize; ++s)
    {
      size_t sampleIdx = frame * hopSize + s;
      if (sampleIdx >= numSamples)
        break;

      if (freq > 0.0f)
      {
        waveform[sampleIdx] = 0.3f * std::sin(phase);
        phase += 2.0f * juce::MathConstants<float>::pi * freq / sampleRate;
        if (phase > 2.0f * juce::MathConstants<float>::pi)
          phase -= 2.0f * juce::MathConstants<float>::pi;
      }
    }
  }

  return waveform;
}

void Vocoder::setExecutionDevice(const juce::String &device)
{
  if (executionDevice != device)
  {
    executionDevice = device;
    log("Execution device set to: " + device.toStdString());
  }
}

void Vocoder::setExecutionDeviceId(int deviceId)
{
  if (deviceId < 0)
    deviceId = 0;
  if (executionDeviceId != deviceId)
  {
    executionDeviceId = deviceId;
    log("Execution device ID set to: " + std::to_string(deviceId));
  }
}

bool Vocoder::reloadModel()
{
  if (!modelFile.existsAsFile())
  {
    log("Cannot reload: no model file set");
    return false;
  }

  // Lock to prevent reload during inference
  std::lock_guard<std::mutex> lock(inferenceMutex);

  log("Reloading model with new settings...");

#ifdef HAVE_ONNXRUNTIME
  // Release existing session
  onnxSession.reset();
  ioBinding.reset();
  inputNames.clear();
  outputNames.clear();
  inputNameStrings.clear();
  outputNameStrings.clear();
  inputTensorScratch.clear();
  outputTensorScratch.clear();
  loaded = false;
#endif

  return loadModel(modelFile);
}

#ifdef HAVE_ONNXRUNTIME
Ort::SessionOptions Vocoder::createSessionOptions()
{
  Ort::SessionOptions sessionOptions;

  // Enable all optimizations
  sessionOptions.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);

  // Enable memory pattern optimization
  sessionOptions.EnableMemPattern();

  // Enable CPU memory arena
  sessionOptions.EnableCpuMemArena();

  // GPU-backed providers generally run best with minimal ORT CPU thread pools.
  if (executionDevice != "CPU")
  {
    sessionOptions.SetIntraOpNumThreads(1);
    sessionOptions.SetInterOpNumThreads(1);
  }
  else
  {
    const int numThreads =
        std::max(1u, std::thread::hardware_concurrency()) / 2;
    sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
  }

  log("Creating session with device: " + executionDevice.toStdString());

  // Add execution provider based on device selection
#ifdef USE_CUDA
  if (executionDevice == "CUDA")
  {
    try
    {
      OrtCUDAProviderOptions cudaOptions{};
      cudaOptions.device_id = executionDeviceId;
      sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
      log("CUDA execution provider added (device " +
          std::to_string(executionDeviceId) + ")");
    }
    catch (const Ort::Exception &e)
    {
      log("Failed to add CUDA provider: " + std::string(e.what()));
      log("Falling back to CPU");
    }
  }
  else
#endif
#ifdef USE_DIRECTML
      if (executionDevice == "DirectML")
  {
    try
    {
      const OrtApi &ortApi = Ort::GetApi();
      const OrtDmlApi *ortDmlApi = nullptr;
      Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
          "DML", ORT_API_VERSION, reinterpret_cast<const void **>(&ortDmlApi)));

      sessionOptions.DisableMemPattern();
      sessionOptions.SetExecutionMode(ORT_SEQUENTIAL);

      Ort::ThrowOnError(ortDmlApi->SessionOptionsAppendExecutionProvider_DML(
          sessionOptions, executionDeviceId));
      log("DirectML execution provider added (device " +
          std::to_string(executionDeviceId) + ")");
    }
    catch (const Ort::Exception &e)
    {
      log("Failed to add DirectML provider: " + std::string(e.what()));
      log("Falling back to CPU");
    }
  }
  else
#endif
      if (executionDevice == "CoreML")
  {
    try
    {
      sessionOptions.AppendExecutionProvider("CoreML",
                                             {{"MLComputeUnits", "ALL"}});
      log("CoreML execution provider added");
    }
    catch (const Ort::Exception &e)
    {
      log("Failed to add CoreML provider: " + std::string(e.what()));
      log("Falling back to CPU");
    }
  }
#ifdef USE_TENSORRT
  else if (executionDevice == "TensorRT")
  {
    try
    {
      OrtTensorRTProviderOptions trtOptions{};
      sessionOptions.AppendExecutionProvider_TensorRT(trtOptions);
      log("TensorRT execution provider added");
    }
    catch (const Ort::Exception &e)
    {
      log("Failed to add TensorRT provider: " + std::string(e.what()));
      log("Falling back to CPU");
    }
  }
#endif
  // CPU is the default fallback

  return sessionOptions;
}
#endif
