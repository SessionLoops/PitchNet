#include "EditorController.h"
#include "../Utils/SHA256Utils.h"
#include "../Utils/Constants.h"
#include "../Utils/F0Smoother.h"
#include "../Utils/Localization.h"
#include "../Utils/MelSpectrogram.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/PlatformPaths.h"

#include <algorithm>
#include <climits>
#include <cmath>

EditorController::EditorController(bool enableAudioDevice)
{
  project = std::make_unique<Project>();
  if (enableAudioDevice)
    audioEngine = std::make_unique<AudioEngine>();

  fcpePitchDetector = std::make_unique<FCPEPitchDetector>();
  rmvpePitchDetector = std::make_unique<RMVPEPitchDetector>();
  gameDetector = std::make_unique<GAMEDetector>();
  vocoder = std::make_unique<Vocoder>();
  audioAnalyzer = std::make_unique<AudioAnalyzer>();
  incrementalSynth = std::make_unique<IncrementalSynthesizer>();
  playbackController = std::make_unique<PlaybackController>();

  fcpeModelPath = PlatformPaths::getModelFile("fcpe.onnx");
  melFilterbankPath = PlatformPaths::getModelFile("mel_filterbank.bin");
  centTablePath = PlatformPaths::getModelFile("cent_table.bin");
  rmvpeModelPath = PlatformPaths::getModelFile("rmvpe.onnx");
  gameModelDir = PlatformPaths::getModelSubDir("GAME", "encoder.onnx");

  audioAnalyzer->setFCPEDetector(fcpePitchDetector.get());
  audioAnalyzer->setRMVPEDetector(rmvpePitchDetector.get());
  audioAnalyzer->setGAMEDetector(gameDetector.get());
  audioAnalyzer->setPitchDetectorType(pitchDetectorType);

  incrementalSynth->setVocoder(vocoder.get());
  if (audioEngine)
    playbackController->setAudioEngine(audioEngine.get());
}

EditorController::~EditorController()
{
  if (modelReloadThread.joinable())
    modelReloadThread.join();
  cancelLoadingFlag = true;
  if (loaderThread.joinable())
    loaderThread.join();
  if (loaderJoinerThread.joinable())
    loaderJoinerThread.join();
  cancelRenderFlag = true;
  if (renderThread.joinable())
    renderThread.join();
}

void EditorController::setProject(std::unique_ptr<Project> newProject)
{
  project = std::move(newProject);
}

GPUProvider EditorController::getProviderFromDevice(
    const juce::String &deviceName) const
{
  if (deviceName == "CUDA")
    return GPUProvider::CUDA;
  if (deviceName == "DirectML")
    return GPUProvider::DirectML;
  if (deviceName == "CoreML")
    return GPUProvider::CoreML;
  if (deviceName.isNotEmpty() && deviceName != "CPU")
    LOG("Unsupported pitch detector device: " + deviceName + ", using CPU");
  return GPUProvider::CPU;
}

void EditorController::reloadInferenceModels(bool async)
{
  auto provider = getProviderFromDevice(device);
  int resolvedDeviceId = deviceId < 0 ? 0 : deviceId;

  auto fcpePath = fcpeModelPath;
  auto melPath = melFilterbankPath;
  auto centPath = centTablePath;
  auto rmvpePath = rmvpeModelPath;
  auto gamePath = gameModelDir;
  auto reloadTask = [device = device,
                     provider,
                     resolvedDeviceId,
                     fcpePath,
                     melPath,
                     centPath,
                     rmvpePath,
                     gamePath](EditorController *self)
  {
    if (!self)
      return;

    // Load all models in parallel — each operates on an independent object
    // with its own Ort::Env, so no shared state.
    std::thread fcpeThread;
    std::thread rmvpeThread;
    std::thread gameThread;

    if (self->fcpePitchDetector && fcpePath.existsAsFile())
    {
      fcpeThread = std::thread([&]()
                               {
        LOG("EditorController: loading FCPE model (device " + device +
            ", id " + juce::String(resolvedDeviceId) + ")...");
        if (self->fcpePitchDetector->loadModel(fcpePath, melPath, centPath,
                                               provider, resolvedDeviceId)) {
          LOG("FCPE pitch detector loaded successfully");
        } else {
          LOG("Failed to load FCPE model");
        } });
    }
    else if (self->fcpePitchDetector)
    {
      LOG("FCPE model not found at: " + fcpePath.getFullPathName());
    }

    if (self->rmvpePitchDetector && rmvpePath.existsAsFile())
    {
      rmvpeThread = std::thread([&]()
                                {
        LOG("EditorController: loading RMVPE model (device " + device +
            ", id " + juce::String(resolvedDeviceId) + ")...");
        if (self->rmvpePitchDetector->loadModel(rmvpePath, provider,
                                                resolvedDeviceId)) {
          LOG("RMVPE pitch detector loaded successfully");
        } else {
          LOG("Failed to load RMVPE model");
        } });
    }
    else if (self->rmvpePitchDetector)
    {
      LOG("RMVPE model not found at: " + rmvpePath.getFullPathName());
    }

    if (self->gameDetector && gamePath.isDirectory())
    {
      gameThread = std::thread([&]()
                               {
        LOG("EditorController: loading GAME models from " + gamePath.getFullPathName() +
            " (device " + device + ", id " + juce::String(resolvedDeviceId) + ")...");
        if (self->gameDetector->loadModels(gamePath, provider,
                                           resolvedDeviceId)) {
          LOG("GAME detector loaded successfully, isLoaded=" +
              juce::String(self->gameDetector->isLoaded() ? "true" : "false"));
        } else {
          LOG("Failed to load GAME models from " + gamePath.getFullPathName());
        } });
    }
    else if (self->gameDetector)
    {
      LOG("GAME model directory not found: " + gamePath.getFullPathName() +
          " isDirectory=" + juce::String(gamePath.isDirectory() ? "true" : "false"));
    }
    else
    {
      LOG("GAME detector not created (gameDetector is null)");
    }

    // Wait for all parallel loads to complete
    if (fcpeThread.joinable())
      fcpeThread.join();
    if (rmvpeThread.joinable())
      rmvpeThread.join();
    if (gameThread.joinable())
      gameThread.join();
  };

  if (!async)
  {
    reloadTask(this);
    return;
  }

  if (isReloadingModels.exchange(true))
    return;

  if (modelReloadThread.joinable())
    modelReloadThread.join();

  modelReloadThread = std::thread([this, reloadTask]() mutable
                                  {
    reloadTask(this);
    isReloadingModels = false; });
}

