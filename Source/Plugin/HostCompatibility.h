#pragma once

#include "../JuceHeader.h"

/**
 * Host compatibility detection and information
 * Identifies DAW hosts and their ARA support status
 */
class HostCompatibility {
public:
    enum class HostType {
        Unknown,
        StudioOne,    // ARA
        Cubase,       // ARA
        LogicPro,     // ARA (AU)
        ProTools,     // ARA (AAX)
        Reaper,       // ARA
        Nuendo,       // ARA
        FLStudio,     // No ARA
        AbletonLive,  // No ARA
        Bitwig,       // No ARA
        AdobeAudition // No ARA
    };

    struct HostInfo {
        HostType type = HostType::Unknown;
        juce::String name;
        bool supportsARA = false;
        bool requiresSpecialHandling = false;
        juce::String notes;
    };

    static HostInfo detectHost(juce::AudioProcessor* processor);
    static bool hostSupportsARA(const HostInfo& info) { return info.supportsARA; }
    static juce::String getRecommendedMode(const HostInfo& info);

private:
    static HostType detectHostType(juce::AudioProcessor* processor);
    static HostInfo createHostInfo(HostType type);
};
