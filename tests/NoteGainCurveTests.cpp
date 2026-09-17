#include "../Source/Utils/NoteGainCurve.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    using namespace NoteGainCurve;
    constexpr int count = 4096, hop = 256;
    const std::vector<Region> before{{512, 2048, -60.0f}, {2048, 3072, -3.0f}};
    const std::vector<Region> after{{512, 2048, 6.0f}, {2048, 3072, -3.0f}};
    const auto oldGain = build(before, 0, count, count, hop);
    const auto newGain = build(after, 0, count, count, hop);
    const auto crop = build(after, 448, 1664, count, hop);
    for (int i = 0; i < count; ++i)
    {
        // Direct gain edits match rendering from the same ungained audio,
        // including boundaries, and can be undone even from -60 dB.
        const float dry = 0.25f * std::sin(i * 0.1f);
        const float original = dry * oldGain[i];
        const float edited = original * (newGain[i] / oldGain[i]);
        assert(std::abs(edited - dry * newGain[i]) < 1e-6f);
        assert(std::abs(edited * (oldGain[i] / newGain[i]) - original) < 1e-6f);
        if (i < 448 || i >= 2112)
            assert(oldGain[i] == newGain[i]);
    }
    for (size_t i = 0; i < crop.size(); ++i)
        assert(std::abs(crop[i] - newGain[i + 448]) < 1e-6f);
    assert(std::abs(oldGain[1000] - 0.001f) < 1e-6f);
    assert(newGain[512] > 1.0f && newGain[512] < linearGain(6.0f));
    const auto overlap = build({{0, count, 6.0f}, {0, count, -6.0f}}, 0, count, count, hop);
    for (float gain : overlap) assert(std::abs(gain - 1.0f) < 1e-6f);
    assert(build({}, 0, 0, 0, hop).empty());
    const auto shortNote = build({{0, 1, -60.0f}}, 0, 1, 1, hop);
    assert(shortNote.size() == 1 && std::abs(shortNote[0] - 0.001f) < 1e-6f);
    const auto neutral = build({}, 0, count, count, hop);
    for (float gain : neutral) assert(gain == 1.0f);
    std::cout << "Note gain curve tests passed\n";
}
