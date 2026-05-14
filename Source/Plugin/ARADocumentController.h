#pragma once

#include "../Audio/RealtimePitchProcessor.h"
#include "../JuceHeader.h"

#include <atomic>
#include <cstdint>
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
    std::atomic<bool> posPending{false};
    std::atomic<bool> stoppedPending{false};
  };

  bool readFromARARegions(juce::AudioBuffer<float> &buffer,
                          juce::int64 timeInSamples, int numSamples);
  PitchNetDocumentController *getDocController() const;

  std::map<juce::ARAAudioSource *, std::unique_ptr<juce::ARAAudioSourceReader>>
      readers;
  std::unique_ptr<juce::AudioBuffer<float>> tempBuffer;
  std::shared_ptr<HostUiSyncState> hostUiSyncState =
      std::make_shared<HostUiSyncState>();
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
  void reanalyze();

  void setMainComponent(IMainView *mc) { mainComponent = mc; }
  IMainView *getMainComponent() const { return mainComponent; }

  void setRealtimeProcessor(RealtimePitchProcessor *processor) {
    realtimeProcessor = processor;
  }
  RealtimePitchProcessor *getRealtimeProcessor() const {
    return realtimeProcessor;
  }

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

  void stopAnalysisThread();

  struct AnalysisState {
    std::atomic<std::uint64_t> jobId{0};
    std::atomic<bool> cancel{false};
  };

  IMainView *mainComponent = nullptr;
  juce::ARAAudioSource *currentAudioSource = nullptr;
  RealtimePitchProcessor *realtimeProcessor = nullptr;

  std::shared_ptr<AnalysisState> analysisState =
      std::make_shared<AnalysisState>();
  std::thread analysisThread;
  std::thread analysisJoinerThread;
};

#endif // JucePlugin_Enable_ARA
