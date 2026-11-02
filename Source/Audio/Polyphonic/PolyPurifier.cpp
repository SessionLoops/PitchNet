// Polyphonic engine - note filter: score each note candidate as a real note or a harmonic "ghost" (an
// overtone of a lower note taken for a note of its own).
//
// Gradient-boosted trees over the features below, trained on mixtures of real instrument
// recordings with known notes (see docs/polyphonic-editing.md): relative energy, how much of the
// candidate's partials it shares with other candidates, whether it moves like an overtone of a
// lower candidate, and so on. All features describe pitched candidates; with neural note evidence
// (Basic Pitch's note posteriors at the candidate's pitch) the "Evidence" model is used, otherwise
// the DSP-only one.

#include "PolyInternal.h"
#include "PolyPurifierModel.h"

#include <cassert>

#ifdef POLYPHONIC_DUMP_FEATURES
#include <cstdio>
#endif

namespace polyphonic
{
namespace detail
{

namespace
{
float median(std::vector<float> v)
{
    if (v.empty())
        return 0.0f;
    const size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2), v.end());
    const float hi = v[n / 2];
    if (n % 2)
        return hi;
    return 0.5f * (hi + *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2)));
}

double stddev(const std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    double m = 0.0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size()));
}

double correlation(const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t n = std::min(a.size(), b.size());
    if (n < 2)
        return 0.0;
    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    if (saa <= 0.0 || sbb <= 0.0)
        return 0.0;
    const double r = sab / std::sqrt(saa * sbb);
    return (r >= -1.0 && r <= 1.0) ? r : 0.0;          // no isfinite(): -ffast-math
}

float predict(const purifier::TreeNode* nodes, const int* starts, int numTrees, float init, float rate,
              int numFeatures, const std::vector<float>& x)
{
    // the generated model and this file's feature code must agree (retrain after changing features)
    assert(static_cast<int>(x.size()) == numFeatures);
    if (static_cast<int>(x.size()) != numFeatures)
        return 0.5f;
    double raw = init;
    for (int k = 0; k < numTrees; ++k)
    {
        int i = starts[k];
        while (nodes[i].feature >= 0)
            i = (x[static_cast<size_t>(nodes[i].feature)] <= nodes[i].threshold) ? nodes[i].left : nodes[i].right;
        raw += static_cast<double>(rate) * nodes[i].value;
    }
    return static_cast<float>(1.0 / (1.0 + std::exp(-raw)));
}
} // namespace

