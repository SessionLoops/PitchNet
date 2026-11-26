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

    // Separators as the canvas menu draws its own: a 1 px rule inset 8 px.
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon,
                           const juce::Colour* textColour) override
    {
        if (isSeparator)
        {
            g.setColour(juce::Colour(0xFF3E3E3Eu));
            g.fillRect(area.getX() + 8, area.getCentreY(), area.getWidth() - 16, 1);
            return;
        }
        juce::LookAndFeel_V4::drawPopupMenuItem(g, area, isSeparator, isActive,
                                                isHighlighted, isTicked, hasSubMenu,
                                                text, shortcutKeyText, icon, textColour);
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

    /** Shown right-aligned and dimmed, as a key hint ("⌘C"). */
    void setShortcutText(juce::String text) { shortcutText = std::move(text); }

    void getIdealSize(int& idealWidth, int& idealHeight) override
    {
        const int textWidth = juce::GlyphArrangement::getStringWidthInt(
            AppFont::getFont(14.0f), itemText);
        idealWidth = textWidth + (reservesSelectionSpace ? 44 : 24);
        if (shortcutText.isNotEmpty())
            idealWidth += kShortcutGap + juce::GlyphArrangement::getStringWidthInt(
                                             AppFont::getFont(13.0f), shortcutText);
        const auto* menuItem = getItem();
        if (menuItem != nullptr && menuItem->subMenu != nullptr)
            idealWidth += kShortcutGap;
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

        const auto* menuItem = getItem();
        const bool itemEnabled = menuItem == nullptr || menuItem->isEnabled;
        g.setColour(itemEnabled ? APP_COLOR_TEXT_PRIMARY
                                : APP_COLOR_TEXT_PRIMARY.withMultipliedAlpha(0.35f));
        g.setFont(AppFont::getFont(14.0f));
        g.drawText(itemText, getLocalBounds().withTrimmedLeft(
                                  reservesSelectionSpace ? 22 : 12),
                   juce::Justification::centredLeft, true);

        const auto right = getLocalBounds().withTrimmedRight(12);
        if (shortcutText.isNotEmpty())
        {
            g.setColour(APP_COLOR_TEXT_PRIMARY.withMultipliedAlpha(itemEnabled ? 0.5f : 0.25f));
            g.setFont(AppFont::getFont(13.0f));
            g.drawText(shortcutText, right, juce::Justification::centredRight, false);
        }
        if (menuItem != nullptr && menuItem->subMenu != nullptr)
        {
            // A small chevron for a submenu.
            const float x = static_cast<float>(right.getRight()) - 4.0f;
            const float cy = area.getCentreY();
            juce::Path chevron;
            chevron.startNewSubPath(x - 3.0f, cy - 4.0f);
            chevron.lineTo(x + 1.0f, cy);
            chevron.lineTo(x - 3.0f, cy + 4.0f);
            g.setColour(APP_COLOR_TEXT_PRIMARY.withMultipliedAlpha(itemEnabled ? 0.7f : 0.3f));
            g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
        }
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
    static constexpr int kShortcutGap = 24;
    juce::String itemText;
    juce::String shortcutText;
    bool isSelected = false;
    std::function<void()> onHover;
    bool reservesSelectionSpace = true;
};
}
