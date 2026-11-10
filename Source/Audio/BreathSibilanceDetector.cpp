#include "BreathSibilanceDetector.h"

#include "Polyphonic/PolyFFT.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace breathsibilance
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kFar = std::numeric_limits<int>::max() / 2;

// Frames this quiet are digital silence (the level unit is the windowed FFT
// power, where full-scale noise sits near +45 dB). Excluded from the
// background estimate so leading or trailing silence cannot drag it down.
constexpr float kDigitalSilenceDb = -100.0f;

// Periodicity search range (Hz) and the low-pass applied before it.
constexpr double kPeriodicityMinHz = 60.0;
constexpr double kPeriodicityMaxHz = 1100.0;
constexpr double kPeriodicityLowPassHz = 4000.0;

int nextPowerOfTwo(int n)
{
  int p = 1;
  while (p < n)
    p <<= 1;
  return p;
}

int scaledEvenLength(int lengthAt44k, double sampleRate)
{
  const int scaled = static_cast<int>(
      std::lround(static_cast<double>(lengthAt44k) * sampleRate / 44100.0));
  return std::max(16, scaled / 2 * 2);
}

std::vector<double> periodicHann(int length)
{
  std::vector<double> window(static_cast<size_t>(length));
  for (int i = 0; i < length; ++i)
    window[static_cast<size_t>(i)] =
        0.5 - 0.5 * std::cos(2.0 * kPi * i / length);
  return window;
}

float toDb(double energy)
{
  return static_cast<float>(10.0 * std::log10(energy + 1e-20));
}

// numpy.percentile with linear interpolation.
float percentile(std::vector<float> values, float pct)
{
  if (values.empty())
    return 0.0f;
  const double position =
      static_cast<double>(pct) / 100.0 * static_cast<double>(values.size() - 1);
  const size_t lower = static_cast<size_t>(std::floor(position));
  std::nth_element(values.begin(), values.begin() + static_cast<long>(lower),
                   values.end());
  const float low = values[lower];
  if (lower + 1 >= values.size())
    return low;
  const float high =
      *std::min_element(values.begin() + static_cast<long>(lower) + 1,
                        values.end());
  return low + static_cast<float>(position - static_cast<double>(lower)) *
                   (high - low);
}

// The window of `length` samples centred on `centre`; zero outside the take.
void readWindow(const float *samples, int numSamples, int centre, int length,
                float *out)
{
  const int start = centre - length / 2;
  for (int i = 0; i < length; ++i)
  {
    const int index = start + i;
    out[i] = index >= 0 && index < numSamples ? samples[index] : 0.0f;
  }
}

struct FrameSpectrum
{
  double low = 0.0, mid = 0.0, high = 0.0;
  float centroid = 0.0f;
};

/** Band energies and centroid of one Hann-windowed frame. */
class SpectralAnalyser
{
public:
  SpectralAnalyser(const Config &config, double sampleRate)
      : length(scaledEvenLength(config.spectralWindow, sampleRate)),
        fft(nextPowerOfTwo(length)), window(periodicHann(length)),
        frame(static_cast<size_t>(fft.size()), 0.0f),
        spectrum(static_cast<size_t>(fft.numBins()))
  {
    const double binHz = sampleRate / fft.size();
    const double top =
        std::min(static_cast<double>(config.highBandEnd), 0.475 * sampleRate);
    band.assign(static_cast<size_t>(fft.numBins()), -1);
    frequency.assign(static_cast<size_t>(fft.numBins()), 0.0);
    for (int k = 0; k < fft.numBins(); ++k)
    {
      const double f = k * binHz;
      frequency[static_cast<size_t>(k)] = f;
      if (f >= config.lowBandStart && f < config.midBandStart)
        band[static_cast<size_t>(k)] = 0;
      else if (f >= config.midBandStart && f < config.highBandStart)
        band[static_cast<size_t>(k)] = 1;
      else if (f >= config.highBandStart && f < top)
        band[static_cast<size_t>(k)] = 2;
    }
  }

