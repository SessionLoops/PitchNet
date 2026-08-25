#include "PsolaSynthesizer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// Period bounds in samples, derived from the app's MIDI range (C1..F6) with
// margin. They only guard against a wild F0 frame producing an absurd grain.
constexpr double kMinPeriodHz = 25.0;
constexpr double kMaxPeriodHz = 2200.0;

// Grain length for unvoiced runs, where there is no period to be synchronous
// with. Short enough not to smear consonants, long enough to overlap smoothly.
constexpr double kUnvoicedGrainSeconds = 0.006;

constexpr float kMinPitchRatio = 0.25f;
constexpr float kMaxPitchRatio = 4.0f;

bool isCancelled(const std::shared_ptr<std::atomic<bool>> &flag) {
  return flag != nullptr && flag->load();
}

/// Frame index in the ORIGINAL audio for a slice-relative sample.
inline double frameForSliceSample(double sliceSample, int sourceStartSample,
                                  int hopSize) {
  return (sliceSample + static_cast<double>(sourceStartSample)) /
         static_cast<double>(hopSize);
}

float f0AtFrame(const std::vector<float> &f0, double frame) {
  if (f0.empty())
    return 0.0f;
  const int index = std::clamp(static_cast<int>(std::lround(frame)), 0,
                               static_cast<int>(f0.size()) - 1);
  return f0[static_cast<size_t>(index)];
}

bool voicedAtFrame(const std::vector<std::uint8_t> &voiced, double frame) {
  if (voiced.empty())
    return true; // no mask supplied: treat everything as pitched
  const int index = std::clamp(static_cast<int>(std::lround(frame)), 0,
                               static_cast<int>(voiced.size()) - 1);
  return voiced[static_cast<size_t>(index)] != 0;
}

/// Period in samples at a slice-relative position, clamped to sane bounds.
double periodAt(const PsolaSynthesizer::Request &request, double sliceSample) {
  const double frame = frameForSliceSample(sliceSample, request.sourceStartSample,
                                           request.hopSize);
  const double hz = std::clamp(static_cast<double>(f0AtFrame(request.sourceF0, frame)),
                               kMinPeriodHz, kMaxPeriodHz);
  return static_cast<double>(request.sampleRate) / hz;
}

/// Normalised cross-correlation of two equal-length windows of @p x centred on
/// @p centreA and @p centreB. Used to place each pitch mark at the same phase
/// of the period as the one before it, which is what keeps consecutive grains
/// from cancelling when they overlap.
double normalisedCorrelation(const std::vector<float> &x, int centreA,
                             int centreB, int half) {
  const int size = static_cast<int>(x.size());
  double dot = 0.0, energyA = 0.0, energyB = 0.0;

  for (int offset = -half; offset < half; ++offset) {
    const int a = centreA + offset;
    const int b = centreB + offset;
    const double va = (a >= 0 && a < size) ? x[static_cast<size_t>(a)] : 0.0;
    const double vb = (b >= 0 && b < size) ? x[static_cast<size_t>(b)] : 0.0;
    dot += va * vb;
    energyA += va * va;
    energyB += vb * vb;
  }

  const double denominator = std::sqrt(energyA * energyB);
  return denominator > 1.0e-12 ? dot / denominator : 0.0;
}


/// RMS over a window centred on @p centre, clipped to the buffer.
double windowRms(const std::vector<float> &buffer, int centre, int halfWidth) {
  const int size = static_cast<int>(buffer.size());
  const int first = std::max(0, centre - halfWidth);
  const int last = std::min(size, centre + halfWidth);
  if (last <= first)
    return 0.0;

  double sum = 0.0;
  for (int index = first; index < last; ++index) {
    const double value = buffer[static_cast<size_t>(index)];
    sum += value * value;
  }
  return std::sqrt(sum / static_cast<double>(last - first));
}

