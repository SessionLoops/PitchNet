#pragma once

#include "../JuceHeader.h"

/**
 * Pitch detector types available for F0 extraction.
 */
enum class PitchDetectorType
{
    RMVPE = 0,  // Robust Model for Vocal Pitch Estimation
    FCPE,       // Fast Context-aware Pitch Estimation
    SwiftF0     // Default - SwiftF0 (lars76/swift-f0), CPU-only, 16 ms hop
};

/**
 * Convert PitchDetectorType to string for display/storage.
 */
inline const char* pitchDetectorTypeToString(PitchDetectorType type)
{
    switch (type)
    {
        case PitchDetectorType::RMVPE: return "RMVPE";
        case PitchDetectorType::FCPE:  return "FCPE";
        case PitchDetectorType::SwiftF0: return "SwiftF0";
        default: return "SwiftF0";
    }
}

/**
 * Convert string to PitchDetectorType.
 */
inline PitchDetectorType stringToPitchDetectorType(const juce::String& str)
{
    if (str == "FCPE")  return PitchDetectorType::FCPE;
    if (str == "SwiftF0") return PitchDetectorType::SwiftF0;
    return PitchDetectorType::SwiftF0;  // Default
}
