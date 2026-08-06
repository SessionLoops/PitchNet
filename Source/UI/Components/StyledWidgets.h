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
 * Thin horizontal slider used by macro controls and compact side-panel cards.
 */
class MacroSliderLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        if (label.getComponentID() == "macroSliderValueBox")
        {
            const auto bounds = label.getLocalBounds().toFloat();
            g.setColour(label.findColour(juce::Label::backgroundColourId));
            g.fillRoundedRectangle(bounds, 5.0f);
            g.setColour(label.findColour(juce::Label::textColourId));
            g.setFont(label.getFont());
            g.drawFittedText(label.getText(), label.getLocalBounds(),
                             label.getJustificationType(), 1);
            return;
        }

        juce::LookAndFeel_V4::drawLabel(g, label);
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
        label->setComponentID("macroSliderValueBox");
        label->onEditorShow = [label]
        {
            if (auto* editor = label->getCurrentTextEditor())
            {
                const auto textColour =
                    editor->findColour(juce::TextEditor::textColourId);
                editor->setColour(juce::TextEditor::highlightColourId,
                                  juce::Colour(0xFF7A7D8Bu));
                editor->setColour(juce::TextEditor::outlineColourId,
                                  juce::Colour(0xFF3C3C3Cu));
                editor->setColour(juce::TextEditor::focusedOutlineColourId,
                                  juce::Colour(0xFF3C3C3Cu));
                editor->setColour(juce::CaretComponent::caretColourId, textColour);
            }
        };
        return label;
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float, float,
                          juce::Slider::SliderStyle, juce::Slider&) override
    {
        const auto bounds = juce::Rectangle<float>(
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(width), static_cast<float>(height));
        const auto track = juce::Rectangle<float>(
            bounds.getX(), bounds.getCentreY() - 1.0f, bounds.getWidth(), 2.0f);
        g.setColour(juce::Colour(0xFF494949u));
        g.fillRoundedRectangle(track, 1.0f);

        constexpr float thumbRadius = 4.5f;
        const auto thumbX = juce::jlimit(track.getX() + thumbRadius,
                                         track.getRight() - thumbRadius,
                                         sliderPos);
        g.setColour(juce::Colour(0xFF9B9B9Bu));
        g.fillEllipse(thumbX - thumbRadius, bounds.getCentreY() - thumbRadius,
                      thumbRadius * 2.0f, thumbRadius * 2.0f);
    }

    static MacroSliderLookAndFeel& getInstance()
    {
        static MacroSliderLookAndFeel instance;
        return instance;
    }
};

class MacroSlider : public juce::Slider
{
public:
    MacroSlider()
    {
        setSliderStyle(juce::Slider::LinearHorizontal);
        setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 22);
        setLookAndFeel(&MacroSliderLookAndFeel::getInstance());
        setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xFFE6E6E6u));
        setColour(juce::Slider::textBoxBackgroundColourId,
                  juce::Colour(0xFF30302Eu));
        setColour(juce::Slider::textBoxOutlineColourId,
                  juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxHighlightColourId,
                  juce::Colour(0xFF7A7D8Bu));
    }

    ~MacroSlider() override
    {
        setLookAndFeel(nullptr);
    }
};

/**
 * Pre-styled combo box with rounded, modern appearance.
 */
