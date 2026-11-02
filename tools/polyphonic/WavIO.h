#pragma once
// Minimal RIFF/WAVE reader and writer (PCM 16/24/32-bit integer, 32-bit float) for the polyphonic tools.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wavio
{
struct Audio
{
    int sampleRate = 44100;
    std::vector<std::vector<float>> channels;
    int numSamples() const { return channels.empty() ? 0 : static_cast<int>(channels[0].size()); }
};

inline bool read(const std::string& path, Audio& out, std::string* error = nullptr)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24)); };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) { if (error) *error = "not a WAVE file"; return false; }
    int fmtTag = 0, channels = 0, bits = 0;
    size_t pos = 12, dataOff = 0, dataLen = 0;
    while (pos + 8 <= d.size())
    {
        const uint32_t len = u32(pos + 4);
        if (!std::memcmp(d.data() + pos, "fmt ", 4))
        {
            fmtTag = static_cast<int>(u16(pos + 8));
            channels = static_cast<int>(u16(pos + 10));
            out.sampleRate = static_cast<int>(u32(pos + 12));
            bits = static_cast<int>(u16(pos + 22));
            if (fmtTag == 0xFFFE && len >= 40) fmtTag = static_cast<int>(u16(pos + 32));
        }
        else if (!std::memcmp(d.data() + pos, "data", 4))
        {
            dataOff = pos + 8;
            dataLen = std::min<size_t>(len, d.size() - dataOff);
        }
        pos += 8 + len + (len & 1);
    }
    if (!dataOff || channels <= 0) { if (error) *error = "missing fmt/data"; return false; }
    const int bps = bits / 8;
    const size_t frames = dataLen / static_cast<size_t>(bps * channels);
    out.channels.assign(static_cast<size_t>(channels), std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            const size_t o = dataOff + (i * channels + c) * bps;
            float v = 0.0f;
            if (fmtTag == 3 && bits == 32) std::memcpy(&v, d.data() + o, 4);
            else if (bits == 16) v = static_cast<int16_t>(u16(o)) / 32768.0f;
            else if (bits == 24) { int32_t s = static_cast<int32_t>((d[o] << 8) | (d[o + 1] << 16) | (static_cast<uint32_t>(d[o + 2]) << 24)) >> 8; v = s / 8388608.0f; }
            else if (bits == 32) v = static_cast<int32_t>(u32(o)) / 2147483648.0f;
            out.channels[static_cast<size_t>(c)][i] = v;
        }
    return true;
}

inline bool writeFloat(const std::string& path, const Audio& a)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t ch = static_cast<uint32_t>(a.channels.size()), frames = static_cast<uint32_t>(a.numSamples());
    const uint32_t dataLen = frames * ch * 4, rate = static_cast<uint32_t>(a.sampleRate);
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataLen); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(3); w16(static_cast<uint16_t>(ch)); w32(rate); w32(rate * ch * 4); w16(static_cast<uint16_t>(ch * 4)); w16(32);
    std::fwrite("data", 1, 4, f); w32(dataLen);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < ch; ++c) std::fwrite(&a.channels[c][i], 4, 1, f);
    std::fclose(f);
    return true;
}
} // namespace wavio
