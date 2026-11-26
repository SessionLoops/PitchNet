#include "PolyChords.h"

#include "PolyEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <cctype>
#include <limits>
#include <optional>

namespace polyphonic
{
namespace
{
constexpr int kNoChordState = -1;
constexpr double kLevelExponent = 0.8;   // compresses levels a little, so a loud note does not drown the rest
constexpr double kRecentShare = 0.6;     // a note's weight falls to 1 - this share as its attack recedes ...
constexpr double kRecentSeconds = 1.5;   // ... over this time: what is struck now outweighs what still rings
constexpr float kMelodyMidi = 72.0f;     // above C5, notes weigh less (a melody's passing notes are not the harmony)
constexpr double kMinGridSlice = 0.3;    // a faster beat is read two beats at a time
constexpr double kSignificant = 0.12;    // a pitch class counts at this share of the slice's strongest
constexpr double kSilentShare = 0.02;    // a slice this far below the take's typical slice is silent
constexpr double kNoChordSilent = 1.0;   // no-chord score of a silent slice
constexpr double kNoChordSingle = 0.75;  // ... of a slice with one pitch class, where a chord scores at most ...
constexpr double kSingleChordCap = 0.6;  // ... this: a passing melody note continues a chord, a lone line has none
constexpr double kNoChordDefault = 0.35; // ... otherwise

std::uint16_t mask(std::initializer_list<int> intervals)
{
    std::uint16_t m = 0;
    for (int i : intervals)
        m = static_cast<std::uint16_t>(m | (1u << i));
    return m;
}

struct Quality
{
    std::uint16_t intervals;
    const char* suffix;
    double prior;  // added to the score; favours the simpler reading of an ambiguous slice
    bool detect;   // a template for detection, or only a name for host chords
};

const std::vector<Quality>& qualities()
{
    // Detection templates first, in menu-like order; then names only, for
    // chords that are typed or come from a host (anything else is named by
    // chordSuffix's general rules).
    static const std::vector<Quality> table = {
        {mask({0, 4, 7}), "", 0.0, true},
        {mask({0, 3, 7}), "m", -0.01, true},
        {mask({0, 4, 7, 10}), "7", -0.03, true},
        {mask({0, 4, 7, 11}), "maj7", -0.04, true},
        {mask({0, 3, 7, 10}), "m7", -0.04, true},
        {mask({0, 3, 6}), "dim", -0.06, true},
        {mask({0, 3, 6, 10}), "m7b5", -0.06, true},
        {mask({0, 3, 6, 9}), "dim7", -0.08, true},
        {mask({0, 4, 8}), "aug", -0.10, true},
        {mask({0, 5, 7}), "sus4", -0.07, true},
        {mask({0, 2, 7}), "sus2", -0.08, true},
        {mask({0, 5, 7, 10}), "7sus4", -0.09, true},
        {mask({0, 4, 7, 9}), "6", -0.07, true},
        {mask({0, 3, 7, 9}), "m6", -0.08, true},
        {mask({0, 7}), "5", -0.06, true},
        {mask({0, 2, 4, 7}), "add9", -0.05, true},
        {mask({0, 2, 3, 7}), "madd9", -0.06, true},
        {mask({0, 2, 4, 7, 10}), "9", -0.09, true},
        {mask({0, 2, 4, 7, 11}), "maj9", -0.10, true},
        {mask({0, 2, 3, 7, 10}), "m9", -0.10, true},
        {mask({0, 3, 7, 11}), "mMaj7", -0.12, true},
        {mask({0, 2, 4, 7, 9}), "6/9", -0.12, true},
        {mask({0, 4, 8, 10}), "7#5", -0.12, true},
        // names only
        {mask({0, 2, 3, 7, 9}), "m6/9", 0.0, false},
        {mask({0, 4, 6, 10}), "7b5", 0.0, false},
        {mask({0, 4, 8, 11}), "maj7#5", 0.0, false},
        {mask({0, 1, 4, 7, 10}), "7b9", 0.0, false},
        {mask({0, 3, 4, 7, 10}), "7#9", 0.0, false},
        {mask({0, 4, 6, 7, 10}), "7#11", 0.0, false},
        {mask({0, 4, 7, 8, 10}), "7b13", 0.0, false},
        {mask({0, 2, 5, 7, 10}), "9sus4", 0.0, false},
        {mask({0, 2, 4, 5, 7, 10}), "11", 0.0, false},
        {mask({0, 2, 3, 5, 7, 10}), "m11", 0.0, false},
        {mask({0, 2, 4, 7, 9, 10}), "13", 0.0, false},
        {mask({0, 2, 3, 5, 7, 9, 10}), "m13", 0.0, false},
        {mask({0, 2, 4, 7, 9, 11}), "maj13", 0.0, false},
        {mask({0, 2, 4, 6, 7, 11}), "maj9#11", 0.0, false},
        {mask({0, 4, 5, 7}), "add11", 0.0, false},
        {mask({0, 3, 5, 7}), "madd11", 0.0, false},
        {mask({0, 4}), "(no5)", 0.0, false},
        {mask({0, 3}), "m(no5)", 0.0, false},
    };
    return table;
}

int popcount(std::uint16_t m)
{
    int n = 0;
    for (; m != 0; m = static_cast<std::uint16_t>(m & (m - 1)))
        ++n;
    return n;
}

bool has(std::uint16_t intervals, int i) { return (intervals >> (((i % 12) + 12) % 12)) & 1u; }

int pitchClassOf(float midi)
{
    const int m = static_cast<int>(std::lround(midi));
    return ((m % 12) + 12) % 12;
}

struct Slice
{
    double start = 0.0, end = 0.0;
    std::array<double, 12> chroma{};
    std::array<double, 12> bass{}; // sums to 1 when anything sounds
    double total = 0.0;
};

/** Slice boundaries over [first, last). */
std::vector<double> boundaries(double first, double last, const ChordOptions& options)
{
    std::vector<double> b;
    double step = options.sliceSeconds > 0.0 ? options.sliceSeconds : 0.25;
    double origin = 0.0;
    if (options.beatSeconds > 0.0 && std::isfinite(options.beatSeconds))
    {
        step = options.beatSeconds;
        while (step < kMinGridSlice)
            step *= 2.0;
        origin = std::isfinite(options.gridOriginSeconds) ? options.gridOriginSeconds : 0.0;
    }
    const double k0 = std::floor((first - origin) / step);
    const double k1 = std::ceil((last - origin) / step);
    if (!(k1 > k0) || k1 - k0 > 200000.0)
        return b;
    for (double k = k0; k <= k1; k += 1.0)
        b.push_back(origin + k * step);
    return b;
}

/** Each slice's pitch-class and bass profiles. */
std::vector<Slice> profile(const std::vector<ChordNote>& notes, const std::vector<double>& edges)
{
    std::vector<Slice> slices(edges.size() > 1 ? edges.size() - 1 : 0);
    for (size_t s = 0; s < slices.size(); ++s)
    {
        slices[s].start = edges[s];
        slices[s].end = edges[s + 1];
    }
    if (slices.empty())
        return slices;

    double peak = 0.0;
    for (const auto& n : notes)
        for (float v : n.levels)
            if (std::isfinite(v))
                peak = std::max(peak, static_cast<double>(v));
    const double levelScale = peak > 0.0 ? 1.0 / peak : 1.0;

    // per slice: (midi, weight) of each note sounding in it, for the bass
    std::vector<std::vector<std::pair<float, double>>> sounding(slices.size());
    const double first = edges.front();
    const double width = (edges.back() - first) / static_cast<double>(slices.size());

    auto sliceIndex = [&](double t)
    {
        const double k = std::floor((t - first) / width);
        return static_cast<int>(std::clamp(k, 0.0, static_cast<double>(slices.size() - 1)));
    };

    for (const auto& n : notes)
    {
        if (!std::isfinite(n.midi) || !std::isfinite(n.startSeconds) || !std::isfinite(n.endSeconds)
            || n.endSeconds <= n.startSeconds || !(n.gain > 0.0))
            continue;
        const int pc = pitchClassOf(n.midi);
        const double gain = std::pow(n.gain, kLevelExponent)
            / (1.0 + std::max(0.0, static_cast<double>(n.midi - kMelodyMidi)) / 12.0);
        auto recency = [&](double t)
        { return 1.0 - kRecentShare + kRecentShare * std::exp(-std::max(0.0, t - n.startSeconds) / kRecentSeconds); };
        std::vector<double> weight(slices.size(), 0.0);
        int lo = sliceIndex(n.startSeconds), hi = sliceIndex(n.endSeconds);

        auto add = [&](double a, double b, double level)
        {
            if (b <= a || level <= 0.0)
                return;
            level *= recency(0.5 * (a + b));
            for (int s = sliceIndex(a); s < static_cast<int>(slices.size()); ++s)
            {
                const double o = std::min(b, slices[static_cast<size_t>(s)].end)
                    - std::max(a, slices[static_cast<size_t>(s)].start);
                if (slices[static_cast<size_t>(s)].start >= b)
                    break;
                if (o > 0.0)
                    weight[static_cast<size_t>(s)] += o * level;
            }
        };

        if (n.levels.empty() || !(n.levelStepSeconds > 0.0))
        {
            // in short steps, so the weight follows the attack's age
            constexpr double step = 0.05;
            for (double a = n.startSeconds; a < n.endSeconds; a += step)
                add(a, std::min(n.endSeconds, a + step), 1.0);
        }
        else
        {
            for (size_t k = 0; k < n.levels.size(); ++k)
            {
                const double a = n.startSeconds + static_cast<double>(k) * n.levelStepSeconds;
                if (a >= n.endSeconds)
                    break;
                const double b = std::min(n.endSeconds, a + n.levelStepSeconds);
                const float v = n.levels[k];
                if (std::isfinite(v) && v > 0.0f)
                    add(a, b, std::pow(static_cast<double>(v) * levelScale, kLevelExponent));
            }
        }

        for (int s = lo; s <= hi; ++s)
        {
            const double w = weight[static_cast<size_t>(s)] * gain;
            if (w <= 0.0)
                continue;
            slices[static_cast<size_t>(s)].chroma[static_cast<size_t>(pc)] += w;
            slices[static_cast<size_t>(s)].total += w;
            sounding[static_cast<size_t>(s)].push_back({n.midi, w});
        }
    }

    // The bass: the lowest notes that are not just a trace.
    for (size_t s = 0; s < slices.size(); ++s)
    {
        auto& slice = slices[s];
        double strongest = 0.0;
        for (const auto& [m, w] : sounding[s])
            strongest = std::max(strongest, w);
        float lowest = std::numeric_limits<float>::max();
        for (const auto& [m, w] : sounding[s])
            if (w >= 0.2 * strongest)
                lowest = std::min(lowest, m);
        double sum = 0.0;
        for (const auto& [m, w] : sounding[s])
        {
            if (w < 0.2 * strongest)
                continue;
            const double b = w * std::exp(-(m - lowest) / 3.0);
            slice.bass[static_cast<size_t>(pitchClassOf(m))] += b;
            sum += b;
        }
        if (sum > 0.0)
            for (auto& b : slice.bass)
                b /= sum;
    }
    return slices;
}

double templateScore(const Slice& slice, double norm, int root, const Quality& q)
{
    const int size = popcount(q.intervals);
    double dot = 0.0, bassIn = 0.0, bassOut = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        const auto pc = static_cast<size_t>((root + i) % 12);
        if (has(q.intervals, i))
        {
            dot += slice.chroma[pc];
            if (i != 0)
                bassIn += slice.bass[pc];
        }
        else
        {
            bassOut += slice.bass[pc];
        }
    }
    const double cosine = dot / (norm * std::sqrt(static_cast<double>(size)));
    const double bassTerm = 0.12 * slice.bass[static_cast<size_t>(root)] + 0.03 * bassIn - 0.06 * bassOut;
    return cosine + q.prior + bassTerm;
}

double nearestOnset(const std::vector<ChordNote>& notes, const Chord& chord, double t, double before,
                    double after)
{
    double best = t, bestDistance = std::numeric_limits<double>::max();
    for (const auto& n : notes)
    {
        if (!std::isfinite(n.midi) || !has(chord.intervals, pitchClassOf(n.midi) - chord.root))
            continue;
        const double d = n.startSeconds - t;
        if (d < -before || d > after)
            continue;
        if (std::abs(d) < bestDistance)
        {
            bestDistance = std::abs(d);
            best = n.startSeconds;
        }
    }
    return best;
}

const std::array<double, 12> kMajorProfile = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                              2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
const std::array<double, 12> kMinorProfile = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                              2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

double correlation(const std::array<double, 12>& x, const std::array<double, 12>& profile, int tonic)
{
    double mx = 0.0, mp = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        mx += x[static_cast<size_t>(i)];
        mp += profile[static_cast<size_t>(i)];
    }
    mx /= 12.0;
    mp /= 12.0;
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        const double a = x[static_cast<size_t>((tonic + i) % 12)] - mx;
        const double b = profile[static_cast<size_t>(i)] - mp;
        sxy += a * b;
        sxx += a * a;
        syy += b * b;
    }
    return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt(sxx * syy) : 0.0;
}
} // namespace

