#include "QuantizePitchDialog.h"
#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "../Components/StyledWidgets.h"
#include "../../Utils/UI/Theme.h"
#include <utility>

namespace QuantizePitchDialog {
namespace {

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
    pitchCenter.setTextValueSuffix(" %");
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
  juce::Label pitchCenterLabel;
  MacroSlider pitchCenter;
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
