#pragma once

#include "../JuceHeader.h"
#include "AudioEngine.h"
#include "Engine/PlaybackController.h"
#include "FCPEPitchDetector.h"
#include "SwiftF0PitchDetector.h"
#include "PitchDetectorType.h"
#include "RMVPEPitchDetector.h"
#include "GAMEDetector.h"
#include "Synthesis/IncrementalSynthesizer.h"
#include "SynthesisEngineType.h"
#include "Vocoder.h"
#include "../Models/Project.h"
#include "../Utils/AppLogger.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct SharedInferenceModels;
struct ClassifierModels;

// Options of EditorController::setHostAudioAsync (outside the class so it
// can be a default argument there).
struct EditorHostAnalysisOptions
{
  // The clip's own channels at the analysed rate (at most two), kept for the
  // polyphonic engine; dropped again when the clip is analysed as vocal.
  std::shared_ptr<const juce::AudioBuffer<float>> takeChannels;
  // The clip's analysis state to start from (e.g. its classification from
  // a track pre-pass, so it is not classified twice).
  std::optional<AnalysisModeState> initialMode;
  // The track's mode: applied whatever the clip's own classification says
  // (which is still recorded). With it, the track's detector and whether the
  // user chose the mode.
  std::optional<AnalysisMode> trackMode;
  PolyphonicDetector trackDetector = PolyphonicDetector::Harmonic;
  bool trackModeUserChosen = false;
};

class EditorController
{
public:
  explicit EditorController(bool enableAudioDevice);
  ~EditorController();

  // Signal cancellation without waiting; safe to repeat before destruction.
  void requestShutdown();

  Project *getProject() const { return project.get(); }
  std::unique_ptr<Project> takeProject() { return std::move(project); }
  void setProject(std::unique_ptr<Project> newProject);

  AudioEngine *getAudioEngine() const { return audioEngine.get(); }
  Vocoder *getVocoder() const { return vocoder.get(); }
  IncrementalSynthesizer *getIncrementalSynth() const
  {
    return incrementalSynth.get();
  }
  PlaybackController *getPlaybackController() const
  {
    return playbackController.get();
  }

  void setPitchDetectorType(PitchDetectorType type)
  {
    pitchDetectorType = type;
  }

  void setSynthesisEngineType(SynthesisEngineType type)
  {
    synthesisEngineType = type;
    if (incrementalSynth)
      incrementalSynth->setSynthesisEngine(type);
  }

  SynthesisEngineType getSynthesisEngineType() const
  {
    return synthesisEngineType;
  }

  // Called on the message thread when a render failure forced the engine
  // from the AI vocoder to classic (PSOLA), so the UI can follow.
  std::function<void(SynthesisEngineType)> onSynthesisEngineFallback;

  // Polyphonic takes run the polyphonic engine instead of the vocal pipeline.
  // Off by default; the standalone app turns it on. The plug-in follows in M5
  // (docs/polyphonic-integration-plan.md), until then it analyses as vocal.
  void setPolyphonicAnalysisEnabled(bool enabled)
  {
    polyphonicAnalysisEnabled = enabled;
  }

  void setDeviceConfig(const juce::String &deviceName, int gpuDeviceId)
  {
    device = deviceName;
    deviceId = gpuDeviceId;
  }

  void reloadInferenceModels(bool async);
  bool isSelectedPitchDetectorLoaded() const;
  bool isInferenceBusy() const;
  bool isLoading() const { return isLoadingAudio.load(); }

  using ProgressCallback =
      std::function<void(double, const juce::String &)>;
  using LoadCompleteCallback =
      std::function<void(const juce::AudioBuffer<float> &)>;
  using CancelCallback = std::function<void()>;

  void loadAudioFileAsync(const juce::File &file,
                          const ProgressCallback &onProgress,
                          const LoadCompleteCallback &onComplete,
                          const CancelCallback &onCancelled);
  using HostAnalysisOptions = EditorHostAnalysisOptions;

  // `buffer` is analysed as before (the vocal path's mono input).
  void setHostAudioAsync(const juce::AudioBuffer<float> &buffer,
                         double sampleRate,
                         const ProgressCallback &onProgress,
                         const LoadCompleteCallback &onComplete,
                         HostAnalysisOptions options = {});

  // Classifies several clips (mono, each at its own rate) on the loader
  // thread, for a track's mode; onComplete gets one state per clip (not
  // classified where it could not be), on the message thread. Nothing when
  // cancelled or superseded by another load.
  void classifyHostAudioAsync(
      std::vector<std::pair<juce::AudioBuffer<float>, double>> clips,
      std::function<void(std::vector<AnalysisModeState>)> onComplete);
  void requestCancelLoading();

