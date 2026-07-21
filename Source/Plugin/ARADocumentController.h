#pragma once

#include "../Audio/RealtimePitchProcessor.h"
#include "../JuceHeader.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

#if JucePlugin_Enable_ARA

// Persistent per-region resampler state so the interpolator survives across
// audio blocks (avoids block-boundary clicks when a region's rate differs from
// the host rate). Mirrors VocalNet's ResamplingState.
struct AraResamplingState {
  std::vector<juce::LagrangeInterpolator> interpolators;
  juce::int64 nextSourceSample = 0;
  juce::int64 lastRenderedOutputEnd = std::numeric_limits<juce::int64>::lowest();
  double ratio = 1.0;
  bool initialised = false;
};

class IMainView;
class PitchNetAudioProcessor;
class PitchNetDocumentController;
class PitchNetEditorRenderer;

// Persistent identity for an ARA playback region: the audio-modification
// persistent ID plus the region's current index within that modification. Host
// refs are reassigned every session, so they cannot be used for saved DAW
// projects. Used to key per-region Projects and per-region processed audio so
// each region/track is analysed, edited, and played back independently.
juce::String pitchnetRegionKey(const juce::ARAPlaybackRegion &region);
juce::String pitchnetRegionKeyForIndex(const juce::String &modificationID,
                                       int regionIndex);
void pitchnetReleaseRegionKey(const juce::ARAPlaybackRegion &region);

struct AraPreviewState {
  std::atomic<double> previewStartTime{0.0};
  std::atomic<double> previewEndTime{0.0};
  std::atomic<juce::ARAPlaybackRegion *> previewedRegion{nullptr};
  std::atomic<PitchNetEditorRenderer *> previewClaimedRenderer{nullptr};
  std::shared_ptr<juce::AudioBuffer<float>> auditionBuffer;
  std::atomic<double> editorRendererSampleRate{0.0};
  std::atomic<bool> auditionActive{false};
};

/**
 * ARA Playback Renderer
 * Reads audio from ARA sources and applies pitch correction
 */
class PitchNetPlaybackRenderer : public juce::ARAPlaybackRenderer {
public:
  using ARAPlaybackRenderer::ARAPlaybackRenderer;

  void prepareToPlay(double sampleRateIn, int maxBlockSize, int numChannelsIn,
                     juce::AudioProcessor::ProcessingPrecision,
                     AlwaysNonRealtime alwaysNonRealtime) override;
  void releaseResources() override;
  bool processBlock(
      juce::AudioBuffer<float> &buffer, juce::AudioProcessor::Realtime realtime,
      const juce::AudioPlayHead::PositionInfo &positionInfo) noexcept override;

  // Public so the processor can reach the document controller when binding to
  // ARA without an open editor (e.g. loading a saved project with the UI
  // closed).
  PitchNetDocumentController *getDocController() const;

private:
  struct HostUiSyncState {
    std::atomic<bool> latestPlaying{false};
    std::atomic<double> latestLoopStartSeconds{0.0};
    std::atomic<double> latestLoopEndSeconds{0.0};
    std::atomic<bool> latestLoopEnabled{false};
    std::atomic<bool> latestLoopHasRange{false};
    std::atomic<bool> playStatePending{false};
    std::atomic<bool> stoppedPending{false};
    std::atomic<bool> loopPending{false};
  };

  struct HostLoopState {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    bool enabled = false;
    bool hasRange = false;

    bool operator==(const HostLoopState &other) const {
      constexpr double epsilon = 0.0001;
      return std::abs(startSeconds - other.startSeconds) < epsilon &&
             std::abs(endSeconds - other.endSeconds) < epsilon &&
             enabled == other.enabled && hasRange == other.hasRange;
    }

    bool operator!=(const HostLoopState &other) const {
      return !(*this == other);
    }
  };