// ---------------------------------------------------------------------------

std::vector<ChordSpan> detectChords(const std::vector<ChordNote>& notes, const ChordOptions& options)
{
    double first = std::numeric_limits<double>::max(), last = std::numeric_limits<double>::lowest();
    for (const auto& n : notes)
    {
        if (!std::isfinite(n.startSeconds) || !std::isfinite(n.endSeconds) || n.endSeconds <= n.startSeconds)
            continue;
        first = std::min(first, n.startSeconds);
        last = std::max(last, n.endSeconds);
    }
    if (!(last > first))
        return {};

    const auto edges = boundaries(first, last, options);
    const auto slices = profile(notes, edges);
    if (slices.empty())
        return {};

    // What a silent slice is, relative to the take's busier slices.
    std::vector<double> totals;
    for (const auto& s : slices)
        if (s.total > 0.0)
            totals.push_back(s.total);
    if (totals.empty())
        return {};
    std::sort(totals.begin(), totals.end());
    const double typical = totals[static_cast<size_t>(0.9 * static_cast<double>(totals.size() - 1))];
    const double silent = kSilentShare * typical;

    // ---- scores: state = quality * 12 + root ----
    std::vector<const Quality*> templates;
    for (const auto& q : qualities())
        if (q.detect)
            templates.push_back(&q);
    const int numChordStates = static_cast<int>(templates.size()) * 12;
    const int numStates = numChordStates + 1; // + no chord
    const int none = numChordStates;
    const size_t T = slices.size();
    std::vector<double> emission(T * static_cast<size_t>(numStates), -1.0);

    for (size_t t = 0; t < T; ++t)
    {
        const auto& s = slices[t];
        double* e = &emission[t * static_cast<size_t>(numStates)];
        if (s.total <= silent)
        {
            e[none] = kNoChordSilent;
            continue;
        }
        double strongest = 0.0, norm = 0.0;
        for (double c : s.chroma)
        {
            strongest = std::max(strongest, c);
            norm += c * c;
        }
        norm = std::sqrt(norm);
        int present = 0;
        for (double c : s.chroma)
            present += c >= kSignificant * strongest ? 1 : 0;
        e[none] = present <= 1 ? kNoChordSingle : kNoChordDefault;
        for (size_t q = 0; q < templates.size(); ++q)
            for (int root = 0; root < 12; ++root)
            {
                const double v = templateScore(s, norm, root, *templates[q]);
                e[q * 12 + static_cast<size_t>(root)] = present <= 1 ? std::min(v, kSingleChordCap) : v;
            }
    }

    // ---- Viterbi: every change costs the same ----
    const double cost = options.changeCost;
    std::vector<double> score(static_cast<size_t>(numStates)), next(static_cast<size_t>(numStates));
    std::vector<int> back(T * static_cast<size_t>(numStates), 0);
    for (int k = 0; k < numStates; ++k)
        score[static_cast<size_t>(k)] = emission[static_cast<size_t>(k)];
    for (size_t t = 1; t < T; ++t)
    {
        int bestPrev = 0;
        for (int k = 1; k < numStates; ++k)
            if (score[static_cast<size_t>(k)] > score[static_cast<size_t>(bestPrev)])
                bestPrev = k;
        for (int k = 0; k < numStates; ++k)
        {
            const double stay = score[static_cast<size_t>(k)];
            const double change = score[static_cast<size_t>(bestPrev)] - cost;
            const bool keep = stay >= change;
            next[static_cast<size_t>(k)] = (keep ? stay : change)
                + emission[t * static_cast<size_t>(numStates) + static_cast<size_t>(k)];
            back[t * static_cast<size_t>(numStates) + static_cast<size_t>(k)] = keep ? k : bestPrev;
        }
        std::swap(score, next);
    }
    std::vector<int> path(T);
    {
        int k = 0;
        for (int j = 1; j < numStates; ++j)
            if (score[static_cast<size_t>(j)] > score[static_cast<size_t>(k)])
                k = j;
        for (size_t t = T; t-- > 0;)
        {
            path[t] = k;
            if (t > 0)
                k = back[t * static_cast<size_t>(numStates) + static_cast<size_t>(k)];
        }
    }

    // ---- spans ----
    const bool grid = options.beatSeconds > 0.0;
    const bool flats = keyPrefersFlats(notes);
    std::vector<ChordSpan> spans;
    size_t t = 0;
    while (t < T)
    {
        size_t u = t;
        while (u + 1 < T && path[u + 1] == path[t])
            ++u;
        const int state = path[t] == none ? kNoChordState : path[t];
        if (state != kNoChordState)
        {
            const auto& q = *templates[static_cast<size_t>(state / 12)];
            ChordSpan span;
            span.chord.root = state % 12;
            span.chord.intervals = q.intervals;
            span.chord.bass = span.chord.root;

            // an inversion: another chord tone carries the bass through the span
            std::array<double, 12> bass{};
            double sum = 0.0;
            for (size_t k = t; k <= u; ++k)
                for (int pc = 0; pc < 12; ++pc)
                {
                    bass[static_cast<size_t>(pc)] += slices[k].bass[static_cast<size_t>(pc)] * slices[k].total;
                    sum += slices[k].bass[static_cast<size_t>(pc)] * slices[k].total;
                }
            if (sum > 0.0)
            {
                const auto top = static_cast<int>(std::max_element(bass.begin(), bass.end()) - bass.begin());
                if (top != span.chord.root && has(q.intervals, top - span.chord.root)
                    && bass[static_cast<size_t>(top)] >= 0.5 * sum
                    && bass[static_cast<size_t>(span.chord.root)] < 0.3 * sum)
                    span.chord.bass = top;
            }

            span.startSeconds = slices[t].start;
            span.endSeconds = slices[u].end;
            if (!grid)
            {
                // Free time: the chord starts where its notes do.
                const double w = slices[t].end - slices[t].start;
                span.startSeconds = nearestOnset(notes, span.chord, slices[t].start, w, w);
            }
            span.startSeconds = std::max(span.startSeconds, first);
            span.endSeconds = std::min(span.endSeconds, last);
            span.name = chordName(span.chord, flats);
            spans.push_back(std::move(span));
        }
        t = u + 1;
    }

    // A chord lasts until the next one starts: it rings over a short gap, and a
    // chord whose notes come in before its slice ends the previous one there.
    const double sliceWidth = (edges.back() - edges.front()) / static_cast<double>(T);
    for (size_t i = 1; i < spans.size(); ++i)
        if (spans[i].startSeconds - spans[i - 1].endSeconds < sliceWidth)
            spans[i - 1].endSeconds = spans[i].startSeconds;
    spans.erase(std::remove_if(spans.begin(), spans.end(),
                               [](const ChordSpan& s) { return !(s.endSeconds > s.startSeconds); }),
                spans.end());
    return spans;
}

