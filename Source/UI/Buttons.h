/*
  ==============================================================================

    Buttons.h
    Created: 23 Jun 2025 10:21:52am
    Author:  200gaga

  ==============================================================================
*/

#pragma once
#include "../JuceHeader.h"

class Button : public juce::TextButton
{
public:
    Button(juce::Image img = juce::Image());
    void paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void setEnabled (bool shouldBeEnabled);
    void setImage(juce::Image img);
    void setTextPosition(int x, int y);
private:
    juce::Image image;
    int textX, textY;
};

class ToggleButton : public juce::ToggleButton
{
public:
    ToggleButton();
    ~ToggleButton();
    void paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void setImage(juce::Image img);
private:
    juce::Image image;
};
