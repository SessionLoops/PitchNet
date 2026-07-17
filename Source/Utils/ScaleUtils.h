#pragma once

#include "../Models/Project.h"
#include "Constants.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>

namespace ScaleUtils
{
inline constexpr std::uint16_t intervalBit(int semitone)
{
    return static_cast<std::uint16_t>(1u << semitone);
}

inline constexpr std::uint16_t getIntervalMaskForMode(ScaleMode mode)
{
    switch (mode) {
    case ScaleMode::Major:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::Minor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Dorian:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(10);
    case ScaleMode::Phrygian:
        return intervalBit(0) | intervalBit(1) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Lydian:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(6) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::Mixolydian:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(10);
    case ScaleMode::Locrian:
        return intervalBit(0) | intervalBit(1) | intervalBit(3) |
               intervalBit(5) | intervalBit(6) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Blues:
        return intervalBit(0) | intervalBit(3) | intervalBit(5) |
               intervalBit(6) | intervalBit(7) | intervalBit(10);
    case ScaleMode::HarmonicMinor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(11);
    case ScaleMode::MajorPentatonic:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(7) | intervalBit(9);
    case ScaleMode::MelodicMinor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::MinorPentatonic:
        return intervalBit(0) | intervalBit(3) | intervalBit(5) |
               intervalBit(7) | intervalBit(10);
    case ScaleMode::PhrygianDominant:
        return intervalBit(0) | intervalBit(1) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::WholeTone:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(6) | intervalBit(8) | intervalBit(10);
    case ScaleMode::None:
        return 0;
    case ScaleMode::Chromatic:
        return 0x0FFFu;
    }
    return 0;
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
    const auto intervalMask = getIntervalMaskForMode(mode);
    return (intervalMask & intervalBit(relativeSemitone)) != 0;
}

inline bool detectMajorOrRelativeMinor(const Project& project,
                                       int& detectedRootNote,
                                       ScaleMode& detectedMode)
{
    std::array<int, 12> pitchClassCounts {};
    int noteCount = 0;

    for (const auto& note : project.getNotes()) {
        if (note.isRest())
            continue;

        const int roundedMidi = static_cast<int>(std::round(note.getMidiNote()));
        const int pitchClass = (roundedMidi % 12 + 12) % 12;
        ++pitchClassCounts[static_cast<size_t>(pitchClass)];
        ++noteCount;
    }

    if (noteCount == 0)
        return false;

    int bestMajorRoot = 0;
    int bestFitCount = -1;
    for (int candidateRoot = 0; candidateRoot < 12; ++candidateRoot) {
        int fitCount = 0;
        for (int pitchClass = 0; pitchClass < 12; ++pitchClass) {
            if (isPitchClassInScale(ScaleMode::Major, pitchClass,
                                    candidateRoot))
                fitCount += pitchClassCounts[static_cast<size_t>(pitchClass)];
        }

        if (fitCount > bestFitCount) {
            bestFitCount = fitCount;
            bestMajorRoot = candidateRoot;
        }
    }

    const int relativeMinorRoot = (bestMajorRoot + 9) % 12;
    const int majorRootCount =
        pitchClassCounts[static_cast<size_t>(bestMajorRoot)];
    const int minorRootCount =
        pitchClassCounts[static_cast<size_t>(relativeMinorRoot)];

    if (minorRootCount > majorRootCount) {
        detectedRootNote = relativeMinorRoot;
        detectedMode = ScaleMode::Minor;
    } else {
        detectedRootNote = bestMajorRoot;
        detectedMode = ScaleMode::Major;
    }

    return true;
}

inline void detectAndApplyScale(Project& project)
{
    int rootNote = 0;
    ScaleMode mode = ScaleMode::Major;
    if (!detectMajorOrRelativeMinor(project, rootNote, mode))
        return;

    project.setScaleRootNote(rootNote);
    project.setPreferredScaleMode(mode);

    const auto activeMode = project.getScaleMode();
    if (activeMode != ScaleMode::Chromatic && activeMode != ScaleMode::None)
        project.setScaleMode(mode);
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