  // Per-region playback: mix each region's processed audio (from its ARA
  // modification), falling back to its raw source. Returns true if any region
  // rendered. Output buffer must be pre-cleared. This is the ONLY source of
  // ARA transport playback — no realtime-engine fallback.
  bool renderProcessedRegions(juce::AudioBuffer<float> &buffer,
                              juce::int64 timeInSamples, int numSamples);
  void syncHostLoopState(PitchNetDocumentController *docCtrl,
                         const juce::AudioPlayHead::PositionInfo &posInfo,
                         bool shouldSyncUi);

  std::map<juce::ARAAudioSource *, std::unique_ptr<juce::ARAAudioSourceReader>>
      readers;
  // Persistent resampler state per region, split by source so a failed processed
  // render cannot poison the raw-source fallback state.
  std::unordered_map<juce::ARAPlaybackRegion *, AraResamplingState>
      rawResamplingStates;
  std::unordered_map<juce::ARAPlaybackRegion *, AraResamplingState>
      processedResamplingStates;
  std::unique_ptr<juce::AudioBuffer<float>> tempBuffer;
  std::shared_ptr<HostUiSyncState> hostUiSyncState =
      std::make_shared<HostUiSyncState>();
  HostLoopState previousLoopState;
  bool hasPreviousLoopState = false;
  double sampleRate = 44100.0;
  int numChannels = 2;
};

class PitchNetEditorRenderer : public juce::ARAEditorRenderer {
public:
  using ARAEditorRenderer::ARAEditorRenderer;

  void prepareToPlay(double sampleRateIn, int maxBlockSize, int numChannelsIn,
                     juce::AudioProcessor::ProcessingPrecision,
                     AlwaysNonRealtime alwaysNonRealtime) override;
  void releaseResources() override;
  bool processBlock(
      juce::AudioBuffer<float> &buffer, juce::AudioProcessor::Realtime realtime,
      const juce::AudioPlayHead::PositionInfo &positionInfo) noexcept override;

private:
  void renderPreviewBuffer(juce::ARAPlaybackRegion *region,
                           double previewStartTime, double previewEndTime);
  bool readPlaybackRangeIntoBuffer(juce::Range<double> playbackRange,
                                   juce::ARAPlaybackRegion *region,
                                   juce::AudioBuffer<float> &buffer);
  void writePreviewOnce(juce::AudioBuffer<float> &buffer);
  void writePreviewLoop(juce::AudioBuffer<float> &buffer);
  bool readFromARARegions(juce::AudioBuffer<float> &buffer,
                          juce::int64 timeInSamples, int numSamples);
  PitchNetDocumentController *getDocController() const;

  std::map<juce::ARAAudioSource *, std::unique_ptr<juce::ARAAudioSourceReader>>
      readers;
  std::shared_ptr<juce::AudioBuffer<float>> previewBuffer;
  std::shared_ptr<juce::AudioBuffer<float>> previousPreviewBuffer;
  juce::Range<juce::int64> previewLoopRange;
  juce::int64 previewLoopPosition = 0;
  juce::int64 previousPreviewLoopPosition = 0;
  int previewTransitionRemaining = 0;
  int previewTransitionTotal = 0;
  double lastPreviewStartTime = -1.0;
  double lastPreviewEndTime = -1.0;
  juce::ARAPlaybackRegion *lastPreviewRegion = nullptr;
  bool wasPreviewing = false;
  std::shared_ptr<juce::AudioBuffer<float>> lastAuditionBuffer;
  double sampleRate = 44100.0;
  int numChannels = 2;
};

/**
 * ARA Document Controller
 * Manages ARA document lifecycle and audio source analysis
 */
