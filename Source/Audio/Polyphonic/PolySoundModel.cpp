// Polyphonic engine - sound model: a pitch-informed harmonic NMF (after Ewert & Mueller 2012) that gives each
// note its share of the bins around its partials.

#include "PolyInternal.h"

namespace polyphonic
{
namespace detail
{

namespace
{
// Magnitude of the Hann window's transform at a fractional bin offset, 1 at the centre.
inline float hannLobe(float d)
{
    d = std::fabs(d);
    if (d >= 2.0f)
        return 0.0f;
    if (d < 1e-6f)
        return 1.0f;
    const float den = 1.0f - d * d;
    if (std::fabs(den) < 1e-5f)
        return 0.5f;
    const float x = static_cast<float>(kPi) * d;
    return std::fabs(std::sin(x) / x / den);
}

inline int roundHalfEven(float v) { return static_cast<int>(std::nearbyint(v)); }

struct Entry
{
    int t, k, note, harm, seg;
    float phi, kc;
};
} // namespace

namespace
{
// One NMF solve over frames [lo, hi) for the notes in `idx`; masks are stored for [coreLo, coreHi).
void nmfBlock(const Context& ctx, const std::vector<Candidate>& notes, const std::vector<int>& idx,
              int lo, int hi, int coreLo, int coreHi, Analysis& out)
{
    const Params& p = ctx.p;
    const int K = ctx.numBins;
    const float binHz = static_cast<float>(p.sampleRate / p.fftSize);
    const int H = p.nmfHarmonics;
    const int kmax = std::min(K - 2, static_cast<int>(p.nmfMaxHz / binHz));
    const int segLen = std::max(1, static_cast<int>(std::lround(p.nmfSegmentSeconds * p.sampleRate / p.hop)));
    const int N = static_cast<int>(idx.size());
    const auto& pk = ctx.peaks;

    std::vector<Entry> entries;
    std::vector<int> nseg(static_cast<size_t>(N), 1), first(static_cast<size_t>(N)), last(static_cast<size_t>(N)), gOff(static_cast<size_t>(N) + 1, 0);
    for (int q = 0; q < N; ++q)
    {
        const auto& n = notes[static_cast<size_t>(idx[static_cast<size_t>(q)])];
        const int a = std::max({0, n.start - p.preFrames, lo});
        const int b = std::min({ctx.numFrames - 1, n.end + p.postFrames, hi - 1});
        first[static_cast<size_t>(q)] = a;
        last[static_cast<size_t>(q)] = std::max(a, b);
        nseg[static_cast<size_t>(q)] = 1 + (std::max(a, b) - a) / segLen;
        gOff[static_cast<size_t>(q) + 1] = gOff[static_cast<size_t>(q)] + (std::max(a, b) - a + 1);
        for (int t = a; t <= b; ++t)
        {
            const float f0 = n.f0At(t);
            for (int h = 1; h <= H; ++h)
            {
                const float fh = static_cast<float>(f0 * h * std::sqrt(1.0 + n.B * h * h));
                if (!(fh < std::min(p.nmfMaxHz, static_cast<float>(p.sampleRate / 2 - 200))))
                    continue;
                float kc = fh / binHz;
                if (pk.begin(t) < pk.end(t))
                {
                    const int j = pk.nearest(t, kc);
                    const float tol = std::min(std::max(1.0f, 0.01f * kc), 0.35f * f0 / binHz);
                    if (std::fabs(pk.fbin[static_cast<size_t>(j)] - kc) < tol)
                        kc = pk.fbin[static_cast<size_t>(j)];
                }
                const int k0 = roundHalfEven(kc);
                for (int o = -p.nmfHalfWidth; o <= p.nmfHalfWidth; ++o)
                {
                    const int kk = k0 + o;
                    const float phi = hannLobe(static_cast<float>(kk) - kc);
                    if (kk < 1 || kk > kmax || phi <= 1e-3f)
                        continue;
                    entries.push_back({t, kk, q, h - 1, (t - a) / segLen, phi, kc});
                }
            }
        }
    }
    if (entries.empty())
        return;
    int maxSeg = 1;
    for (int s : nseg)
        maxSeg = std::max(maxSeg, s);
    auto wIndex = [&](int n, int s, int h) { return (static_cast<size_t>(n) * maxSeg + static_cast<size_t>(s)) * H + static_cast<size_t>(h); };
    auto gIndex = [&](int n, int t) { return static_cast<size_t>(gOff[static_cast<size_t>(n)] + (t - first[static_cast<size_t>(n)])); };
    std::vector<double> W(static_cast<size_t>(N) * maxSeg * H, 0.0), numW(W.size()), denW(W.size());
    for (int q = 0; q < N; ++q)
        for (int s = 0; s < nseg[static_cast<size_t>(q)]; ++s)
            for (int h = 0; h < H; ++h)
                W[wIndex(q, s, h)] = 1.0 / (h + 1);
    std::vector<double> G(static_cast<size_t>(gOff.back()), 0.05), numG(G.size()), denG(G.size());

    // cells: distinct (t, k) touched by the model
    std::vector<size_t> flat(entries.size()), order(entries.size());
    for (size_t e = 0; e < entries.size(); ++e)
    {
        flat[e] = static_cast<size_t>(entries[e].t) * K + static_cast<size_t>(entries[e].k);
        order[e] = e;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return flat[a] < flat[b]; });
    std::vector<int> cell(entries.size());
    int numCells = 0;
    for (size_t q = 0; q < order.size(); ++q)
    {
        if (q > 0 && flat[order[q]] != flat[order[q - 1]])
            ++numCells;
        cell[order[q]] = numCells;
    }
    ++numCells;
    std::vector<double> Vh(static_cast<size_t>(numCells)), cellV(static_cast<size_t>(numCells)), cellNoise(static_cast<size_t>(numCells));
    {
        // magnitude and noise floor for this block only (recomputed: nothing frame-sized is kept)
        StftFrames frames(ctx.audio, ctx.numSamples, p);
        Matrix magB(hi - lo, K), noiseB(hi - lo, K);
        std::vector<std::complex<float>> spec(static_cast<size_t>(K));
        std::vector<float> scratch;
        for (int t = lo; t < hi; ++t)
        {
            frames.spectrum(t, spec.data());
            float* m = magB.row(t - lo);
            for (int k = 0; k < K; ++k)
                m[k] = std::abs(spec[static_cast<size_t>(k)]);
            medianRow(m, K, p.noiseMedianBins, noiseB.row(t - lo), scratch);
        }
        for (size_t e = 0; e < entries.size(); ++e)
        {
            cellV[static_cast<size_t>(cell[e])] = magB.at(entries[e].t - lo, entries[e].k);
            cellNoise[static_cast<size_t>(cell[e])] = noiseB.at(entries[e].t - lo, entries[e].k);
        }
    }
    const double eps = 1e-12;
    std::vector<double> contrib(entries.size());
    auto evaluate = [&]() {
        for (int c = 0; c < numCells; ++c)
            Vh[static_cast<size_t>(c)] = cellNoise[static_cast<size_t>(c)];
        for (size_t e = 0; e < entries.size(); ++e)
        {
            const auto& en = entries[e];
            contrib[e] = G[gIndex(en.note, en.t)] * W[wIndex(en.note, en.seg, en.harm)] * en.phi;
            Vh[static_cast<size_t>(cell[e])] += contrib[e];
        }
    };
    std::vector<double> tmp(static_cast<size_t>(maxSeg) * H);
    for (int it = 0; it < p.nmfIterations; ++it)
    {
        evaluate();
        std::fill(numW.begin(), numW.end(), 0.0);
        std::fill(denW.begin(), denW.end(), 0.0);
        for (size_t e = 0; e < entries.size(); ++e)
        {
            const auto& en = entries[e];
            const double r = cellV[static_cast<size_t>(cell[e])] / (Vh[static_cast<size_t>(cell[e])] + eps);
            const double g = G[gIndex(en.note, en.t)] * en.phi;
            numW[wIndex(en.note, en.seg, en.harm)] += g * r;
            denW[wIndex(en.note, en.seg, en.harm)] += g;
        }
        for (size_t i = 0; i < W.size(); ++i)
            W[i] *= numW[i] / (denW[i] + eps);
        for (int q = 0; q < N; ++q)
        {
            const int ns = nseg[static_cast<size_t>(q)];
            if (ns > 1)
            {
                for (int s = 0; s < ns; ++s)
                    for (int h = 0; h < H; ++h)
                    {
                        const double l = W[wIndex(q, std::max(0, s - 1), h)], c = W[wIndex(q, s, h)], r = W[wIndex(q, std::min(ns - 1, s + 1), h)];
                        tmp[static_cast<size_t>(s) * H + static_cast<size_t>(h)] = 0.5 * c + 0.25 * (l + r);
                    }
                for (int s = 0; s < ns; ++s)
                    for (int h = 0; h < H; ++h)
                        W[wIndex(q, s, h)] = tmp[static_cast<size_t>(s) * H + static_cast<size_t>(h)];
            }
            double meanScale = 0.0;
            for (int s = 0; s < ns; ++s)
            {
                double sum = eps;
                for (int h = 0; h < H; ++h)
                    sum += W[wIndex(q, s, h)];
                for (int h = 0; h < H; ++h)
                    W[wIndex(q, s, h)] /= sum;
                meanScale += sum;
            }
            meanScale /= ns;
            for (int g = gOff[static_cast<size_t>(q)]; g < gOff[static_cast<size_t>(q) + 1]; ++g)
                G[static_cast<size_t>(g)] *= meanScale;
        }
        evaluate();
        std::fill(numG.begin(), numG.end(), 0.0);
        std::fill(denG.begin(), denG.end(), 0.0);
        for (size_t e = 0; e < entries.size(); ++e)
        {
            const auto& en = entries[e];
            const double r = cellV[static_cast<size_t>(cell[e])] / (Vh[static_cast<size_t>(cell[e])] + eps);
            const double w = W[wIndex(en.note, en.seg, en.harm)] * en.phi;
            numG[gIndex(en.note, en.t)] += w * r;
            denG[gIndex(en.note, en.t)] += w;
        }
        for (size_t i = 0; i < G.size(); ++i)
            G[i] *= numG[i] / (denG[i] + eps);
    }
    evaluate();