void applyLocalGainMatch(std::vector<float> &output,
                         const PsolaSynthesizer::Request &request,
                         int numOutSamples) {
  const int numOutputFrames = request.numOutputFrames;
  const int hopSize = request.hopSize;
  if (numOutputFrames <= 0 || hopSize <= 0)
    return;

  const int halfWidth = 2 * hopSize;
  constexpr double kMinRms = 1.0e-6;
  constexpr double kMinGain = 0.25;
  constexpr double kMaxGain = 4.0;
  constexpr int kSmoothingRadius = 2;

  std::vector<double> gain(static_cast<size_t>(numOutputFrames), 1.0);
  for (int frame = 0; frame < numOutputFrames; ++frame) {
    const int outputCentre = frame * hopSize + hopSize / 2;
    const int sourceCentre =
        static_cast<int>(std::lround(request.sourceFrame[static_cast<size_t>(frame)] *
                                     static_cast<double>(hopSize))) -
        request.sourceStartSample + hopSize / 2;

    const double outputRms = windowRms(output, outputCentre, halfWidth);
    const double sourceRms = windowRms(request.source, sourceCentre, halfWidth);

    if (outputRms > kMinRms && sourceRms > kMinRms)
      gain[static_cast<size_t>(frame)] =
          std::clamp(sourceRms / outputRms, kMinGain, kMaxGain);
    else if (sourceRms <= kMinRms)
      gain[static_cast<size_t>(frame)] = 0.0;
  }

  std::vector<double> smoothed(static_cast<size_t>(numOutputFrames), 1.0);
  for (int frame = 0; frame < numOutputFrames; ++frame) {
    const int first = std::max(0, frame - kSmoothingRadius);
    const int last = std::min(numOutputFrames - 1, frame + kSmoothingRadius);
    double sum = 0.0;
    for (int index = first; index <= last; ++index)
      sum += gain[static_cast<size_t>(index)];
    smoothed[static_cast<size_t>(frame)] = sum / static_cast<double>(last - first + 1);
  }

  for (int sample = 0; sample < numOutSamples; ++sample) {
    const double position =
        (static_cast<double>(sample) - 0.5 * hopSize) / static_cast<double>(hopSize);
    const int left = std::clamp(static_cast<int>(std::floor(position)), 0,
                                numOutputFrames - 1);
    const int right = std::min(left + 1, numOutputFrames - 1);
    const double amount = std::clamp(position - static_cast<double>(left), 0.0, 1.0);
    const double frameGain = smoothed[static_cast<size_t>(left)] * (1.0 - amount) +
                             smoothed[static_cast<size_t>(right)] * amount;
    output[static_cast<size_t>(sample)] *= static_cast<float>(frameGain);
  }
}

} // namespace

std::vector<int>
PsolaSynthesizer::detectPitchMarks(const Request &request) {
  std::vector<int> marks;
  const int numSamples = static_cast<int>(request.source.size());
  if (numSamples <= 0 || request.hopSize <= 0)
    return marks;

  const auto &x = request.source;

  auto isVoicedSample = [&](int sample) {
    return voicedAtFrame(request.sourceVoiced,
                         frameForSliceSample(sample, request.sourceStartSample,
                                             request.hopSize));
  };

  int sample = 0;
  while (sample < numSamples) {
    // Find the next voiced run.
    while (sample < numSamples && !isVoicedSample(sample))
      ++sample;
    if (sample >= numSamples)
      break;

    const int runStart = sample;
    while (sample < numSamples && isVoicedSample(sample))
      ++sample;
    const int runEnd = sample;

    const double seedPeriod = periodAt(request, runStart);
    if (runEnd - runStart < static_cast<int>(seedPeriod) * 2)
      continue; // too short to be worth marking; the OLA path covers it

    // Seed on the strongest sample in the first period, so the run starts on a
    // glottal pulse rather than wherever the voiced flag happened to turn on.
    int seed = runStart;
    float best = -1.0f;
    const int seedEnd = std::min(runEnd, runStart + static_cast<int>(seedPeriod));
    for (int index = runStart; index < seedEnd; ++index) {
      const float magnitude = std::abs(x[static_cast<size_t>(index)]);
      if (magnitude > best) {
        best = magnitude;
        seed = index;
      }
    }
    marks.push_back(seed);

    // Walk forward one period at a time, letting correlation correct the
    // prediction. This self-corrects a slightly wrong F0 and, unlike raw peak
    // picking, does not jump an octave when a harmonic is louder than the
    // fundamental.
    int current = seed;
    while (true) {
      const double period = periodAt(request, current);
      const int predicted = current + static_cast<int>(std::lround(period));
      if (predicted >= runEnd)
        break;

      const int searchRadius = std::max(2, static_cast<int>(period * 0.25));
      const int half = std::max(4, static_cast<int>(period * 0.5));

      int bestCandidate = predicted;
      double bestScore = -2.0;
      const int first = std::max(current + 2, predicted - searchRadius);
      const int last = std::min(runEnd - 1, predicted + searchRadius);
      for (int candidate = first; candidate <= last; ++candidate) {
        const double score = normalisedCorrelation(x, current, candidate, half);
        if (score > bestScore) {
          bestScore = score;
          bestCandidate = candidate;
        }
      }

      marks.push_back(bestCandidate);
      current = bestCandidate;
    }
  }

  std::sort(marks.begin(), marks.end());
  marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
  return marks;
}

