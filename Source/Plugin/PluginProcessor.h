#pragma once

#include "../Audio/Engine/PluginTransportController.h"
#include "../Audio/RealtimePitchProcessor.h"
#include "../JuceHeader.h"
#include "../UI/IMainView.h"
#include "../Undo/PitchUndoManager.h"
#include "HostCompatibility.h"
#include "NonAraCaptureController.h"
#include <atomic>
#include <map>
#include <memory>

class EditorController;
class Project;
class PitchNetDocumentController;
class PitchNetAudioModification;

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
  void updateProjectStateFromEditor(const Project &project);
  void requestCapturedAudioAnalysis(const juce::AudioBuffer<float> &buffer,
                                    double sampleRate,
                                    double timelineOffsetSeconds);
  void updateAraTimelineOffset(double timelineOffsetSeconds);
  bool serializePersistentProjectState(juce::MemoryBlock &destData) const;
  bool restorePersistentProjectState(const void *data, size_t sizeInBytes);

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

#if JucePlugin_Enable_ARA
  juce::AudioProcessorARAExtension *getARAClientExtensions();

  // Bind the realtime processor to the ARA document controller. Ownership of
  // the binding lives with the processor (not the editor) so ARA playback keeps
  // working headlessly after the editor closes; the processor clears it in its
  // destructor when the realtime processor actually goes away.
  void setAraDocumentController(PitchNetDocumentController *dc);

  // ARA bind hook: runs when the host binds this instance to ARA, including
  // when a saved project is loaded with the UI closed. Establishes the
  // document-controller binding and the headless realtime playback state.
  void didBindToARA() noexcept override;

  // Make the given ARA playback region the one shown/edited on the canvas.
  // Each region keeps its own persistent Project; switching saves the outgoing
  // region's edits and loads the incoming region's project. Called from the
  // editor when the host selection changes (and, later, from the in-plugin
  // timeline). Safe to call with a region that has no analysis yet.
  void setActiveAraRegion(juce::ARAPlaybackRegion *region);
  void updateActiveAraRegionProperties(juce::ARAPlaybackRegion *region);
  juce::String getActiveAraRegionKey() const { return activeRegionKey; }

  // Analyse a single region's audio into its own persistent Project (keyed by
  // regionKey) and, if that region is the active one, show it on the canvas.
  // Also stores the region's processed audio on its modification so the timeline
  // clip + per-region playback can read it headlessly (Stage B). Called by the
  // document controller after extracting one region's audio. Uses a dedicated
  // controller so it never disturbs the composite analysis / playback pipeline.
  // True when the project contains actual user edits (per-note synthesis or
  // global pitch/formant offsets) — only such projects publish per-region
  // processed audio; unedited regions play their original source.
  static bool projectHasRegionEdits(const Project &project);
  void analyzeAraRegionForCanvas(const juce::String &regionKey,
                                 PitchNetAudioModification *modification,
                                 juce::int64 startSampleInModification,
                                 double timelineOffsetSeconds,
                                 const juce::AudioBuffer<float> &buffer,
                                 double sampleRate);

  // Called when a playback region is removed. Its Project and undo history
  // have the same lifetime and are destroyed together.
  void removeAraRegion(const juce::String &regionKey);

  // Per-region project persistence. serialize returns false if no project is
  // cached for regionKey; restore installs a project so a saved region is not
  // re-analysed on reload.
  bool serializeAraRegionProject(const juce::String &regionKey,
                                 juce::MemoryBlock &out) const;
  bool hasAraRegionProject(const juce::String &regionKey) const;
  bool araRegionProjectAppearsToCover(const juce::String &regionKey,
                                      double regionStart,
                                      double regionEnd) const;
  bool restoredAraProjectAppearsToCover(double regionStart,
                                        double regionEnd) const;
  void restoreAraRegionProject(const juce::String &regionKey, const void *data,
                               size_t sizeInBytes);
