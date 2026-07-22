#include "ResampledLoopAudition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
constexpr float kAuditionGain = 0.5011872336f; // -6 dB

int findPositiveZeroCrossing(const float *samples, int start, int end,
                             bool searchForward)
{
  if (end - start < 2)
    return -1;

  if (searchForward)
  {
    for (int i = start + 1; i < end; ++i)
      if (samples[i - 1] <= 0.0f && samples[i] > 0.0f)
        return i;
  }
  else
  {
    for (int i = end - 1; i > start; --i)
      if (samples[i - 1] <= 0.0f && samples[i] > 0.0f)
        return i;
  }
  return -1;
}

struct PeriodEstimate
{
  int period = 0;
  float correlation = -1.0f;
};

PeriodEstimate estimatePitchPeriod(const float *samples, int start, int end)
{
  constexpr int kMinPeriod = 28;   // ~1.6 kHz at 44.1 kHz
  constexpr int kMaxPeriod = 1200; // ~37 Hz at 44.1 kHz
  const int length = end - start;
  if (length < kMinPeriod * 4)
    return {};

  const int maxPeriod = std::min(kMaxPeriod, length / 3);
  float bestCorrelation = -1.0f;
  int bestPeriod = 0;
  for (int lag = kMinPeriod; lag <= maxPeriod; ++lag)
  {
    double correlation = 0.0;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (int i = start; i < end - lag; ++i)
    {
      const float left = samples[i];
      const float right = samples[i + lag];
      correlation += left * right;
      leftEnergy += left * left;
      rightEnergy += right * right;
    }
    const double energy = std::sqrt(leftEnergy * rightEnergy);
    if (energy <= 1.0e-12)
      continue;
    const float normalized = static_cast<float>(correlation / energy);
    if (normalized > bestCorrelation)
    {
      bestCorrelation = normalized;
      bestPeriod = lag;
    }
  }
  return bestCorrelation >= 0.35f
             ? PeriodEstimate{bestPeriod, bestCorrelation}
             : PeriodEstimate{};
}

struct LoopPoints
{
  int start = -1;
  int end = -1;
};

LoopPoints findPeriodicLoop(const float *samples, int numSamples,
                            float targetMidiNote)
{
  LoopPoints best;
  double bestScore = -std::numeric_limits<double>::max();

  // Short windows favour a compact, stable tone; longer ones reject a brief
  // coincidental match.  Select the most periodic voiced region, rather than
  // assuming that the middle of every note is the cleanest section.
  constexpr float kG3MidiNote = 55.0f;
  const std::array<int, 1> kWindowLengths = {
      targetMidiNote <= kG3MidiNote ? 4096 : 2048};
  const int searchStart = numSamples / 8;
  const int searchEnd = (numSamples * 7) / 8;
  for (const int targetLength : kWindowLengths)
  {
    const int windowLength = std::min(targetLength, searchEnd - searchStart);
    if (windowLength < 28 * 4)
      continue;

    const int windowStep = std::max(128, windowLength / 8);
    for (int windowStart = searchStart;
         windowStart + windowLength <= searchEnd;
         windowStart += windowStep)
    {
      const auto estimate = estimatePitchPeriod(
          samples, windowStart, windowStart + windowLength);
      if (estimate.period <= 0)
        continue;

      const int period = estimate.period;
      const int compareLength = std::min(1024, period * 3);
      const int minCycles = 6;
      const int maxCycles = (windowLength - compareLength) / period;
      if (maxCycles < minCycles)
        continue;

      const int startStep = std::max(1, period / 4);
      for (int approximateStart = windowStart;
           approximateStart + minCycles * period + compareLength <
               windowStart + windowLength;
           approximateStart += startStep)
      {
        const int start = findPositiveZeroCrossing(
            samples, approximateStart,
            std::min(windowStart + windowLength,
                     approximateStart + startStep + 1), true);
        if (start < 0)
          continue;

        // Keep the audition loop compact and stationary. Longer loops can
        // retain a note's slow vibrato or amplitude drift, making sustained
        // notes sound less pure than short notes.
        const int cycles = minCycles;
        if (start + cycles * period + compareLength >=
            windowStart + windowLength)
          continue;

        const int end = start + cycles * period;
        double mismatch = 0.0;
        double energy = 0.0;
        for (int i = 0; i < compareLength; ++i)
        {
          const double delta = samples[start + i] - samples[end + i];
          mismatch += delta * delta;
          energy += samples[start + i] * samples[start + i] +
                    samples[end + i] * samples[end + i];
        }
        const double normalizedMismatch = mismatch / std::max(energy, 1.0e-12);
        // Correlation establishes that a candidate is voiced and periodic;
        // after that, prefer continuity at the loop join. This avoids a
        // nominally periodic segment whose slow envelope change produces a
        // rough or fluctuating sustained audition.
        const double continuity =
            1.0 - std::min(1.0, normalizedMismatch);
        const double score = 0.25 * estimate.correlation + 0.75 * continuity;
        if (score > bestScore)
        {
          bestScore = score;
          best = {start, end};
        }
      }
    }
  }
  return best;
}
}

ResampledLoopAudition::ResampledLoopAudition(ReadyCallback callback)
    : onReady(std::move(callback))
{
  // Start the worker only after every member it may access has been
  // constructed. Starting it from the member-initializer list allowed the
  // thread to enter workerLoop() before mutex/condition were initialized.
  worker = std::thread(&ResampledLoopAudition::workerLoop, this);
}