bool EditorController::isInferenceBusy() const
{
  if (audioAnalyzer && audioAnalyzer->isAnalyzing())
    return true;
  if (incrementalSynth && incrementalSynth->isSynthesizing())
    return true;
  if (isReloadingModels.load())
    return true;
  return false;
}

void EditorController::requestCancelLoading()
{
  cancelLoadingFlag = true;
}

void EditorController::loadAudioFileAsync(
    const juce::File &file,
    const ProgressCallback &onProgress,
    const LoadCompleteCallback &onComplete,
    const CancelCallback &onCancelled)
{
  if (isLoadingAudio.load())
    return;

  cancelLoadingFlag = false;
  isLoadingAudio = true;

  if (loaderThread.joinable())
    loaderThread.join();

  loaderThread = std::thread([this, file, onProgress, onComplete, onCancelled]()
                             {
    auto updateProgress = [&](double p, const juce::String &msg) {
      if (onProgress)
        onProgress(p, msg);
    };

    updateProgress(0.05, TR("progress.loading_audio"));

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(file));
    if (reader == nullptr || cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const int srcSampleRate = static_cast<int>(reader->sampleRate);

    juce::AudioBuffer<float> buffer(1, numSamples);

    updateProgress(0.10, "Reading audio...");
    if (reader->numChannels == 1) {
      reader->read(&buffer, 0, numSamples, 0, true, false);
    } else {
      juce::AudioBuffer<float> stereoBuffer(2, numSamples);
      reader->read(&stereoBuffer, 0, numSamples, 0, true, true);

      const float *left = stereoBuffer.getReadPointer(0);
      const float *right = stereoBuffer.getReadPointer(1);
      float *mono = buffer.getWritePointer(0);

      for (int i = 0; i < numSamples; ++i)
        mono[i] = (left[i] + right[i]) * 0.5f;
    }

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    if (srcSampleRate != SAMPLE_RATE) {
      updateProgress(0.18, "Resampling...");
      const double ratio = static_cast<double>(srcSampleRate) / SAMPLE_RATE;
      const int newNumSamples = static_cast<int>(numSamples / ratio);

      juce::AudioBuffer<float> resampledBuffer(1, newNumSamples);
      const float *src = buffer.getReadPointer(0);
      float *dst = resampledBuffer.getWritePointer(0);

      for (int i = 0; i < newNumSamples; ++i) {
        const double srcPos = i * ratio;
        const int srcIndex = static_cast<int>(srcPos);
        const double frac = srcPos - srcIndex;

        if (srcIndex + 1 < numSamples)
          dst[i] = static_cast<float>(src[srcIndex] * (1.0 - frac) +
                                      src[srcIndex + 1] * frac);
        else
          dst[i] = src[srcIndex];
      }

      buffer = std::move(resampledBuffer);
    }

    updateProgress(0.22, "Preparing project...");
    auto newProject = std::make_unique<Project>();
    newProject->setFilePath(file);
    newProject->setAudioSha256(SHA256Utils::fileSHA256(file));
    auto &audioData = newProject->getAudioData();
    audioData.waveform = std::move(buffer);
    audioData.sampleRate = SAMPLE_RATE;

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    updateProgress(0.25, TR("progress.analyzing_audio"));
    analyzeAudio(*newProject, updateProgress);

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    updateProgress(0.95, "Finalizing...");

    // Store pristine original waveform in AudioData for blend-based synthesis
    audioData.originalWaveform.makeCopyOf(audioData.waveform);

    juce::AudioBuffer<float> originalWaveform;
    originalWaveform.makeCopyOf(audioData.waveform);

    juce::MessageManager::callAsync(
        [this, project = std::move(newProject),
         original = std::move(originalWaveform), onComplete]() mutable {
          setProject(std::move(project));
          isLoadingAudio = false;
          if (onComplete)
            onComplete(original);
        }); });
}

