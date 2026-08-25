#include "RealtimePitchProcessor.h"
#include "../Utils/AudioResampler.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

RealtimePitchProcessor::RealtimePitchProcessor() = default;

RealtimePitchProcessor::~RealtimePitchProcessor() {
  invalidateGeneration.fetch_add(1);
  if (computeThread && computeThread->joinable())
    computeThread->join();
}

void RealtimePitchProcessor::setProject(Project *proj) {
  {
    const juce::ScopedLock sl(bufferLock);
    project = proj;
  }
  invalidate();
}

void RealtimePitchProcessor::prepareToPlay(double sr, int) {
  const bool sampleRateChanged = !juce::approximatelyEqual(sampleRate, sr);
  bool hasProject = false;
  {
    const juce::ScopedLock sl(bufferLock);
    hasProject = project != nullptr;
  }
  sampleRate = sr;
  position.store(0.0);
  readPosition = 0;
  readPositionValid = false;

  // A project may be attached before the host reveals its actual sample rate.
  // Rebuild the cache so its sample positions match the new host timeline.
  if (sampleRateChanged && hasProject)
    invalidate();
}

bool RealtimePitchProcessor::processBlock(
    juce::AudioBuffer<float> &input, juce::AudioBuffer<float> &output,
    const juce::AudioPlayHead::PositionInfo *posInfo) {
  // Resolve the host playback position as an exact integer sample index.
  // Do NOT round-trip through seconds (samples -> seconds -> samples): the
  // float truncation that introduces makes the read pointer advance by
  // blockSize +/- 1 at a large fraction of block boundaries, producing a
  // one-sample skip or repeat each time, i.e. an audible click train.
  juce::int64 hostPos = 0;
  bool haveHostPos = false;
  if (posInfo) {
    if (auto time = posInfo->getTimeInSamples()) {
      hostPos = *time;
      haveHostPos = true;
    } else if (auto seconds = posInfo->getTimeInSeconds()) {
      hostPos = static_cast<juce::int64>(std::llround(*seconds * sampleRate));
      haveHostPos = true;
    }
  }
  position.store(haveHostPos ? static_cast<double>(hostPos) / sampleRate : 0.0);

  // The continuous read cursor below is only meaningful for streaming playback
  // (contiguous blocks advancing through the timeline). One-shot renders such
  // as note preview call processBlock with isPlaying == false and an output
  // buffer sized to the whole requested range; they must read from exactly the
  // requested position and must not consult or advance the streaming cursor.
  const bool hostPlaying = posInfo && posInfo->getIsPlaying();

  // Passthrough if not ready
  if (!ready.load()) {
    output.makeCopyOf(input);
    readPositionValid = false; // resync to host when playback resumes
    return false;
  }

  const int numSamples = output.getNumSamples();
  const int numChannels = output.getNumChannels();

  juce::int64 posSamples;
  if (!hostPlaying) {
    // One-shot render (e.g. preview): honor the exact requested position and
    // do not disturb the streaming cursor. Invalidate it so streaming resyncs
    // cleanly when real playback resumes.
    posSamples = haveHostPos ? hostPos : (readPositionValid ? readPosition : 0);
    readPositionValid = false;
  } else {
    // Follow the host position contiguously. During normal playback the host
    // advances by exactly numSamples per block, so we keep our own integer read
    // cursor and only resync to the host on a genuine jump (seek/loop). This
    // keeps the read pointer sample-accurate and free of boundary clicks even
    // if the host's reported time has sub-sample jitter.
    const juce::int64 resyncTolerance = juce::jmax<juce::int64>(64, numSamples);
    if (!haveHostPos) {
      posSamples = readPositionValid ? readPosition : 0;
    } else if (!readPositionValid ||
               std::llabs(hostPos - readPosition) > resyncTolerance) {
      posSamples = hostPos; // first block, seek, or loop
    } else {
      posSamples = readPosition; // contiguous playback: ignore host jitter
    }
    readPosition = posSamples + numSamples;
    readPositionValid = true;
  }

  // Copy from processed buffer
  {
    const juce::ScopedLock sl(bufferLock);

    if (processedBuffer.getNumSamples() == 0) {
      output.makeCopyOf(input);
      return false;
    }

    int available =
        processedBuffer.getNumSamples() - static_cast<int>(posSamples);
    if (posSamples < 0 || available <= 0) {
      output.makeCopyOf(input);
      return false;
    }

    int toCopy = std::min(numSamples, available);
    int channelsToCopy =
        std::min(numChannels, processedBuffer.getNumChannels());

    for (int ch = 0; ch < channelsToCopy; ++ch) {
      output.copyFrom(ch, 0, processedBuffer, ch, static_cast<int>(posSamples),
                      toCopy);
      if (toCopy < numSamples)
        output.clear(ch, toCopy, numSamples - toCopy);
    }

    for (int ch = channelsToCopy; ch < numChannels; ++ch)
      output.clear(ch, 0, numSamples);

    // Ramp out of the cache this render replaced. Both caches describe the
    // same timeline and are identical outside the re-rendered region, so the
    // ramp is linear (smoothstep-weighted) and leaves untouched audio
    // bit-exact; an equal-power fade would sum to sqrt(2) on correlated
    // content and add an audible level bump at every commit.
    if (swapFadeRemaining > 0) {
      const int previousAvailable =
          previousProcessedBuffer.getNumSamples() - static_cast<int>(posSamples);
      const int fadeCount =
          std::min({swapFadeRemaining, toCopy, std::max(0, previousAvailable)});
      const int fadeChannels =
          std::min(channelsToCopy, previousProcessedBuffer.getNumChannels());
      const float fadeTotal = static_cast<float>(kBufferSwapFadeSamples);

      for (int ch = 0; ch < fadeChannels; ++ch) {
        const float *outgoing = previousProcessedBuffer.getReadPointer(
            ch, static_cast<int>(posSamples));
        float *blended = output.getWritePointer(ch);
        for (int sample = 0; sample < fadeCount; ++sample) {
          const float t = juce::jlimit(
              0.0f, 1.0f,
              static_cast<float>(kBufferSwapFadeSamples - swapFadeRemaining +
                                 sample) /
                  fadeTotal);
          const float weight = t * t * (3.0f - 2.0f * t);
          blended[sample] =
              outgoing[sample] + weight * (blended[sample] - outgoing[sample]);
        }
      }

      swapFadeRemaining = std::max(0, swapFadeRemaining - toCopy);
      if (swapFadeRemaining == 0)
        previousProcessedBuffer.setSize(0, 0, false, false, true);
    }
  }

  return true;
}

