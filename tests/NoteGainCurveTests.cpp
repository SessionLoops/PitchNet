#include "../Source/Utils/NoteGainCurve.h"
#include "../Source/Utils/NoteAmplitude.h"
#include "TestAssert.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    AudioData audio;
    audio.originalWaveform.setSize(2, 2 * HOP_SIZE);
    audio.waveform.setSize(2, 2 * HOP_SIZE);
    audio.waveform.clear();
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 2 * HOP_SIZE; ++sample)
            audio.originalWaveform.setSample(channel, sample, sample < HOP_SIZE ? 0.25f : 0.5f);
    Note left(0, 1, 60.0f), right(1, 2, 60.0f), merged(0, 2, 60.0f);
    NoteAmplitude::update(left, audio);
    NoteAmplitude::update(right, audio);
    NoteAmplitude::update(merged, audio);
    CHECK(std::abs(left.getSourceAmplitude() - 0.25f) < 1e-6f);
    CHECK(std::abs(right.getSourceAmplitude() - 0.5f) < 1e-6f);
    CHECK(std::abs(merged.getSourceAmplitude() - std::sqrt(0.15625f)) < 1e-6f);
    merged.setVolumeDb(6.0f);
    NoteAmplitude::update(merged, audio);
    CHECK(std::abs(merged.getSourceAmplitude() - std::sqrt(0.15625f)) < 1e-6f);
    Note empty(2, 3, 60.0f);
    NoteAmplitude::update(empty, audio);
    CHECK(empty.getSourceAmplitude() == 0.0f);

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
