#include "NonAraCaptureController.h"

#include <algorithm>
#include <cmath>

void NonAraCaptureController::prepare(double sampleRate, int numChannels,
                                      int maxCaptureSeconds) {
  const int maxSamples = static_cast<int>(sampleRate * maxCaptureSeconds);

  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    preparedChannels = numChannels;
    preparedMaxSamples = maxSamples;
    // Playback-only instances (including duplicated/bounced tracks) need no
    // five-minute recording buffer. Allocate it only when capture is armed.
    captureBuffer.setSize(0, 0);
    capturePosition = 0;
    publishedCapturePosition.store(0);
    finalLength = 0;
    holdReason.store(HoldReason::None);
    expectedHostPosition.reset();
  }

  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  state.store(State::Idle);
}

void NonAraCaptureController::resetToWaiting() {
  const juce::SpinLock::ScopedLockType lock(bufferLock);
  captureBuffer.setSize(preparedChannels, preparedMaxSamples);
  captureBuffer.clear();
  capturePosition = 0;
  publishedCapturePosition.store(0);
  finalLength = 0;
  holdReason.store(HoldReason::None);
  expectedHostPosition.reset();
  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  state.store(State::WaitingForAudio);
}

void NonAraCaptureController::processBlock(
    const juce::AudioBuffer<float> &input, bool hostIsPlaying,
    std::optional<juce::int64> hostSamplePosition) {
  if (analysisPending.load())
    return;

  auto currentState = state.load();

  // An armed capture begins with the first block processed while the host is
  // running. Record silence too: the incoming timeline must remain intact.
  if (currentState == State::WaitingForAudio && hostIsPlaying) {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    capturePosition = 0;
    holdReason.store(HoldReason::None);
    expectedHostPosition.reset();
    state.store(State::Capturing);
    shouldFinalizeFlag.store(false);
  }

  currentState = state.load();

  if (currentState != State::Capturing)
    return;

  if (hostIsPlaying) {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    // A bounded take remains armed and passes audio through until transport
    // stops. Never launch analysis or switch to cached playback mid-song.
    if (holdReason.load() != HoldReason::None)
      return;

    if (hostSamplePosition && expectedHostPosition) {
      const auto gap = *hostSamplePosition - *expectedHostPosition;
      if (gap < -1) {
        holdReason.store(HoldReason::TimelineJump);
        return;
      }
      if (gap > 1) {
        // Storage is cleared when armed, so skipped timeline samples remain
        // silent rather than shifting every subsequent block earlier.
        const auto space = captureBuffer.getNumSamples() - capturePosition;
        capturePosition += static_cast<int>(std::min<juce::int64>(gap, space));
        publishedCapturePosition.store(capturePosition);
      }
    }
    expectedHostPosition = hostSamplePosition
        ? std::optional<juce::int64>(*hostSamplePosition + input.getNumSamples())
        : std::nullopt;

    int spaceLeft = captureBuffer.getNumSamples() - capturePosition;
    int toCopy = std::min(input.getNumSamples(), spaceLeft);

    if (toCopy > 0) {
      int channelsToCopy =
          std::min(input.getNumChannels(), captureBuffer.getNumChannels());
      for (int ch = 0; ch < channelsToCopy; ++ch)
        captureBuffer.copyFrom(ch, capturePosition, input, ch, 0, toCopy);
      capturePosition += toCopy;
      publishedCapturePosition.store(capturePosition);
    }

    if (capturePosition >= captureBuffer.getNumSamples())
      holdReason.store(HoldReason::Capacity);
  } else {
    shouldFinalizeFlag.store(true);
  }
}

bool NonAraCaptureController::finalizeCapture(double hostSampleRate,
                                              FinalizeResult &out) {
  if (state.load() != State::Capturing)
    return false;

  const int minSamples = static_cast<int>(hostSampleRate * minCaptureSeconds);

  int captured = 0;
  int channels = 0;
  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    captured = capturePosition;
    channels = captureBuffer.getNumChannels();
  }

  if (captured < minSamples) {
    stop();
    return false;
  }

  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    finalLength = capturePosition;
  }

  analysisPending.store(true);
  shouldFinalizeFlag.store(false);
  state.store(State::Complete);

  out.numChannels = channels;
  out.numSamples = finalLength;
  out.sampleRate = hostSampleRate;
  return true;
}

juce::AudioBuffer<float>
NonAraCaptureController::copyCapturedAudio(int numSamples) const {
  juce::AudioBuffer<float> trimmed;

  const juce::SpinLock::ScopedLockType lock(bufferLock);
  int length = std::min(numSamples, captureBuffer.getNumSamples());
  trimmed.setSize(captureBuffer.getNumChannels(), length);
  for (int ch = 0; ch < captureBuffer.getNumChannels(); ++ch)
    trimmed.copyFrom(ch, 0, captureBuffer, ch, 0, length);

  return trimmed;
}

juce::AudioBuffer<float> NonAraCaptureController::copyCapturedAudioRange(
    int startSample, int numSamples) const {
  juce::AudioBuffer<float> chunk;
  const juce::SpinLock::ScopedLockType lock(bufferLock);
  const int start = juce::jlimit(0, capturePosition, startSample);
  const int length = juce::jlimit(0, capturePosition - start, numSamples);
  chunk.setSize(captureBuffer.getNumChannels(), length);
  for (int ch = 0; ch < captureBuffer.getNumChannels(); ++ch)
    chunk.copyFrom(ch, 0, captureBuffer, ch, start, length);
  return chunk;
}

void NonAraCaptureController::onAnalysisDispatched() {
  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  holdReason.store(HoldReason::None);
  state.store(State::Idle);
}

void NonAraCaptureController::stop() {
  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    captureBuffer.clear();
    capturePosition = 0;
    publishedCapturePosition.store(0);
    finalLength = 0;
    holdReason.store(HoldReason::None);
    expectedHostPosition.reset();
  }
  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  state.store(State::Idle);
}
