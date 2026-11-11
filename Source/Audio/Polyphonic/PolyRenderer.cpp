// Polyphonic engine - resynthesis with per-note edits, streaming frame by frame.
//
// The output spectrum of a frame starts as the input spectrum. Every edited note first removes its
// own share (its masks) and then adds that share back transformed:
//   pitch   each partial's bin region moves by round((r - 1) f / binHz) bins and is rotated by an
//           accumulated phase  psi += 2 pi (r - 1) f hop / fs  (Laroche & Dolson 1999), which keeps
//           the partial phase-coherent from frame to frame without any time stretching. Phases are
//           those at the frame centre: frames start N/2 before it, so bin k carries (-1)^k and a
//           region moved by an odd number of bins is flipped back (vibrato changes the shift).
//           A scalar compensates Hann overlap-add attenuation at fractional-bin offsets;
//   formant optional: partial amplitudes follow the note's smoothed spectral envelope;
//   gain    a scalar; mute removes the share;
//   time    whole hops plus a fractional linear-phase delay;
//   stretch lengthened: the note's first Params::stretchAttackSeconds play as recorded; after that,
//           output frame u plays source time tau(u) (StretchMap). Each partial's region is taken
//           from frame floor(tau), its level interpolated towards the next frame, and the whole
//           region rotated so that its peak continues the phase the partial had in the previous
//           output frame (phase propagation with identity phase locking, Laroche & Dolson 1999),
//           also at the frame centre, since the peak changes bin as the partial wanders.
//           Shortened: the note plays as recorded up to its new end, fading out over the
//           Params::cutFadeSeconds before it (raised cosine, not into the attack), and nothing
//           of it after.
// Only a note's own share moves. Unpitched sound (a hammer, a pick, breath) is not assigned to
// notes and stays where it was in the remainder of the recording. (The experimental
// NoteEdit::takeBackground gives an edited note a part of everything no note owns; see
// BackgroundShares.)
// Notes without edits are never touched, so an unedited render is the analysis/resynthesis
// identity (perfect reconstruction of the weighted overlap-add); frames no edit reaches skip the
// transforms and add their windowed input directly. Memory is O(fftSize) per channel.

#include "PolyInternal.h"

#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace polyphonic
{

float Note::medianMidi() const
{
    if (f0.empty())
        return 0.0f;
    std::vector<float> v(f0);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    float m = v[v.size() / 2];
    if (v.size() % 2 == 0)
        m = 0.5f * (m + *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2)));
    return 69.0f + 12.0f * std::log2(m / 440.0f);
}

float Note::f0At(int frame) const
{
    if (f0.empty())
        return 0.0f;
    const int i = std::clamp(frame - startFrame, 0, static_cast<int>(f0.size()) - 1);
    return f0[static_cast<size_t>(i)];
}

