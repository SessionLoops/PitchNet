#include "PolyTempo.h"

#include "PolyEngine.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace polyphonic
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;
constexpr double kChordSeconds = 0.04;   // onsets this close are one attack
constexpr int kMinOnsets = 8;
constexpr double kMinSpanSeconds = 4.0;
constexpr double kPriorCentreBpm = 120.0;
constexpr double kPriorOctaves = 1.0;    // standard deviation of the tempo prior

struct Cluster
{
    double t = 0.0, w = 0.0, bass = 0.0;
};

std::vector<Cluster> cluster(std::vector<TempoOnset> onsets)
{
    onsets.erase(std::remove_if(onsets.begin(), onsets.end(),
                                [](const TempoOnset& o)
                                { return !(o.weight > 0.0) || !std::isfinite(o.seconds); }),
                 onsets.end());
    std::sort(onsets.begin(), onsets.end(),
              [](const TempoOnset& a, const TempoOnset& b) { return a.seconds < b.seconds; });

    std::vector<Cluster> out;
    size_t i = 0;
    while (i < onsets.size())
    {
        // A chord: its time is its strongest note's, its weight grows with the
        // number of notes but less than linearly (a five-note chord is not five
        // beats' worth of evidence).
        const double first = onsets[i].seconds;
        double sum = 0.0, best = -1.0, t = first, bass = 0.0;
        for (; i < onsets.size() && onsets[i].seconds - first <= kChordSeconds; ++i)
        {
            sum += onsets[i].weight;
            bass = std::max(bass, onsets[i].bass);
            if (onsets[i].weight > best)
            {
                best = onsets[i].weight;
                t = onsets[i].seconds;
            }
        }
        out.push_back({t, std::sqrt(sum), bass});
    }
    return out;
}

/** |mean of w e^(2 pi i t / period)|, and its phase as a time in [0, period). */
struct Phase
{
    double strength = 0.0, seconds = 0.0;
};

Phase phaseAt(const std::vector<Cluster>& c, double totalWeight, double period)
{
    std::complex<double> z{};
    const double k = kTwoPi / period;
    for (const auto& o : c)
        z += o.w * std::polar(1.0, k * o.t);
    Phase p;
    p.strength = std::abs(z) / totalWeight;
    double s = std::arg(z) / k;
    if (s < 0.0)
        s += period;
    p.seconds = s;
    return p;
}

double prior(double bpm)
{
    const double octaves = std::log2(bpm / kPriorCentreBpm) / kPriorOctaves;
    return std::exp(-0.5 * octaves * octaves);
}

/** The share of beats from the first onset to the last that have an onset
 *  on them. Phase agreement only counts the onsets there are, so twice the
 *  tempo agrees as well as the tempo itself; the empty beats tell them apart. */
double coverage(const std::vector<Cluster>& c, double period, double phase)
{
    const double tolerance = std::min(0.06, 0.15 * period);
    const double first = c.front().t, last = c.back().t;
    double beat = phase + std::ceil((first - tolerance - phase) / period) * period;
    size_t i = 0;
    int beats = 0, hit = 0;
    for (; beat <= last + tolerance; beat += period)
    {
        ++beats;
        while (i < c.size() && c[i].t < beat - tolerance)
            ++i;
        if (i < c.size() && c[i].t <= beat + tolerance)
            ++hit;
    }
    return beats > 0 ? static_cast<double>(hit) / beats : 0.0;
}

/** How a beat period explains the onsets. Steady subdivisions put half the
 *  onsets between the beats, where they cancel the beat's own phase, so the
 *  half beat counts as well: whichever agrees more is the pulse ("tatum")
 *  the onsets follow. */
struct Pulse
{
    double strength = 0.0; // phase agreement on the pulse, 0..1
    double period = 0.0;   // the beat, or the half beat
    double phase = 0.0;    // where the pulse falls, in [0, period)
};

Pulse pulseAt(const std::vector<Cluster>& c, double totalWeight, double beatPeriod)
{
    const auto beat = phaseAt(c, totalWeight, beatPeriod);
    const auto half = phaseAt(c, totalWeight, beatPeriod * 0.5);
    if (half.strength > beat.strength)
        return {half.strength, beatPeriod * 0.5, half.seconds};
    return {beat.strength, beatPeriod, beat.seconds};
}

double score(const std::vector<Cluster>& c, double totalWeight, double bpm)
{
    const double period = 60.0 / bpm;
    // The pulse, plus the two-beat period (the beat's own alternation). Whole
    // fractions of the beat score as well as the beat; the prior and the
    // share of beats that are played choose between them.
    const auto pulse = pulseAt(c, totalWeight, period);
    const double s = pulse.strength + 0.5 * phaseAt(c, totalWeight, period * 2.0).strength;
    return prior(bpm) * s * coverage(c, pulse.period, pulse.phase);
}

double circularDistance(double t, double origin, double period)
{
    double d = std::fmod(t - origin, period);
    if (d < 0.0)
        d += period;
    return std::min(d, period - d);
}
} // namespace

