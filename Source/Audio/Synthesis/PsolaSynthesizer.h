#pragma once

#include "../../JuceHeader.h"
#include "DspSynthesisRequest.h"
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

/**
 * Time-domain PSOLA resynthesis.
 *
 * The other DSP engine is a phase vocoder: it rebuilds every phase from
 * accumulated frequency estimates, which is why asking it for a ratio of 1.0
 * returns a signal whose error is larger than the signal itself. Pitch and
 * magnitude spectrum survive that; waveform shape does not, and on sustained
 * vowels the result is the familiar phasiness.
 *
 * PSOLA never leaves the time domain. It cuts the original into one grain per
 * pitch period, then lays those grains back down at whatever spacing the new
 * pitch calls for. Nothing is re-estimated, so a ratio of 1.0 is a copy, and
 * formants come along inside the grain for free - no cepstral envelope, no
 * counter-shift, no FFT anywhere in the render.
 *
 * It suits this application specifically. The material is monophonic voice;
 * the pitch curve PSOLA needs is already computed and cleaned (AudioData's
 * denseF0); and IncrementalSynthesizer's blend mask already routes unvoiced
 * frames to the untouched original, so the engine only has to be good at the
 * voiced material it is best at.
 *
 * Takes the same request and returns the same span of samples as
 * PhaseVocoderSynthesizer, so the two are interchangeable to the caller.
 */
class PsolaSynthesizer
{
public:
  using Request = DspSynthesisRequest;

  PsolaSynthesizer();
  ~PsolaSynthesizer();

  /**
   * Render on a worker thread and deliver the result to @p callback.
   * The callback receives an empty vector on failure or cancellation.
   */
  void renderAsync(Request request,
                   std::function<void(std::vector<float>)> callback,
                   std::shared_ptr<std::atomic<bool>> cancelFlag = nullptr);

  /// Synchronous render. Returns empty on invalid input or cancellation.
  static std::vector<float> render(const Request &request,
                                   const std::shared_ptr<std::atomic<bool>> &cancelFlag = nullptr);

  /**
   * Pitch marks for @p request's source slice, one per pitch period inside
   * each voiced run, in slice-relative samples. Exposed for testing.
   */
  static std::vector<int> detectPitchMarks(const Request &request);

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PsolaSynthesizer)
};
