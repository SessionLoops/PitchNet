#pragma once

#include "../Audio/RealtimePitchProcessor.h"
#include "../JuceHeader.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
// projects. ARA gives persistent identity to audio modifications, not to
// playback regions, so every region referencing a modification resolves to one
// key and shares one edit layer.
juce::String pitchnetRegionKey(const juce::ARAPlaybackRegion &region);
juce::String
pitchnetArchivedRegionKey(const juce::ARAPlaybackRegion &region);

struct AraPreviewState {
  std::atomic<double> previewStartTime{0.0};
  std::atomic<double> previewEndTime{0.0};
  // Bumped on every explicit audition request. The render path plays a preview
  // through ONCE and then empties its loop range, so it must re-render to make
  // a sound again - but it only re-rendered when the range or region changed.
  // Auditioning the same span twice therefore fell silent until something else
  // reset the cached range, which is why nudging the transport "fixed" it.
  std::atomic<std::uint32_t> previewGeneration{0};
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

  // Region assignment happens on the model thread, so readers and resampler
  // slots are allocated there rather than lazily inside processBlock().
  void ensureRenderResourcesFor(juce::ARAPlaybackRegion *region);
  void didAddPlaybackRegion(
      ARA::PlugIn::PlaybackRegion *playbackRegion) noexcept override;
  void willRemovePlaybackRegion(
      ARA::PlugIn::PlaybackRegion *playbackRegion) noexcept override;

  std::map<juce::ARAAudioSource *, std::unique_ptr<juce::ARAAudioSourceReader>>
      readers;
  // Persistent resampler state per region, split by source so a failed processed
  // render cannot poison the raw-source fallback state.
  std::unordered_map<juce::ARAPlaybackRegion *, AraResamplingState>
      rawResamplingStates;
  std::unordered_map<juce::ARAPlaybackRegion *, AraResamplingState>
      processedResamplingStates;
  std::unique_ptr<juce::AudioBuffer<float>> tempBuffer;
  // Diagnostics: last reported render outcome per region, so a line is only
  // emitted when something actually changes (see PITCHNET_ARA_DIAGNOSTICS).
  struct DiagnosticRenderState {
    double playbackStart = -1.0;
    juce::int64 startInModification = -1;
    int blobSamples = -2;
    bool rendered = false;
    // Cheap content fingerprint of the served blob. Without it this state only
    // tracks the blob's DIMENSIONS, so a republished blob of identical length
    // - which is every edit, since the project spans the whole modification -
    // looks unchanged and never reports. That blind spot is what stopped the
    // first-edit-after-split bug from being diagnosable.
    float blobFingerprint = 0.0f;
    bool operator==(const DiagnosticRenderState &o) const {
      return juce::approximatelyEqual(playbackStart, o.playbackStart) &&
             startInModification == o.startInModification &&
             blobSamples == o.blobSamples && rendered == o.rendered &&
             blobFingerprint == o.blobFingerprint;
    }
  };
  std::unordered_map<juce::ARAPlaybackRegion *, DiagnosticRenderState>
      diagnosticRenderStates;
  std::shared_ptr<HostUiSyncState> hostUiSyncState =
      std::make_shared<HostUiSyncState>();
  HostLoopState previousLoopState;
  bool hasPreviousLoopState = false;
  double sampleRate = 44100.0;
  int numChannels = 2;
};

// Bridges model-thread reconfiguration of the editor renderer to the realtime
// render thread. ARA explicitly permits a host to assign editor-renderer
// regions while the plug-in is in render state: see ARAInterface.h,
// @ref Assigning_ARAEditorRendererInterface_Regions -- "The host can make these
// calls while the plug-in is in render-state [...] Plug-ins must implement a
// proper bridging to the concurrent render threads." The document editing lock
// therefore does NOT cover these calls, because no editing cycle is required
// for them. Modelled on AsyncConfigurationCallback in JUCE's ARAPluginDemo.
//
// (The playback renderer needs no such bridge: its assignment calls "must only
// be made when the plug-in is not in render-state" -- ARAInterface.h,
// @ref Assigning_ARAPlaybackRendererInterface_Regions.)
class AraAsyncConfigurationCallback final : private juce::AsyncUpdater {
public:
  explicit AraAsyncConfigurationCallback(std::function<void()> callbackIn)
      : callback(std::move(callbackIn)) {}

