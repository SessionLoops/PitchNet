// Polyphonic engine - note analysis and per-note resynthesis (Source/Audio/Polyphonic).
// Self-contained: synthetic harmonic tones, no model files, DSP-only purification.

#include "../Source/Audio/Polyphonic/PolyEngine.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 44100;

// A plucked-string-like tone: 1/h partials with slight inharmonicity, faster decay for higher
// partials, 5 ms attack.
void addTone(std::vector<float>& x, double f0, double start, double dur, double gain, double B = 0.0002)
{
    const int s0 = static_cast<int>(start * kRate), n = static_cast<int>(dur * kRate);
    for (int h = 1; h <= 24; ++h)
    {
        const double fh = h * f0 * std::sqrt(1.0 + B * h * h);
        if (fh > 15000.0)
            break;
        const double phase = 0.37 * h * h;
        for (int i = 0; i < n && s0 + i < static_cast<int>(x.size()); ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            const double env = std::min(1.0, t / 0.005) * std::exp(-t * (0.8 + 0.15 * h)) * std::min(1.0, (dur - t) / 0.02);
            x[static_cast<size_t>(s0 + i)] += static_cast<float>(gain / h * env * std::sin(2 * kPi * fh * t + phase));
        }
    }
}

double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

// Magnitude of a sinusoid at `hz` in x[from, to) (DFT at one frequency).
double toneAmplitude(const std::vector<float>& x, double hz, int from, int to)
{
    double re = 0, im = 0, wsum = 0;
    for (int i = from; i < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * (i - from) / (to - from));
        re += w * x[static_cast<size_t>(i)] * std::cos(2 * kPi * hz * i / kRate);
        im += w * x[static_cast<size_t>(i)] * std::sin(2 * kPi * hz * i / kRate);
        wsum += w;
    }
    return 2.0 * std::hypot(re, im) / wsum;
}

double db(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }

struct SineFit
{
    double amplitude, residualSnr;
};

// Fit both quadratures at the requested frequency: this measures amplitude and spurious
// content without assuming the renderer's constant phase or an integer number of cycles.
SineFit fitSine(const std::vector<float>& x, double hz, double sampleRate, int from, int to)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, xs = 0.0, xc = 0.0;
    for (int i = from; i < to; ++i)
    {
        const double s = std::sin(2 * kPi * hz * i / sampleRate);
        const double c = std::cos(2 * kPi * hz * i / sampleRate);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += x[static_cast<size_t>(i)] * s;
        xc += x[static_cast<size_t>(i)] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det;
    const double b = (xc * ss - xs * sc) / det;
    double signal = 0.0, error = 0.0;
    for (int i = from; i < to; ++i)
    {
        const double fitted = a * std::sin(2 * kPi * hz * i / sampleRate)
                            + b * std::cos(2 * kPi * hz * i / sampleRate);
        signal += fitted * fitted;
        error += std::pow(x[static_cast<size_t>(i)] - fitted, 2);
    }
    return {std::hypot(a, b), 10.0 * std::log10(signal / std::max(error, 1e-30))};
}

int findNote(const polyphonic::Analysis& A, double midi)
{
    for (size_t i = 0; i < A.notes.size(); ++i)
        if (A.notes[i].active && std::fabs(A.notes[i].medianMidi() - midi) < 0.3)
            return static_cast<int>(i);
    return -1;
}

int countActive(const polyphonic::Analysis& A)
{
    int n = 0;
    for (const auto& note : A.notes)
        n += note.active ? 1 : 0;
    return n;
}

// A sustained tone with vibrato (a bowed or blown note): 1/h partials, constant level.
void addVibratoTone(std::vector<float>& x, double f0, double start, double dur, double gain, double depth = 0.01, double rate = 5.5)
{
    const int s0 = static_cast<int>(start * kRate), n = static_cast<int>(dur * kRate);
    for (int h = 1; h <= 12; ++h)
    {
        double phase = 0.37 * h * h;
        for (int i = 0; i < n && s0 + i < static_cast<int>(x.size()); ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            const double env = std::min(1.0, t / 0.01) * std::min(1.0, (dur - t) / 0.03);
            x[static_cast<size_t>(s0 + i)] += static_cast<float>(gain / h * env * std::sin(phase));
            phase += 2 * kPi * h * f0 * (1.0 + depth * std::sin(2 * kPi * rate * t)) / kRate;
        }
    }
}

