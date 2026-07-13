#pragma once

#include "../JuceHeader.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/**
 * Builds short, loopable note previews by resampling their selected audio.
 *
 * Requests are coalesced so dragging can update the requested pitch at UI
 * rate without ever running resampling on the message or audio thread.
 */
class ResampledLoopAudition
{
public:
  using ReadyCallback = std::function<void(juce::AudioBuffer<float>)>;

  explicit ResampledLoopAudition(ReadyCallback onReady);
  ~ResampledLoopAudition();

  void request(std::vector<float> source, float semitoneShift,
               float targetMidiNote);
  void cancel();

private:
  void workerLoop();
  static juce::AudioBuffer<float> render(std::vector<float> source,
                                         float semitoneShift,
                                         float targetMidiNote);

  ReadyCallback onReady;
  std::thread worker;
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<float> pendingSource;
  float pendingSemitoneShift = 0.0f;
  float pendingTargetMidiNote = 60.0f;
  std::uint64_t requestId = 0;
  bool hasPendingRequest = false;
  bool shuttingDown = false;
};
