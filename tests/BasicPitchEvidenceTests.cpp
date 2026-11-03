// Basic Pitch window geometry and optional real-model inference. No JUCE dependency.
#include "../Source/Audio/Polyphonic/BasicPitchEvidence.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
using Evidence = polyphonic::BasicPitchEvidence;
constexpr int kWindowHop = Evidence::kWindowSamples - Evidence::kOverlapFrames * Evidence::kHop;
constexpr int kKeptFrames = Evidence::kWindowFrames - Evidence::kOverlapFrames;

void testFrameGeometry()
{
    CHECK(Evidence::frameTimesForSamples(0).empty());
    CHECK(Evidence::frameTimesForSamples(-1).empty());
    const auto tiny = Evidence::frameTimesForSamples(1);
    CHECK(tiny.size() == 1);
    CHECK(tiny.front() == 0.0f);
    CHECK(Evidence::frameTimesForSamples(Evidence::kHop).size() == 1);
    CHECK(Evidence::frameTimesForSamples(Evidence::kHop + 1).size() == 2);

    const auto boundary = Evidence::frameTimesForSamples(kWindowHop + 1);
    CHECK(boundary.size() == kKeptFrames + 1);
    CHECK(std::fabs(boundary[kKeptFrames] - static_cast<double>(kWindowHop) / Evidence::kSampleRate) < 1e-6);
    CHECK(std::fabs(boundary[kKeptFrames - 1] - static_cast<double>((kKeptFrames - 1) * Evidence::kHop) / Evidence::kSampleRate) < 1e-6);

    // Five minutes exposes both the fractional window stride and tail loss from rounding
    // 22050 / 256 to 86 fps. Every timestamp must still refer to a real source sample.
    constexpr int samples = 300 * Evidence::kSampleRate + 73;
    const auto times = Evidence::frameTimesForSamples(samples);
    CHECK(times.size() > 300 * 86);
    CHECK(times.front() == 0.0f);
    for (size_t i = 0; i < times.size(); ++i)
    {
        const int64_t sample = static_cast<int64_t>(i / kKeptFrames) * kWindowHop
                             + static_cast<int64_t>(i % kKeptFrames) * Evidence::kHop;
        CHECK(std::fabs(times[i] - static_cast<double>(sample) / Evidence::kSampleRate) < 2e-5);
        CHECK(times[i] < static_cast<double>(samples) / Evidence::kSampleRate);
        CHECK(i == 0 || times[i] > times[i - 1]);
    }
    CHECK(static_cast<double>(samples) / Evidence::kSampleRate - times.back() <= static_cast<double>(Evidence::kHop) / Evidence::kSampleRate + 2e-5);
}

void testModel(const char* model)
{
    Evidence bp;
    CHECK(bp.load(model));
    constexpr int rate = Evidence::kSampleRate;
    constexpr double pi = 3.14159265358979323846;
    // Put a real model input near the end of a long recording. Previously 86-fps
    // truncation removed roughly the final 400 ms of this minute-long evidence.
    std::vector<float> audio(60 * rate, 0.0f);
    const int start = static_cast<int>(59.25 * rate);
    for (int i = start; i < static_cast<int>(audio.size()); ++i)
    {
        const double t = static_cast<double>(i - start) / rate;
        const double envelope = std::min(1.0, t / 0.01);
        audio[static_cast<size_t>(i)] = static_cast<float>(envelope * (0.3 * std::sin(2 * pi * 440 * t)
                                           + 0.15 * std::sin(2 * pi * 880 * t) + 0.1 * std::sin(2 * pi * 1320 * t)));
    }
    const auto ev = bp.compute(audio.data(), static_cast<int>(audio.size()), rate);
    CHECK(!ev.empty());
    CHECK(bp.lastError().empty());
    CHECK(ev.frameTimes == Evidence::frameTimesForSamples(static_cast<int>(audio.size())));
    CHECK(ev.note.size() == ev.frameTimes.size() * 88);
    CHECK(ev.frameTimes.back() > 59.98f);
    float lateMax = 0.0f;
    for (size_t i = 0; i < ev.frameTimes.size(); ++i)
        if (ev.frameTimes[i] > 59.7f)
            lateMax = std::max(lateMax, ev.note[i * 88 + 69 - 21]);
    CHECK(lateMax > 0.1f);

    // A sub-hop recording still has a frame at time zero rather than empty evidence.
    const auto tiny = bp.compute(audio.data(), 1, rate);
    CHECK(tiny.frameTimes.size() == 1);
    CHECK(tiny.note.size() == 88);
    const std::atomic<bool> cancel{true};
    CHECK(bp.compute(audio.data(), rate, rate, &cancel).empty());
}
} // namespace

int main(int argc, char** argv)
{
    testFrameGeometry();
    if (argc > 1)
        testModel(argv[1]);
    std::puts("Basic Pitch evidence tests passed");
    return 0;
}
