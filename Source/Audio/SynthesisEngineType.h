#pragma once

#include "../JuceHeader.h"

/**
 * Engines available for resynthesising edited regions.
 */
enum class SynthesisEngineType
{
    Vocoder = 0,   // Default - PC-NSF-HiFiGAN neural vocoder (mel + F0 -> waveform)
    Psola          // Time-domain PSOLA (pitch-synchronous overlap-add)
};

/**
 * Convert SynthesisEngineType to string for display/storage.
 */
inline const char* synthesisEngineTypeToString(SynthesisEngineType type)
{
    switch (type)
    {
        case SynthesisEngineType::Psola:   return "PSOLA";
        case SynthesisEngineType::Vocoder: return "Vocoder";
        default:                           return "Vocoder";
    }
}

/**
 * Convert string to SynthesisEngineType.
 */
inline SynthesisEngineType stringToSynthesisEngineType(const juce::String& str)
{
    if (str == "PSOLA")
        return SynthesisEngineType::Psola;
    return SynthesisEngineType::Vocoder;  // Default
}