ResampledLoopAudition::~ResampledLoopAudition()
{
  {
    const std::lock_guard<std::mutex> lock(mutex);
    shuttingDown = true;
    hasPendingRequest = false;
    ++requestId;
  }
  condition.notify_one();
  if (worker.joinable())
    worker.join();
}

void ResampledLoopAudition::request(std::vector<float> source,
                                    float semitoneShift,
                                    float targetMidiNote)
{
  if (source.empty())
    return;

  {
    const std::lock_guard<std::mutex> lock(mutex);
    pendingSource = std::move(source);
    pendingSemitoneShift = semitoneShift;
    pendingTargetMidiNote = targetMidiNote;
    hasPendingRequest = true;
    ++requestId;
  }
  condition.notify_one();
}

void ResampledLoopAudition::cancel()
{
  const std::lock_guard<std::mutex> lock(mutex);
  hasPendingRequest = false;
  ++requestId;
}

void ResampledLoopAudition::workerLoop()
{
  for (;;)
  {
    std::vector<float> source;
    float semitoneShift = 0.0f;
    float targetMidiNote = 60.0f;
    std::uint64_t id = 0;
    {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [this] { return shuttingDown || hasPendingRequest; });
      if (shuttingDown)
        return;

      source = std::move(pendingSource);
      semitoneShift = pendingSemitoneShift;
      targetMidiNote = pendingTargetMidiNote;
      id = requestId;
      hasPendingRequest = false;
    }

    auto result = render(std::move(source), semitoneShift, targetMidiNote);
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (shuttingDown)
        return;
    }

    auto callback = onReady;
    juce::MessageManager::callAsync(
        [callback = std::move(callback), result = std::move(result)]() mutable
        {
          if (callback)
            callback(std::move(result));
        });
  }
}

juce::AudioBuffer<float>
ResampledLoopAudition::render(std::vector<float> source,
                              float semitoneShift,
                              float targetMidiNote)
{
  const int originalSamples = static_cast<int>(source.size());
  if (originalSamples < 2)
    return {};

  // Drag audition needs only a small, loopable portion.  Simple resampling
  // changes pitch directly and avoids time-stretching dependencies. A
  // positive shift consumes the source faster, producing a
  // shorter, higher-pitched loop; a negative shift does the inverse.
  const double pitchRatio = std::pow(2.0, static_cast<double>(semitoneShift) / 12.0);
  if (!std::isfinite(pitchRatio) || pitchRatio <= 0.0)
    return {};
  const int resampledSamples = std::max(
      2, static_cast<int>(std::lround(originalSamples / pitchRatio)));
  juce::AudioBuffer<float> result(1, resampledSamples);
  auto *output = result.getWritePointer(0);
  for (int i = 0; i < resampledSamples; ++i)
  {
    const double position = std::min(
        static_cast<double>(originalSamples - 1), i * pitchRatio);
    const int left = static_cast<int>(position);
    const int right = std::min(originalSamples - 1, left + 1);
    const float fraction = static_cast<float>(position - left);
    output[i] = source[left] + fraction * (source[right] - source[left]);
  }

  float peak = 0.0f;
  for (int i = 0; i < resampledSamples; ++i)
  {
    const float sample = result.getSample(0, i);
    if (!std::isfinite(sample))
      return {};
    peak = std::max(peak, std::abs(sample));
  }
  if (peak < 1.0e-7f)
    return {};

  // Audition is intentionally quieter than program playback.
  result.applyGain(kAuditionGain);

  // Autocorrelation finds the voiced period in the sustained part of the
  // note. Choose an integer-period loop whose two boundaries have the lowest
  // waveform mismatch; this is substantially smoother than a zero-crossing
  // heuristic alone.
  const auto *samples = result.getReadPointer(0);
  if (const auto periodicLoop = findPeriodicLoop(samples, resampledSamples,
                                                 targetMidiNote);
      periodicLoop.start >= 0 && periodicLoop.end > periodicLoop.start)
  {
    juce::AudioBuffer<float> loop(1, periodicLoop.end - periodicLoop.start);
    loop.copyFrom(0, 0, result, 0, periodicLoop.start,
                  loop.getNumSamples());
    return loop;
  }

  // Fallback for unvoiced/noisy material: a later sustained portion bounded
  // at positive-going zero crossings avoids repeatedly looping the onset.
  const int preferredStart = resampledSamples / 4;
  const int preferredEnd = std::max(preferredStart + 2048,
                                    (resampledSamples * 3) / 4);
  const int boundarySearch = std::min(4096, resampledSamples / 8);
  const int loopStart = findPositiveZeroCrossing(
      samples, preferredStart,
      std::min(resampledSamples, preferredStart + boundarySearch), true);
  const int loopEnd = findPositiveZeroCrossing(
      samples, std::max(loopStart + 2048, preferredEnd - boundarySearch),
      std::min(resampledSamples, preferredEnd + boundarySearch),
      false);
  if (loopStart >= 0 && loopEnd > loopStart + 2048)
  {
    juce::AudioBuffer<float> loop(1, loopEnd - loopStart);
    loop.copyFrom(0, 0, result, 0, loopStart, loop.getNumSamples());
    return loop;
  }
  return result;
}
