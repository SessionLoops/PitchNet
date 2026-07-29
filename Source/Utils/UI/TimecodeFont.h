#pragma once

#include "../../JuceHeader.h"
#include "../PlatformPaths.h"
#include "DPIScaleManager.h"

/**
 * Timecode font manager - loads Sarasa-UI-Music-Regular.ttf for timecode/timeline only.
 * Falls back to system font if not found.
 * Uses reference counting to support multiple plugin instances.
 */
class TimecodeFont
{
public:
    static void initialize()
    {
        auto& instance = getInstance();
        ++instance.refCount;

        if (instance.initialized)
            return;

        instance.initialized = true;

        auto projectFont = PlatformPaths::getProjectResourcesDirectory()
                               .getChildFile("fonts/Sarasa-UI-Music-Regular.ttf");
        juce::File appDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        juce::StringArray fontPaths = {
            projectFont.getFullPathName(),
            appDir.getChildFile("Resources/fonts/Sarasa-UI-Music-Regular.ttf").getFullPathName(),
            appDir.getChildFile("../Resources/fonts/Sarasa-UI-Music-Regular.ttf").getFullPathName(),
            appDir.getChildFile("fonts/Sarasa-UI-Music-Regular.ttf").getFullPathName(),
#if JUCE_MAC
            appDir.getChildFile("../Resources/fonts/Sarasa-UI-Music-Regular.ttf").getFullPathName(),
#endif
        };

        for (const auto& path : fontPaths)
        {
            juce::File fontFile(path);
            if (fontFile.existsAsFile())
            {
                juce::MemoryBlock fontData;
                if (fontFile.loadFileAsData(fontData))
                {
                    instance.customTypeface = juce::Typeface::createSystemTypefaceFor(
                        fontData.getData(), fontData.getSize());
                    if (instance.customTypeface != nullptr)
                    {
                        instance.fontLoaded = true;
                        break;
                    }
                }
            }
        }
    }

    static void shutdown()
    {
        auto& instance = getInstance();
        if (instance.refCount > 0)
            --instance.refCount;

        if (instance.refCount == 0 && instance.initialized)
        {
            instance.customTypeface = nullptr;
            instance.fontLoaded = false;
            instance.initialized = false;
            juce::Typeface::clearTypefaceCache();
        }
    }

    static juce::Font getFont(float height = 14.0f)
    {
        auto& instance = getInstance();
        if (instance.fontLoaded && instance.customTypeface != nullptr)
            return juce::Font(
                       juce::FontOptions(instance.customTypeface).withHeight(
                           height));

#if JUCE_MAC
        return juce::Font(juce::FontOptions(height));
#elif JUCE_WINDOWS
        return juce::Font(
            juce::FontOptions("Yu Gothic UI", height, juce::Font::plain));
#else
        return juce::Font(juce::FontOptions(height));
#endif
    }

    static juce::Font getBoldFont(float height = 14.0f)
    {
        auto& instance = getInstance();
        if (instance.fontLoaded && instance.customTypeface != nullptr)
            return juce::Font(
                       juce::FontOptions(instance.customTypeface).withHeight(
                           height))
                .boldened();

#if JUCE_MAC
        return juce::Font(juce::FontOptions(height)).boldened();
#elif JUCE_WINDOWS
        return juce::Font(
            juce::FontOptions("Yu Gothic UI", height, juce::Font::bold));
#else
        return juce::Font(juce::FontOptions(height)).boldened();
#endif
    }

    static juce::Font getScaledFont(float baseHeight, const juce::Component* component)
    {
        float scaledHeight = DPIScaleManager::scaleFont(baseHeight, component);
        return getFont(scaledHeight);
    }

    static juce::Font getScaledBoldFont(float baseHeight, const juce::Component* component)
    {
        float scaledHeight = DPIScaleManager::scaleFont(baseHeight, component);
        return getBoldFont(scaledHeight);
    }

private:
    TimecodeFont() = default;

    static TimecodeFont& getInstance()
    {
        static TimecodeFont instance;
        return instance;
    }

    juce::Typeface::Ptr customTypeface;
    bool fontLoaded = false;
    bool initialized = false;
    int refCount = 0;
};