  ~AraAsyncConfigurationCallback() override { cancelPendingUpdate(); }

  // Realtime-safe: never blocks. A failed lock means a reconfiguration is in
  // flight and this block must not touch the resources it is rebuilding.
  juce::SpinLock::ScopedTryLockType tryLockForRender() const noexcept {
    return juce::SpinLock::ScopedTryLockType(processingFlag);
  }

  // Any thread. Coalesces; the callback runs later on the message thread.
  void startConfigure() { triggerAsyncUpdate(); }

private:
  void handleAsyncUpdate() override {
    const juce::SpinLock::ScopedLockType scope(processingFlag);
    callback();
  }

  std::function<void()> callback;
  mutable juce::SpinLock processingFlag;
};

class PitchNetEditorRenderer : public juce::ARAEditorRenderer,
                               private juce::ARARegionSequence::Listener {
public:
  using ARAEditorRenderer::ARAEditorRenderer;
  ~PitchNetEditorRenderer() override;

  // Model thread. Asks for the source readers to be rebuilt under the lock that
  // processBlock() try-holds. Used by the document controller when it selects a
  // region for preview that the host never assigned to this renderer.
  void requestReaderConfiguration() { asyncConfigCallback.startConfigure(); }

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

  void ensureReaderFor(juce::ARAPlaybackRegion *region);
  void configure();

  // A host assigns editor-renderer regions either one region at a time or a
  // whole region sequence at a time (ARAEditorRendererInterface offers both,
  // and hosts are told not to mix them on one instance). Regions assigned by
  // sequence never appear in getPlaybackRegions(), so both routes must be
  // walked to see everything this renderer is expected to preview.
  template <typename Callback>
  void forEachAssignedPlaybackRegion(Callback &&cb) {
    for (auto *region : getPlaybackRegions<juce::ARAPlaybackRegion>())
      if (!cb(region))
        return;

    for (auto *sequence : getRegionSequences<juce::ARARegionSequence>())
      for (auto *region : sequence->getPlaybackRegions<juce::ARAPlaybackRegion>())
        if (!cb(region))
          return;
  }

  void didAddPlaybackRegion(
      ARA::PlugIn::PlaybackRegion *playbackRegion) noexcept override;
  void didAddRegionSequence(
      ARA::PlugIn::RegionSequence *regionSequence) noexcept override;
  void willRemoveRegionSequence(
      ARA::PlugIn::RegionSequence *regionSequence) noexcept override;
  void didAddPlaybackRegionToRegionSequence(
      juce::ARARegionSequence *regionSequence,
      juce::ARAPlaybackRegion *playbackRegion) override;

  std::map<juce::ARAAudioSource *, std::unique_ptr<juce::ARAAudioSourceReader>>
      readers;
  std::set<juce::ARARegionSequence *> listenedRegionSequences;
  AraAsyncConfigurationCallback asyncConfigCallback{[this] { configure(); }};
  // Offline rendering intentionally starts with no readers and does not run on
  // a realtime thread, so it may still create one on demand mid-render.
  bool mayCreateReadersWhileRendering = false;
  std::shared_ptr<juce::AudioBuffer<float>> previewBuffer;
  std::shared_ptr<juce::AudioBuffer<float>> previousPreviewBuffer;
  juce::Range<juce::int64> previewLoopRange;
  juce::int64 previewLoopPosition = 0;
  juce::int64 previousPreviewLoopPosition = 0;
  int previewTransitionRemaining = 0;
  int previewTransitionTotal = 0;
  std::uint32_t lastPreviewGeneration = 0;
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
  void willBeginEditing(juce::ARADocument *document) override;
  void didEndEditing(juce::ARADocument *document) override;

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
  void didUpdateAudioModificationProperties(
      juce::ARAAudioModification *audioModification) override;
  // ARA gives persistent identity to audio modifications, and the processor
  // holds a raw pointer to the one on the canvas. Track its lifetime so that
  // pointer cannot outlive the object.
  void willDeactivateAudioModificationForUndoHistory(
      juce::ARAAudioModification *audioModification, bool deactivate) override;
  void willDestroyAudioModification(
      juce::ARAAudioModification *audioModification) override;
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

  // Extract a single region's audio and hand it to the processor for per-region
  // analysis (populates that region's persistent Project and, if it is the
  // active region, switches the canvas to it). Used for selection-driven
  // per-region editing without disturbing the composite pipeline.
  void requestRegionCanvasAnalysis(juce::ARAPlaybackRegion *region);
  void setCurrentPlaybackRegion(juce::ARAPlaybackRegion *region);

  void setMainComponent(IMainView *mc);
  IMainView *getMainComponent() const { return mainComponent; }
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
  bool processExistingAudioSources(juce::ARADocument *document);
  bool processPlaybackRegions(
      const std::vector<juce::ARAPlaybackRegion *> &playbackRegions,
      double projectSampleRate);
  juce::ARAPlaybackRegion *getCurrentPlaybackRegion() const {
    return currentPlaybackRegion;
  }
  // Every region this document controller has discovered for the current
  // sequence. Used by the editor to list selectable regions when the plugin
  // instance's playback renderer has none assigned yet.
  const std::vector<juce::ARAPlaybackRegion *> &
  getCurrentPlaybackRegions() const {
    return currentPlaybackRegions;
  }
  // Takes MODIFICATION seconds, matching the Project's coordinate space.
  void startPreviewRange(double previewStartInModificationSeconds,
                         double previewEndInModificationSeconds);
  void startPreviewAudio(const juce::AudioBuffer<float> &buffer,
                         double sampleRate);
  void stopPreview();
  AraPreviewState &getPreviewState() { return previewState; }
  const AraPreviewState &getPreviewState() const { return previewState; }

  // Excludes the audio thread while the host mutates the ARA model graph.
  // Renderers take this for read in processBlock() and produce nothing when it
  // is unavailable, so a playback region cannot be destroyed out from under a
  // render that is mid-read. Mirrors the ProcessingLockInterface pattern in
  // JUCE's ARAPluginDemo.
  juce::ScopedTryReadLock getProcessingLock();

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
  void clearStaleRegionSequenceFilter(juce::ARADocument *document);
  bool shouldProcessPlaybackRegion(juce::ARAPlaybackRegion *region) const;
  void clearMainComponentHostAudio();
  void notifyAudioModificationContentChanged(bool notifyHost);
  bool restoreProjectStateToDocument(const void *data, size_t sizeInBytes);
  bool serializeDocumentProjectState(juce::MemoryBlock &destData) const;
  void restoreAraRegionProjectOrPend(const juce::String &regionKey,
                                     const void *data, size_t sizeInBytes);
  void flushPendingAraRegionProjects();
  void snapshotRegionState(juce::ARAPlaybackRegion &region);
  PitchNetAudioProcessor *getRegionCanvasProcessor() const;

  bool hostEditing = false;
  std::vector<juce::ARAPlaybackRegion *> deferredRegionUpdates;
  void stopAnalysisThread();

  struct AnalysisState {
    std::atomic<std::uint64_t> jobId{0};
    std::atomic<bool> cancel{false};
  };

  // Write-held across the host's editing cycle (willBeginEditing ->
  // didEndEditing); try-read-held by both renderers' processBlock().
  juce::ReadWriteLock processBlockLock;
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
