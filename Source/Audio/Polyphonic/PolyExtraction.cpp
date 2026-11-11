// Polyphonic engine - cents field, harmonic-sum salience, iterative estimation and cancellation of note
// candidates, candidate pruning, f0/inharmonicity refinement, repeated-note splitting and fragment
// merging. Everything here works on pitched (harmonic) energy only.

#include "PolyInternal.h"

#include <limits>
#include <numeric>

namespace polyphonic
{
namespace detail
{

namespace
{
template <typename T>
float medianOf(std::vector<T> v)
{
    if (v.empty())
        return 0.0f;
    const size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2), v.end());
    const float hi = static_cast<float>(v[n / 2]);
    if (n % 2)
        return hi;
    const float lo = static_cast<float>(*std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2)));
    return 0.5f * (lo + hi);
}

// Fill the entries without a value by linear interpolation between known neighbours (held flat
// past the ends). An explicit mask rather than NaN sentinels: pitchnet_core builds with
// -ffast-math, under which NaN checks may be optimised away.
void fillGaps(std::vector<float>& v, const std::vector<uint8_t>& known)
{
    const int n = static_cast<int>(v.size());
    int prev = -1;
    for (int i = 0; i < n; ++i)
    {
        if (!known[static_cast<size_t>(i)])
            continue;
        if (prev < 0)
            for (int j = 0; j < i; ++j)
                v[static_cast<size_t>(j)] = v[static_cast<size_t>(i)];
        else
            for (int j = prev + 1; j < i; ++j)
            {
                const float u = static_cast<float>(j - prev) / static_cast<float>(i - prev);
                v[static_cast<size_t>(j)] = v[static_cast<size_t>(prev)] * (1 - u) + v[static_cast<size_t>(i)] * u;
            }
        prev = i;
    }
    if (prev >= 0)
        for (int j = prev + 1; j < n; ++j)
            v[static_cast<size_t>(j)] = v[static_cast<size_t>(prev)];
}

std::vector<float> medianFilter3(const std::vector<float>& v)
{
    const int n = static_cast<int>(v.size());
    std::vector<float> out(v);
    for (int i = 0; i < n; ++i)
    {
        float a = v[static_cast<size_t>(std::max(0, i - 1))], b = v[static_cast<size_t>(i)], c = v[static_cast<size_t>(std::min(n - 1, i + 1))];
        out[static_cast<size_t>(i)] = std::max(std::min(a, b), std::min(std::max(a, b), c));
    }
    return out;
}
} // namespace

float Candidate::midi() const
{
    if (f0.empty())
        return 0.0f;
    return 69.0f + 12.0f * std::log2(medianOf(f0) / 440.0f);
}

int numCentsBins(const Params& p)
{
    return static_cast<int>(std::ceil(1200.0 * std::log2(p.fTop / p.fRef) / p.centsStep)) + 1;
}

float hzToField(float hz, const Params& p)
{
    return static_cast<float>(1200.0 * std::log2(std::max(hz, 1e-6f) / p.fRef) / p.centsStep);
}

float fieldToHz(float c, const Params& p)
{
    return static_cast<float>(p.fRef * std::pow(2.0, c * p.centsStep / 1200.0));
}

namespace
{
// F(t, f, E) for frames [t0, t1): tonality-weighted, whitened peak amplitudes deposited at their
// instantaneous frequency on a cents grid with a half-tone-wide Hann window, smoothed over time.
Matrix buildField(const Context& ctx, int t0, int t1)
{
    const Params& p = ctx.p;
    const int C = numCentsBins(p);
    const int a = std::max(0, t0 - 1), b = std::min(ctx.numFrames, t1 + 1);
    Matrix E(b - a, C);
    const double binHz = p.sampleRate / p.fftSize;
    const float s = p.spreadCents / p.centsStep;
    const int r = static_cast<int>(std::ceil(s));
    const auto& pk = ctx.peaks;
    for (int t = a; t < b; ++t)
    {
        float* row = E.row(t - a);
        for (int j = pk.begin(t); j < pk.end(t); ++j)
        {
            const float f = static_cast<float>(pk.fbin[static_cast<size_t>(j)] * binHz);
            if (!(f > p.fRef && f < p.fTop))
                continue;
            const float c = hzToField(f, p);
            const float val = pk.wamp[static_cast<size_t>(j)] * pk.tonality[static_cast<size_t>(j)];
            const int base = static_cast<int>(std::floor(c));
            for (int o = -r; o <= r + 1; ++o)
            {
                const int ci = base + o;
                const float d = std::fabs(static_cast<float>(ci) - c);
                if (ci < 0 || ci >= C || d >= s)
                    continue;
                row[ci] += val * (0.5f + 0.5f * std::cos(static_cast<float>(kPi) * d / s));
            }
        }
    }
    Matrix out(t1 - t0, C);
    for (int t = t0; t < t1; ++t)
    {
        const bool smooth = t > 0 && t < ctx.numFrames - 1;
        for (int c = 0; c < C; ++c)
            out.at(t - t0, c) = smooth ? 0.25f * E.at(t - 1 - a, c) + 0.5f * E.at(t - a, c) + 0.25f * E.at(t + 1 - a, c)
                                       : E.at(t - a, c);
    }
    return out;
}
} // namespace

namespace
{
struct RelevanceTable
{
    int c0Lo = 0, c0Hi = 0, C = 0;
    std::vector<int> offsets;              // per harmonic
    std::vector<std::vector<float>> w;     // [harmonic][column]
};

RelevanceTable makeRelevanceTable(const Params& p)
{
    RelevanceTable rt;
    rt.C = numCentsBins(p);
    rt.c0Lo = static_cast<int>(std::floor(hzToField(p.f0Min, p)));
    rt.c0Hi = static_cast<int>(std::ceil(hzToField(p.f0Max, p)));
    const int cols = rt.c0Hi - rt.c0Lo + 1;
    for (int h = 1; h <= p.numHarmonicsRelevance; ++h)
    {
        rt.offsets.push_back(static_cast<int>(std::lround(1200.0 * std::log2(h) / p.centsStep)));
        std::vector<float> w(static_cast<size_t>(cols));
        for (int j = 0; j < cols; ++j)
        {
            const float f0 = fieldToHz(static_cast<float>(rt.c0Lo + j), p);
            w[static_cast<size_t>(j)] = (f0 + p.alpha) / (h * f0 + p.beta);
        }
        rt.w.push_back(std::move(w));
    }
    return rt;
}

void relevanceRows(const RelevanceTable& rt, const Matrix& E, int t0, int t1, Matrix& R)
{
    const int cols = rt.c0Hi - rt.c0Lo + 1;
    for (int t = t0; t < t1; ++t)
    {
        float* out = R.row(t);
        std::fill(out, out + cols, 0.0f);
        const float* e = E.row(t);
        for (size_t h = 0; h < rt.offsets.size(); ++h)
        {
            const int off = rt.offsets[h];
            const float* w = rt.w[h].data();
            const int jMax = std::min(cols, rt.C - rt.c0Lo - off);
            for (int j = 0; j < jMax; ++j)
                out[j] += w[j] * e[rt.c0Lo + j + off];
        }
    }
}
} // namespace