std::vector<float>
PsolaSynthesizer::render(const Request &request,
                         const std::shared_ptr<std::atomic<bool>> &cancelFlag) {
  const int numOutputFrames = request.numOutputFrames;
  const int hopSize = request.hopSize;

  if (numOutputFrames <= 0 || hopSize <= 0 || request.source.empty())
    return {};
  if (static_cast<int>(request.sourceFrame.size()) < numOutputFrames ||
      static_cast<int>(request.pitchRatio.size()) < numOutputFrames)
    return {};

  const int numOutSamples = numOutputFrames * hopSize;
  const int numSourceSamples = static_cast<int>(request.source.size());
  const auto &x = request.source;

  const auto marks = detectPitchMarks(request);
  if (isCancelled(cancelFlag))
    return {};

  // Grains straddle their placement point, so both ends need room for one.
  const int pad = static_cast<int>(std::lround(
      2.0 * static_cast<double>(request.sampleRate) / kMinPeriodHz));
  const int paddedLength = numOutSamples + 2 * pad;

  std::vector<float> padded(static_cast<size_t>(paddedLength), 0.0f);
  std::vector<float> envelope(static_cast<size_t>(paddedLength), 0.0f);

  const int unvoicedGrainHalf = std::max(
      8, static_cast<int>(kUnvoicedGrainSeconds * request.sampleRate));

  auto sourcePositionAt = [&](double outputSample) {
    const double outputFrame = outputSample / static_cast<double>(hopSize);
    const double sourceFrame = dspSynthesis::sampleSourceFrame(
        request.sourceFrame, outputFrame, numOutputFrames);
    return sourceFrame * static_cast<double>(hopSize) -
           static_cast<double>(request.sourceStartSample);
  };

  auto ratioAt = [&](double outputSample) {
    const double outputFrame = outputSample / static_cast<double>(hopSize);
    return std::clamp(dspSynthesis::samplePitchRatio(request.pitchRatio,
                                                     outputFrame, numOutputFrames),
                      kMinPitchRatio, kMaxPitchRatio);
  };

  // Lay grains along the output timeline. Each one independently asks the time
  // map where it reads from and snaps to the nearest source pitch mark, so a
  // stretch repeats grains and a compression skips them without any special
  // case, and an arbitrary time map needs no extra machinery.
  double outputPosition = -static_cast<double>(pad);
  bool needsAnchor = true;
  const double outputEnd = static_cast<double>(numOutSamples + pad);
  int guard = 0;
  // Pure runaway backstop: the smallest advance the clamp below permits is 4
  // samples, so this is the most grains any valid render can need. Sizing it
  // from a typical advance instead would silently truncate the tail of a
  // high-pitched region into silence.
  const int guardLimit = paddedLength / 4 + 1024;

  while (outputPosition < outputEnd && ++guard < guardLimit) {
    if ((guard & 0xFF) == 0 && isCancelled(cancelFlag))
      return {};

    const double sourcePosition = sourcePositionAt(outputPosition);
    const double frame = frameForSliceSample(sourcePosition,
                                             request.sourceStartSample, hopSize);
    bool voiced = voicedAtFrame(request.sourceVoiced, frame) && !marks.empty();

    int grainCentre = 0;
    int grainHalf = 0;
    double advance = 0.0;
    int index = 0;

    if (voiced) {
      // Nearest pitch mark to where this output instant reads from.
      const auto upper = std::lower_bound(
          marks.begin(), marks.end(), static_cast<int>(std::lround(sourcePosition)));
      index = static_cast<int>(upper - marks.begin());
      if (index >= static_cast<int>(marks.size()))
        index = static_cast<int>(marks.size()) - 1;
      if (index > 0) {
        const double here = std::abs(marks[static_cast<size_t>(index)] - sourcePosition);
        const double before =
            std::abs(marks[static_cast<size_t>(index - 1)] - sourcePosition);
        if (before < here)
          --index;
      }

      // "Nearest" is only meaningful if it is actually near. Inside a marked
      // run the nearest mark is at most half a period away; further than a
      // full period means there is no mark here at all - the pre-roll ahead
      // of the first one, the tail past the last, or a voiced stretch too
      // short for the detector to have marked. Snapping anyway stamps one
      // distant pulse over and over, so hand those frames to the copy path
      // instead, which is what they wanted in the first place.
      const double distance =
          std::abs(static_cast<double>(marks[static_cast<size_t>(index)]) - sourcePosition);
      if (distance > periodAt(request, sourcePosition))
        voiced = false;
      else
        grainCentre = marks[static_cast<size_t>(index)];
    }

    if (voiced) {

      // Local period from the marks themselves rather than from F0: the marks
      // were correlation-refined, so they are the better estimate by now.
      //
      // But only when they agree with F0 to within an octave either way. The
      // neighbouring mark at the last pulse of a voiced run belongs to the
      // NEXT run, so its "spacing" is the whole breath in between - thousands
      // of samples. Taken literally that builds one enormous grain, smears a
      // single pulse across the gap and over the following note, and advances
      // the output timeline clean past everything in between. Heard as the
      // previous note doubling under the next one at its original pitch.
      const double f0Period = periodAt(request, grainCentre);
      double period = f0Period;
      if (marks.size() >= 2) {
        const int neighbour = index + 1 < static_cast<int>(marks.size()) ? index + 1
                                                                        : index - 1;
        const double spacing = std::abs(
            static_cast<double>(marks[static_cast<size_t>(neighbour)] - grainCentre));
        if (spacing > 1.0 && spacing > 0.5 * f0Period && spacing < 2.0 * f0Period)
          period = spacing;
      }

      const double synthesisPeriod = period / ratioAt(outputPosition);

      // Two analysis periods, always - never the synthesis period. The
      // window's zeros land exactly on the neighbouring pulses, which is what
      // leaves one pulse per grain and is the entire mechanism: emit those
      // pulses at a new spacing and the pitch moves while the waveform inside
      // each pulse, and so the formants, stay untouched.
      //
      // Widening the grain to guarantee overlap when lowering the pitch looks
      // tempting and is wrong. The neighbouring pulses come off the zeros,
      // the envelope division below restores them to full amplitude, and the
      // shift silently vanishes - measured: an octave down came back reading
      // as no shift at all.
      grainHalf = std::max(4, static_cast<int>(std::lround(period)));
      advance = synthesisPeriod;
    } else {
      // No period to be synchronous with: fixed grains straight from the time
      // map. At unity ratio and an identity map this is a plain copy, which is
      // what preserves consonants and breath.
      grainCentre = static_cast<int>(std::lround(sourcePosition));
      grainHalf = unvoicedGrainHalf;
      advance = static_cast<double>(unvoicedGrainHalf);
    }

    const double maxAdvance =
        2.0 * static_cast<double>(request.sampleRate) / kMinPeriodHz;
    advance = std::clamp(advance, 4.0, maxAdvance);

    // Set the phase at the start of each voiced run, then leave it alone.
    //
    // Snapping to the nearest mark moves a grain by up to half a period in the
    // source, and without undoing that at the onset the whole run slides by a
    // sub-period offset - inaudible alone, but this audio is crossfaded
    // against untouched original samples at the region edges, where an offset
    // comb-filters the splice.
    //
    // Only at the onset, though. Grain spacing drifting away from the analysis
    // marks is not an error, it IS the pitch shift; correcting it every grain
    // pins the output timeline to the source and cancels the shift entirely.
    // So: anchor once, then accumulate. At a ratio of 1.0 the accumulation
    // equals the mark spacing, so the alignment holds for free and the pass
    // reduces to a copy.
    double placementPosition = outputPosition;
    if (voiced && needsAnchor) {
      const double delta = 0.5 * static_cast<double>(hopSize);
      const double slope = (sourcePositionAt(outputPosition + delta) -
                            sourcePositionAt(outputPosition - delta)) /
                           (2.0 * delta);
      if (slope > 1.0e-3) {
        // The snap error is in source samples, so divide by the local
        // d(source)/d(output) to land back in output samples.
        const double correction = (static_cast<double>(grainCentre) - sourcePosition) / slope;

        // Only anchor once a grain is actually near a mark. In the pre-roll,
        // before the first mark, the nearest one can be thousands of samples
        // away; consuming the anchor on that leaves the run permanently
        // offset. Inside the marked region the nearest mark is always within
        // half a period, so this fires on the first grain that matters.
        if (std::abs(correction) <= static_cast<double>(grainHalf)) {
          placementPosition = outputPosition + correction;
          needsAnchor = false;
        }
      }
    } else if (!voiced) {
      // Re-anchor at the next voiced onset, so a run following a breath or a
      // consonant still lines up with the original.
      needsAnchor = true;
    }

    // Hann grain of length 2 * grainHalf, added at the output position.
    const int placement = static_cast<int>(std::lround(placementPosition)) + pad;
    for (int offset = -grainHalf; offset < grainHalf; ++offset) {
      const int destination = placement + offset;
      if (destination < 0 || destination >= paddedLength)
        continue;
      const int sourceIndex = grainCentre + offset;
      if (sourceIndex < 0 || sourceIndex >= numSourceSamples)
        continue;

      const double phase =
          (static_cast<double>(offset + grainHalf) + 0.5) /
          static_cast<double>(2 * grainHalf);
      const float window = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * phase));

      padded[static_cast<size_t>(destination)] +=
          x[static_cast<size_t>(sourceIndex)] * window;
      envelope[static_cast<size_t>(destination)] += window;
    }

    outputPosition = placementPosition + advance;
  }

  if (isCancelled(cancelFlag))
    return {};

  // Grains overlap by however much the new period differs from the old, so the
  // window sum is not constant. Dividing by it makes the amplitude exact for
  // any ratio - including 1.0, where this reduces to reconstructing the
  // original samples - instead of needing a gain correction afterwards.
  // Floor the divisor rather than only guarding against zero. Lowering the
  // pitch spreads grains until they barely overlap, and at an octave down they
  // tile edge to edge so the envelope touches zero at every seam. Dividing by
  // what is left there amplifies almost nothing into something audible; using
  // a floor instead leaves a smooth amplitude dip, which is the honest
  // consequence of asking for more periods than the source contains. Well
  // overlapped regions sit near or above 1.0, so this never engages at
  // moderate ratios and never touches the unity case.
  constexpr float kMinEnvelope = 0.3f;
  std::vector<float> output(static_cast<size_t>(numOutSamples), 0.0f);
  for (int sample = 0; sample < numOutSamples; ++sample) {
    const size_t index = static_cast<size_t>(sample + pad);
    const float weight = envelope[index];
    if (weight > kMinEnvelope)
      output[static_cast<size_t>(sample)] = padded[index] / weight;
  }

  // Match the source's local loudness.
  //
  // Raising the pitch repeats each grain at closer spacing, and the repeats
  // are offset copies rather than genuinely periodic content, so the parts
  // away from the pulse partially cancel - about 3 dB at an octave. The
  // window-envelope division above cannot see that, because the loss is in
  // the content, not the weighting. Downstream this is crossfaded against
  // untouched original samples, so the levels have to agree locally.
  //
  // At a ratio of 1.0 every gain here computes to exactly 1.0, so the
  // bit-exact reconstruction survives.
  applyLocalGainMatch(output, request, numOutSamples);

  return output;
}