void EditorController::setHostAudioAsync(
    const juce::AudioBuffer<float> &buffer,
    double sampleRate,
    const ProgressCallback &onProgress,
    const LoadCompleteCallback &onComplete)
{
  isLoadingAudio = true;
  cancelLoadingFlag.store(true);
  if (loaderThread.joinable())
  {
    if (loaderJoinerThread.joinable())
      loaderJoinerThread.join();
    auto old = std::move(loaderThread);
    loaderJoinerThread = std::thread([t = std::move(old)]() mutable
                                     {
      if (t.joinable())
        t.join(); });
  }
  cancelLoadingFlag.store(false);

  const auto jobId = hostAnalysisJobId.fetch_add(1) + 1;

  loaderThread = std::thread([this, buffer, sampleRate, onProgress, onComplete, jobId]() mutable
                             {
    if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
    {
      isLoadingAudio = false;
      return;
    }

    juce::AudioBuffer<float> resampledBuffer;
    const double inputSampleRate = sampleRate;
    if (inputSampleRate > 0.0 &&
        std::abs(inputSampleRate - static_cast<double>(SAMPLE_RATE)) > 1e-6) {
      const int inSamples = buffer.getNumSamples();
      const int outSamples = static_cast<int>(
          std::llround(static_cast<double>(inSamples) *
                       (static_cast<double>(SAMPLE_RATE) / inputSampleRate)));
      const int channels = buffer.getNumChannels();
      resampledBuffer.setSize(channels, std::max(0, outSamples), false, false,
                              true);
      resampledBuffer.clear();

      const double ratio = inputSampleRate / static_cast<double>(SAMPLE_RATE);
      for (int ch = 0; ch < channels; ++ch) {
        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process(ratio, buffer.getReadPointer(ch),
                       resampledBuffer.getWritePointer(ch),
                       resampledBuffer.getNumSamples());
      }
    }

    const juce::AudioBuffer<float> &stored =
        resampledBuffer.getNumSamples() > 0 ? resampledBuffer : buffer;
    const double storedSampleRate = resampledBuffer.getNumSamples() > 0
                                        ? static_cast<double>(SAMPLE_RATE)
                                        : inputSampleRate;

    auto projectCopy = std::make_unique<Project>();
    projectCopy->getAudioData().waveform = stored;
    projectCopy->getAudioData().sampleRate = static_cast<int>(storedSampleRate);

    auto updateProgress = [&](double p, const juce::String &msg) {
      if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
        return;
      if (onProgress)
        onProgress(p, msg);
    };

    analyzeAudio(*projectCopy, updateProgress);

    if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
    {
      isLoadingAudio = false;
      return;
    }

    // Store pristine original waveform in AudioData for blend-based synthesis
    projectCopy->getAudioData().originalWaveform.makeCopyOf(projectCopy->getAudioData().waveform);

    juce::AudioBuffer<float> originalWaveform;
    originalWaveform.makeCopyOf(projectCopy->getAudioData().waveform);

    juce::MessageManager::callAsync(
        [this, project = std::move(projectCopy),
         original = std::move(originalWaveform), onComplete, jobId]() mutable {
          if (hostAnalysisJobId.load() != jobId)
            return;
          setProject(std::move(project));
          isLoadingAudio = false;
          if (onComplete)
            onComplete(original);
        }); });
}

void EditorController::requestCancelRender()
{
  cancelRenderFlag = true;
}

void EditorController::renderProcessedAudioAsync(
    const Project &project,
    float globalPitchOffset,
    const std::function<void(bool)> &onComplete)
{
  cancelRenderFlag = true;
  if (renderThread.joinable())
    renderThread.join();
  isRenderingFlag = false;
  cancelRenderFlag = false;

  auto f0Snapshot = project.getAudioData().f0;
  auto voicedMaskSnapshot = project.getAudioData().voicedMask;
  auto melSpecSnapshot = project.getAudioData().melSpectrogram;
  Vocoder *voc = vocoder.get();

  renderThread = std::thread(
      [this, f0Snapshot = std::move(f0Snapshot),
       voicedMaskSnapshot = std::move(voicedMaskSnapshot),
       melSpecSnapshot = std::move(melSpecSnapshot), globalPitchOffset, voc,
       onComplete]() mutable
      {
        isRenderingFlag = true;

        auto finishRendering = [this]()
        { isRenderingFlag = false; };

        if (cancelRenderFlag.load())
          return finishRendering();

        if (f0Snapshot.empty() || melSpecSnapshot.empty())
        {
          if (onComplete)
            juce::MessageManager::callAsync([onComplete]()
                                            { onComplete(false); });
          return finishRendering();
        }

        if (voicedMaskSnapshot.size() < f0Snapshot.size())
          voicedMaskSnapshot.resize(f0Snapshot.size(), true);

        for (size_t i = 0; i < f0Snapshot.size(); ++i)
        {
          if (cancelRenderFlag.load())
            return finishRendering();
          if (voicedMaskSnapshot[i] && f0Snapshot[i] > 0)
            f0Snapshot[i] *= std::pow(2.0f, globalPitchOffset / 12.0f);
        }

        if (cancelRenderFlag.load())
          return finishRendering();

        auto synthesized = voc ? voc->infer(melSpecSnapshot, f0Snapshot)
                               : std::vector<float>{};

        if (cancelRenderFlag.load())
          return finishRendering();

        if (onComplete)
          juce::MessageManager::callAsync(
              [onComplete, ok = !synthesized.empty()]()
              { onComplete(ok); });
        finishRendering();
      });
}

