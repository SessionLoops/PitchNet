#include "AudioEngine.h"
#include <algorithm>

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine() { shutdownAudio(); }

void AudioEngine::initializeAudio() {
  // Initialize audio device
  auto result = deviceManager.initialiseWithDefaultDevices(
      0, 2); // No input, stereo output

  if (result.isNotEmpty()) {
  } else {
    auto *device = deviceManager.getCurrentAudioDevice();
    if (device) {
    }
  }

  deviceManager.addAudioCallback(&audioSourcePlayer);
  audioSourcePlayer.setSource(this);
}

void AudioEngine::shutdownAudio() {
  audioSourcePlayer.setSource(nullptr);
  deviceManager.removeAudioCallback(&audioSourcePlayer);
  deviceManager.closeAudioDevice();
}

void AudioEngine::prepareToPlay(int samplesPerBlockExpected,
                                double sampleRate) {
  currentSampleRate = sampleRate;
  playbackRatio = static_cast<double>(waveformSampleRate) / sampleRate;
  interpolator.reset();
  fractionalPosition = 0.0;

}

void AudioEngine::releaseResources() {}

void AudioEngine::getNextAudioBlock(
    const juce::AudioSourceChannelInfo &bufferToFill) {
  const bool auditioning = auditionActive.load();
  if (!playing ||
      (auditioning ? auditionWaveform.getNumSamples()
                   : currentWaveform.getNumSamples()) == 0) {
    bufferToFill.clearActiveBufferRegion();
    return;
  }

  const juce::SpinLock::ScopedTryLockType lock(waveformLock);
  if (!lock.isLocked()) {
    // Waveform is being updated, output silence to avoid glitches
    bufferToFill.clearActiveBufferRegion();
    return;
  }

  // Audition is a self-contained loop: crossfade its tail into its head and
  // then continue after the already-crossfaded head section. This eliminates
  // the click/gap of a hard wrap without affecting the project transport.
  if (auditioning) {
    auto *outputBuffer = bufferToFill.buffer;
    const int numOutputSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;
    auto renderLoopSample = [this](const juce::AudioBuffer<float> &waveform,
                                   int sampleRate, double &position) {
      const int sampleCount = waveform.getNumSamples();
      const int overlap = std::min(8192, std::max(1, sampleCount / 2));
      const double overlapStart = static_cast<double>(sampleCount - overlap);
      const double sampleRatio = currentSampleRate > 0.0
                                     ? static_cast<double>(sampleRate) / currentSampleRate
                                     : 1.0;
      auto sampleAt = [&waveform, sampleCount](double samplePosition) {
        samplePosition = juce::jlimit(0.0,
                                      static_cast<double>(sampleCount - 1),
                                      samplePosition);
        const int left = static_cast<int>(samplePosition);
        const int right = std::min(sampleCount - 1, left + 1);
        const float fraction = static_cast<float>(samplePosition - left);
        const auto *data = waveform.getReadPointer(0);
        return data[left] + fraction * (data[right] - data[left]);
      };

      float value = sampleAt(position);
      if (position >= overlapStart) {
        const float t = static_cast<float>((position - overlapStart) / overlap);
        value = value * std::cos(t * juce::MathConstants<float>::halfPi) +
                sampleAt(position - overlapStart) *
                    std::sin(t * juce::MathConstants<float>::halfPi);
      }
      position += sampleRatio;
      while (position >= sampleCount)
        position -= static_cast<double>(sampleCount - overlap);
      return value;
    };

    outputBuffer->clear(startSample, numOutputSamples);
    auto *output = outputBuffer->getWritePointer(0, startSample);
    double position = auditionReadPosition.load();
    double previousPosition = previousAuditionReadPosition.load();
    int transitionRemaining = auditionTransitionSamplesRemaining.load();
    const int transitionTotal = auditionTransitionSamplesTotal.load();
    for (int i = 0; i < numOutputSamples; ++i) {
      float value = renderLoopSample(auditionWaveform, auditionSampleRate,
                                     position);
      if (transitionRemaining > 0 && transitionTotal > 0 &&
          previousAuditionWaveform.getNumSamples() > 0) {
        const float t = 1.0f - static_cast<float>(transitionRemaining) /
                                    static_cast<float>(transitionTotal);
        const float oldValue = renderLoopSample(previousAuditionWaveform,
                                                previousAuditionSampleRate,
                                                previousPosition);
        value = oldValue * std::cos(t * juce::MathConstants<float>::halfPi) +
                value * std::sin(t * juce::MathConstants<float>::halfPi);
        --transitionRemaining;
      }
      output[i] = value;
    }

    const float gain = volumeGain.load();
    if (std::abs(gain - 1.0f) > 0.0001f)
      juce::FloatVectorOperations::multiply(output, gain, numOutputSamples);
    for (int ch = 1; ch < outputBuffer->getNumChannels(); ++ch)
      outputBuffer->copyFrom(ch, startSample, output, numOutputSamples);
    auditionReadPosition.store(position);
    previousAuditionReadPosition.store(previousPosition);
    auditionTransitionSamplesRemaining.store(transitionRemaining);
    return;
  }

  auto *outputBuffer = bufferToFill.buffer;
  auto numOutputSamples = bufferToFill.numSamples;
  auto startSample = bufferToFill.startSample;

  int64_t pos = auditioning ? auditionPosition.load() : currentPosition.load();
  const auto &activeWaveform = auditioning ? auditionWaveform : currentWaveform;
  const int activeSampleRate = auditioning ? auditionSampleRate : waveformSampleRate;
  const double activePlaybackRatio =
      currentSampleRate > 0.0 ? static_cast<double>(activeSampleRate) / currentSampleRate
                              : 1.0;
  int64_t waveformLength = activeWaveform.getNumSamples();

  bool loopActive = auditioning || loopEnabled.load();
  int64_t loopStart = auditioning ? 0 : loopStartSample.load();
  int64_t loopEnd = auditioning ? waveformLength : loopEndSample.load();

  if (loopActive) {
    loopStart = juce::jlimit<int64_t>(0, waveformLength, loopStart);
    loopEnd = juce::jlimit<int64_t>(0, waveformLength, loopEnd);
    if (loopEnd <= loopStart)
      loopActive = false;
  }

  if (!loopActive && pos >= waveformLength) {
    bufferToFill.clearActiveBufferRegion();
    playing = false;
    currentPosition.store(0);
    interpolator.reset();

    // Schedule callback on message thread
    if (auto cb = std::atomic_load(&finishCallback))
      juce::MessageManager::callAsync([cb]() { (*cb)(); });
    return;
  }

  if (loopActive && pos >= loopEnd) {
    pos = loopStart;
    interpolator.reset();
  }

  // Use interpolator for sample rate conversion
  const float *inputData = activeWaveform.getReadPointer(0);
  float *outputData = outputBuffer->getWritePointer(0, startSample);

  outputBuffer->clear(startSample, numOutputSamples);

  int samplesRemaining = numOutputSamples;
  int writeOffset = 0;

  while (samplesRemaining > 0) {
    int64_t segmentEnd = loopActive ? loopEnd : waveformLength;
    int64_t inputAvailable = segmentEnd - pos;

    if (inputAvailable <= 0) {
      if (loopActive) {
        pos = loopStart;
        interpolator.reset();
        continue;
      }
      break;
    }

    int maxOutput = static_cast<int>(inputAvailable / activePlaybackRatio);
    if (maxOutput <= 0) {
      if (loopActive) {
        pos = loopStart;
        interpolator.reset();
        continue;
      }
      break;
    }

    int outCount = std::min(samplesRemaining, maxOutput);
    int samplesUsed = interpolator.process(activePlaybackRatio, inputData + pos,
                                           outputData + writeOffset, outCount,
                                           static_cast<int>(inputAvailable),
                                           0 // No wrap
    );

    pos += samplesUsed;
    samplesRemaining -= outCount;
    writeOffset += outCount;

    if (loopActive && pos >= loopEnd) {
      pos = loopStart;
      interpolator.reset();
    }
  }

  // Apply volume gain (lock-free read)
  float gain = volumeGain.load();
  if (std::abs(gain - 1.0f) > 0.0001f) // Only apply if not unity gain
  {
    juce::FloatVectorOperations::multiply(outputData, gain, numOutputSamples);
  }

  if (auditioning)
    auditionPosition.store(pos);
  else
    currentPosition.store(pos);

  // Copy to other channels (if stereo output)
  for (int ch = 1; ch < outputBuffer->getNumChannels(); ++ch) {
    outputBuffer->copyFrom(ch, startSample,
                           outputBuffer->getReadPointer(0, startSample),
                           numOutputSamples);
  }

  if (!loopActive && samplesRemaining > 0) {
    playing = false;
    currentPosition.store(0);
    interpolator.reset();
    if (auto cb = std::atomic_load(&finishCallback))
      juce::MessageManager::callAsync([cb]() { (*cb)(); });
  }

  // Update position callback
  if (!auditioning)
    if (auto cb = std::atomic_load(&positionCallback)) {
    auto state = positionUpdateState;
    state->latestSeconds.store(static_cast<double>(currentPosition.load()) /
                               waveformSampleRate);

    // Schedule at most one pending callback to avoid flooding the message
    // thread
    if (!state->callbackPending.exchange(true)) {
      juce::MessageManager::callAsync([cb, state]() {
        state->callbackPending.store(false);
        (*cb)(state->latestSeconds.load());
      });
    }
    }
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster *source) {}