    std::vector<size_t> byKey;
    for (size_t e = 0; e < entries.size(); ++e)
        if (entries[e].t >= coreLo && entries[e].t < coreHi)
            byKey.push_back(e);
    std::sort(byKey.begin(), byKey.end(), [&](size_t a, size_t b) {
        const auto& x = entries[a];
        const auto& y = entries[b];
        if (x.note != y.note) return x.note < y.note;
        if (x.t != y.t) return x.t < y.t;
        if (x.harm != y.harm) return x.harm < y.harm;
        return x.k < y.k;
    });
    size_t q = 0;
    while (q < byKey.size())
    {
        const auto& e0 = entries[byKey[q]];
        size_t r = q;
        int kLo = e0.k, kHi = e0.k;
        while (r < byKey.size() && entries[byKey[r]].note == e0.note && entries[byKey[r]].t == e0.t && entries[byKey[r]].harm == e0.harm)
        {
            kLo = std::min(kLo, entries[byKey[r]].k);
            kHi = std::max(kHi, entries[byKey[r]].k);
            ++r;
        }
        auto& nm = out.masks[static_cast<size_t>(idx[static_cast<size_t>(e0.note)])];
        PartialMask pm;
        pm.frame = e0.t;
        pm.harmonic = e0.harm + 1;
        pm.lo = kLo;
        pm.count = kHi - kLo + 1;
        pm.weightOffset = static_cast<int>(nm.weights.size());
        pm.freqHz = e0.kc * binHz;
        pm.amplitude = static_cast<float>(G[gIndex(e0.note, e0.t)] * W[wIndex(e0.note, e0.seg, e0.harm)]);
        nm.weights.resize(nm.weights.size() + static_cast<size_t>(pm.count), 0.0f);
        for (size_t s = q; s < r; ++s)
        {
            const size_t e = byKey[s];
            nm.weights[static_cast<size_t>(pm.weightOffset + entries[e].k - kLo)] =
                static_cast<float>(contrib[e] / (Vh[static_cast<size_t>(cell[e])] + eps));
        }
        nm.partials.push_back(pm);
        q = r;
    }
}
} // namespace

// Notes only interact where they overlap in time, so the NMF is solved in blocks of a few seconds
// (with margins) to bound memory on long recordings. A recording shorter than one block is
// solved in one piece.
void buildMasks(const Context& ctx, const std::vector<Candidate>& notes, Analysis& out)
{
    const Params& p = ctx.p;
    const int T = ctx.numFrames;
    out.masks.assign(out.notes.size(), {});
    const int core = std::max(16, static_cast<int>(std::lround(4.0 * p.sampleRate / p.hop)));
    const int margin = static_cast<int>(std::lround(0.5 * p.sampleRate / p.hop));
    for (int blockStart = 0; blockStart < T; blockStart += core)
    {
        int coreEnd = std::min(T, blockStart + core);
        if (T - coreEnd < margin)                     // fold a short tail into this block
            coreEnd = T;
        const int lo = std::max(0, blockStart - margin), hi = std::min(T, coreEnd + margin);
        std::vector<int> idx;
        for (int ni = 0; ni < static_cast<int>(notes.size()); ++ni)
        {
            const auto& n = notes[static_cast<size_t>(ni)];
            if (n.end + p.postFrames >= lo && n.start - p.preFrames < hi)
                idx.push_back(ni);
        }
        if (!idx.empty())
            nmfBlock(ctx, notes, idx, lo, hi, blockStart, coreEnd, out);
        if (coreEnd == T)
            break;
    }
}

} // namespace detail
} // namespace polyphonic