#endif

  // Non-ARA mode: edit preview. Auditions a frame range of the synthesized
  // result while the host transport is stopped, mirroring the ARA editor
  // renderer's preview. These are no-ops in an ARA host, where processNonARAMode
  // never runs and the ARA preview path handles auditioning instead.
  void startPluginPreview(double startSeconds, double endSeconds);
  void stopPluginPreview();
  void startPluginAudition(const juce::AudioBuffer<float> &buffer,
                           double sampleRate);
  void stopPluginAudition();

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

  /** Bind the realtime processor to the persistent backend snapshot when no
   *  editor is present (headless ARA playback / bounce). No-op if an editor is
   *  open (it drives the binding) or no analyzed snapshot exists yet. */
  void bindRealtimeProcessorHeadless();

#if JucePlugin_Enable_ARA
  /** Establish the full headless ARA binding (document-controller pointer,
   *  persistence callbacks, and realtime playback snapshot) at the correct,
   *  already-prepared sample rate. Safe to call repeatedly; no-op for the
   *  realtime bind while an editor is open. */
  void ensureHeadlessAraBinding();
#endif

  /** Detect parameter changes on audio thread and dispatch to message thread. */
  void checkParameterChanges();
  bool restoreProjectJsonToProcessorState(const juce::String &projectJson);
  void publishPersistentProjectSnapshot(const Project &project);
  void attachMacroParameters(Project &project);
  void adoptMacroParameters(Project &project);

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
  std::shared_ptr<MacroParameters> macroParameters =
      std::make_shared<MacroParameters>();

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
#if JucePlugin_Enable_ARA
  PitchNetDocumentController *araDocumentController = nullptr;
#endif
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

  struct AraRegionState {
    std::unique_ptr<Project> project;
    std::unique_ptr<PitchUndoManager> undoManager;

    PitchUndoManager *ensureUndoManager() {
      if (!undoManager)
        undoManager = std::make_unique<PitchUndoManager>(100);
      return undoManager.get();
    }
  };

  // The active Project temporarily moves into the editor controller; inactive
  // Projects remain here. Moving ownership preserves the object identity and
  // raw pointers retained by that region's undo actions.
  std::map<juce::String, AraRegionState> araRegions;
  juce::String activeRegionKey;
  // True only while the canvas is showing the ACTIVE REGION's own (region-local)
  // project. onProjectDataChanged fires for every project change — including
  // completion of the composite/document analysis, whose waveform is anchored to
  // the whole timeline. Publishing that composite as the region's processed
  // audio made the renderer play the composite's leading silence at the region
  // position. The flag lets updateProjectStateFromEditor publish per-region
  // audio only when the canvas project is actually the region's own.
  bool canvasShowsActiveAraRegion = false;
  // The active region's modification + its start in the modification, tracked so
  // edits can be re-published onto the modification (resynth-on-edit) and stay
  // in sync for headless/persisted per-region playback.
  PitchNetAudioModification *activeModification = nullptr;
  juce::int64 activeStartSampleInModification = 0;
  double activeRegionStartSeconds = 0.0;
  double activeRegionEndSeconds = 0.0;
  // Dedicated controller for per-region canvas analysis, kept separate from the
  // composite araAnalysisController so the two never interfere.
  std::unique_ptr<EditorController> regionCanvasController;
  std::atomic<bool> regionCanvasRenderPendingRerun{false};

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
  std::shared_ptr<juce::AudioBuffer<float>> pluginAuditionBuffer;
  std::atomic<double> pluginAuditionSampleRate{44100.0};
  // Audio-thread-only handoff state for crossfading new audition renders.
  std::shared_ptr<juce::AudioBuffer<float>> activePluginAuditionBuffer;
  std::shared_ptr<juce::AudioBuffer<float>> previousPluginAuditionBuffer;
  juce::int64 pluginAuditionCursor = 0;
  juce::int64 previousPluginAuditionCursor = 0;
  int pluginAuditionTransitionRemaining = 0;
  int pluginAuditionTransitionTotal = 0;

  // Plugin state version for forward/backward compatibility
  static constexpr int PLUGIN_STATE_VERSION = 1;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNetAudioProcessor)
};
