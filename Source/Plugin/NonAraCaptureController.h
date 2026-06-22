#pragma once

#include "../JuceHeader.h"
#include <atomic>

class NonAraCaptureController {
public:
  enum class State { Idle, WaitingForAudio, Capturing, Complete };

  struct FinalizeResult {
    int numChannels = 0;
    int numSamples = 0;
    double sampleRate = 44100.0;
  };

  void prepare(double sampleRate, int numChannels, int maxCaptureSeconds);

  // Called from audio thread
  void resetToWaiting();

  // Called from audio thread
  void processBlock(const juce::AudioBuffer<float> &input, bool hostIsPlaying);

  // Called from audio thread
  bool shouldFinalize() const { return shouldFinalizeFlag.load(); }

  // Called from audio thread
  bool finalizeCapture(double hostSampleRate, FinalizeResult &out);

  // Called from message thread
  juce::AudioBuffer<float> copyCapturedAudio(int numSamples) const;
  juce::AudioBuffer<float> copyCapturedAudioRange(int startSample,
                                                  int numSamples) const;
  int getCapturedSampleCount() const { return publishedCapturePosition.load(); }

  // Called from message thread after the captured audio has been copied out and
  // dispatched for analysis.
  void onAnalysisDispatched();

  // Called from audio thread (or message thread when safe) to stop capturing.
  void stop();

  bool isAnalysisPending() const { return analysisPending.load(); }
  State getState() const { return state.load(); }

  void setAudioThreshold(float t) { audioThreshold = t; }
  void setMinCaptureSeconds(double seconds) { minCaptureSeconds = seconds; }

private:
  std::atomic<State> state{State::Idle};

  mutable juce::SpinLock bufferLock;

  juce::AudioBuffer<float> captureBuffer;
  int capturePosition = 0;
  std::atomic<int> publishedCapturePosition{0};
  int stopDebounceBlocks = 0;
  int finalLength = 0;

  std::atomic<bool> analysisPending{false};
  std::atomic<bool> shouldFinalizeFlag{false};

  float audioThreshold = 0.001f;
  double minCaptureSeconds = 0.5;

  static constexpr int kStopDebounceBlocks = 3;
};