void RealtimePitchProcessor::publishProcessedBuffer(
    juce::AudioBuffer<float> &&buffer) {
  const bool canCrossfade =
      ready.load() && processedBuffer.getNumSamples() > 0 &&
      buffer.getNumSamples() > 0;

  if (canCrossfade) {
    previousProcessedBuffer = std::move(processedBuffer);
    swapFadeRemaining = kBufferSwapFadeSamples;
  } else {
    previousProcessedBuffer.setSize(0, 0, false, false, true);
    swapFadeRemaining = 0;
  }

  processedBuffer = std::move(buffer);
  ready = processedBuffer.getNumSamples() > 0;
}

void RealtimePitchProcessor::invalidate() {
  // Deliberately does NOT clear `ready`. Keep serving the existing cache until
  // its replacement is built, then hand over with a ramp in processBlock().
  // Dropping to passthrough here means every resynthesis commit that lands
  // during playback cuts to the dry input and back.
  const auto generation = invalidateGeneration.fetch_add(1) + 1;

  Project *proj = nullptr;
  juce::AudioBuffer<float> waveformSnapshot;
  int srcSampleRate = 0;

  {
    const juce::ScopedLock sl(bufferLock);
    proj = project;
    if (proj) {
      auto &audioData = proj->getAudioData();
      waveformSnapshot.makeCopyOf(audioData.waveform);
      srcSampleRate = audioData.sampleRate;
    }
  }

  if (!proj) {
    return;
  }

  // Safety check: verify waveform has valid dimensions before accessing
  const int numSamples = waveformSnapshot.getNumSamples();
  const int numChannels = waveformSnapshot.getNumChannels();

  if (numSamples <= 0 || numChannels <= 0) {
    return;
  }

  // Use the already-synthesized waveform from project (updated by
  // resynthesizeIncremental) This avoids duplicate synthesis and ensures
  // consistency with standalone mode
  const int dstSampleRate = static_cast<int>(sampleRate);

  if (srcSampleRate == dstSampleRate || srcSampleRate <= 0) {
    // No resampling needed
    const juce::ScopedLock sl(bufferLock);
    publishProcessedBuffer(std::move(waveformSnapshot));
  } else {
    // Build the host-rate cache in the background. Chaining ownership of the
    // previous worker keeps destruction safe without blocking the caller.
    auto previousWorker = std::move(computeThread);
    computeThread = std::make_unique<std::thread>(
        [this, generation, waveform = std::move(waveformSnapshot),
         srcSampleRate, dstSampleRate,
         previous = std::move(previousWorker)]() mutable {
          if (previous && previous->joinable())
            previous->join();
          if (invalidateGeneration.load() != generation)
            return;

          // r8brain's one-shot converter provides band-limited SRC without
          // adding variable-output streaming state to processBlock().
          auto resampled = AudioResampler::resample(
              waveform, srcSampleRate, dstSampleRate);
          const juce::ScopedLock sl(bufferLock);
          if (invalidateGeneration.load() != generation)
            return;
          publishProcessedBuffer(std::move(resampled));
        });
  }
}
