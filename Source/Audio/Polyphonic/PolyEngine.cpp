// Polyphonic engine - analysis entry point.

#include "PolyInternal.h"

namespace polyphonic
{

Analysis analyze(const float* mono, int numSamples, const Params& params, const NoteEvidence* evidence,
                 const std::atomic<bool>* cancel, const Progress& progress)
{
    using namespace detail;
    Analysis out;
    out.params = params;
    out.numSamples = numSamples;
    if (mono == nullptr || numSamples <= 0)
        return out;
    auto report = [&](float v) { if (progress) progress(v); };
    auto cancelled = [&] { return cancel != nullptr && cancel->load(); };

    Context ctx;
    ctx.p = params;
    ctx.audio = mono;
    ctx.numSamples = numSamples;
    analyzeSpectra(ctx, cancel);
    if (cancelled()) return out;
    out.numFrames = ctx.numFrames;
    out.numBins = ctx.numBins;
    report(0.2f);

    // note candidates from the harmonic-sum landscape, then the learned real-note/ghost filter
    auto cands = extractCandidates(ctx, cancel);
    if (cancelled()) return out;
    report(0.5f);
    pruneCandidates(ctx, cands);
    refineCandidates(ctx, cands);
    cands = splitReattacks(ctx, cands);
    scoreCandidates(ctx, cands, evidence, params.exportFeatures ? &out.candidateFeatures : nullptr);
    report(0.6f);

    std::vector<Candidate> active, inactive;
    for (auto& c : cands)
        (c.probability >= params.activationThreshold ? active : inactive).push_back(c);
    active = mergeFragments(ctx, std::move(active));
    refineOnsets(ctx, active);
    std::stable_sort(active.begin(), active.end(), [](const Candidate& a, const Candidate& b) {
        return a.start != b.start ? a.start < b.start : a.midi() < b.midi();
    });

    auto toNote = [](const Candidate& c, int id, bool isActive) {
        Note n;
        n.id = id;
        n.startFrame = c.start;
        n.endFrame = c.end;
        n.f0 = c.f0;
        n.inharmonicity = c.B;
        n.energy = c.energy;
        n.probability = c.probability;
        n.active = isActive;
        return n;
    };
    for (const auto& c : active)
        out.notes.push_back(toNote(c, static_cast<int>(out.notes.size()), true));
    for (const auto& c : inactive)
    {
        Note n = toNote(c, static_cast<int>(out.notes.size()), false);
        // an inactive candidate that sits on a partial of an active note belongs to that note
        float bestErr = 40.0f;
        for (size_t ai = 0; ai < active.size(); ++ai)
        {
            const auto& a = active[ai];
            if (a.end < c.start || a.start > c.end)
                continue;
            const int mid = (std::max(a.start, c.start) + std::min(a.end, c.end)) / 2;
            const float ratio = c.f0At(mid) / a.f0At(mid);
            const int h = static_cast<int>(std::lround(ratio));
            if (h < 1)
                continue;
            const float err = std::fabs(1200.0f * std::log2(ratio / (h * std::sqrt(1.0f + a.B * h * h))));
            if (err < bestErr)
            {
                bestErr = err;
                n.parent = static_cast<int>(ai);
            }
        }
        out.notes.push_back(std::move(n));
    }
    if (cancelled()) return out;

    buildMasks(ctx, active, out);
    report(1.0f);
    return out;
}

} // namespace polyphonic