class PitchNetDocumentController
    : public juce::ARADocumentControllerSpecialisation {
public:
  using ARADocumentControllerSpecialisation::
      ARADocumentControllerSpecialisation;

  ~PitchNetDocumentController() override;

  void didAddAudioSourceToDocument(juce::ARADocument *doc,
                                   juce::ARAAudioSource *audioSource) override;
  void willRemoveAudioSourceFromDocument(
      juce::ARADocument *doc, juce::ARAAudioSource *audioSource) override;
  // On unload/reload the host restores audio sources with sample access
  // disabled and enables it afterwards; re-run analysis once samples become
  // readable so regions are recognised and analysed.
  void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource *audioSource,
                                         bool enable) override;
  void willDestroyAudioSource(juce::ARAAudioSource *audioSource) override;
  void didAddPlaybackRegionToRegionSequence(
      juce::ARARegionSequence *regionSequence,
      juce::ARAPlaybackRegion *playbackRegion) override;
  void willDestroyRegionSequence(juce::ARARegionSequence *regionSequence)
      override;
  void didAddPlaybackRegionToAudioModification(
      juce::ARAAudioModification *audioModification,
      juce::ARAPlaybackRegion *playbackRegion) override;
  void willRemovePlaybackRegionFromAudioModification(
      juce::ARAAudioModification *audioModification,
      juce::ARAPlaybackRegion *playbackRegion) override;
  void didUpdatePlaybackRegionProperties(
      juce::ARAPlaybackRegion *playbackRegion) override;
  void willDestroyPlaybackRegion(juce::ARAPlaybackRegion *playbackRegion)
      override;
  void reanalyze();

  // Extract a single region's audio and hand it to the processor for per-region
  // analysis (populates that region's persistent Project and, if it is the
  // active region, switches the canvas to it). Used for selection-driven
  // per-region editing without disturbing the composite pipeline.
  void requestRegionCanvasAnalysis(juce::ARAPlaybackRegion *region);
  void setCurrentPlaybackRegion(juce::ARAPlaybackRegion *region);

  void setMainComponent(IMainView *mc);
  IMainView *getMainComponent() const { return mainComponent; }
  void setAnalysisCallbacks(
      std::function<bool(
          std::uintptr_t, double,
          const std::vector<std::pair<double, double>> &)>
          attachCachedAnalysis,
      std::function<void(std::uintptr_t, const juce::AudioBuffer<float> &,
                         double, double,
                         const std::vector<std::pair<double, double>> &)>
          requestAnalysis);
  void setPersistenceCallbacks(
      std::function<bool(juce::MemoryBlock &)> serializeProjectState,
      std::function<bool(const void *, size_t)> restoreProjectState);
  void setRealtimeProcessor(RealtimePitchProcessor *processor) {
    realtimeProcessor = processor;
  }
  RealtimePitchProcessor *getRealtimeProcessor();
  void setOwningProcessor(PitchNetAudioProcessor *processor);
  void releaseOwningProcessor(PitchNetAudioProcessor *processor);
  // ARA hosts may bind several processor instances to one document controller.
  // Keep the processor that owns the visible editor separate from the
  // headless/playback owner so canvas work cannot be dispatched to a processor
  // with no UI.
  void setEditorProcessor(PitchNetAudioProcessor *processor);
  void releaseEditorProcessor(PitchNetAudioProcessor *processor);
  void ensureHeadlessPlaybackBinding();
  void prepareDocumentPlayback(double sampleRate, int maxBlockSize);
  void setDocumentProjectSnapshot(const Project &project,
                                  bool notifyHost = true);
  // Publish the edited parts of a COMPOSITE (timeline-anchored) project onto
  // the per-region modifications: every playback region overlapping an edited
  // note gets its slice of the composite waveform stored as processed audio,
  // so ARA playback (strictly modification-or-original) reflects edits made
  // without selecting a region. Regions not touching any edit stay unpublished
  // and keep playing their original source. Message thread only.
  void publishCompositeEditsToRegions(const Project &project);
  bool processExistingAudioSources(juce::ARADocument *document);
  bool processPlaybackRegions(
      const std::vector<juce::ARAPlaybackRegion *> &playbackRegions,
      double projectSampleRate);
  void startPreviewRange(double previewStartSeconds, double previewEndSeconds);
  void startPreviewAudio(const juce::AudioBuffer<float> &buffer,
                         double sampleRate);
  void stopPreview();
  AraPreviewState &getPreviewState() { return previewState; }
  const AraPreviewState &getPreviewState() const { return previewState; }