namespace
{
using detail::kPi;
using detail::StftFrames;
using Complex = std::complex<float>;

/** Smoothed log-amplitude envelope over log-frequency from a note's partial amplitudes. */
struct Envelope
{
    std::vector<double> f, a;
    bool valid() const { return f.size() >= 2; }
    double at(double logF) const
    {
        if (logF <= f.front()) return a.front();
        if (logF >= f.back()) return a.back();
        const size_t j = static_cast<size_t>(std::upper_bound(f.begin(), f.end(), logF) - f.begin());
        const double u = (logF - f[j - 1]) / (f[j] - f[j - 1]);
        return a[j - 1] * (1 - u) + a[j] * u;
    }
};

// Third-octave bins, then a +-1 bin moving average: a gentle envelope, not one that follows the
// odd/even pattern of individual partials (which is not a formant).
Envelope noteEnvelope(const NoteMasks& nm, double sampleRate)
{
    const double lo = std::log(40.0), hi = std::log(sampleRate / 2);
    const int nb = std::max(2, static_cast<int>(std::ceil((hi - lo) / (std::log(2.0) / 3.0))));
    std::vector<double> acc(static_cast<size_t>(nb), 0.0), cnt(static_cast<size_t>(nb), 0.0);
    for (const auto& pm : nm.partials)
    {
        const double lf = std::log(std::max(1.0f, pm.freqHz));
        const int i = std::clamp(static_cast<int>(std::floor((lf - lo) / (hi - lo) * nb)), 0, nb - 1);
        acc[static_cast<size_t>(i)] += std::log(pm.amplitude + 1e-12);
        cnt[static_cast<size_t>(i)] += 1.0;
    }
    Envelope e;
    for (int i = 0; i < nb; ++i)
        if (cnt[static_cast<size_t>(i)] > 0)
        {
            e.f.push_back(lo + (hi - lo) * (i + 0.5) / nb);
            e.a.push_back(acc[static_cast<size_t>(i)] / cnt[static_cast<size_t>(i)]);
        }
    if (e.a.size() >= 3)
    {
        std::vector<double> s(e.a.size());
        for (size_t i = 0; i < e.a.size(); ++i)
            s[i] = (e.a[i == 0 ? 0 : i - 1] + e.a[i] + e.a[std::min(e.a.size() - 1, i + 1)]) / 3.0;
        e.a = std::move(s);
    }
    return e;
}

/** Range of a note's partial masks at frame t. */
std::pair<size_t, size_t> partialsAt(const NoteMasks& nm, int t)
{
    auto lo = std::lower_bound(nm.partials.begin(), nm.partials.end(), t, [](const PartialMask& pm, int f) { return pm.frame < f; });
    auto hi = std::upper_bound(lo, nm.partials.end(), t, [](int f, const PartialMask& pm) { return f < pm.frame; });
    return {static_cast<size_t>(lo - nm.partials.begin()), static_cast<size_t>(hi - nm.partials.begin())};
}

/** Input spectra with a small cache (moved notes read frames other than the one being written). */
class SourceFrames
{
public:
    SourceFrames(const std::vector<std::vector<float>>& channels, const Params& p)
    {
        for (const auto& ch : channels)
            src.push_back(std::make_unique<StftFrames>(ch.data(), static_cast<int>(ch.size()), p));
        bins = src.empty() ? 0 : src[0]->numBins();
    }
    const Complex* get(int c, int t)
    {
        for (auto& e : cache)
            if (e.channel == c && e.frame == t)
                return e.data.data();
        Entry& e = cache[next++ % cache.size()];
        e.channel = c;
        e.frame = t;
        e.data.resize(static_cast<size_t>(bins));
        src[static_cast<size_t>(c)]->spectrum(t, e.data.data());
        return e.data.data();
    }

private:
    struct Entry { int channel = -1, frame = -1; std::vector<Complex> data; };
    std::vector<std::unique_ptr<StftFrames>> src;
    std::vector<Entry> cache = std::vector<Entry>(24);
    size_t next = 0;
    int bins = 0;
};

struct EditState
{
    int note = 0;
    const NoteEdit* edit = nullptr;
    float gain = 1.0f;
    int D = 0;                 // whole-hop move
    double delta = 0.0;        // remaining fractional move, samples
    Envelope env;
    std::map<int, std::pair<int, double>> psi;   // harmonic -> (last source frame, phase)
    bool shifts = false;
    bool moves = false;
    int touchLo = 0;           // output frames this edit can change (inclusive, conservative)
    int touchHi = -1;
    // stretching (lengthened) or cutting (shortened)
    StretchMap map;
    bool stretches = false;
    bool cuts = false;
    double fadeFrames = 1.0;
    int maskHi = -1;           // last frame of the note's masks
    struct Track { int frame = -1; double phase = 0.0; };
    std::map<std::pair<int, int>, Track> track;  // (harmonic, channel) -> output frame and peak phase
    // NoteEdit::takeBackground: frames of its claim on what no note owns (none when bgHi < bgLo)
    int bgLo = 0, bgHi = -1;
};

/** Index of the strongest bin of a partial's share in one frame. */
int peakBin(const NoteMasks& nm, const PartialMask& pm, const std::complex<float>* x)
{
    int best = 0;
    float bestValue = -1.0f;
    for (int k = 0; k < pm.count; ++k)
    {
        const float v = nm.weights[static_cast<size_t>(pm.weightOffset + k)] * std::abs(x[pm.lo + k]);
        if (v > bestValue)
        {
            bestValue = v;
            best = k;
        }
    }
    return best;
}

/**
 * Frames start N/2 samples before their centre, so a sinusoid's phase at the centre is arg X[k] +
 * pi k: moving a region by an odd number of bins flips its sign unless it is flipped back.
 */
inline float binShiftSign(int shift) { return (shift & 1) != 0 ? -1.0f : 1.0f; }

/**
 * Integer bin relocation plus continuous phase advance puts a stationary partial at the exact
 * target frequency, but the residual fractional-bin offset attenuates its carrier in Hann
 * analysis/synthesis overlap-add. The normalized Fourier transform of the centred Hann squared
 * window is G(a) = 4 sinc(a) / ((1-a*a)(4-a*a)); compensate its gain for |a| <= 0.5.
 * This is exact for a complete stationary partial away from edges and approximate for masked
 * or changing partials. Regions narrower than a full Hann main lobe are left alone. Keeping
 * the original bin region avoids interpolation truncation spurs.
 */
float fractionalShiftGain(double bins, int count)
{
    if (count < 3)
        return 1.0f;
    const double a = bins - std::nearbyint(bins);
    if (std::fabs(a) < 1e-8)
        return 1.0f;
    const double a2 = a * a;
    return static_cast<float>((kPi * a / std::sin(kPi * a)) * (1.0 - a2) * (1.0 - 0.25 * a2));
}

/** Frame range [lo, hi] of a note's masks (empty when hi < lo). */
std::pair<int, int> noteFrameRange(const Analysis& A, int note)
{
    const auto& nm = A.masks[static_cast<size_t>(note)];
    const int lo = nm.partials.empty() ? 0 : nm.partials.front().frame;
    const int hi = nm.partials.empty() ? -1 : nm.partials.back().frame;
    return {lo, hi};
}

constexpr double kClaimLeadSeconds = 0.05;   // a claim on what no note owns fades in before the onset
constexpr double kClaimTailSeconds = 2.0;    // and lasts this long after the last frame (fading over the second half)

/** Frames [lo, hi] in which `note` may have a claim on what no note owns (empty when hi < lo). */
std::pair<int, int> claimSpan(const Analysis& A, int note, int numFrames)
{
    if (note < 0 || note >= static_cast<int>(std::min(A.notes.size(), A.masks.size())))
        return {0, -1};
    const auto& nm = A.masks[static_cast<size_t>(note)];
    if (nm.partials.empty())
        return {0, -1};
    const Params& p = A.params;
    const double framesPerSecond = p.sampleRate / p.hop;
    const int lead = std::max(1, static_cast<int>(std::lround(kClaimLeadSeconds * framesPerSecond)));
    const int tail = std::max(2, static_cast<int>(std::lround(kClaimTailSeconds * framesPerSecond)));
    const int leadEnd = std::min(nm.partials.front().frame, A.notes[static_cast<size_t>(note)].startFrame - p.preFrames);
    return {std::max(0, leadEnd - lead), std::min(numFrames - 1, nm.partials.back().frame + tail)};
}

/**
 * Experimental (NoteEdit::takeBackground): what no note owns, the input minus every note's share,
 * split among the notes sounding in a frame so that an edited note can take its part along.
 *
 * A note's claim on a bin is its level in that frame times the shape of its spectrum: its partials
 * widened to fill the gaps between them, normalised to 1 at the strongest, with a small floor
 * elsewhere (so that what lies far from every note is shared by level). In its attack a note also
 * takes what is new: the part of each bin by which the input has grown since just before the note
 * (its onset noise, its first partials), while what was sounding already stays with the notes that
 * own it. Notes starting together share what is new in proportion to their own spectra in that
 * frame (each partial's amplitude over a few bins, plus a floor in proportion to the note's
 * level), so a note keeps the onset sound around its partials even beside a louder note, and what
 * lies far from every partial is shared by level. After its last frame the claim keeps that
 * frame's shape and decays; it fades in over a short lead-in before the onset and out over the
 * second half of the tail. Frames must be prepared in increasing order.
 */
class BackgroundShares
{
public:
    using Spectrum = std::function<const Complex*(int channel, int frame)>;

