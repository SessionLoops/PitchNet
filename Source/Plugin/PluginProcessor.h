#pragma once

#include "../Audio/Engine/PluginTransportController.h"
#include "../Audio/RealtimePitchProcessor.h"
#include "../JuceHeader.h"
#include "../UI/IMainView.h"
#include "HostCompatibility.h"
#include "NonAraCaptureController.h"
#include <atomic>
#include <memory>

class EditorController;
class Project;
class PitchUndoManager;

/**
 * PitchNet Audio Processor
 *
 * Supports two modes like Melodyne:
 * 1. ARA Mode: Direct audio access via ARA protocol (Studio One, Cubase, Logic,
 * etc.)
 * 2. Non-ARA Mode: Auto-capture and process (FL Studio, Ableton, etc.)
 *
 * Exposes 5 host-automatable parameters:
 * - Bypass: pass through original audio
 * - Output Gain: post-processing volume (-24 to +12 dB)
 * - Dry/Wet: blend between original and processed audio (0-100%)
 * - Pitch Offset: global pitch shift in semitones (-24 to +24 st)
 * - Formant Shift: formant preservation shift (-12 to +12 st)
 */
class PitchNetAudioProcessor : public juce::AudioProcessor
#if JucePlugin_Enable_ARA
    ,
                                public juce::AudioProcessorARAExtension
#endif
{
public:
  PitchNetAudioProcessor();
  ~PitchNetAudioProcessor() override;

  // AudioProcessor interface
  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;
  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
  void processBlockBypassed(juce::AudioBuffer<float> &,
                            juce::MidiBuffer &) override;

#if !JucePlugin_PreferredChannelConfigurations
  bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
#endif

  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override;
  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  double getTailLengthSeconds() const override { return 0.0; }

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String &) override {}

  void getStateInformation(juce::MemoryBlock &destData) override;
  void setStateInformation(const void *data, int sizeInBytes) override;

  // Mode detection
  bool isARAModeActive() const;
  HostCompatibility::HostInfo getHostInfo() const;
  juce::String getHostStatusMessage() const;

  // Editor connection
  void setMainComponent(IMainView *mc);
  IMainView *getMainComponent() const { return mainComponent; }
  bool attachCachedAraAnalysis(std::uintptr_t sourceKey,
                               double timelineOffsetSeconds,
                               const std::vector<std::pair<double, double>> &
                                   playbackRegionRanges);
  void requestAraSourceAnalysis(std::uintptr_t sourceKey,
                               const juce::AudioBuffer<float> &buffer,
                               double sampleRate, double timelineOffsetSeconds,
                               const std::vector<std::pair<double, double>> &
                                   playbackRegionRanges);
  void requestPluginProjectRender(const Project &project);
  void requestCapturedAudioAnalysis(const juce::AudioBuffer<float> &buffer,
                                    double sampleRate,
                                    double timelineOffsetSeconds);
  void updateAraTimelineOffset(double timelineOffsetSeconds);

  void removeAraRegionFromProject(
      std::uintptr_t newSourceKey, const std::pair<double, double> &removedRange,
      const std::vector<std::pair<double, double>> &remainingRanges);
  void analyzeAndMergeAraRegion(
      std::uintptr_t newSourceKey, const juce::AudioBuffer<float> &buffer,
      double sampleRate, const std::pair<double, double> &addedRange,
      const std::vector<std::pair<double, double>> &allRanges);

  // ========== Host Transport Control ==========

  PluginTransportController &getTransportController() {
    return transportController;
  }

  void requestHostPlayState(bool shouldPlay) {
    transportController.requestPlay(shouldPlay);
  }

  void requestHostStop() { transportController.requestStop(); }

  void requestHostSeek(double timeInSeconds) {
    transportController.requestSeek(timeInSeconds);
  }

  void toggleHostPlayPause() { transportController.togglePlayPause(); }

  bool canControlHostTransport() const {
    return transportController.canControlTransport();
  }

  // Real-time processor access
  RealtimePitchProcessor &getRealtimeProcessor() { return realtimeProcessor; }
  double getHostSampleRate() const { return hostSampleRate; }

  // Non-ARA mode: edit preview. Auditions a frame range of the synthesized
  // result while the host transport is stopped, mirroring the ARA editor
  // renderer's preview. These are no-ops in an ARA host, where processNonARAMode
  // never runs and the ARA preview path handles auditioning instead.
  void startPluginPreview(double startSeconds, double endSeconds);
  void stopPluginPreview();

  // Non-ARA mode: capture control
  void startCapture();
  void stopCapture();
  bool isCapturing() const {
    return captureController && captureController->getState() ==
                                    NonAraCaptureController::State::Capturing;
  }
  bool isCaptureArmed() const {
    if (!captureController)
      return false;
    const auto state = captureController->getState();
    return state == NonAraCaptureController::State::WaitingForAudio ||
           state == NonAraCaptureController::State::Capturing;
  }

  // ========== Parameter Access ==========

  juce::AudioProcessorValueTreeState &getAPVTS() { return apvts; }

  // Parameter IDs (stable across versions for automation compatibility)
  static constexpr const char *PARAM_BYPASS = "bypass";
  static constexpr const char *PARAM_OUTPUT_GAIN = "outputGain";
  static constexpr const char *PARAM_DRY_WET = "dryWet";
  static constexpr const char *PARAM_PITCH_OFFSET = "pitchOffset";
  static constexpr const char *PARAM_FORMANT_SHIFT = "formantShift";

