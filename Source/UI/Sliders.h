#pragma once

#include "../JuceHeader.h"

class SliderBox : public juce::Slider
{
public:
    explicit SliderBox(juce::String helpText = {});
    ~SliderBox() override;

    double snapValue(double attemptedValue, DragMode dragMode) override;
    void setEnabled(bool shouldBeEnabled);
};