void AudioEngine::loadWaveform(const juce::AudioBuffer<float> &buffer,
                               int sampleRate, bool preservePosition) {
  // Save playing state if we need to preserve it
  bool wasPlaying = playing.load();

  // Stop playback first to safely update waveform
  playing = false;

  {
    const juce::SpinLock::ScopedLockType lock(waveformLock);
    currentWaveform = buffer;
    waveformSampleRate = sampleRate;

    if (!preservePosition) {
      currentPosition.store(0);
      fractionalPosition = 0.0;
    }

    // Update playback ratio for sample rate conversion
    if (currentSampleRate > 0)
      playbackRatio =
          static_cast<double>(waveformSampleRate) / currentSampleRate;
    else
      playbackRatio = 1.0;

    interpolator.reset();
  }

  // Restore playing state if preserving position (e.g., during incremental
  // synthesis)
  if (preservePosition && wasPlaying) {
    playing = true;
  }


  if (loopEnabled.load()) {
    auto loopStart = loopStartSample.load();
    auto loopEnd = loopEndSample.load();
    const int64_t waveformLength = currentWaveform.getNumSamples();
    loopStart = juce::jlimit<int64_t>(0, waveformLength, loopStart);
    loopEnd = juce::jlimit<int64_t>(0, waveformLength, loopEnd);
    loopStartSample.store(loopStart);
    loopEndSample.store(loopEnd);
    if (loopEnd <= loopStart)
      loopEnabled.store(false);
  }
}