namespace
{
struct Crest
{
    std::vector<int> t;
    std::vector<int> c;                // relevance column
};

Crest trackCrest(const Matrix& R, int t0, int c0, const Params& p)
{
    const float peak = R.at(t0, c0);
    const int jump = static_cast<int>(std::lround(p.crestJumpCents / p.centsStep));
    std::vector<std::pair<int, int>> fwd, bwd;
    for (int dir : {1, -1})
    {
        int c = c0, t = t0 + dir;
        while (t >= 0 && t < R.rows)
        {
            const int lo = std::max(0, c - jump), hi = std::min(R.cols, c + jump + 1);
            const float* row = R.row(t);
            int cn = lo;
            for (int j = lo + 1; j < hi; ++j)
                if (row[j] > row[cn])
                    cn = j;
            if (row[cn] < p.crestRelative * peak)
                break;
            (dir > 0 ? fwd : bwd).push_back({t, cn});
            c = cn;
            t += dir;
        }
    }
    Crest cr;
    for (auto it = bwd.rbegin(); it != bwd.rend(); ++it)
    {
        cr.t.push_back(it->first);
        cr.c.push_back(it->second);
    }
    cr.t.push_back(t0);
    cr.c.push_back(c0);
    for (auto& e : fwd)
    {
        cr.t.push_back(e.first);
        cr.c.push_back(e.second);
    }
    return cr;
}

// Withdraw a fraction G of the note's energy at the fundamental and its harmonics, bounded for
// h >= 2 by a robust spectral-smoothness envelope (median over +-3 harmonics) so that energy of
// coinciding partials of other notes survives. The fundamental is always reduced by G.
double withdraw(Matrix& E, const std::vector<int>& ts, const std::vector<int>& cField, const Params& p)
{
    const int C = E.cols;
    const float s = p.spreadCents / p.centsStep;
    const int rad = static_cast<int>(std::ceil(s));
    const int H = p.numHarmonicsSubtract;
    std::vector<int> offs(static_cast<size_t>(H));
    for (int h = 1; h <= H; ++h)
        offs[static_cast<size_t>(h - 1)] = static_cast<int>(std::lround(1200.0 * std::log2(h) / p.centsStep));
    std::vector<float> kern;
    for (int o = -rad; o <= rad; ++o)
        kern.push_back(std::fabs(static_cast<float>(o)) < s ? 0.5f + 0.5f * std::cos(static_cast<float>(kPi) * o / s) : 0.0f);

    const size_t n = ts.size();
    std::vector<int> ip(n * static_cast<size_t>(H));
    std::vector<float> amt(n * static_cast<size_t>(H));
    std::vector<uint8_t> valid(n * static_cast<size_t>(H));
    std::vector<float> a(static_cast<size_t>(H)), padded(static_cast<size_t>(H) + 6), win(7);
    for (size_t i = 0; i < n; ++i)
    {
        const float* row = E.row(ts[i]);
        for (int h = 0; h < H; ++h)
        {
            const int pos = cField[i] + offs[static_cast<size_t>(h)];
            const bool ok = (pos + rad + 4 < C) && (pos - rad - 4 >= 0);
            const int pc = std::clamp(pos, 3, C - 4);
            int best = pc - 3;
            for (int o = -2; o <= 3; ++o)
                if (row[pc + o] > row[best])
                    best = pc + o;
            const size_t idx = i * static_cast<size_t>(H) + static_cast<size_t>(h);
            ip[idx] = best;
            valid[idx] = ok ? 1 : 0;
            a[static_cast<size_t>(h)] = ok ? row[best] : 0.0f;
        }
        for (int h = 0; h < H + 6; ++h)
            padded[static_cast<size_t>(h)] = a[static_cast<size_t>(std::clamp(h - 3, 0, H - 1))];
        for (int h = 0; h < H; ++h)
        {
            for (int j = 0; j < 7; ++j)
                win[static_cast<size_t>(j)] = padded[static_cast<size_t>(h + j)];
            std::nth_element(win.begin(), win.begin() + 3, win.end());
            const float env = win[3];
            const float av = a[static_cast<size_t>(h)];
            amt[i * static_cast<size_t>(H) + static_cast<size_t>(h)] = p.subtractFraction * (h == 0 ? av : std::min(av, env));
        }
    }
    double total = 0.0;
    std::vector<float> takes(n * static_cast<size_t>(H));
    for (int o = -rad; o <= rad; ++o)
    {
        const float k = kern[static_cast<size_t>(o + rad)];
        if (k <= 0.0f)
            continue;
        for (size_t i = 0; i < n; ++i)
        {
            const float* row = E.row(ts[i]);
            for (int h = 0; h < H; ++h)
            {
                const size_t idx = i * static_cast<size_t>(H) + static_cast<size_t>(h);
                const int ci = std::clamp(ip[idx] + o, 0, C - 1);
                takes[idx] = valid[idx] ? std::min(row[ci], amt[idx] * k) : 0.0f;
            }
        }
        for (size_t i = 0; i < n; ++i)
        {
            float* row = E.row(ts[i]);
            for (int h = 0; h < H; ++h)
            {
                const size_t idx = i * static_cast<size_t>(H) + static_cast<size_t>(h);
                if (takes[idx] <= 0.0f)
                    continue;
                const int ci = std::clamp(ip[idx] + o, 0, C - 1);
                row[ci] -= takes[idx];
                total += takes[idx];
            }
        }
    }
    for (size_t i = 0; i < n; ++i)
    {
        float* row = E.row(ts[i]);
        for (int c = 0; c < C; ++c)
            row[c] = std::max(row[c], 0.0f);
    }
    return total;
}

void mergeOrAdd(std::vector<Candidate>& notes, const std::vector<int>& ts, const std::vector<int>& cs,
                const std::vector<float>& strength, double energy, const Params& p)
{
    const float jump = p.crestJumpCents / p.centsStep;
    const int a = ts.front(), b = ts.back();
    for (auto& n : notes)
    {
        if (a > n.end + p.mergeGapFrames || b < n.start - p.mergeGapFrames)
            continue;
        const int ovLo = std::max(a, n.start), ovHi = std::min(b, n.end);
        float d;
        if (ovLo <= ovHi)
        {
            std::vector<float> diffs;
            for (size_t i = 0; i < ts.size(); ++i)
                if (ts[i] >= ovLo && ts[i] <= ovHi)
                    diffs.push_back(std::fabs(static_cast<float>(cs[i]) - n.cents[static_cast<size_t>(ts[i] - n.start)]));
            d = medianOf(diffs);
        }
        else if (a > n.end)
            d = std::fabs(static_cast<float>(cs.front()) - n.cents.back());
        else
            d = std::fabs(static_cast<float>(cs.back()) - n.cents.front());
        if (d > jump)
            continue;
        const int lo = std::min(n.start, a), hi = std::max(n.end, b);
        std::vector<float> cents(static_cast<size_t>(hi - lo + 1), 0.0f);
        std::vector<uint8_t> known(cents.size(), 0);
        std::vector<float> stren(static_cast<size_t>(hi - lo + 1), 0.0f);
        for (int t = n.start; t <= n.end; ++t)
        {
            cents[static_cast<size_t>(t - lo)] = n.cents[static_cast<size_t>(t - n.start)];
            known[static_cast<size_t>(t - lo)] = 1;
            stren[static_cast<size_t>(t - lo)] = n.strength[static_cast<size_t>(t - n.start)];
        }
        for (size_t i = 0; i < ts.size(); ++i)
        {
            const size_t k = static_cast<size_t>(ts[i] - lo);
            if (!known[k] || strength[i] > stren[k])
                cents[k] = static_cast<float>(cs[i]);
            known[k] = 1;
            stren[k] += strength[i];
        }
        fillGaps(cents, known);
        n.start = lo;
        n.end = hi;
        n.cents = std::move(cents);
        n.strength = std::move(stren);
        n.energy += energy;
        n.hits += 1;
        return;
    }
    Candidate c;
    c.start = a;
    c.end = b;
    for (int v : cs)
        c.cents.push_back(static_cast<float>(v));
    c.strength = strength;
    c.energy = energy;
    notes.push_back(std::move(c));
}
} // namespace

