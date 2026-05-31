#pragma once

#include "DPIScaleManager.h"

/**
 * DPI-aware UI layout constants.
 *
 * This class provides scaled versions of common UI dimensions.
 * All values are in logical pixels and will be scaled based on the
 * component's display DPI.
 *
 * Usage:
 *   // In a component's resized() or paint():
 *   UILayout layout(this);
 *   int keysWidth = layout.pianoKeysWidth();
 *   int headerH = layout.headerHeight();
 */
class UILayout
{
public:
    explicit UILayout(const juce::Component* component = nullptr)
        : comp(component)
        , scaleFactor(DPIScaleManager::getEffectiveScale(component))
    {
    }

    // ========== Piano Roll Layout ==========

    /** Width of the piano keys sidebar (logical: 30px) */
    int pianoKeysWidth() const { return scaled(30); }

    /** Height of the timeline header (logical: 18px) */
    int timelineHeight() const { return scaled(18); }

    /** Height of the loop timeline (logical: 16px) */
    int loopTimelineHeight() const { return scaled(16); }

    /** Combined header height (timeline + loop timeline) */
    int headerHeight() const { return timelineHeight() + loopTimelineHeight(); }

    /** Width of a single piano key (logical: 30px) */
    int pianoKeyWidth() const { return scaled(30); }

    /** Height of a white piano key (logical: 20px) */
    int whiteKeyHeight() const { return scaled(20); }

    /** Height of a black piano key (logical: 12px) */
    int blackKeyHeight() const { return scaled(12); }

    // ========== Toolbar & Menu ==========

    /** Height of the main toolbar (logical: 48px) */
    int toolbarHeight() const { return scaled(48); }

    /** Height of the menu bar (logical: 28px) */
    int menuBarHeight() const { return scaled(28); }

    /** Standard button height (logical: 32px) */
    int buttonHeight() const { return scaled(32); }

    /** Standard button width (logical: 80px) */
    int buttonWidth() const { return scaled(80); }

    /** Icon size for toolbar buttons (logical: 24px) */
    int toolbarIconSize() const { return scaled(24); }

    /** Small icon size (logical: 16px) */
    int smallIconSize() const { return scaled(16); }

    // ========== Spacing & Margins ==========

    /** Standard padding (logical: 8px) */
    int padding() const { return scaled(8); }

    /** Large padding (logical: 16px) */
    int largePadding() const { return scaled(16); }

    /** Small padding (logical: 4px) */
    int smallPadding() const { return scaled(4); }

    /** Standard margin (logical: 12px) */
    int margin() const { return scaled(12); }

    /** Standard gap between elements (logical: 8px) */
    int gap() const { return scaled(8); }

    /** Border radius for rounded elements (logical: 6px) */
    int borderRadius() const { return scaled(6); }

    /** Standard border width (logical: 1px, minimum 1) */
    int borderWidth() const { return std::max(1, scaled(1)); }

    // ========== Text & Labels ==========

    /** Standard text box width (logical: 60px) */
    int textBoxWidth() const { return scaled(60); }

    /** Standard text box height (logical: 20px) */
    int textBoxHeight() const { return scaled(20); }

    /** Label height (logical: 20px) */
    int labelHeight() const { return scaled(20); }

    // ========== Sliders & Controls ==========

    /** Slider track height (logical: 4px) */
    int sliderTrackHeight() const { return scaled(4); }

    /** Slider thumb diameter (logical: 16px) */
    int sliderThumbSize() const { return scaled(16); }

    /** Rotary knob diameter (logical: 48px) */
    int knobSize() const { return scaled(48); }

    /** Combo box height (logical: 24px) */
    int comboBoxHeight() const { return scaled(24); }

    // ========== Scrollbars ==========

    /** Scrollbar width (logical: 12px) */
    int scrollbarWidth() const { return scaled(12); }

    /** Scrollbar thumb minimum length (logical: 30px) */
    int scrollbarThumbMinLength() const { return scaled(30); }

    // ========== Panels & Windows ==========

    /** Sidebar width (logical: 200px) */
    int sidebarWidth() const { return scaled(200); }

    /** Parameter panel width (logical: 280px) */
    int parameterPanelWidth() const { return scaled(280); }

    /** Minimum window width (logical: 960px) */
    int minWindowWidth() const { return scaled(960); }

    /** Minimum window height (logical: 600px) */
    int minWindowHeight() const { return scaled(600); }

    // ========== Font Sizes ==========

    /** Standard font size (logical: 15px) */
    float fontSize() const { return scaledFont(15.0f); }

    /** Small font size (logical: 13px) */
    float smallFontSize() const { return scaledFont(13.0f); }

    /** Large font size (logical: 18px) */
    float largeFontSize() const { return scaledFont(18.0f); }

    /** Title font size (logical: 20px) */
    float titleFontSize() const { return scaledFont(20.0f); }

    /** Header font size (logical: 28px) */
    float headerFontSize() const { return scaledFont(28.0f); }

    // ========== Utility ==========

    /** Get the current scale factor */
    float getScaleFactor() const { return scaleFactor; }

    /** Scale an arbitrary integer value */
    int scaled(int logicalPixels) const
    {
        return juce::roundToInt(static_cast<float>(logicalPixels) * scaleFactor);
    }

    /** Scale an arbitrary float value */
    float scaled(float logicalPixels) const
    {
        return logicalPixels * scaleFactor;
    }

    /** Scale a font size with dampening */
    float scaledFont(float baseFontSize) const
    {
        // Use dampened scaling for fonts (same formula as DPIScaleManager)
        float fontScale = 1.0f + (scaleFactor - 1.0f) * 0.7f;
        return baseFontSize * fontScale;
    }

private:
    const juce::Component* comp;
    float scaleFactor;
};

/**
 * Static UI constants for use when no component context is available.
 * These return unscaled logical pixel values.
 */
namespace UIConstants
{
    // Piano Roll
    constexpr int kPianoKeysWidth = 30;
    constexpr int kTimelineHeight = 18;
    constexpr int kLoopTimelineHeight = 16;
    constexpr int kHeaderHeight = kTimelineHeight + kLoopTimelineHeight;

    // Toolbar & Menu
    constexpr int kToolbarHeight = 48;
    constexpr int kMenuBarHeight = 28;
    constexpr int kButtonHeight = 32;
    constexpr int kButtonWidth = 80;
    constexpr int kToolbarIconSize = 24;
    constexpr int kSmallIconSize = 16;

    // Spacing
    constexpr int kPadding = 8;
    constexpr int kLargePadding = 16;
    constexpr int kSmallPadding = 4;
    constexpr int kMargin = 12;
    constexpr int kGap = 8;
    constexpr int kBorderRadius = 6;

    // Controls
    constexpr int kTextBoxWidth = 60;
    constexpr int kTextBoxHeight = 20;
    constexpr int kSliderThumbSize = 16;
    constexpr int kKnobSize = 48;
    constexpr int kComboBoxHeight = 24;
    constexpr int kScrollbarWidth = 12;

    // Panels
    constexpr int kSidebarWidth = 200;
    constexpr int kParameterPanelWidth = 280;
    constexpr int kMinWindowWidth = 960;
    constexpr int kMinWindowHeight = 600;

    // Font sizes (logical)
    constexpr float kFontSize = 15.0f;
    constexpr float kSmallFontSize = 13.0f;
    constexpr float kLargeFontSize = 18.0f;
    constexpr float kTitleFontSize = 20.0f;
    constexpr float kHeaderFontSize = 28.0f;
}