std::vector<ChordSpan> chordsOver(const std::vector<ChordNote>& notes, const std::vector<double>& boundaries)
{
    std::vector<double> edges;
    for (double b : boundaries)
        if (std::isfinite(b) && (edges.empty() || b > edges.back()))
            edges.push_back(b);
    if (edges.size() < 2)
        return {};
    const auto slices = profile(notes, edges);
    const bool flats = keyPrefersFlats(notes);

    // Silence by level per second: the spans can be of any length.
    std::vector<double> rates;
    for (const auto& s : slices)
        if (s.total > 0.0)
            rates.push_back(s.total / (s.end - s.start));
    std::sort(rates.begin(), rates.end());
    const double typical =
        rates.empty() ? 0.0 : rates[static_cast<size_t>(0.9 * static_cast<double>(rates.size() - 1))];

    std::vector<ChordSpan> out;
    for (const auto& s : slices)
    {
        ChordSpan span;
        span.startSeconds = s.start;
        span.endSeconds = s.end;
        double strongest = 0.0, norm = 0.0;
        for (double c : s.chroma)
        {
            strongest = std::max(strongest, c);
            norm += c * c;
        }
        norm = std::sqrt(norm);
        int present = 0;
        for (double c : s.chroma)
            present += strongest > 0.0 && c >= kSignificant * strongest ? 1 : 0;
        if (s.total / (s.end - s.start) > kSilentShare * typical && present >= 2)
        {
            double best = -std::numeric_limits<double>::max();
            for (const auto& q : qualities())
            {
                if (!q.detect)
                    continue;
                for (int root = 0; root < 12; ++root)
                {
                    const double v = templateScore(s, norm, root, q);
                    if (v > best)
                    {
                        best = v;
                        span.chord.root = root;
                        span.chord.intervals = q.intervals;
                    }
                }
            }
            span.chord.bass = span.chord.root;
            const auto top = static_cast<int>(std::max_element(s.bass.begin(), s.bass.end()) - s.bass.begin());
            if (top != span.chord.root && has(span.chord.intervals, top - span.chord.root)
                && s.bass[static_cast<size_t>(top)] >= 0.5 && s.bass[static_cast<size_t>(span.chord.root)] < 0.3)
                span.chord.bass = top;
            span.name = chordName(span.chord, flats);
        }
        out.push_back(std::move(span));
    }
    return out;
}