  FrameSpectrum analyse(const float *samples, int numSamples, int centre)
  {
    std::fill(frame.begin(), frame.end(), 0.0f);
    readWindow(samples, numSamples, centre, length, frame.data());
    for (int i = 0; i < length; ++i)
      frame[static_cast<size_t>(i)] = static_cast<float>(
          frame[static_cast<size_t>(i)] * window[static_cast<size_t>(i)]);
    fft.forward(frame.data(), spectrum.data());

    FrameSpectrum result;
    double weighted = 0.0;
    for (size_t k = 0; k < spectrum.size(); ++k)
    {
      const int b = band[k];
      if (b < 0)
        continue;
      const double power = static_cast<double>(std::norm(spectrum[k]));
      (b == 0 ? result.low : b == 1 ? result.mid : result.high) += power;
      weighted += power * frequency[k];
    }
    const double total = result.low + result.mid + result.high;
    result.centroid = static_cast<float>(weighted / (total + 1e-20));
    return result;
  }

private:
  int length;
  polyphonic::RealFFT fft;
  std::vector<double> window;
  std::vector<int> band;
  std::vector<double> frequency;
  std::vector<float> frame;
  std::vector<std::complex<float>> spectrum;
};

/**
 * Normalised autocorrelation peak over 60-1100 Hz of a low-passed frame,
 * corrected for the window's own autocorrelation (Boersma 1993). Near 1 for
 * a periodic voice, well below for noise. Only evaluated where a decision
 * depends on it.
 */
class PeriodicityAnalyser
{
public:
  PeriodicityAnalyser(const Config &config, double sampleRate)
      : length(scaledEvenLength(config.periodicityWindow, sampleRate)),
        fft(nextPowerOfTwo(2 * length)), window(periodicHann(length)),
        frame(static_cast<size_t>(fft.size()), 0.0f),
        spectrum(static_cast<size_t>(fft.numBins())),
        correlation(static_cast<size_t>(fft.size()), 0.0f)
  {
    const double binHz = sampleRate / fft.size();
    lowPassBin = static_cast<int>(std::floor(kPeriodicityLowPassHz / binHz));
    minLag = static_cast<int>(sampleRate / kPeriodicityMaxHz);
    maxLag = std::min(static_cast<int>(sampleRate / kPeriodicityMinHz),
                      length / 2);

    // Autocorrelation of the window itself, for the bias correction.
    std::fill(frame.begin(), frame.end(), 0.0f);
    for (int i = 0; i < length; ++i)
      frame[static_cast<size_t>(i)] =
          static_cast<float>(window[static_cast<size_t>(i)]);
    fft.forward(frame.data(), spectrum.data());
    for (auto &bin : spectrum)
      bin = {std::norm(bin), 0.0f};
    fft.inverse(spectrum.data(), correlation.data());
    windowCorrelation.assign(correlation.begin(),
                             correlation.begin() + length);
  }

  float analyse(const float *samples, int numSamples, int centre)
  {
    if (maxLag <= minLag)
      return 0.0f;
    std::fill(frame.begin(), frame.end(), 0.0f);
    readWindow(samples, numSamples, centre, length, frame.data());
    double mean = 0.0;
    for (int i = 0; i < length; ++i)
      mean += frame[static_cast<size_t>(i)];
    mean /= length;
    for (int i = 0; i < length; ++i)
      frame[static_cast<size_t>(i)] = static_cast<float>(
          (frame[static_cast<size_t>(i)] - mean) *
          window[static_cast<size_t>(i)]);

    fft.forward(frame.data(), spectrum.data());
    for (size_t k = 0; k < spectrum.size(); ++k)
      spectrum[k] = static_cast<int>(k) > lowPassBin
                        ? std::complex<float>{}
                        : std::complex<float>{std::norm(spectrum[k]), 0.0f};
    fft.inverse(spectrum.data(), correlation.data());

    const double zeroLag = static_cast<double>(correlation[0]) + 1e-12;
    const double windowZeroLag = windowCorrelation[0];
    // The result is clamped to [0, 1], so 0 is a safe floor. No infinity:
    // Release builds compile this file with -ffast-math, where it is UB.
    double best = 0.0;
    for (int lag = minLag; lag < maxLag; ++lag)
    {
      const double windowRatio =
          windowCorrelation[static_cast<size_t>(lag)] / windowZeroLag;
      if (windowRatio <= 0.0)
        continue;
      best = std::max(best, (correlation[static_cast<size_t>(lag)] / zeroLag) /
                                windowRatio);
    }
    return static_cast<float>(std::clamp(best, 0.0, 1.0));
  }

private:
  int length;
  polyphonic::RealFFT fft;
  std::vector<double> window;
  std::vector<float> frame;
  std::vector<std::complex<float>> spectrum;
  std::vector<float> correlation;
  std::vector<double> windowCorrelation;
  int lowPassBin = 0;
  int minLag = 0;
  int maxLag = 0;
};

