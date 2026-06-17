#pragma once

#include "../JuceHeader.h"

/**
 * Pitch detector types available for F0 extraction.
 */
enum class PitchDetectorType
{
    RMVPE = 0,  // Robust Model for Vocal Pitch Estimation
    FCPE        // Default - Fast Context-aware Pitch Estimation
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
        default: return "FCPE";
    }
}

/**
 * Convert string to PitchDetectorType.
 */
inline PitchDetectorType stringToPitchDetectorType(const juce::String& str)
{
    if (str == "FCPE")  return PitchDetectorType::FCPE;
    return PitchDetectorType::FCPE;  // Default
}
