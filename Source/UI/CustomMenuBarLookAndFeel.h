#pragma once

#include "../JuceHeader.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/Theme.h"
#include "Components/AppFont.h"

class CustomMenuBarLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CustomMenuBarLookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::PopupMenu::textColourId, APP_COLOR_TEXT_PRIMARY);
        setColour(juce::PopupMenu::headerTextColourId, APP_COLOR_TEXT_PRIMARY);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, APP_COLOR_PRIMARY.withAlpha(0.25f));
        setColour(juce::PopupMenu::highlightedTextColourId, APP_COLOR_TEXT_PRIMARY);
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        const auto bounds = juce::Rectangle<float>(
            0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        const auto area = bounds.reduced(0.5f);
        juce::Path shape;
        shape.addRoundedRectangle(area, 8.0f);

        g.setColour(APP_COLOR_SURFACE_ALT.withMultipliedBrightness(0.92f));
        g.fillPath(shape);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.6f));
        g.strokePath(shape, juce::PathStrokeType(0.75f));
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                          bool isSeparator, bool isActive, bool isHighlighted,
                          bool isTicked, bool hasSubMenu,
                          const juce::String& text,
                          const juce::String& shortcutKeyText,
                          const juce::Drawable* icon,
                          const juce::Colour* textColour) override
    {
        if (isSeparator)
        {
            auto r = area.reduced(10, 0);
            r.removeFromTop(r.getHeight() / 2 - 1);
            g.setColour(APP_COLOR_BORDER_SUBTLE);
            g.fillRect(r.removeFromTop(1));
        }
        else
        {
            auto textColourToUse = textColour != nullptr ? *textColour
                                 : findColour(juce::PopupMenu::textColourId);

            if (isHighlighted && isActive)
            {
                // Pill-shaped highlight
                g.setColour(juce::Colour(0xFF191818u));
                g.fillRoundedRectangle(area.toFloat().reduced(4.0f, 1.0f), 6.0f);
                textColourToUse = APP_COLOR_TEXT_PRIMARY;
            }

            if (!isActive)
                textColourToUse = textColourToUse.withAlpha(0.4f);

            auto r = area.reduced(1);
            g.setColour(textColourToUse);
            g.setFont(getPopupMenuFont());

            auto iconArea = r.removeFromLeft(r.getHeight()).toFloat().reduced(2);

            if (icon != nullptr)
                icon->drawWithin(g, iconArea, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize, 1.0f);
            else if (isTicked)
            {
                auto tick = getTickShape(1.0f);
                g.fillPath(tick, tick.getTransformToScaleToFit(iconArea, true));
            }

            if (hasSubMenu)
            {
                auto arrowH = 0.6f * getPopupMenuFont().getAscent();
                auto x = static_cast<float>(r.removeFromRight((int)arrowH).getX());
                auto halfH = static_cast<float>(r.getCentreY());

                juce::Path path;
                path.addTriangle(x, halfH - arrowH * 0.5f,
                               x, halfH + arrowH * 0.5f,
                               x + arrowH * 0.6f, halfH);

                g.fillPath(path);
            }

            r.removeFromRight(3);
            g.drawFittedText(text, r, juce::Justification::centredLeft, 1);

            if (shortcutKeyText.isNotEmpty())
            {
                g.setFont(AppFont::getFont(12.0f));
                g.setColour(textColourToUse.withAlpha(0.6f));
                g.drawText(shortcutKeyText, r, juce::Justification::centredRight, true);
            }
        }
    }

    void drawResizableFrame(juce::Graphics&, int, int,
                            const juce::BorderSize<int>&) override
    {
        // Suppress square frame around rounded popup
    }

    void drawMenuBarBackground(juce::Graphics& g, int width, int height,
                              bool, juce::MenuBarComponent&) override
    {
        g.fillAll(APP_COLOR_SURFACE_ALT);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawLine(0.0f, static_cast<float>(height) - 0.5f,
                   static_cast<float>(width), static_cast<float>(height) - 0.5f, 1.0f);
    }

    void drawMenuBarItem(juce::Graphics& g, int width, int height,
                        int itemIndex, const juce::String& itemText,
                        bool isMouseOverItem, bool isMenuOpen,
                        bool, juce::MenuBarComponent&) override
    {
        if (isMenuOpen || isMouseOverItem)
        {
            // Subtle rounded highlight instead of solid primary block
            g.setColour(juce::Colour(0xFF191818u));
            g.fillRoundedRectangle(juce::Rectangle<float>(1.0f, 2.0f,
                static_cast<float>(width) - 2.0f, static_cast<float>(height) - 4.0f), 5.0f);
        }

        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getFont(static_cast<float>(height) * 0.6f));
        g.drawFittedText(itemText, 0, 0, width, height, juce::Justification::centred, 1);
    }

    juce::Font getPopupMenuFont() override
    {
        return AppFont::getFont(14.0f);
    }
};