protected:
  juce::ARAPlaybackRenderer *doCreatePlaybackRenderer() noexcept override;
  juce::ARAEditorRenderer *doCreateEditorRenderer() override;
  // Create our custom modification so each region can carry its own processed
  // audio + thumbnail (per-region data model; foundation for the timeline and
  // per-region playback).
  juce::ARAAudioModification *doCreateAudioModification(
      juce::ARAAudioSource *audioSource,
      ARA::ARAAudioModificationHostRef hostRef,
      const juce::ARAAudioModification *optionalModificationToClone) override;
  bool doRestoreObjectsFromStream(
      juce::ARAInputStream &input,
      const juce::ARARestoreObjectsFilter *filter) noexcept override;
  bool doStoreObjectsToStream(
      juce::ARAOutputStream &output,
      const juce::ARAStoreObjectsFilter *filter) noexcept override;

private:
  void processDocument(juce::ARADocument *document,
                       juce::ARAPlaybackRegion *excludedRegion = nullptr,
                       juce::ARAAudioSource *excludedSource = nullptr);
  void clearStaleRegionSequenceFilter(juce::ARADocument *document);
  bool shouldProcessPlaybackRegion(juce::ARAPlaybackRegion *region) const;
  void clearMainComponentHostAudio();
  void notifyAudioModificationContentChanged(bool notifyHost);
  bool restoreProjectStateToDocument(const void *data, size_t sizeInBytes);
  bool serializeDocumentProjectState(juce::MemoryBlock &destData) const;
  void restoreAraRegionProjectOrPend(const juce::String &regionKey,
                                     const void *data, size_t sizeInBytes);
  void flushPendingAraRegionProjects();
  PitchNetAudioProcessor *getRegionCanvasProcessor() const;

  void stopAnalysisThread();

  struct AnalysisState {
    std::atomic<std::uint64_t> jobId{0};
    std::atomic<bool> cancel{false};
  };

  IMainView *mainComponent = nullptr;
  juce::ARAAudioSource *currentAudioSource = nullptr;
  juce::ARADocument *currentDocument = nullptr;
  juce::ARARegionSequence *currentRegionSequence = nullptr;
  juce::ARAPlaybackRegion *currentPlaybackRegion = nullptr;
  std::vector<juce::ARAPlaybackRegion *> currentPlaybackRegions;
  double analysisTimelineSampleRate = 0.0;
  RealtimePitchProcessor *realtimeProcessor = nullptr;
  std::unique_ptr<Project> documentProjectSnapshot;
  RealtimePitchProcessor documentRealtimeProcessor;
  PitchNetAudioProcessor *owningProcessor = nullptr;
  PitchNetAudioProcessor *editorProcessor = nullptr;
  AraPreviewState previewState;
  std::function<bool(std::uintptr_t, double,
                     const std::vector<std::pair<double, double>> &)>
      attachCachedAnalysisCallback;
  std::function<void(std::uintptr_t, const juce::AudioBuffer<float> &, double,
                     double,
                     const std::vector<std::pair<double, double>> &)>
      requestAnalysisCallback;
  std::function<bool(juce::MemoryBlock &)> serializeProjectStateCallback;
  std::function<bool(const void *, size_t)> restoreProjectStateCallback;
  juce::MemoryBlock pendingRestoredProjectData;
  struct PendingAraRegionProject {
    juce::String regionKey;
    juce::MemoryBlock data;
  };
  std::vector<PendingAraRegionProject> pendingRestoredRegionProjects;
  std::shared_ptr<AnalysisState> analysisState =
      std::make_shared<AnalysisState>();
  std::thread analysisThread;
  std::thread analysisJoinerThread;
};

#endif // JucePlugin_Enable_ARA
