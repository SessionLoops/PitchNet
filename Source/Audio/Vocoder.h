#pragma once

#include "../JuceHeader.h"
#if __APPLE__
#include "CoreMLVocoderBackend.h"
#endif
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

/**
 * PC-NSF-HiFiGAN Vocoder wrapper using Core ML on macOS and ONNX Runtime elsewhere.
 * Converts mel spectrogram + F0 to waveform with pitch control.
 */
class Vocoder
{
public:
  Vocoder();
  ~Vocoder();

  /**
   * Load vocoder model from ONNX file.
   * @param modelPath Path to .onnx model file
   * @return true if successful
   */
  bool loadModel(const juce::File &modelPath);

  /**
   * Check if model is loaded.
   */
  bool isLoaded() const { return loaded; }

  /**
   * Check if ONNX Runtime is available.
   */
  static bool isOnnxRuntimeAvailable();

  /**
   * Synthesize waveform from mel spectrogram and F0.
   * @param mel Mel spectrogram [T, NUM_MELS] (T frames, each with NUM_MELS
   * values)
   * @param f0 F0 values [T] (fundamental frequency per frame)
   * @return Synthesized waveform, or empty vector on failure
   */
  std::vector<float> infer(const std::vector<std::vector<float>> &mel,
                           const std::vector<float> &f0);

  /**
   * Synthesize with pitch shift.
   * @param mel Mel spectrogram
   * @param f0 F0 values
   * @param pitchShiftSemitones Pitch shift in semitones (+12 = one octave up)
   * @return Synthesized waveform
   */
  std::vector<float>
  inferWithPitchShift(const std::vector<std::vector<float>> &mel,
                      const std::vector<float> &f0, float pitchShiftSemitones);

  /**
   * Asynchronous inference with callback.
   * @param mel Mel spectrogram
   * @param f0 F0 values
   * @param callback Called with result on completion
   */
  void inferAsync(std::vector<std::vector<float>> mel,
                  std::vector<float> f0,
                  std::function<void(std::vector<float>)> callback,
                  std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  // Model parameters
  int getSampleRate() const { return sampleRate; }
  int getHopSize() const { return hopSize; }
  int getNumMels() const { return numMels; }
  bool isPitchControllable() const { return pitchControllable; }

  // Device settings
  void setExecutionDevice(const juce::String &device);
  juce::String getExecutionDevice() const { return executionDevice; }
  void setExecutionDeviceId(int deviceId);
  int getExecutionDeviceId() const { return executionDeviceId; }

  // Reload model with new settings (call after changing device)
  bool reloadModel();

private:
  struct AsyncTask
  {
    std::vector<std::vector<float>> mel;
    std::vector<float> f0;
    std::function<void(std::vector<float>)> callback;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
  };

  bool loaded = false;
  int sampleRate = 44100;
  int hopSize = 512;
  int numMels = 128;
  bool pitchControllable = true;

#ifdef USE_DIRECTML
  juce::String executionDevice = "DirectML";
#elif defined(USE_CUDA)
  juce::String executionDevice = "CUDA";
#else
  juce::String executionDevice = "CPU";
#endif

  juce::File modelFile;
#if __APPLE__
  juce::File coreMLModelFile;
  std::unique_ptr<CoreMLVocoderBackend> coreMLBackend;
  bool usingCoreMLModel = false;
#endif
  std::unique_ptr<std::ofstream> logFile;
  int executionDeviceId = 0;

  // Thread safety for async operations
  std::atomic<bool> isShuttingDown{false};
  std::atomic<int> activeAsyncTasks{0};
  std::mutex asyncMutex;
  std::condition_variable asyncCondition;
  std::thread asyncWorker;
  std::deque<AsyncTask> asyncQueue;

  // Mutex to protect ONNX session access during inference
  mutable std::mutex inferenceMutex;

  void log(const std::string &message);

#ifdef HAVE_ONNXRUNTIME
  std::unique_ptr<Ort::Env> onnxEnv;
  std::unique_ptr<Ort::Session> onnxSession;
  std::unique_ptr<Ort::IoBinding> ioBinding;
  std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;

  // Input/output names (cached)
  std::vector<const char *> inputNames;
  std::vector<const char *> outputNames;
  std::vector<std::string> inputNameStrings;
  std::vector<std::string> outputNameStrings;

  // Reused per-inference buffers to avoid frequent heap allocations.
  std::vector<float> melScratch;
  std::vector<float> f0Scratch;
  std::vector<int64_t> melShapeScratch;
  std::vector<int64_t> f0ShapeScratch;
  std::vector<Ort::Value> inputTensorScratch;
  std::vector<Ort::Value> outputTensorScratch;

  // Single-chunk ONNX inference (caller must hold inferenceMutex).
  // Returns empty vector on failure.
  std::vector<float> inferChunkLocked(const std::vector<std::vector<float>> &mel,
                                      const std::vector<float> &f0,
                                      size_t numFrames);

  // Create session options based on current settings
  Ort::SessionOptions createSessionOptions();
#endif

#if __APPLE__
  std::vector<float>
  inferCoreMLChunkLocked(const std::vector<std::vector<float>> &mel,
                         const std::vector<float> &f0,
                         size_t numFrames);
#endif

  /**
   * Generate simple sine wave fallback when ONNX is not available.
   */
  std::vector<float> generateSineFallback(const std::vector<float> &f0);

  juce::File resolvePreferredModelFile(const juce::File &requestedModelPath) const;
};
