#pragma once

#include <cmath>
#include <complex>
#include <vector>

namespace polyphonic
{

/**
 * Power-of-two real FFT with no external dependencies.
 *
 * A real transform of size n is computed with one complex transform of size
 * n/2 (even samples in the real part, odd samples in the imaginary part) and a
 * split step. Internals are double precision so that an unedited
 * analysis/resynthesis round trip stays far below audibility.
 *
 * forward(): n real samples -> n/2+1 bins, unscaled.
 * inverse(): n/2+1 bins -> n real samples, scaled by 1/n, so that
 *            inverse(forward(x)) == x.
 */
class RealFFT
{
public:
    explicit RealFFT(int size) : n(size), m(size / 2)
    {
        twiddle.resize(static_cast<size_t>(m / 2 > 0 ? m / 2 : 1));
        for (int k = 0; k < static_cast<int>(twiddle.size()); ++k)
            twiddle[static_cast<size_t>(k)] = std::polar(1.0, -2.0 * pi * k / m);
        split.resize(static_cast<size_t>(m + 1));
        for (int k = 0; k <= m; ++k)
            split[static_cast<size_t>(k)] = std::polar(1.0, -2.0 * pi * k / n);
        bitrev.resize(static_cast<size_t>(m));
        int bits = 0;
        while ((1 << bits) < m)
            ++bits;
        for (int i = 0; i < m; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    r |= 1 << (bits - 1 - b);
            bitrev[static_cast<size_t>(i)] = r;
        }
        buffer.resize(static_cast<size_t>(m));
    }

    int size() const { return n; }
    int numBins() const { return m + 1; }

    void forward(const float* in, std::complex<float>* out)
    {
        for (int k = 0; k < m; ++k)
            buffer[static_cast<size_t>(k)] = {static_cast<double>(in[2 * k]), static_cast<double>(in[2 * k + 1])};
        transform(buffer.data(), false);
        for (int k = 0; k <= m; ++k)
        {
            const auto zk = buffer[static_cast<size_t>(k % m)];
            const auto zc = std::conj(buffer[static_cast<size_t>((m - k) % m)]);
            const auto even = 0.5 * (zk + zc);
            const auto odd = std::complex<double>(0.0, -0.5) * (zk - zc);
            const auto x = even + split[static_cast<size_t>(k)] * odd;
            out[k] = {static_cast<float>(x.real()), static_cast<float>(x.imag())};
        }
    }

    void inverse(const std::complex<float>* in, float* out)
    {
        for (int k = 0; k < m; ++k)
        {
            const std::complex<double> xk(in[k].real(), in[k].imag());
            const std::complex<double> xc(in[m - k].real(), -in[m - k].imag());
            const auto even = 0.5 * (xk + xc);
            const auto odd = 0.5 * (xk - xc) * std::conj(split[static_cast<size_t>(k)]);
            buffer[static_cast<size_t>(k)] = even + std::complex<double>(0.0, 1.0) * odd;
        }
        transform(buffer.data(), true);
        const double scale = 1.0 / m;
        for (int k = 0; k < m; ++k)
        {
            out[2 * k] = static_cast<float>(buffer[static_cast<size_t>(k)].real() * scale);
            out[2 * k + 1] = static_cast<float>(buffer[static_cast<size_t>(k)].imag() * scale);
        }
    }

private:
    static constexpr double pi = 3.14159265358979323846;
    int n, m;
    std::vector<std::complex<double>> twiddle, split, buffer;
    std::vector<int> bitrev;

    void transform(std::complex<double>* data, bool inverseTransform) const
    {
        for (int i = 0; i < m; ++i)
        {
            const int r = bitrev[static_cast<size_t>(i)];
            if (i < r)
                std::swap(data[i], data[r]);
        }
        for (int len = 2; len <= m; len <<= 1)
        {
            const int halfLen = len / 2;
            const int step = m / len;
            for (int i = 0; i < m; i += len)
            {
                for (int j = 0; j < halfLen; ++j)
                {
                    auto w = twiddle[static_cast<size_t>(j * step)];
                    if (inverseTransform)
                        w = std::conj(w);
                    const auto u = data[i + j];
                    const auto v = data[i + j + halfLen] * w;
                    data[i + j] = u + v;
                    data[i + j + halfLen] = u - v;
                }
            }
        }
    }
};

} // namespace polyphonic
