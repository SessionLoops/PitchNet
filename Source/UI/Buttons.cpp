/*
  ==============================================================================

    Buttons.cpp
    Created: 23 Jun 2025 10:21:52am
    Author:  200gaga

  ==============================================================================
*/

#include "Buttons.h"

std::vector<juce::Image> splitSpriteFrames(const juce::Image &strip, int count)
{
    std::vector<juce::Image> frames;
    if (!strip.isValid() || count <= 0)
        return frames;
    const int band = strip.getHeight() / count;
    for (int k = 0; k < count; ++k)
        frames.push_back(strip.getClippedImage({0, k * band, strip.getWidth(), band}).createCopy());
    return frames;
}

Button::Button(juce::Image img)
:image(img), frames(splitSpriteFrames(img, 3)), textX(0), textY(0)
{
    if (img.isValid())
        setSize((img.getWidth() + 1) / 2, (img.getHeight() + 1) / 3 / 2);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Button::paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    if (image.isValid() && frames.size() == 3)
    {
        const auto &frame = frames[shouldDrawButtonAsDown || !isEnabled() ? 2 : (shouldDrawButtonAsHighlighted ? 1 : 0)];
        g.drawImage(frame, (getWidth() - image.getWidth() / 2) / 2, (getHeight() - image.getHeight() / 3 / 2) / 2, image.getWidth() / 2, image.getHeight() / 3 / 2, 0, 0, frame.getWidth(), frame.getHeight());
    }
    else
    {
        auto baseColour = findColour(juce::TextButton::buttonColourId).withMultipliedSaturation (hasKeyboardFocus (true) ? 1.3f : 0.9f).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f);
        if ((shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted))
            baseColour = baseColour.contrasting (shouldDrawButtonAsDown ? 0.2f : 0.05f);
        g.setColour(shouldDrawButtonAsDown ? findColour(juce::TextButton::buttonOnColourId) : baseColour);
        g.fillRoundedRectangle(0, 0, getWidth(), getHeight(), getWidth() > 40 ? 13.f : 3.5f);
    }
    g.setFont(getLookAndFeel().getTextButtonFont(*this, getHeight()));
    g.setColour(isEnabled() ? findColour(shouldDrawButtonAsHighlighted ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId) : juce::Colour(0xFF3E3E3E));
    g.drawText(getButtonText(), getLocalBounds().withX(textX).withY(textY), juce::Justification::centred);
}

void Button::setEnabled (bool shouldBeEnabled)
{
    juce::TextButton::setEnabled(shouldBeEnabled);
    setMouseCursor(shouldBeEnabled ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void Button::setImage(juce::Image img)
{
    image = img;
    frames = splitSpriteFrames(img, 3);
    setSize((img.getWidth() + 1) / 2, (img.getHeight() + 1) / 3 / 2);
    repaint();
}

void Button::setTextPosition(int x, int y)
{
    textX = x;
    textY = y;
}

ToggleButton::ToggleButton()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

ToggleButton::~ToggleButton()
{
    
}

void ToggleButton::setEnabled (bool shouldBeEnabled)
{
    juce::ToggleButton::setEnabled(shouldBeEnabled);
    setMouseCursor(shouldBeEnabled ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void ToggleButton::setImage(juce::Image img)
{
    image = img;
    frames = splitSpriteFrames(img, 4);
    setSize((img.getWidth() + 1) / 2, (img.getHeight() + 1) / 4 / 2);
    repaint();
}

void ToggleButton::paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    setAlpha(isEnabled() ? 1.f : 0.3f);
    if (image.isValid() && frames.size() == 4)
    {
        const auto &frame = frames[static_cast<size_t>((shouldDrawButtonAsHighlighted ? 1 : 0) + (getToggleState() ? 2 : 0))];
        g.drawImage(frame, (getWidth() - (image.getWidth() + 1) / 2) / 2, (getHeight() - (image.getHeight() / 4 + 1) / 2) / 2, (image.getWidth() + 1) / 2, (image.getHeight() / 4  + 1) / 2, 0, 0, frame.getWidth(), frame.getHeight());
    }
    else
    {
        g.setColour(juce::Colour(shouldDrawButtonAsHighlighted ? 0xFF727272 : 0xFF565656));
        g.fillRoundedRectangle(0, 0, getWidth(), getHeight(), getWidth() > 40 ? 13.f : 3.5f);
    }
    g.setFont(juce::Font("Montserrat", "Medium", 14.f).withPointHeight(14.f));
    g.setColour(juce::Colour(getToggleState() ? 0xFFE6E6E6 : 0xFF9B9B9B));
    g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
}