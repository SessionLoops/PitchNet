#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/UI/Theme.h"
#include "AppFont.h"

/**
 * Shared dark theme LookAndFeel for the application.
 *
 * Provides modern, rounded, shadow-rich rendering for popup menus,
 * tick boxes, progress bars, and all standard JUCE widgets.
 */
class DarkLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DarkLookAndFeel();

    // ── Font overrides ────────────────────────────────────────────
    juce::Font getTextButtonFont(juce::TextButton&, int) override
    {
        return AppFont::getFont(15.0f);
    }

    juce::Font getLabelFont(juce::Label&) override
    {
        return AppFont::getFont(14.0f);
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override
    {
        return AppFont::getFont(13.5f);
    }

    juce::Font getPopupMenuFont() override
    {
        return AppFont::getFont(13.5f);
    }

    // Keep tooltips centred above the pointer, matching the app's compact UI.
    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText,
                                          juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override;
    void drawTooltip(juce::Graphics& g, const juce::String& text,
                     int width, int height) override;

    // ── Custom paint overrides ────────────────────────────────────
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                          bool isSeparator, bool isActive, bool isHighlighted,
                          bool isTicked, bool hasSubMenu,
                          const juce::String& text, const juce::String& shortcutKeyText,
                          const juce::Drawable* icon, const juce::Colour* textColour) override;

    void drawTickBox(juce::Graphics& g, juce::Component& component,
                     float x, float y, float w, float h,
                     bool ticked, bool isEnabled, bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override;

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

    void drawProgressBar(juce::Graphics& g, juce::ProgressBar& bar,
                         int width, int height, double progress,
                         const juce::String& textToShow) override;

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;

    // Suppress default square frame around rounded popup menus
    void drawResizableFrame(juce::Graphics&, int, int,
                            const juce::BorderSize<int>&) override {}

    // Rounded text editor rendering
    void fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                  juce::TextEditor& editor) override;
    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& editor) override;

    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                       int x, int y, int width, int height,
                       bool isScrollbarVertical, int thumbStartPosition,
                       int thumbSize, bool isMouseOver, bool isMouseDown) override;

    int getDefaultScrollbarWidth() override { return 8; }

    // ── Singleton ─────────────────────────────────────────────────
    static DarkLookAndFeel& getInstance()
    {
        static DarkLookAndFeel instance;
        return instance;
    }
};
