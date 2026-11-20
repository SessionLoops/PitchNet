#include "EditorController.h"
#include "BreathSibilanceAnalysis.h"
#include "NoteEdges.h"
#include "../Utils/NoteAmplitude.h"
#include "../Utils/AudioResampler.h"
#include "../Utils/SHA256Utils.h"
#include "../Utils/ScaleUtils.h"
#include "../Utils/Constants.h"
#include "../Utils/F0Smoother.h"
#include "../Utils/Localization.h"
#include "../Utils/MelSpectrogram.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/PlatformPaths.h"
#include "../Utils/OnnxRuntime.h"
#include "Analysis/InputTypeClassifier.h"
#include "Analysis/YamnetVoiceTagger.h"
#include "Polyphonic/BasicPitchEvidence.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

// The input-type classifier's models (and, from M2, the polyphonic engine's
// note evidence): small, CPU-only and optional. One instance serves the whole
// process - every controller, whatever its device - held weakly like the GPU
// sets so the sessions die with the last controller. Each model's mutex
// covers both loading and inference.
struct ClassifierModels
{
  std::mutex basicPitchMutex;
  std::mutex yamnetMutex;
  polyphonic::BasicPitchEvidence basicPitch;
  inputtype::YamnetVoiceTagger yamnet;
};

static std::shared_ptr<ClassifierModels> acquireClassifierModels()
{
  static std::mutex mutex;
  static std::weak_ptr<ClassifierModels> shared;
  const std::lock_guard<std::mutex> lock(mutex);
  if (auto existing = shared.lock())
    return existing;
  auto created = std::make_shared<ClassifierModels>();
  shared = created;
  return created;
}

// Analysis models for one device. GPU sets are shared by every controller in
// the process on that device; each CPU controller gets its own set so CPU
// analysis of several tracks still runs in parallel. Detectors keep per-call
// scratch state, so each model has its own mutex for inference.
struct SharedInferenceModels
{
  juce::String key;
  std::mutex loadMutex;
  std::mutex fcpeMutex;
  std::mutex rmvpeMutex;
  std::mutex swiftF0Mutex;
  std::mutex gameMutex;
  std::shared_ptr<FCPEPitchDetector> fcpe = std::make_shared<FCPEPitchDetector>();
  std::shared_ptr<RMVPEPitchDetector> rmvpe = std::make_shared<RMVPEPitchDetector>();
  // SwiftF0 always runs on the CPU (it is tiny), whatever the set's device.
  std::shared_ptr<SwiftF0PitchDetector> swiftF0 = std::make_shared<SwiftF0PitchDetector>();
  std::shared_ptr<GAMEDetector> game = std::make_shared<GAMEDetector>();
  // Process-wide, shared by every set (see ClassifierModels).
  std::shared_ptr<ClassifierModels> classifier = acquireClassifierModels();
};

