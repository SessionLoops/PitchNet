// Tempo and downbeat estimation from polyphonic note onsets (PolyTempo.h).
// Part one feeds onsets directly; part two renders a short rhythmic piece and
// runs it through the engine, so the onsets are the ones the editor would see.

#include "../Source/Audio/Polyphonic/PolyEngine.h"
#include "../Source/Audio/Polyphonic/PolyTempo.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 44100;

struct Rng
{
    uint64_t s = 0x9e3779b97f4a7c15ull;
    double uniform() // [0, 1)
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
    double jitter(double width) { return (uniform() - 0.5) * 2.0 * width; }
};

// Distance between two times on a circle of `period` seconds.
double circular(double a, double b, double period)
{
    double d = std::fmod(a - b, period);
    if (d < 0.0)
        d += period;
    return std::min(d, period - d);
}

// A groove: every beat, a louder low note on the downbeat, offbeat eighths
// some of the time, a few ghost notes, timing jitter.
std::vector<polyphonic::TempoOnset> groove(double bpm, int beatsPerBar, double downbeat,
                                           double seconds, uint64_t seed, double offbeatShare = 0.5)
{
    Rng rng;
    rng.s ^= seed;
    std::vector<polyphonic::TempoOnset> out;
    const double beat = 60.0 / bpm;
    int index = 0;
    for (double t = downbeat - beat * beatsPerBar; t < seconds; t += beat, ++index)
    {
        const bool down = ((index % beatsPerBar) + beatsPerBar) % beatsPerBar == 0;
        if (t >= 0.0)
        {
            // A chord on the beat: melody and harmony, a bass note on the downbeat.
            out.push_back({t + rng.jitter(0.012), down ? 1.0 : 0.7, 0.1});
            out.push_back({t + rng.jitter(0.012), 0.6, 0.2});
            if (down)
                out.push_back({t + rng.jitter(0.012), 1.2, 0.9});
        }
        const double off = t + beat * 0.5;
        if (off >= 0.0 && off < seconds && rng.uniform() < offbeatShare)
            out.push_back({off + rng.jitter(0.012), 0.5, 0.1});
        if (rng.uniform() < 0.08)
        {
            const double ghost = t + rng.uniform() * beat;
            if (ghost >= 0.0 && ghost < seconds)
                out.push_back({ghost, 0.2, 0.0});
        }
    }
    return out;
}

void expectTempo(const char *name, const polyphonic::TempoEstimate &e, double bpm, double downbeat,
                 int beatsPerBar)
{
    std::printf("%s: found=%d bpm=%.3f downbeat=%.3f confidence=%.2f (want %.2f @ %.3f)\n", name,
                e.found ? 1 : 0, e.bpm, e.downbeatSeconds, e.confidence, bpm, downbeat);
    CHECK(e.found);
    CHECK(std::abs(e.bpm - bpm) < 0.05);
    CHECK(circular(e.downbeatSeconds, downbeat, 60.0 / bpm * beatsPerBar) < 0.03);
    CHECK(e.downbeatSeconds >= 0.0 && e.downbeatSeconds < 60.0 / bpm * beatsPerBar + 1e-9);
}

// A harmonic tone with a fast attack and an exponential decay.
void addTone(std::vector<float> &x, double f0, double start, double dur, double gain)
{
    const int s0 = static_cast<int>(start * kRate), n = static_cast<int>(dur * kRate);
    for (int h = 1; h <= 12; ++h)
    {
        const double fh = h * f0;
        if (fh > 12000.0)
            break;
        const double a = gain / h;
        for (int i = 0; i < n && s0 + i < static_cast<int>(x.size()); ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            const double env = std::min(1.0, t / 0.004) * std::exp(-t * (2.5 + 0.4 * h));
            x[static_cast<size_t>(s0 + i)] += static_cast<float>(a * env * std::sin(2.0 * kPi * fh * t));
        }
    }
}

double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }
} // namespace