// Spectral SNR (dB) of y against a reference: STFT magnitudes, Hann 2048, hop 512.
double spectralSnr(const std::vector<float>& ref, const std::vector<float>& y)
{
    constexpr int N = 2048, H = 512;
    std::vector<std::complex<double>> a(N), b(N);
    const auto fft = [](std::vector<std::complex<double>>& v) {
        const int n = static_cast<int>(v.size());
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(v[static_cast<size_t>(i)], v[static_cast<size_t>(j)]);
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const std::complex<double> w = std::polar(1.0, -2 * kPi / len);
            for (int i = 0; i < n; i += len)
            {
                std::complex<double> wk = 1.0;
                for (int k = 0; k < len / 2; ++k, wk *= w)
                {
                    const auto u = v[static_cast<size_t>(i + k)], t = wk * v[static_cast<size_t>(i + k + len / 2)];
                    v[static_cast<size_t>(i + k)] = u + t;
                    v[static_cast<size_t>(i + k + len / 2)] = u - t;
                }
            }
        }
    };
    double sig = 0.0, err = 0.0;
    for (size_t s = 0; s + N <= std::min(ref.size(), y.size()); s += H)
    {
        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / N);
            a[static_cast<size_t>(i)] = w * ref[s + static_cast<size_t>(i)];
            b[static_cast<size_t>(i)] = w * y[s + static_cast<size_t>(i)];
        }
        fft(a);
        fft(b);
        for (int k = 0; k <= N / 2; ++k)
        {
            const double ma = std::abs(a[static_cast<size_t>(k)]), mb = std::abs(b[static_cast<size_t>(k)]);
            sig += ma * ma;
            err += (ma - mb) * (ma - mb);
        }
    }
    return 10.0 * std::log10(sig / std::max(err, 1e-30));
}

double rmsDb(const std::vector<float>& y, double t0, double t1)
{
    double s = 0.0;
    for (int i = static_cast<int>(t0 * kRate); i < static_cast<int>(t1 * kRate); ++i)
        s += static_cast<double>(y[static_cast<size_t>(i)]) * y[static_cast<size_t>(i)];
    return 10.0 * std::log10(s / ((t1 - t0) * kRate) + 1e-30);
}
} // namespace

