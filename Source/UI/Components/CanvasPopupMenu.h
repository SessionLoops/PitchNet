#pragma once

#include "PitchPopupMenu.h"
#include "../../Utils/Localization.h"
#include "BinaryData.h"
#include <array>

namespace canvasPopupMenu
{
class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
    }

    void drawPopupMenuBackground(juce::Graphics&, int, int) override {}
    void drawResizableFrame(juce::Graphics&, int, int,
                            const juce::BorderSize<int>&) override {}
    int getPopupMenuBorderSize() override { return 0; }
    // The upper action paints its own outline; keep the tool strip unframed.
    int getMenuWindowFlags() override { return 0; }
};

inline LookAndFeel& getLookAndFeel()
{
    static LookAndFeel instance;
    return instance;
}

class ActionButton final : public juce::Button
{
public:
    explicit ActionButton(const juce::String& label) : juce::Button(label) {}
    juce::Image sprite;

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        if (sprite.isValid())
        {
            const int frameHeight = sprite.getHeight() / 3;
            g.drawImage(sprite, 0, 0, getWidth(), getHeight(), 0,
                        (down ? 2 : over ? 1 : 0) * frameHeight,
                        sprite.getWidth(), frameHeight);
            return;
        }
        if (isEnabled() && (over || down || hasKeyboardFocus(false)))
        {
            g.setColour(juce::Colour(0xFF171717u));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2, 1), 5);
        }
        g.setColour(APP_COLOR_TEXT_PRIMARY.withMultipliedAlpha(isEnabled() ? 1.0f : 0.4f));
        g.setFont(AppFont::getFont(14));
        g.drawText(getButtonText(), getLocalBounds().withTrimmedLeft(12),
                   juce::Justification::centredLeft);
    }
};

class Content final : public juce::PopupMenu::CustomComponent
{
public:
    static juce::Rectangle<int> getTargetArea(juce::Point<int> mousePosition)
    {
        const juce::Point<int> selectCentre(buttonWidth / 2,
                                           topHeight + 1 + buttonHeight / 2);
        // JUCE places the menu below the target rectangle's bottom edge.
        return { mousePosition.x - selectCentre.x,
                 mousePosition.y - selectCentre.y - 1, 1, 1 };
    }

    Content(std::shared_ptr<int> result, bool canUndo, bool canRedo)
        : juce::PopupMenu::CustomComponent(false), choice(std::move(result))
    {
        const juce::String names[] = {
            TR("canvas.select_all"), TR("canvas.select"), TR("canvas.split"),
            TR("canvas.drawing"),    TR("canvas.timing"), TR("command.undo"),
            TR("command.redo")
        };
        const void* data[] = { BinaryData::selectmenu_png, BinaryData::splitmenu_png,
                               BinaryData::drawingmenu_png, BinaryData::timingmenu_png };
        const int sizes[] = { BinaryData::selectmenu_pngSize, BinaryData::splitmenu_pngSize,
                              BinaryData::drawingmenu_pngSize, BinaryData::timingmenu_pngSize };
        for (int i = 0; i < 7; ++i)
        {
            buttons[i] = std::make_unique<ActionButton>(names[i]);
            if (i > 0 && i < 5)
                buttons[i]->sprite = juce::ImageCache::getFromMemory(data[i - 1], sizes[i - 1]);
            buttons[i]->onClick = [this, i]
            {
                *choice = i + 1;
                triggerMenuItem();
            };
            addAndMakeVisible(*buttons[i]);
        }
        buttons[5]->setEnabled(canUndo);
        buttons[6]->setEnabled(canRedo);
        topWidth = juce::GlyphArrangement::getStringWidthInt(
                      AppFont::getFont(14), names[0]) + 24;
    }

    void getIdealSize(int& width, int& height) override
    {
        width = juce::jmax(topWidth, 4 * buttonWidth + 3);
        height = topHeight + 1 + buttonHeight;
    }

    void paint(juce::Graphics& g) override
    {
        pitchPopupMenu::getLookAndFeel().drawPopupMenuBackground(g, topWidth, topHeight);
        g.setColour(juce::Colour(0xFF3E3E3Eu));
        g.fillRect(8, padding + 2 * rowHeight + separatorHeight / 2,
                   topWidth - 16, 1);
    }

    void resized() override
    {
        buttons[5]->setBounds(0, padding, topWidth, rowHeight);
        buttons[6]->setBounds(0, padding + rowHeight, topWidth, rowHeight);
        buttons[0]->setBounds(0, padding + 2 * rowHeight + separatorHeight,
                              topWidth, rowHeight);
        for (int i = 1; i < 5; ++i)
            buttons[i]->setBounds((i - 1) * (buttonWidth + 1), topHeight + 1,
                                  buttonWidth, buttonHeight);
    }

private:
    static constexpr int padding = 4, rowHeight = 26, separatorHeight = 8;
    static constexpr int topHeight = 2 * padding + 3 * rowHeight + separatorHeight;
    static constexpr int buttonWidth = 22, buttonHeight = 21;
    int topWidth = 0;
    std::shared_ptr<int> choice;
    std::array<std::unique_ptr<ActionButton>, 7> buttons;
};
}