namespace
{
// Iterative estimation and cancellation over frames [lo, hi). Crests whose maximum lies in the core
// [coreLo, coreHi) become (or merge into) note candidates; crests found in the margins are only
// withdrawn - the neighbouring block owns them.
void extractBlock(Context& ctx, int lo, int hi, int coreLo, int coreHi, std::vector<Candidate>& notes,
                  const std::atomic<bool>* cancel)
{
    const Params& p = ctx.p;
    Matrix E = buildField(ctx, lo, hi);
    const auto rt = makeRelevanceTable(p);
    const int T = E.rows, cols = rt.c0Hi - rt.c0Lo + 1;
    Matrix R(T, cols);
    relevanceRows(rt, E, 0, T, R);
    // Guide all pitch hypotheses before explaining away shared harmonic energy.
    // Keep unweighted strengths/r0max in the learned feature schema.
    const bool guided = ctx.evidence != nullptr && !ctx.evidence->empty();
    Matrix guide(guided ? T : 0, guided ? cols : 0, 1.0f);
    constexpr double guideStrength = 0.75;
    if (guided)
    {
        const auto& ev = *ctx.evidence;
        for (int t = 0; t < T; ++t)
        {
            const float sec = static_cast<float>((lo + t) * p.hop / p.sampleRate);
            auto it = std::lower_bound(ev.frameTimes.begin(), ev.frameTimes.end(), sec);
            const int ef = static_cast<int>(std::min<std::ptrdiff_t>(it - ev.frameTimes.begin(), ev.frameTimes.size() - 1));
            for (int c = 0; c < cols; ++c)
            {
                const float m = 69.0f + 12.0f * std::log2(fieldToHz(static_cast<float>(rt.c0Lo + c), p) / 440.0f) - 21.0f;
                if (m < 0.0f || m > 87.0f)
                    continue;
                const int a = static_cast<int>(m), b = std::min(87, a + 1);
                const float u = m - a;
                const float q = ev.note[static_cast<size_t>(ef) * 88 + a] * (1 - u) + ev.note[static_cast<size_t>(ef) * 88 + b] * u;
                guide.at(t, c) = static_cast<float>((1 - guideStrength) + guideStrength * std::sqrt(std::max(0.0f, q)));
            }
        }
    }
    auto applyGuide = [&](int a, int b) {
        if (!guided)
            return;
        for (int t = a; t < b; ++t)
            for (int c = 0; c < cols; ++c)
                R.at(t, c) *= guide.at(t, c);
    };
    for (int t = 0; t < T; ++t)
        if (guided && lo + t >= coreLo && lo + t < coreHi)
            ctx.r0max[static_cast<size_t>(lo + t)] = *std::max_element(R.row(t), R.row(t) + cols);
    applyGuide(0, T);
    std::vector<uint8_t> supp(static_cast<size_t>(T) * cols, 0);
    std::vector<float> rowMax(static_cast<size_t>(T));
    std::vector<int> rowArg(static_cast<size_t>(T));
    auto updateRow = [&](int t) {
        const float* r = R.row(t);
        int best = 0;
        for (int j = 1; j < cols; ++j)
            if (r[j] > r[best])
                best = j;
        rowMax[static_cast<size_t>(t)] = r[best];
        rowArg[static_cast<size_t>(t)] = best;
    };
    for (int t = 0; t < T; ++t)
    {
        updateRow(t);
        if (!guided && lo + t >= coreLo && lo + t < coreHi)
            ctx.r0max[static_cast<size_t>(lo + t)] = rowMax[static_cast<size_t>(t)];
    }
    double eTotal = 0.0;
    for (float v : E.data)
        eTotal += v;
    const double e0 = eTotal;
    float first = -1.0f;
    for (int it = 0; it < p.maxIterations; ++it)
    {
        if (cancel && cancel->load())
            return;
        const int t0 = static_cast<int>(std::max_element(rowMax.begin(), rowMax.end()) - rowMax.begin());
        const int c0 = rowArg[static_cast<size_t>(t0)];
        const float peak = R.at(t0, c0);
        if (first < 0.0f)
            first = peak;
        if (peak <= 0.0f || peak < p.stopRelativePeak * first || eTotal < p.stopEnergy * e0)
            break;
        const Crest cr = trackCrest(R, t0, c0, p);
        std::vector<int> cField(cr.c.size());
        std::vector<float> strength(cr.c.size());
        for (size_t i = 0; i < cr.c.size(); ++i)
        {
            cField[i] = cr.c[i] + rt.c0Lo;
            strength[i] = guided ? R.at(cr.t[i], cr.c[i]) / guide.at(cr.t[i], cr.c[i]) : R.at(cr.t[i], cr.c[i]);
        }
        const double took = withdraw(E, cr.t, cField, p);
        eTotal -= took;
        const int a = std::max(0, cr.t.front() - 1), b = std::min(T, cr.t.back() + 2);
        relevanceRows(rt, E, a, b, R);
        applyGuide(a, b);
        if (R.at(t0, c0) > 0.9f * peak)
            for (size_t i = 0; i < cr.t.size(); ++i)
                for (int d = -1; d <= 1; ++d)
                    supp[static_cast<size_t>(cr.t[i]) * cols + static_cast<size_t>(std::clamp(cr.c[i] + d, 0, cols - 1))] = 1;
        for (int t = a; t < b; ++t)
        {
            float* r = R.row(t);
            const uint8_t* sp = supp.data() + static_cast<size_t>(t) * cols;
            for (int j = 0; j < cols; ++j)
                if (sp[j])
                    r[j] = 0.0f;
            updateRow(t);
        }
        if (lo + t0 < coreLo || lo + t0 >= coreHi)
            continue;
        std::vector<int> tg(cr.t);
        for (auto& t : tg)
            t += lo;
        mergeOrAdd(notes, tg, cField, strength, took, p);
    }
}
} // namespace

