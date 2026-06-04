#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/UI/DPIScaleManager.h"

/**
 * Global font manager - uses Montserrat Medium by default.
 * Falls back through JUCE if Montserrat is not installed.
 * Uses reference counting to support multiple plugin instances.
 * Supports DPI-aware font scaling.
 */
class AppFont
{
public:
    static void initialize()
    {
        auto& instance = getInstance();
        ++instance.refCount;

        if (instance.initialized)
            return;

        instance.initialized = true;

        // Use the installed Montserrat family by name. If unavailable, JUCE
        // falls back to an available system font.
    }

    /**
     * Release font resources. Call this before application/plugin shutdown
     * to avoid JUCE leak detector warnings.
     * Uses reference counting - only releases when last user calls shutdown.
     */
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
        }
    }

    /**
     * Get font with specified height (no DPI scaling applied).
     * Use getScaledFont() for DPI-aware font sizing.
     */
    static juce::Font getFont(float height = 14.0f)
    {
        auto& instance = getInstance();
        if (instance.fontLoaded && instance.customTypeface != nullptr)
            return juce::Font(
                       juce::FontOptions(instance.customTypeface).withHeight(
                           height));

        return juce::Font(juce::FontOptions("Montserrat", "Medium", height));
    }

    /**
     * Get font with DPI-scaled height based on the component's display.
     * @param baseHeight The logical font height (e.g., 14.0f)
     * @param component The component to get DPI scale from (can be nullptr)
     */
    static juce::Font getScaledFont(float baseHeight, const juce::Component* component)
    {
        float scaledHeight = DPIScaleManager::scaleFont(baseHeight, component);
        return getFont(scaledHeight);
    }

    /**
     * Get bold font with specified height (no DPI scaling applied).
     * Use getScaledBoldFont() for DPI-aware font sizing.
     */
    static juce::Font getBoldFont(float height = 14.0f)
    {
        auto& instance = getInstance();
        if (instance.fontLoaded && instance.customTypeface != nullptr)
            return juce::Font(
                       juce::FontOptions(instance.customTypeface).withHeight(
                           height))
                .boldened();

        return juce::Font(juce::FontOptions("Montserrat", "Medium", height))
            .boldened();
    }

    /**
     * Get bold font with DPI-scaled height based on the component's display.
     * @param baseHeight The logical font height (e.g., 14.0f)
     * @param component The component to get DPI scale from (can be nullptr)
     */
    static juce::Font getScaledBoldFont(float baseHeight, const juce::Component* component)
    {
        float scaledHeight = DPIScaleManager::scaleFont(baseHeight, component);
        return getBoldFont(scaledHeight);
    }

    static bool isCustomFontLoaded()
    {
        return getInstance().fontLoaded;
    }

private:
    AppFont() = default;

    static AppFont& getInstance()
    {
        static AppFont instance;
        return instance;
    }

    juce::Typeface::Ptr customTypeface;
    bool fontLoaded = false;
    bool initialized = false;
    int refCount = 0;
};
