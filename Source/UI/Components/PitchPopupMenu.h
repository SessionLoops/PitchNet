#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/UI/Theme.h"
#include "AppFont.h"

#include <functional>
#include <utility>

namespace pitchPopupMenu
{
class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId,
                  juce::Colours::transparentBlack);
        setColour(juce::PopupMenu::textColourId, APP_COLOR_TEXT_PRIMARY);
        setColour(juce::PopupMenu::highlightedBackgroundColourId,
                  juce::Colour(0xFF171717u));
        setColour(juce::PopupMenu::highlightedTextColourId,
                  APP_COLOR_TEXT_PRIMARY);
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width,
                                 int height) override
    {
        const auto bounds = juce::Rectangle<float>(
            0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        juce::Path shape;
        shape.addRoundedRectangle(bounds.reduced(0.5f), 9.0f);

        g.setColour(juce::Colour(0xFF30302Eu));
        g.fillPath(shape);
        g.setColour(juce::Colour(0xFF3E3E3Eu));
        g.strokePath(shape, juce::PathStrokeType(1.0f));
    }

    void drawResizableFrame(juce::Graphics&, int, int,
                            const juce::BorderSize<int>&) override
    {
    }
};

inline LookAndFeel& getLookAndFeel()
{
    static LookAndFeel lookAndFeel;
    return lookAndFeel;
}

class MenuItemComponent final : public juce::PopupMenu::CustomComponent
{
public:
    MenuItemComponent(juce::String text, bool selected = false,
                      std::function<void()> hoverCallback = {},
                      bool reserveSelectionSpace = true)
        : juce::PopupMenu::CustomComponent(true),
          itemText(std::move(text)),
          isSelected(selected),
          onHover(std::move(hoverCallback)),
          reservesSelectionSpace(reserveSelectionSpace)
    {
        setOpaque(false);
    }

    void getIdealSize(int& idealWidth, int& idealHeight) override
    {
        const int textWidth = juce::GlyphArrangement::getStringWidthInt(
            AppFont::getFont(14.0f), itemText);
        idealWidth = textWidth + (reservesSelectionSpace ? 44 : 24);
        idealHeight = 26;
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        if (isItemHighlighted())
        {
            g.setColour(juce::Colour(0xFF171717u));
            g.fillRoundedRectangle(area.reduced(2.0f, 1.0f), 5.0f);
        }

        if (isSelected)
        {
            g.setColour(juce::Colour(0xFFEFEFEFu));
            g.fillEllipse(8.0f, area.getCentreY() - 3.5f, 7.0f, 7.0f);
        }

        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getFont(14.0f));
        g.drawText(itemText, getLocalBounds().withTrimmedLeft(
                                  reservesSelectionSpace ? 22 : 12),
                   juce::Justification::centredLeft, true);
    }

    void mouseEnter(const juce::MouseEvent& event) override
    {
        juce::PopupMenu::CustomComponent::mouseEnter(event);
        if (onHover)
            onHover();
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        juce::PopupMenu::CustomComponent::mouseMove(event);
        if (onHover)
            onHover();
    }

private:
    juce::String itemText;
    bool isSelected = false;
    std::function<void()> onHover;
    bool reservesSelectionSpace = true;
};
}