std::vector<ChordNote> chordNotes(const Analysis& analysis, const std::vector<std::vector<float>>* levels,
                                  const std::map<int, NoteEdit>* edits)
{
    std::vector<ChordNote> out;
    if (!(analysis.params.sampleRate > 0.0) || analysis.params.hop <= 0)
        return out;
    const double hop = analysis.frameToSeconds(1.0);
    // each note, then each copy (a pasted note) where it plays
    std::vector<std::pair<size_t, const NoteEdit*>> entries;
    for (size_t i = 0; i < analysis.notes.size(); ++i)
    {
        const NoteEdit* edit = nullptr;
        if (edits != nullptr)
            if (const auto it = edits->find(static_cast<int>(i)); it != edits->end())
                edit = &it->second;
        entries.emplace_back(i, edit);
    }
    if (edits != nullptr)
        for (auto it = edits->begin(); it != edits->end() && isCopyKey(it->first); ++it)
            if (it->second.copyOf >= 0 && static_cast<size_t>(it->second.copyOf) < analysis.notes.size())
                entries.emplace_back(static_cast<size_t>(it->second.copyOf), &it->second);
    for (const auto& [i, edit] : entries)
    {
        const auto& note = analysis.notes[i];
        if (!note.active || note.f0.empty())
            continue;
        if (edit != nullptr && edit->mute)
            continue;

        ChordNote n;
        n.midi = note.medianMidi();
        n.startSeconds = analysis.frameToSeconds(note.startFrame);
        double frames = note.numFrames();
        n.levelStepSeconds = hop;
        if (levels != nullptr && i < levels->size())
            n.levels = (*levels)[i];
        if (n.levels.empty() && note.energy > 0.0)
            // no levels: a constant one from the energy the note took out of the mix
            n.levels.assign(static_cast<size_t>(note.numFrames()),
                            static_cast<float>(std::sqrt(note.energy / note.numFrames())));
        if (edit != nullptr)
        {
            float curve = 0.0f;
            if (!edit->pitchCurve.empty())
            {
                double sum = 0.0;
                for (float c : edit->pitchCurve)
                    sum += c;
                curve = static_cast<float>(sum / static_cast<double>(edit->pitchCurve.size()));
            }
            n.midi += edit->semitones + curve;
            n.startSeconds += edit->timeOffsetSeconds;
            n.gain = std::pow(10.0, edit->gainDb / 20.0);
            const double stretch = std::clamp(static_cast<double>(edit->stretch), 0.0, 4.0);
            if (stretch > 1.0)
            {
                n.levelStepSeconds = hop * stretch;
                frames *= stretch;
            }
            else if (stretch < 1.0)
            {
                frames = std::max(1.0, std::ceil(frames * stretch));
                if (n.levels.size() > static_cast<size_t>(frames))
                    n.levels.resize(static_cast<size_t>(frames));
            }
        }
        n.endSeconds = n.startSeconds + frames * hop;
        out.push_back(std::move(n));
    }
    return out;
}

