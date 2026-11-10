#pragma once

#include <cstdint>
#include <vector>

/**
 * Breath and sibilance detection for monophonic vocals.
 *
 * Labels every analysis frame - frame i is centred on sample i * hopSize, the
 * grid of the mel spectrogram and AudioData::voicedMask - as one of:
 *
 *   Sibilance  unvoiced consonant noise next to sung audio: s, sh, ch, f, th,
 *              t/k releases and aspirated h.
 *   Breath     aspirated noise at a phrase boundary: inhales and exhales.
 *   None       everything else - voiced, silence, background, unclassified.
 *
 * Plain DSP on top of the pitch detector's voicing decision, which is already
 * the most reliable voiced/unvoiced cue PitchNet has (SwiftF0 calls fewer than
 * 3% of real sibilant frames voiced). Per frame it measures the energy in a
 * low (80-900 Hz), mid (0.9-3.5 kHz) and high (3.5-16 kHz) band, the spectral
 * centroid and, where it matters, a window-corrected autocorrelation
 * periodicity. Unvoiced frames whose energy sits in the high band are
 * sibilant-like; unvoiced, aperiodic frames centred between 1 and 4.5 kHz are
 * breath-like. Thresholds are relative to the take's own background (a low
 * percentile per band) and vocal level, so input gain does not matter.
 *
 * Context then decides between the two: frames are grouped into noise
 * regions, a short region touching sung audio is a consonant, a region of
 * 150 ms or more is a breath except for a sibilant edge that touches singing
 * (the "s" before an inhale), sibilants must sit within ~175 ms of voicing and
 * breaths within ~1 s.
 *
 * docs/breath-sibilance-detection.md describes the evaluation; the hnsep
 * harmonic/noise model was tried as an extra aperiodicity cue and did not
 * beat the pitch detector's voicing, so it is not used.
 *
 * No JUCE dependency.
 */
namespace breathsibilance
{
enum Label : std::uint8_t
{
  None = 0,
  Sibilance = 1,
  Breath = 2
};

/** True for frames that carry breath or consonant noise. */
inline bool isNoise(std::uint8_t label)
{
  return label == Sibilance || label == Breath;
}

/** Label of `frame`, None outside the vector. */
inline std::uint8_t labelAt(const std::vector<std::uint8_t> &labels,
                            int frame)
{
  return frame >= 0 && frame < static_cast<int>(labels.size())
             ? labels[static_cast<size_t>(frame)]
             : static_cast<std::uint8_t>(None);
}

struct Config
{
  // Analysis windows, in samples at 44.1 kHz (scaled with the sample rate).
  int spectralWindow = 1024;    // ~23 ms: resolves a 40 ms consonant
  int periodicityWindow = 2048; // ~46 ms: two periods of a 45 Hz voice

  // Band edges in Hz.
  float lowBandStart = 80.0f;
  float midBandStart = 900.0f;
  float highBandStart = 3500.0f;
  float highBandEnd = 16000.0f;

  // A band's background level is this percentile of its audible frames, and
  // the vocal reference this percentile of the voiced frames' level.
  float floorPercentile = 10.0f;
  float referencePercentile = 95.0f;

  // Sibilant-like frame: at least this share of its energy in the high
  // band, the high band this far above its background (dB), and the frame
  // no further below the vocal reference (dB).
  float sibilanceHighShare = 0.35f;
  float sibilanceAboveFloorDb = 9.0f;
  float sibilanceBelowReferenceDb = 50.0f;

  // Breath-like frame: energy above midBandStart this far over its
  // background, within this distance of the vocal reference, a spectral
  // centroid inside [breathCentroidMin, breathCentroidMax], no more than
  // breathMaxLowShare of its energy below midBandStart, and aperiodic.
  float breathAboveFloorDb = 9.0f;
  float breathBelowReferenceDb = 55.0f;
  float breathCentroidMin = 1000.0f;
  float breathCentroidMax = 4500.0f;
  float breathMaxLowShare = 0.55f;
  float breathMaxPeriodicity = 0.80f;

  // Voicing sometimes leaks a frame into a sibilant. A frame the pitch
  // detector called voiced is still sibilant-like when its high band holds
  // this share of the energy and its periodicity is below this.
  float voicedOverrideHighShare = 0.65f;
  float voicedOverridePeriodicity = 0.40f;

  // Region shaping, in seconds (converted to frames with the hop size).
  double bridgeSeconds = 0.046;        // unvoiced gaps closed inside a region
  double minSibilanceSeconds = 0.035;
  double minBreathSeconds = 0.070;
  double longRegionSeconds = 0.150;    // a region this long is a breath
  double touchSeconds = 0.023;         // voicing this close touches a region
  double sibilanceMaxGapSeconds = 0.175; // sibilants sit next to singing
  double breathMaxGapSeconds = 1.0;      // breaths belong to a phrase
};

/**
 * Label numFrames analysis frames of a mono take.
 *
 * @param samples     mono audio
 * @param voiced      the pitch detector's per-frame voicing (voicedMask);
 *                    frames past its end count as unvoiced
 * @return            numFrames labels (Label values)
 */
std::vector<std::uint8_t> detect(const float *samples, int numSamples,
                                 double sampleRate, int hopSize,
                                 int numFrames,
                                 const std::vector<bool> &voiced,
                                 const Config &config = {});

} // namespace breathsibilance
