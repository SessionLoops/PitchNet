#include "Sliders.h"
#include <cmath>

namespace
{
class SliderBoxLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);

        label->onEditorShow = [label, &slider]
        {
            if (auto* editor = label->getCurrentTextEditor())
            {
                if (const auto* sliderBox = dynamic_cast<SliderBox*>(&slider);
                    sliderBox != nullptr && sliderBox->shouldHideSuffixWhileEditing())
                {
                    editor->setText(
                        juce::String(slider.getValue(),
                                     slider.getNumDecimalPlacesToDisplay()),
                        false);
                }

                const auto textColour =
                    editor->findColour(juce::TextEditor::textColourId);
                editor->setColour(juce::TextEditor::highlightColourId,
                                  juce::Colour(0xFF7A7D8Bu));
                editor->setColour(juce::TextEditor::outlineColourId,
                                  juce::Colour(0xFF3C3C3Cu));
                editor->setColour(juce::TextEditor::focusedOutlineColourId,
                                  juce::Colour(0xFF3C3C3Cu));
                editor->setColour(juce::CaretComponent::caretColourId,
                                  textColour);
            }
        };

        return label;
    }

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
    setColour(juce::Slider::textBoxHighlightColourId,
              juce::Colour(0xFF7A7D8Bu));
    setColour(juce::Slider::textBoxOutlineColourId,
              juce::Colour(0xFF3C3C3Cu));
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

void SliderBox::setHideSuffixWhileEditing(bool shouldHide)
{
    hideSuffixWhileEditing = shouldHide;
}

bool SliderBox::shouldHideSuffixWhileEditing() const
{
    return hideSuffixWhileEditing;
}