// ---- retuning ----

namespace
{
int circularDelta(int from, int to)
{
    int d = ((to - from) % 12 + 12) % 12; // 0..11
    if (d > 6)
        d -= 12;                          // -5..6
    return d;
}

/** How much a tone of a chord matters to hearing it as that chord. */
double toneWeight(int interval)
{
    switch (interval)
    {
    case 0: return 1.5;           // root
    case 3: case 4: return 1.5;   // third
    case 10: case 11: return 1.0; // seventh
    default: return 0.7;          // fifth, sixth, suspensions, extensions
    }
}

std::vector<int> tonesOf(const Chord& c)
{
    std::vector<int> t;
    for (int i = 0; i < 12; ++i)
        if (has(c.intervals, i))
            t.push_back((c.root + i) % 12);
    return t;
}

int effectivePitchClass(const Note& note, const NoteEdit* edit)
{
    float midi = note.medianMidi();
    if (edit != nullptr)
        midi += edit->semitones;
    return pitchClassOf(midi);
}
} // namespace

std::array<int, 12> chordToneMotion(const Chord& from, const Chord& to, std::uint16_t* missing)
{
    std::array<int, 12> motion;
    motion.fill(kNotAChordTone);
    if (missing != nullptr)
        *missing = 0;
    if (from.isNone() || to.isNone())
        return motion;

    const auto source = tonesOf(from);
    const auto target = tonesOf(to);
    constexpr double kUncovered = 4.0; // a lost tone of the new chord, per unit of weight

    // Brute force over every assignment: a chord has at most a handful of tones.
    std::vector<size_t> pick(source.size(), 0), best;
    double bestCost = std::numeric_limits<double>::max();
    int bestLargest = 99;
    while (true)
    {
        double cost = 0.0;
        int largest = 0;
        std::uint16_t covered = 0;
        for (size_t i = 0; i < source.size(); ++i)
        {
            const int d = circularDelta(source[i], target[pick[i]]);
            cost += std::abs(d);
            largest = std::max(largest, std::abs(d));
            covered = static_cast<std::uint16_t>(covered | (1u << pick[i]));
        }
        for (size_t j = 0; j < target.size(); ++j)
            if (!(covered & (1u << j)))
                cost += kUncovered * toneWeight(((target[j] - to.root) % 12 + 12) % 12);
        if (cost < bestCost - 1.0e-9 || (std::abs(cost - bestCost) <= 1.0e-9 && largest < bestLargest))
        {
            bestCost = cost;
            bestLargest = largest;
            best = pick;
        }
        size_t k = 0;
        while (k < pick.size() && ++pick[k] == target.size())
            pick[k++] = 0;
        if (k == pick.size())
            break;
    }

    std::uint16_t covered = 0;
    for (size_t i = 0; i < source.size(); ++i)
    {
        motion[static_cast<size_t>(source[i])] = circularDelta(source[i], target[best[i]]);
        covered = static_cast<std::uint16_t>(covered | (1u << ((target[best[i]] - to.root + 12) % 12)));
    }
    if (missing != nullptr)
        *missing = static_cast<std::uint16_t>(to.intervals & ~covered);
    return motion;
}