  void resynthesizeIncrementalAsync(
      Project &project,
      const std::function<void(const juce::String &)> &onProgress,
      const std::function<void(bool)> &onComplete,
      std::atomic<bool> &pendingRerun,
      bool isPluginMode);

  void analyzeAudio(Project &targetProject,
                    const std::function<void(double, const juce::String &)>
                        &onProgress,
                    std::function<void()> onComplete = nullptr);

  void segmentIntoNotes(Project &targetProject,
                        std::function<void()> onStreamingUpdate = nullptr,
                        std::function<void(double)> onProgress = nullptr);

  void analyzeAudioAsync(
      const std::function<void(Project &)> &onProjectReady,
      const std::function<void()> &onProjectChanged);

  void segmentIntoNotesAsync(
      const std::function<void(Project &)> &onProjectReady,
      const std::function<void()> &onNotesChanged);

  // Analyses the current take again with another mode or detector (the Input
  // menu, the Analysis card). Works on a copy of the project with its edits
  // dropped: the pristine waveform comes back, notes and any polyphonic
  // analysis are cleared, the take's own channels are kept, and settings
  // (loop, scale, timeline, MIDI reference) carry over. onComplete runs on the
  // message thread with the analysed copy, or null when the analysis did not
  // finish; the caller installs it with setProject.
  void reanalyzeAsync(const AnalysisModeState &state,
                      const ProgressCallback &onProgress,
                      std::function<void(std::unique_ptr<Project>)> onComplete);

  // Rebuilds a saved polyphonic take (Project::getPendingPolyphonicRestore)
  // in the background after a project is opened, while the project is shown
  // and edited with its restoring take (saved notes and levels): the source is
  // the project's audio file when it is there and unchanged (stereo kept),
  // else the saved original waveform (its mono mix); the masks come from
  // polyphonic::restore, without detecting notes again. onComplete runs on
  // the message thread with the restored take (the caller puts it into the
  // project, keeping any edits made meanwhile), or null when it did not
  // finish.
  void restorePolyphonicAsync(
      std::function<void(std::shared_ptr<const PolyphonicTake>)> onComplete);

  // The work of a restore, on the caller's thread: the masks for `restore`'s
  // notes from `source` (the take's channels, any rate, at most two). Null
  // when cancelled or on failure.
  static std::shared_ptr<const PolyphonicTake>
  restorePolyphonicTake(const PolyphonicRestore &restore, juce::AudioBuffer<float> source,
                        double sourceRate, const std::atomic<bool> *cancel);

  // Renders the polyphonic take with the project's current edits on a worker
  // thread. One render at a time: a request while one runs cancels it and
  // renders again with the newest edits, so a burst of edits costs one
  // render. Call on the message thread. When a render lands for the take it
  // was made from, the result becomes the project's polyphonic playback (the
  // take's channels) and its waveform (their mono mix, for display), and
  // onComplete(true) runs on the message thread; the caller hands the
  // playback to the audio engine. onComplete(false) when nothing was rendered.
  void renderPolyphonicAsync(std::function<void(bool)> onComplete);
  bool isPolyphonicRenderBusy() const { return polyRenderRunning; }

  // Live audition of a drag on a polyphonic take: `notes` on their own with
  // `edits` applied (polyphonic::renderNotes), mixed to mono, delivered on the
  // message thread. The newest request wins: one still running is cancelled
  // and only the latest is delivered. cancelPolyphonicAudition drops any
  // result still to come.
  void renderPolyphonicAuditionAsync(std::vector<int> notes,
                                     Project::PolyphonicEdits edits,
                                     std::function<void(juce::AudioBuffer<float>)> onReady);
  void cancelPolyphonicAudition();

  // Live Audition ahead of time: each of `notes` rendered on its own with its
  // edit from `edits` (as the audition renders it), one by one on a
  // background thread; onNote(note, buffer, startSeconds) on the message
  // thread for each. A new call, or cancelPolyphonicPrerender, stops the one
  // running (its results still to come are dropped).
  void prerenderPolyphonicNotesAsync(
      std::shared_ptr<const PolyphonicTake> take, Project::PolyphonicEdits edits,
      std::vector<int> notes,
      std::function<void(int, juce::AudioBuffer<float>, double)> onNote);
  void cancelPolyphonicPrerender();

private:
  // If the vocoder had to load on CPU, move an AI engine to classic (PSOLA)
  // and tell the UI. Returns true when it switched.
  bool switchToClassicIfVocoderOnCpu();

