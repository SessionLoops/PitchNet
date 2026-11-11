#pragma once

#include "../JuceHeader.h"

class SliderBox : public juce::Slider
{
public:
    explicit SliderBox(juce::String helpText = {});
    ~SliderBox() override;

    double snapValue(double attemptedValue, DragMode dragMode) override;
    void setEnabled(bool shouldBeEnabled);
    void setHideSuffixWhileEditing(bool shouldHide);
    bool shouldHideSuffixWhileEditing() const;

private:
    bool hideSuffixWhileEditing = false;
};

class FloatingZoomSliderLookAndFeel final : public juce::LookAndFeel_V4
{
public:
  void drawLinearSlider(juce::Graphics &g,
                        int x,
                        int y,
                        int width,
                        int height,
                        float sliderPos,
                        float minSliderPos,
                        float maxSliderPos,
                        const juce::Slider::SliderStyle style,
                        juce::Slider &slider) override;
};