int secondsToFrames(double seconds, double sampleRate, int hopSize)
{
  return std::max(
      0, static_cast<int>(std::lround(seconds * sampleRate / hopSize)));
}

// Calls visit(start, end) for every maximal run [start, end) of frames in
// [0, count) where inRun(frame) holds.
template <typename Predicate, typename Visitor>
void forEachRun(int count, Predicate inRun, Visitor visit)
{
  int frame = 0;
  while (frame < count)
  {
    if (!inRun(frame))
    {
      ++frame;
      continue;
    }
    const int start = frame;
    while (frame < count && inRun(frame))
      ++frame;
    visit(start, frame);
  }
}
} // namespace

std::vector<std::uint8_t> detect(const float *samples, int numSamples,
                                 double sampleRate, int hopSize,
                                 int numFrames,
                                 const std::vector<bool> &voicedMask,
                                 const Config &config)
{
  std::vector<std::uint8_t> labels(
      static_cast<size_t>(std::max(0, numFrames)), None);
  if (samples == nullptr || numSamples <= 0 || numFrames <= 0 ||
      sampleRate <= 0.0 || hopSize <= 0)
    return labels;

  const auto frameCount = static_cast<size_t>(numFrames);
  std::vector<std::uint8_t> voiced(frameCount, 0);
  for (size_t i = 0; i < std::min(frameCount, voicedMask.size()); ++i)
    voiced[i] = voicedMask[i] ? 1 : 0;

  // 1) Per-frame band energies.
  SpectralAnalyser spectral(config, sampleRate);
  std::vector<float> level(frameCount), highDb(frameCount),
      midHighDb(frameCount), highShare(frameCount), lowShare(frameCount),
      centroid(frameCount);
  for (int frame = 0; frame < numFrames; ++frame)
  {
    const auto s = spectral.analyse(samples, numSamples, frame * hopSize);
    const double total = s.low + s.mid + s.high + 1e-20;
    const auto i = static_cast<size_t>(frame);
    level[i] = toDb(total);
    highDb[i] = toDb(s.high);
    midHighDb[i] = toDb(s.mid + s.high);
    highShare[i] = static_cast<float>(s.high / total);
    lowShare[i] = static_cast<float>(s.low / total);
    centroid[i] = s.centroid;
  }

  // 2) The take's background per band, and its vocal level.
  std::vector<float> audibleHigh, audibleMidHigh, audibleLevel, voicedLevel;
  for (size_t i = 0; i < frameCount; ++i)
  {
    if (level[i] <= kDigitalSilenceDb)
      continue;
    audibleHigh.push_back(highDb[i]);
    audibleMidHigh.push_back(midHighDb[i]);
    audibleLevel.push_back(level[i]);
    if (voiced[i])
      voicedLevel.push_back(level[i]);
  }
  if (audibleLevel.size() < 10)
    return labels;
  const float highFloor = percentile(audibleHigh, config.floorPercentile);
  const float midHighFloor =
      percentile(audibleMidHigh, config.floorPercentile);
  const float reference =
      percentile(voicedLevel.size() > 10 ? voicedLevel : audibleLevel,
                 config.referencePercentile);

  // 3) Frame classes. Periodicity costs two FFTs, so it is only measured
  //    where the answer can change the class.
  PeriodicityAnalyser periodicity(config, sampleRate);
  std::vector<std::uint8_t> frameClass(frameCount, None);
  for (int frame = 0; frame < numFrames; ++frame)
  {
    const auto i = static_cast<size_t>(frame);
    const bool loudEnoughForSibilance =
        highShare[i] >= config.sibilanceHighShare &&
        highDb[i] > highFloor + config.sibilanceAboveFloorDb &&
        level[i] > reference - config.sibilanceBelowReferenceDb;
    if (loudEnoughForSibilance)
    {
      if (!voiced[i] ||
          (highShare[i] >= config.voicedOverrideHighShare &&
           periodicity.analyse(samples, numSamples, frame * hopSize) <
               config.voicedOverridePeriodicity))
      {
        frameClass[i] = Sibilance;
        continue;
      }
    }

    if (voiced[i])
      continue;
    const bool breathLike =
        midHighDb[i] > midHighFloor + config.breathAboveFloorDb &&
        level[i] > reference - config.breathBelowReferenceDb &&
        centroid[i] >= config.breathCentroidMin &&
        centroid[i] <= config.breathCentroidMax &&
        lowShare[i] <= config.breathMaxLowShare;
    if (breathLike &&
        periodicity.analyse(samples, numSamples, frame * hopSize) <
            config.breathMaxPeriodicity)
      frameClass[i] = Breath;
  }

  // 4) Noise regions: classified frames, joined across short unvoiced gaps.
  const int bridge =
      secondsToFrames(config.bridgeSeconds, sampleRate, hopSize);
  std::vector<std::uint8_t> inRegion(frameCount, 0);
  int previousNoise = -1;
  for (int frame = 0; frame < numFrames; ++frame)
  {
    if (frameClass[static_cast<size_t>(frame)] == None)
      continue;
    inRegion[static_cast<size_t>(frame)] = 1;
    const int gap = frame - previousNoise;
    if (previousNoise >= 0 && gap > 1 && gap <= bridge + 1)
    {
      const bool gapVoiced =
          std::any_of(voiced.begin() + previousNoise + 1,
                      voiced.begin() + frame,
                      [](std::uint8_t v) { return v != 0; });
      if (!gapVoiced)
        std::fill(inRegion.begin() + previousNoise + 1,
                  inRegion.begin() + frame, 1);
    }
    previousNoise = frame;
  }

  // Nearest voiced frame on either side, for the distance rules.
  std::vector<int> previousVoiced(frameCount, -1), nextVoiced(frameCount, -1);
  for (int frame = 0, last = -1; frame < numFrames; ++frame)
  {
    if (voiced[static_cast<size_t>(frame)])
      last = frame;
    previousVoiced[static_cast<size_t>(frame)] = last;
  }
  for (int frame = numFrames - 1, next = -1; frame >= 0; --frame)
  {
    if (voiced[static_cast<size_t>(frame)])
      next = frame;
    nextVoiced[static_cast<size_t>(frame)] = next;
  }
  const auto distanceToVoiced = [&](int start, int end) {
    int distance = kFar;
    if (start > 0 && previousVoiced[static_cast<size_t>(start - 1)] >= 0)
      distance = start - previousVoiced[static_cast<size_t>(start - 1)];
    if (end < numFrames && nextVoiced[static_cast<size_t>(end)] >= 0)
      distance =
          std::min(distance, nextVoiced[static_cast<size_t>(end)] - (end - 1));
    return distance;
  };
  const auto anyVoiced = [&](int start, int end) {
    start = std::clamp(start, 0, numFrames);
    end = std::clamp(end, start, numFrames);
    return std::any_of(voiced.begin() + start, voiced.begin() + end,
                       [](std::uint8_t v) { return v != 0; });
  };

  const int touch = secondsToFrames(config.touchSeconds, sampleRate, hopSize);
  const int minSibilance =
      secondsToFrames(config.minSibilanceSeconds, sampleRate, hopSize);
  const int minBreath =
      secondsToFrames(config.minBreathSeconds, sampleRate, hopSize);
  const int longRegion =
      secondsToFrames(config.longRegionSeconds, sampleRate, hopSize);
  const int sibilanceMaxGap =
      secondsToFrames(config.sibilanceMaxGapSeconds, sampleRate, hopSize);
  const int breathMaxGap =
      secondsToFrames(config.breathMaxGapSeconds, sampleRate, hopSize);

  // 5) Decide each region from its context.
  forEachRun(
      numFrames, [&](int f) { return inRegion[static_cast<size_t>(f)] != 0; },
      [&](int start, int end) {
        const int length = end - start;
        const bool touchesLeft = anyVoiced(start - touch, start);
        const bool touchesRight = anyVoiced(end, end + touch);
        const bool isLong = length >= longRegion;

        // Bridged gap frames take the class before them; then a 5-frame
        // majority vote removes isolated flips.
        std::vector<std::uint8_t> shape(
            frameClass.begin() + start, frameClass.begin() + end);
        for (int i = 0; i < length; ++i)
        {
          auto &value = shape[static_cast<size_t>(i)];
          if (value != None)
            continue;
          if (i > 0)
            value = shape[static_cast<size_t>(i - 1)];
          else
          {
            const auto next = std::find_if(
                shape.begin(), shape.end(),
                [](std::uint8_t v) { return v != None; });
            value = next != shape.end() ? *next
                                        : static_cast<std::uint8_t>(Breath);
          }
        }
        if (length >= 5)
        {
          std::vector<std::uint8_t> smoothed(shape.size());
          for (int i = 0; i < length; ++i)
          {
            const int lo = std::max(0, i - 2);
            const int hi = std::min(length, i + 3);
            const auto sibilant = std::count(shape.begin() + lo,
                                             shape.begin() + hi, Sibilance);
            smoothed[static_cast<size_t>(i)] =
                sibilant * 2 > hi - lo ? Sibilance : Breath;
          }
          shape.swap(smoothed);
        }

        std::vector<std::uint8_t> out(static_cast<size_t>(length), None);
        forEachRun(
            length, [&](int i) { return shape[static_cast<size_t>(i)] == Sibilance; },
            [&](int a, int b) {
              // A sibilant inside a long region is breath noise, unless it is
              // the region's edge against singing.
              const bool edgeTouch =
                  (a == 0 && touchesLeft) || (b == length && touchesRight);
              std::fill(out.begin() + a, out.begin() + b,
                        !isLong || edgeTouch ? Sibilance : Breath);
            });
        forEachRun(
            length, [&](int i) { return shape[static_cast<size_t>(i)] == Breath; },
            [&](int a, int b) {
              // A short breath-like burst against singing is an aspirated
              // consonant (h, breathy onset), not a breath.
              const bool consonant = !isLong && (touchesLeft || touchesRight);
              std::fill(out.begin() + a, out.begin() + b,
                        consonant ? Sibilance : Breath);
            });

        forEachRun(
            length, [&](int i) { return out[static_cast<size_t>(i)] == Sibilance; },
            [&](int a, int b) {
              const bool tooShort = b - a < minSibilance;
              if (!tooShort &&
                  distanceToVoiced(start + a, start + b) <= sibilanceMaxGap)
                return;
              std::fill(out.begin() + a, out.begin() + b,
                        tooShort || !isLong ? None : Breath);
            });
        forEachRun(
            length, [&](int i) { return out[static_cast<size_t>(i)] == Breath; },
            [&](int a, int b) {
              if (b - a < minBreath ||
                  distanceToVoiced(start + a, start + b) > breathMaxGap)
                std::fill(out.begin() + a, out.begin() + b, None);
            });

        std::copy(out.begin(), out.end(), labels.begin() + start);
      });

  return labels;
}

} // namespace breathsibilance