int main()
{
    // ---- 1. onsets: whole and fractional tempos, downbeat away from zero ----
    {
        polyphonic::TempoOptions options;
        expectTempo("97 bpm 4/4", polyphonic::estimateTempo(groove(97.0, 4, 0.37, 60.0, 1), options),
                    97.0, 0.37, 4);
        expectTempo("128 bpm 4/4", polyphonic::estimateTempo(groove(128.0, 4, 1.10, 40.0, 2), options),
                    128.0, 1.10, 4);
        expectTempo("123.4 bpm 4/4",
                    polyphonic::estimateTempo(groove(123.4, 4, 0.05, 90.0, 3), options), 123.4, 0.05, 4);
        expectTempo("84 bpm, no offbeats",
                    polyphonic::estimateTempo(groove(84.0, 4, 0.9, 50.0, 4, 0.0), options), 84.0, 0.9, 4);
        // Even eighths with no accent but a bass note per bar: half the
        // onsets fall between the beats (the case that failed in the app).
        {
            std::vector<polyphonic::TempoOnset> eighths;
            Rng rng;
            const double beat = 60.0 / 92.0;
            int k = 0;
            for (double t = 0.25; t < 20.0; t += beat * 0.5, ++k)
            {
                eighths.push_back({t + 0.025 + rng.jitter(0.015), 0.9, 0.2});
                if (k % 8 == 0)
                    eighths.push_back({t + 0.025 + rng.jitter(0.015), 1.0, 1.0});
            }
            expectTempo("92 bpm even eighths", polyphonic::estimateTempo(eighths, options), 92.0,
                        0.275, 4);
        }
        options.beatsPerBar = 3;
        expectTempo("140 bpm 3/4", polyphonic::estimateTempo(groove(140.0, 3, 0.6, 45.0, 5), options),
                    140.0, 0.6, 3);
    }

    // ---- 2. no tempo: random onsets, too few onsets, too short ----
    {
        int falsePositives = 0;
        for (uint64_t seed = 1; seed <= 20; ++seed)
        {
            Rng rng;
            rng.s ^= seed * 7919;
            std::vector<polyphonic::TempoOnset> random;
            for (int i = 0; i < 120; ++i)
                random.push_back({rng.uniform() * 60.0, 0.3 + rng.uniform(), rng.uniform()});
            const auto e = polyphonic::estimateTempo(random);
            falsePositives += e.found ? 1 : 0;
        }
        std::printf("random onsets: %d of 20 found a tempo\n", falsePositives);
        CHECK(falsePositives <= 1);

        auto few = groove(100.0, 4, 0.0, 60.0, 6);
        few.resize(5);
        CHECK(!polyphonic::estimateTempo(few).found);
        CHECK(!polyphonic::estimateTempo(groove(100.0, 4, 0.0, 2.5, 7)).found);
        CHECK(!polyphonic::estimateTempo(std::vector<polyphonic::TempoOnset>{}).found);
    }

    // ---- 3. through the engine: a bass-and-chords pattern at 104 BPM ----
    {
        const double bpm = 104.0, beat = 60.0 / bpm, downbeat = 0.42;
        const double seconds = 20.0;
        std::vector<float> x(static_cast<size_t>((seconds + 1.0) * kRate), 0.0f);
        const int chords[4][3] = {{60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}};
        const int bassNotes[4] = {36, 33, 29, 31};
        int index = 0;
        for (double t = downbeat; t < seconds; t += beat, ++index)
        {
            const int bar = (index / 4) % 4;
            if (index % 4 == 0)
                addTone(x, midiToHz(bassNotes[bar]), t, beat * 3.5, 0.35);
            for (int m : chords[bar])
                addTone(x, midiToHz(m), t, beat * 0.9, index % 4 == 0 ? 0.16 : 0.11);
        }
        const auto params = polyphonic::Params::forSampleRate(kRate);
        const auto analysis = polyphonic::analyze(x.data(), static_cast<int>(x.size()), params);
        const auto onsets = polyphonic::tempoOnsets(analysis);
        std::printf("engine: %zu onsets\n", onsets.size());
        expectTempo("engine 104 bpm", polyphonic::estimateTempo(analysis), bpm, downbeat, 4);
    }

    std::printf("PolyTempoTests passed\n");
    return 0;
}