namespace {

struct AsyncTask {
  PsolaSynthesizer::Request request;
  std::function<void(std::vector<float>)> callback;
  std::shared_ptr<std::atomic<bool>> cancelFlag;
};

/// One shared worker for every PSOLA render, matching the phase vocoder's
/// arrangement: renders are already serialised by IncrementalSynthesizer
/// cancelling the previous job, so a single long-lived thread avoids spawning
/// one per keystroke.
class RenderWorker {
public:
  static RenderWorker &get() {
    static RenderWorker instance;
    return instance;
  }

  void post(AsyncTask task) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push_back(std::move(task));
    }
    condition.notify_one();
  }

private:
  RenderWorker() { thread = std::thread([this]() { run(); }); }

  ~RenderWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      shuttingDown = true;
    }
    condition.notify_all();
    if (thread.joinable())
      thread.join();
  }

  void run() {
    for (;;) {
      AsyncTask task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this]() { return shuttingDown || !queue.empty(); });
        if (shuttingDown)
          return;
        task = std::move(queue.front());
        queue.pop_front();
      }

      std::vector<float> result;
      if (!isCancelled(task.cancelFlag))
        result = PsolaSynthesizer::render(task.request, task.cancelFlag);

      if (task.callback)
        task.callback(std::move(result));
    }
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::deque<AsyncTask> queue;
  bool shuttingDown = false;
  std::thread thread;
};

} // namespace

PsolaSynthesizer::PsolaSynthesizer() = default;
PsolaSynthesizer::~PsolaSynthesizer() = default;

void PsolaSynthesizer::renderAsync(
    Request request, std::function<void(std::vector<float>)> callback,
    std::shared_ptr<std::atomic<bool>> cancelFlag) {
  RenderWorker::get().post(
      AsyncTask{std::move(request), std::move(callback), std::move(cancelFlag)});
}