    BackgroundShares(const Analysis& A, int numFrames, int numChannels, Spectrum spectrumOf)
        : analysis(A), K(A.params.fftSize / 2 + 1), binHz(A.params.sampleRate / A.params.fftSize),
          channels(numChannels), spectrum(std::move(spectrumOf))
    {
        const Params& p = A.params;
        const double framesPerSecond = p.sampleRate / p.hop;
        const int attack = static_cast<int>(std::lround(p.stretchAttackSeconds * framesPerSecond));
        const int tail = std::max(2, static_cast<int>(std::lround(kClaimTailSeconds * framesPerSecond)));
        decayFrames = kTailDecaySeconds * framesPerSecond;
        ownerOf.assign(A.notes.size(), -1);
        for (int n = 0; n < static_cast<int>(std::min(A.notes.size(), A.masks.size())); ++n)
        {
            const auto& nm = A.masks[static_cast<size_t>(n)];
            if (nm.partials.empty())
                continue;
            Owner o;
            o.note = n;
            o.maskLo = nm.partials.front().frame;
            o.maskHi = nm.partials.back().frame;
            o.leadEnd = std::min(o.maskLo, A.notes[static_cast<size_t>(n)].startFrame - p.preFrames);
            std::tie(o.lo, o.hi) = claimSpan(A, n, numFrames);
            o.attackHi = A.notes[static_cast<size_t>(n)].startFrame + attack;
            o.fadeLo = o.maskHi + tail / 2;
            int frame = nm.partials.front().frame;
            double level = 0.0;
            for (const auto& pm : nm.partials)
            {
                if (pm.frame != frame)
                {
                    o.peak = std::max(o.peak, level);
                    frame = pm.frame;
                    level = 0.0;
                }
                level += pm.amplitude;
            }
            o.peak = std::max(o.peak, level);
            o.last = level;
            if (o.hi < o.lo || o.peak <= 0.0)
                continue;
            ownerOf[static_cast<size_t>(n)] = static_cast<int>(owners.size());
            owners.push_back(o);
        }
        byStart.resize(owners.size());
        std::iota(byStart.begin(), byStart.end(), 0);
        std::sort(byStart.begin(), byStart.end(), [&](int a, int b) { return owners[static_cast<size_t>(a)].lo < owners[static_cast<size_t>(b)].lo; });
        for (auto* v : { &rest, &total, &shape, &onset, &current, &fresh, &freshTotal, &scratch })
            v->resize(static_cast<size_t>(K));
    }

    /** Frames [lo, hi] in which `note` has a claim (empty when hi < lo). */
    std::pair<int, int> span(int note) const
    {
        const int o = note >= 0 && note < static_cast<int>(ownerOf.size()) ? ownerOf[static_cast<size_t>(note)] : -1;
        return o < 0 ? std::pair<int, int>{0, -1} : std::pair<int, int>{owners[static_cast<size_t>(o)].lo, owners[static_cast<size_t>(o)].hi};
    }

    /**
     * `note`'s part of what no note owns at frame u of `channel`, written to out[0..K); false (and
     * out untouched) when it has none there.
     */
    bool part(int note, int u, int channel, Complex* out)
    {
        const int o = note >= 0 && note < static_cast<int>(ownerOf.size()) ? ownerOf[static_cast<size_t>(note)] : -1;
        if (o < 0)
            return false;
        prepare(u);
        const auto it = std::find(present.begin(), present.end(), o);
        if (it == present.end())
            return false;
        const Complex* x = spectrum(channel, u);   // after prepare(), which reads other frames
        const size_t j = static_cast<size_t>(it - present.begin());
        const auto& c = claims[j];
        const auto& n = newClaims[j];
        for (int k = 0; k < K; ++k)
        {
            const size_t q = static_cast<size_t>(k);
            float share = total[q] > 0.0f ? (1.0f - fresh[q]) * c[q] / total[q] : 0.0f;
            if (!n.empty() && freshTotal[q] > 0.0f)
                share += fresh[q] * n[q] / freshTotal[q];
            out[k] = (coverage * rest[q] * share) * x[k];
        }
        return true;
    }

private:
    static constexpr double kTailDecaySeconds = 0.3;   // decay of the claim's level in the tail
    static constexpr float kFloor = 0.05f;             // claim far from the note's partials
    static constexpr double kMinWidthBins = 2.0;       // narrowest widening of a partial
    static constexpr double kFillWidth = 0.5;          // claim: partials widened by this much of their spacing
    static constexpr double kOnsetWidth = 0.05;        // onset: partials widened by this much of their spacing
    static constexpr double kOnsetFloor = 0.1;         // onset: claim far from the partials, relative to the level
    static constexpr int kSmoothBins = 2;              // magnitudes compared over +-2 bins

    struct Owner
    {
        int note = 0;
        int lo = 0, hi = -1;        // frames with a claim
        int maskLo = 0, maskHi = -1;
        int leadEnd = 0;            // the claim is complete from here on
        int attackHi = 0;           // last frame of the attack (claims what is new)
        int fadeLo = 0;             // the claim fades out after this frame
        double peak = 0.0;          // largest frame level (sum of partial amplitudes)
        double last = 0.0;          // level at the last mask frame
        std::vector<float> before;  // input magnitude just before the claim starts (while in the attack)
    };

    /** dst: the input magnitude at frame t, summed over channels and smoothed over +-kSmoothBins. */
    void magnitude(int t, std::vector<float>& dst)
    {
        std::fill(scratch.begin(), scratch.end(), 0.0f);
        if (t >= 0)
            for (int c = 0; c < channels; ++c)
            {
                const Complex* x = spectrum(c, t);
                for (int k = 0; k < K; ++k)
                    scratch[static_cast<size_t>(k)] += std::abs(x[k]);
            }
        dst.assign(static_cast<size_t>(K), 0.0f);
        double run = 0.0;
        int count = 0;
        for (int k = -kSmoothBins; k < K; ++k)
        {
            if (k + kSmoothBins < K)
            {
                run += scratch[static_cast<size_t>(k + kSmoothBins)];
                ++count;
            }
            if (k - kSmoothBins - 1 >= 0)
            {
                run -= scratch[static_cast<size_t>(k - kSmoothBins - 1)];
                --count;
            }
            if (k >= 0)
                dst[static_cast<size_t>(k)] = static_cast<float>(run / std::max(1, count));
        }
    }

