#include "QuantizePitchDialog.h"
#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "../Components/StyledWidgets.h"
#include "../../Utils/UI/Theme.h"
#include <utility>

namespace QuantizePitchDialog {
namespace {

class ZoomSliderLookAndFeel final : public juce::LookAndFeel_V4
{
public:
  void drawLabel(juce::Graphics &g, juce::Label &label) override
  {
    if (label.getComponentID() == "pitchCenterValueBox")
    {
      const auto bounds = label.getLocalBounds().toFloat();
      g.setColour(label.findColour(juce::Label::backgroundColourId));
      g.fillRoundedRectangle(bounds, 5.0f);
      g.setColour(label.findColour(juce::Label::textColourId));
      g.setFont(label.getFont());
      g.drawFittedText(label.getText(), label.getLocalBounds(),
                       label.getJustificationType(), 1);
      return;
    }

    juce::LookAndFeel_V4::drawLabel(g, label);
  }

  juce::Label *createSliderTextBox(juce::Slider &slider) override
  {
    auto *label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setComponentID("pitchCenterValueBox");
    label->onEditorShow = [label]
    {
      if (auto *editor = label->getCurrentTextEditor())
      {
        const auto textColour = editor->findColour(juce::TextEditor::textColourId);
        editor->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xFF7A7D8B));
        editor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xFF3C3C3C));
        editor->setColour(juce::TextEditor::focusedOutlineColourId,
                          juce::Colour(0xFF3C3C3C));
        editor->setColour(juce::CaretComponent::caretColourId, textColour);
      }
    };
    return label;
  }

  void drawLinearSlider(juce::Graphics &g, int x, int y, int width, int height,
                        float sliderPos, float, float,
                        juce::Slider::SliderStyle, juce::Slider &) override
  {
    const auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                               static_cast<float>(width), static_cast<float>(height));
    const auto track = juce::Rectangle<float>(bounds.getX(), bounds.getCentreY() - 1.0f,
                                              bounds.getWidth(), 2.0f);
    g.setColour(juce::Colour(0xFF494949));
    g.fillRoundedRectangle(track, 1.0f);
    const float thumbRadius = 4.5f;
    const auto thumbX = juce::jlimit(track.getX() + thumbRadius,
                                     track.getRight() - thumbRadius, sliderPos);
    g.setColour(juce::Colour(0xFF9B9B9B));
    g.fillEllipse(thumbX - thumbRadius, bounds.getCentreY() - thumbRadius,
                  thumbRadius * 2.0f, thumbRadius * 2.0f);
  }
};

class Content final : public juce::Component
{
public:
  Content(float initialPitchCenter, std::function<void(float, bool)> preview,
          std::function<void(bool)> completed)
      : onPreview(std::move(preview)), onComplete(std::move(completed))
  {
    pitchCenterLabel.setText("Pitch Center", juce::dontSendNotification);
    pitchCenterLabel.setFont(AppFont::getFont(15.0f));
    pitchCenterLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);

