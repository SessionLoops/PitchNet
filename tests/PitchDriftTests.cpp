#include "../Source/Utils/PitchToolOperations.h"
#include "../Source/Utils/Constants.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <numeric>

// Standalone: c++ -std=c++17 tests/PitchDriftTests.cpp Source/Utils/PitchToolOperations.cpp -o /tmp/pitch-drift-tests
int main() {
    using namespace PitchToolOperations;
    assert(scalePitchDrift({}, 0).empty());
    assert(scalePitchDrift({0.7f}, 0) == std::vector<float>{0.7f});
    const std::vector<float> constant(100, 0.5f);
    assert(scalePitchDrift(constant, 0) == constant);
    std::vector<float> curve(344), wobble(curve.size());
    for (size_t i = 0; i < curve.size(); ++i) {
        const double t = double(i) * HOP_SIZE / SAMPLE_RATE;
        wobble[i] = 0.2f * std::sin(2 * 3.141592653589793 * 6 * t);
        curve[i] = 0.3f + 0.4f * (t - 2) + wobble[i];
    }
    assert(scalePitchDrift(curve, 1) == curve);
    const auto flat = scalePitchDrift(curve, 0);
    const auto inverted = scalePitchDrift(curve, -1);
    const auto amplified = scalePitchDrift(curve, 2);
    assert(std::abs(computeMean(curve) - computeMean(flat)) < 1e-5f);
    double residual = 0;
    const float center = computeMean(curve);
    for (size_t i = 30; i < curve.size() - 30; ++i) {
        residual += std::pow(flat[i] - center - wobble[i], 2);
        assert(std::abs(inverted[i] - (2 * flat[i] - curve[i])) < 1e-6f);
        assert(std::abs(amplified[i] - (2 * curve[i] - flat[i])) < 1e-6f);
    }
    // Remove the ramp while retaining the 6 Hz vibrato (under 2 cents RMS error).
    assert(std::sqrt(residual / (curve.size() - 60)) < 0.02);
    const auto composed = applyAllTransformations(curve, 0, 0, 1, 0, 0, {}, 0);
    assert(composed == flat);
    const auto zeroVibrato = applyAllTransformations(curve, 0, 0, 0, 0, 0, {}, 0);
    for (float x : zeroVibrato) assert(x == 0);
    std::cout << "Pitch drift tests passed\n";
}