    void prepare(int u)
    {
        if (u == preparedFrame)
            return;
        preparedFrame = u;
        while (nextStart < byStart.size() && owners[static_cast<size_t>(byStart[nextStart])].lo <= u)
            present.push_back(byStart[nextStart++]);
        present.erase(std::remove_if(present.begin(), present.end(), [&](int o) { return owners[static_cast<size_t>(o)].hi < u; }),
                      present.end());
        claims.resize(present.size());
        newClaims.resize(present.size());
        std::fill(rest.begin(), rest.end(), 1.0f);
        std::fill(total.begin(), total.end(), 0.0f);
        std::fill(fresh.begin(), fresh.end(), 0.0f);
        std::fill(freshTotal.begin(), freshTotal.end(), 0.0f);
        coverage = 0.0f;
        bool haveCurrent = false;
        for (size_t j = 0; j < present.size(); ++j)
        {
            Owner& o = owners[static_cast<size_t>(present[j])];
            const auto& nm = analysis.masks[static_cast<size_t>(o.note)];
            if (u >= o.maskLo && u <= o.maskHi)
            {
                const auto [b, e] = partialsAt(nm, u);
                for (size_t q = b; q < e; ++q)
                {
                    const auto& pm = nm.partials[q];
                    for (int k = 0; k < pm.count; ++k)
                        rest[static_cast<size_t>(pm.lo + k)] -= nm.weights[static_cast<size_t>(pm.weightOffset + k)];
                }
            }
            // the claim fades in over the lead-in and out at the end of the tail; where the strongest
            // claim is faded, part of what no note owns stays (coverage), so the edges are not gated
            double fade = 1.0;
            if (u < o.leadEnd)
                fade = (u - o.lo + 1.0) / (o.leadEnd - o.lo + 1.0);
            if (u > o.fadeLo)
                fade *= std::max(0.0, 1.0 - (u - o.fadeLo) / static_cast<double>(o.hi - o.fadeLo + 1));
            coverage = std::max(coverage, static_cast<float>(fade));
            // its share of what was sounding: level times the shape of its spectrum
            const int at = std::clamp(u, o.maskLo, o.maskHi);
            double atLevel = 0.0;   // level at `at` (before the mask starts: its first frame)
            {
                const auto [b, e] = partialsAt(nm, at);
                for (size_t q = b; q < e; ++q)
                    atLevel += nm.partials[q].amplitude;
            }
            double level = u >= o.maskLo ? atLevel : 0.0;
            if (u > o.maskHi)
                level = o.last * std::exp(-(u - o.maskHi) / decayFrames);
            level = fade * std::max(level, 1e-3 * o.peak);
            spectrumShape(nm, u >= o.maskLo ? at : -1);
            auto& c = claims[j];
            c.resize(static_cast<size_t>(K));
            for (int k = 0; k < K; ++k)
            {
                c[static_cast<size_t>(k)] = static_cast<float>(level) * shape[static_cast<size_t>(k)];
                total[static_cast<size_t>(k)] += c[static_cast<size_t>(k)];
            }
            // in its attack, what is new since just before the note, in proportion to the note's own
            // spectrum in this frame (its partials over a few bins, plus a floor in proportion to its
            // level), so notes starting together each keep the onset sound around their partials
            auto& n = newClaims[j];
            if (u > o.attackHi)
            {
                n.clear();
                o.before.clear();
                o.before.shrink_to_fit();
                continue;
            }
            if (o.before.empty())
                magnitude(o.lo - 1, o.before);
            if (!haveCurrent)
            {
                magnitude(u, current);
                haveCurrent = true;
            }
            partialSpectrum(nm, at, kOnsetWidth, onset);
            const double onsetFloor = kOnsetFloor * std::max(atLevel, 1e-3 * o.peak);
            n.resize(static_cast<size_t>(K));
            for (int k = 0; k < K; ++k)
            {
                const size_t q = static_cast<size_t>(k);
                const float grown = current[q] > 0.0f ? std::clamp(1.0f - o.before[q] / current[q], 0.0f, 1.0f) : 0.0f;
                n[q] = static_cast<float>(fade * (onset[q] + onsetFloor)) * grown;
                fresh[q] = std::max(fresh[q], static_cast<float>(fade) * grown);
                freshTotal[q] += n[q];
            }
        }
        for (auto& r : rest)
            r = std::clamp(r, 0.0f, 1.0f);
    }

    /**
     * out: the note's partials at frame t (none when t < 0), each its amplitude times a Gaussian
     * over max(kMinWidthBins, widthFraction x the spacing of its harmonics) bins.
     */
    void partialSpectrum(const NoteMasks& nm, int t, double widthFraction, std::vector<float>& out) const
    {
        std::fill(out.begin(), out.end(), 0.0f);
        if (t < 0)
            return;
        const auto [b, e] = partialsAt(nm, t);
        for (size_t q = b; q < e; ++q)
        {
            const auto& pm = nm.partials[q];
            const double kc = pm.freqHz / binHz;
            const double width = std::max(kMinWidthBins, widthFraction * pm.freqHz / std::max(1, pm.harmonic) / binHz);
            const int k0 = std::max(1, static_cast<int>(std::floor(kc - 3.0 * width)));
            const int k1 = std::min(K - 2, static_cast<int>(std::ceil(kc + 3.0 * width)));
            for (int k = k0; k <= k1; ++k)
            {
                const double d = (k - kc) / width;
                out[static_cast<size_t>(k)] += static_cast<float>(pm.amplitude * std::exp(-0.5 * d * d));
            }
        }
    }

    /** shape: the note's partials at frame t widened to fill the gaps between them, peak 1, floor kFloor. */
    void spectrumShape(const NoteMasks& nm, int t)
    {
        partialSpectrum(nm, t, kFillWidth, shape);
        const float peak = *std::max_element(shape.begin(), shape.end());
        for (auto& s : shape)
            s = std::max(kFloor, peak > 0.0f ? s / peak : 0.0f);
    }