void AudioEngine::play() {
  if (currentWaveform.getNumSamples() == 0) {
    return;
  }

  playing = true;
}

void AudioEngine::pause() { playing = false; }

void AudioEngine::stop() {
  playing = false;

  const juce::SpinLock::ScopedLockType lock(waveformLock);
  currentPosition.store(0);
  interpolator.reset();
  fractionalPosition = 0.0;
}

void AudioEngine::seek(double timeSeconds) {
  const juce::SpinLock::ScopedLockType lock(waveformLock);
  int64_t newPos = static_cast<int64_t>(timeSeconds * waveformSampleRate);
  newPos = juce::jlimit<int64_t>(0, currentWaveform.getNumSamples(), newPos);
  currentPosition.store(newPos);
  interpolator.reset();
  fractionalPosition = 0.0;
}

void AudioEngine::setLoopRange(double startSeconds, double endSeconds) {
  if (startSeconds > endSeconds)
    std::swap(startSeconds, endSeconds);

  const juce::SpinLock::ScopedLockType lock(waveformLock);
  const int64_t waveformLength = currentWaveform.getNumSamples();
  int64_t startSample =
      static_cast<int64_t>(startSeconds * waveformSampleRate);
  int64_t endSample = static_cast<int64_t>(endSeconds * waveformSampleRate);

  startSample = juce::jlimit<int64_t>(0, waveformLength, startSample);
  endSample = juce::jlimit<int64_t>(0, waveformLength, endSample);

  loopStartSample.store(startSample);
  loopEndSample.store(endSample);
  loopEnabled.store(endSample > startSample);
}