void scoreCandidates(const Context& ctx, std::vector<Candidate>& cands, const NoteEvidence* evidence,
                     std::vector<CandidateFeatures>* exportTo)
{
    const Params& p = ctx.p;
    const auto& pk = ctx.peaks;
    const int N = static_cast<int>(cands.size());
    if (N == 0)
        return;
    const float binHz = static_cast<float>(p.sampleRate / p.fftSize);
    constexpr int numHarm = 30;
    const bool useEvidence = evidence != nullptr && !evidence->empty();

    std::vector<float> midi(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        midi[static_cast<size_t>(i)] = cands[static_cast<size_t>(i)].midi();

    // claims table: amplitude at each partial, and per spectral peak its owners (count, highest pitch)
    const size_t numPeaks = pk.amp.size();
    std::vector<int> ownerCount(numPeaks, 0), lastOwner(numPeaks, -1);
    std::vector<float> maxOwnerMidi(numPeaks, -1e9f);
    std::vector<std::vector<float>> amps(static_cast<size_t>(N));
    std::vector<std::vector<int>> pids(static_cast<size_t>(N));
    for (int ni = 0; ni < N; ++ni)
    {
        const auto& n = cands[static_cast<size_t>(ni)];
        auto& am = amps[static_cast<size_t>(ni)];
        auto& pi = pids[static_cast<size_t>(ni)];
        am.assign(static_cast<size_t>(n.length()) * numHarm, 0.0f);
        pi.assign(static_cast<size_t>(n.length()) * numHarm, -1);
        for (int i = 0; i < n.length(); ++i)
        {
            const int t = n.start + i;
            if (pk.begin(t) == pk.end(t))
                continue;
            const float f0 = n.f0[static_cast<size_t>(i)];
            for (int h = 1; h <= numHarm; ++h)
            {
                const float fh = static_cast<float>(f0 * h * std::sqrt(1.0 + n.B * h * h));
                if (!(fh < p.sampleRate / 2 - 100))
                    continue;
                const float tol = std::min(std::max(1.2f * binHz, 0.012f * fh), 0.4f * f0);
                const int j = pk.nearest(t, fh / binHz);
                if (!(std::fabs(pk.fbin[static_cast<size_t>(j)] * binHz - fh) < tol))
                    continue;
                const size_t idx = static_cast<size_t>(i) * numHarm + static_cast<size_t>(h - 1);
                am[idx] = pk.amp[static_cast<size_t>(j)];
                pi[idx] = j;
                if (lastOwner[static_cast<size_t>(j)] != ni)
                {
                    lastOwner[static_cast<size_t>(j)] = ni;
                    ++ownerCount[static_cast<size_t>(j)];
                    maxOwnerMidi[static_cast<size_t>(j)] = std::max(maxOwnerMidi[static_cast<size_t>(j)], midi[static_cast<size_t>(ni)]);
                }
            }
        }
    }

    // relevance of the frame-wise strongest crest, for the relative-strength feature
    const auto& r0max = ctx.r0max;

    // neural evidence resampled to the analysis frame grid (next evidence frame at or after t)
    std::vector<int> evFrame;
    if (useEvidence)
    {
        const int T = ctx.numFrames;
        evFrame.resize(static_cast<size_t>(T));
        for (int t = 0; t < T; ++t)
        {
            const float sec = static_cast<float>(t * p.hop / p.sampleRate);
            const auto it = std::lower_bound(evidence->frameTimes.begin(), evidence->frameTimes.end(), sec);
            evFrame[static_cast<size_t>(t)] = static_cast<int>(std::min<std::ptrdiff_t>(it - evidence->frameTimes.begin(), static_cast<std::ptrdiff_t>(evidence->frameTimes.size()) - 1));
        }
    }

    std::vector<float> x;
    for (int ni = 0; ni < N; ++ni)
    {
        auto& n = cands[static_cast<size_t>(ni)];
        const float m = midi[static_cast<size_t>(ni)];
        const auto& am = amps[static_cast<size_t>(ni)];
        const auto& pi = pids[static_cast<size_t>(ni)];
        // ---- relative strength / energy
        float ref = 0.0f;
        for (int t = n.start; t <= n.end; ++t)
            ref = std::max(ref, r0max[static_cast<size_t>(t)]);
        const float smax = *std::max_element(n.strength.begin(), n.strength.end());
        const double rel = smax / std::max(ref, 1e-12f);
        std::vector<int> ov;
        double maxEnergy = n.energy;
        for (int mi = 0; mi < N; ++mi)
        {
            const auto& o = cands[static_cast<size_t>(mi)];
            if (mi != ni && o.start <= n.end && o.end >= n.start)
            {
                ov.push_back(mi);
                maxEnergy = std::max(maxEnergy, o.energy);
            }
        }
        const double energyRel = n.energy / maxEnergy;
        // ---- partial sharing
        double tot = 0.0, uniqHi = 0.0, uniqAny = 0.0, f0Share = 0.0;
        std::vector<float> f0db;
        for (int i = 0; i < n.length(); ++i)
        {
            float amax = 0.0f;
            for (int h = 0; h < numHarm; ++h)
            {
                const size_t idx = static_cast<size_t>(i) * numHarm + static_cast<size_t>(h);
                const double P = static_cast<double>(am[idx]) * am[idx];
                amax = std::max(amax, am[idx]);
                tot += P;
                if (h == 0)
                    f0Share += P;
                const int j = pi[idx];
                const bool shHi = j >= 0 && maxOwnerMidi[static_cast<size_t>(j)] > m + 0.5f;
                const bool shAny = j >= 0 && ownerCount[static_cast<size_t>(j)] > 1;
                if (!shHi) uniqHi += P;
                if (!shAny) uniqAny += P;
            }
            if (amax > 0.0f)
                f0db.push_back(20.0f * std::log10(std::max(am[static_cast<size_t>(i) * numHarm], 1e-9f) / amax));
        }
        tot = std::max(tot, 1e-20);
        // ---- overtone relation to a lower candidate: does it move like one of its harmonics?
        int bestH = 0, bestDt = 99;
        double bestPc = 0.0, bestAc = 0.0;
        for (int li = 0; li < N; ++li)
        {
            if (li == ni || !(midi[static_cast<size_t>(li)] < m - 0.5f))
                continue;
            const auto& l = cands[static_cast<size_t>(li)];
            const int a = std::max(n.start, l.start), b = std::min(n.end, l.end);
            if (b - a + 1 < 0.5 * n.length())
                continue;
            std::vector<float> fu, fl, ratio;
            for (int t = a; t <= b; ++t)
            {
                fu.push_back(n.f0[static_cast<size_t>(t - n.start)]);
                fl.push_back(l.f0[static_cast<size_t>(t - l.start)]);
                ratio.push_back(fu.back() / fl.back());
            }
            const int h = static_cast<int>(std::nearbyint(median(ratio)));
            if (h < 2 || h > 16)
                continue;
            const float mfl = median(fl), mfu = median(fu);
            const double fh = h * mfl * std::sqrt(1.0 + l.B * h * h);
            if (std::fabs(1200.0 * std::log2(mfu / fh)) > 40.0)
                continue;
            if (!(bestH == 0 || h < bestH))
                continue;
            std::vector<double> du, dl;
            for (size_t q = 0; q < fu.size(); ++q)
            {
                du.push_back(1200.0 * std::log2(fu[q] / mfu));
                dl.push_back(1200.0 * std::log2(fl[q] / mfl));
            }
            bestPc = (stddev(du) > 3.0 && stddev(dl) > 3.0) ? correlation(du, dl) : 0.0;
            bestH = h;
            bestDt = std::abs(n.start - l.start);
            const auto eu = harmonicEnvelope(ctx, n, 1, a, b);
            const auto el = harmonicEnvelope(ctx, l, std::max(1, h + 1), a, b);
            std::vector<double> ue(eu.begin(), eu.end()), le(el.begin(), el.end());
            bestAc = 0.0;
            if (ue.size() > 4 && stddev(ue) > 0.0 && stddev(le) > 0.0 && ue.size() > 5)
            {
                std::vector<double> dlu, dll;
                for (size_t q = 1; q < ue.size(); ++q)
                {
                    dlu.push_back(std::log(ue[q] + 1e-6) - std::log(ue[q - 1] + 1e-6));
                    dll.push_back(std::log(le[q] + 1e-6) - std::log(le[q - 1] + 1e-6));
                }
                bestAc = correlation(dlu, dll);
            }
        }
        // ---- sub-harmonic relation: a higher overlapping candidate on this one's h-th partial
        int sub = 0;
        bool isLowest = true;
        for (int mi : ov)
        {
            const float om = midi[static_cast<size_t>(mi)];
            if (!(om > m))
                isLowest = false;
            if (om <= m + 0.5f)
                continue;
            const double r = std::pow(2.0, (om - m) / 12.0);
            const int h = static_cast<int>(std::nearbyint(r));
            if (h >= 2 && h <= 8 && std::fabs(1200.0 * std::log2(r / h)) < 40.0)
                sub = sub == 0 ? h : std::min(sub, h);
        }
        const double dur = n.length() * p.hop / p.sampleRate;
        const double f0RelDb = f0db.empty() ? -90.0 : median(f0db);

        // feature vector, in the order of PolyPurifierModel.h
        x.assign(0, 0.0f);
        x.push_back(static_cast<float>(std::log10(std::max(rel, 1e-3))));
        x.push_back(static_cast<float>(std::log10(std::max(energyRel, 1e-4))));
        x.push_back(static_cast<float>(uniqHi / tot));
        x.push_back(static_cast<float>(uniqAny / tot));
        x.push_back(static_cast<float>(f0Share / tot));
        x.push_back(static_cast<float>(std::max(f0RelDb, -60.0) / 60.0));
        x.push_back(static_cast<float>(std::log10(std::max(dur, 0.02))));
        x.push_back(static_cast<float>((m - 60.0) / 24.0));
        x.push_back(static_cast<float>(std::log2(std::max(n.hits, 1))));
        x.push_back(bestH == 2 ? 1.0f : 0.0f);
        x.push_back(bestH == 3 ? 1.0f : 0.0f);
        x.push_back(bestH >= 4 ? 1.0f : 0.0f);
        x.push_back(bestH ? static_cast<float>(std::min(bestDt, 20) / 20.0) : 1.0f);
        x.push_back(bestH ? static_cast<float>(bestPc) : 0.0f);
        x.push_back(bestH ? static_cast<float>(bestAc) : 0.0f);
        x.push_back(sub == 2 ? 1.0f : 0.0f);
        x.push_back(sub >= 3 ? 1.0f : 0.0f);
        x.push_back(static_cast<float>(std::min(static_cast<int>(ov.size()), 12) / 12.0));
        x.push_back(isLowest ? 1.0f : 0.0f);
        if (useEvidence)
        {
            const int k = static_cast<int>(std::nearbyint(m)) - 21;
            double bpMean = 0.0, bpMax = 0.0;
            if (k >= 0 && k < 88)
            {
                for (int t = n.start; t <= n.end; ++t)
                {
                    const float v = evidence->note[static_cast<size_t>(evFrame[static_cast<size_t>(t)]) * 88 + static_cast<size_t>(k)];
                    bpMean += v;
                    bpMax = std::max(bpMax, static_cast<double>(v));
                }
                bpMean /= n.length();
            }
            x.push_back(static_cast<float>(bpMean));
            x.push_back(static_cast<float>(bpMax));
            n.probability = predict(purifier::kEvidenceNodes, purifier::kEvidenceTreeStart, purifier::kEvidenceNumTrees,
                                    purifier::kEvidenceInit, purifier::kEvidenceRate, purifier::kEvidenceNumFeatures, x);
        }
        else
        {
            n.probability = predict(purifier::kDspNodes, purifier::kDspTreeStart, purifier::kDspNumTrees,
                                    purifier::kDspInit, purifier::kDspRate, purifier::kDspNumFeatures, x);
        }
        if (exportTo != nullptr)
            exportTo->push_back({n.start, n.end, m, n.probability, x});
#ifdef POLYPHONIC_DUMP_FEATURES
        std::fprintf(stderr, "feat %d", ni);
        for (float v : x)
            std::fprintf(stderr, " %.6g", v);
        std::fprintf(stderr, " p=%.6g\n", n.probability);
#endif
    }
}

} // namespace detail
} // namespace polyphonic
