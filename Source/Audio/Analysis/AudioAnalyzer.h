#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Utils/Constants.h"
#include "../../Utils/F0Smoother.h"
#include "../../Utils/MelSpectrogram.h"
#include "../../Utils/PitchCurveProcessor.h"
#include "../FCPEPitchDetector.h"
#include "../PitchDetectorType.h"
#include "../RMVPEPitchDetector.h"
#include "../GAMEDetector.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

/**
 * Coordinates audio analysis operations including:
 * - Mel spectrogram computation
 * - F0 (pitch) extraction using RMVPE or FCPE
 * - F0 smoothing and interpolation
 * - Note segmentation using GAME model
 */
class AudioAnalyzer
{
public:
  using ProgressCallback =
      std::function<void(double progress, const juce::String &message)>;
  using CompleteCallback = std::function<void()>;

  AudioAnalyzer();
  ~AudioAnalyzer();

  // Initialize detectors
  void initialize();

  // Check if FCPE is available and should be used
  bool isFCPEAvailable() const;
  void setUseFCPE(bool use) { useFCPE = use; }
  bool getUseFCPE() const { return useFCPE; }

  // Check if RMVPE is available
  bool isRMVPEAvailable() const;

  // Set pitch detector type
  void setPitchDetectorType(PitchDetectorType type) { detectorType = type; }
  PitchDetectorType getPitchDetectorType() const { return detectorType; }

  // Main analysis function - runs synchronously (call from background thread)
  void analyze(Project &project, ProgressCallback onProgress,
               CompleteCallback onComplete = nullptr);

  // Async wrapper - spawns background thread
  void analyzeAsync(std::shared_ptr<Project> project,
                    ProgressCallback onProgress, CompleteCallback onComplete);

  // Note segmentation
  void segmentIntoNotes(Project &project);

  // Cancel ongoing analysis
  void cancel() { cancelFlag = true; }
  bool isAnalyzing() const { return isRunning.load(); }

  // Access to detectors for configuration
  FCPEPitchDetector *getFCPEDetector()
  {
    return fcpeDetector ? fcpeDetector.get() : externalFCPEDetector;
  }
  RMVPEPitchDetector *getRMVPEDetector()
  {
    return rmvpeDetector ? rmvpeDetector.get() : externalRMVPEDetector;
  }
  GAMEDetector *getGAMEDetector()
  {
    return gameDetector ? gameDetector.get() : externalGAMEDetector;
  }

  // Set external detectors (optional - if not set, internal ones are used)
  void setFCPEDetector(FCPEPitchDetector *detector)
  {
    externalFCPEDetector = detector;
  }
  void setRMVPEDetector(RMVPEPitchDetector *detector)
  {
    externalRMVPEDetector = detector;
  }
  void setGAMEDetector(GAMEDetector *detector)
  {
    externalGAMEDetector = detector;
  }

private:
  // Extract F0 using RMVPE
  void extractF0WithRMVPE(AudioData &audioData, int targetFrames);

  // Extract F0 using FCPE
  void extractF0WithFCPE(AudioData &audioData, int targetFrames);

  // Segment notes using GAME model
  void segmentWithGAME(Project &project);

  // Fallback segmentation based on F0 changes
  void segmentFallback(Project &project);

  // Compute energy-based VAD mask from waveform
  void computeVadMask(AudioData &audioData);

  // Extend note start frames backward to capture consonant onsets using VAD
  void extendNoteBoundariesWithVad(Project &project);

  std::unique_ptr<FCPEPitchDetector> fcpeDetector;
  std::unique_ptr<RMVPEPitchDetector> rmvpeDetector;
  std::unique_ptr<GAMEDetector> gameDetector;

  // External detectors (optional, not owned)
  FCPEPitchDetector *externalFCPEDetector = nullptr;
  RMVPEPitchDetector *externalRMVPEDetector = nullptr;
  GAMEDetector *externalGAMEDetector = nullptr;

  bool useFCPE = true;
  PitchDetectorType detectorType = PitchDetectorType::FCPE;
  std::atomic<bool> cancelFlag{false};
  std::atomic<bool> isRunning{false};
  std::thread analysisThread;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioAnalyzer)
};
