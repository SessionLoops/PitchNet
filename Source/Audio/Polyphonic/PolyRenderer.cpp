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
//   stretch the note's first Params::stretchAttackSeconds play as recorded; after that, output frame
//           u plays source time tau(u) (StretchMap). Each partial's region is taken from frame
//           floor(tau), its level interpolated towards the next frame, and the whole region rotated
//           so that its peak continues the phase the partial had in the previous output frame
//           (phase propagation with identity phase locking, Laroche & Dolson 1999), also at the
//           frame centre, since the peak changes bin as the partial wanders.
// Only a note's own share moves. Unpitched sound (a hammer, a pick, breath) is not assigned to
// notes and stays where it was in the remainder of the recording.
// Notes without edits are never touched, so an unedited render is the analysis/resynthesis
// identity (perfect reconstruction of the weighted overlap-add); frames no edit reaches skip the
// transforms and add their windowed input directly. Memory is O(fftSize) per channel.

#include "PolyInternal.h"

#include <numeric>

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
    // stretching
    StretchMap map;
    bool stretches = false;
    int maskHi = -1;           // last frame of the note's masks
    struct Track { int frame = -1; double phase = 0.0; };
    std::map<std::pair<int, int>, Track> track;  // (harmonic, channel) -> output frame and peak phase
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

/**
 * Core synthesis. With a non-empty `solo` the output is the separated signal of those notes only
 * (edits are ignored).
 *
 * Frames that no edit can reach are not transformed at all: their windowed input goes straight
 * into the overlap-add, which is exactly what the FFT round trip would give. Rendering a few
 * edits therefore costs time in proportion to the edited notes, not to the recording.
 */
std::vector<std::vector<float>> synthesize(const Analysis& A, const std::vector<std::vector<float>>& channels,
                                           const std::map<int, NoteEdit>& edits, const std::vector<int>& solo,
                                           const std::atomic<bool>* cancel)
{
    const Params& p = A.params;
    const int numCh = static_cast<int>(channels.size());
    if (numCh == 0)
        return {};
    const int numSamples = static_cast<int>(channels[0].size());
    const int N = p.fftSize, H = p.hop, pad = N / 2;
    const int T = 1 + (numSamples + H - 1) / H;
    const int K = N / 2 + 1;
    const double binHz = p.sampleRate / p.fftSize;
    SourceFrames source(channels, p);
    const auto window = detail::hannPeriodic(N);
    const float scale = std::accumulate(window.begin(), window.end(), 0.0f) / 2.0f;
    RealFFT fft(N);

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
        s.stretches = s.map.ratio != 1.0;
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
        states.push_back(std::move(s));
    }
    std::vector<int> soloNotes;
    for (int ni : solo)
        if (ni >= 0 && ni < static_cast<int>(A.masks.size()))
            soloNotes.push_back(ni);
    const bool soloMode = !solo.empty();
    auto untouched = [&](int u) {
        for (const auto& st : states)
            if (u >= st.touchLo && u <= st.touchHi)
                return false;
        return true;
    };

    std::vector<std::vector<Complex>> Y(static_cast<size_t>(numCh), std::vector<Complex>(static_cast<size_t>(K)));
    std::vector<std::vector<double>> acc(static_cast<size_t>(numCh), std::vector<double>(static_cast<size_t>(N), 0.0));
    std::vector<double> nrm(static_cast<size_t>(N), 0.0);
    std::vector<std::vector<float>> out(static_cast<size_t>(numCh), std::vector<float>(static_cast<size_t>(numSamples), 0.0f));
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
            if (i >= 0 && i < numSamples)
                for (int c = 0; c < numCh; ++c)
                    out[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                        static_cast<float>(acc[static_cast<size_t>(c)][r] / std::max(nrm[r], 1e-8));
            for (int c = 0; c < numCh; ++c)
                acc[static_cast<size_t>(c)][r] = 0.0;
            nrm[r] = 0.0;
        }
    };

    std::vector<double> window2(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        window2[static_cast<size_t>(i)] = static_cast<double>(window[static_cast<size_t>(i)]) * window[static_cast<size_t>(i)];

    for (int u = 0; u < T; ++u)
    {
        if (cancel && cancel->load())
            return {};
        bool transform = true;
        if (soloMode)
        {
            transform = false;
            for (auto& y : Y)
                std::fill(y.begin(), y.end(), Complex{});
            for (int ni : soloNotes)
            {
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
        const size_t editsHere = (soloMode || !transform) ? 0 : states.size();
        for (size_t sIndex = 0; sIndex < editsHere; ++sIndex)
        {
            auto& st = states[sIndex];
            const Note& n = A.notes[static_cast<size_t>(st.note)];
            const NoteMasks& nm = A.masks[static_cast<size_t>(st.note)];
            const NoteEdit& ed = *st.edit;
            // 1) remove the note's own share at this frame
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
                float g = st.gain;
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
    finalize(static_cast<std::ptrdiff_t>(T) * H, static_cast<std::ptrdiff_t>(T - 1) * H + N);
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
    m.anchor = A.notes[static_cast<size_t>(note)].startFrame + std::max(0.0, std::round(p.stretchAttackSeconds / hopSeconds));
    m.ratio = std::clamp(static_cast<double>(edit.stretch), 0.25, 4.0);
    return m;
}

std::vector<std::vector<float>> render(const Analysis& A, const std::vector<std::vector<float>>& channels,
                                       const std::map<int, NoteEdit>& edits, const std::atomic<bool>* cancel)
{
    return synthesize(A, channels, edits, {}, cancel);
}

std::vector<std::vector<float>> extractNotes(const Analysis& A, const std::vector<std::vector<float>>& channels,
                                             const std::vector<int>& noteIndices, const std::atomic<bool>* cancel)
{
    if (noteIndices.empty())
        return std::vector<std::vector<float>>(channels.size(), std::vector<float>(channels.empty() ? 0 : channels[0].size(), 0.0f));
    return synthesize(A, channels, {}, noteIndices, cancel);
}

std::vector<float> extractNote(const Analysis& A, const std::vector<float>& channel, int noteIndex)
{
    auto out = synthesize(A, {channel}, {}, {std::max(0, noteIndex)}, nullptr);
    return out.empty() ? std::vector<float>{} : std::move(out[0]);
}

} // namespace polyphonic
