/*
  ==============================================================================

    Buttons.h
    Created: 23 Jun 2025 10:21:52am
    Author:  200gaga

  ==============================================================================
*/

#pragma once
#include "../JuceHeader.h"

#include <vector>

/** The frames of a vertical sprite strip of `count` equal bands, each copied
 *  into an image of its own: drawn scaled, a frame cannot pick up pixels of
 *  its neighbours (sampling a band of the whole strip reads across its edges). */
std::vector<juce::Image> splitSpriteFrames(const juce::Image &strip, int count);

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
    std::vector<juce::Image> frames; // normal, hover, down/disabled
    int textX, textY;
};

class ToggleButton : public juce::ToggleButton
{
public:
    ToggleButton();
    ~ToggleButton();
    void paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void setEnabled (bool shouldBeEnabled);
    void setImage(juce::Image img);
private:
    juce::Image image;
    std::vector<juce::Image> frames; // off, off hover, on, on hover
};