ChordRetune retuneToChord(const Analysis& analysis, const std::map<int, NoteEdit>& edits,
                          double startSeconds, double endSeconds, const Chord& from, const Chord& to,
                          double toleranceSeconds)
{
    ChordRetune result;
    result.edits = edits;
    const auto motion = chordToneMotion(from, to, &result.missing);
    if (from.isNone() || to.isNone())
        return result;

    // The notes of the span that are tones of the original chord.
    struct Tone
    {
        int index;
        float midi;
        int pc;
    };
    std::vector<Tone> tones;
    // each note, then each copy (a pasted note, retuned by its own edit)
    std::vector<std::pair<int, size_t>> entries; // (key in edits, analysis note)
    for (size_t i = 0; i < analysis.notes.size(); ++i)
        entries.emplace_back(static_cast<int>(i), i);
    for (auto it = edits.begin(); it != edits.end() && isCopyKey(it->first); ++it)
        if (it->second.copyOf >= 0 && static_cast<size_t>(it->second.copyOf) < analysis.notes.size())
            entries.emplace_back(it->first, static_cast<size_t>(it->second.copyOf));
    for (const auto& [key, i] : entries)
    {
        const auto& note = analysis.notes[i];
        if (!note.active || note.f0.empty())
            continue;
        const auto it = edits.find(key);
        const NoteEdit* edit = it != edits.end() ? &it->second : nullptr;
        if (edit != nullptr && edit->mute)
            continue;
        const double start =
            analysis.frameToSeconds(note.startFrame) + (edit != nullptr ? edit->timeOffsetSeconds : 0.0);
        if (start < startSeconds - toleranceSeconds || start >= endSeconds - toleranceSeconds)
            continue;
        const int pc = effectivePitchClass(note, edit);
        if (motion[static_cast<size_t>(pc)] == kNotAChordTone)
            continue;
        tones.push_back({key, note.medianMidi() + (edit != nullptr ? edit->semitones : 0.0f), pc});
    }

    // An explicit bass: the lowest of them go to it.
    float lowest = std::numeric_limits<float>::max();
    for (const auto& t : tones)
        lowest = std::min(lowest, t.midi);

    for (const auto& t : tones)
    {
        int d = motion[static_cast<size_t>(t.pc)];
        if (to.bass >= 0 && t.midi <= lowest + 0.5f)
            d = circularDelta(t.pc, to.bass);
        if (d == 0)
            continue;
        result.edits[t.index].semitones += static_cast<float>(d);
        ++result.movedNotes;
    }
    // A pitch edit back to nothing is no edit (as Project::setPolyphonicEdits keeps them).
    for (auto it = result.edits.begin(); it != result.edits.end();)
        it = it->second.isIdentity() ? result.edits.erase(it) : std::next(it);
    return result;
}

// ---- names ----

