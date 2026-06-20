#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/UI/Theme.h"
#include "DarkLookAndFeel.h"
#include "AppFont.h"
#include <cmath>
#include <algorithm>

/**
 * Pre-styled slider with premium dark theme colors.
 */
class StyledSlider : public juce::Slider
{
public:
    StyledSlider()
    {
        setSliderStyle(juce::Slider::LinearHorizontal);
        setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 20);
        applyStyle();
    }

    void applyStyle()
    {
        setColour(juce::Slider::backgroundColourId, APP_COLOR_SURFACE);
        setColour(juce::Slider::trackColourId, APP_COLOR_PRIMARY.withAlpha(0.65f));
        setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);
        setColour(juce::Slider::textBoxTextColourId, APP_COLOR_TEXT_PRIMARY);
        setColour(juce::Slider::textBoxBackgroundColourId, APP_COLOR_SURFACE);
        setColour(juce::Slider::textBoxOutlineColourId, APP_COLOR_BORDER_SUBTLE);
    }
};

/**
 * Pre-styled combo box with rounded, modern appearance.
 */
class StyledComboBox : public juce::ComboBox
{
public:
    StyledComboBox()
    {
        applyStyle();
    }

    void applyStyle()
    {
        setColour(juce::ComboBox::backgroundColourId, APP_COLOR_SURFACE);
        setColour(juce::ComboBox::textColourId, APP_COLOR_TEXT_PRIMARY);
        setColour(juce::ComboBox::outlineColourId, APP_COLOR_BORDER);
        setColour(juce::ComboBox::arrowColourId, APP_COLOR_TEXT_MUTED);
        setColour(juce::ComboBox::focusedOutlineColourId, APP_COLOR_PRIMARY.withAlpha(0.6f));
        setLookAndFeel(&DarkLookAndFeel::getInstance());
    }

    ~StyledComboBox() override
    {
        setLookAndFeel(nullptr);
    }
};

/**
 * Pre-styled toggle button with modern checkbox rendering.
 */
class StyledToggleButton : public juce::ToggleButton
{
public:
    StyledToggleButton(const juce::String& buttonText = {}) : juce::ToggleButton(buttonText)
    {
        applyStyle();
    }

    void applyStyle()
    {
        setColour(juce::ToggleButton::textColourId, APP_COLOR_TEXT_PRIMARY);
        setLookAndFeel(&DarkLookAndFeel::getInstance());
    }

    // Only allow clicks on the switch area, not the label text
    bool hitTest(int x, int /*y*/) override
    {
        const float h = static_cast<float>(getHeight());
        const float switchH = std::floor(std::min(h * 0.75f, 16.0f));
        const float switchW = switchH * 1.75f;
        const float switchX = 4.0f;
        return x >= 0 && x <= static_cast<int>(switchX + switchW + 4.0f);
    }

    ~StyledToggleButton() override
    {
        setLookAndFeel(nullptr);
    }
};

/**
 * Compact radio button matching VocalNet's selector rendering.
 */
class RadioButton : public juce::ToggleButton
{
public:
    explicit RadioButton(const juce::String& buttonText = {})
        : juce::ToggleButton(buttonText)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool /*down*/) override
    {
        constexpr float diameter = 16.0f;
        const juce::Rectangle<float> circle(
            4.0f, (static_cast<float>(getHeight()) - diameter) * 0.5f + 1.0f,
            diameter, diameter);

        const bool selected = getToggleState();
        g.setColour(selected ? juce::Colours::white
                             : juce::Colour(highlighted ? 0xFF9B9B9Bu : 0xFF515151u));
        g.drawEllipse(circle, 1.0f);
        if (selected)
            g.fillEllipse(circle.reduced(3.0f));

        g.setColour(selected ? juce::Colour(0xFFE6E6E6u)
                             : juce::Colour(highlighted ? 0xFF9B9B9Bu : 0xFF515151u));
        g.setFont(AppFont::getFont(15.0f));
        if (!isEnabled())
            g.setOpacity(0.5f);

        g.drawFittedText(getButtonText(),
                         getLocalBounds().withTrimmedLeft(30).withTrimmedRight(2),
                         juce::Justification::centredLeft, 1);
    }
};

/** Compact borderless selector used for beat and grid values. */
class CompactSelectionButton : public juce::TextButton
{
public:
    explicit CompactSelectionButton(const juce::String& buttonText = {})
        : juce::TextButton(buttonText)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void setEnabled(bool shouldBeEnabled)
    {
        juce::TextButton::setEnabled(shouldBeEnabled);
        setMouseCursor(shouldBeEnabled ? juce::MouseCursor::PointingHandCursor
                                       : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        auto colour = juce::Colour(0xFF30302Eu);
        if (down)
            colour = colour.brighter(0.10f);
        else if (highlighted)
            colour = colour.brighter(0.05f);

        g.setColour(colour);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 5.0f);
        g.setColour(juce::Colour(0xFFE6E6E6u)
                        .withMultipliedAlpha(isEnabled() ? 1.0f : 0.5f));
        g.setFont(AppFont::getFont(13.0f));
        g.drawFittedText(getButtonText(), getLocalBounds().reduced(3, 0),
                         juce::Justification::centred, 1);
    }
};

/**
 * Pre-styled label — muted secondary text.
 */
class StyledLabel : public juce::Label
{
public:
    StyledLabel(const juce::String& text = {})
    {
        setText(text, juce::dontSendNotification);
        setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
        setFont(AppFont::getFont(13.0f));
    }
};

/**
 * Section header label — accent-coloured, semi-bold, with subtle underline.
 */
class SectionLabel : public juce::Label
{
public:
    SectionLabel(const juce::String& text = {})
    {
        setText(text, juce::dontSendNotification);
        setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        setFont(AppFont::getBoldFont(13.0f));
    }

    void paint(juce::Graphics& g) override
    {
        // Draw the label text
        juce::Label::paint(g);

        // Subtle accent underline
        auto bounds = getLocalBounds().toFloat();
        g.setColour(APP_COLOR_PRIMARY.withAlpha(0.35f));
        g.fillRect(bounds.getX(), bounds.getBottom() - 1.0f,
                   bounds.getWidth() * 0.4f, 1.0f);
    }
};