void EditorController::resynthesizeIncrementalAsync(
    Project &project,
    const std::function<void(const juce::String &)> &onProgress,
    const std::function<void(bool)> &onComplete,
    std::atomic<bool> &pendingRerun,
    bool isPluginMode)
{
  auto *synth = incrementalSynth.get();
  if (!synth || !vocoder)
  {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (synth->isSynthesizing())
  {
    pendingRerun.store(true);
    synth->cancel();
    return;
  }

  auto &audioData = project.getAudioData();
  if (audioData.melSpectrogram.empty() || audioData.f0.empty())
  {
    if (onComplete)
      onComplete(false);
    return;
  }
  if (!vocoder->isLoaded())
  {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!project.hasDirtyNotes() && !project.hasF0DirtyRange())
  {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto [dirtyStart, dirtyEnd] = project.getDirtyFrameRange();
  if (dirtyStart < 0 || dirtyEnd < 0)
  {
    if (onComplete)
      onComplete(false);
    return;
  }

  synth->setProject(&project);
  synth->setVocoder(vocoder.get());
  pendingRerun.store(false);

  if (onProgress)
    onProgress(TR("progress.synthesizing"));

  AudioEngine *audioEnginePtr = nullptr;
  if (!isPluginMode && audioEngine)
    audioEnginePtr = audioEngine.get();

  synth->synthesizeRegion(
      [onProgress](const juce::String &message)
      {
        if (onProgress)
          onProgress(message);
      },
      [this, projectPtr = &project, pending = &pendingRerun, onComplete,
       audioEnginePtr, isPluginMode](bool success)
      {
        if (!success)
        {
          if (pending->exchange(false))
          {
            juce::MessageManager::callAsync([this, projectPtr, pending, onComplete,
                                             audioEnginePtr, isPluginMode]()
                                            { resynthesizeIncrementalAsync(*projectPtr, nullptr, onComplete,
                                                                           *pending, isPluginMode); });
          }
          else if (onComplete)
          {
            onComplete(false);
          }
          return;
        }

        if (audioEnginePtr && !isPluginMode)
        {
          auto &audioData = projectPtr->getAudioData();
          try
          {
            audioEnginePtr->loadWaveform(audioData.waveform,
                                         audioData.sampleRate, true);
          }
          catch (...)
          {
          }
        }

        if (onComplete)
          onComplete(true);

        if (pending->exchange(false))
        {
          juce::MessageManager::callAsync([this, projectPtr, pending, onComplete,
                                           audioEnginePtr, isPluginMode]()
                                          { resynthesizeIncrementalAsync(*projectPtr, nullptr, onComplete,
                                                                         *pending, isPluginMode); });
        }
      });
}

void EditorController::analyzeAudio(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress,
    std::function<void()> onComplete)
{
  auto &audioData = targetProject.getAudioData();
  if (audioData.waveform.getNumSamples() == 0)
    return;

  auto showMissingModelAndAbort = [](const juce::String &modelName,
                                     const juce::File &path)
  {
    juce::MessageManager::callAsync([modelName, path]()
                                    { juce::AlertWindow::showMessageBoxAsync(
                                          juce::AlertWindow::WarningIcon, "Missing model file",
                                          modelName + " was not found at:\n" + path.getFullPathName() +
                                              "\n\nPlease install the required model files and try again."); });
  };
  auto showModelLoadFailedAndAbort = [](const juce::String &modelName,
                                        const juce::File &path)
  {
    juce::MessageManager::callAsync([modelName, path]()
                                    { juce::AlertWindow::showMessageBoxAsync(
                                          juce::AlertWindow::WarningIcon, "Model load failed",
                                          modelName + " exists but failed to load:\n" + path.getFullPathName() +
                                              "\n\nPlease check inference device settings (CPU/CUDA/DirectML) "
                                              "or model compatibility."); });
  };

  // Extract F0
  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();

  onProgress(0.35, "Computing mel spectrogram...");
  MelSpectrogram melComputer(audioData.sampleRate, N_FFT, HOP_SIZE, NUM_MELS,
                             FMIN, FMAX);
  audioData.melSpectrogram = melComputer.compute(samples, numSamples);

  int targetFrames = static_cast<int>(audioData.melSpectrogram.size());

  onProgress(0.55, "Extracting pitch (F0)...");

  if (pitchDetectorType == PitchDetectorType::RMVPE)
  {
    if (!rmvpeModelPath.existsAsFile())
    {
      showMissingModelAndAbort("rmvpe.onnx", rmvpeModelPath);
      return;
    }
    if (!rmvpePitchDetector || !rmvpePitchDetector->isLoaded())
    {
      showModelLoadFailedAndAbort("rmvpe.onnx", rmvpeModelPath);
      return;
    }
  }
  else if (pitchDetectorType == PitchDetectorType::FCPE)
  {
    if (!fcpeModelPath.existsAsFile())
    {
      showMissingModelAndAbort("fcpe.onnx", fcpeModelPath);
      return;
    }
    if (!fcpePitchDetector || !fcpePitchDetector->isLoaded())
    {
      showModelLoadFailedAndAbort("fcpe.onnx", fcpeModelPath);
      return;
    }
    if (!melFilterbankPath.existsAsFile())
    {
      showMissingModelAndAbort("mel_filterbank.bin", melFilterbankPath);
      return;
    }
    if (!centTablePath.existsAsFile())
    {
      showMissingModelAndAbort("cent_table.bin", centTablePath);
      return;
    }
  }

  LOG("========== PITCH DETECTOR SELECTION ==========");
  LOG("Selected detector: " +
      juce::String(pitchDetectorTypeToString(pitchDetectorType)));
  LOG("RMVPE loaded: " +
      juce::String(
          rmvpePitchDetector && rmvpePitchDetector->isLoaded() ? "YES" : "NO"));
  LOG("FCPE loaded: " +
      juce::String(fcpePitchDetector && fcpePitchDetector->isLoaded() ? "YES"
                                                                      : "NO"));

  std::vector<float> extractedF0;
  if (pitchDetectorType == PitchDetectorType::RMVPE)
  {
    extractedF0 = rmvpePitchDetector->extractF0(samples, numSamples,
                                                audioData.sampleRate);
  }
  else if (pitchDetectorType == PitchDetectorType::FCPE)
  {
    extractedF0 =
        fcpePitchDetector->extractF0(samples, numSamples, audioData.sampleRate);
  }

  if (extractedF0.empty() || targetFrames <= 0)
  {
    juce::MessageManager::callAsync([]()
                                    { juce::AlertWindow::showMessageBoxAsync(
                                          juce::AlertWindow::WarningIcon, "Inference failed",
                                          "Failed to extract pitch (F0). Please check your model installation "
                                          "and settings."); });
    return;
  }

  {
    audioData.f0.resize(targetFrames);

    const double neuralFrameTime = 160.0 / 16000.0;
    const double vocoderFrameTime =
        static_cast<double>(HOP_SIZE) /
        static_cast<double>(std::max(1, audioData.sampleRate));

    for (int i = 0; i < targetFrames; ++i)
    {
      double vocoderTime = i * vocoderFrameTime;
      double neuralFramePos = vocoderTime / neuralFrameTime;
      int srcIdx = static_cast<int>(neuralFramePos);
      double frac = neuralFramePos - srcIdx;

      if (srcIdx + 1 < static_cast<int>(extractedF0.size()))
      {
        float f0_a = extractedF0[srcIdx];
        float f0_b = extractedF0[srcIdx + 1];

        if (f0_a > 0.0f && f0_b > 0.0f)
        {
          float logF0_a = std::log(f0_a);
          float logF0_b = std::log(f0_b);
          float logF0_interp = logF0_a * (1.0 - frac) + logF0_b * frac;
          audioData.f0[i] = std::exp(logF0_interp);
        }
        else if (f0_a > 0.0f)
        {
          audioData.f0[i] = f0_a;
        }
        else if (f0_b > 0.0f)
        {
          audioData.f0[i] = f0_b;
        }
        else
        {
          audioData.f0[i] = 0.0f;
        }
      }
      else if (srcIdx < static_cast<int>(extractedF0.size()))
      {
        audioData.f0[i] = extractedF0[srcIdx];
      }
      else
      {
        audioData.f0[i] = extractedF0.back() > 0.0f ? extractedF0.back() : 0.0f;
      }
    }

    audioData.voicedMask.resize(audioData.f0.size());
    for (size_t i = 0; i < audioData.f0.size(); ++i)
    {
      audioData.voicedMask[i] = audioData.f0[i] > 0;
    }

    // Compute energy-based VAD mask (captures consonants)
    {
      constexpr float kVadThreshold = 0.008f;
      const float *vadSamples = audioData.waveform.getReadPointer(0);
      const int vadNumSamples = audioData.waveform.getNumSamples();
      const int vadNumFrames = static_cast<int>(audioData.f0.size());
      audioData.vadMask.resize(vadNumFrames);
      for (int vi = 0; vi < vadNumFrames; ++vi)
      {
        int ss = vi * HOP_SIZE;
        int se = std::min(ss + HOP_SIZE, vadNumSamples);
        if (ss >= vadNumSamples)
        {
          audioData.vadMask[vi] = false;
          continue;
        }
        float sumSq = 0.0f;
        for (int vj = ss; vj < se; ++vj)
          sumSq += vadSamples[vj] * vadSamples[vj];
        float rms = std::sqrt(sumSq / static_cast<float>(se - ss));
        audioData.vadMask[vi] = rms > kVadThreshold;
      }
    }

    onProgress(0.65, "Smoothing pitch curve...");
    audioData.f0 = F0Smoother::smoothF0(audioData.f0, audioData.voicedMask);
    audioData.f0 = PitchCurveProcessor::interpolateWithUvMask(
        audioData.f0, audioData.voicedMask);
  }

  onProgress(0.75, TR("progress.loading_vocoder"));
  auto modelPath = PlatformPaths::getModelFile("pc_nsf_hifigan.onnx");

  if (!modelPath.existsAsFile() && !vocoder->isLoaded())
  {
    showMissingModelAndAbort("pc_nsf_hifigan.onnx", modelPath);
    return;
  }

  if (modelPath.existsAsFile() && !vocoder->isLoaded())
  {
    if (vocoder->loadModel(modelPath))
    {
    }
    else
    {
      juce::MessageManager::callAsync([modelPath]()
                                      { juce::AlertWindow::showMessageBoxAsync(
                                            juce::AlertWindow::WarningIcon, "Inference failed",
                                            "Failed to load vocoder model at:\n" + modelPath.getFullPathName() +
                                                "\n\nPlease check your model installation and try again."); });
      return;
    }
  }

  onProgress(0.90, "Segmenting notes...");
  segmentIntoNotes(targetProject);

  PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);

  if (onComplete)
    onComplete();
}

void EditorController::analyzeAudioAsync(
    const std::function<void(Project &)> &onProjectReady,
    const std::function<void()> &onProjectChanged)
{
  if (loaderThread.joinable())
    loaderThread.join();

  loaderThread = std::thread([this, onProjectReady, onProjectChanged]()
                             {
    if (!project)
      return;

    auto projectCopy = std::make_shared<Project>(*project);

    analyzeAudio(*projectCopy, [](double, const juce::String &) {});

    juce::MessageManager::callAsync([this, projectCopy, onProjectReady,
                                     onProjectChanged]() {
      if (!project)
        return;

      project->getAudioData().melSpectrogram =
          projectCopy->getAudioData().melSpectrogram;
      project->getAudioData().f0 = projectCopy->getAudioData().f0;
      project->getAudioData().voicedMask =
          projectCopy->getAudioData().voicedMask;
      project->getAudioData().vadMask =
          projectCopy->getAudioData().vadMask;
      project->getAudioData().originalWaveform.makeCopyOf(
          projectCopy->getAudioData().originalWaveform);
      project->getAudioData().basePitch =
          projectCopy->getAudioData().basePitch;
      project->getAudioData().deltaPitch =
          projectCopy->getAudioData().deltaPitch;
      if (onProjectReady)
        onProjectReady(*project);
      if (onProjectChanged)
        onProjectChanged();
    }); });
}

void EditorController::segmentIntoNotesAsync(
    const std::function<void(Project &)> &onProjectReady,
    const std::function<void()> &onNotesChanged)
{
  if (loaderThread.joinable())
    loaderThread.join();

  loaderThread = std::thread([this, onProjectReady, onNotesChanged]()
                             {
    if (!project)
      return;

    auto projectCopy = std::make_shared<Project>(*project);
    segmentIntoNotes(*projectCopy);

    juce::MessageManager::callAsync([this, projectCopy, onProjectReady,
                                     onNotesChanged]() {
      if (!project)
        return;

      project->getNotes() = projectCopy->getNotes();

      if (onProjectReady)
        onProjectReady(*project);
      if (onNotesChanged)
        onNotesChanged();
    }); });
}

void EditorController::segmentIntoNotes(Project &targetProject,
                                        std::function<void()> onStreamingUpdate)
{
  auto &audioData = targetProject.getAudioData();
  auto &notes = targetProject.getNotes();
  notes.clear();
  audioData.segmentChunkRanges.clear();
  audioData.segmentDebugChunks.clear();

  if (audioData.f0.empty())
    return;

  if (!gameDetector || !gameDetector->isLoaded())
  {
    auto searchedPath = gameModelDir.getFullPathName();
    auto bundlePath = PlatformPaths::getModelsDirectory().getChildFile("GAME").getFullPathName();

    juce::String detail = "GAME models were not found.\n\n"
                          "Searched path: " +
                          searchedPath + "\n"
                                         "Bundle path: " +
                          bundlePath + "\n"
                                       "gameDetector: " +
                          juce::String(gameDetector ? "created" : "null") + "\n"
                                                                            "isLoaded: " +
                          juce::String(gameDetector ? (gameDetector->isLoaded() ? "true" : "false") : "N/A") + "\n"
                                                                                                               "isDirectory: " +
                          juce::String(gameModelDir.isDirectory() ? "true" : "false") + "\n\n"
                                                                                        "Required files: encoder.onnx, segmenter.onnx, estimator.onnx, bd2dur.onnx, config.json\n\n";

    // List which files exist / missing
    for (auto *name : {"encoder.onnx", "segmenter.onnx", "estimator.onnx", "bd2dur.onnx", "config.json"})
    {
      auto f = gameModelDir.getChildFile(name);
      detail += juce::String(name) + ": " + (f.existsAsFile() ? "OK" : "MISSING") + "\n";
    }

    juce::MessageManager::callAsync([detail]()
                                    { juce::AlertWindow::showMessageBoxAsync(
                                          juce::AlertWindow::WarningIcon, TR("error.game_error"), detail); });
    return;
  }

  if (gameDetector && gameDetector->isLoaded() &&
      audioData.waveform.getNumSamples() > 0)
  {

    const float *samples = audioData.waveform.getReadPointer(0);
    int numSamples = audioData.waveform.getNumSamples();
    const int f0Size = static_cast<int>(audioData.f0.size());

    auto gameNotes = gameDetector->detectNotesWithProgress(
        samples, numSamples, GAMEDetector::SAMPLE_RATE, nullptr);

    // Build debug chunks from actual GAME slicer chunk ranges
    {
      const auto &slicerChunks = gameDetector->getLastChunkRanges();
      for (int ci = 0; ci < static_cast<int>(slicerChunks.size()); ++ci)
      {
        const auto &sc = slicerChunks[ci];
        int chunkStartFrame = sc.startSample / GAMEDetector::HOP_SIZE;
        int chunkEndFrame = (sc.endSample + GAMEDetector::HOP_SIZE - 1) / GAMEDetector::HOP_SIZE;
        chunkStartFrame = std::max(0, std::min(chunkStartFrame, f0Size));
        chunkEndFrame = std::max(chunkStartFrame, std::min(chunkEndFrame, f0Size));

        audioData.segmentChunkRanges.emplace_back(chunkStartFrame, chunkEndFrame);

        AudioData::SegmentDebugChunk dbgChunk;
        dbgChunk.chunkIndex = ci;
        dbgChunk.startFrame = chunkStartFrame;
        dbgChunk.endFrame = chunkEndFrame;
        dbgChunk.shortRestThreshold = 0;

        for (const auto &gameNote : gameNotes)
        {
          if (gameNote.startFrame < chunkStartFrame || gameNote.startFrame >= chunkEndFrame)
            continue;
          AudioData::SegmentDebugEvent ev;
          ev.startFrame = gameNote.startFrame;
          ev.endFrame = gameNote.endFrame;
          ev.attachedStartFrame = gameNote.startFrame;
          ev.midiNote = gameNote.midiNote;
          ev.isRest = gameNote.isRest;
          ev.durationSeconds = static_cast<float>(gameNote.endFrame - gameNote.startFrame) *
                               GAMEDetector::HOP_SIZE / static_cast<float>(GAMEDetector::SAMPLE_RATE);
          ev.durationFrames = gameNote.endFrame - gameNote.startFrame;
          dbgChunk.events.push_back(ev);
        }
        audioData.segmentDebugChunks.push_back(std::move(dbgChunk));
      }

      // Fallback: if no chunks (shouldn't happen), use full range
      if (audioData.segmentChunkRanges.empty())
        audioData.segmentChunkRanges.emplace_back(0, f0Size);
    }

    for (const auto &gameNote : gameNotes)
    {
      if (gameNote.isRest)
        continue;

      int f0Start = gameNote.startFrame;
      int f0End = gameNote.endFrame;

      f0Start = std::max(0, std::min(f0Start, f0Size - 1));
      f0End = std::max(f0Start + 1, std::min(f0End, f0Size));

      if (f0End - f0Start < 3)
        continue;

      Note note(f0Start, f0End, gameNote.midiNote);
      std::vector<float> f0Values(audioData.f0.begin() + f0Start,
                                  audioData.f0.begin() + f0End);
      note.setF0Values(std::move(f0Values));
      notes.push_back(note);
    }

    if (onStreamingUpdate)
    {
      juce::MessageManager::callAsync(onStreamingUpdate);
    }

    // VAD + GAME rest-guided boundary refinement:
    // expand note heads/tails into energetic consonant regions so note lengths
    // better cover pre/post-consonants.
    if (!notes.empty() && !audioData.vadMask.empty())
    {
      struct RestRange
      {
        int start = 0;
        int end = 0;
      };
      std::vector<RestRange> rests;
      for (const auto &chunk : audioData.segmentDebugChunks)
      {
        for (const auto &ev : chunk.events)
        {
          if (!ev.isRest)
            continue;
          if (ev.endFrame <= ev.startFrame)
            continue;
          rests.push_back({ev.startFrame, ev.endFrame});
        }
      }

      auto vadRatioInRange = [&](int s, int e) -> float
      {
        if (e <= s || audioData.vadMask.empty())
          return 0.0f;
        s = std::max(0, s);
        e = std::min(e, static_cast<int>(audioData.vadMask.size()));
        if (e <= s)
          return 0.0f;
        int voiced = 0;
        for (int i = s; i < e; ++i)
        {
          if (audioData.vadMask[static_cast<size_t>(i)])
            ++voiced;
        }
        return static_cast<float>(voiced) / static_cast<float>(e - s);
      };

      constexpr int kMaxHeadFrames = 14;       // ~162ms
      constexpr int kMaxTailFrames = 10;       // ~116ms
      constexpr int kRestBridgeGap = 4;        // allow tiny gap
      constexpr int kMaxRestAttach = 18;       // max attached rest length
      constexpr float kVadAttachRatio = 0.30f; // energetic enough to attach

      const int f0Size = static_cast<int>(audioData.f0.size());

      for (size_t ni = 0; ni < notes.size(); ++ni)
      {
        auto &note = notes[ni];
        int start = note.getStartFrame();
        int end = note.getEndFrame();
        if (end <= start)
          continue;

        int prevEnd = 0;
        if (ni > 0)
          prevEnd = notes[ni - 1].getEndFrame();
        int nextStart = static_cast<int>(audioData.vadMask.size());
        if (ni + 1 < notes.size())
          nextStart = notes[ni + 1].getStartFrame();

        // 1) Plain VAD backward/forward expansion.
        int newStart = start;
        for (int i = start - 1; i >= std::max(prevEnd, start - kMaxHeadFrames);
             --i)
        {
          if (i >= 0 && i < static_cast<int>(audioData.vadMask.size()) &&
              audioData.vadMask[static_cast<size_t>(i)])
            newStart = i;
          else
            break;
        }

        int newEnd = end;
        for (int i = end; i < std::min(nextStart, end + kMaxTailFrames); ++i)
        {
          if (i >= 0 && i < static_cast<int>(audioData.vadMask.size()) &&
              audioData.vadMask[static_cast<size_t>(i)])
            newEnd = i + 1;
          else
            break;
        }

        // 2) Rest-guided attach (front/back) gated by VAD ratio.
        for (const auto &rr : rests)
        {
          const int restLen = rr.end - rr.start;
          if (restLen <= 0 || restLen > kMaxRestAttach)
            continue;

          // Front rest -> note head
          if (rr.end <= start && start - rr.end <= kRestBridgeGap)
          {
            const int candStart = std::max(prevEnd, rr.start);
            if (candStart < newStart &&
                vadRatioInRange(candStart, start) >= kVadAttachRatio)
            {
              newStart = std::max(candStart, start - kMaxHeadFrames);
            }
          }

          // Back rest -> note tail
          if (rr.start >= end && rr.start - end <= kRestBridgeGap)
          {
            const int candEnd = std::min(nextStart, rr.end);
            if (candEnd > newEnd &&
                vadRatioInRange(end, candEnd) >= kVadAttachRatio)
            {
              newEnd = std::min(candEnd, end + kMaxTailFrames);
            }
          }
        }

        newStart = std::max(prevEnd, newStart);
        newEnd = std::max(newStart + 1, std::min(nextStart, newEnd));
        newStart = std::max(0, std::min(newStart, f0Size - 1));
        newEnd = std::max(newStart + 1, std::min(newEnd, f0Size));

        note.setStartFrame(newStart);
        note.setEndFrame(newEnd);
        note.setSrcStartFrame(newStart);
        note.setSrcEndFrame(newEnd);
        std::vector<float> f0Values(audioData.f0.begin() + newStart,
                                    audioData.f0.begin() + newEnd);
        note.setF0Values(std::move(f0Values));
      }
    }

    juce::Thread::sleep(100);

    if (!audioData.f0.empty())
      PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);

    return;
  }

  auto finalizeNote = [&](int start, int end)
  {
    if (end - start < 5)
      return;

    float midiSum = 0.0f;
    int midiCount = 0;
    for (int j = start; j < end; ++j)
    {
      if (j < static_cast<int>(audioData.voicedMask.size()) &&
          audioData.voicedMask[j] && audioData.f0[j] > 0)
      {
        midiSum += freqToMidi(audioData.f0[j]);
        midiCount++;
      }
    }
    if (midiCount == 0)
      return;

    float midi = midiSum / midiCount;

    Note note(start, end, midi);
    std::vector<float> f0Values(audioData.f0.begin() + start,
                                audioData.f0.begin() + end);
    note.setF0Values(std::move(f0Values));
    notes.push_back(note);
  };

  constexpr float pitchSplitThreshold = 0.5f;
  constexpr int minFramesForSplit = 3;
  constexpr int maxUnvoicedGap = INT_MAX;

  bool inNote = false;
  int noteStart = 0;
  int currentMidiNote = 0;
  int pitchChangeCount = 0;
  int pitchChangeStart = 0;
  int unvoicedCount = 0;

  for (size_t i = 0; i < audioData.f0.size(); ++i)
  {
    bool voiced = i < audioData.voicedMask.size() && audioData.voicedMask[i];

    if (voiced && !inNote)
    {
      inNote = true;
      noteStart = static_cast<int>(i);
      currentMidiNote =
          static_cast<int>(std::round(freqToMidi(audioData.f0[i])));
      pitchChangeCount = 0;
      unvoicedCount = 0;
    }
    else if (voiced && inNote)
    {
      unvoicedCount = 0;

      float currentMidi = freqToMidi(audioData.f0[i]);
      int quantizedMidi = static_cast<int>(std::round(currentMidi));

      if (quantizedMidi != currentMidiNote &&
          std::abs(currentMidi - currentMidiNote) > pitchSplitThreshold)
      {
        if (pitchChangeCount == 0)
          pitchChangeStart = static_cast<int>(i);
        pitchChangeCount++;

        if (pitchChangeCount >= minFramesForSplit)
        {
          finalizeNote(noteStart, pitchChangeStart);

          noteStart = pitchChangeStart;
          currentMidiNote = quantizedMidi;
          pitchChangeCount = 0;
        }
      }
      else
      {
        pitchChangeCount = 0;
      }
    }
    else if (!voiced && inNote)
    {
      unvoicedCount++;
      if (unvoicedCount > maxUnvoicedGap)
      {
        finalizeNote(noteStart, static_cast<int>(i) - unvoicedCount);
        inNote = false;
        pitchChangeCount = 0;
        unvoicedCount = 0;
      }
    }
  }

  if (inNote)
  {
    finalizeNote(noteStart, static_cast<int>(audioData.f0.size()));
  }

  if (!audioData.f0.empty())
    PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);
}