namespace
{
std::filesystem::path toFsPath(const juce::File &file)
{
#if JUCE_WINDOWS
  // Wide characters, so non-ASCII paths survive.
  return std::filesystem::path(file.getFullPathName().toWideCharPointer());
#else
  return std::filesystem::path(file.getFullPathName().toStdString());
#endif
}

// Loads one optional CPU model of the input-type classifier once per process
// (under its own mutex); logs, never fails.
template <typename Model>
void loadOptionalModel(Model &model, std::mutex &mutex, const juce::File &file,
                       const char *name)
{
  juce::ignoreUnused(name); // only logged; LOG may be compiled out
  const std::lock_guard<std::mutex> lock(mutex);
  if (model.isLoaded())
    return;
  if (!file.existsAsFile())
  {
    LOG(juce::String(name) + " model not found at: " + file.getFullPathName() +
        " (the input-type classifier runs without it)");
    return;
  }
  if (model.load(toFsPath(file)))
    LOG(juce::String(name) + " model loaded");
  else
    LOG("Failed to load " + juce::String(name) + " model");
}

// Held weakly so the ORT sessions die with the last controller, never during
// static destruction at DLL unload.
std::shared_ptr<SharedInferenceModels> acquireSharedModels(
    const juce::String &key, bool shareAcrossControllers,
    std::shared_ptr<SharedInferenceModels> current)
{
  if (current && current->key == key)
    return current;

  auto create = [&key]()
  {
    auto created = std::make_shared<SharedInferenceModels>();
    created->key = key;
    return created;
  };
  if (!shareAcrossControllers)
    return create();

  static std::mutex cacheMutex;
  static std::map<juce::String, std::weak_ptr<SharedInferenceModels>> cache;
  const std::lock_guard<std::mutex> lock(cacheMutex);
  auto &slot = cache[key];
  if (auto existing = slot.lock())
    return existing;
  auto created = create();
  slot = created;
  return created;
}

// Shared locks can be held by another controller's long pass. Give up once
// this controller is shutting down so its destructor never waits on them;
// callers check shuttingDown after locking.
std::unique_lock<std::mutex> lockShared(std::mutex &mutex,
                                        const std::atomic<bool> &shuttingDown)
{
  std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
  while (!lock.try_lock() && !shuttingDown.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return lock;
}

// Locks the set's mutex only when the detector really belongs to the set; a
// private DirectML-to-CPU fallback detector needs no cross-controller lock.
template <typename Detector>
std::unique_lock<std::mutex> lockModel(
    const std::shared_ptr<SharedInferenceModels> &models,
    const std::shared_ptr<Detector> &detector,
    std::shared_ptr<Detector> SharedInferenceModels::*member,
    std::mutex SharedInferenceModels::*modelMutex,
    const std::atomic<bool> &shuttingDown)
{
  if (!models || (*models).*member != detector)
    return {};
  return lockShared((*models).*modelMutex, shuttingDown);
}
} // namespace

EditorController::EditorController(bool enableAudioDevice)
{
  project = std::make_unique<Project>();
  if (enableAudioDevice)
    audioEngine = std::make_unique<AudioEngine>();

  fcpePitchDetector = std::make_shared<FCPEPitchDetector>();
  rmvpePitchDetector = std::make_shared<RMVPEPitchDetector>();
  swiftF0PitchDetector = std::make_shared<SwiftF0PitchDetector>();
  gameDetector = std::make_shared<GAMEDetector>();
  vocoder = std::make_unique<Vocoder>();
  incrementalSynth = std::make_unique<IncrementalSynthesizer>();
  playbackController = std::make_unique<PlaybackController>();

  fcpeModelPath = PlatformPaths::getModelFile("fcpe.onnx");
  melFilterbankPath = PlatformPaths::getModelFile("mel_filterbank.bin");
  centTablePath = PlatformPaths::getModelFile("cent_table.bin");
  rmvpeModelPath = PlatformPaths::getModelFile("rmvpe.onnx");
  swiftF0ModelPath = PlatformPaths::getModelFile("swift_f0.onnx");
  basicPitchModelPath = PlatformPaths::getModelFile("basic_pitch_nmp.onnx");
  yamnetModelPath = PlatformPaths::getModelFile("yamnet.onnx");
  gameModelDir = PlatformPaths::getModelSubDir("GAME", "encoder.onnx");

  incrementalSynth->setVocoder(vocoder.get());
  incrementalSynth->setSynthesisEngine(synthesisEngineType);
  if (audioEngine)
    playbackController->setAudioEngine(audioEngine.get());
}

void EditorController::requestShutdown()
{
  shuttingDown.store(true);
  cancelLoadingFlag.store(true);
  hostAnalysisJobId.fetch_add(1);
  if (incrementalSynth)
    incrementalSynth->cancel();
  if (polyRenderCancel)
    polyRenderCancel->store(true);
  if (auditionCancel)
    auditionCancel->store(true);
  if (prerenderCancel)
    prerenderCancel->store(true);
  if (vocoder)
    vocoder->requestShutdown();
}

EditorController::~EditorController()
{
  requestShutdown();
  if (modelReloadThread.joinable())
    modelReloadThread.join();
  if (loaderThread.joinable())
    loaderThread.join();
  if (loaderJoinerThread.joinable())
    loaderJoinerThread.join();
  if (polyRenderThread.joinable())
    polyRenderThread.join();
  if (auditionThread.joinable())
    auditionThread.join();
  if (prerenderThread.joinable())
    prerenderThread.join();
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
  if (shuttingDown.load())
    return;
  auto provider = getProviderFromDevice(device);
  int resolvedDeviceId = deviceId < 0 ? 0 : deviceId;

  std::shared_ptr<SharedInferenceModels> currentModels;
  {
    const std::lock_guard<std::mutex> lock(modelPtrMutex);
    currentModels = sharedModels;
  }
  const auto generation = reloadGeneration.fetch_add(1) + 1;
  auto models = acquireSharedModels(
      device + ":" + juce::String(resolvedDeviceId),
      provider != GPUProvider::CPU, std::move(currentModels));

  auto fcpePath = fcpeModelPath;
  auto melPath = melFilterbankPath;
  auto centPath = centTablePath;
  auto rmvpePath = rmvpeModelPath;
  auto swiftF0Path = swiftF0ModelPath;
  auto basicPitchPath = basicPitchModelPath;
  auto yamnetPath = yamnetModelPath;
  auto gamePath = gameModelDir;
  auto loadTask = [device = device,
                     provider,
                     resolvedDeviceId,
                     fcpePath,
                     melPath,
                     centPath,
                     rmvpePath,
                     swiftF0Path,
                     basicPitchPath,
                     yamnetPath,
                     gamePath,
                     models](EditorController *self)
  {
    if (!self || self->shuttingDown.load())
      return;

    // SwiftF0 is a ~135 KB CPU model that loads in milliseconds; load it
    // inline before the GPU models.
    if (models->swiftF0 && !models->swiftF0->isLoaded())
    {
      if (swiftF0Path.existsAsFile())
      {
        if (models->swiftF0->loadModel(swiftF0Path))
          LOG("SwiftF0 pitch detector loaded successfully");
        else
          LOG("Failed to load SwiftF0 model");
      }
      else
      {
        LOG("SwiftF0 model not found at: " + swiftF0Path.getFullPathName());
      }
    }

    // The input-type classifier's CPU models, inline for the same reason
    // (loaded once per process; later calls return at once).
    if (auto &classifier = models->classifier)
    {
      loadOptionalModel(classifier->basicPitch, classifier->basicPitchMutex,
                        basicPitchPath, "Basic Pitch");
      loadOptionalModel(classifier->yamnet, classifier->yamnetMutex, yamnetPath,
                        "YAMNet");
    }

    // ONNX Runtime DirectML 1.18/1.19 can crash inside the GPU driver when
    // multiple sessions for the same device are created concurrently. This is
    // especially easy to trigger in optimized builds, where model setup
    // overlaps much more tightly. Keep DirectML session creation on this one
    // loader thread; CPU and other providers retain parallel startup.
    if (provider == GPUProvider::DirectML)
    {
      const std::lock_guard<std::mutex> directMLLoadLock(
          OnnxRuntime::getDirectMLSessionMutex());
      LOG("EditorController: serializing DirectML model loading");

      if (!self->shuttingDown.load() && models->fcpe && !models->fcpe->isLoaded() &&
          fcpePath.existsAsFile())
      {
        if (self->shuttingDown.load())
          return;
        LOG("EditorController: loading FCPE model (device " + device +
            ", id " + juce::String(resolvedDeviceId) + ")...");
        if (models->fcpe->loadModel(fcpePath, melPath, centPath,
                                     provider, resolvedDeviceId))
          LOG("FCPE pitch detector loaded successfully");
        else
          LOG("Failed to load FCPE model");
      }
      else if (models->fcpe && !models->fcpe->isLoaded())
      {
        LOG("FCPE model not found at: " + fcpePath.getFullPathName());
      }

      if (!self->shuttingDown.load() && models->rmvpe && !models->rmvpe->isLoaded() &&
          rmvpePath.existsAsFile())
      {
        if (self->shuttingDown.load())
          return;
        LOG("EditorController: loading RMVPE model (device " + device +
            ", id " + juce::String(resolvedDeviceId) + ")...");
        if (models->rmvpe->loadModel(rmvpePath, provider,
                                      resolvedDeviceId))
          LOG("RMVPE pitch detector loaded successfully");
        else
          LOG("Failed to load RMVPE model");
      }
      else if (models->rmvpe && !models->rmvpe->isLoaded())
      {
        LOG("RMVPE model not found at: " + rmvpePath.getFullPathName());
      }

      if (!self->shuttingDown.load() && models->game && !models->game->isLoaded() &&
          gamePath.isDirectory())
      {
        if (self->shuttingDown.load())
          return;
        LOG("EditorController: loading GAME models from " +
            gamePath.getFullPathName() + " (device " + device + ", id " +
            juce::String(resolvedDeviceId) + ")...");
        if (models->game->loadModels(gamePath, provider,
                                      resolvedDeviceId))
          LOG("GAME detector loaded successfully, isLoaded=" +
              juce::String(models->game->isLoaded() ? "true" :
                           "false"));
        else
          LOG("Failed to load GAME models from " +
              gamePath.getFullPathName());
      }
      else if (models->game && !models->game->isLoaded())
      {
        LOG("GAME model directory not found: " + gamePath.getFullPathName() +
            " isDirectory=" +
            juce::String(gamePath.isDirectory() ? "true" : "false"));
      }
      else if (!models->game)
      {
        LOG("GAME detector not created (gameDetector is null)");
      }

      return;
    }

    // Load all models in parallel — each operates on an independent object
    // with its own Ort::Env, so no shared state.
    std::thread fcpeThread;
    std::thread rmvpeThread;
    std::thread gameThread;

    if (!self->shuttingDown.load() && models->fcpe && !models->fcpe->isLoaded() &&
        fcpePath.existsAsFile())
    {
      fcpeThread = std::thread([&]()
                               {
        if (self->shuttingDown.load())
          return;
        LOG("EditorController: loading FCPE model (device " + device +
            ", id " + juce::String(resolvedDeviceId) + ")...");
        if (models->fcpe->loadModel(fcpePath, melPath, centPath,
                                     provider, resolvedDeviceId)) {
          LOG("FCPE pitch detector loaded successfully");
        } else {
          LOG("Failed to load FCPE model");
        } });
    }
    else if (models->fcpe && !models->fcpe->isLoaded())
    {
      LOG("FCPE model not found at: " + fcpePath.getFullPathName());
    }

    if (!self->shuttingDown.load() && models->rmvpe && !models->rmvpe->isLoaded() &&
        rmvpePath.existsAsFile())
    {
      rmvpeThread = std::thread([&]()
                                {
        if (self->shuttingDown.load())
          return;
        LOG("EditorController: loading RMVPE model (device " + device +
            ", id " + juce::String(resolvedDeviceId) + ")...");
        if (models->rmvpe->loadModel(rmvpePath, provider,
                                      resolvedDeviceId)) {
          LOG("RMVPE pitch detector loaded successfully");
        } else {
          LOG("Failed to load RMVPE model");
        } });
    }
    else if (models->rmvpe && !models->rmvpe->isLoaded())
    {
      LOG("RMVPE model not found at: " + rmvpePath.getFullPathName());
    }

    if (!self->shuttingDown.load() && models->game && !models->game->isLoaded() &&
        gamePath.isDirectory())
    {
      gameThread = std::thread([&]()
                               {
        if (self->shuttingDown.load())
          return;
        LOG("EditorController: loading GAME models from " + gamePath.getFullPathName() +
            " (device " + device + ", id " + juce::String(resolvedDeviceId) + ")...");
        if (models->game->loadModels(gamePath, provider,
                                      resolvedDeviceId)) {
          LOG("GAME detector loaded successfully, isLoaded=" +
              juce::String(models->game->isLoaded() ? "true" : "false"));
        } else {
          LOG("Failed to load GAME models from " + gamePath.getFullPathName());
        } });
    }
    else if (models->game && !models->game->isLoaded())
    {
      LOG("GAME model directory not found: " + gamePath.getFullPathName() +
          " isDirectory=" + juce::String(gamePath.isDirectory() ? "true" : "false"));
    }
    else if (!models->game)
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

  auto reloadTask = [loadTask, models, generation](EditorController *self)
  {
    {
      // One loader per set; models another controller already loaded are
      // skipped by loadTask.
      // Per controller: the sync (analysis) and async (editor/settings) paths
      // must not load at once, even into two private CPU sets.
      const std::lock_guard<std::mutex> modelLoadLock(self->modelLoadMutex);
      const auto loadLock = lockShared(models->loadMutex, self->shuttingDown);
      if (self->shuttingDown.load())
        return;
      loadTask(self);
    }
    if (self->shuttingDown.load())
      return;
    const std::lock_guard<std::mutex> lock(self->modelPtrMutex);
    // Sync (analysis) and async (device change) loads for different devices
    // can finish in either order; keep the newest request's models.
    if (generation < self->installedGeneration)
      return;
    self->installedGeneration = generation;
    self->sharedModels = models;
    self->fcpePitchDetector = models->fcpe;
    self->rmvpePitchDetector = models->rmvpe;
    self->swiftF0PitchDetector = models->swiftF0;
    self->gameDetector = models->game;
    self->classifierModels = models->classifier;
  };

  if (!async)
  {
    reloadTask(this);
    return;
  }

  // A worker that is already running takes the newest request when its
  // current load finishes, so a device change mid-load is never dropped.
  {
    const std::lock_guard<std::mutex> lock(reloadRequestMutex);
    pendingReload = [this, reloadTask]() { reloadTask(this); };
    if (isReloadingModels.exchange(true))
      return;
  }

  if (modelReloadThread.joinable())
    modelReloadThread.join();

  modelReloadThread = std::thread([this]()
                                  {
    for (;;)
    {
      std::function<void()> task;
      {
        const std::lock_guard<std::mutex> lock(reloadRequestMutex);
        task = std::exchange(pendingReload, nullptr);
        if (!task)
        {
          isReloadingModels = false;
          reloadFinished.notify_all();
          return;
        }
      }
      task();
    } });
}

bool EditorController::isInferenceBusy() const
{
  if (incrementalSynth && incrementalSynth->isSynthesizing())
    return true;
  if (polyRenderRunning) // a polyphonic render reads this controller's project
    return true;
  if (isReloadingModels.load())
    return true;
  return false;
}

bool EditorController::isSelectedPitchDetectorLoaded() const
{
  const std::lock_guard<std::mutex> lock(modelPtrMutex);
  if (pitchDetectorType == PitchDetectorType::FCPE)
    return fcpePitchDetector && fcpePitchDetector->isLoaded();
  if (pitchDetectorType == PitchDetectorType::RMVPE)
    return rmvpePitchDetector && rmvpePitchDetector->isLoaded();
  if (pitchDetectorType == PitchDetectorType::SwiftF0)
    return swiftF0PitchDetector && swiftF0PitchDetector->isLoaded();
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

    updateProgress(0.04, TR("progress.loading_audio"));

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
    // The file's own channels, kept for the polyphonic engine until the take
    // is classified (stereo matters for a piano or a string section).
    auto pendingTake = std::make_shared<PolyphonicTake>();
    pendingTake->sourceRate = static_cast<double>(srcSampleRate);

    updateProgress(0.08, TR("progress.reading_audio"));
    if (reader->numChannels == 1) {
      reader->read(&buffer, 0, numSamples, 0, true, false);
      pendingTake->source.makeCopyOf(buffer);
    } else {
      juce::AudioBuffer<float> stereoBuffer(2, numSamples);
      reader->read(&stereoBuffer, 0, numSamples, 0, true, true);

      const float *left = stereoBuffer.getReadPointer(0);
      const float *right = stereoBuffer.getReadPointer(1);
      float *mono = buffer.getWritePointer(0);

      for (int i = 0; i < numSamples; ++i)
        mono[i] = (left[i] + right[i]) * 0.5f;
      pendingTake->source = std::move(stereoBuffer);
    }

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    if (srcSampleRate != SAMPLE_RATE) {
      updateProgress(0.14, TR("progress.resampling"));
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

    updateProgress(0.15, TR("progress.preparing_project"));
    auto newProject = std::make_unique<Project>();
    newProject->setFilePath(file);
    newProject->setAudioSha256(SHA256Utils::fileSHA256(file));
    auto &audioData = newProject->getAudioData();
    audioData.waveform = std::move(buffer);
    audioData.sampleRate = SAMPLE_RATE;
    newProject->setPolyphonicTake(std::move(pendingTake));

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    updateProgress(0.17, TR("progress.analyzing_audio"));
    analyzeAudio(*newProject, updateProgress);

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    updateProgress(0.95, TR("progress.finalizing"));

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
    const LoadCompleteCallback &onComplete,
    HostAnalysisOptions options)
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

  loaderThread = std::thread([this, buffer, sampleRate, onProgress, onComplete, jobId,
                              options = std::move(options)]() mutable
                             {
    if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
    {
      isLoadingAudio = false;
      return;
    }

    // Let an async load from editor open finish rather than loading the same
    // detectors concurrently from this thread.
    {
      // Timed so cancel, shutdown or a newer job (plain atomics) still end it.
      std::unique_lock<std::mutex> lock(reloadRequestMutex);
      while (isReloadingModels.load() && !shuttingDown.load() &&
             !cancelLoadingFlag.load() && hostAnalysisJobId.load() == jobId)
        reloadFinished.wait_for(lock, std::chrono::milliseconds(50));
    }

    if (!isSelectedPitchDetectorLoaded())
    {
      if (onProgress)
        onProgress(0.05, TR("progress.loading"));
      reloadInferenceModels(false);
    }

    if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
    {
      isLoadingAudio = false;
      return;
    }

    juce::AudioBuffer<float> resampledBuffer;
    const double inputSampleRate = sampleRate;
    if (inputSampleRate > 0.0 &&
        std::abs(inputSampleRate - static_cast<double>(SAMPLE_RATE)) > 1e-6) {
      resampledBuffer = AudioResampler::resample(
          buffer, inputSampleRate, static_cast<double>(SAMPLE_RATE));
    }

    const juce::AudioBuffer<float> &stored =
        resampledBuffer.getNumSamples() > 0 ? resampledBuffer : buffer;
    const double storedSampleRate = resampledBuffer.getNumSamples() > 0
                                        ? static_cast<double>(SAMPLE_RATE)
                                        : inputSampleRate;

    auto projectCopy = std::make_unique<Project>();
    projectCopy->getAudioData().waveform = stored;
    projectCopy->getAudioData().sampleRate = static_cast<int>(storedSampleRate);
    const auto &takeChannels = options.takeChannels;
    if (options.initialMode)
      projectCopy->setAnalysisMode(*options.initialMode);
    if (takeChannels != nullptr && takeChannels->getNumSamples() > 0)
    {
      // The clip's own channels, for the polyphonic engine (as the file
      // loader keeps them).
      auto pendingTake = std::make_shared<PolyphonicTake>();
      const int channels = std::min(2, takeChannels->getNumChannels());
      pendingTake->source.setSize(channels, takeChannels->getNumSamples());
      for (int ch = 0; ch < channels; ++ch)
        pendingTake->source.copyFrom(ch, 0, *takeChannels, ch, 0, takeChannels->getNumSamples());
      pendingTake->sourceRate = inputSampleRate;
      projectCopy->setPolyphonicTake(std::move(pendingTake));
    }

    auto updateProgress = [&](double p, const juce::String &msg) {
      if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
        return;
      if (onProgress)
        onProgress(p, msg);
    };

    if (options.trackMode)
    {
      AnalysisModeState track;
      track.mode = *options.trackMode;
      track.detector = options.trackDetector;
      track.userChosen = options.trackModeUserChosen;
      analysisTrackMode = track;
    }
    analyzeAudio(*projectCopy, updateProgress);
    analysisTrackMode.reset();
    // A vocal clip does not keep its channels: a host session holds many
    // clips, and the host can read the source again.
    if (takeChannels != nullptr && !projectCopy->isPolyphonic())
      projectCopy->setPolyphonicTake(nullptr);

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

void EditorController::classifyHostAudioAsync(
    std::vector<std::pair<juce::AudioBuffer<float>, double>> clips,
    std::function<void(std::vector<AnalysisModeState>)> onComplete)
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
  loaderThread = std::thread([this, clips = std::move(clips), onComplete, jobId]() mutable
                             {
    const auto superseded = [this, jobId] {
      return cancelLoadingFlag.load() || shuttingDown.load() ||
             hostAnalysisJobId.load() != jobId;
    };
    {
      std::unique_lock<std::mutex> lock(reloadRequestMutex);
      while (isReloadingModels.load() && !superseded())
        reloadFinished.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (!superseded() && !isSelectedPitchDetectorLoaded())
      reloadInferenceModels(false);

    std::vector<AnalysisModeState> states;
    states.reserve(clips.size());
    for (auto &[buffer, rate] : clips)
    {
      if (superseded())
        break;
      // As the analysis sees it: the project rate.
      Project clip;
      auto &audioData = clip.getAudioData();
      if (rate > 0.0 && std::abs(rate - static_cast<double>(SAMPLE_RATE)) > 1e-6)
        audioData.waveform = AudioResampler::resample(buffer, rate, static_cast<double>(SAMPLE_RATE));
      else
        audioData.waveform = std::move(buffer);
      audioData.sampleRate = SAMPLE_RATE;
      try
      {
        classifyInputType(clip);
      }
      catch (const std::exception &e)
      {
        reportAnalysisException("classifyHostAudio", e.what());
      }
      states.push_back(clip.getAnalysisMode());
    }

    const bool done = !superseded() && states.size() == clips.size();
    juce::MessageManager::callAsync(
        [this, states = std::move(states), onComplete, jobId, done]() mutable {
          if (hostAnalysisJobId.load() != jobId)
            return;
          isLoadingAudio = false;
          if (done && onComplete)
            onComplete(std::move(states));
        }); });
}

void EditorController::resynthesizeIncrementalAsync(
    Project &project,
    const std::function<void(const juce::String &)> &onProgress,
    const std::function<void(bool)> &onComplete,
    std::atomic<bool> &pendingRerun,
    bool isPluginMode)
{
  // A polyphonic take renders with its engine (the host's render path comes
  // here); one render at a time, newest edits win (renderPolyphonicAsync).
  if (project.isPolyphonic())
  {
    if (&project != getProject())
    {
      if (onComplete)
        onComplete(false);
      return;
    }
    renderPolyphonicAsync(onComplete);
    return;
  }

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
    if (onProgress)
      onProgress(TR("progress.loading_vocoder"));

    auto modelPath = PlatformPaths::getVocoderModelFile();
    if (!modelPath.exists() || !vocoder->loadModel(modelPath))
    {
      if (onComplete)
        onComplete(false);
      return;
    }
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

  // Covers every other load path (lazy load above, settings device change,
  // project open): never render AI on a CPU fallback session.
  switchToClassicIfVocoderOnCpu();

  synth->setProject(&project);
  synth->setVocoder(vocoder.get());
  synth->setSynthesisEngine(synthesisEngineType);
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
          // The AI render failed for good: fall back to classic (PSOLA) and
          // render the same edit again rather than leaving it silent. Same
          // switch as the DirectML analysis fallback.
          if (synthesisEngineType == SynthesisEngineType::Vocoder && vocoder &&
              vocoder->consumeUnrecoverableRenderFailure())
          {
            LOG("Vocoder render failed on every device; switching synthesis "
                "engine to PSOLA");
            juce::MessageManager::callAsync(
                [this, projectPtr, pending, onComplete, isPluginMode]()
                {
                  setSynthesisEngineType(SynthesisEngineType::Psola);
                  if (onSynthesisEngineFallback)
                    onSynthesisEngineFallback(SynthesisEngineType::Psola);
                  resynthesizeIncrementalAsync(*projectPtr, nullptr, onComplete,
                                               *pending, isPluginMode);
                });
            return;
          }

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

bool EditorController::switchToClassicIfVocoderOnCpu()
{
  if (!vocoder || !vocoder->consumeCpuFallbackOnLoad())
    return false;
  if (synthesisEngineType != SynthesisEngineType::Vocoder)
    return false;

  setSynthesisEngineType(SynthesisEngineType::Psola);
  LOG("Vocoder loaded on CPU after a GPU provider failed; synthesis engine "
      "switched to PSOLA");
  juce::MessageManager::callAsync(
      [this]()
      {
        if (onSynthesisEngineFallback)
          onSynthesisEngineFallback(SynthesisEngineType::Psola);
      });
  return true;
}

void EditorController::reportAnalysisException(const juce::String &where,
                                                 const juce::String &what)
{
  LOG(where + " threw: " + what);
  const auto detail = "\n\n" + TR("error.details") + "\n" + what.substring(0, 1000);
  juce::MessageManager::callAsync([detail]()
                                  { juce::AlertWindow::showMessageBoxAsync(
                                        juce::AlertWindow::WarningIcon, TR("error.inference_failed"),
                                        TR("error.inference_failed") + detail); });
}

void EditorController::analyzeAudio(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress,
    std::function<void()> onComplete)
{
  // Called on loader threads. An exception escaping a std::thread calls
  // std::terminate, which closed the standalone app silently mid-import
  // (issue #17), so catch everything here and report it.
  try
  {
    analyzeAudioImpl(targetProject, onProgress, std::move(onComplete));
  }
  catch (const std::exception &e)
  {
    reportAnalysisException("analyzeAudio", e.what());
  }
  catch (...)
  {
    reportAnalysisException("analyzeAudio", "unknown error");
  }
}

void EditorController::classifyInputType(Project &targetProject)
{
  auto state = targetProject.getAnalysisMode();
  if (state.classified)
    return;

  const auto &audioData = targetProject.getAudioData();
  const int numSamples = audioData.waveform.getNumSamples();
  if (numSamples == 0 || audioData.sampleRate <= 0)
    return;
  const double sampleRate = static_cast<double>(audioData.sampleRate);
  const auto t0 = std::chrono::steady_clock::now();

  // Channel 0 is the analysis signal, as for the rest of the analysis (the
  // file loader stores mono; the host path keeps the host's channels).
  const auto excerpt = inputtype::selectExcerpt(
      audioData.waveform.getReadPointer(0), numSamples, sampleRate);

  std::shared_ptr<SharedInferenceModels> models;
  std::shared_ptr<SwiftF0PitchDetector> swiftF0Model;
  std::shared_ptr<ClassifierModels> classifier;
  {
    const std::lock_guard<std::mutex> lock(modelPtrMutex);
    models = sharedModels;
    swiftF0Model = swiftF0PitchDetector;
    classifier = classifierModels;
  }

  // SwiftF0 on the excerpt itself: classify() reads the pitch track against
  // the excerpt's samples, frame for frame. Too short to classify: skip it.
  std::vector<float> f0, confidence;
  const bool longEnough = static_cast<double>(excerpt.size()) / sampleRate + 1e-3 >=
                          inputtype::Config{}.minAudibleSeconds;
  if (longEnough && swiftF0Model && swiftF0Model->isLoaded())
  {
    const auto modelLock =
        lockModel(models, swiftF0Model, &SharedInferenceModels::swiftF0,
                  &SharedInferenceModels::swiftF0Mutex, shuttingDown);
    if (shuttingDown.load())
      return;
    f0 = swiftF0Model->extractF0(excerpt.data(), static_cast<int>(excerpt.size()),
                                 audioData.sampleRate);
    confidence = swiftF0Model->getLastConfidence();
  }
  inputtype::PitchTrack track;
  if (!f0.empty() && f0.size() == confidence.size())
    track = {f0.data(), confidence.data(), static_cast<int>(f0.size())};

  inputtype::Decision decision;
  if (classifier)
  {
    // Process-wide models: another track's classification may hold them for a
    // moment. Fixed order (Basic Pitch, then YAMNet) wherever both are held.
    const auto basicPitchLock = lockShared(classifier->basicPitchMutex, shuttingDown);
    const auto yamnetLock = lockShared(classifier->yamnetMutex, shuttingDown);
    if (shuttingDown.load())
      return;
    decision = inputtype::classify(
        excerpt.data(), static_cast<int>(excerpt.size()), sampleRate, track,
        classifier->basicPitch.isLoaded() ? &classifier->basicPitch : nullptr,
        classifier->yamnet.isLoaded() ? &classifier->yamnet : nullptr,
        inputtype::Config{}, &shuttingDown);
  }
  else
  {
    decision = inputtype::classify(
        excerpt.data(), static_cast<int>(excerpt.size()), sampleRate, track,
        nullptr, nullptr, inputtype::Config{}, &shuttingDown);
  }
  if (shuttingDown.load())
    return;

  analysismode::recordDecision(state, decision);
  targetProject.setAnalysisMode(state);

  const auto &f = decision.features;
  LOG("Input type: " + juce::String(inputtype::toString(decision.pipeline)) +
      " (" + juce::String(inputtype::toString(decision.reason)) +
      (decision.confident() ? "" : ", not confident") + "); audible " +
      juce::String(f.audibleSeconds, 1) + " s, poly2 " +
      (f.polyphonyMeasured ? juce::String(f.polyphony.poly2, 3) : juce::String("-")) +
      ", voice " + (f.voiceScore >= 0.0f ? juce::String(f.voiceScore, 3) : juce::String("-")) +
      ", pitch cues " + juce::String(f.cues.offgridCents, 1) + " c / " +
      juce::String(f.cues.glide, 3) + "; mode " + analysismode::id(state.mode) +
      (state.userChosen ? " (user)" : "") + "; " +
      juce::String(std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count(),
                   0) +
      " ms");
  juce::ignoreUnused(t0, f); // only logged; LOG may be compiled out
}

bool EditorController::analyzePolyphonicTake(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress)
{
  auto &audioData = targetProject.getAudioData();
  const auto t0 = std::chrono::steady_clock::now();

  // The take's own channels (kept by the file loader) at the project rate, with
  // r8brain; otherwise the project's waveform. At most two channels.
  juce::AudioBuffer<float> source;
  double sourceRate = static_cast<double>(audioData.sampleRate);
  if (const auto pending = targetProject.getPolyphonicTake();
      pending && pending->source.getNumSamples() > 0 && pending->sourceRate > 0.0)
  {
    source.makeCopyOf(pending->source);
    sourceRate = pending->sourceRate;
  }
  else
  {
    source.makeCopyOf(audioData.waveform);
  }
  if (source.getNumChannels() > 2)
    source.setSize(2, source.getNumSamples(), true);
  if (std::abs(sourceRate - static_cast<double>(SAMPLE_RATE)) > 1e-6)
  {
    onProgress(0.18, TR("progress.resampling"));
    source = AudioResampler::resample(source, sourceRate,
                                      static_cast<double>(SAMPLE_RATE));
  }
  const int numSamples = source.getNumSamples();
  if (numSamples <= 0 || shuttingDown.load())
    return false;

  std::vector<float> mono(static_cast<size_t>(numSamples), 0.0f);
  const float channelGain = 1.0f / static_cast<float>(source.getNumChannels());
  for (int ch = 0; ch < source.getNumChannels(); ++ch)
    juce::FloatVectorOperations::addWithMultiply(mono.data(), source.getReadPointer(ch),
                                                 channelGain, numSamples);

  std::shared_ptr<ClassifierModels> classifier;
  {
    const std::lock_guard<std::mutex> lock(modelPtrMutex);
    classifier = classifierModels;
  }

  // Basic Pitch note evidence for the note filter (and, for Guided, the search).
  onProgress(0.20, TR("progress.detecting_notes"));
  polyphonic::NoteEvidence evidence;
  if (classifier)
  {
    const auto lock = lockShared(classifier->basicPitchMutex, shuttingDown);
    if (shuttingDown.load())
      return false;
    if (classifier->basicPitch.isLoaded())
      evidence = classifier->basicPitch.compute(mono.data(), numSamples,
                                                static_cast<double>(SAMPLE_RATE),
                                                &shuttingDown);
  }
  if (shuttingDown.load())
    return false;

  auto params = polyphonic::Params::forSampleRate(static_cast<double>(SAMPLE_RATE));
  const bool guided = targetProject.getAnalysisMode().detector == PolyphonicDetector::Guided;
  params.detectionMode = guided && !evidence.empty()
                             ? polyphonic::DetectionMode::NeuralGuided
                             : polyphonic::DetectionMode::Harmonic;
  auto analysis = polyphonic::analyze(
      mono.data(), numSamples, params, evidence.empty() ? nullptr : &evidence,
      &shuttingDown, [&onProgress](float p) {
        onProgress(0.25 + 0.70 * juce::jlimit(0.0, 1.0, static_cast<double>(p)),
                   TR("progress.detecting_notes"));
      });
  if (shuttingDown.load())
    return false;

  auto take = std::make_shared<PolyphonicTake>();
  take->analysis = std::make_shared<const polyphonic::Analysis>(std::move(analysis));
  take->source = std::move(source);
  take->sourceRate = static_cast<double>(SAMPLE_RATE);
  int activeNotes = 0;
  for (const auto &note : take->analysis->notes)
    activeNotes += note.active ? 1 : 0;
  targetProject.setPolyphonicTake(take);

  // The waveform is the mono mix of what the engine analysed, so the display
  // and playback match it; nothing of the vocal analysis applies.
  audioData.waveform.setSize(1, numSamples, false, false, true);
  audioData.waveform.copyFrom(0, 0, mono.data(), numSamples);
  audioData.sampleRate = SAMPLE_RATE;
  audioData.melSpectrogram.clear();
  audioData.rawF0.clear();
  audioData.cleanedF0.clear();
  audioData.denseF0.clear();
  audioData.f0.clear();
  audioData.basePitch.clear();
  audioData.deltaPitch.clear();
  audioData.voicedMask.clear();
  audioData.vadMask.clear();
  audioData.breathSibilance.clear();
  audioData.breathGainDb.clear();
  audioData.baseF0.clear();
  audioData.segmentDebugChunks.clear();
  targetProject.clearNotes();
  // The key from the take's notes, for Scale drag snapping and double-click
  // (feature review #15).
  ScaleUtils::detectAndApplyScale(targetProject);

  LOG("Polyphonic analysis: " + juce::String(activeNotes) + " notes (" +
      juce::String(static_cast<int>(take->analysis->notes.size()) - activeNotes) +
      " candidates), " + juce::String(take->source.getNumChannels()) + " ch, " +
      (params.detectionMode == polyphonic::DetectionMode::NeuralGuided ? "guided"
                                                                        : "harmonic") +
      (evidence.empty() ? ", no Basic Pitch" : "") + ", " +
      juce::String(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                       .count(),
                   1) +
      " s");
  juce::ignoreUnused(t0, activeNotes);
  onProgress(1.0, TR("progress.detecting_notes"));
  return true;
}

void EditorController::analyzeAudioImpl(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress,
    std::function<void()> onComplete)
{
  if (shuttingDown.load())
    return;

  auto &audioData = targetProject.getAudioData();
  if (audioData.waveform.getNumSamples() == 0)
    return;

  classifyInputType(targetProject);
  if (shuttingDown.load())
    return;
  if (analysisTrackMode)
  {
    // The track's mode applies; the take's own classification stays recorded.
    auto state = targetProject.getAnalysisMode();
    state.mode = analysisTrackMode->mode;
    state.detector = analysisTrackMode->detector;
    state.userChosen = analysisTrackMode->userChosen;
    targetProject.setAnalysisMode(state);
  }

  if (polyphonicAnalysisEnabled &&
      targetProject.getAnalysisMode().mode == AnalysisMode::Polyphonic)
  {
    if (analyzePolyphonicTake(targetProject, onProgress) && onComplete)
      onComplete();
    return;
  }
  // A vocal take keeps only the take's own channels (no analysis), so a later
  // switch to polyphonic (reanalyzeAsync) still has the stereo source.
  if (const auto take = targetProject.getPolyphonicTake(); take && take->hasAnalysis())
  {
    auto sourceOnly = std::make_shared<PolyphonicTake>();
    sourceOnly->source.makeCopyOf(take->source);
    sourceOnly->sourceRate = take->sourceRate;
    targetProject.setPolyphonicTake(std::move(sourceOnly));
  }

  auto showMissingModelAndAbort = [](const juce::String &modelName,
                                     const juce::File &path)
  {
    juce::MessageManager::callAsync([modelName, path]()
                                    { juce::AlertWindow::showMessageBoxAsync(
                                          juce::AlertWindow::WarningIcon, TR("error.missing_model"),
                                          modelName + " " + TR("error.model_not_found_at") + "\n" +
                                              path.getFullPathName() + "\n\n" +
                                              TR("error.install_models")); });
  };
  auto showModelLoadFailedAndAbort = [](const juce::String &modelName,
                                        const juce::File &path)
  {
    juce::MessageManager::callAsync([modelName, path]()
                                    { juce::AlertWindow::showMessageBoxAsync(
                                          juce::AlertWindow::WarningIcon, TR("error.model_load_failed"),
                                          modelName + " " + TR("error.model_exists_but_failed") + "\n" +
                                              path.getFullPathName() + "\n\n" +
                                              TR("error.check_device_settings")); });
  };

  // Extract F0
  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();

  onProgress(0.175, TR("progress.mel_spectrogram"));
  MelSpectrogram melComputer(audioData.sampleRate, N_FFT, HOP_SIZE, NUM_MELS,
                             FMIN, FMAX);
  audioData.melSpectrogram = melComputer.compute(samples, numSamples);

  if (shuttingDown.load())
    return;

  int targetFrames = static_cast<int>(audioData.melSpectrogram.size());

  onProgress(0.275, "Extracting pitch (F0)...");

  // Hold this analysis' models so a device switch on another thread cannot
  // free them mid-inference.
  std::shared_ptr<SharedInferenceModels> models;
  std::shared_ptr<FCPEPitchDetector> fcpeModel;
  std::shared_ptr<RMVPEPitchDetector> rmvpeModel;
  std::shared_ptr<SwiftF0PitchDetector> swiftF0Model;
  {
    const std::lock_guard<std::mutex> lock(modelPtrMutex);
    models = sharedModels;
    fcpeModel = fcpePitchDetector;
    rmvpeModel = rmvpePitchDetector;
    swiftF0Model = swiftF0PitchDetector;
  }

  if (pitchDetectorType == PitchDetectorType::RMVPE)
  {
    if (!rmvpeModelPath.existsAsFile())
    {
      showMissingModelAndAbort("rmvpe.onnx", rmvpeModelPath);
      return;
    }
    if (!rmvpeModel || !rmvpeModel->isLoaded())
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
    if (!fcpeModel || !fcpeModel->isLoaded())
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
  else if (pitchDetectorType == PitchDetectorType::SwiftF0)
  {
    if (!swiftF0ModelPath.existsAsFile())
    {
      showMissingModelAndAbort("swift_f0.onnx", swiftF0ModelPath);
      return;
    }
    if (!swiftF0Model || !swiftF0Model->isLoaded())
    {
      showModelLoadFailedAndAbort("swift_f0.onnx", swiftF0ModelPath);
      return;
    }
  }

  LOG("========== PITCH DETECTOR SELECTION ==========");
  LOG("Selected detector: " +
      juce::String(pitchDetectorTypeToString(pitchDetectorType)));
  LOG("RMVPE loaded: " +
      juce::String(
          rmvpeModel && rmvpeModel->isLoaded() ? "YES" : "NO"));
  LOG("FCPE loaded: " +
      juce::String(fcpeModel && fcpeModel->isLoaded() ? "YES"
                                                                      : "NO"));

  std::vector<float> extractedF0;
  juce::String pitchInferenceError;
  if (pitchDetectorType == PitchDetectorType::RMVPE)
  {
    const auto modelLock =
        lockModel(models, rmvpeModel, &SharedInferenceModels::rmvpe,
                  &SharedInferenceModels::rmvpeMutex, shuttingDown);
    if (shuttingDown.load())
      return;
    extractedF0 = rmvpeModel->extractF0(samples, numSamples,
                                        audioData.sampleRate);
    pitchInferenceError = rmvpeModel->getLastError();
  }
  else if (pitchDetectorType == PitchDetectorType::FCPE)
  {
    const auto modelLock =
        lockModel(models, fcpeModel, &SharedInferenceModels::fcpe,
                  &SharedInferenceModels::fcpeMutex, shuttingDown);
    if (shuttingDown.load())
      return;
    extractedF0 =
        fcpeModel->extractF0(samples, numSamples, audioData.sampleRate);
    pitchInferenceError = fcpeModel->getLastError();
  }
  else if (pitchDetectorType == PitchDetectorType::SwiftF0)
  {
    const auto modelLock =
        lockModel(models, swiftF0Model, &SharedInferenceModels::swiftF0,
                  &SharedInferenceModels::swiftF0Mutex, shuttingDown);
    if (shuttingDown.load())
      return;
    const auto t0 = std::chrono::steady_clock::now();
    extractedF0 =
        swiftF0Model->extractF0(samples, numSamples, audioData.sampleRate);
    pitchInferenceError = swiftF0Model->getLastError();
    LOG("SwiftF0: " + juce::String(static_cast<int>(extractedF0.size())) +
        " frames in " +
        juce::String(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count(),
                     1) +
        " ms");
  }

  if (shuttingDown.load())
    return;

  // A DirectML session can be created successfully even when its adapter or
  // driver later rejects a graph during Run(). Retry with a fresh CPU session
  // so systems without a usable GPU do not fail with a misleading model-file
  // error. Keep the successful CPU detector for subsequent analyses.
  if (extractedF0.empty() &&
      pitchDetectorType != PitchDetectorType::SwiftF0 &&
      getProviderFromDevice(device) == GPUProvider::DirectML)
  {
    const auto detectorName =
        juce::String(pitchDetectorTypeToString(pitchDetectorType));
    LOG(detectorName + " DirectML inference failed: " +
        (pitchInferenceError.isNotEmpty() ? pitchInferenceError
                                           : "unknown error") +
        "; retrying with CPU");

    juce::String cpuRetryError;
    if (pitchDetectorType == PitchDetectorType::RMVPE)
    {
      auto cpuDetector = std::make_unique<RMVPEPitchDetector>();
      if (cpuDetector->loadModel(rmvpeModelPath, GPUProvider::CPU, 0))
      {
        auto cpuF0 = cpuDetector->extractF0(samples, numSamples,
                                            audioData.sampleRate);
        cpuRetryError = cpuDetector->getLastError();
        if (!cpuF0.empty())
        {
          extractedF0 = std::move(cpuF0);
          rmvpeModel = std::move(cpuDetector);
          const std::lock_guard<std::mutex> lock(modelPtrMutex);
          rmvpePitchDetector = rmvpeModel;
        }
      }
      else
      {
        cpuRetryError = cpuDetector->getLastError();
      }
    }
    else if (pitchDetectorType == PitchDetectorType::FCPE)
    {
      auto cpuDetector = std::make_unique<FCPEPitchDetector>();
      if (cpuDetector->loadModel(fcpeModelPath, melFilterbankPath,
                                 centTablePath, GPUProvider::CPU, 0))
      {
        auto cpuF0 = cpuDetector->extractF0(samples, numSamples,
                                            audioData.sampleRate);
        cpuRetryError = cpuDetector->getLastError();
        if (!cpuF0.empty())
        {
          extractedF0 = std::move(cpuF0);
          fcpeModel = std::move(cpuDetector);
          const std::lock_guard<std::mutex> lock(modelPtrMutex);
          fcpePitchDetector = fcpeModel;
        }
      }
      else
      {
        cpuRetryError = cpuDetector->getLastError();
      }
    }

    if (!extractedF0.empty())
    {
      LOG(detectorName +
          " CPU retry succeeded; using CPU for subsequent inference");
      device = "CPU";
      deviceId = 0;
      if (vocoder)
      {
        vocoder->setExecutionDevice("CPU");
        vocoder->setExecutionDeviceId(0);
        if (vocoder->isLoaded() && !vocoder->reloadModel())
          LOG("Vocoder failed to reload after DirectML-to-CPU fallback");
      }

      // Move the synthesis engine at the same moment. On the CPU provider a
      // vocoder render costs orders of magnitude more than PSOLA, which runs
      // no model and no FFT at all, so leaving the engine behind just makes
      // the user's next edit the slow one. Only reached through the DirectML
      // fallback, which exists on Windows alone - the Core ML path never gets
      // here and keeps the vocoder.
      if (synthesisEngineType == SynthesisEngineType::Vocoder)
      {
        setSynthesisEngineType(SynthesisEngineType::Psola);
        LOG("Synthesis engine switched to PSOLA for CPU inference");
      }

      // GAME will run later in this analysis. Give it a fresh CPU session as
      // well, but keep the existing detector if the optional reload fails.
      if (gameModelDir.isDirectory())
      {
        auto cpuGameDetector = std::make_unique<GAMEDetector>();
        if (cpuGameDetector->loadModels(gameModelDir, GPUProvider::CPU, 0))
        {
          const std::lock_guard<std::mutex> lock(modelPtrMutex);
          gameDetector = std::move(cpuGameDetector);
        }
        else
          LOG("GAME failed to reload after DirectML-to-CPU fallback");
      }
    }
    else
    {
      LOG(detectorName + " CPU retry failed: " +
          (cpuRetryError.isNotEmpty() ? cpuRetryError : "unknown error"));
      if (cpuRetryError.isNotEmpty())
        pitchInferenceError +=
            "\n\n" + TR("error.cpu_retry_failed") + "\n" + cpuRetryError;
    }
  }

  if (shuttingDown.load())
    return;

  if (extractedF0.empty() || targetFrames <= 0)
  {
    const auto detail = pitchInferenceError.isNotEmpty()
                            ? "\n\n" + TR("error.details") + "\n" +
                                  pitchInferenceError.substring(0, 1000)
                            : juce::String();
    juce::MessageManager::callAsync([detail]()
                                    { juce::AlertWindow::showMessageBoxAsync(
                                          juce::AlertWindow::WarningIcon, TR("error.inference_failed"),
                                          TR("error.f0_extraction_failed") + detail); });
    return;
  }

  {
    audioData.rawF0.assign(static_cast<size_t>(targetFrames), 0.0f);
    audioData.voicedMask.assign(static_cast<size_t>(targetFrames), false);

    // FCPE/RMVPE: 160-sample hop at 16 kHz (10 ms); SwiftF0: 256 (16 ms).
    const double neuralFrameTime =
        pitchDetectorType == PitchDetectorType::SwiftF0
            ? SwiftF0PitchDetector::FRAME_TIME
            : 160.0 / 16000.0;
    const double vocoderFrameTime =
        static_cast<double>(HOP_SIZE) /
        static_cast<double>(std::max(1, audioData.sampleRate));
    const int sourceFrameCount = static_cast<int>(extractedF0.size());

    for (int i = 0; i < targetFrames; ++i)
    {
      const double vocoderTime = i * vocoderFrameTime;
      const double neuralFramePos = vocoderTime / neuralFrameTime;
      const int leftIdx = std::clamp(static_cast<int>(std::floor(neuralFramePos)),
                                     0, sourceFrameCount - 1);
      const int rightIdx = std::min(leftIdx + 1, sourceFrameCount - 1);
      const int nearestIdx = std::clamp(
          static_cast<int>(std::llround(neuralFramePos)), 0,
          sourceFrameCount - 1);
      const double frac =
          std::clamp(neuralFramePos - std::floor(neuralFramePos), 0.0, 1.0);

      // Resample the detector's U/V decision independently from pitch. This
      // prevents a nearby positive F0 from turning an originally unvoiced
      // target frame into a voiced one.
      const bool isVoiced = extractedF0[static_cast<size_t>(nearestIdx)] > 0.0f;
      audioData.voicedMask[static_cast<size_t>(i)] = isVoiced;
      if (!isVoiced)
        continue;

      const float leftF0 = extractedF0[static_cast<size_t>(leftIdx)];
      const float rightF0 = extractedF0[static_cast<size_t>(rightIdx)];
      if (leftF0 > 0.0f && rightF0 > 0.0f && leftIdx != rightIdx)
      {
        const float logF0 = static_cast<float>(
            std::log(leftF0) * (1.0 - frac) + std::log(rightF0) * frac);
        audioData.rawF0[static_cast<size_t>(i)] = std::exp(logF0);
      }
      else
      {
        audioData.rawF0[static_cast<size_t>(i)] =
            extractedF0[static_cast<size_t>(nearestIdx)];
      }
    }

    // Compute energy-based VAD mask (captures consonants)
    {
      constexpr float kVadThreshold = 0.008f;
      const float *vadSamples = audioData.waveform.getReadPointer(0);
      const int vadNumSamples = audioData.waveform.getNumSamples();
      const int vadNumFrames = static_cast<int>(audioData.rawF0.size());
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

    // Breath and sibilance frames, from the same audio and voicing. Note
    // segmentation keeps breaths out of notes and the synthesizer keeps both
    // as original audio instead of vocoding them.
    breathsibilance::analyse(audioData, audioData.waveform);

    onProgress(0.325, TR("progress.preparing_pitch"));
    audioData.cleanedF0 =
        F0Smoother::removeOutliers(audioData.rawF0, 1.5f);
    audioData.denseF0 = PitchCurveProcessor::interpolateWithUvMask(
        audioData.cleanedF0, audioData.voicedMask);
    audioData.f0 = audioData.denseF0;
  }

  if (shuttingDown.load())
    return;

  onProgress(0.375, TR("progress.loading_vocoder"));
  auto modelPath = PlatformPaths::getVocoderModelFile();

  if (!modelPath.exists() && !vocoder->isLoaded())
  {
    showMissingModelAndAbort(TR("error.vocoder_model_name"), modelPath);
    return;
  }

  if (modelPath.exists() && !vocoder->isLoaded())
  {
    if (vocoder->loadModel(modelPath))
    {
      switchToClassicIfVocoderOnCpu();
    }
    else
    {
      juce::MessageManager::callAsync([modelPath]()
                                      { juce::AlertWindow::showMessageBoxAsync(
                                            juce::AlertWindow::WarningIcon, TR("error.inference_failed"),
                                            TR("error.vocoder_load_failed") + "\n" +
                                                modelPath.getFullPathName() + "\n\n" +
                                                TR("error.check_model_install")); });
      return;
    }
  }

  if (shuttingDown.load())
    return;

  onProgress(0.50, TR("progress.detecting_notes"));
  segmentIntoNotes(targetProject, nullptr, [&](double progress)
                   { onProgress(0.50 + juce::jlimit(0.0, 1.0, progress) * 0.50,
                                TR("progress.detecting_notes")); });

  if (shuttingDown.load())
    return;

  PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);
  ScaleUtils::detectAndApplyScale(targetProject);

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
    if (shuttingDown.load())
      return;

    juce::MessageManager::callAsync([this, projectCopy, onProjectReady,
                                     onProjectChanged]() {
      if (!project)
        return;

      project->getAudioData().melSpectrogram =
          projectCopy->getAudioData().melSpectrogram;
      project->getAudioData().rawF0 = projectCopy->getAudioData().rawF0;
      project->getAudioData().cleanedF0 =
          projectCopy->getAudioData().cleanedF0;
      project->getAudioData().denseF0 = projectCopy->getAudioData().denseF0;
      project->getAudioData().f0 = projectCopy->getAudioData().f0;
      project->getAudioData().voicedMask =
          projectCopy->getAudioData().voicedMask;
      project->getAudioData().vadMask =
          projectCopy->getAudioData().vadMask;
      project->getAudioData().breathSibilance =
          projectCopy->getAudioData().breathSibilance;
      project->getAudioData().originalWaveform.makeCopyOf(
          projectCopy->getAudioData().originalWaveform);
      project->getAudioData().basePitch =
          projectCopy->getAudioData().basePitch;
      project->getAudioData().deltaPitch =
          projectCopy->getAudioData().deltaPitch;
      project->setAnalysisMode(projectCopy->getAnalysisMode());
      project->setPolyphonicTake(projectCopy->getPolyphonicTake());
      project->setPolyphonicEdits(projectCopy->getPolyphonicEdits());
      project->setPolyphonicPlayback(projectCopy->getPolyphonicPlayback());
      if (onProjectReady)
        onProjectReady(*project);
      if (onProjectChanged)
        onProjectChanged();
    }); });
}

void EditorController::reanalyzeAsync(
    const AnalysisModeState &state, const ProgressCallback &onProgress,
    std::function<void(std::unique_ptr<Project>)> onComplete)
{
  if (isLoadingAudio.load() || !project)
  {
    if (onComplete)
      onComplete(nullptr);
    return;
  }

  cancelLoadingFlag = false;
  isLoadingAudio = true;
  if (loaderThread.joinable())
    loaderThread.join();

  auto copy = std::make_shared<Project>(*project);
  loaderThread = std::thread([this, copy, state, onProgress,
                              onComplete = std::move(onComplete)]() mutable
                             {
    auto &audioData = copy->getAudioData();
    // Drop the edits: the rendered waveform goes back to the original, and
    // the notes and any polyphonic analysis (with its edits) are rebuilt.
    if (audioData.originalWaveform.getNumSamples() > 0)
      audioData.waveform.makeCopyOf(audioData.originalWaveform);
    copy->clearNotes();
    if (const auto take = copy->getPolyphonicTake(); take && take->hasAnalysis())
    {
      auto sourceOnly = std::make_shared<PolyphonicTake>();
      sourceOnly->source.makeCopyOf(take->source);
      sourceOnly->sourceRate = take->sourceRate;
      copy->setPolyphonicTake(std::move(sourceOnly));
    }
    copy->setPolyphonicEdits({});
    copy->setPolyphonicPlayback(nullptr);
    copy->setPendingPolyphonicRestore(nullptr);
    copy->setAnalysisMode(state); // classified, so the classifier is skipped

    bool finished = false;
    analyzeAudio(*copy,
                 [&onProgress](double p, const juce::String &msg) {
                   if (onProgress)
                     onProgress(p, msg);
                 },
                 [&finished] { finished = true; });
    finished = finished && !shuttingDown.load() && !cancelLoadingFlag.load();
    if (finished)
      audioData.originalWaveform.makeCopyOf(audioData.waveform);

    juce::MessageManager::callAsync(
        [this, copy, finished, onComplete = std::move(onComplete)]() {
          isLoadingAudio = false;
          if (onComplete)
            onComplete(finished ? std::make_unique<Project>(std::move(*copy))
                                : nullptr);
        }); });
}

namespace
{
// Past a lengthened note's new end: the analysis window's half and room for
// the release to ring out.
constexpr double kRenderTailSeconds = 0.25;

/**
 * The take's channels for the engine, padded with silence when an edit makes
 * a note end after the recording does (a note lengthened, or moved, past the
 * end): the render, and with it the take, is then that much longer. Without
 * such an edit the channels are the take's own length.
 */
std::vector<std::vector<float>> renderChannels(const PolyphonicTake &take,
                                               const Project::PolyphonicEdits &edits)
{
  const auto &source = take.source;
  const auto &analysis = *take.analysis;
  const int numChannels = source.getNumChannels();
  const int numSamples = source.getNumSamples();
  const double rate = analysis.params.sampleRate;

  int length = numSamples;
  for (const auto &[index, edit] : edits)
  {
    if (edit.mute || index < 0 || index >= static_cast<int>(analysis.notes.size()))
      continue;
    const auto &note = analysis.notes[static_cast<size_t>(index)];
    const auto map = polyphonic::stretchMap(analysis, index, edit);
    const double lastOut = map.cuts() ? map.end + 1.0 : map.outputFrame(note.endFrame + 1.0);
    const double endSeconds =
        analysis.frameToSeconds(lastOut) + edit.timeOffsetSeconds + kRenderTailSeconds;
    length = std::max(length, static_cast<int>(std::ceil(endSeconds * rate)));
  }

  std::vector<std::vector<float>> channels(static_cast<size_t>(numChannels));
  for (int ch = 0; ch < numChannels; ++ch)
  {
    auto &channel = channels[static_cast<size_t>(ch)];
    channel.assign(static_cast<size_t>(length), 0.0f);
    std::copy(source.getReadPointer(ch), source.getReadPointer(ch) + numSamples,
              channel.begin());
  }
  return channels;
}

// The listed notes on their own with `requested` edits (the engine's default
// for a note without one, which brings its background along as the full
// render does), mixed to mono and trimmed to the notes themselves: from just
// before the earliest onset to a little after the latest end, 5 ms fades at
// the cuts. `startSeconds` receives the take time of its first sample.
juce::AudioBuffer<float> renderAuditionNotes(const PolyphonicTake &take,
                                             const std::vector<int> &notes,
                                             const Project::PolyphonicEdits &requested,
                                             const std::atomic<bool> *cancel,
                                             double &startSeconds)
{
  juce::AudioBuffer<float> mono;
  startSeconds = 0.0;
  Project::PolyphonicEdits edits;
  for (int note : notes)
  {
    const auto it = requested.find(note);
    edits[note] = it != requested.end() ? it->second : polyphonic::NoteEdit{};
  }
  const auto channels = renderChannels(take, edits);
  const auto &analysis = *take.analysis;
  const auto span = polyphonic::renderNotes(analysis, channels, notes, edits, cancel);
  const int spanLength =
      span.channels.empty() ? 0 : static_cast<int>(span.channels.front().size());
  // The span also covers the analysis window's lead and the background
  // claim's 2 s fade, which would put ~100 ms of near-silence before the
  // attack and a long gap in the loop.
  const double rate = analysis.params.sampleRate;
  double first = std::numeric_limits<double>::max();
  double last = 0.0;
  for (const auto &[index, edit] : edits)
  {
    const auto &note = analysis.notes[static_cast<size_t>(index)];
    const auto map = polyphonic::stretchMap(analysis, index, edit);
    const double lastOut = map.cuts() ? map.end + 1.0 : map.outputFrame(note.endFrame + 1.0);
    first = std::min(first, analysis.frameToSeconds(note.startFrame) + edit.timeOffsetSeconds);
    last = std::max(last, analysis.frameToSeconds(lastOut) + edit.timeOffsetSeconds);
  }
  constexpr double kLeadSeconds = 0.03; // detected onsets can be a frame late
  constexpr double kTailSeconds = 0.3;
  const int from = juce::jlimit(0, spanLength,
                                static_cast<int>((first - kLeadSeconds) * rate) - span.start);
  const int to = juce::jlimit(from, spanLength,
                              static_cast<int>(std::ceil((last + kTailSeconds) * rate)) - span.start);
  const int length = to - from;
  if ((cancel != nullptr && cancel->load()) || length <= 0)
    return mono;
  mono.setSize(1, length);
  mono.clear();
  const float gain = 1.0f / static_cast<float>(span.channels.size());
  for (const auto &channel : span.channels)
    juce::FloatVectorOperations::addWithMultiply(mono.getWritePointer(0),
                                                 channel.data() + from, gain, length);
  // Fade the cut edges (5 ms) so the loop does not click.
  const int fade = std::min(length / 2, static_cast<int>(0.005 * rate));
  mono.applyGainRamp(0, fade, 0.0f, 1.0f);
  mono.applyGainRamp(length - fade, fade, 1.0f, 0.0f);
  startSeconds = static_cast<double>(span.start + from) / rate;
  return mono;
}
} // namespace

std::shared_ptr<const PolyphonicTake>
EditorController::restorePolyphonicTake(const PolyphonicRestore &restore,
                                        juce::AudioBuffer<float> source, double sourceRate,
                                        const std::atomic<bool> *cancel)
{
  if (source.getNumSamples() == 0 || sourceRate <= 0.0 || (cancel && cancel->load()))
    return nullptr;
  if (source.getNumChannels() > 2)
    source.setSize(2, source.getNumSamples(), true);
  if (std::abs(sourceRate - static_cast<double>(SAMPLE_RATE)) > 1e-6)
    source = AudioResampler::resample(source, sourceRate, static_cast<double>(SAMPLE_RATE));
  const int numSamples = source.getNumSamples();
  if (numSamples == 0)
    return nullptr;
  std::vector<float> mono(static_cast<size_t>(numSamples), 0.0f);
  const float channelGain = 1.0f / static_cast<float>(source.getNumChannels());
  for (int ch = 0; ch < source.getNumChannels(); ++ch)
    juce::FloatVectorOperations::addWithMultiply(mono.data(), source.getReadPointer(ch),
                                                 channelGain, numSamples);
  const auto t0 = std::chrono::steady_clock::now();
  auto analysis = polyphonic::restore(mono.data(), numSamples, restore.params,
                                      restore.notes, cancel);
  if (cancel && cancel->load())
    return nullptr;
  auto take = std::make_shared<PolyphonicTake>();
  take->analysis = std::make_shared<const polyphonic::Analysis>(std::move(analysis));
  take->source = std::move(source);
  take->sourceRate = static_cast<double>(SAMPLE_RATE);
  LOG("Polyphonic restore: " + juce::String(static_cast<int>(restore.notes.size())) +
      " notes, " +
      juce::String(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), 1) +
      " s");
  juce::ignoreUnused(t0);
  return take;
}

void EditorController::restorePolyphonicAsync(
    std::function<void(std::shared_ptr<const PolyphonicTake>)> onComplete)
{
  const auto pending = project ? project->getPendingPolyphonicRestore() : nullptr;
  if (isLoadingAudio.load() || !pending)
  {
    if (onComplete)
      onComplete(nullptr);
    return;
  }

  // What the job needs, copied here: the project itself stays with the UI and
  // can be edited while the masks are rebuilt.
  const auto file = project->getFilePath();
  const auto savedSha = project->getAudioSha256();
  const auto &audioData = project->getAudioData();
  auto fallback = std::make_shared<juce::AudioBuffer<float>>();
  fallback->makeCopyOf(audioData.originalWaveform.getNumSamples() > 0 ? audioData.originalWaveform
                                                                      : audioData.waveform);
  const double fallbackRate = static_cast<double>(audioData.sampleRate);

  cancelLoadingFlag = false;
  isLoadingAudio = true;
  if (loaderThread.joinable())
    loaderThread.join();

  loaderThread = std::thread([this, pending, file, savedSha, fallback, fallbackRate,
                              onComplete = std::move(onComplete)]() mutable
                             {
    std::shared_ptr<const PolyphonicTake> restored;
    try
    {
      // The take's own channels: the audio file when it is the one the
      // project was made from, else the saved original (mono) waveform.
      juce::AudioBuffer<float> source;
      double sourceRate = 0.0;
      if (file.existsAsFile() &&
          (savedSha.isEmpty() || savedSha.equalsIgnoreCase(SHA256Utils::fileSHA256(file))))
      {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader{formats.createReaderFor(file)})
        {
          const int channels = static_cast<int>(std::min<unsigned int>(2, reader->numChannels));
          const int samples = static_cast<int>(reader->lengthInSamples);
          source.setSize(channels, samples);
          reader->read(&source, 0, samples, 0, true, channels > 1);
          sourceRate = reader->sampleRate;
        }
      }
      if (source.getNumSamples() == 0)
      {
        source = std::move(*fallback);
        sourceRate = fallbackRate;
        LOG("Polyphonic restore: audio file missing or changed; using the saved mono waveform");
      }
      if (!cancelLoadingFlag.load() && !shuttingDown.load())
        restored = restorePolyphonicTake(*pending, std::move(source), sourceRate,
                                         &cancelLoadingFlag);
      if (shuttingDown.load())
        restored = nullptr;
    }
    catch (const std::exception &e)
    {
      reportAnalysisException("restorePolyphonic", e.what());
    }
    catch (...)
    {
      reportAnalysisException("restorePolyphonic", "unknown error");
    }

    juce::MessageManager::callAsync(
        [this, restored, onComplete = std::move(onComplete)]() {
          isLoadingAudio = false;
          if (onComplete)
            onComplete(restored);
        }); });
}

void EditorController::renderPolyphonicAsync(std::function<void(bool)> onComplete)
{
  polyRenderOnComplete = std::move(onComplete);
  if (polyRenderRunning)
  {
    // Superseded: stop the render in flight and run again when it returns.
    polyRenderRerun = true;
    if (polyRenderCancel)
      polyRenderCancel->store(true);
    return;
  }
  startPolyphonicRender();
}

void EditorController::startPolyphonicRender()
{
  polyRenderRerun = false;
  const auto take = project ? project->getPolyphonicTake() : nullptr;
  // A restoring take has no masks yet: nothing to render with (the caller
  // renders again once the restore lands).
  if (!take || !take->hasAnalysis() || take->restoring || take->source.getNumSamples() == 0 ||
      shuttingDown.load())
  {
    if (auto done = polyRenderOnComplete)
      done(false);
    return;
  }

  if (polyRenderThread.joinable())
    polyRenderThread.join(); // finished: it posted its result before returning

  auto edits = project->getPolyphonicEdits();
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  polyRenderCancel = cancel;
  polyRenderRunning = true;
  polyRenderThread = std::thread([this, take, edits = std::move(edits), cancel]()
                                 {
    std::shared_ptr<const juce::AudioBuffer<float>> result;
    try
    {
      const auto &source = take->source;
      auto buffer = std::make_shared<juce::AudioBuffer<float>>();
      if (edits.empty())
      {
        buffer->makeCopyOf(source);
      }
      else
      {
        // Possibly longer than the take (renderChannels).
        const auto channels = renderChannels(*take, edits);
        const auto out = polyphonic::render(*take->analysis, channels, edits, cancel.get());
        const int numChannels = static_cast<int>(out.size());
        const int length = numChannels > 0 ? static_cast<int>(out.front().size()) : 0;
        buffer->setSize(numChannels, length);
        for (int ch = 0; ch < numChannels; ++ch)
          buffer->copyFrom(ch, 0, out[static_cast<size_t>(ch)].data(), length);
      }
      if (!cancel->load() && !shuttingDown.load())
        result = std::move(buffer);
    }
    catch (const std::exception &e)
    {
      LOG("Polyphonic render failed: " + juce::String(e.what()));
    }
    catch (...)
    {
      LOG("Polyphonic render failed");
    }
    juce::MessageManager::callAsync([this, take, result]() {
      finishPolyphonicRender(take, result);
    }); });
}

void EditorController::finishPolyphonicRender(
    std::shared_ptr<const PolyphonicTake> take,
    std::shared_ptr<const juce::AudioBuffer<float>> rendered)
{
  polyRenderRunning = false;
  if (shuttingDown.load())
    return;
  if (polyRenderRerun)
  {
    startPolyphonicRender();
    return;
  }

  // A render made for a take that has since been re-analysed is stale.
  const bool current = project && project->getPolyphonicTake() == take;
  if (rendered && current)
  {
    project->setPolyphonicPlayback(rendered);
    // The waveform is the playback's mono mix, as after analysis, so the
    // display shows the edited audio.
    auto &waveform = project->getAudioData().waveform;
    const int numSamples = rendered->getNumSamples();
    const int numChannels = rendered->getNumChannels();
    waveform.setSize(1, numSamples, false, false, true);
    waveform.clear();
    for (int ch = 0; ch < numChannels; ++ch)
      waveform.addFrom(0, 0, *rendered, ch, 0, numSamples,
                       1.0f / static_cast<float>(numChannels));
  }
  if (auto done = polyRenderOnComplete)
    done(rendered != nullptr && current);
}

void EditorController::renderPolyphonicAuditionAsync(
    std::vector<int> notes, Project::PolyphonicEdits edits,
    std::function<void(juce::AudioBuffer<float>)> onReady)
{
  AuditionRequest request{std::move(notes), std::move(edits), std::move(onReady)};
  if (auditionRunning)
  {
    // The render in flight finishes and plays (a drag keeps sounding while it
    // moves); only the newest request waits for it, older waiting ones are
    // dropped.
    pendingAudition = std::move(request);
    return;
  }
  startPolyphonicAudition(std::move(request));
}

void EditorController::prerenderPolyphonicNotesAsync(
    std::shared_ptr<const PolyphonicTake> take, Project::PolyphonicEdits edits,
    std::vector<int> notes,
    std::function<void(int, juce::AudioBuffer<float>, double)> onNote)
{
  cancelPolyphonicPrerender();
  if (take == nullptr || !take->hasAnalysis() || take->restoring || notes.empty() ||
      shuttingDown.load())
    return;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  prerenderCancel = cancel;
  const int generation = ++prerenderGeneration;
  prerenderThread = std::thread([this, take, edits = std::move(edits),
                                 notes = std::move(notes), onNote, cancel, generation]() {
    // One note at a time, each delivered as it is done.
    for (const int note : notes)
    {
      if (cancel->load() || shuttingDown.load())
        return;
      double startSeconds = 0.0;
      juce::AudioBuffer<float> buffer;
      try
      {
        buffer = renderAuditionNotes(*take, {note}, edits, cancel.get(), startSeconds);
      }
      catch (...)
      {
        continue;
      }
      if (buffer.getNumSamples() == 0 || cancel->load())
        continue;
      juce::MessageManager::callAsync(
          [this, note, buffer = std::move(buffer), startSeconds, onNote, generation]() mutable {
            if (!shuttingDown.load() && generation == prerenderGeneration && onNote)
              onNote(note, std::move(buffer), startSeconds);
          });
    }
  });
}

void EditorController::cancelPolyphonicPrerender()
{
  ++prerenderGeneration;
  if (prerenderCancel)
    prerenderCancel->store(true);
  if (prerenderThread.joinable())
    prerenderThread.join(); // a note at a time: returns within one note's render
}

void EditorController::cancelPolyphonicAudition()
{
  ++auditionGeneration;
  pendingAudition.reset();
  if (auditionCancel)
    auditionCancel->store(true);
}

void EditorController::startPolyphonicAudition(AuditionRequest request)
{
  const auto take = project ? project->getPolyphonicTake() : nullptr;
  if (!take || !take->hasAnalysis() || take->restoring || request.notes.empty() ||
      shuttingDown.load())
    return;
  if (auditionThread.joinable())
    auditionThread.join(); // finished: it posted its result before returning

  auto cancel = std::make_shared<std::atomic<bool>>(false);
  auditionCancel = cancel;
  auditionRunning = true;
  const int generation = auditionGeneration;
  auditionThread = std::thread([this, take, request = std::move(request), cancel,
                                generation]() mutable
                               {
    juce::AudioBuffer<float> mono;
    try
    {
      double startSeconds = 0.0;
      mono = renderAuditionNotes(*take, request.notes, request.edits, cancel.get(),
                                 startSeconds);
    }
    catch (...)
    {
      LOG("Polyphonic audition render failed");
    }
    juce::MessageManager::callAsync(
        [this, mono = std::move(mono), onReady = std::move(request.onReady),
         generation]() mutable {
          auditionRunning = false;
          if (shuttingDown.load())
            return;
          if (pendingAudition)
          {
            auto next = std::move(*pendingAudition);
            pendingAudition.reset();
            startPolyphonicAudition(std::move(next));
            return;
          }
          if (generation == auditionGeneration && mono.getNumSamples() > 0 && onReady)
            onReady(std::move(mono));
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
                                        std::function<void()> onStreamingUpdate,
                                        std::function<void(double)> onProgress)
{
  try
  {
    segmentIntoNotesImpl(targetProject, std::move(onStreamingUpdate),
                         std::move(onProgress));
  }
  catch (const std::exception &e)
  {
    reportAnalysisException("segmentIntoNotes", e.what());
  }
  catch (...)
  {
    reportAnalysisException("segmentIntoNotes", "unknown error");
  }
}

void EditorController::segmentIntoNotesImpl(Project &targetProject,
                                            std::function<void()> onStreamingUpdate,
                                            std::function<void(double)> onProgress)
{
  auto &audioData = targetProject.getAudioData();
  auto &notes = targetProject.getNotes();
  notes.clear();
  audioData.segmentChunkRanges.clear();
  audioData.segmentDebugChunks.clear();

  if (audioData.f0.empty())
    return;

  std::shared_ptr<SharedInferenceModels> models;
  std::shared_ptr<GAMEDetector> game;
  {
    const std::lock_guard<std::mutex> lock(modelPtrMutex);
    models = sharedModels;
    game = gameDetector;
  }

  if (!game || !game->isLoaded())
  {
    auto searchedPath = gameModelDir.getFullPathName();
    auto bundlePath = PlatformPaths::getModelsDirectory().getChildFile("GAME").getFullPathName();

    juce::String detail = TR("error.game_models_missing") + "\n\n"
                                                            "Searched path: " +
                          searchedPath + "\n"
                                         "Bundle path: " +
                          bundlePath + "\n"
                                       "gameDetector: " +
                          juce::String(game ? "created" : "null") + "\n"
                                                                            "isLoaded: " +
                          juce::String(game ? (game->isLoaded() ? "true" : "false") : "N/A") + "\n"
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

  if (game && game->isLoaded() &&
      audioData.waveform.getNumSamples() > 0)
  {

    const float *samples = audioData.waveform.getReadPointer(0);
    int numSamples = audioData.waveform.getNumSamples();
    const int f0Size = static_cast<int>(audioData.f0.size());

    auto gameLock = lockModel(models, game, &SharedInferenceModels::game,
                              &SharedInferenceModels::gameMutex, shuttingDown);
    if (shuttingDown.load())
      return;
    auto gameNotes = game->detectNotesWithProgress(
        samples, numSamples, GAMEDetector::SAMPLE_RATE, onProgress);
    // Copied under the lock: the ranges are this call's detector scratch state.
    auto slicerChunks = game->getLastChunkRanges();
    juce::String gameError = gameNotes.empty() ? game->getLastError() : juce::String();
    if (gameLock.owns_lock())
      gameLock.unlock();

    // Like the pitch detectors, GAME can create GPU sessions successfully and
    // then fail inside Run() on some adapters/drivers (seen with DirectML).
    // Retry once on a fresh CPU session and keep it for later analyses.
    if (gameError.isNotEmpty())
    {
      if (getProviderFromDevice(device) != GPUProvider::CPU &&
          gameModelDir.isDirectory() && !shuttingDown.load())
      {
        LOG("GAME " + device + " inference failed: " + gameError +
            "; retrying with CPU");
        // Private detector: no shared lock, so other tracks are not blocked
        // for the whole CPU pass.
        auto cpuGameDetector = std::make_shared<GAMEDetector>();
        if (cpuGameDetector->loadModels(gameModelDir, GPUProvider::CPU, 0))
        {
          gameNotes = cpuGameDetector->detectNotesWithProgress(
              samples, numSamples, GAMEDetector::SAMPLE_RATE, onProgress);
          if (cpuGameDetector->getLastError().isEmpty())
          {
            LOG("GAME CPU retry succeeded; using CPU for note detection");
            slicerChunks = cpuGameDetector->getLastChunkRanges();
            {
              const std::lock_guard<std::mutex> lock(modelPtrMutex);
              gameDetector = cpuGameDetector;
            }
            game = std::move(cpuGameDetector);
            gameError.clear();
          }
          else
          {
            gameError += "\n\n" + TR("error.cpu_retry_failed") + "\n" +
                         cpuGameDetector->getLastError();
          }
        }
        else
        {
          LOG("GAME failed to load on CPU for retry");
        }
      }

      if (gameError.isNotEmpty())
      {
        LOG("GAME note detection failed: " + gameError);
        const auto detail = TR("error.details") + "\n" + gameError.substring(0, 1000);
        juce::MessageManager::callAsync([detail]()
                                        { juce::AlertWindow::showMessageBoxAsync(
                                              juce::AlertWindow::WarningIcon, TR("error.game_error"), detail); });
        return;
      }
    }

    // Build debug chunks from actual GAME slicer chunk ranges
    {
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

    // Note edges around the vowels: a consonant joins the note it leads
    // into, a word-final one before a pause or breath stays with its note,
    // and breaths stay out of notes (see NoteEdges.h for the rules).
    if (!notes.empty() && f0Size > 0)
    {
      std::sort(notes.begin(), notes.end(),
                [](const Note &a, const Note &b)
                { return a.getStartFrame() < b.getStartFrame(); });
      std::vector<noteedges::Span> spans;
      spans.reserve(notes.size());
      for (const auto &note : notes)
        spans.push_back({note.getStartFrame(), note.getEndFrame()});
      spans = noteedges::attachConsonants(
          std::move(spans), audioData.voicedMask, audioData.vadMask,
          audioData.breathSibilance, f0Size);

      // A note on a sibilant alone joins its neighbour in the region.
      const auto merged = noteedges::mergeSibilantOnly(
          spans, audioData.voicedMask, audioData.breathSibilance,
          audioData.segmentChunkRanges);

      std::vector<Note> kept;
      kept.reserve(notes.size());
      for (size_t i = 0; i < notes.size(); ++i)
      {
        if (!merged.kept[i])
          continue;
        auto note = notes[i];
        const int newStart = std::clamp(merged.spans[i].start, 0, f0Size - 1);
        const int newEnd = std::clamp(merged.spans[i].end, newStart + 1, f0Size);
        note.setStartFrame(newStart);
        note.setEndFrame(newEnd);
        note.setSrcStartFrame(newStart);
        note.setSrcEndFrame(newEnd);
        note.setF0Values(std::vector<float>(audioData.f0.begin() + newStart,
                                            audioData.f0.begin() + newEnd));
        kept.push_back(std::move(note));
      }
      notes = std::move(kept);
    }

    const auto isBreathFrame = [&audioData](int frame)
    {
      return breathsibilance::labelAt(audioData.breathSibilance, frame) ==
             breathsibilance::Breath;
    };

    // A note GAME placed on unvoiced audio that is a third or more breath is
    // not a sung note: drop it.
    if (!audioData.breathSibilance.empty())
    {
      notes.erase(
          std::remove_if(
              notes.begin(), notes.end(),
              [&](const Note &note)
              {
                int breathFrames = 0;
                for (int i = note.getStartFrame(); i < note.getEndFrame(); ++i)
                {
                  if (i < static_cast<int>(audioData.voicedMask.size()) &&
                      audioData.voicedMask[static_cast<size_t>(i)])
                    return false;
                  breathFrames += isBreathFrame(i) ? 1 : 0;
                }
                return breathFrames * 3 >= note.getDurationFrames();
              }),
          notes.end());
    }

    for (auto& note : notes)
      NoteAmplitude::update(note, audioData);

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

  for (auto& note : notes)
    NoteAmplitude::update(note, audioData);

  if (!audioData.f0.empty())
    PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);
}