TempoEstimate estimateTempo(const std::vector<TempoOnset>& onsets, const TempoOptions& options)
{
    TempoEstimate result;
    const auto c = cluster(onsets);
    if (static_cast<int>(c.size()) < kMinOnsets)
        return result;
    const double span = c.back().t - c.front().t;
    if (span < kMinSpanSeconds)
        return result;

    double totalWeight = 0.0, sumSquares = 0.0;
    for (const auto& o : c)
    {
        totalWeight += o.w;
        sumSquares += o.w * o.w;
    }
    if (!(totalWeight > 0.0))
        return result;
    const double effectiveCount = totalWeight * totalWeight / sumSquares;

    // A score peak is about period^2 / span wide in seconds, i.e. period / span
    // in log-period; a few steps per peak finds it, then a finer search
    // around the best few refines it.
    const double minBpm = std::max(20.0, options.minBpm);
    const double maxBpm = std::max(minBpm + 1.0, options.maxBpm);
    const double step = std::clamp(0.2 * (60.0 / maxBpm) / span, 2.0e-5, 2.0e-3);
    const int count = static_cast<int>(std::ceil(std::log(maxBpm / minBpm) / step)) + 1;

    std::vector<std::pair<double, double>> scored; // (score, bpm)
    scored.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const double bpm = minBpm * std::exp(step * i);
        scored.push_back({score(c, totalWeight, bpm), bpm});
    }

    // Local maxima, best first.
    std::vector<std::pair<double, double>> peaks;
    for (int i = 0; i < count; ++i)
    {
        const double s = scored[static_cast<size_t>(i)].first;
        if ((i == 0 || s >= scored[static_cast<size_t>(i - 1)].first) &&
            (i + 1 == count || s >= scored[static_cast<size_t>(i + 1)].first))
            peaks.push_back(scored[static_cast<size_t>(i)]);
    }
    std::sort(peaks.begin(), peaks.end(), [](auto a, auto b) { return a.first > b.first; });
    if (peaks.size() > 5)
        peaks.resize(5);

    double bestScore = -1.0, bestBpm = 0.0;
    for (const auto& peak : peaks)
    {
        for (int j = -20; j <= 20; ++j)
        {
            const double bpm = peak.second * std::exp(step * 0.1 * j);
            if (bpm < minBpm || bpm > maxBpm)
                continue;
            const double s = score(c, totalWeight, bpm);
            if (s > bestScore)
            {
                bestScore = s;
                bestBpm = bpm;
            }
        }
    }
    if (!(bestBpm > 0.0))
        return result;

    // Recordings made to a click are usually at a whole (or half) BPM; prefer
    // it when it explains the onsets about as well.
    for (const double grid : {1.0, 0.5})
    {
        const double rounded = std::round(bestBpm / grid) * grid;
        if (std::abs(rounded - bestBpm) < 0.5 && rounded >= minBpm && rounded <= maxBpm &&
            score(c, totalWeight, rounded) >= 0.97 * bestScore)
        {
            bestBpm = rounded;
            break;
        }
    }

    const double period = 60.0 / bestBpm;
    const auto pulse = pulseAt(c, totalWeight, period);

    // Onsets at random times still agree a little on any phase, and the best
    // of thousands of candidates more so: roughly sqrt(ln(candidates) / N).
    const double chance = std::sqrt(std::log(static_cast<double>(count)) / effectiveCount);
    result.confidence = pulse.strength;
    if (pulse.strength < std::max(0.25, 1.6 * chance))
        return result;

    // The downbeat: the pulse position in the bar where strong and low onsets
    // gather. With a half-beat pulse this also decides which half is the beat.
    const int beatsPerBar = std::max(1, options.beatsPerBar);
    const double bar = period * beatsPerBar;
    const int positions = static_cast<int>(std::lround(bar / pulse.period));
    const double sigma = std::min(0.05, 0.12 * pulse.period);
    int bestBeat = 0;
    double bestBeatScore = -1.0;
    for (int k = 0; k < positions; ++k)
    {
        const double origin = pulse.phase + k * pulse.period;
        double s = 0.0;
        for (const auto& o : c)
        {
            const double d = circularDistance(o.t, origin, bar) / sigma;
            s += o.w * (0.5 + o.bass) * std::exp(-0.5 * d * d);
        }
        if (s > bestBeatScore)
        {
            bestBeatScore = s;
            bestBeat = k;
        }
    }

    double downbeat = std::fmod(pulse.phase + bestBeat * pulse.period, bar);
    if (downbeat < 0.0)
        downbeat += bar;
    result.found = true;
    result.bpm = bestBpm;
    result.downbeatSeconds = downbeat;
    return result;
}

std::vector<TempoOnset> tempoOnsets(const Analysis& analysis)
{
    std::vector<TempoOnset> onsets;
    for (const auto& note : analysis.notes)
    {
        if (!note.active || note.numFrames() <= 0)
            continue;
        TempoOnset o;
        o.seconds = analysis.frameToSeconds(note.startFrame);
        // Energy grows with a note's length as well as its loudness; the log
        // keeps one long note from outweighing the rhythm around it.
        o.weight = std::log1p(std::max(0.0, note.energy)) * std::clamp(note.probability, 0.05f, 1.0f);
        if (!(o.weight > 0.0))
            continue;
        o.bass = std::clamp((72.0 - static_cast<double>(note.medianMidi())) / 36.0, 0.0, 1.0);
        onsets.push_back(o);
    }
    return onsets;
}
} // namespace polyphonic