    const Analysis& analysis;
    const int K;
    const double binHz;
    const int channels;
    Spectrum spectrum;
    double decayFrames = 1.0;
    std::vector<Owner> owners;
    std::vector<int> ownerOf;          // note -> owner index (-1: no masks)
    std::vector<int> byStart;          // owners by first claimed frame
    size_t nextStart = 0;
    std::vector<int> present;          // owners with a claim at the prepared frame
    int preparedFrame = std::numeric_limits<int>::min();
    std::vector<std::vector<float>> claims, newClaims;
    std::vector<float> rest, total, shape, onset, current, fresh, freshTotal, scratch;
    float coverage = 0.0f;             // the strongest (least faded) claim at the prepared frame
};

/**
 * Core synthesis. With a non-empty `solo` the output is those notes only: each as its edit in
 * `edits` makes it (transformed exactly as in a full render), or its separated signal when it has
 * no edit. Edits of notes outside `solo` are ignored.
 *
 * Frames that no edit can reach are not transformed at all: their windowed input goes straight
 * into the overlap-add, which is exactly what the FFT round trip would give. Rendering a few
 * edits therefore costs time in proportion to the edited notes, not to the recording.
 *
 * The output covers samples [fromSample, toSample) (toSample < 0: to the end). Only the frames
 * that overlap that range are processed, and they are processed in order, so the samples in it
 * are exactly those of a whole render.
 */
std::vector<std::vector<float>> synthesize(const Analysis& A, const std::vector<std::vector<float>>& channels,
                                           const std::map<int, NoteEdit>& edits, const std::vector<int>& solo,
                                           const std::atomic<bool>* cancel, int fromSample = 0, int toSample = -1,
                                           bool soloBackground = false)
{
    const Params& p = A.params;
    const int numCh = static_cast<int>(channels.size());
    if (numCh == 0)
        return {};
    const int numSamples = static_cast<int>(channels[0].size());
    const int N = p.fftSize, H = p.hop, pad = N / 2;
    const int T = 1 + (numSamples + H - 1) / H;
    const int from = std::clamp(fromSample, 0, numSamples);
    const int to = toSample < 0 ? numSamples : std::clamp(toSample, from, numSamples);
    // frame u overlaps samples [u H - pad, u H - pad + N)
    const auto floorDiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const int uFirst = std::max(0, floorDiv(from + pad - N, H) + 1);
    const int uLast = std::min(T - 1, floorDiv(to + pad - 1, H));
    const int K = N / 2 + 1;
    const double binHz = p.sampleRate / p.fftSize;
    SourceFrames source(channels, p);
    const auto window = detail::hannPeriodic(N);
    const float scale = std::accumulate(window.begin(), window.end(), 0.0f) / 2.0f;
    RealFFT fft(N);

    // what no note owns, split among the notes (NoteEdit::takeBackground): one splitter per offset
    // between the output frame and the frame a note's part is taken from (a moved note's copy comes
    // from D frames earlier), since each splitter goes through its frames in increasing order
    std::map<int, std::unique_ptr<BackgroundShares>> background;
    auto backgroundAt = [&](int offset) -> BackgroundShares& {
        auto& b = background[offset];
        if (b == nullptr)
            b = std::make_unique<BackgroundShares>(A, T, numCh, [&source](int c, int t) { return source.get(c, t); });
        return *b;
    };
    std::vector<Complex> part(static_cast<size_t>(K));

    std::vector<EditState> states;
    for (const auto& [ni, ed] : edits)
    {
        if (ni < 0 || ni >= static_cast<int>(A.notes.size()) || ni >= static_cast<int>(A.masks.size()) || ed.isIdentity())
            continue;
        EditState s;
        s.note = ni;
        s.edit = &ed;
        s.gain = std::pow(10.0f, ed.gainDb / 20.0f);
        s.shifts = ed.semitones != 0.0f;
        for (float v : ed.pitchCurve)
            s.shifts = s.shifts || v != 0.0f;
        if (ed.preserveFormants && s.shifts)
            s.env = noteEnvelope(A.masks[static_cast<size_t>(ni)], p.sampleRate);
        const double offset = ed.timeOffsetSeconds * p.sampleRate;
        s.D = static_cast<int>(std::lround(offset / H));
        s.delta = offset - static_cast<double>(s.D) * H;
        s.moves = offset != 0.0;
        s.map = stretchMap(A, ni, ed);
        s.stretches = s.map.ratio > 1.0;
        s.cuts = s.map.cuts();
        // the fade ends at the cut and never reaches into the attack
        s.fadeFrames = std::max(1.0, std::min(static_cast<double>(p.cutFadeSeconds) * p.sampleRate / H, s.map.end - s.map.anchor));
        // removal touches the note's own frames; the transformed copy lands D frames later (and,
        // when stretched, ends where the time map puts the note's last frame)
        const auto [lo, hi] = noteFrameRange(A, ni);
        s.maskHi = hi;
        s.touchLo = lo;
        s.touchHi = hi;
        if (!ed.mute && hi >= lo)
        {
            const int outHi = s.stretches ? static_cast<int>(std::ceil(s.map.outputFrame(hi))) : hi;
            s.touchLo = std::min(lo, lo + s.D);
            s.touchHi = std::max(hi, outHi + s.D);
        }
        // with its background, the note's part of what no note owns goes too: removed over its
        // claim and, unless muted, added back D frames later (up to its new end when shortened; not
        // for a lengthened note)
        if (ed.takeBackground && (ed.mute || !s.stretches))
        {
            const auto [blo, bhi] = backgroundAt(0).span(ni);
            if (bhi >= blo)
            {
                s.bgLo = blo;
                s.bgHi = bhi;
                s.touchLo = std::min({s.touchLo, blo, ed.mute ? blo : blo + s.D});
                s.touchHi = std::max({s.touchHi, bhi, ed.mute ? bhi : bhi + s.D});
            }
        }
        states.push_back(std::move(s));
    }
    std::vector<int> soloNotes;
    for (int ni : solo)
        if (ni >= 0 && ni < static_cast<int>(A.masks.size()))
            soloNotes.push_back(ni);
    const bool soloMode = !solo.empty();
    // On their own, only the listed notes' edits apply: an edited note contributes its transformed
    // copy (and nothing is removed), an unedited one its separated share, everything else silence.
    std::vector<char> soloEdited(A.masks.size(), 0);
    std::vector<std::pair<int, int>> soloClaims(soloNotes.size(), {0, -1});   // unedited notes taking their background
    if (soloMode)
    {
        states.erase(std::remove_if(states.begin(), states.end(),
                                    [&](const EditState& s) {
                                        return std::find(soloNotes.begin(), soloNotes.end(), s.note) == soloNotes.end();
                                    }),
                     states.end());
        for (const auto& s : states)
            soloEdited[static_cast<size_t>(s.note)] = 1;
        for (size_t j = 0; j < soloNotes.size(); ++j)
        {
            const auto entry = edits.find(soloNotes[j]);
            if (!soloEdited[static_cast<size_t>(soloNotes[j])] && (soloBackground || (entry != edits.end() && entry->second.takeBackground)))
                soloClaims[j] = backgroundAt(0).span(soloNotes[j]);
        }
    }
    auto untouched = [&](int u) {
        for (const auto& st : states)
            if (u >= st.touchLo && u <= st.touchHi)
                return false;
        return true;
    };

    std::vector<std::vector<Complex>> Y(static_cast<size_t>(numCh), std::vector<Complex>(static_cast<size_t>(K)));
    std::vector<std::vector<double>> acc(static_cast<size_t>(numCh), std::vector<double>(static_cast<size_t>(N), 0.0));
    std::vector<double> nrm(static_cast<size_t>(N), 0.0);
    std::vector<std::vector<float>> out(static_cast<size_t>(numCh), std::vector<float>(static_cast<size_t>(to - from), 0.0f));
    std::vector<float> frame(static_cast<size_t>(N));
    std::vector<Complex> spec(static_cast<size_t>(K));

    auto fracDelay = [&](double delta, int k) {
        return delta == 0.0 ? Complex(1.0f, 0.0f) : std::polar(1.0f, static_cast<float>(-2.0 * kPi * k * delta / N));
    };
    auto finalize = [&](std::ptrdiff_t q0, std::ptrdiff_t q1) {             // padded sample range [q0, q1) is complete
        for (std::ptrdiff_t q = q0; q < q1; ++q)
        {
            const size_t r = static_cast<size_t>(q % N);
            const std::ptrdiff_t i = q - pad;
            if (i >= from && i < to)
                for (int c = 0; c < numCh; ++c)
                    out[static_cast<size_t>(c)][static_cast<size_t>(i - from)] =
                        static_cast<float>(acc[static_cast<size_t>(c)][r] / std::max(nrm[r], 1e-8));
            for (int c = 0; c < numCh; ++c)
                acc[static_cast<size_t>(c)][r] = 0.0;
            nrm[r] = 0.0;
        }
    };

    std::vector<double> window2(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        window2[static_cast<size_t>(i)] = static_cast<double>(window[static_cast<size_t>(i)]) * window[static_cast<size_t>(i)];

    for (int u = uFirst; u <= uLast; ++u)
    {
        if (cancel && cancel->load())
            return {};
        bool transform = true;
        if (soloMode)
        {
            transform = false;
            for (auto& y : Y)
                std::fill(y.begin(), y.end(), Complex{});
            for (const auto& st : states)
                transform = transform || (u >= st.touchLo && u <= st.touchHi);
            for (size_t j = 0; j < soloNotes.size(); ++j)
            {
                const int ni = soloNotes[j];
                if (soloEdited[static_cast<size_t>(ni)])
                    continue;                           // added below as its edited copy
                // its part of what no note owns, when it takes its background
                if (u >= soloClaims[j].first && u <= soloClaims[j].second)
                {
                    transform = true;
                    for (int c = 0; c < numCh; ++c)
                        if (backgroundAt(0).part(ni, u, c, part.data()))
                        {
                            auto& y = Y[static_cast<size_t>(c)];
                            for (int k = 0; k < K; ++k)
                                y[static_cast<size_t>(k)] += part[static_cast<size_t>(k)];
                        }
                }
                const auto& nm = A.masks[static_cast<size_t>(ni)];
                const auto [b, e] = partialsAt(nm, u);
                if (b == e)
                    continue;
                transform = true;
                for (int c = 0; c < numCh; ++c)
                {
                    const Complex* x = source.get(c, u);
                    auto& y = Y[static_cast<size_t>(c)];
                    for (size_t q = b; q < e; ++q)
                    {
                        const auto& pm = nm.partials[q];
                        for (int k = 0; k < pm.count; ++k)
                            y[static_cast<size_t>(pm.lo + k)] += nm.weights[static_cast<size_t>(pm.weightOffset + k)] * x[pm.lo + k];
                    }
                }
            }
        }
        else if (untouched(u))
        {
            // identity frame: the windowed input itself (what IFFT(FFT(x w)) w would return)
            transform = false;
            const std::ptrdiff_t first = static_cast<std::ptrdiff_t>(u) * H - pad;
            for (int c = 0; c < numCh; ++c)
            {
                const auto& x = channels[static_cast<size_t>(c)];
                auto& a = acc[static_cast<size_t>(c)];
                for (int i = 0; i < N; ++i)
                {
                    const std::ptrdiff_t si = first + i;
                    if (si >= 0 && si < numSamples)
                        a[static_cast<size_t>((static_cast<std::ptrdiff_t>(u) * H + i) % N)] +=
                            static_cast<double>(x[static_cast<size_t>(si)]) * window2[static_cast<size_t>(i)];
                }
            }
        }
        else
        {
            for (int c = 0; c < numCh; ++c)
                std::copy(source.get(c, u), source.get(c, u) + K, Y[static_cast<size_t>(c)].begin());
        }
        const size_t editsHere = transform ? states.size() : 0;
        for (size_t sIndex = 0; sIndex < editsHere; ++sIndex)
        {
            auto& st = states[sIndex];
            const Note& n = A.notes[static_cast<size_t>(st.note)];
            const NoteMasks& nm = A.masks[static_cast<size_t>(st.note)];
            const NoteEdit& ed = *st.edit;
            // 1) remove the note's own share at this frame (on its own there is nothing to remove)
            if (!soloMode)
            {
                const auto [b, e] = partialsAt(nm, u);
                for (int c = 0; c < numCh; ++c)
                {
                    const Complex* x = source.get(c, u);
                    auto& y = Y[static_cast<size_t>(c)];
                    for (size_t q = b; q < e; ++q)
                    {
                        const auto& pm = nm.partials[q];
                        for (int k = 0; k < pm.count; ++k)
                            y[static_cast<size_t>(pm.lo + k)] -= nm.weights[static_cast<size_t>(pm.weightOffset + k)] * x[pm.lo + k];
                    }
                }
            }
            // and its part of what no note owns, when it takes its background
            if (!soloMode && st.bgHi >= st.bgLo && u >= st.bgLo && u <= st.bgHi)
                for (int c = 0; c < numCh; ++c)
                    if (backgroundAt(0).part(st.note, u, c, part.data()))
                    {
                        auto& y = Y[static_cast<size_t>(c)];
                        for (int k = 0; k < K; ++k)
                            y[static_cast<size_t>(k)] -= part[static_cast<size_t>(k)];
                    }
            if (ed.mute)
                continue;
            const int uo = u - st.D;                    // output frame before the move
            if (st.stretches && uo > st.map.anchor)
            {
                // 2b) stretched part: source time tau, partial regions from frame floor(tau)
                const double tau = st.map.sourceFrame(uo);
                if (tau > st.maskHi || tau < 0.0)
                    continue;
                const int a = static_cast<int>(std::floor(tau));
                const double mu = tau - a;
                const int bFrame = std::min(a + 1, st.maskHi);
                const auto [ba, ea] = partialsAt(nm, a);
                const auto [bb, eb] = partialsAt(nm, bFrame);
                for (size_t q = ba; q < ea; ++q)
                {
                    const auto& pa = nm.partials[q];
                    const PartialMask* pb = nullptr;
                    for (size_t j = bb; j < eb && pb == nullptr; ++j)
                        if (nm.partials[j].harmonic == pa.harmonic)
                            pb = &nm.partials[j];
                    double semis = ed.semitones;
                    if (!ed.pitchCurve.empty())
                        semis += ed.pitchCurve[static_cast<size_t>(std::clamp(static_cast<int>(std::lround(tau)) - n.startFrame, 0,
                                                                              static_cast<int>(ed.pitchCurve.size()) - 1))];
                    const double r = std::pow(2.0, semis / 12.0);
                    const double f = pb != nullptr ? (1.0 - mu) * pa.freqHz + mu * pb->freqHz : pa.freqHz;
                    // level between this frame and the next (a partial that ends fades out)
                    double level = 1.0 - mu;
                    if (pb != nullptr)
                        level += mu * (pa.amplitude > 0.0f ? std::clamp(static_cast<double>(pb->amplitude / pa.amplitude), 0.0, 4.0) : 1.0);
                    const int shift = r != 1.0 ? static_cast<int>(std::nearbyint((r - 1.0) * f / binHz)) : 0;
                    float g = st.gain * static_cast<float>(level) * fractionalShiftGain((r - 1.0) * f / binHz, pa.count);
                    if (r != 1.0 && st.env.valid())
                    {
                        const double l0 = std::log(std::max(1.0, f)), l1 = std::log(std::max(1.0, f * r));
                        g *= static_cast<float>(std::exp(std::clamp(st.env.at(l1) - st.env.at(l0), -2.3, 2.3)));
                    }
                    const double advance = 2.0 * kPi * r * f * H / p.sampleRate;
                    for (int c = 0; c < numCh; ++c)
                    {
                        const Complex* x = source.get(c, a);
                        // phases at the frame centre (frames start N/2 before it, so bin k carries an
                        // extra (-1)^k); the peak may sit in another bin from one frame to the next
                        const int kp = pa.lo + peakBin(nm, pa, x);
                        const double srcPhase = std::arg(x[kp]) + kPi * kp;
                        auto& tr = st.track[{pa.harmonic, c}];
                        const double target = tr.frame == u - 1 ? tr.phase + advance : srcPhase;
                        tr.frame = u;
                        tr.phase = std::remainder(target, 2.0 * kPi);
                        const Complex rot = std::polar(1.0f, static_cast<float>(target - srcPhase)) * binShiftSign(shift);
                        auto& y = Y[static_cast<size_t>(c)];
                        for (int k = 0; k < pa.count; ++k)
                        {
                            const int kk = pa.lo + k + shift;
                            if (kk < 1 || kk > K - 2)
                                continue;
                            y[static_cast<size_t>(kk)] += g * nm.weights[static_cast<size_t>(pa.weightOffset + k)] * x[pa.lo + k] * rot * fracDelay(st.delta, kk);
                        }
                    }
                }
                continue;
            }
            // 2) add it back, transformed, from source frame s = u - D
            const int s = uo;
            if (s < 0 || s >= T)
                continue;
            // a shortened note fades out before its new end, and nothing of it plays after that
            float cutGain = 1.0f;
            if (st.cuts)
            {
                const double left = st.map.end - s;
                if (left <= 0.0)
                    continue;
                if (left < st.fadeFrames)
                    cutGain = static_cast<float>(std::pow(std::sin(0.5 * kPi * left / st.fadeFrames), 2.0));
            }
            const auto [b, e] = partialsAt(nm, s);
            for (size_t q = b; q < e; ++q)
            {
                const auto& pm = nm.partials[q];
                double semis = ed.semitones;
                if (!ed.pitchCurve.empty())
                    semis += ed.pitchCurve[static_cast<size_t>(std::clamp(s - n.startFrame, 0, static_cast<int>(ed.pitchCurve.size()) - 1))];
                const double r = std::pow(2.0, semis / 12.0);
                double phase = 0.0;
                int shift = 0;
                float g = st.gain * cutGain;
                if (st.shifts)
                {
                    // A curve returning to zero keeps the phase accumulated by its earlier
                    // pitch changes. Update the frame even at zero so a later change does not
                    // apply its frequency offset across the intervening unshifted frames.
                    const double dfh = (r - 1.0) * pm.freqHz;
                    auto it = st.psi.find(pm.harmonic);
                    const int tLast = it == st.psi.end() ? s - 1 : it->second.first;
                    const double phLast = it == st.psi.end() ? 0.0 : it->second.second;
                    phase = phLast + (s - tLast) * 2.0 * kPi * dfh * H / p.sampleRate;
                    st.psi[pm.harmonic] = {s, phase};
                    shift = static_cast<int>(std::nearbyint(dfh / binHz));
                    g *= fractionalShiftGain(dfh / binHz, pm.count);
                    if (r != 1.0 && st.env.valid())
                    {
                        const double l0 = std::log(std::max(1.0f, pm.freqHz)), l1 = std::log(std::max(1.0, pm.freqHz * r));
                        g *= static_cast<float>(std::exp(std::clamp(st.env.at(l1) - st.env.at(l0), -2.3, 2.3)));
                    }
                }
                const Complex rot = std::polar(1.0f, static_cast<float>(phase)) * binShiftSign(shift);
                for (int c = 0; c < numCh; ++c)
                {
                    const Complex* x = source.get(c, s);
                    if (st.stretches)
                    {
                        // where this partial's phase stands at the frame centre, for the stretched part
                        // to continue from
                        const int kp = pm.lo + peakBin(nm, pm, x);
                        auto& tr = st.track[{pm.harmonic, c}];
                        tr.frame = u;
                        tr.phase = std::remainder(std::arg(x[kp]) + kPi * kp + phase, 2.0 * kPi);
                    }
                    auto& y = Y[static_cast<size_t>(c)];
                    for (int k = 0; k < pm.count; ++k)
                    {
                        const int kk = pm.lo + k + shift;
                        if (kk < 1 || kk > K - 2)
                            continue;
                        y[static_cast<size_t>(kk)] += g * nm.weights[static_cast<size_t>(pm.weightOffset + k)] * x[pm.lo + k] * rot * fracDelay(st.delta, kk);
                    }
                }
            }
            // its part of what no note owns, moved with it; with a pitch change, each partial's band
            // (half way to its neighbours) moves with that partial, with its phase
            if (st.bgHi >= st.bgLo && s >= st.bgLo && s <= st.bgHi)
            {
                std::vector<int> bandShift(static_cast<size_t>(K), 0);
                std::vector<Complex> bandRot(static_cast<size_t>(K), Complex(1.0f, 0.0f));
                const int maskLo = nm.partials.front().frame;
                const auto [bq, eq] = partialsAt(nm, std::clamp(s, maskLo, st.maskHi));
                for (size_t q = bq; st.shifts && q < eq; ++q)
                {
                    const auto& pm = nm.partials[q];
                    double semis = ed.semitones;
                    if (!ed.pitchCurve.empty())
                        semis += ed.pitchCurve[static_cast<size_t>(std::clamp(s - n.startFrame, 0, static_cast<int>(ed.pitchCurve.size()) - 1))];
                    const double dfh = (std::pow(2.0, semis / 12.0) - 1.0) * pm.freqHz;
                    const double step = 2.0 * kPi * dfh * H / p.sampleRate;
                    // the partial's own phase at frame s (continued past its last frame)
                    const auto it = st.psi.find(pm.harmonic);
                    const double phase = it != st.psi.end() ? it->second.second + (s - it->second.first) * step : (s - (maskLo - 1)) * step;
                    const int shift = static_cast<int>(std::nearbyint(dfh / binHz));
                    const double kc = pm.freqHz / binHz;
                    const double kPrev = q > bq ? nm.partials[q - 1].freqHz / binHz : -1.0;
                    const double kNext = q + 1 < eq ? nm.partials[q + 1].freqHz / binHz : -1.0;
                    const double halfGap = kNext > 0.0 ? 0.5 * (kNext - kc) : (kPrev > 0.0 ? 0.5 * (kc - kPrev) : 0.5 * kc);
                    const double edgeLo = kPrev > 0.0 ? 0.5 * (kPrev + kc) : kc - halfGap;
                    const double edgeHi = kNext > 0.0 ? 0.5 * (kc + kNext) : kc + (kPrev > 0.0 ? 0.5 * (kc - kPrev) : halfGap);
                    const Complex rot = std::polar(1.0f, static_cast<float>(phase)) * binShiftSign(shift);
                    for (int k = std::max(1, static_cast<int>(std::ceil(edgeLo))); k <= std::min(K - 2, static_cast<int>(std::floor(edgeHi))); ++k)
                    {
                        bandShift[static_cast<size_t>(k)] = shift;
                        bandRot[static_cast<size_t>(k)] = rot;
                    }
                }
                for (int c = 0; c < numCh; ++c)
                {
                    if (!backgroundAt(st.D).part(st.note, s, c, part.data()))
                        continue;
                    auto& y = Y[static_cast<size_t>(c)];
                    for (int k = 1; k < K - 1; ++k)
                    {
                        const int kk = k + bandShift[static_cast<size_t>(k)];
                        if (kk < 1 || kk > K - 2)
                            continue;
                        y[static_cast<size_t>(kk)] += st.gain * cutGain * part[static_cast<size_t>(k)] * bandRot[static_cast<size_t>(k)] * fracDelay(st.delta, kk);
                    }
                }
            }
        }
        // inverse transform and weighted overlap-add
        for (int c = 0; transform && c < numCh; ++c)
        {
            auto& y = Y[static_cast<size_t>(c)];
            for (int k = 0; k < K; ++k)
                spec[static_cast<size_t>(k)] = y[static_cast<size_t>(k)] * scale;
            spec[0] = {spec[0].real(), 0.0f};
            spec[static_cast<size_t>(K - 1)] = {spec[static_cast<size_t>(K - 1)].real(), 0.0f};
            fft.inverse(spec.data(), frame.data());
            auto& a = acc[static_cast<size_t>(c)];
            for (int i = 0; i < N; ++i)
                a[static_cast<size_t>((static_cast<std::ptrdiff_t>(u) * H + i) % N)] += static_cast<double>(frame[static_cast<size_t>(i)]) * window[static_cast<size_t>(i)];
        }
        for (int i = 0; i < N; ++i)
            nrm[static_cast<size_t>((static_cast<std::ptrdiff_t>(u) * H + i) % N)] += window2[static_cast<size_t>(i)];
        finalize(static_cast<std::ptrdiff_t>(u) * H, static_cast<std::ptrdiff_t>(u + 1) * H);
    }
    finalize(static_cast<std::ptrdiff_t>(uLast + 1) * H, static_cast<std::ptrdiff_t>(uLast) * H + N);
    return out;
}
} // namespace

StretchMap stretchMap(const Analysis& A, int note, const NoteEdit& edit)
{
    StretchMap m;
    if (note < 0 || note >= static_cast<int>(A.notes.size()))
        return m;
    const Params& p = A.params;
    const double hopSeconds = p.hop / p.sampleRate;
    const Note& n = A.notes[static_cast<size_t>(note)];
    m.anchor = n.startFrame + std::max(0.0, std::round(p.stretchAttackSeconds / hopSeconds));
    m.ratio = std::clamp(static_cast<double>(edit.stretch), 0.0, 4.0);
    // shortened, the note keeps that share of its length: nothing is sped up, so the cut may fall
    // anywhere, inside the attack too
    m.end = n.startFrame + std::max(0, n.endFrame - n.startFrame) * std::min(1.0, m.ratio);
    return m;
}

std::vector<std::vector<float>> render(const Analysis& A, const std::vector<std::vector<float>>& channels,
                                       const std::map<int, NoteEdit>& edits, const std::atomic<bool>* cancel)
{
    return synthesize(A, channels, edits, {}, cancel);
}

std::vector<std::vector<float>> extractNotes(const Analysis& A, const std::vector<std::vector<float>>& channels,
                                             const std::vector<int>& noteIndices, const std::atomic<bool>* cancel,
                                             bool takeBackground)
{
    if (noteIndices.empty())
        return std::vector<std::vector<float>>(channels.size(), std::vector<float>(channels.empty() ? 0 : channels[0].size(), 0.0f));
    return synthesize(A, channels, {}, noteIndices, cancel, 0, -1, takeBackground);
}

AudioSpan renderNotes(const Analysis& A, const std::vector<std::vector<float>>& channels,
                      const std::vector<int>& noteIndices, const std::map<int, NoteEdit>& edits,
                      const std::atomic<bool>* cancel)
{
    AudioSpan span;
    span.channels.resize(channels.size());
    if (channels.empty() || channels[0].empty())
        return span;
    const Params& p = A.params;
    const int H = p.hop, pad = p.fftSize / 2;
    // the frames these notes reach: their own (with their claim on what no note owns when they take
    // their background), or where an edit puts their transformed copy
    const int numFrames = 1 + (static_cast<int>(channels[0].size()) + H - 1) / H;
    int lo = std::numeric_limits<int>::max(), hi = std::numeric_limits<int>::min();
    for (int ni : noteIndices)
    {
        if (ni < 0 || ni >= static_cast<int>(A.notes.size()) || ni >= static_cast<int>(A.masks.size()))
            continue;
        auto [a, b] = noteFrameRange(A, ni);
        if (b < a)
            continue;
        const auto it = edits.find(ni);
        const auto map = it != edits.end() ? stretchMap(A, ni, it->second) : StretchMap{};
        if (it != edits.end() && it->second.takeBackground && (it->second.mute || map.ratio <= 1.0))
        {
            const auto [ca, cb] = claimSpan(A, ni, numFrames);
            a = std::min(a, ca);
            b = std::max(b, cb);
        }
        if (it != edits.end() && !it->second.isIdentity())
        {
            const NoteEdit& ed = it->second;
            if (ed.mute)
                continue;
            if (map.ratio != 1.0)
                b = static_cast<int>(std::ceil(map.outputFrame(b)));
            const int D = static_cast<int>(std::lround(ed.timeOffsetSeconds * p.sampleRate / H));
            a += D;
            b += D;
        }
        lo = std::min(lo, a);
        hi = std::max(hi, b);
    }
    if (hi < lo)
        return span;
    const int numSamples = static_cast<int>(channels[0].size());
    const int from = std::clamp(lo * H - pad, 0, numSamples);
    const int to = std::clamp(hi * H - pad + p.fftSize, from, numSamples);
    if (to <= from)
        return span;
    span.start = from;
    span.channels = synthesize(A, channels, edits, noteIndices, cancel, from, to);
    return span;
}

std::vector<float> extractNote(const Analysis& A, const std::vector<float>& channel, int noteIndex)
{
    auto out = synthesize(A, {channel}, {}, {std::max(0, noteIndex)}, nullptr);
    return out.empty() ? std::vector<float>{} : std::move(out[0]);
}

} // namespace polyphonic