    pitchCenter.setRange(0.0, 100.0, 1.0);
    pitchCenter.setSliderStyle(juce::Slider::LinearHorizontal);
    pitchCenter.setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 22);
    pitchCenter.setTextValueSuffix(" %");
    pitchCenter.setLookAndFeel(&sliderLookAndFeel);
    pitchCenter.setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xFFE6E6E6));
    pitchCenter.setColour(juce::Slider::textBoxBackgroundColourId,
                          juce::Colour(0xFF30302E));
    pitchCenter.setColour(juce::Slider::textBoxOutlineColourId,
                          juce::Colours::transparentBlack);
    pitchCenter.setColour(juce::Slider::textBoxHighlightColourId,
                          juce::Colour(0xFF7A7D8B));
    pitchCenter.setValue(initialPitchCenter, juce::dontSendNotification);
    pitchCenter.onValueChange = [this] { previewCorrection(); };

    snapToScale.onClick = [this] { previewCorrection(); };

    setupButton(cancelButton, "Cancel");
    setupButton(okButton, "OK");
    cancelButton.onClick = [this] { finish(false); };
    okButton.onClick = [this] { finish(true); };

    addAndMakeVisible(pitchCenterLabel);
    addAndMakeVisible(pitchCenter);
    addAndMakeVisible(snapToScale);
    addAndMakeVisible(cancelButton);
    addAndMakeVisible(okButton);
  }

  ~Content() override
  {
    pitchCenter.setLookAndFeel(nullptr);
    // Covers Escape/window dismissal as well as the explicit Cancel button.
    if (!finished && onComplete)
      onComplete(false);
  }

  void cancel() { finish(false); }

  void paint(juce::Graphics &g) override
  {
    g.setColour(juce::Colour(0xFF181818));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
    g.setColour(APP_COLOR_BORDER.withAlpha(0.55f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 10.0f, 1.0f);
  }

  void resized() override
  {
    auto area = getLocalBounds().reduced(16, 10);
    auto row = area.removeFromTop(32);
    pitchCenterLabel.setBounds(row.removeFromLeft(94));
    pitchCenter.setBounds(row);
    area.removeFromTop(10);
    auto actionRow = area.removeFromTop(24);
    snapToScale.setBounds(actionRow.removeFromLeft(148));
    auto buttons = actionRow.removeFromRight(160).translated(1, 0);
    cancelButton.setBounds(buttons.removeFromLeft(75));
    buttons.removeFromLeft(10);
    okButton.setBounds(buttons);
  }

private:
  static void setupButton(juce::TextButton &button, const juce::String &text)
  {
    button.setButtonText(text);
    button.setLookAndFeel(&DarkLookAndFeel::getInstance());
    button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF5B5B5B));
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF3E3E3E));
    button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFEFEFEF));
    button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  void finish(bool accepted)
  {
    if (finished)
      return;
    finished = true;
    if (onComplete)
      onComplete(accepted);
    auto safeThis = juce::Component::SafePointer<Content>(this);
    juce::MessageManager::callAsync([safeThis]
    {
      if (auto *content = safeThis.getComponent())
      {
        if (auto *parent = content->getParentComponent())
          parent->removeChildComponent(content);
        delete content;
      }
    });
  }

  void previewCorrection()
  {
    if (onPreview)
      onPreview(static_cast<float>(pitchCenter.getValue()), snapToScale.getToggleState());
  }

  std::function<void(float, bool)> onPreview;
  std::function<void(bool)> onComplete;
  ZoomSliderLookAndFeel sliderLookAndFeel;
  juce::Label pitchCenterLabel;
  juce::Slider pitchCenter;
  StyledToggleButton snapToScale { "Snap to Scale" };
  juce::TextButton cancelButton, okButton;
  bool finished = false;
};

juce::Component::SafePointer<Content> activePopup;
} // namespace

void dismissPopup()
{
  if (auto *content = activePopup.getComponent())
    content->cancel();
}

void showPopup(juce::Component *parent, juce::Rectangle<int> anchorBounds,
               float initialPitchCenter, std::function<void(float, bool)> onPreview,
               std::function<void(bool)> onComplete)
{
  if (parent == nullptr)
    return;

  dismissPopup();

  constexpr int width = 340;
  constexpr int height = 86;
  auto *content = new Content(initialPitchCenter, std::move(onPreview), std::move(onComplete));
  content->setSize(width, height);
  const auto bounds = parent->getLocalBounds();
  const int x = juce::jlimit(8, juce::jmax(8, bounds.getWidth() - width - 8),
                             anchorBounds.getRight() - width);
  const int y = juce::jlimit(0, juce::jmax(0, bounds.getHeight() - height),
                             anchorBounds.getY());
  parent->addAndMakeVisible(content);
  content->setBounds(x, y, width, height);
  content->toFront(false);
  activePopup = content;
}

} // namespace QuantizePitchDialog
