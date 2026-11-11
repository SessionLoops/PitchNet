#pragma once

// Internal building blocks of the polyphonic engine. Not part of the public API.

#include "PolyEngine.h"
#include "PolyFFT.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

namespace polyphonic
{
namespace detail
{

constexpr double kPi = 3.14159265358979323846;

/**
 * STFT frames computed on demand (nothing frame-sized is stored for the whole signal).
 * Frame t is centred on sample t * hop; spectra are normalised so a sinusoid of
 * amplitude A peaks at ~A.
 */
class StftFrames
{
public:
    StftFrames(const float* signal, int numSamples, const Params& p);
    int numFrames() const { return frames; }
    int numBins() const { return bins; }
    void spectrum(int t, std::complex<float>* out);

private:
    const float* x;
    int n, frames, bins, N, H;
    std::vector<float> window, buffer;
    float norm;
    std::unique_ptr<RealFFT> fft;
};

/** Dense frames x bins float matrix. */
struct Matrix
{
    int rows = 0;
    int cols = 0;
    std::vector<float> data;

    Matrix() = default;
    Matrix(int r, int c, float v = 0.0f) : rows(r), cols(c), data(static_cast<size_t>(r) * c, v) {}
    float* row(int r) { return data.data() + static_cast<size_t>(r) * cols; }
    const float* row(int r) const { return data.data() + static_cast<size_t>(r) * cols; }
    float& at(int r, int c) { return data[static_cast<size_t>(r) * cols + c]; }
    float at(int r, int c) const { return data[static_cast<size_t>(r) * cols + c]; }
};

/** Spectral peaks, grouped by frame (CSR layout). */
struct Peaks
{
    std::vector<int> frameStart;       // numFrames + 1 offsets
    std::vector<int> bin;              // integer peak bin
    std::vector<float> fbin;           // refined frequency in bins
    std::vector<float> amp;            // sinusoid amplitude
    std::vector<float> tonality;       // 0..1
    std::vector<float> wamp;           // whitened amplitude

    int begin(int t) const { return frameStart[static_cast<size_t>(t)]; }
    int end(int t) const { return frameStart[static_cast<size_t>(t) + 1]; }
    /** Index of the peak in frame t nearest to frequency `bins`, or -1. */
    int nearest(int t, float bins) const;
};

/** A note candidate during analysis, before the note filter decides whether it is real. */
struct Candidate
{
    int start = 0;
    int end = 0;                       // inclusive
    std::vector<float> cents;          // field index per frame
    std::vector<float> strength;       // relevance along the crest
    double energy = 0.0;
    int hits = 1;
    std::vector<float> f0;             // Hz per frame (after refinement)
    float B = 0.0f;
    float probability = 1.0f;

    int length() const { return end - start + 1; }
    float f0At(int t) const
    {
        const int i = std::clamp(t - start, 0, static_cast<int>(f0.size()) - 1);
        return f0[static_cast<size_t>(i)];
    }
    float midi() const;
};

struct Context
{
    Params p;
    const float* audio = nullptr;      // mono analysis signal (owned by the caller of analyze())
    int numSamples = 0;
    int numFrames = 0;
    int numBins = 0;
    const NoteEvidence* evidence = nullptr; // non-owning; only for neural-guided candidate search
    Peaks peaks;
    std::vector<float> r0max;          // per frame: maximum of the initial relevance landscape
};

// ---- PolySpectral.cpp ----
std::vector<float> hannPeriodic(int n);
void medianRow(const float* in, int K, int width, float* out, std::vector<float>& scratch);
/** One streaming pass: spectral peaks with instantaneous frequency, tonality and whitening. */
void analyzeSpectra(Context& ctx, const std::atomic<bool>* cancel);

// ---- PolyExtraction.cpp ----
int numCentsBins(const Params& p);
float hzToField(float hz, const Params& p);
float fieldToHz(float c, const Params& p);
std::vector<Candidate> extractCandidates(Context& ctx, const std::atomic<bool>* cancel);
void pruneCandidates(const Context& ctx, std::vector<Candidate>& cands);
void refineCandidates(const Context& ctx, std::vector<Candidate>& cands);
std::vector<float> harmonicEnvelope(const Context& ctx, const Candidate& n, int numHarmonics, int t0, int t1);
/** envMax: the maximum of env when the caller already has it (computed otherwise). */
bool isReattack(const std::vector<float>& env, int i, float ratio = 2.0f, float floorRel = 0.1f, float envMax = -1.0f);
/** Amplitude of each of the first numHarmonics partials, per frame in [t0, t1]. */
std::vector<std::vector<float>> partialEnvelopes(const Context& ctx, const Candidate& n, int numHarmonics, int t0, int t1);
/** True when at least `minShare` of the partials present around frame i rise together there. */
bool partialsRiseTogether(const std::vector<std::vector<float>>& partials, int i, float minShare);
constexpr float kReattackShare = 0.6f;
constexpr float kReattackDip = 0.7f;
constexpr float kReattackStandOut = 2.5f;   // rise over the note's typical frame-to-frame change
/** Split candidates where their own partials are struck again (repeated notes at one pitch). */
std::vector<Candidate> splitReattacks(const Context& ctx, const std::vector<Candidate>& cands);
/** One note per stroke from overlapping or touching notes of one pitch (see PolyExtraction.cpp). */
std::vector<Candidate> mergeFragments(const Context& ctx, std::vector<Candidate> notes, bool overlapsOnly = false);
void refineOnsets(const Context& ctx, std::vector<Candidate>& notes);

// ---- PolyPurifier.cpp ----
void scoreCandidates(const Context& ctx, std::vector<Candidate>& cands, const NoteEvidence* evidence,
                     std::vector<CandidateFeatures>* exportTo = nullptr);

// ---- PolySoundModel.cpp ----
void buildMasks(const Context& ctx, const std::vector<Candidate>& notes, Analysis& out);

} // namespace detail
} // namespace polyphonic