// Long recordings are processed in overlapping blocks so that memory stays bounded; a recording
// shorter than one block is processed in one piece (a single global iteration).
std::vector<Candidate> extractCandidates(Context& ctx, const std::atomic<bool>* cancel)
{
    const Params& p = ctx.p;
    const int T = ctx.numFrames;
    ctx.r0max.assign(static_cast<size_t>(T), 0.0f);
    const int core = std::max(64, static_cast<int>(std::lround(15.0 * p.sampleRate / p.hop)));
    const int margin = static_cast<int>(std::lround(2.5 * p.sampleRate / p.hop));
    std::vector<Candidate> notes;
    for (int start = 0; start < T; start += core)
    {
        int end = std::min(T, start + core);
        if (T - end < margin)
            end = T;
        extractBlock(ctx, std::max(0, start - margin), std::min(T, end + margin), start, end, notes, cancel);
        if (end == T || (cancel && cancel->load()))
            break;
    }
    return notes;
}

void pruneCandidates(const Context& ctx, std::vector<Candidate>& cands)
{
    const Params& p = ctx.p;
    const int minLen = static_cast<int>(std::lround(p.minNoteSeconds * p.sampleRate / p.hop));
    const auto& r0max = ctx.r0max;
    std::vector<Candidate> keep;
    for (auto& n : cands)
    {
        if (n.length() < minLen)
            continue;
        float ref = 0.0f;
        for (int t = n.start; t <= n.end; ++t)
            ref = std::max(ref, r0max[static_cast<size_t>(t)]);
        const float smax = *std::max_element(n.strength.begin(), n.strength.end());
        if (smax < p.minRelativeStrength * ref)
            continue;
        keep.push_back(std::move(n));
    }
    std::stable_sort(keep.begin(), keep.end(), [](const Candidate& x, const Candidate& y) {
        if (x.start != y.start)
            return x.start < y.start;
        return medianOf(x.cents) < medianOf(y.cents);
    });
    cands = std::move(keep);
}

void refineCandidates(const Context& ctx, std::vector<Candidate>& cands)
{
    const Params& p = ctx.p;
    const auto& pk = ctx.peaks;
    const float binHz = static_cast<float>(p.sampleRate / p.fftSize);
    for (auto& n : cands)
    {
        const int T = n.length();
        std::vector<float> f0c(static_cast<size_t>(T));
        for (int i = 0; i < T; ++i)
            f0c[static_cast<size_t>(i)] = fieldToHz(n.cents[static_cast<size_t>(i)], p);
        if (T >= 5)
        {
            std::vector<float> lg(static_cast<size_t>(T));
            for (int i = 0; i < T; ++i)
                lg[static_cast<size_t>(i)] = std::log(f0c[static_cast<size_t>(i)]);
            for (int i = 0; i < T; ++i)
            {
                float s = 0.0f;
                for (int d = -2; d <= 2; ++d)
                    s += lg[static_cast<size_t>(std::clamp(i + d, 0, T - 1))];
                f0c[static_cast<size_t>(i)] = std::exp(s / 5.0f);
            }
        }
        std::vector<float> f0r(f0c);
        std::vector<int> hh, tt;
        std::vector<float> rr, ww;
        std::vector<float> ests, wts;
        for (int i = 0; i < T; ++i)
        {
            const int t = n.start + i;
            if (pk.begin(t) == pk.end(t))
                continue;
            ests.clear();
            wts.clear();
            for (int h = 1; h <= 12; ++h)
            {
                const float fh = h * f0c[static_cast<size_t>(i)];
                if (fh > p.sampleRate / 2 - 200)
                    break;
                const float tol = h <= 6 ? std::max(0.035f * fh, 1.5f * binHz) : 0.05f * fh;
                const int j = pk.nearest(t, fh / binHz);
                const float fp = pk.fbin[static_cast<size_t>(j)] * binHz;
                if (std::fabs(fp - fh) < tol)
                {
                    hh.push_back(h);
                    rr.push_back(fp / h);
                    ww.push_back(pk.amp[static_cast<size_t>(j)]);
                    tt.push_back(i);
                    if (h <= 6)
                    {
                        ests.push_back(fp / h);
                        wts.push_back(pk.amp[static_cast<size_t>(j)] * pk.tonality[static_cast<size_t>(j)]);
                    }
                }
            }
            if (!ests.empty())
            {
                const float med = medianOf(ests);
                double sw = 0.0, s = 0.0;
                for (size_t q = 0; q < ests.size(); ++q)
                    if (std::fabs(ests[q] - med) < 0.01f * med)
                    {
                        s += ests[q] * wts[q];
                        sw += wts[q];
                    }
                if (sw > 0.0)
                    f0r[static_cast<size_t>(i)] = static_cast<float>(s / sw);
            }
        }
        float B = 0.0f;
        if (hh.size() > 10)
        {
            double num = 0.0, den = 0.0;
            int count = 0;
            for (size_t q = 0; q < hh.size(); ++q)
            {
                if (hh[q] < 2)
                    continue;
                const double ratio = rr[q] / f0r[static_cast<size_t>(tt[q])];
                const double y = ratio * ratio - 1.0, x = static_cast<double>(hh[q]) * hh[q];
                num += ww[q] * x * y;
                den += ww[q] * x * x;
                ++count;
            }
            if (count > 5 && den > 0.0)
                B = static_cast<float>(std::clamp(num / den, 0.0, 0.002));
        }
        if (T >= 3)
            f0r = medianFilter3(f0r);
        n.f0 = std::move(f0r);
        n.B = B;
    }
}

