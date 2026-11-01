#pragma once

#include "../JuceHeader.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

/**
 * SwiftF0 pitch detector (https://github.com/lars76/swift-f0, MIT).
 *
 * A ~135 KB STFT + CNN model that runs on the CPU in a few milliseconds per
 * second of audio, so it never uses a GPU provider. Ports the batch path of
 * swift_f0.core.SwiftF0.detect():
 *   - mono audio at 16 kHz, one frame per 256 samples (16 ms)
 *   - long audio is processed in 1875-frame windows with 11 frames of left
 *     context and 10 frames of look-ahead, which reproduces a single pass
 *   - frames whose audio peaks below 1e-3 get confidence 0
 *   - a frame is voiced when its confidence >= 0.5 (calibrated threshold)
 */
class SwiftF0PitchDetector
{
public:
  static constexpr int SWIFT_SAMPLE_RATE = 16000;
  static constexpr int HOP_SIZE = 256;
  static constexpr float F0_MIN = 46.875f;
  static constexpr float F0_MAX = 2093.75f;
  static constexpr float DEFAULT_THRESHOLD = 0.5f;

  // Seconds per output frame.
  static constexpr double FRAME_TIME =
      static_cast<double>(HOP_SIZE) / static_cast<double>(SWIFT_SAMPLE_RATE);

  SwiftF0PitchDetector();
  ~SwiftF0PitchDetector();

  /** Load swift_f0.onnx. Always uses the CPU execution provider. */
  bool loadModel(const juce::File &modelPath);

  bool isLoaded() const { return loaded; }
  juce::String getLastError() const { return lastError; }

  /**
   * Extract F0 (Hz, 0 for unvoiced) at FRAME_TIME spacing.
   * Audio is resampled to 16 kHz internally.
   */
  std::vector<float> extractF0(const float *audio, int numSamples,
                               int sampleRate,
                               float threshold = DEFAULT_THRESHOLD,
                               float fmin = F0_MIN, float fmax = F0_MAX);

  /** Per-frame confidence from the most recent extractF0() call. */
  const std::vector<float> &getLastConfidence() const { return lastConfidence; }

private:
  bool loaded = false;
  juce::String lastError;
  std::vector<float> lastConfidence;

  std::vector<float> resampleTo16k(const float *audio, int numSamples,
                                   int srcRate) const;

#ifdef HAVE_ONNXRUNTIME
  std::unique_ptr<Ort::Env> onnxEnv;
  std::unique_ptr<Ort::Session> onnxSession;

  // Runs the model on one window; fills pitch/confidence (frames of window).
  bool runWindow(const float *samples, int numSamples, float fmin, float fmax,
                 std::vector<float> &pitch, std::vector<float> &confidence);
#endif
};