private:
  struct HostUiSyncState {
    std::atomic<double> latestSeconds{0.0};
    std::atomic<bool> posPending{false};
    std::atomic<bool> stoppedPending{false};
  };

  void processNonARAMode(juce::AudioBuffer<float> &buffer,
                         const juce::AudioPlayHead::PositionInfo &posInfo,
                         bool isRealtime);
  void disarmCaptureUi();
  void dispatchLiveCaptureUpdate();

  /** Render the non-ARA edit preview into the buffer. Returns true if preview
   *  is active (and produced output or intentional silence), false to let the
   *  normal stopped-transport handling proceed. Audio thread only. */
  bool processPluginPreview(juce::AudioBuffer<float> &buffer);

  /** Apply bypass, dry/wet mix, and output gain to the processed buffer. */
  void applyOutputProcessing(juce::AudioBuffer<float> &processedBuffer,
                             const juce::AudioBuffer<float> &dryBuffer);

  /** Detect parameter changes on audio thread and dispatch to message thread. */
  void checkParameterChanges();

  static juce::AudioProcessorValueTreeState::ParameterLayout
  createParameterLayout();

  // APVTS (must be declared after AudioProcessor base is constructed)
  juce::AudioProcessorValueTreeState apvts;

  // Raw parameter value pointers for lock-free audio-thread access
  std::atomic<float> *bypassParamValue = nullptr;
  std::atomic<float> *outputGainParamValue = nullptr;
  std::atomic<float> *dryWetParamValue = nullptr;
  std::atomic<float> *pitchOffsetParamValue = nullptr;
  std::atomic<float> *formantShiftParamValue = nullptr;

  // Cached parameter values for change detection (audio thread only)
  float cachedPitchOffset = 0.0f;
  float cachedFormantShift = 0.0f;

  // Debounced parameter sync: audio thread -> message thread -> project
  struct ParamSyncState {
    std::atomic<bool> pending{false};
    std::atomic<float> pitchOffset{0.0f};
    std::atomic<float> formantShift{0.0f};
    std::atomic<bool> needsResynth{false};
  };
  std::shared_ptr<ParamSyncState> paramSyncState =
      std::make_shared<ParamSyncState>();

  PluginTransportController transportController;
  RealtimePitchProcessor realtimeProcessor;
  IMainView *mainComponent = nullptr;
  std::shared_ptr<HostUiSyncState> hostUiSyncState =
      std::make_shared<HostUiSyncState>();
  double hostSampleRate = 44100.0;

  juce::String pendingStateJson;
  std::unique_ptr<EditorController> araAnalysisController;
  std::unique_ptr<EditorController> araIncrementalAnalysisController;
  std::unique_ptr<PitchUndoManager> undoManager;
  MainViewViewportState viewportState;
  std::uintptr_t araAnalysisSourceKey = 0;
  bool araAnalysisLoading = false;
  bool araAnalysisReady = false;
  double araAnalysisTimelineOffsetSeconds = 0.0;
  std::vector<std::pair<double, double>> araPlaybackRegionRanges;
  std::unique_ptr<Project> araAnalysisProjectSnapshot;
  juce::String araAnalysisProjectJson;
  std::atomic<bool> araRenderPendingRerun{false};

  // Non-ARA capture (Stage 2A): decoupled controller
  std::shared_ptr<NonAraCaptureController> captureController =
      std::make_shared<NonAraCaptureController>();
  NonAraCaptureController::State lastCaptureUiState =
      NonAraCaptureController::State::Idle;
  struct LiveCaptureUiState {
    std::atomic<bool> pending{false};
    std::atomic<int> latestSamples{0};
    std::atomic<int> sentSamples{0};
    std::atomic<unsigned int> generation{0};
    std::atomic<double> timelineOffsetSeconds{0.0};
  };
  std::shared_ptr<LiveCaptureUiState> liveCaptureUiState =
      std::make_shared<LiveCaptureUiState>();
  double captureTimelineOffsetSeconds = 0.0;
  static constexpr int MAX_CAPTURE_SECONDS = 300; // 5 minutes max

  // Non-ARA edit preview. Written from the message thread, consumed on the
  // audio thread; pluginPreviewCursor is audio-thread only.
  struct PluginPreviewState {
    std::atomic<bool> active{false};
    std::atomic<bool> restart{false};
    std::atomic<double> startSeconds{0.0};
    std::atomic<double> endSeconds{0.0};
  };
  PluginPreviewState pluginPreview;
  juce::int64 pluginPreviewCursor = 0;

  // Plugin state version for forward/backward compatibility
  static constexpr int PLUGIN_STATE_VERSION = 1;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNetAudioProcessor)
};