std::vector<float> harmonicEnvelope(const Context& ctx, const Candidate& n, int numHarmonics, int t0, int t1)
{
    const Params& p = ctx.p;
    const auto& pk = ctx.peaks;
    const float binHz = static_cast<float>(p.sampleRate / p.fftSize);
    std::vector<float> env(static_cast<size_t>(std::max(0, t1 - t0 + 1)), 0.0f);
    for (int t = std::max(0, t0); t <= std::min(ctx.numFrames - 1, t1); ++t)
    {
        if (pk.begin(t) == pk.end(t))
            continue;
        const float f0 = n.f0At(t);
        double s = 0.0;
        for (int h = 1; h <= numHarmonics; ++h)
        {
            const float fh = static_cast<float>(f0 * h * std::sqrt(1.0 + n.B * h * h));
            const float tol = std::min(std::max(1.2f * binHz, 0.012f * fh), 0.4f * f0);
            const int j = pk.nearest(t, fh / binHz);
            if (std::fabs(pk.fbin[static_cast<size_t>(j)] * binHz - fh) < tol)
                s += static_cast<double>(pk.amp[static_cast<size_t>(j)]) * pk.amp[static_cast<size_t>(j)];
        }
        env[static_cast<size_t>(t - t0)] = static_cast<float>(std::sqrt(s));
    }
    return env;
}

bool isReattack(const std::vector<float>& env, int i, float ratio, float floorRel, float envMax)
{
    const int n = static_cast<int>(env.size());
    if (i < 2 || i + 1 >= n)
        return false;
    float before = 1e30f, after = 0.0f, mx = 0.0f;
    double meanBefore = 0.0, meanAfter = 0.0;
    int nb = 0, na = 0;
    for (int j = std::max(0, i - 4); j < i; ++j)
        before = std::min(before, env[static_cast<size_t>(j)]);
    for (int j = i; j < std::min(n, i + 3); ++j)
        after = std::max(after, env[static_cast<size_t>(j)]);
    for (int j = std::max(0, i - 6); j < i; ++j, ++nb)
        meanBefore += env[static_cast<size_t>(j)];
    for (int j = i; j < std::min(n, i + 4); ++j, ++na)
        meanAfter += env[static_cast<size_t>(j)];
    meanBefore /= std::max(1, nb);
    meanAfter /= std::max(1, na);
    if (envMax >= 0.0f)
        mx = envMax;
    else
        for (float v : env)
            mx = std::max(mx, v);
    // a real attack lifts the level for several frames, not just one amplitude-modulation peak
    return after >= ratio * before + 1e-12f && meanAfter >= 0.75 * ratio * meanBefore && after >= floorRel * mx;
}

// Amplitude of each of the first `numHarmonics` partials of a candidate, per frame in [t0, t1]
// (0 where no spectral peak lies close enough to the partial).
std::vector<std::vector<float>> partialEnvelopes(const Context& ctx, const Candidate& n, int numHarmonics, int t0, int t1)
{
    const Params& p = ctx.p;
    const auto& pk = ctx.peaks;
    const float binHz = static_cast<float>(p.sampleRate / p.fftSize);
    std::vector<std::vector<float>> env(static_cast<size_t>(numHarmonics), std::vector<float>(static_cast<size_t>(std::max(0, t1 - t0 + 1)), 0.0f));
    for (int t = std::max(0, t0); t <= std::min(ctx.numFrames - 1, t1); ++t)
    {
        if (pk.begin(t) == pk.end(t))
            continue;
        const float f0 = n.f0At(t);
        for (int h = 1; h <= numHarmonics; ++h)
        {
            const float fh = static_cast<float>(f0 * h * std::sqrt(1.0 + n.B * h * h));
            const float tol = std::min(std::max(1.2f * binHz, 0.012f * fh), 0.4f * f0);
            const int j = pk.nearest(t, fh / binHz);
            if (std::fabs(pk.fbin[static_cast<size_t>(j)] * binHz - fh) < tol)
                env[static_cast<size_t>(h - 1)][static_cast<size_t>(t - t0)] = pk.amp[static_cast<size_t>(j)];
        }
    }
    return env;
}

bool partialsRiseTogether(const std::vector<std::vector<float>>& partials, int i, float minShare)
{
    if (partials.empty())
        return false;
    const int n = static_cast<int>(partials.front().size());
    std::vector<double> before(partials.size(), 0.0), after(partials.size(), 0.0);
    double strongest = 0.0;
    for (size_t h = 0; h < partials.size(); ++h)
    {
        int nb = 0, na = 0;
        for (int j = std::max(0, i - 6); j < i; ++j, ++nb)
            before[h] += partials[h][static_cast<size_t>(j)];
        for (int j = i; j < std::min(n, i + 4); ++j, ++na)
            after[h] += partials[h][static_cast<size_t>(j)];
        before[h] /= std::max(1, nb);
        after[h] /= std::max(1, na);
        strongest = std::max(strongest, after[h]);
    }
    int present = 0, rising = 0;
    for (size_t h = 0; h < partials.size(); ++h)
    {
        if (!(after[h] > 0.06 * strongest))             // within ~24 dB of the strongest partial
            continue;
        ++present;
        rising += after[h] >= 1.5 * before[h] ? 1 : 0;
    }
    return present > 0 && rising >= minShare * present;
}

namespace
{
// Repeated-note evidence along a candidate, from its own partials only. A repeated note shows up as
// their energy rising again, all of them together, by much more than the note's own amplitude
// modulation (beating, vibrato, bow pressure) moves it, and staying up. Another note entering on
// one shared partial lifts only some of the partials.
struct ReattackTest
{
    std::vector<float> env, rise, peakSoFar;
    std::vector<uint8_t> valid;
    std::vector<std::vector<float>> partials;
    float envMax = 0.0f;
    int len = 0;

