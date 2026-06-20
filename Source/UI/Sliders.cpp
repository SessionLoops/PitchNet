#include "Sliders.h"
#include <cmath>

namespace
{
class SliderBoxLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle style,
                          juce::Slider& slider) override
    {
        if (slider.isBar())
        {
            g.setColour(slider.findColour(juce::Slider::backgroundColourId));
            g.fillRoundedRectangle(slider.getLocalBounds().toFloat(), 5.0f);
            return;
        }

        juce::LookAndFeel_V4::drawLinearSlider(
            g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos,
            style, slider);
    }
};

SliderBoxLookAndFeel& getSliderBoxLookAndFeel()
{
    static SliderBoxLookAndFeel lookAndFeel;
    return lookAndFeel;
}
}

SliderBox::SliderBox(juce::String helpText)
{
    setHelpText(helpText);
    setSliderStyle(juce::Slider::LinearBarVertical);
    setSliderSnapsToMousePosition(false);
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    setLookAndFeel(&getSliderBoxLookAndFeel());
}

SliderBox::~SliderBox()
{
    setLookAndFeel(nullptr);
}

double SliderBox::snapValue(double attemptedValue, DragMode dragMode)
{
    return isEnabled() && dragMode == DragMode::absoluteDrag
        ? std::round(attemptedValue)
        : attemptedValue;
}

void SliderBox::setEnabled(bool shouldBeEnabled)
{
    juce::Slider::setEnabled(shouldBeEnabled);
    setMouseCursor(shouldBeEnabled ? juce::MouseCursor::UpDownResizeCursor
                                   : juce::MouseCursor::NormalCursor);
}
