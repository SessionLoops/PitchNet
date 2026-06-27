#pragma once

#include "../Audio/RealtimePitchProcessor.h"
#include "../JuceHeader.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>

#if JucePlugin_Enable_ARA

class IMainView;
class PitchNetAudioProcessor;
class PitchNetDocumentController;

struct AraPreviewState {
  std::atomic<double> previewStartTime{0.0};
  std::atomic<double> previewEndTime{0.0};
  std::atomic<juce::ARAPlaybackRegion *> previewedRegion{nullptr};
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
    std::atomic<double> latestSeconds{0.0};
    std::atomic<bool> latestPlaying{false};
    std::atomic<double> latestLoopStartSeconds{0.0};
    std::atomic<double> latestLoopEndSeconds{0.0};
    std::atomic<bool> latestLoopEnabled{false};
    std::atomic<bool> latestLoopHasRange{false};
    std::atomic<bool> posPending{false};
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

  bool readFromARARegions(juce::AudioBuffer<float> &buffer,
                          juce::int64 timeInSamples, int numSamples);
  void syncHostLoopState(PitchNetDocumentController *docCtrl,
                         const juce::AudioPlayHead::PositionInfo &posInfo,
                         bool shouldSyncUi);

  std::map<juce::ARAAudioSource *, std::unique_ptr<juce::ARAAudioSourceReader>>
      readers;
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
  bool readFromARARegions(juce::AudioBuffer<float> &buffer,
                          juce::int64 timeInSamples, int numSamples);
  PitchNetDocumentController *getDocController() const;

  std::map<juce::ARAAudioSource *, std::unique_ptr<juce::ARAAudioSourceReader>>
      readers;
  std::unique_ptr<juce::AudioBuffer<float>> previewBuffer;
  juce::Range<juce::int64> previewLoopRange;
  juce::int64 previewLoopPosition = 0;
  double lastPreviewStartTime = -1.0;
  double lastPreviewEndTime = -1.0;
  juce::ARAPlaybackRegion *lastPreviewRegion = nullptr;
  bool wasPreviewing = false;
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
  void didAddPlaybackRegionToAudioModification(
      juce::ARAAudioModification *audioModification,
      juce::ARAPlaybackRegion *playbackRegion) override;
  void willRemovePlaybackRegionFromAudioModification(
      juce::ARAAudioModification *audioModification,
      juce::ARAPlaybackRegion *playbackRegion) override;
  void didUpdatePlaybackRegionProperties(
      juce::ARAPlaybackRegion *playbackRegion) override;
  void reanalyze();

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
  void ensureHeadlessPlaybackBinding();
  void prepareDocumentPlayback(double sampleRate, int maxBlockSize);
  void setDocumentProjectSnapshot(const Project &project,
                                  bool notifyHost = true);
  bool processExistingAudioSources(juce::ARADocument *document);
  bool processPlaybackRegions(
      const std::vector<juce::ARAPlaybackRegion *> &playbackRegions,
      double projectSampleRate);
  void startPreviewRange(double previewStartSeconds, double previewEndSeconds);
  void stopPreview();
  const AraPreviewState &getPreviewState() const { return previewState; }

protected:
  juce::ARAPlaybackRenderer *doCreatePlaybackRenderer() noexcept override;
  juce::ARAEditorRenderer *doCreateEditorRenderer() override;
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
  void clearMainComponentHostAudio();
  void notifyAudioModificationContentChanged(bool notifyHost);
  bool restoreProjectStateToDocument(const void *data, size_t sizeInBytes);
  bool serializeDocumentProjectState(juce::MemoryBlock &destData) const;

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
  double analysisTimelineSampleRate = 0.0;
  RealtimePitchProcessor *realtimeProcessor = nullptr;
  std::unique_ptr<Project> documentProjectSnapshot;
  RealtimePitchProcessor documentRealtimeProcessor;
  PitchNetAudioProcessor *owningProcessor = nullptr;
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
  std::shared_ptr<AnalysisState> analysisState =
      std::make_shared<AnalysisState>();
  std::thread analysisThread;
  std::thread analysisJoinerThread;
};

#endif // JucePlugin_Enable_ARA
