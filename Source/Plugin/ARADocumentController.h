#pragma once

#include "../Audio/RealtimePitchProcessor.h"
#include "../JuceHeader.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>

#if JucePlugin_Enable_ARA

class IMainView;
class PitchNetDocumentController;

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
  PitchNetDocumentController *getDocController() const;
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
      std::function<bool(std::uintptr_t, double)> attachCachedAnalysis,
      std::function<void(std::uintptr_t, const juce::AudioBuffer<float> &,
                         double, double)>
          requestAnalysis);
  void setTimelineOffsetCallback(std::function<void(double)> callback) {
    timelineOffsetCallback = std::move(callback);
  }

  void setRealtimeProcessor(RealtimePitchProcessor *processor) {
    realtimeProcessor = processor;
  }
  RealtimePitchProcessor *getRealtimeProcessor() const {
    return realtimeProcessor;
  }
  bool processExistingAudioSources(juce::ARADocument *document);

protected:
  juce::ARAPlaybackRenderer *doCreatePlaybackRenderer() noexcept override;
  bool doRestoreObjectsFromStream(
      juce::ARAInputStream &input,
      const juce::ARARestoreObjectsFilter *filter) noexcept override;
  bool doStoreObjectsToStream(
      juce::ARAOutputStream &output,
      const juce::ARAStoreObjectsFilter *filter) noexcept override;

private:
  void processAudioSource(juce::ARAAudioSource *source);
  void updateAudioSourceTimelineOffset(
      juce::ARAAudioSource *source,
      juce::ARAPlaybackRegion *excludedRegion = nullptr);
  void clearMainComponentHostAudio();
  std::optional<double>
  getTimelineOffsetSecondsForSource(
      juce::ARAAudioSource *source,
      juce::ARAPlaybackRegion *excludedRegion = nullptr) const;

  void stopAnalysisThread();

  struct AnalysisState {
    std::atomic<std::uint64_t> jobId{0};
    std::atomic<bool> cancel{false};
  };

  IMainView *mainComponent = nullptr;
  juce::ARAAudioSource *currentAudioSource = nullptr;
  RealtimePitchProcessor *realtimeProcessor = nullptr;
  std::function<bool(std::uintptr_t, double)> attachCachedAnalysisCallback;
  std::function<void(std::uintptr_t, const juce::AudioBuffer<float> &, double,
                     double)>
      requestAnalysisCallback;
  std::function<void(double)> timelineOffsetCallback;

  std::shared_ptr<AnalysisState> analysisState =
      std::make_shared<AnalysisState>();
  std::thread analysisThread;
  std::thread analysisJoinerThread;
};

#endif // JucePlugin_Enable_ARA