    ReattackTest(const Context& ctx, const Candidate& n)
        : env(harmonicEnvelope(ctx, n, 8, n.start, n.end)), partials(partialEnvelopes(ctx, n, 8, n.start, n.end))
    {
        len = static_cast<int>(env.size());
        // local rise: level of the next 4 frames over the previous 6, in log units
        rise.assign(static_cast<size_t>(len), 0.0f);
        valid.assign(static_cast<size_t>(len), 0);
        for (int i = 3; i + 3 <= len; ++i)
        {
            rise[static_cast<size_t>(i)] = static_cast<float>(std::log((meanOf(i, i + 3) + 1e-12) / (meanOf(i - 6, i - 1) + 1e-12)));
            valid[static_cast<size_t>(i)] = 1;
        }
        // running maximum, for "the note had been louder before it dipped"
        peakSoFar.assign(static_cast<size_t>(len), 0.0f);
        for (int i = 0; i < len; ++i)
            peakSoFar[static_cast<size_t>(i)] = std::max(i > 0 ? peakSoFar[static_cast<size_t>(i - 1)] : 0.0f, env[static_cast<size_t>(i)]);
        envMax = len > 0 ? peakSoFar.back() : 0.0f;
    }

    double meanOf(int a, int b) const        // mean of env over [a, b] clipped to the candidate
    {
        a = std::max(a, 0);
        b = std::min(b, len - 1);
        double s = 0.0;
        for (int j = a; j <= b; ++j)
            s += env[static_cast<size_t>(j)];
        return b >= a ? s / (b - a + 1) : 0.0;
    }

    bool standsOut(int i) const
    {
        std::vector<float> around;
        for (int j = i - 18; j <= i + 18; ++j)
            if (std::abs(j - i) > 6 && j >= 0 && j < len && valid[static_cast<size_t>(j)])
                around.push_back(std::fabs(rise[static_cast<size_t>(j)]));
        float typical = 0.0f;
        if (!around.empty())
        {
            std::nth_element(around.begin(), around.begin() + static_cast<std::ptrdiff_t>(around.size() / 2), around.end());
            typical = around[around.size() / 2];
        }
        return rise[static_cast<size_t>(i)] >= kReattackStandOut * typical;
    }

    bool dipped(int i) const { return i >= 4 && meanOf(i - 6, i - 1) <= kReattackDip * peakSoFar[static_cast<size_t>(i - 4)]; }

    // the level rises after a dip and stays up
    bool risesAndStays(int i) const
    {
        return isReattack(env, i, 2.0f, 0.1f, envMax) && dipped(i) && meanOf(i + 1, i + 12) >= 1.5 * meanOf(i - 12, i - 1);
    }

    // ... and by more than the note's own modulation, with its partials together: a repeated note
    bool qualifies(int i) const { return risesAndStays(i) && standsOut(i) && partialsRiseTogether(partials, i, kReattackShare); }
};
} // namespace

std::vector<Candidate> splitReattacks(const Context& ctx, const std::vector<Candidate>& cands)
{
    std::vector<Candidate> out;
    for (const auto& n : cands)
    {
        const ReattackTest test(ctx, n);
        const int len = test.len;
        std::vector<int> cuts;
        for (int i = 4; i <= len - 5;)
        {
            if (!test.qualifies(i))
            {
                ++i;
                continue;
            }
            // within a run of frames that qualify, cut where the level steps up most (the next 4
            // frames over the previous 6, which peaks at the onset); cuts stay 6 frames (~70 ms) apart
            int best = i, j = i;
            for (; j <= len - 5 && test.qualifies(j); ++j)
                if (test.rise[static_cast<size_t>(j)] > test.rise[static_cast<size_t>(best)])
                    best = j;
            if (cuts.empty() || n.start + best - cuts.back() >= 6)
                cuts.push_back(n.start + best);
            i = j + 1;
        }
        if (cuts.empty())
        {
            out.push_back(n);
            continue;
        }
        std::vector<int> bounds{n.start};
        bounds.insert(bounds.end(), cuts.begin(), cuts.end());
        bounds.push_back(n.end + 1);
        for (size_t q = 0; q + 1 < bounds.size(); ++q)
        {
            const int i0 = bounds[q] - n.start, i1 = bounds[q + 1] - n.start;
            Candidate m;
            m.start = bounds[q];
            m.end = bounds[q + 1] - 1;
            m.cents.assign(n.cents.begin() + i0, n.cents.begin() + i1);
            m.strength.assign(n.strength.begin() + i0, n.strength.begin() + i1);
            m.f0.assign(n.f0.begin() + i0, n.f0.begin() + i1);
            m.energy = n.energy * (i1 - i0) / static_cast<double>(n.cents.size());
            m.hits = n.hits;
            m.B = n.B;
            out.push_back(std::move(m));
        }
    }
    const int minLen = static_cast<int>(std::lround(ctx.p.minNoteSeconds * ctx.p.sampleRate / ctx.p.hop));
    out.erase(std::remove_if(out.begin(), out.end(), [&](const Candidate& c) { return c.length() < minLen; }), out.end());
    return out;
}

