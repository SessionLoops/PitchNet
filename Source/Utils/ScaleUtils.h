#pragma once

#include "../Models/Project.h"
#include "Constants.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ScaleUtils
{
inline constexpr std::array<int, 7> kMajorIntervals { 0, 2, 4, 5, 7, 9, 11 };
inline constexpr std::array<int, 7> kMinorIntervals { 0, 2, 3, 5, 7, 8, 10 };
inline constexpr std::array<int, 7> kDorianIntervals { 0, 2, 3, 5, 7, 9, 10 };
inline constexpr std::array<int, 7> kPhrygianIntervals { 0, 1, 3, 5, 7, 8, 10 };
inline constexpr std::array<int, 7> kLydianIntervals { 0, 2, 4, 6, 7, 9, 11 };
inline constexpr std::array<int, 7> kMixolydianIntervals { 0, 2, 4, 5, 7, 9, 10 };
inline constexpr std::array<int, 7> kLocrianIntervals { 0, 1, 3, 5, 6, 8, 10 };

inline const std::array<int, 7>& getIntervalsForMode(ScaleMode mode)
{
    switch (mode) {
    case ScaleMode::Major:
        return kMajorIntervals;
    case ScaleMode::Minor:
        return kMinorIntervals;
    case ScaleMode::Dorian:
        return kDorianIntervals;
    case ScaleMode::Phrygian:
        return kPhrygianIntervals;
    case ScaleMode::Lydian:
        return kLydianIntervals;
    case ScaleMode::Mixolydian:
        return kMixolydianIntervals;
    case ScaleMode::Locrian:
        return kLocrianIntervals;
    case ScaleMode::None:
    case ScaleMode::Chromatic:
        return kMajorIntervals;
    }
    return kMajorIntervals;
}

inline bool isPitchClassInScale(ScaleMode mode, int pitchClass, int rootNote)
{
    if (mode == ScaleMode::None || rootNote < 0)
        return false;
    if (mode == ScaleMode::Chromatic)
        return true;

    const int normalizedPitch = (pitchClass % 12 + 12) % 12;
    const int normalizedRoot = (rootNote % 12 + 12) % 12;
    const int relativeSemitone = (normalizedPitch - normalizedRoot + 12) % 12;
    const auto& intervals = getIntervalsForMode(mode);
    return std::find(intervals.begin(), intervals.end(), relativeSemitone) != intervals.end();
}

inline float getReferenceOffsetSemitones(int referenceHz)
{
    const int normalized = juce::jlimit(430, 450, referenceHz);
    return 12.0f * std::log2(static_cast<float>(normalized) / FREQ_A4);
}

inline float snapMidiToSemitone(float midi, int referenceHz = static_cast<int>(FREQ_A4))
{
    const float offset = getReferenceOffsetSemitones(referenceHz);
    return std::round(midi - offset) + offset;
}

inline float snapMidiToScale(float midi,
                             ScaleMode mode,
                             int rootNote,
                             int referenceHz = static_cast<int>(FREQ_A4))
{
    const float offset = getReferenceOffsetSemitones(referenceHz);
    const float normalizedMidi = midi - offset;
    const int roundedMidi = static_cast<int>(std::round(normalizedMidi));
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic || rootNote < 0)
        return snapMidiToSemitone(midi, referenceHz);

    constexpr int kSearchRadius = 24;
    constexpr float kTieEpsilon = 1.0e-4f;

    int bestMidi = roundedMidi;
    float bestDistance = std::numeric_limits<float>::max();
    int bestRoundedDistance = std::numeric_limits<int>::max();

    const int searchStart = roundedMidi - kSearchRadius;
    const int searchEnd = roundedMidi + kSearchRadius;
    for (int candidate = searchStart; candidate <= searchEnd; ++candidate) {
        if (!isPitchClassInScale(mode, candidate, rootNote))
            continue;

        const float distance = std::abs(static_cast<float>(candidate) - normalizedMidi);
        const int roundedDistance = std::abs(candidate - roundedMidi);
        const bool isCloser = distance < bestDistance - kTieEpsilon;
        const bool isDistanceTie = std::abs(distance - bestDistance) <= kTieEpsilon;
        const bool betterRoundedDistance =
            roundedDistance < bestRoundedDistance;
        const bool lowerPitchTie =
            roundedDistance == bestRoundedDistance && candidate < bestMidi;

        if (isCloser || (isDistanceTie && (betterRoundedDistance || lowerPitchTie))) {
            bestDistance = distance;
            bestRoundedDistance = roundedDistance;
            bestMidi = candidate;
        }
    }

    return static_cast<float>(bestMidi) + offset;
}
}