class SidePanelComboLookAndFeel : public DarkLookAndFeel
{
public:
    SidePanelComboLookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::PopupMenu::textColourId, APP_COLOR_TEXT_PRIMARY);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xFF171717u));
        setColour(juce::PopupMenu::highlightedTextColourId, APP_COLOR_TEXT_PRIMARY);

        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF30302Eu));
        setColour(juce::ComboBox::textColourId, juce::Colour(0xFFE6E6E6u));
        setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::ComboBox::arrowColourId, juce::Colour(0xFFE6E6E6u));
        setColour(juce::ComboBox::focusedOutlineColourId, juce::Colour(0xFF3E3E3Eu));
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override
    {
        return AppFont::getFont(13.0f);
    }

    juce::Font getPopupMenuFont() override
    {
        return AppFont::getFont(14.0f);
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int, int, int, int, juce::ComboBox& box) override
    {
        auto colour = box.findColour(juce::ComboBox::backgroundColourId);
        if (isButtonDown)
            colour = colour.brighter(0.10f);
        else if (box.isMouseOver())
            colour = colour.brighter(0.05f);

        const auto bounds = box.getLocalBounds().toFloat();
        g.setColour(colour.withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.5f));
        g.fillRoundedRectangle(bounds, 5.0f);

        if (box.hasKeyboardFocus(true))
        {
            g.setColour(box.findColour(juce::ComboBox::focusedOutlineColourId));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);
        }

        auto arrowZone = juce::Rectangle<float>(
            static_cast<float>(width) - 20.0f, 0.0f, 14.0f, static_cast<float>(height));
        juce::Path arrow;
        const auto cx = arrowZone.getCentreX();
        const auto cy = arrowZone.getCentreY();
        arrow.startNewSubPath(cx - 3.5f, cy - 1.5f);
        arrow.lineTo(cx, cy + 2.0f);
        arrow.lineTo(cx + 3.5f, cy - 1.5f);

        g.setColour(box.findColour(juce::ComboBox::arrowColourId)
                        .withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.5f));
        g.strokePath(arrow, juce::PathStrokeType(1.35f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        const auto bounds = juce::Rectangle<float>(
            0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        const auto area = bounds.reduced(0.5f);
        juce::Path shape;
        shape.addRoundedRectangle(area, 9.0f);

        g.setColour(juce::Colour(0xFF30302Eu));
        g.fillPath(shape);
        g.setColour(juce::Colour(0xFF3E3E3Eu));
        g.strokePath(shape, juce::PathStrokeType(1.0f));
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu,
                           const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon,
                           const juce::Colour* textColour) override
    {
        juce::ignoreUnused(hasSubMenu, icon, textColour);

        if (isSeparator)
        {
            g.setColour(juce::Colour(0xFF3E3E3Eu));
            g.fillRect(area.reduced(10, 0).withHeight(1).withY(area.getCentreY()));
            return;
        }

        const auto itemArea = area.toFloat();
        if (isHighlighted && isActive)
        {
            g.setColour(juce::Colour(0xFF171717u));
            g.fillRoundedRectangle(itemArea.reduced(2.0f, 1.0f), 5.0f);
        }

        if (isTicked)
        {
            g.setColour(juce::Colour(0xFFEFEFEFu));
            g.fillEllipse(8.0f, itemArea.getCentreY() - 3.5f, 7.0f, 7.0f);
        }

        g.setColour((isActive ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_TEXT_MUTED)
                        .withMultipliedAlpha(isActive ? 1.0f : 0.5f));
        g.setFont(getPopupMenuFont());

        auto textArea = area.withTrimmedLeft(22).withTrimmedRight(10);
        g.drawText(text, textArea, juce::Justification::centredLeft, true);

        if (shortcutKeyText.isNotEmpty())
        {
            g.setColour(APP_COLOR_TEXT_MUTED.withAlpha(0.6f));
            g.drawText(shortcutKeyText, textArea, juce::Justification::centredRight, true);
        }
    }

    void drawResizableFrame(juce::Graphics&, int, int,
                            const juce::BorderSize<int>&) override {}

    static SidePanelComboLookAndFeel& getInstance()
    {
        static SidePanelComboLookAndFeel instance;
        return instance;
    }
};

class StyledComboBox : public juce::ComboBox
{
public:
    StyledComboBox()
    {
        applyStyle();
    }

    void applyStyle()
    {
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF30302Eu));
        setColour(juce::ComboBox::textColourId, juce::Colour(0xFFE6E6E6u));
        setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::ComboBox::arrowColourId, juce::Colour(0xFFE6E6E6u));
        setColour(juce::ComboBox::focusedOutlineColourId, juce::Colour(0xFF3E3E3Eu));
        setLookAndFeel(&SidePanelComboLookAndFeel::getInstance());
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