// A note whose pitch coincides with a partial of an earlier note tends to be tracked back into that
// partial. Move its start to where its own (unshared) partials actually begin to rise, when that is at
// least one analysis window later: closer than that, the frames still see the note's own attack.
void refineOnsets(const Context& ctx, std::vector<Candidate>& notes)
{
    const Params& p = ctx.p;
    const int maxLook = static_cast<int>(std::lround(0.6 * p.sampleRate / p.hop));
    const int minLen = static_cast<int>(std::lround(p.minNoteSeconds * p.sampleRate / p.hop));
    const auto& pk = ctx.peaks;
    const float binHz = static_cast<float>(p.sampleRate / p.fftSize);
    for (size_t ni = 0; ni < notes.size(); ++ni)
    {
        auto& n = notes[ni];
        const int t1 = std::min(n.end - minLen, n.start + maxLook);
        if (t1 <= n.start + 2)
            continue;
        // harmonics of n that coincide with a partial of another note sounding in this window
        std::vector<int> unique;
        for (int h = 1; h <= 8; ++h)
        {
            bool shared = false;
            for (size_t mi = 0; mi < notes.size() && !shared; ++mi)
            {
                const auto& m = notes[mi];
                if (mi == ni || m.end < n.start || m.start > t1)
                    continue;
                const float fh = h * n.f0At(n.start + 2);
                const float fm = m.f0At(std::max(m.start, n.start));
                for (int k = 1; k <= 24 && !shared; ++k)
                    shared = std::fabs(fh - k * fm * std::sqrt(1.0f + m.B * k * k)) < 0.015f * fh;
            }
            if (!shared)
                unique.push_back(h);
        }
        if (unique.empty())
            continue;
        // level of the given partials of n at frame t
        auto partialsLevel = [&](int t, const std::vector<int>& harmonics) {
            if (t >= ctx.numFrames || pk.begin(t) == pk.end(t))
                return 0.0f;
            double s = 0.0;
            for (int h : harmonics)
            {
                const float fh = static_cast<float>(n.f0At(t) * h * std::sqrt(1.0 + n.B * h * h));
                const int j = pk.nearest(t, fh / binHz);
                if (std::fabs(pk.fbin[static_cast<size_t>(j)] * binHz - fh) < std::max(1.2f * binHz, 0.012f * fh))
                    s += static_cast<double>(pk.amp[static_cast<size_t>(j)]) * pk.amp[static_cast<size_t>(j)];
            }
            return static_cast<float>(std::sqrt(s));
        };
        std::vector<float> env(static_cast<size_t>(t1 - n.start + 4), 0.0f);
        for (int t = n.start; t < n.start + static_cast<int>(env.size()); ++t)
            env[static_cast<size_t>(t - n.start)] = partialsLevel(t, unique);
        // steepest rise of the unshared partials (works for closely spaced onsets too)
        int bestI = -1;
        double bestRise = 0.0;
        float envMax = 0.0f;
        for (float v : env)
            envMax = std::max(envMax, v);
        for (int i = 3; i <= t1 - n.start; ++i)
        {
            double before = 0.0, after = 0.0;
            int nb = 0, na = 0;
            for (int j = std::max(0, i - 6); j < i; ++j, ++nb)
                before += env[static_cast<size_t>(j)];
            for (int j = i; j < std::min(static_cast<int>(env.size()), i + 4); ++j, ++na)
                after += env[static_cast<size_t>(j)];
            before /= std::max(1, nb);
            after /= std::max(1, na);
            if (after >= 3.0 * before && after >= 0.25 * envMax && after - before > bestRise)
            {
                bestRise = after - before;
                bestI = i;
            }
        }
        if (bestI > 0)
        {
            // everything before the rise must be quiet compared with the note itself
            double pre = 0.0;
            for (int j = 0; j < bestI; ++j)
                pre += env[static_cast<size_t>(j)];
            pre /= bestI;
            if (pre < 0.3 * bestRise)
            {
                // the onset is where the rise begins, not its steepest point (soft attacks ramp up)
                const double level = bestRise;
                while (bestI > 0 && env[static_cast<size_t>(bestI - 1)] < env[static_cast<size_t>(bestI)] &&
                       env[static_cast<size_t>(bestI - 1)] > 0.1 * level)
                    --bestI;
            }
            // A rise within one analysis window of the start is that window still sliding over the
            // note's own attack, not an onset further on.
            const int window = std::max(3, p.fftSize / p.hop);
            bool move = pre < 0.3 * bestRise && bestI >= window;
            std::vector<float> whole;   // the note's whole level (its first eight partials), when needed
            auto wholeLevel = [&]() -> const std::vector<float>& {
                static const std::vector<int> all{ 1, 2, 3, 4, 5, 6, 7, 8 };
                if (whole.empty())
                    for (int j = 0; j < static_cast<int>(env.size()); ++j)
                        whole.push_back(partialsLevel(n.start + j, all));
                return whole;
            };
            // with its lower partials all shared, the rise is seen only in weak upper ones, where noise and
            // other notes can fake it: then the note's whole level must rise there too
            if (move && unique.front() > 4)
            {
                const auto& w = wholeLevel();
                const int to = std::min(static_cast<int>(w.size()), bestI + 4);
                double before = 0.0, after = 0.0;
                for (int j = 0; j < bestI; ++j)
                    before += w[static_cast<size_t>(j)];
                for (int j = bestI; j < to; ++j)
                    after += w[static_cast<size_t>(j)];
                move = after / std::max(1, to - bestI) >= 1.5 * before / bestI;
            }
            // if nothing sounds at the note's partials before the rise, no other note's partial held its
            // start back: that is a slow attack, unless the rise comes well over a window later
            if (move && bestI < window + window / 4)
            {
                const auto& w = wholeLevel();
                double before = 0.0, peak = 0.0;
                for (int j = 0; j < static_cast<int>(w.size()); ++j)
                {
                    if (j < bestI)
                        before += w[static_cast<size_t>(j)];
                    peak = std::max(peak, static_cast<double>(w[static_cast<size_t>(j)]));
                }
                move = before / bestI >= 0.05 * peak;
            }
            if (move)
            {
                n.start += bestI;
                n.cents.erase(n.cents.begin(), n.cents.begin() + bestI);
                n.strength.erase(n.strength.begin(), n.strength.begin() + bestI);
                n.f0.erase(n.f0.begin(), n.f0.begin() + bestI);
            }
        }
    }
}

