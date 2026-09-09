#pragma once

#include "../JuceHeader.h"

/**
 * Engines available for resynthesising edited regions.
 */
enum class SynthesisEngineType
{
    Vocoder = 0,   // PC-NSF-HiFiGAN neural vocoder (mel + F0 -> waveform)
    Psola          // Classic - time-domain PSOLA (pitch-synchronous overlap-add)
};

/**
 * Default engine for a fresh install/instance with no saved preference.
 * Linux defaults to the classic PSOLA engine, since the neural vocoder path
 * only runs on CPU there (no CUDA/DirectML/CoreML execution provider).
 */
inline SynthesisEngineType defaultSynthesisEngineType()
{
#if JUCE_LINUX
    return SynthesisEngineType::Psola;
#else
    return SynthesisEngineType::Vocoder;
#endif
}

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
    if (str == "Vocoder")
        return SynthesisEngineType::Vocoder;
    return defaultSynthesisEngineType();
}