  // Unguarded bodies of analyzeAudio()/segmentIntoNotes(). The public
  // wrappers catch exceptions so a failure on a loader thread shows an
  // error instead of terminating the app.
  void analyzeAudioImpl(Project &targetProject,
                        const std::function<void(double, const juce::String &)>
                            &onProgress,
                        std::function<void()> onComplete);
  void segmentIntoNotesImpl(Project &targetProject,
                            std::function<void()> onStreamingUpdate,
                            std::function<void(double)> onProgress);
  // Classifies the take (vocal pipeline or polyphonic engine) and records the
  // decision in its AnalysisModeState, unless it is already classified. Until
  // M2 the analysis that follows is vocal whatever the decision.
  void classifyInputType(Project &targetProject);
  // A track's mode for the analysis running on the loader thread
  // (HostAnalysisOptions); applied after the take's own classification.
  std::optional<AnalysisModeState> analysisTrackMode;
  // Builds the take's PolyphonicTake (Basic Pitch evidence + polyphonic::analyze)
  // and clears the vocal analysis. False when cancelled.
  bool analyzePolyphonicTake(
      Project &targetProject,
      const std::function<void(double, const juce::String &)> &onProgress);
  static void reportAnalysisException(const juce::String &where,
                                      const juce::String &what);
  GPUProvider getProviderFromDevice(const juce::String &device) const;
  std::unique_ptr<Project> project;
  std::unique_ptr<AudioEngine> audioEngine;
  // Analysis models come from a SharedInferenceModels set (see the .cpp).
  // modelPtrMutex guards these pointers; analysis copies them first, so a
  // device switch never frees models that are still running.
  mutable std::mutex modelPtrMutex;
  std::shared_ptr<SharedInferenceModels> sharedModels;
  std::shared_ptr<FCPEPitchDetector> fcpePitchDetector;
  std::shared_ptr<RMVPEPitchDetector> rmvpePitchDetector;
  std::shared_ptr<SwiftF0PitchDetector> swiftF0PitchDetector;
  std::shared_ptr<GAMEDetector> gameDetector;
  // Input-type classifier models (CPU, optional, one instance per process).
  std::shared_ptr<ClassifierModels> classifierModels;
  // Generation of the installed set; an older load never replaces a newer one.
  std::uint64_t installedGeneration = 0;
  std::atomic<std::uint64_t> reloadGeneration{0};
  std::unique_ptr<Vocoder> vocoder;
  std::unique_ptr<IncrementalSynthesizer> incrementalSynth;
  std::unique_ptr<PlaybackController> playbackController;

  juce::File fcpeModelPath;
  juce::File melFilterbankPath;
  juce::File centTablePath;
  juce::File rmvpeModelPath;
  juce::File swiftF0ModelPath;
  juce::File basicPitchModelPath;
  juce::File yamnetModelPath;
  juce::File gameModelDir;
  PitchDetectorType pitchDetectorType = PitchDetectorType::SwiftF0;
  SynthesisEngineType synthesisEngineType = defaultSynthesisEngineType();
  juce::String device = "CPU";
  int deviceId = 0;

  std::atomic<bool> shuttingDown{false};
  bool polyphonicAnalysisEnabled = false;
  std::atomic<bool> isReloadingModels{false};
  std::mutex modelLoadMutex;
  std::mutex reloadRequestMutex;
  std::function<void()> pendingReload;
  std::condition_variable reloadFinished;
  std::thread modelReloadThread;

  // Async load state
  std::thread loaderThread;

  // renderPolyphonicAsync state; message thread only, except the thread.
  void startPolyphonicRender();
  void finishPolyphonicRender(std::shared_ptr<const PolyphonicTake> take,
                              std::shared_ptr<const juce::AudioBuffer<float>> rendered);
  std::thread polyRenderThread;
  std::shared_ptr<std::atomic<bool>> polyRenderCancel;
  std::function<void(bool)> polyRenderOnComplete;
  bool polyRenderRunning = false;
  bool polyRenderRerun = false;

  struct AuditionRequest
  {
    std::vector<int> notes;
    Project::PolyphonicEdits edits;
    std::function<void(juce::AudioBuffer<float>)> onReady;
  };
  void startPolyphonicAudition(AuditionRequest request);
  std::thread auditionThread;
  std::shared_ptr<std::atomic<bool>> auditionCancel;
  std::optional<AuditionRequest> pendingAudition;
  bool auditionRunning = false;
  int auditionGeneration = 0; // bumped by cancel: late results are dropped
  std::thread prerenderThread;
  std::shared_ptr<std::atomic<bool>> prerenderCancel;
  int prerenderGeneration = 0; // message thread
  std::thread loaderJoinerThread;
  std::atomic<bool> isLoadingAudio{false};
  std::atomic<bool> cancelLoadingFlag{false};
  std::atomic<std::uint64_t> hostAnalysisJobId{0};

  // Async render state

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorController)
};
