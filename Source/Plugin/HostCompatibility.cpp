#include "HostCompatibility.h"

HostCompatibility::HostInfo HostCompatibility::createHostInfo(HostType type) {
    HostInfo info;
    info.type = type;

    switch (type) {
        case HostType::StudioOne:
            info.name = "Studio One";
            info.supportsARA = true;
            info.notes = "ARA supported. Use ARA mode for best integration.";
            break;

        case HostType::Cubase:
            info.name = "Cubase";
            info.supportsARA = true;
            info.notes = "ARA supported.";
            break;

        case HostType::LogicPro:
            info.name = "Logic Pro";
            info.supportsARA = true;
            info.notes = "ARA supported (AU format).";
            break;

        case HostType::ProTools:
            info.name = "Pro Tools";
            info.supportsARA = true;
            info.notes = "ARA supported (AAX format).";
            break;

        case HostType::Reaper:
            info.name = "REAPER";
            info.supportsARA = true;
            info.notes = "ARA supported.";
            break;

        case HostType::Nuendo:
            info.name = "Nuendo";
            info.supportsARA = true;
            info.notes = "ARA supported.";
            break;

        case HostType::FLStudio:
            info.name = "FL Studio";
            info.supportsARA = false;
            info.requiresSpecialHandling = true;
            info.notes = "No ARA support. Uses auto-capture mode.";
            break;

        case HostType::AbletonLive:
            info.name = "Ableton Live";
            info.supportsARA = false;
            info.notes = "No ARA support. Uses auto-capture mode.";
            break;

        case HostType::Bitwig:
            info.name = "Bitwig Studio";
            info.supportsARA = false;
            info.notes = "No ARA support. Uses auto-capture mode.";
            break;

        case HostType::AdobeAudition:
            info.name = "Adobe Audition";
            info.supportsARA = false;
            info.notes = "No ARA support. Uses auto-capture mode.";
            break;

        case HostType::Unknown:
        default:
            info.name = "";
            info.supportsARA = false;
            info.notes = "Unknown host.";
            break;
    }

    return info;
}

HostCompatibility::HostType HostCompatibility::detectHostType(juce::AudioProcessor* processor) {
    if (!processor)
        return HostType::Unknown;

    // Only the AAX wrapper implies Pro Tools. JucePlugin_Build_AAX is set for
    // the code shared by every format, so it cannot be used to tell hosts apart.
    if (processor->wrapperType == juce::AudioProcessor::wrapperType_AAX)
        return HostType::ProTools;

    const juce::PluginHostType host;
    if (host.isProTools())       return HostType::ProTools;
    if (host.isStudioOne())      return HostType::StudioOne;
    if (host.isNuendo())         return HostType::Nuendo;
    if (host.isCubase())         return HostType::Cubase;
    if (host.isLogic())          return HostType::LogicPro;
    if (host.isReaper())         return HostType::Reaper;
    if (host.isFruityLoops())    return HostType::FLStudio;
    if (host.isAbletonLive())    return HostType::AbletonLive;
    if (host.isBitwigStudio())   return HostType::Bitwig;
    if (host.isAdobeAudition())  return HostType::AdobeAudition;
    return HostType::Unknown;
}

HostCompatibility::HostInfo HostCompatibility::detectHost(juce::AudioProcessor* processor) {
    auto info = createHostInfo(detectHostType(processor));

#if JucePlugin_Enable_ARA
    // An active ARA editor proves ARA support even for a host not listed above.
    if (processor) {
        if (auto* editor = processor->getActiveEditor()) {
            if (auto* araEditor = dynamic_cast<juce::AudioProcessorEditorARAExtension*>(editor)) {
                if (auto* editorView = araEditor->getARAEditorView()) {
                    if (editorView->getDocumentController()) {
                        info.supportsARA = true;
                        info.notes = "ARA mode active.";
                    }
                }
            }
        }
    }
#endif

    return info;
}

juce::String HostCompatibility::getRecommendedMode(const HostInfo& info) {
    return info.supportsARA ? "ARA Mode (Recommended)" : "Non-ARA Mode (Auto-capture)";
}
