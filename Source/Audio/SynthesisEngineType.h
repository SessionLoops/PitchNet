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
 *
 * - macOS: the neural vocoder, which Core ML always takes off the CPU.
 * - Linux: classic PSOLA, since the vocoder only runs on CPU there (no
 *   CUDA/DirectML/CoreML execution provider).
 * - Windows: the vocoder only when there is real GPU headroom for it. A
 *   machine with no hardware adapter, or whose only adapter is an integrated
 *   GPU sharing system memory, starts on classic PSOLA instead - DirectML on an
 *   iGPU is barely faster than the CPU for this model, and every edit would
 *   render slowly. Machines with a discrete card (including iGPU + dGPU
 *   laptops) keep the vocoder.
 *
 * The Windows check touches DXGI/D3D12, so the result is computed once and
 * cached for the life of the process.
 */
SynthesisEngineType defaultSynthesisEngineType();

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
