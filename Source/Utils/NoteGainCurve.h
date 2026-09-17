#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace NoteGainCurve
{
struct Region { int start; int end; float db; };

// Keep the -60 dB endpoint finite so gain edits remain reversible.
inline float linearGain(float db) { return std::pow(10.0f, db / 20.0f); }

inline std::vector<float> build(const std::vector<Region>& regions,
                                int start, int count, int totalSamples, int hopSize)
{
    if (count <= 0 || totalSamples <= 0)
        return {};
    const int half = std::max(64, hopSize / 2) / 2;
    const int first = std::max(0, start - half);
    const int end = std::min(totalSamples, start + count + half);
    std::vector<float> raw(static_cast<size_t>(end - first), 1.0f);
    for (const auto& region : regions)
    {
        const float gain = linearGain(region.db);
        for (int i = std::max(first, region.start); i < std::min(end, region.end); ++i)
            raw[static_cast<size_t>(i - first)] *= gain;
    }
    const auto at = [&](int i) {
        return static_cast<double>(raw[static_cast<size_t>(std::clamp(i, first, end - 1) - first)]);
    };
    double sum = 0.0;
    for (int i = start - half; i <= start + half; ++i)
        sum += at(i);
    std::vector<float> result(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        result[static_cast<size_t>(i)] = static_cast<float>(sum / (2 * half + 1));
        sum -= at(start + i - half);
        sum += at(start + i + half + 1);
    }
    return result;
}
}