int main()
{
    const auto params = polyphonic::Params::forSampleRate(kRate);

    // ---- 1. a single tone: one note, right pitch; unedited render is the input ----
    {
        std::vector<float> x(static_cast<size_t>(1.6 * kRate), 0.0f);
        addTone(x, midiToHz(57), 0.1, 1.4, 0.3);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        std::printf("single tone: %d active notes\n", countActive(A));
        const int ni = findNote(A, 57);
        CHECK(ni >= 0);
        CHECK(countActive(A) == 1);
        CHECK(std::fabs(A.frameToSeconds(A.notes[static_cast<size_t>(ni)].startFrame) - 0.1) < 0.05);

        const auto same = polyphonic::render(A, {x}, {});
        double err = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            err = std::max(err, static_cast<double>(std::fabs(same[0][i] - x[i])));
        std::printf("identity render max error %.3g\n", err);
        CHECK(err < 1e-4);

        // pitch shift +3 semitones: the energy moves to the new fundamental
        polyphonic::NoteEdit up;
        up.semitones = 3.0f;
        const auto shifted = polyphonic::render(A, {x}, {{ni, up}})[0];
        const int a = static_cast<int>(0.3 * kRate), b = static_cast<int>(1.2 * kRate);
        const double oldF = midiToHz(57), newF = midiToHz(60);
        const double before = db(toneAmplitude(x, oldF, a, b)), afterOld = db(toneAmplitude(shifted, oldF, a, b));
        const double afterNew = db(toneAmplitude(shifted, newF, a, b));
        std::printf("shift +3: old f0 %.1f dB -> %.1f dB, new f0 %.1f dB\n", before, afterOld, afterNew);
        CHECK(afterNew > before - 2.0);
        CHECK(afterOld < before - 25.0);
        // second harmonic follows the note (no time stretching, partials stay harmonic)
        CHECK(db(toneAmplitude(shifted, 2 * newF, a, b)) > db(toneAmplitude(x, 2 * oldF, a, b)) - 3.0);

        // gain edit: -6 dB on the fundamental
        polyphonic::NoteEdit quieter;
        quieter.gainDb = -6.0f;
        const auto q = polyphonic::render(A, {x}, {{ni, quieter}})[0];
        const double d = db(toneAmplitude(q, oldF, a, b)) - before;
        std::printf("gain -6 dB: measured %.2f dB\n", d);
        CHECK(std::fabs(d + 6.0) < 1.0);
    }

    // ---- 2. a chord: C4 + F#4 + B4 (no shared partials below the 6th); edit one note ----
    {
        std::vector<float> x(static_cast<size_t>(2.0 * kRate), 0.0f);
        addTone(x, midiToHz(60), 0.1, 1.8, 0.25);
        addTone(x, midiToHz(66), 0.1, 1.8, 0.2);
        addTone(x, midiToHz(71), 0.1, 1.8, 0.2);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        std::printf("chord: %d active notes\n", countActive(A));
        const int c4 = findNote(A, 60), fs4 = findNote(A, 66), b4 = findNote(A, 71);
        CHECK(c4 >= 0 && fs4 >= 0 && b4 >= 0);

        const int a = static_cast<int>(0.4 * kRate), b = static_cast<int>(1.4 * kRate);
        polyphonic::NoteEdit mute;
        mute.mute = true;
        const auto m = polyphonic::render(A, {x}, {{fs4, mute}})[0];
        const double fsBefore = db(toneAmplitude(x, midiToHz(66), a, b)), fsAfter = db(toneAmplitude(m, midiToHz(66), a, b));
        const double cBefore = db(toneAmplitude(x, midiToHz(60), a, b)), cAfter = db(toneAmplitude(m, midiToHz(60), a, b));
        std::printf("mute F#4: F#4 %.1f -> %.1f dB, C4 %.1f -> %.1f dB\n", fsBefore, fsAfter, cBefore, cAfter);
        CHECK(fsAfter < fsBefore - 20.0);
        CHECK(std::fabs(cAfter - cBefore) < 1.0);

        // "major to minor": move the middle note down a semitone, others untouched
        polyphonic::NoteEdit down;
        down.semitones = -1.0f;
        const auto e = polyphonic::render(A, {x}, {{fs4, down}})[0];
        const double fNew = db(toneAmplitude(e, midiToHz(65), a, b)), fOld = db(toneAmplitude(e, midiToHz(66), a, b));
        const double bAfter = db(toneAmplitude(e, midiToHz(71), a, b)), bBefore = db(toneAmplitude(x, midiToHz(71), a, b));
        std::printf("F#4 -> F4: F4 %.1f dB, F#4 %.1f dB, B4 %.1f -> %.1f dB\n", fNew, fOld, bBefore, bAfter);
        CHECK(fNew > fsBefore - 3.0);
        CHECK(fOld < fsBefore - 20.0);
        CHECK(std::fabs(bAfter - bBefore) < 1.0);

        // stereo: the same analysis drives both channels; an unedited channel pair is unchanged
        std::vector<float> right(x.size());
        for (size_t i = 0; i < x.size(); ++i)
            right[i] = 0.5f * x[i];
        const auto st = polyphonic::render(A, {x, right}, {{fs4, down}});
        CHECK(st.size() == 2);
        CHECK(std::fabs(db(toneAmplitude(st[1], midiToHz(65), a, b)) - (fNew - 6.02)) < 0.5);

        // solo extraction of C4 contains C4 and hardly any B4
        const auto solo = polyphonic::extractNote(A, x, c4);
        const double soloC = db(toneAmplitude(solo, midiToHz(60), a, b)), soloB = db(toneAmplitude(solo, midiToHz(71), a, b));
        std::printf("solo C4: C4 %.1f dB, B4 leakage %.1f dB\n", soloC, soloB);
        CHECK(soloC > cBefore - 1.5);
        CHECK(soloB < bBefore - 25.0);

        // a solo set: C4 and B4 together, without F#4
        const auto pair = polyphonic::extractNotes(A, {x}, {c4, b4});
        CHECK(pair.size() == 1 && pair[0].size() == x.size());
        const double pairC = db(toneAmplitude(pair[0], midiToHz(60), a, b));
        const double pairB = db(toneAmplitude(pair[0], midiToHz(71), a, b));
        const double pairFs = db(toneAmplitude(pair[0], midiToHz(66), a, b));
        std::printf("solo C4+B4: C4 %.1f dB, B4 %.1f dB, F#4 leakage %.1f dB\n", pairC, pairB, pairFs);
        CHECK(std::fabs(pairC - soloC) < 0.5);
        CHECK(pairB > bBefore - 1.5);
        CHECK(pairFs < fsBefore - 25.0);
        const auto none = polyphonic::extractNotes(A, {x, right}, {});
        CHECK(none.size() == 2 && none[1].size() == x.size());
        for (float v : none[0])
            CHECK(v == 0.0f);
    }

    // ---- 3. moving a note in time ----
    {
        std::vector<float> x(static_cast<size_t>(2.0 * kRate), 0.0f);
        addTone(x, midiToHz(64), 0.1, 0.8, 0.3);
        addTone(x, midiToHz(55), 0.1, 1.8, 0.2);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const int e4 = findNote(A, 64);
        CHECK(e4 >= 0);
        polyphonic::NoteEdit later;
        later.timeOffsetSeconds = 0.5;
        const auto y = polyphonic::render(A, {x}, {{e4, later}})[0];
        const double early = db(toneAmplitude(y, midiToHz(64), static_cast<int>(0.15 * kRate), static_cast<int>(0.5 * kRate)));
        const double late = db(toneAmplitude(y, midiToHz(64), static_cast<int>(0.65 * kRate), static_cast<int>(1.0 * kRate)));
        const double orig = db(toneAmplitude(x, midiToHz(64), static_cast<int>(0.15 * kRate), static_cast<int>(0.5 * kRate)));
        std::printf("move +0.5 s: E4 early %.1f dB (was %.1f), late %.1f dB\n", early, orig, late);
        CHECK(early < orig - 20.0);
        CHECK(late > orig - 3.0);
    }

    // ---- 4. degenerate input ----
    {
        std::vector<float> silence(4096, 0.0f);
        const auto A = polyphonic::analyze(silence.data(), static_cast<int>(silence.size()), params);
        CHECK(countActive(A) == 0);
        const auto y = polyphonic::render(A, {silence}, {});
        CHECK(y.size() == 1 && y[0].size() == silence.size());
        for (float v : y[0])
            CHECK(v == 0.0f);
        const auto empty = polyphonic::analyze(nullptr, 0, params);
        CHECK(empty.notes.empty());
    }

    // ---- 5. frames no edit reaches come back exactly ----
    {
        std::vector<float> x(static_cast<size_t>(2.0 * kRate), 0.0f);
        addTone(x, midiToHz(62), 0.1, 0.4, 0.3);
        addTone(x, midiToHz(69), 1.2, 0.6, 0.3);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const int d4 = findNote(A, 62);
        CHECK(d4 >= 0);
        polyphonic::NoteEdit up;
        up.semitones = 2.0f;
        const auto y = polyphonic::render(A, {x}, {{d4, up}})[0];
        double far = 0.0;
        for (size_t i = static_cast<size_t>(1.0 * kRate); i < x.size(); ++i)
            far = std::max(far, static_cast<double>(std::fabs(y[i] - x[i])));
        const int a = static_cast<int>(0.15 * kRate), b = static_cast<int>(0.45 * kRate);
        const double was = db(toneAmplitude(x, midiToHz(62), a, b));
        const double now = db(toneAmplitude(y, midiToHz(64), a, b));
        std::printf("edit elsewhere: untouched frames max error %.2e; D4 -> E4 %.1f dB (was %.1f)\n", far, now, was);
        CHECK(far < 1e-6);
        CHECK(now > was - 3.0);
    }

    // ---- 6. a repeated note at one pitch is split where its own partials rise again ----
    {
        std::vector<float> x(static_cast<size_t>(2.3 * kRate), 0.0f);
        addTone(x, midiToHz(57), 0.1, 2.0, 0.3);
        addTone(x, midiToHz(57), 1.1, 1.0, 0.3);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        std::vector<double> starts;
        for (const auto& n : A.notes)
            if (n.active && std::fabs(n.medianMidi() - 57) < 0.3)
                starts.push_back(A.frameToSeconds(n.startFrame));
        std::printf("repeated A3: %d notes at A3, starts", static_cast<int>(starts.size()));
        for (double s : starts)
            std::printf(" %.3f", s);
        std::printf(" s\n");
        CHECK(starts.size() == 2);
        CHECK(starts.size() == 2 && std::fabs(starts[0] - 0.1) < 0.05 && std::fabs(starts[1] - 1.1) < 0.05);
    }

    // ---- 7. unpitched sound is not a note and stays put when a note is muted ----
    {
        std::vector<float> x(static_cast<size_t>(1.6 * kRate), 0.0f);
        addTone(x, midiToHz(60), 0.1, 1.4, 0.3);
        uint32_t seed = 12345u;                       // a 3 ms noise burst on the attack, like a hammer
        for (int i = 0; i < static_cast<int>(0.003 * kRate); ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            x[static_cast<size_t>(0.1 * kRate) + static_cast<size_t>(i)] += 0.3f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
        }
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const int c4 = findNote(A, 60);
        CHECK(c4 >= 0);
        CHECK(countActive(A) == 1);
        polyphonic::NoteEdit mute;
        mute.mute = true;
        const auto y = polyphonic::render(A, {x}, {{c4, mute}})[0];
        // energy of the burst above the note's partials (12-18 kHz), before and after the mute
        const int a = static_cast<int>(0.09 * kRate), b = static_cast<int>(0.13 * kRate);
        std::vector<float> diff(x.size());
        for (size_t i = 0; i < x.size(); ++i)
            diff[i] = y[i] - x[i];
        double burst = 0.0, changed = 0.0;
        for (double hz = 12000.0; hz <= 18000.0; hz += 100.0)
        {
            burst += std::pow(toneAmplitude(x, hz, a, b), 2.0);
            changed += std::pow(toneAmplitude(diff, hz, a, b), 2.0);
        }
        const int c = static_cast<int>(0.4 * kRate), d = static_cast<int>(1.2 * kRate);
        const double cBefore = db(toneAmplitude(x, midiToHz(60), c, d)), cAfter = db(toneAmplitude(y, midiToHz(60), c, d));
        std::printf("mute C4 with a noise burst: C4 %.1f -> %.1f dB, burst change %.1f dB below the burst\n",
                    cBefore, cAfter, 10.0 * std::log10(burst / std::max(changed, 1e-30)));
        CHECK(cAfter < cBefore - 20.0);
        CHECK(changed < 1e-6 * burst);
    }

    // ---- 8. stretching: a note lengthened or shortened; its attack and the other note unchanged ----
    {
        std::vector<float> x(static_cast<size_t>(3.0 * kRate), 0.0f);    // room for the longer note
        addTone(x, midiToHz(57), 0.1, 1.0, 0.3);
        addTone(x, midiToHz(65), 0.1, 2.6, 0.2);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const int a3 = findNote(A, 57), f4 = findNote(A, 65);
        CHECK(a3 >= 0 && f4 >= 0);
        const auto at = [&](const std::vector<float>& y, double hz, double t0, double t1) {
            return db(toneAmplitude(y, hz, static_cast<int>(t0 * kRate), static_cast<int>(t1 * kRate)));
        };
        const auto rms = [&](const std::vector<float>& y, double t0, double t1) {
            double s = 0.0;
            for (int i = static_cast<int>(t0 * kRate); i < static_cast<int>(t1 * kRate); ++i)
                s += static_cast<double>(y[static_cast<size_t>(i)]) * y[static_cast<size_t>(i)];
            return 10.0 * std::log10(s / ((t1 - t0) * kRate) + 1e-30);
        };
        const double fA = midiToHz(57), fF = midiToHz(65);
        polyphonic::NoteEdit longer;
        longer.stretch = 2.0f;
        const auto y = polyphonic::render(A, {x}, {{a3, longer}})[0];
        const auto map = polyphonic::stretchMap(A, a3, longer);
        // output 1.3..1.7 s plays source time tau(1.3..1.7 s)
        const double hopS = static_cast<double>(params.hop) / kRate;
        const double s0 = map.sourceFrame(1.3 / hopS) * hopS, s1 = map.sourceFrame(1.7 / hopS) * hopS;
        std::printf("stretch x2: attack %.2f -> %.2f dB; A3 at 1.3-1.7 s %.1f dB (source %.2f-%.2f s: %.1f dB, before %.1f dB); "
                    "A3 a semitone up %.1f dB; F4 %.2f -> %.2f dB\n",
                    rms(x, 0.09, 0.16), rms(y, 0.09, 0.16), at(y, fA, 1.3, 1.7), s0, s1, at(x, fA, s0, s1), at(x, fA, 1.3, 1.7),
                    at(y, fA * std::pow(2.0, 1.0 / 12.0), 1.3, 1.7), at(x, fF, 0.3, 1.0), at(y, fF, 0.3, 1.0));
        CHECK(std::fabs(rms(y, 0.09, 0.16) - rms(x, 0.09, 0.16)) < 0.5);
        CHECK(std::fabs(at(y, fA, 1.3, 1.7) - at(x, fA, s0, s1)) < 2.0);
        CHECK(at(y, fA, 1.3, 1.7) > at(x, fA, 1.3, 1.7) + 30.0);
        CHECK(at(y, fA * std::pow(2.0, 1.0 / 12.0), 1.3, 1.7) < at(y, fA, 1.3, 1.7) - 25.0);
        CHECK(std::fabs(at(y, fF, 0.3, 1.0) - at(x, fF, 0.3, 1.0)) < 0.5);

        polyphonic::NoteEdit shorter;
        shorter.stretch = 0.5f;
        const auto z = polyphonic::render(A, {x}, {{a3, shorter}})[0];
        std::printf("stretch x0.5: A3 at 0.3-0.5 s %.1f dB (was %.1f), at 0.8-1.0 s %.1f dB (was %.1f); F4 %.2f -> %.2f dB\n",
                    at(z, fA, 0.3, 0.5), at(x, fA, 0.3, 0.5), at(z, fA, 0.8, 1.0), at(x, fA, 0.8, 1.0), at(x, fF, 0.3, 1.0), at(z, fF, 0.3, 1.0));
        CHECK(at(z, fA, 0.3, 0.5) > at(x, fA, 0.3, 0.5) - 6.0);
        CHECK(at(z, fA, 0.8, 1.0) < at(x, fA, 0.8, 1.0) - 20.0);
        CHECK(std::fabs(at(z, fF, 0.3, 1.0) - at(x, fF, 0.3, 1.0)) < 0.5);
    }

    // ---- 9. vibrato: partials keep their phase when they move between bins ----
    {
        // a pitch shift, against the same tone synthesised a semitone lower
        std::vector<float> x(static_cast<size_t>(2.0 * kRate), 0.0f), ref(x.size(), 0.0f);
        addVibratoTone(x, midiToHz(72), 0.1, 1.8, 0.3);
        addVibratoTone(ref, midiToHz(71), 0.1, 1.8, 0.3);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const int c5 = findNote(A, 72);
        CHECK(c5 >= 0);
        polyphonic::NoteEdit down;
        down.semitones = -1.0f;
        const auto y = polyphonic::render(A, {x}, {{c5, down}})[0];
        const double shifted = spectralSnr(ref, y), untouched = spectralSnr(ref, x);
        std::printf("vibrato C5 -> B4: %.1f dB against the tone played a semitone lower (unedited %.1f dB)\n", shifted, untouched);
        CHECK(shifted > 18.0);

        // stretching keeps the level of a note whose partials wander between bins
        std::vector<float> v(static_cast<size_t>(2.5 * kRate), 0.0f);
        addVibratoTone(v, midiToHz(72), 0.1, 1.0, 0.3);
        const auto B = polyphonic::analyze(v.data(), static_cast<int>(v.size()), params);
        const int n = findNote(B, 72);
        CHECK(n >= 0);
        for (float ratio : {0.6f, 1.5f})
        {
            polyphonic::NoteEdit e;
            e.stretch = ratio;
            const auto z = polyphonic::render(B, {v}, {{n, e}})[0];
            const auto map = polyphonic::stretchMap(B, n, e);
            const double hopS = static_cast<double>(params.hop) / kRate;
            const double t0 = map.outputFrame(0.4 / hopS) * hopS, t1 = map.outputFrame(0.9 / hopS) * hopS;
            std::printf("vibrato C5 stretched x%.1f: body %.2f dB (source %.2f dB)\n", ratio, rmsDb(z, t0, t1), rmsDb(v, 0.4, 0.9));
            CHECK(std::fabs(rmsDb(z, t0, t1) - rmsDb(v, 0.4, 0.9)) < 1.0);
        }
    }

    // ---- 10. a pitch curve returning to zero keeps its accumulated phase ----
    {
        // An exact bin-centred sinusoid occupies three Hann bins. Supplying that known mask
        // isolates the renderer from pitch detection and lets us check phase, not just pitch
        // or spectral magnitude. Raising the frequency by half a cycle over 40 hops must
        // leave the original-frequency waveform inverted when the pitch offset returns to 0.
        constexpr int bin = 40, raisedFrames = 40;
        const double f = static_cast<double>(bin) * kRate / params.fftSize;
        std::vector<float> x(static_cast<size_t>(2 * kRate));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = static_cast<float>(0.3 * std::sin(2 * kPi * f * i / kRate));
        polyphonic::Analysis A;
        A.params = params;
        A.numSamples = static_cast<int>(x.size());
        A.numFrames = 1 + (A.numSamples + params.hop - 1) / params.hop;
        A.numBins = params.fftSize / 2 + 1;
        polyphonic::Note n;
        n.endFrame = A.numFrames - 1;
        n.f0.assign(static_cast<size_t>(A.numFrames), static_cast<float>(f));
        A.notes.push_back(n);
        A.masks.resize(1);
        for (int t = 0; t < A.numFrames; ++t)
        {
            polyphonic::PartialMask pm;
            pm.frame = t;
            pm.harmonic = 1;
            pm.lo = bin - 1;
            pm.count = 3;
            pm.weightOffset = static_cast<int>(A.masks[0].weights.size());
            pm.freqHz = static_cast<float>(f);
            pm.amplitude = 0.3f;
            A.masks[0].partials.push_back(pm);
            A.masks[0].weights.insert(A.masks[0].weights.end(), 3, 1.0f);
        }
        polyphonic::NoteEdit curve;
        curve.pitchCurve.assign(static_cast<size_t>(A.numFrames), 0.0f);
        const float offset = static_cast<float>(12.0 * std::log2(1.0 + kRate / (2.0 * raisedFrames * params.hop * f)));
        std::fill(curve.pitchCurve.begin(), curve.pitchCurve.begin() + raisedFrames, offset);
        // A second rise after the zero plateau also checks that the new offset is integrated
        // for its own duration, rather than for all the frames since the first rise ended.
        std::fill(curve.pitchCurve.begin() + 100, curve.pitchCurve.begin() + 120, offset);
        const auto y = polyphonic::render(A, {x}, {{0, curve}})[0];
        for (const auto& interval : {std::pair<int, int>{50, 90}, {140, 165}})
        {
            const double phase = interval.first == 50 ? kPi : 1.5 * kPi;
            double sig = 0.0, err = 0.0;
            for (int i = interval.first * params.hop; i < interval.second * params.hop; ++i)
            {
                const double ref = 0.3 * std::sin(2 * kPi * f * i / kRate + phase);
                sig += ref * ref;
                err += std::pow(y[static_cast<size_t>(i)] - ref, 2);
            }
            const double snr = 10.0 * std::log10(sig / std::max(err, 1e-30));
            std::printf("pitch curve zero plateau %d-%d: %.1f dB waveform SNR against continuous phase\n",
                        interval.first, interval.second, snr);
            CHECK(snr > 75.0);
        }
    }

    // ---- 11. fractional-bin shifts preserve amplitude without adding distortion ----
    for (const int sampleRate : {44100, 48000, 96000})
    {
        const auto p = polyphonic::Params::forSampleRate(sampleRate);
        constexpr int bin = 40;
        const double binHz = static_cast<double>(sampleRate) / p.fftSize;
        // The exact-bin sine has only three Hann bins, so its analytic output is independent
        // of analysis, mask estimation, and the pitch-shift implementation. A low off-bin sine
        // also catches interpolation spurs where the positive and negative spectra are close.
        for (const bool offBin : {false, true})
        {
            const double f = (offBin ? 2.8 : bin) * binHz;
            std::vector<float> left(static_cast<size_t>(sampleRate)), right(left.size());
            for (size_t i = 0; i < left.size(); ++i)
            {
                left[i] = static_cast<float>(0.3 * std::sin(2 * kPi * f * i / sampleRate + 0.37));
                right[i] = -0.375f * left[i];
            }
            polyphonic::Analysis A;
            A.params = p;
            A.numSamples = static_cast<int>(left.size());
            A.numFrames = 1 + (A.numSamples + p.hop - 1) / p.hop;
            A.numBins = p.fftSize / 2 + 1;
            polyphonic::Note n;
            n.endFrame = A.numFrames - 1;
            n.f0.assign(static_cast<size_t>(A.numFrames), static_cast<float>(f));
            A.notes.push_back(n);
            A.masks.resize(1);
            const int center = static_cast<int>(std::lround(f / binHz));
            const int lo = std::max(1, center - (offBin ? 5 : 1));
            const int hi = center + (offBin ? 5 : 1);
            for (int t = 0; t < A.numFrames; ++t)
            {
                polyphonic::PartialMask pm;
                pm.frame = t;
                pm.harmonic = 1;
                pm.lo = lo;
                pm.count = hi - lo + 1;
                pm.weightOffset = static_cast<int>(A.masks[0].weights.size());
                pm.freqHz = static_cast<float>(f);
                pm.amplitude = 0.3f;
                A.masks[0].partials.push_back(pm);
                A.masks[0].weights.insert(A.masks[0].weights.end(), static_cast<size_t>(pm.count), 1.0f);
            }
            const std::vector<double> shifts = offBin ? std::vector<double>{-1.0, 1.0}
                : std::vector<double>{-2.0, -0.51, -0.5, -0.49, 0.125, 0.49, 0.5, 0.51, 1.0, 2.5};
            for (const double shift : shifts)
            {
                polyphonic::NoteEdit edit;
                edit.semitones = static_cast<float>(offBin ? shift : 12.0 * std::log2(1.0 + shift / bin));
                const double target = f * std::pow(2.0, static_cast<double>(edit.semitones) / 12.0);
                const auto output = polyphonic::render(A, {left, right}, {{0, edit}});
                CHECK(output.size() == 2);
                const int from = sampleRate / 5, to = 4 * sampleRate / 5;
                for (int channel = 0; channel < 2; ++channel)
                {
                    const auto fit = fitSine(output[static_cast<size_t>(channel)], target, sampleRate, from, to);
                    const double amplitudeDb = db(fit.amplitude / (channel == 0 ? 0.3 : 0.3 * 0.375));
                    std::printf("shift sine %d Hz rate, %.2f -> %.2f Hz, channel %d: %+.5f dB gain, %.1f dB residual SNR\n",
                                sampleRate, f, target, channel, amplitudeDb, fit.residualSnr);
                    CHECK(std::fabs(amplitudeDb) < 0.01);
                    CHECK(fit.residualSnr > 75.0);
                }
                double stereoError = 0.0, stereoSignal = 0.0;
                for (int i = from; i < to; ++i)
                {
                    stereoError += std::pow(output[1][static_cast<size_t>(i)] + 0.375 * output[0][static_cast<size_t>(i)], 2);
                    stereoSignal += std::pow(output[0][static_cast<size_t>(i)], 2);
                }
                CHECK(stereoError / stereoSignal < 1e-12);
            }
        }
    }

    // ---- 12. a note on its own with its edit: what the full render adds for that note ----
    {
        std::vector<float> x(static_cast<size_t>(2.4 * kRate), 0.0f);
        addTone(x, midiToHz(60), 0.1, 1.8, 0.25);
        addTone(x, midiToHz(66), 0.1, 1.8, 0.2);
        addTone(x, midiToHz(71), 0.1, 1.8, 0.2);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const int c4 = findNote(A, 60), fs4 = findNote(A, 66), b4 = findNote(A, 71);
        CHECK(c4 >= 0 && fs4 >= 0 && b4 >= 0);
        const int a = static_cast<int>(0.4 * kRate), b = static_cast<int>(1.4 * kRate);
        polyphonic::NoteEdit muted;
        muted.mute = true;
        // the span as a whole-length signal, for comparison with full renders
        auto placed = [&](const polyphonic::AudioSpan& s) {
            std::vector<float> y(x.size(), 0.0f);
            CHECK(s.channels.size() == 1 && s.start >= 0 && s.start + s.channels[0].size() <= x.size());
            std::copy(s.channels[0].begin(), s.channels[0].end(), y.begin() + s.start);
            return y;
        };
        // render(edit) - render(mute) is exactly the edited note's contribution, and nothing is
        // left out of the span
        polyphonic::NoteEdit down, warped;
        down.semitones = -1.0f;
        warped.semitones = -1.0f;
        warped.stretch = 1.5f;
        warped.timeOffsetSeconds = 0.1;
        for (const auto& ed : { down, warped })
        {
            const auto full = polyphonic::render(A, {x}, {{fs4, ed}})[0];
            const auto without = polyphonic::render(A, {x}, {{fs4, muted}})[0];
            const auto span = polyphonic::renderNotes(A, {x}, {fs4}, {{fs4, ed}, {c4, down}});
            const auto alone = placed(span);
            double err = 0.0;
            for (size_t i = 0; i < x.size(); ++i)
                err = std::max(err, std::fabs(static_cast<double>(full[i]) - without[i] - alone[i]));
            std::printf("F#4 on its own (stretch %.1f): %.2f-%.2f s, differs from the full render's share by %.1e\n", ed.stretch,
                        span.start / static_cast<double>(kRate), (span.start + span.channels[0].size()) / static_cast<double>(kRate), err);
            CHECK(err < 1e-4);
            CHECK(span.channels[0].size() < x.size());
        }
        // the edited note alone: F4, without F#4 or the other notes
        const auto alone = placed(polyphonic::renderNotes(A, {x}, {fs4}, {{fs4, down}}));
        const double f4 = db(toneAmplitude(alone, midiToHz(65), a, b)), fs = db(toneAmplitude(alone, midiToHz(66), a, b));
        const double c = db(toneAmplitude(alone, midiToHz(60), a, b)), bb = db(toneAmplitude(alone, midiToHz(71), a, b));
        std::printf("F#4 -> F4 on its own: F4 %.1f dB, F#4 %.1f dB, C4 %.1f dB, B4 %.1f dB\n", f4, fs, c, bb);
        CHECK(f4 > db(toneAmplitude(x, midiToHz(66), a, b)) - 1.5);
        CHECK(fs < f4 - 30.0 && c < f4 - 25.0 && bb < f4 - 25.0);
        // unedited notes come out as their separated signal; other notes' edits are ignored
        const auto plain = placed(polyphonic::renderNotes(A, {x}, {c4}, {{fs4, down}}));
        const auto separated = polyphonic::extractNotes(A, {x}, {c4})[0];
        for (size_t i = 0; i < x.size(); ++i)
            CHECK(std::fabs(plain[i] - separated[i]) < 1e-6f);
        // a muted note on its own is an empty span
        const auto silent = polyphonic::renderNotes(A, {x}, {fs4}, {{fs4, muted}});
        CHECK(silent.channels.size() == 1 && silent.channels[0].empty());
    }

    // ---- 13. a note taking its background (experimental): a muted note removes its part of what no
    //      note owns, a moved one takes it along, and on its own it is what the full render adds ----
    {
        // a tone whose onset comes with a noise burst (its attack), over a quiet noise floor
        std::vector<float> x(static_cast<size_t>(2.2 * kRate), 0.0f);
        uint32_t seed = 4242u;
        auto noise = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
        };
        for (auto& v : x)
            v = 0.003f * noise();
        const double on = 0.4;
        const int burst = static_cast<int>(0.03 * kRate);
        for (int i = 0; i < burst; ++i)
            x[static_cast<size_t>(on * kRate) + static_cast<size_t>(i)] += 0.2f * noise() * (1.0f - static_cast<float>(i) / burst);
        addTone(x, midiToHz(64), on, 1.4, 0.25);
        const auto A = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const int e4 = findNote(A, 64);
        CHECK(e4 >= 0);
        polyphonic::NoteEdit flagOnly, ownMute, plainMove, ownMove;
        flagOnly.takeBackground = true;               // with no other change the flag changes nothing
        ownMute.mute = true;
        ownMute.takeBackground = true;
        plainMove.timeOffsetSeconds = 0.3;
        ownMove = plainMove;
        ownMove.takeBackground = true;
        const auto same = polyphonic::render(A, {x}, {{e4, flagOnly}})[0];
        double err = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            err = std::max(err, std::fabs(static_cast<double>(same[i]) - x[i]));
        CHECK(err < 1e-4);

        // muting the only note with its background leaves next to nothing while it sounds
        const auto muted = polyphonic::render(A, {x}, {{e4, ownMute}})[0];
        double eIn = 0.0, eLeft = 0.0;
        for (int i = static_cast<int>((on + 0.1) * kRate); i < static_cast<int>((on + 1.3) * kRate); ++i)
        {
            eIn += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
            eLeft += static_cast<double>(muted[static_cast<size_t>(i)]) * muted[static_cast<size_t>(i)];
        }
        // moving it takes the attack along: the burst's energy above the tone's partials (9-14 kHz),
        // around the old onset and around the new one
        const auto pMoved = polyphonic::render(A, {x}, {{e4, plainMove}})[0];
        const auto oMoved = polyphonic::render(A, {x}, {{e4, ownMove}})[0];
        auto burstEnergy = [&](const std::vector<float>& y, double t) {
            double e = 0.0;
            for (double hz = 9000.0; hz < 14000.0; hz += 50.0)
                e += std::pow(toneAmplitude(y, hz, static_cast<int>((t - 0.01) * kRate), static_cast<int>((t + 0.05) * kRate)), 2.0);
            return 10.0 * std::log10(e + 1e-30);
        };
        const double orig = burstEnergy(x, on);
        const double oldPlain = burstEnergy(pMoved, on), oldOwn = burstEnergy(oMoved, on);
        const double newPlain = burstEnergy(pMoved, on + 0.3), newOwn = burstEnergy(oMoved, on + 0.3);
        std::printf("own background: muted %.1f dB re input; attack burst %.1f dB, moved 0.3 s: at the old onset %.1f (plain %.1f), at the new %.1f (plain %.1f)\n",
                    10.0 * std::log10(eLeft / eIn), orig, oldOwn, oldPlain, newOwn, newPlain);
        CHECK(10.0 * std::log10(eLeft / eIn) < -40.0);
        CHECK(oldOwn < orig - 10.0 && oldPlain > orig - 3.0);
        CHECK(std::fabs(newOwn - orig) < 3.0 && newPlain < orig - 10.0);

        // on its own it is exactly what the full render adds for it, and Solo with the background
        // is exactly what such a mute removes
        const auto span = polyphonic::renderNotes(A, {x}, {e4}, {{e4, ownMove}});
        std::vector<float> alone(x.size(), 0.0f);
        CHECK(span.channels.size() == 1 && span.start >= 0 && span.start + span.channels[0].size() <= x.size());
        std::copy(span.channels[0].begin(), span.channels[0].end(), alone.begin() + span.start);
        const auto solo = polyphonic::extractNotes(A, {x}, {e4}, nullptr, true)[0];
        double aloneErr = 0.0, soloErr = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            aloneErr = std::max(aloneErr, std::fabs(static_cast<double>(oMoved[i]) - muted[i] - alone[i]));
            soloErr = std::max(soloErr, std::fabs(static_cast<double>(x[i]) - muted[i] - solo[i]));
        }
        std::printf("own background: moved note on its own differs from the full render's share by %.1e, Solo from the mute by %.1e\n",
                    aloneErr, soloErr);
        CHECK(aloneErr < 1e-4 && soloErr < 1e-4);
    }

    std::printf("PolyphonicTests passed\n");
    return 0;
}
