// Polyphonic engine - STFT frames, instantaneous frequency, spectral peaks with tonality and whitening.
// Everything here streams over frames: memory does not grow with the length of the recording
// beyond the (sparse) peak list.

#include "PolyInternal.h"

#include <numeric>
#include <type_traits>

namespace polyphonic
{
namespace detail
{

std::vector<float> hannPeriodic(int n)
{
    std::vector<float> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        w[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / n));
    return w;
}

StftFrames::StftFrames(const float* signal, int numSamples, const Params& p)
    : x(signal), n(numSamples), N(p.fftSize), H(p.hop)
{
    frames = 1 + (numSamples + H - 1) / H;
    bins = N / 2 + 1;
    window = hannPeriodic(N);
    buffer.resize(static_cast<size_t>(N));
    norm = 2.0f / std::accumulate(window.begin(), window.end(), 0.0f);
    fft = std::make_unique<RealFFT>(N);
}

void StftFrames::spectrum(int t, std::complex<float>* out)
{
    const int first = t * H - N / 2;
    for (int i = 0; i < N; ++i)
    {
        const int s = first + i;
        buffer[static_cast<size_t>(i)] = (s >= 0 && s < n) ? x[s] * window[static_cast<size_t>(i)] : 0.0f;
    }
    fft->forward(buffer.data(), out);
    for (int k = 0; k < bins; ++k)
        out[k] *= norm;
}

static inline float princarg(float a)
{
    const float twoPi = static_cast<float>(2.0 * kPi);
    a = std::fmod(a + static_cast<float>(kPi), twoPi);
    if (a < 0)
        a += twoPi;
    return a - static_cast<float>(kPi);
}

// Exact running median over `width` bins (edges replicated).
void medianRow(const float* in, int K, int width, float* out, std::vector<float>& sorted)
{
    const int half = width / 2;
    auto value = [&](int k) { return in[std::clamp(k, 0, K - 1)]; };
    sorted.clear();
    for (int k = -half; k <= half; ++k)
        sorted.push_back(value(k));
    std::sort(sorted.begin(), sorted.end());
    for (int k = 0; k < K; ++k)
    {
        out[k] = sorted[static_cast<size_t>(half)];
        if (k + 1 == K)
            break;
        const float leaving = value(k - half);
        const float entering = value(k + half + 1);
        sorted.erase(std::lower_bound(sorted.begin(), sorted.end(), leaving));
        sorted.insert(std::upper_bound(sorted.begin(), sorted.end(), entering), entering);
    }
}

int Peaks::nearest(int t, float bins) const
{
    // peaks of a frame are sorted by bin: binary search, then compare the two neighbours
    const int b = begin(t), e = end(t);
    if (b == e)
        return -1;
    const auto first = fbin.begin() + b, last = fbin.begin() + e;
    const auto it = std::lower_bound(first, last, bins);
    int j = static_cast<int>(it - fbin.begin());
    if (j >= e)
        return e - 1;
    if (j > b && std::fabs(fbin[static_cast<size_t>(j - 1)] - bins) <= std::fabs(fbin[static_cast<size_t>(j)] - bins))
        return j - 1;
    return j;
}

namespace
{
struct Band
{
    int k0;
    std::vector<float> w;
    float sum;
};

// Klapuri (2006) whitening bands: centres 229 (10^((b+1)/21.4) - 1) Hz with triangular responses.
std::vector<double> whiteningCentres(const Params& p)
{
    std::vector<double> cb;
    for (int b = 0; b < 32; ++b)
    {
        const double c = 229.0 * (std::pow(10.0, (b + 1) / 21.4) - 1.0);
        if (c < p.sampleRate / 2)
            cb.push_back(c);
    }
    return cb;
}

std::vector<Band> whiteningBands(const std::vector<double>& cb, const Params& p, int K)
{
    const double binHz = p.sampleRate / p.fftSize;
    std::vector<Band> bands;
    for (size_t b = 0; b < cb.size(); ++b)
    {
        const double lo = b > 0 ? cb[b - 1] : 0.0;
        const double hi = b + 1 < cb.size() ? cb[b + 1] : p.sampleRate / 2;
        Band band{std::max(0, static_cast<int>(std::floor(lo / binHz))), {}, 0.0f};
        const int k1 = std::min(K - 1, static_cast<int>(std::ceil(hi / binHz)));
        for (int k = band.k0; k <= k1; ++k)
        {
            const double f = k * binHz;
            const double v = std::max(0.0, std::min((f - lo) / std::max(cb[b] - lo, 1e-9), (hi - f) / std::max(hi - cb[b], 1e-9)));
            band.w.push_back(static_cast<float>(v));
            band.sum += static_cast<float>(v);
        }
        bands.push_back(std::move(band));
    }
    return bands;
}

} // namespace

void analyzeSpectra(Context& ctx, const std::atomic<bool>* cancel)
{
    const Params& p = ctx.p;
    int T = 0, K = 0;
    {
        StftFrames frames(ctx.audio, ctx.numSamples, p);
        T = frames.numFrames();
        K = frames.numBins();
    }
    ctx.numFrames = T;
    ctx.numBins = K;
    const double binHz = p.sampleRate / p.fftSize;

    const auto cb = whiteningCentres(p);
    const auto bands = whiteningBands(cb, p, K);
    const float absThr = std::pow(10.0f, p.peakAbsDb / 20.0f);
    const float relFactor = std::pow(10.0f, p.peakRelDb / 20.0f);
    const float overNoise = std::pow(10.0f, p.peakOverNoiseDb / 20.0f);
    const float ifScale = static_cast<float>(p.fftSize / (2.0 * kPi * p.hop));

    // Each frame depends only on its own spectrum and its neighbours', so the frames are worked
    // out in chunks on all cores and their peaks put together in frame order: the same peaks as
    // one pass over the frames.
    constexpr int kChunkFrames = 256;
    const int numChunks = std::max(1, (T + kChunkFrames - 1) / kChunkFrames);
    std::vector<Peaks> chunks(static_cast<size_t>(numChunks));
    parallelFor(numChunks, [&](int chunk) {
        const int t0 = chunk * kChunkFrames, t1 = std::min(T, t0 + kChunkFrames);
        if (t1 <= t0 || (cancel && cancel->load()))
            return;
        StftFrames frames(ctx.audio, ctx.numSamples, p);
        std::vector<float> gamma(cb.size()), gains(static_cast<size_t>(K));
        std::vector<std::complex<float>> cur(static_cast<size_t>(K)), nxt(static_cast<size_t>(K)), prev(static_cast<size_t>(K));
        std::vector<float> mag(static_cast<size_t>(K)), noise(static_cast<size_t>(K)), IF(static_cast<size_t>(K)), scratch;
        Peaks& pk = chunks[static_cast<size_t>(chunk)];
        pk.frameStart.assign(static_cast<size_t>(t1 - t0) + 1, 0);

        // the spectra around the first frame, as one pass would have them there
        if (t0 > 0)
            frames.spectrum(t0 - 1, prev.data());
        frames.spectrum(t0, cur.data());
        if (t0 + 1 < T)
            frames.spectrum(t0 + 1, nxt.data());
        for (int t = t0; t < t1; ++t)
        {
            if (cancel && cancel->load())
                return;
            if (t > t0)
            {
                prev.swap(cur);
                cur.swap(nxt);
                if (t + 1 < T)
                    frames.spectrum(t + 1, nxt.data());
            }
            // instantaneous frequency from the phase advance (frame 0 uses frames 0 -> 1)
            const auto& a = (t == 0) ? cur : prev;
            const auto& b = (t == 0) ? (T > 1 ? nxt : cur) : cur;
            for (int k = 0; k < K; ++k)
            {
                mag[static_cast<size_t>(k)] = std::abs(cur[static_cast<size_t>(k)]);
                const float expected = static_cast<float>(2.0 * kPi * k * p.hop / p.fftSize);
                const float dphi = (t == 0 && T <= 1) ? 0.0f : std::arg(b[static_cast<size_t>(k)]) - std::arg(a[static_cast<size_t>(k)]);
                IF[static_cast<size_t>(k)] = static_cast<float>(k) + princarg(dphi - expected) * ifScale;
            }
            medianRow(mag.data(), K, p.noiseMedianBins, noise.data(), scratch);

            // whitening gains
            if (p.whitenNu < 1.0f)
            {
                for (size_t q = 0; q < bands.size(); ++q)
                {
                    double s = 0.0;
                    for (size_t j = 0; j < bands[q].w.size(); ++j)
                    {
                        const float m = mag[static_cast<size_t>(bands[q].k0) + j];
                        s += bands[q].w[j] * static_cast<double>(m) * m;
                    }
                    const double sigma = std::sqrt(s / std::max(1e-9, static_cast<double>(bands[q].sum)));
                    gamma[q] = static_cast<float>(std::pow(sigma + 1e-7, p.whitenNu - 1.0));
                }
                size_t q = 0;
                for (int k = 0; k < K; ++k)
                {
                    const double f = k * binHz;
                    if (f <= cb.front()) { gains[static_cast<size_t>(k)] = gamma.front(); continue; }
                    if (f >= cb.back()) { gains[static_cast<size_t>(k)] = gamma.back(); continue; }
                    while (q + 1 < cb.size() && cb[q + 1] < f)
                        ++q;
                    const double u = (f - cb[q]) / (cb[q + 1] - cb[q]);
                    gains[static_cast<size_t>(k)] = static_cast<float>(gamma[q] * (1.0 - u) + gamma[q + 1] * u);
                }
            }
            else
            {
                std::fill(gains.begin(), gains.end(), 1.0f);
            }

            // peaks
            pk.frameStart[static_cast<size_t>(t - t0)] = static_cast<int>(pk.bin.size());
            float frameMax = 0.0f;
            for (int k = 0; k < K; ++k)
                frameMax = std::max(frameMax, mag[static_cast<size_t>(k)]);
            const float thr = std::max(absThr, frameMax * relFactor);
            for (int k = 2; k < K - 2; ++k)
            {
                const float v = mag[static_cast<size_t>(k)];
                if (!(v > mag[static_cast<size_t>(k - 1)] && v >= mag[static_cast<size_t>(k + 1)] && v > mag[static_cast<size_t>(k - 2)] && v >= mag[static_cast<size_t>(k + 2)]))
                    continue;
                if (!(v > thr) || !(v > noise[static_cast<size_t>(k)] * overNoise))
                    continue;
                const float la = std::log(mag[static_cast<size_t>(k - 1)] + 1e-12f), lb = std::log(v + 1e-12f), lc = std::log(mag[static_cast<size_t>(k + 1)] + 1e-12f);
                const float den = la - 2.0f * lb + lc;
                float d = std::fabs(den) > 1e-12f ? 0.5f * (la - lc) / den : 0.0f;
                d = std::clamp(d, -0.5f, 0.5f);
                const float amp = std::exp(lb - 0.25f * (la - lc) * d);
                const float fif = IF[static_cast<size_t>(k)];
                const float fbin = std::fabs(fif - k) <= 1.0f ? fif : k + d;
                const float dev = std::max(std::fabs(IF[static_cast<size_t>(k - 1)] - fif), std::fabs(IF[static_cast<size_t>(k + 1)] - fif));
                const float ton = std::exp(-(dev / p.tonalDevBins) * (dev / p.tonalDevBins));
                pk.bin.push_back(k);
                pk.fbin.push_back(fbin);
                pk.amp.push_back(amp);
                pk.tonality.push_back(ton);
                pk.wamp.push_back(amp * gains[static_cast<size_t>(k)]);
            }
            // keep each frame's peaks sorted by refined frequency for binary search
            {
                const int b0 = pk.frameStart[static_cast<size_t>(t - t0)], b1 = static_cast<int>(pk.bin.size());
                bool sorted = true;
                for (int j = b0 + 1; j < b1 && sorted; ++j)
                    sorted = pk.fbin[static_cast<size_t>(j - 1)] <= pk.fbin[static_cast<size_t>(j)];
                if (!sorted)
                {
                    std::vector<int> o(static_cast<size_t>(b1 - b0));
                    for (int j = 0; j < b1 - b0; ++j)
                        o[static_cast<size_t>(j)] = b0 + j;
                    std::stable_sort(o.begin(), o.end(), [&](int x, int y) { return pk.fbin[static_cast<size_t>(x)] < pk.fbin[static_cast<size_t>(y)]; });
                    // only this frame's peaks are copied (copying the whole list made this quadratic)
                    auto permute = [&](auto& v) {
                        const std::vector<typename std::decay_t<decltype(v)>::value_type> copy(v.begin() + b0, v.begin() + b1);
                        for (int j = 0; j < b1 - b0; ++j)
                            v[static_cast<size_t>(b0 + j)] = copy[static_cast<size_t>(o[static_cast<size_t>(j)] - b0)];
                    };
                    permute(pk.bin); permute(pk.fbin); permute(pk.amp); permute(pk.tonality); permute(pk.wamp);
                }
            }
        }
        pk.frameStart[static_cast<size_t>(t1 - t0)] = static_cast<int>(pk.bin.size());
    });
    if (cancel && cancel->load())
        return;

    Peaks& pk = ctx.peaks;
    pk = Peaks{};
    pk.frameStart.assign(static_cast<size_t>(T) + 1, 0);
    size_t total = 0;
    for (const auto& c : chunks)
        total += c.bin.size();
    for (auto* v : { &pk.fbin, &pk.amp, &pk.tonality, &pk.wamp })
        v->reserve(total);
    pk.bin.reserve(total);
    for (int chunk = 0; chunk < numChunks; ++chunk)
    {
        const auto& c = chunks[static_cast<size_t>(chunk)];
        const int t0 = chunk * kChunkFrames;
        const int offset = static_cast<int>(pk.bin.size());
        for (size_t j = 0; j + 1 < c.frameStart.size(); ++j)
            pk.frameStart[static_cast<size_t>(t0) + j] = offset + c.frameStart[j];
        pk.bin.insert(pk.bin.end(), c.bin.begin(), c.bin.end());
        pk.fbin.insert(pk.fbin.end(), c.fbin.begin(), c.fbin.end());
        pk.amp.insert(pk.amp.end(), c.amp.begin(), c.amp.end());
        pk.tonality.insert(pk.tonality.end(), c.tonality.begin(), c.tonality.end());
        pk.wamp.insert(pk.wamp.end(), c.wamp.begin(), c.wamp.end());
    }
    pk.frameStart[static_cast<size_t>(T)] = static_cast<int>(pk.bin.size());
}

} // namespace detail
} // namespace polyphonic
