#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <vector>

namespace formant {

// Change the smooth spectral envelope, retaining each FFT bin's phase and
// frequency. Shared by PSOLA and the neural vocoder; never changes the F0 curve.
// Curves are in output time, so the effect follows retimed notes.
inline bool process(std::vector<float>& audio, const std::vector<float>& shifts,
                    const std::vector<float>& f0, int curveHop, int sampleRate,
                    const std::atomic<bool>* cancelled = nullptr)
{
    if (audio.empty() || shifts.empty() || curveHop <= 0 || sampleRate <= 0)
        return true;
    if (std::none_of(shifts.begin(), shifts.end(), [](float s) { return std::abs(s) > 0.001f; }))
        return true; // Exact bypass, including existing projects.

    constexpr int order = 12, size = 1 << order, step = size / 8;
    constexpr float pi = 3.14159265358979323846f;
    juce::dsp::FFT fft(order);
    using Complex = std::complex<float>;
    std::vector<Complex> input(size), spectrum(size), logSpectrum(size), cepstrum(size), envelope(size), time(size);
    std::vector<float> window(size), output(audio.size(), 0.0f), weight(audio.size(), 0.0f);
    for (int i = 0; i < size; ++i)
        window[i] = 0.5f - 0.5f * std::cos(2.0f * pi * i / size);
    const auto sampleCurve = [curveHop](const std::vector<float>& curve, int sample, float fallback) {
        if (curve.empty()) return fallback;
        const float frame = std::clamp(static_cast<float>(sample) / curveHop, 0.0f, static_cast<float>(curve.size() - 1));
        const int left = static_cast<int>(frame);
        const int right = std::min(left + 1, static_cast<int>(curve.size()) - 1);
        return curve[left] + (curve[right] - curve[left]) * (frame - left);
    };
    for (int centre = 0; centre < static_cast<int>(audio.size()) + size / 2; centre += step)
    {
        if (cancelled && cancelled->load()) return false;
        for (int i = 0; i < size; ++i) {
            const int source = centre + i - size / 2;
            input[i] = Complex(source >= 0 && source < static_cast<int>(audio.size()) ? audio[source] * window[i] : 0.0f, 0.0f);
        }
        fft.perform(input.data(), spectrum.data(), false);
        const float shift = std::clamp(sampleCurve(shifts, centre, 0.0f), -24.0f, 24.0f);
        if (std::abs(shift) > 0.001f) {
            for (int i = 0; i < size; ++i)
                logSpectrum[i] = Complex(std::log(std::max(1.0e-7f, std::abs(spectrum[i]))), 0.0f);
            fft.perform(logSpectrum.data(), cepstrum.data(), true);
            // Remove pitch-period structure from the envelope. A tapered
            // low-quefrency lifter reduces ringing around sharp resonances.
            const float pitch = std::max(70.0f, sampleCurve(f0, centre, 150.0f));
            const int cutoff = std::clamp(static_cast<int>(std::min(sampleRate * 0.0025f, sampleRate * 0.6f / pitch)), 8, size / 4);
            for (int i = 1; i < size; ++i) {
                const int q = std::min(i, size - i);
                const float taper = q < cutoff ? 0.5f + 0.5f * std::cos(pi * q / cutoff) : 0.0f;
                cepstrum[i] *= taper;
            }
            fft.perform(cepstrum.data(), envelope.data(), false);
            const float ratio = std::exp2(shift / 12.0f);
            for (int bin = 0; bin <= size / 2; ++bin) {
                const float source = std::min(bin / ratio, static_cast<float>(size / 2));
                const int left = static_cast<int>(source), right = std::min(left + 1, size / 2);
                const float target = envelope[left].real() + (envelope[right].real() - envelope[left].real()) * (source - left);
                // Limit boosts/cuts to 24 dB to avoid amplifying spectral nulls.
                const float gain = std::exp(std::clamp(target - envelope[bin].real(), -2.7631f, 2.7631f));
                spectrum[bin] *= gain;
                if (bin > 0 && bin < size / 2) spectrum[size - bin] *= gain;
            }
        }
        fft.perform(spectrum.data(), time.data(), true);
        for (int i = 0; i < size; ++i) {
            const int dest = centre + i - size / 2;
            if (dest < 0 || dest >= static_cast<int>(audio.size())) continue;
            output[dest] += time[i].real() * window[i];
            weight[dest] += window[i] * window[i];
        }
    }
    for (size_t i = 0; i < audio.size(); ++i)
        audio[i] = output[i] / std::max(1.0e-8f, weight[i]);
    return true;
}
} // namespace formant
