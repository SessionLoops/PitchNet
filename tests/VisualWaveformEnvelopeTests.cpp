#include "../Source/UI/PianoRoll/VisualWaveformEnvelope.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    using namespace VisualWaveformEnvelope;
    std::vector<float> samples(48000, 0.1f);
    const auto envelope = [&](float gain, const std::vector<GainRegion>& regions = {}) {
        return build(samples.data(), static_cast<int>(samples.size()), 0, 48000,
                     1000, 1000.0f, 48000.0, 1000.0f, true, gain, regions);
    };
    const auto unity = envelope(1.0f);
    const auto louder = envelope(2.0f);
    const auto quieter = envelope(0.5f);
    const auto muted = envelope(0.0f);
    for (size_t i = 0; i < unity.size(); ++i)
    {
        assert(std::abs(louder[i] - 2.0f * unity[i]) < 1e-5f);
        assert(std::abs(quieter[i] - 0.5f * unity[i]) < 1e-5f);
        assert(muted[i] == 0.0f);
    }
    // A selected note changes only its interval in the background waveform.
    const auto regional = envelope(1.0f, {{12000, 24000, 2.0f}});
    assert(std::abs(regional[375] - louder[375]) < 1e-5f);
    assert(std::abs(regional[100] - unity[100]) < 1e-5f);
    assert(std::abs(regional[800] - unity[800]) < 1e-5f);
    // Apply gain before visual saturation, so reducing a loud signal works.
    std::fill(samples.begin(), samples.end(), 1.0f);
    const auto reduced = envelope(0.1f);
    assert(std::abs(reduced[500] - 0.175f) < 1e-5f);
    std::cout << "Visual waveform envelope tests passed\n";
}