void AudioEngine::setLoopEnabled(bool enabled) {
  if (enabled && loopEndSample.load() <= loopStartSample.load())
    loopEnabled.store(false);
  else
    loopEnabled.store(enabled);
}

void AudioEngine::clearLoopRange() {
  loopEnabled.store(false);
  loopStartSample.store(0);
  loopEndSample.store(0);
}

void AudioEngine::beginAuditionLoop(const juce::AudioBuffer<float> &buffer,
                                    int sampleRate) {
  if (buffer.getNumSamples() <= 0)
    return;

  const juce::SpinLock::ScopedLockType lock(waveformLock);
  constexpr int kTransitionSamples = 4096;
  if (auditionActive.load() && auditionWaveform.getNumSamples() > 0) {
    previousAuditionWaveform.makeCopyOf(auditionWaveform);
    previousAuditionSampleRate = auditionSampleRate;
    previousAuditionReadPosition.store(auditionReadPosition.load());
    auditionTransitionSamplesTotal.store(kTransitionSamples);
    auditionTransitionSamplesRemaining.store(kTransitionSamples);
  } else {
    previousAuditionWaveform.setSize(0, 0);
    auditionTransitionSamplesTotal.store(0);
    auditionTransitionSamplesRemaining.store(0);
  }
  auditionWaveform.makeCopyOf(buffer);
  auditionSampleRate = sampleRate > 0 ? sampleRate : waveformSampleRate;
  auditionPosition.store(0);
  auditionReadPosition.store(0.0);
  auditionActive.store(true);
  interpolator.reset();
  playing.store(true);
}

void AudioEngine::endAuditionLoop() {
  const juce::SpinLock::ScopedLockType lock(waveformLock);
  auditionActive.store(false);
  auditionPosition.store(0);
  auditionReadPosition.store(0.0);
  previousAuditionWaveform.setSize(0, 0);
  previousAuditionReadPosition.store(0.0);
  auditionTransitionSamplesTotal.store(0);
  auditionTransitionSamplesRemaining.store(0);
  interpolator.reset();
}

double AudioEngine::getPosition() const {
  return static_cast<double>(currentPosition.load()) / waveformSampleRate;
}

double AudioEngine::getDuration() const {
  if (currentWaveform.getNumSamples() == 0)
    return 0.0;
  return static_cast<double>(currentWaveform.getNumSamples()) /
         waveformSampleRate;
}

void AudioEngine::setVolumeDb(float dB) {
  // Clamp dB to range: -12 dB to +12 dB (symmetric around 0)
  dB = juce::jlimit(-12.0f, 12.0f, dB);
  // Convert dB to linear gain: gain = 10^(dB/20)
  float gain = juce::Decibels::decibelsToGain(dB, -60.0f);
  volumeGain.store(gain);
}

float AudioEngine::getVolumeDb() const {
  float gain = volumeGain.load();
  return juce::Decibels::gainToDecibels(gain, -60.0f);
}