// Notes of one pitch that overlap or nearly touch form a lane, which becomes one note per stroke.
// A note starting where the lane's sound so far ends starts a new note if its level rises again
// there. One starting inside the lane's sound starts a new note only if its level rises and stays
// up (as a repeated note's does, unlike a swell from beating or vibrato); if its level only rises
// but it goes on past the lane's sound so far, it begins where that sound ends. Otherwise it is the
// same note found twice and is merged in. A resulting note follows the member it starts with where
// that member's pitch stays within 25 cents of the lane's (the member nearest the lane's typical
// pitch at each frame), and is that member as it was if it matches it. With `overlapsOnly`, notes
// that do not overlap are left apart.
std::vector<Candidate> mergeFragments(const Context& ctx, std::vector<Candidate> notes, bool overlapsOnly)
{
    const int maxGap = 4;
    const float maxCents = 40.0f;
    const int minApart = 12;   // a start inside a note within ~140 ms of the last one is that same attack
    const int minLen = static_cast<int>(std::lround(ctx.p.minNoteSeconds * ctx.p.sampleRate / ctx.p.hop));
    std::stable_sort(notes.begin(), notes.end(), [](const Candidate& a, const Candidate& b) { return a.start < b.start; });
    struct Lane
    {
        std::vector<size_t> members;           // by start
        int start = 0, end = 0;
    };
    std::vector<Lane> lanes;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& c = notes[i];
        const float hz = medianOf(c.f0);
        // the lane it overlaps most, among those with a member of its pitch that it touches
        int best = -1, bestOverlap = std::numeric_limits<int>::min();
        for (size_t l = 0; l < lanes.size(); ++l)
        {
            const auto& lane = lanes[l];
            if (c.start > lane.end + maxGap || c.end < lane.start - maxGap)
                continue;
            const bool near = std::any_of(lane.members.begin(), lane.members.end(), [&](size_t k) {
                const auto& m = notes[k];
                return c.start <= m.end + maxGap && c.end >= m.start - maxGap &&
                       std::fabs(1200.0f * std::log2(hz / medianOf(m.f0))) <= maxCents;
            });
            const int overlap = std::min(c.end, lane.end) - std::max(c.start, lane.start);
            if (near && overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = static_cast<int>(l);
            }
        }
        if (best < 0)
        {
            lanes.push_back({ { i }, c.start, c.end });
            continue;
        }
        auto& lane = lanes[static_cast<size_t>(best)];
        lane.members.push_back(i);
        lane.end = std::max(lane.end, c.end);
    }
    std::vector<Candidate> out;
    for (const auto& lane : lanes)
    {
        if (lane.members.size() == 1)
        {
            out.push_back(std::move(notes[lane.members.front()]));
            continue;
        }
        // the lane as one candidate: at each frame the pitch of the member nearest the lane's typical
        // pitch there (a member can wander onto a neighbouring note's partials)
        std::vector<float> pooled;
        for (size_t k : lane.members)
            for (float hz : notes[k].f0)
                pooled.push_back(hz);
        const float typical = medianOf(pooled);
        Candidate whole;
        whole.start = lane.start;
        whole.end = lane.end;
        whole.f0.assign(static_cast<size_t>(lane.end - lane.start + 1), 0.0f);
        std::vector<float> off(whole.f0.size(), std::numeric_limits<float>::max());
        std::vector<uint8_t> known(whole.f0.size(), 0);
        for (size_t k : lane.members)
        {
            const auto& m = notes[k];
            for (int t = m.start; t <= m.end; ++t)
            {
                const size_t q = static_cast<size_t>(t - lane.start);
                const float hz = m.f0[static_cast<size_t>(t - m.start)];
                const float d = std::fabs(std::log2(hz / typical));
                if (d < off[q])
                {
                    off[q] = d;
                    whole.f0[q] = hz;
                    known[q] = 1;
                }
            }
            whole.B = std::max(whole.B, m.B);
        }
        fillGaps(whole.f0, known);
        std::unique_ptr<ReattackTest> test;
        std::vector<int> bounds{ lane.start };
        int cover = notes[lane.members.front()].end;
        for (size_t k = 1; k < lane.members.size(); ++k)
        {
            const auto& c = notes[lane.members[k]];
            bool split = false;
            if (c.start >= cover - maxGap)
            {
                if (overlapsOnly)
                    split = c.start > bounds.back();
                else if (c.start > bounds.back() + 3)
                {
                    const auto env = harmonicEnvelope(ctx, c, 8, c.start - 6, c.start + 3);
                    split = isReattack(env, 6);
                }
            }
            else if (c.start - bounds.back() >= minApart)
            {
                const auto env = harmonicEnvelope(ctx, c, 8, c.start - 6, c.start + 3);
                if (isReattack(env, 6))
                {
                    if (!test)
                        test = std::make_unique<ReattackTest>(ctx, whole);
                    for (int i = c.start - 3 - lane.start; i <= c.start + 3 - lane.start && !split; ++i)
                        split = i >= 4 && i <= test->len - 5 && test->risesAndStays(i);
                    if (!split && c.end > cover + maxGap && cover + 1 - bounds.back() >= minApart)
                    {
                        // it goes on past the lane's sound so far and started like a new note: it begins where that sound ends
                        bounds.push_back(cover + 1);
                        cover = std::max(cover, c.end);
                        continue;
                    }
                }
            }
            if (split)
                bounds.push_back(c.start);
            cover = std::max(cover, c.end);
        }
        // no note shorter than a note can be
        for (size_t q = 1; q < bounds.size();)
        {
            const int next = q + 1 < bounds.size() ? bounds[q + 1] : lane.end + 1;
            if (next - bounds[q] < minLen || bounds[q] - bounds[q - 1] < minLen)
                bounds.erase(bounds.begin() + static_cast<std::ptrdiff_t>(q));
            else
                ++q;
        }
        bounds.push_back(lane.end + 1);
        for (size_t q = 0; q + 1 < bounds.size(); ++q)
        {
            const int from = bounds[q];
            int to = bounds[q + 1] - 1;
            // up to where its members reach (a note left apart keeps its own end)
            int reach = from;
            for (size_t k : lane.members)
                if (notes[k].start <= to && notes[k].end >= from)
                    reach = std::max(reach, notes[k].end);
            to = std::min(to, reach);
            // its own member: the one starting there (else the one covering most of it)
            int own = -1, ownScore = -1;
            for (size_t k : lane.members)
            {
                const auto& m = notes[k];
                const int overlap = std::min(m.end, to) - std::max(m.start, from) + 1;
                if (overlap <= 0)
                    continue;
                const int score = (m.start == from ? 1 << 20 : 0) + overlap;
                if (score > ownScore)
                {
                    ownScore = score;
                    own = static_cast<int>(k);
                }
            }
            if (own < 0)
                continue;
            const auto& self = notes[static_cast<size_t>(own)];
            // its own member's pitch where that agrees with the lane's, the lane's elsewhere
            Candidate note;
            note.start = from;
            note.end = to;
            note.f0.resize(static_cast<size_t>(to - from + 1));
            bool agrees = true;
            for (int t = from; t <= to; ++t)
            {
                float hz = whole.f0[static_cast<size_t>(t - lane.start)];
                if (t >= self.start && t <= self.end)
                {
                    const float ownHz = self.f0[static_cast<size_t>(t - self.start)];
                    if (std::fabs(1200.0f * std::log2(ownHz / hz)) <= 25.0f)
                        hz = ownHz;
                    else
                        agrees = false;
                }
                note.f0[static_cast<size_t>(t - from)] = hz;
            }
            if (agrees && self.start == from && self.end == to)
            {
                out.push_back(self);   // a member as it was, the rest merged into it
                continue;
            }
            note.cents.resize(note.f0.size());
            for (size_t i = 0; i < note.f0.size(); ++i)
                note.cents[i] = hzToField(note.f0[i], ctx.p);
            note.strength.assign(note.cents.size(), 1.0f);
            note.B = whole.B;
            note.probability = 0.0f;
            for (size_t k : lane.members)
            {
                const auto& m = notes[k];
                const int overlap = std::min(m.end, note.end) - std::max(m.start, note.start) + 1;
                if (overlap <= 0)
                    continue;
                note.energy += m.energy * overlap / std::max(1, m.length());
                note.probability = std::max(note.probability, m.probability);
                note.hits = std::max(note.hits, m.hits);
            }
            out.push_back(std::move(note));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.start < b.start; });
    return out;
}

} // namespace detail
} // namespace polyphonic