std::string pitchClassName(int pitchClass, bool preferFlats)
{
    static const char* sharps[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* flats[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    const int pc = ((pitchClass % 12) + 12) % 12;
    return preferFlats ? flats[pc] : sharps[pc];
}

std::string circleOfFifthsName(int index)
{
    // F C G D A E B at -1..5; each further 7 steps adds a sharp (or a flat going down).
    static const char letters[7] = {'F', 'C', 'G', 'D', 'A', 'E', 'B'};
    const int shifted = index + 1;
    const int letter = ((shifted % 7) + 7) % 7;
    const int accidentals = (shifted - letter) / 7; // floor division, exact
    std::string name(1, letters[letter]);
    if (accidentals > 0)
        name.append(static_cast<size_t>(std::min(accidentals, 3)), '#');
    else if (accidentals < 0)
        name.append(static_cast<size_t>(std::min(-accidentals, 3)), 'b');
    return name;
}

int circleOfFifthsPitchClass(int index) { return (((index * 7) % 12) + 12) % 12; }

std::string chordSuffix(std::uint16_t intervals)
{
    for (const auto& q : qualities())
        if (q.intervals == intervals)
            return q.suffix;

    // Anything else, by the usual rules: quality, then the highest extension
    // over a seventh (or a sixth), then alterations, added tones, suspensions.
    const auto b = [intervals](int i) { return has(intervals, i); };
    const bool M3 = b(4), m3 = b(3) && !b(4);
    const bool sus4 = !b(3) && !b(4) && b(5), sus2 = !b(3) && !b(4) && !b(5) && b(2);
    const bool P5 = b(7), d5 = b(6) && !b(7), a5 = b(8) && !b(7) && M3;
    const bool dim7 = m3 && d5 && b(9) && !b(10) && !b(11);
    const bool seventh = b(10) || b(11) || dim7;
    const bool halfDim = m3 && d5 && b(10);
    const bool six = !seventh && b(9);

    int ext = seventh ? 7 : 0;
    bool used2 = sus2, used5 = sus4;
    if (seventh && !dim7 && b(2) && !sus2)
    {
        ext = 9;
        used2 = true;
        if (b(5) && !sus4)
        {
            ext = 11;
            used5 = true;
        }
    }
    if (seventh && !dim7 && b(9))
        ext = 13; // a 13th over a seventh (C7 with A is C13)

    std::string s;
    if (dim7)
        s = "dim7";
    else if (halfDim)
        s = ext > 7 ? "m" + std::to_string(ext) + "b5" : "m7b5";
    else if (m3 && d5 && !seventh)
        s = "dim";
    else
    {
        if (m3)
            s = "m";
        else if (a5 && !seventh)
            s = "aug";
        if (b(11))
            s += (m3 ? "Maj" : "maj") + std::to_string(ext);
        else if (b(10))
            s += std::to_string(ext);
        else if (six)
        {
            s += b(2) && !sus2 ? "6/9" : "6";
            used2 = used2 || b(2);
        }
        if (d5 && seventh)
            s += "b5";
        if (a5 && seventh)
            s += "#5";
    }
    if (b(1))
        s += "b9";
    if (b(3) && M3)
        s += "#9";
    if (b(6) && P5)
        s += "#11";
    if (b(8) && P5 && (seventh || m3))
        s += "b13";
    else if (b(8) && P5)
        s += "addb13";
    if (b(2) && !used2)
        s += "add9";
    if (b(5) && !used5)
        s += "add11";
    if (sus4)
        s += "sus4";
    else if (sus2)
        s += "sus2";
    else if (!M3 && !b(3))
        s += "(no3)";
    if (!P5 && !d5 && !b(8) && popcount(intervals) > 2)
        s += "(no5)";
    return s;
}

namespace
{
/** A note name at `text[pos]` (letter, then sharps and flats): its pitch class. */
std::optional<int> readNoteName(const std::string& text, size_t& pos)
{
    if (pos >= text.size())
        return std::nullopt;
    static const int letters[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(text[pos])));
    if (c < 'A' || c > 'G')
        return std::nullopt;
    int pc = letters[c - 'A'];
    ++pos;
    while (pos < text.size() && (text[pos] == '#' || text[pos] == 'b'))
        pc += text[pos++] == '#' ? 1 : -1;
    return ((pc % 12) + 12) % 12;
}

bool startsWith(const std::string& text, size_t pos, const char* prefix)
{
    return text.compare(pos, std::char_traits<char>::length(prefix), prefix) == 0;
}

void replaceAll(std::string& text, const std::string& from, const std::string& to)
{
    for (auto p = text.find(from); p != std::string::npos; p = text.find(from, p + to.size()))
        text.replace(p, from.size(), to);
}
} // namespace

std::optional<Chord> parseChord(const std::string& input)
{
    // Normalise: symbols to letters, no spaces, brackets or commas.
    std::string t;
    for (char c : input)
        if (c != ' ' && c != '\t' && c != '(' && c != ')' && c != ',')
            t += c;
    replaceAll(t, "\xE2\x99\xAD", "b");     // flat sign
    replaceAll(t, "\xE2\x99\xAF", "#");     // sharp sign
    replaceAll(t, "\xC3\xB8" "7", "m7b5");   // half-diminished, o-slash
    replaceAll(t, "\xC3\xB8", "m7b5");
    replaceAll(t, "\xC2\xB0" "7", "dim7");   // degree sign
    replaceAll(t, "\xC2\xB0", "dim");
    for (const char* delta : {"\xCE\x94", "\xE2\x96\xB3"}) // triangle
    {
        replaceAll(t, std::string(delta) + "7", "maj7");
        replaceAll(t, std::string(delta) + "9", "maj9");
        replaceAll(t, std::string(delta) + "11", "maj11");
        replaceAll(t, std::string(delta) + "13", "maj13");
        replaceAll(t, delta, "maj7");
    }
    replaceAll(t, "6/9", "69");
    if (t.empty())
        return std::nullopt;

    size_t pos = 0;
    const auto root = readNoteName(t, pos);
    if (!root)
        return std::nullopt;

    // A slash bass at the end.
    std::optional<int> bass;
    if (const auto slash = t.rfind('/'); slash != std::string::npos && slash >= pos)
    {
        size_t bp = slash + 1;
        bass = readNoteName(t, bp);
        if (!bass || bp != t.size())
            return std::nullopt;
        t.resize(slash);
    }

    // ---- quality ----
    enum class Triad { Major, Minor, Dim, Aug, Power };
    Triad triad = Triad::Major;
    bool majSeventh = false; // maj7 / mMaj7 family
    bool halfDim = false;
    auto takeMaj = [&]()
    {
        for (const char* m : {"maj", "Maj", "MAJ", "ma", "Ma", "M"})
            if (startsWith(t, pos, m) && !(std::string(m) == "ma" && startsWith(t, pos, "mad")))
            {
                pos += std::char_traits<char>::length(m);
                return true;
            }
        return false;
    };
    if (startsWith(t, pos, "m7b5"))
    {
        triad = Triad::Minor;
        halfDim = true;
        pos += 1; // the 7 and b5 are read below
    }
    else if (startsWith(t, pos, "dim") || startsWith(t, pos, "o"))
    {
        triad = Triad::Dim;
        pos += t[pos] == 'o' ? 1 : 3;
    }
    else if (startsWith(t, pos, "aug") || startsWith(t, pos, "+"))
    {
        triad = Triad::Aug;
        pos += t[pos] == '+' ? 1 : 3;
    }
    else if (!startsWith(t, pos, "maj") && !startsWith(t, pos, "Maj") && !startsWith(t, pos, "M")
             && !(startsWith(t, pos, "ma") && !startsWith(t, pos, "mad")) // "ma7" is maj7, "madd9" minor
             && (startsWith(t, pos, "min") || startsWith(t, pos, "mi") || startsWith(t, pos, "m")
                 || startsWith(t, pos, "-")))
    {
        triad = Triad::Minor;
        pos += startsWith(t, pos, "min") ? 3 : startsWith(t, pos, "mi") && !startsWith(t, pos, "mid") ? 2 : 1;
    }
    if (takeMaj())
        majSeventh = true;

    // ---- the number: 5, 6, 69, 7, 9, 11, 13 (2 and 4: add2, sus4) ----
    int number = 0;
    if (pos < t.size() && std::isdigit(static_cast<unsigned char>(t[pos])))
    {
        size_t end = pos;
        while (end < t.size() && std::isdigit(static_cast<unsigned char>(t[end])))
            ++end;
        number = std::stoi(t.substr(pos, end - pos));
        if (number != 2 && number != 4 && number != 5 && number != 6 && number != 69 && number != 7
            && number != 9 && number != 11 && number != 13)
            return std::nullopt;
        pos = end;
    }

    std::uint16_t m = 0;
    auto set = [&](int i) { m = static_cast<std::uint16_t>(m | (1u << i)); };
    auto clear = [&](int i) { m = static_cast<std::uint16_t>(m & ~(1u << i)); };
    switch (triad)
    {
    case Triad::Major: set(0); set(4); set(7); break;
    case Triad::Minor: set(0); set(3); set(7); break;
    case Triad::Dim: set(0); set(3); set(6); break;
    case Triad::Aug: set(0); set(4); set(8); break;
    case Triad::Power: break;
    }
    if (number == 5)
    {
        if (triad != Triad::Major || majSeventh)
            return std::nullopt;
        m = 0;
        set(0);
        set(7);
    }
    else if (number == 2)
        set(2);
    else if (number == 4)
    {
        clear(3); clear(4); set(5);
    }
    else if (number == 6)
        set(9);
    else if (number == 69)
    {
        set(9); set(2);
    }
    else if (number >= 7)
    {
        if (majSeventh)
            set(11);
        else if (triad == Triad::Dim && !halfDim)
            set(9); // diminished seventh
        else
            set(10);
        if (number >= 9)
            set(2);
        if (number >= 11 && (number == 11 || triad == Triad::Minor))
            set(5);
        if (number == 13)
            set(9);
    }
    else if (majSeventh)
    {
        // "Cmaj" or "CM": the major triad; "CmM" alone is not a chord.
        if (triad == Triad::Minor)
            set(11);
    }

    // ---- modifiers ----
    while (pos < t.size())
    {
        if (startsWith(t, pos, "sus"))
        {
            pos += 3;
            int which = 4;
            if (startsWith(t, pos, "2") || startsWith(t, pos, "4"))
                which = t[pos++] - '0';
            clear(3);
            clear(4);
            set(which == 2 ? 2 : 5);
            continue;
        }
        if (startsWith(t, pos, "alt"))
        {
            pos += 3;
            clear(7); clear(2);
            set(1); set(3); set(8);
            continue;
        }
        if (startsWith(t, pos, "no") || startsWith(t, pos, "omit"))
        {
            pos += startsWith(t, pos, "no") ? 2 : 4;
            if (startsWith(t, pos, "3"))
            {
                clear(3); clear(4);
            }
            else if (startsWith(t, pos, "5"))
            {
                clear(6); clear(7); clear(8);
            }
            else
                return std::nullopt;
            ++pos;
            continue;
        }
        const bool add = startsWith(t, pos, "add");
        if (add)
            pos += 3;
        int accidental = 0;
        if (pos < t.size() && (t[pos] == 'b' || t[pos] == '#' || t[pos] == '+' || t[pos] == '-'))
            accidental = t[pos] == 'b' || t[pos] == '-' ? -1 : 1, ++pos;
        if (pos >= t.size() || !std::isdigit(static_cast<unsigned char>(t[pos])))
            return std::nullopt;
        size_t end = pos;
        while (end < t.size() && std::isdigit(static_cast<unsigned char>(t[end])))
            ++end;
        const int degree = std::stoi(t.substr(pos, end - pos));
        pos = end;
        int interval;
        switch (degree)
        {
        case 2: case 9: interval = 2; break;
        case 4: case 11: interval = 5; break;
        case 5: interval = 7; break;
        case 6: case 13: interval = 9; break;
        default: return std::nullopt;
        }
        if (!add && accidental == 0)
            return std::nullopt; // "C7 9": a bare number after the chord
        if (accidental != 0)
        {
            // An altered fifth replaces the fifth; an altered 9, 11 or 13 replaces it too.
            clear(interval);
            if (degree == 5)
                clear(7);
        }
        set((interval + accidental + 12) % 12);
    }

    Chord chord;
    chord.root = *root;
    chord.intervals = static_cast<std::uint16_t>(m | 1u);
    chord.bass = bass ? *bass : chord.root;
    return chord;
}

std::string chordName(const Chord& chord, bool preferFlats)
{
    if (chord.isNone())
        return {};
    std::string name = pitchClassName(chord.root, preferFlats) + chordSuffix(chord.intervals);
    if (chord.bass >= 0 && chord.bass != chord.root)
        name += "/" + pitchClassName(chord.bass, preferFlats);
    return name;
}

bool keyPrefersFlats(const std::vector<ChordNote>& notes)
{
    std::array<double, 12> chroma{};
    for (const auto& n : notes)
        if (std::isfinite(n.midi) && n.endSeconds > n.startSeconds && n.gain > 0.0)
            chroma[static_cast<size_t>(pitchClassOf(n.midi))] += (n.endSeconds - n.startSeconds) * n.gain;
    double best = -2.0;
    int tonic = 0;
    bool major = true;
    for (int k = 0; k < 12; ++k)
    {
        const double a = correlation(chroma, kMajorProfile, k);
        const double b = correlation(chroma, kMinorProfile, k);
        if (a > best)
        {
            best = a;
            tonic = k;
            major = true;
        }
        if (b > best)
        {
            best = b;
            tonic = k;
            major = false;
        }
    }
    // F, Bb, Eb, Ab, Db major; D, G, C, F, Bb, Eb minor
    static const bool flatMajor[12] = {false, true, false, true, false, true, false, false, true, false, true, false};
    static const bool flatMinor[12] = {true, false, true, true, false, true, false, true, false, false, true, false};
    return major ? flatMajor[tonic] : flatMinor[tonic];
}
} // namespace polyphonic
