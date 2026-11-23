// Polyphonic engine - analysis entry point.

#include "PolyInternal.h"

namespace polyphonic
{

namespace
{
/** A note as the candidate the analysis helpers take. */
detail::Candidate candidateOf(const Note& n)
{
    detail::Candidate c;
    c.start = n.startFrame;
    c.end = n.endFrame;
    c.f0 = n.f0;
    c.B = n.inharmonicity;
    c.energy = n.energy;
    c.probability = n.probability;
    return c;
}

/** A candidate's level per frame from the spectral peaks at its partials (the root of their
 *  summed squared amplitudes), laid out as PolyphonicTake's noteLevelsFromMasks(). */
std::vector<float> peakLevel(const detail::Context& ctx, const detail::Candidate& c)
{
    const auto env = detail::partialEnvelopes(ctx, c, ctx.p.nmfHarmonics, c.start, c.end);
    std::vector<float> level(static_cast<size_t>(std::max(1, c.end - c.start + 1)), 0.0f);
    for (size_t k = 0; k < level.size(); ++k)
    {
        double energy = 0.0;
        for (const auto& h : env)
            if (k < h.size())
                energy += static_cast<double>(h[k]) * h[k];
        level[k] = static_cast<float>(std::sqrt(energy));
    }
    return level;
}

/** Analysis::candidateLevels for `out`'s inactive notes (needs the context's spectra). */
void estimateCandidateLevels(const detail::Context& ctx, Analysis& out)
{
    out.candidateLevels.assign(out.notes.size(), {});
    for (size_t i = 0; i < out.notes.size(); ++i)
        if (!out.notes[i].active && !out.notes[i].f0.empty())
            out.candidateLevels[i] = peakLevel(ctx, candidateOf(out.notes[i]));
}

/**
 * analyze() up to its notes: spectra, candidates, scoring, merging. Fills out's notes (and
 * numFrames, numBins) and `active`, the active notes as buildMasks takes them. `report` gets the
 * share done of the whole analysis (up to 0.65, where the masks start).
 */
bool detectNotes(detail::Context& ctx, const float* mono, int numSamples, const Params& params,
                 const EvidenceSource& evidenceSource, const std::atomic<bool>* cancel,
                 const std::function<void(float)>& report, Analysis& out, std::vector<detail::Candidate>& active)
{
    using namespace detail;
    auto cancelled = [&] { return cancel != nullptr && cancel->load(); };
    ctx.p = params;
    ctx.audio = mono;
    ctx.numSamples = numSamples;
    analyzeSpectra(ctx, cancel);
    if (cancelled()) return false;
    out.numFrames = ctx.numFrames;
    out.numBins = ctx.numBins;
    report(0.2f);

    // The evidence is asked for only where it is first needed, so whatever computes it can run
    // meanwhile: the candidate search needs it when guided, otherwise only the scoring does.
    // Guided without evidence falls back to the harmonic search (and its threshold).
    const NoteEvidence* evidence = nullptr;
    bool asked = false;
    auto getEvidence = [&]() {
        if (!asked && evidenceSource)
            evidence = evidenceSource();
        asked = true;
        if (evidence != nullptr && evidence->empty())
            evidence = nullptr;
        return evidence;
    };
    if (ctx.p.detectionMode == DetectionMode::NeuralGuided)
    {
        ctx.evidence = getEvidence();
        if (ctx.evidence == nullptr)
            ctx.p.detectionMode = DetectionMode::Harmonic;
        out.params.detectionMode = ctx.p.detectionMode;
        if (cancelled()) return false;
    }

    // note candidates from the harmonic-sum landscape, then the learned real-note/ghost filter
    auto cands = extractCandidates(ctx, cancel);
    if (cancelled()) return false;
    report(0.5f);
    pruneCandidates(ctx, cands);
    refineCandidates(ctx, cands);
    cands = splitReattacks(ctx, cands);
    scoreCandidates(ctx, cands, getEvidence(), params.exportFeatures ? &out.candidateFeatures : nullptr);
    if (cancelled()) return false;
    report(0.6f);

    std::vector<Candidate> inactive;
    active.clear();
    const float threshold = ctx.p.activationThresholdFor(ctx.p.detectionMode);
    for (auto& c : cands)
        (c.probability >= threshold ? active : inactive).push_back(c);
    active = mergeFragments(ctx, std::move(active));
    refineOnsets(ctx, active);
    // refined onsets change starts and pitch curves: join notes of one pitch that still overlap
    active = mergeFragments(ctx, std::move(active), true);
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
    return !cancelled();
}
} // namespace


Analysis analyze(const float* mono, int numSamples, const Params& params, const NoteEvidence* evidence,
                 const std::atomic<bool>* cancel, const Progress& progress)
{
    return analyze(mono, numSamples, params, EvidenceSource([evidence] { return evidence; }), cancel, progress);
}

Analysis analyze(const float* mono, int numSamples, const Params& params, const EvidenceSource& evidence,
                 const std::atomic<bool>* cancel, const Progress& progress)
{
    using namespace detail;
    Analysis out;
    out.params = params;
    out.numSamples = numSamples;
    if (mono == nullptr || numSamples <= 0)
        return out;
    auto report = [&](float v) { if (progress) progress(v); };
    Context ctx;
    std::vector<Candidate> active;
    if (!detectNotes(ctx, mono, numSamples, params, evidence, cancel, report, out, active))
        return out;
    estimateCandidateLevels(ctx, out);

    // The sound model is about the last third of the time; report it as it goes.
    report(0.65f);
    buildMasks(ctx, active, out, [&](float done) { report(0.65f + 0.35f * done); });
    report(1.0f);
    return out;
}

Analysis detect(const float* mono, int numSamples, const Params& params, const NoteEvidence* evidence,
                const std::atomic<bool>* cancel, const Progress& progress,
                std::vector<std::vector<float>>* levels)
{
    return detect(mono, numSamples, params, EvidenceSource([evidence] { return evidence; }), cancel, progress, levels);
}

Analysis detect(const float* mono, int numSamples, const Params& params, const EvidenceSource& evidence,
                const std::atomic<bool>* cancel, const Progress& progress,
                std::vector<std::vector<float>>* levels)
{
    using namespace detail;
    Analysis out;
    out.params = params;
    out.numSamples = numSamples;
    if (mono == nullptr || numSamples <= 0)
        return out;
    // the notes are the whole job here: their part of the analysis is reported as all of it
    auto report = [&](float v) { if (progress) progress(std::min(1.0f, v / 0.65f)); };
    Context ctx;
    std::vector<Candidate> active;
    if (!detectNotes(ctx, mono, numSamples, params, evidence, cancel, report, out, active))
        return out;
    out.masks.assign(out.notes.size(), {});
    estimateCandidateLevels(ctx, out);
    if (levels != nullptr)
    {
        // until the masks are built: each partial's spectral peak in place of its model amplitude;
        // the candidates' levels as they are kept anyway
        levels->assign(out.notes.size(), {});
        for (size_t i = 0; i < active.size() && i < out.notes.size(); ++i)
            (*levels)[i] = peakLevel(ctx, active[i]);
        for (size_t i = active.size(); i < out.notes.size(); ++i)
            (*levels)[i] = out.candidateLevels[i];
    }
    report(0.65f);
    return out;
}

Analysis restore(const float* mono, int numSamples, const Params& params, std::vector<Note> notes,
                 const std::atomic<bool>* cancel, const Progress& progress)
{
    using namespace detail;
    Analysis out;
    out.params = params;
    out.numSamples = numSamples;
    out.notes = std::move(notes);
    out.masks.assign(out.notes.size(), {});
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
    estimateCandidateLevels(ctx, out);
    report(0.4f);

    // the active notes as the candidates buildMasks takes, in their order; their masks are built
    // as analyze() builds them and then put back at each note's own index
    std::vector<Candidate> active;
    std::vector<size_t> where;
    for (size_t i = 0; i < out.notes.size(); ++i)
    {
        const auto& n = out.notes[i];
        if (!n.active || n.f0.empty())
            continue;
        Candidate c;
        c.start = n.startFrame;
        c.end = n.endFrame;
        c.f0 = n.f0;
        c.B = n.inharmonicity;
        c.energy = n.energy;
        c.probability = n.probability;
        active.push_back(std::move(c));
        where.push_back(i);
    }
    Analysis built;
    built.params = params;
    built.notes.resize(active.size());
    buildMasks(ctx, active, built, [&](float done) { report(0.4f + 0.6f * done); });
    if (cancelled()) return out;
    for (size_t k = 0; k < where.size() && k < built.masks.size(); ++k)
        out.masks[where[k]] = std::move(built.masks[k]);
    report(1.0f);
    return out;
}

} // namespace polyphonic
