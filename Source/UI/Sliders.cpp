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

void FloatingZoomSliderLookAndFeel::drawLinearSlider(
    juce::Graphics &g,
    int x,
    int y,
    int width,
    int height,
    float sliderPos,
    float minSliderPos,
    float maxSliderPos,
    const juce::Slider::SliderStyle style,
    juce::Slider &slider)
{
  juce::ignoreUnused(minSliderPos, maxSliderPos, slider);

  const auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                             static_cast<float>(y),
                                             static_cast<float>(width),
                                             static_cast<float>(height));
  const auto thumbRadius = 4.5f;
  const auto trackHeight = 2.0f;

  if (style == juce::Slider::LinearVertical)
  {
    const auto track = juce::Rectangle<float>(
        bounds.getCentreX() - trackHeight * 0.5f,
        bounds.getY(),
        trackHeight,
        juce::jmax(0.0f, bounds.getHeight()));

    g.setColour(juce::Colour(0xFF494949));
    g.fillRoundedRectangle(track, trackHeight * 0.5f);

    const auto clampedSliderPos = juce::jlimit(track.getY() + thumbRadius,
                                               track.getBottom() - thumbRadius,
                                               sliderPos);
    g.setColour(juce::Colour(0xFF9B9B9B));
    g.fillEllipse(bounds.getCentreX() - thumbRadius,
                  clampedSliderPos - thumbRadius,
                  thumbRadius * 2.0f,
                  thumbRadius * 2.0f);
    return;
  }

  const auto track = juce::Rectangle<float>(bounds.getX(),
                                           bounds.getCentreY() - trackHeight * 0.5f,
                                           juce::jmax(0.0f, bounds.getWidth()),
                                           trackHeight);

  g.setColour(juce::Colour(0xFF494949));
  g.fillRoundedRectangle(track, trackHeight * 0.5f);

  const auto clampedSliderPos = juce::jlimit(track.getX() + thumbRadius,
                                             track.getRight() - thumbRadius,
                                             sliderPos);
  g.setColour(juce::Colour(0xFF9B9B9B));
  g.fillEllipse(clampedSliderPos - thumbRadius,
                bounds.getCentreY() - thumbRadius,
                thumbRadius * 2.0f,
                thumbRadius * 2.0f);
}
