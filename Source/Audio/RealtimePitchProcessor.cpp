#include "RealtimePitchProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

RealtimePitchProcessor::RealtimePitchProcessor() = default;

RealtimePitchProcessor::~RealtimePitchProcessor() {
  cancelCompute = true;
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

void RealtimePitchProcessor::setVocoder(Vocoder *voc) {
  {
    const juce::ScopedLock sl(bufferLock);
    vocoder = voc;
  }
  // Don't call invalidate() here - wait for project to be set first
  // invalidate() will be called by setProject() or explicitly
}

void RealtimePitchProcessor::prepareToPlay(double sr, int) {
  sampleRate = sr;
  position.store(0.0);
  readPosition = 0;
  readPositionValid = false;
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

  // Passthrough if not ready
  if (!ready.load()) {
    output.makeCopyOf(input);
    readPositionValid = false; // resync to host when playback resumes
    return false;
  }

  const int numSamples = output.getNumSamples();
  const int numChannels = output.getNumChannels();

  // Follow the host position contiguously. During normal playback the host
  // advances by exactly numSamples per block, so we keep our own integer read
  // cursor and only resync to the host on a genuine jump (seek/loop). This
  // keeps the read pointer sample-accurate and free of boundary clicks even if
  // the host's reported time has sub-sample jitter.
  const juce::int64 resyncTolerance = juce::jmax<juce::int64>(64, numSamples);
  juce::int64 posSamples;
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
  }

  return true;
}

void RealtimePitchProcessor::invalidate() {
  ready = false;


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
    processedBuffer.makeCopyOf(waveformSnapshot);
    ready = true;
  } else {
    // Resample to host sample rate
    const double ratio = static_cast<double>(srcSampleRate) / dstSampleRate;
    const int srcSamples = waveformSnapshot.getNumSamples();
    const int dstSamples = static_cast<int>(srcSamples / ratio);
    const int numChannels = waveformSnapshot.getNumChannels();

    juce::AudioBuffer<float> resampled(numChannels, dstSamples);

    for (int ch = 0; ch < numChannels; ++ch) {
      const float *src = waveformSnapshot.getReadPointer(ch);
      float *dst = resampled.getWritePointer(ch);

      for (int i = 0; i < dstSamples; ++i) {
        const double srcPos = i * ratio;
        const int srcIndex = static_cast<int>(srcPos);
        const double frac = srcPos - srcIndex;

        if (srcIndex + 1 < srcSamples)
          dst[i] = static_cast<float>(src[srcIndex] * (1.0 - frac) +
                                      src[srcIndex + 1] * frac);
        else if (srcIndex < srcSamples)
          dst[i] = src[srcIndex];
        else
          dst[i] = 0.0f;
      }
    }

    const juce::ScopedLock sl(bufferLock);
    processedBuffer = std::move(resampled);
    ready = true;
  }
}

void RealtimePitchProcessor::startComputation() {
  // Cancel previous computation
  cancelCompute = true;
  if (computeThread && computeThread->joinable()) {
    // Don't block - start new thread that waits for old one
    auto oldThread = std::move(computeThread);
    cancelCompute = false;
    computing = true;

    computeThread = std::make_unique<std::thread>(
        [this, old = std::move(oldThread)]() mutable {
          if (old && old->joinable())
            old->join();
          if (!cancelCompute.load())
            computeInBackground();
        });
  } else {
    cancelCompute = false;
    computing = true;
    computeThread =
        std::make_unique<std::thread>([this]() { computeInBackground(); });
  }
}

void RealtimePitchProcessor::computeInBackground() {

  Project *proj = nullptr;
  Vocoder *voc = nullptr;
  std::vector<std::vector<float>> melSnapshot;
  std::vector<float> adjustedF0Snapshot;
  int numChannelsSnapshot = 1;
  float volumeDbSnapshot = 0.0f;

  {
    const juce::ScopedLock sl(bufferLock);
    proj = project;
    voc = vocoder;

    if (proj) {
      auto &audioData = proj->getAudioData();
      melSnapshot = audioData.melSpectrogram;
      numChannelsSnapshot = std::max(1, audioData.waveform.getNumChannels());
      volumeDbSnapshot = proj->getVolume();
      adjustedF0Snapshot = proj->getAdjustedF0();
    }
  }

  if (!proj || !voc || !voc->isLoaded()) {
    computing = false;
    return;
  }

  if (melSnapshot.empty()) {
    computing = false;
    return;
  }


  if (adjustedF0Snapshot.empty() ||
      adjustedF0Snapshot.size() != melSnapshot.size()) {
    computing = false;
    return;
  }

  if (cancelCompute.load()) {
    computing = false;
    return;
  }

  // Synthesize
  std::vector<float> synthesized;
  try {
    synthesized = voc->infer(melSnapshot, adjustedF0Snapshot);
  } catch (...) {
    computing = false;
    return;
  }


  if (cancelCompute.load() || synthesized.empty()) {
    computing = false;
    return;
  }

  // Create output buffer
  int numChannels = numChannelsSnapshot;
  int numSamples = static_cast<int>(synthesized.size());

  juce::AudioBuffer<float> output(numChannels, numSamples);
  for (int ch = 0; ch < numChannels; ++ch)
    for (int i = 0; i < numSamples; ++i)
      output.setSample(ch, i, synthesized[i]);

  // Apply volume
  float volumeDb = volumeDbSnapshot;
  if (volumeDb != 0.0f)
    output.applyGain(std::pow(10.0f, volumeDb / 20.0f));

  // Update buffer directly (with lock)
  if (!cancelCompute.load()) {
    const juce::ScopedLock sl(bufferLock);
    processedBuffer = std::move(output);
    ready = true;
  }
  computing = false;
}
