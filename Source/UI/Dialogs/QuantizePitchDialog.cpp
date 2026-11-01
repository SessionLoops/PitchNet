#include "QuantizePitchDialog.h"
#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "../Components/StyledWidgets.h"
#include "../../Utils/Localization.h"
#include "../../Utils/UI/Theme.h"
#include <utility>

namespace QuantizePitchDialog {
namespace {

class Content final : public juce::Component
{
public:
  Content(juce::Range<float> pitchCenterRange, float initialPitchCenter,
          juce::Range<float> pitchDriftRange, float initialPitchDrift,
          bool initialSnapToScale,
          std::function<void(float, bool, bool, float, bool)> preview,
          std::function<void(bool)> completed, std::function<void()> refreshPreview)
      : onPreview(std::move(preview)), onComplete(std::move(completed)),
        onRefreshPreview(std::move(refreshPreview))
  {
    pitchCenterLabel.setText(TR("quantize.pitch_center"), juce::dontSendNotification);
    pitchCenterLabel.setFont(AppFont::getFont(15.0f));
    pitchCenterLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);

    pitchCenter.setRange(0.0, 100.0, 1.0);
    pitchCenter.setTextValueSuffix(" %");
    // Plain read-only value label, as in Melodyne's macro dialog.
    pitchCenter.setTextBoxStyle(juce::Slider::TextBoxRight, true, valueBoxWidth, 22);
    pitchCenter.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    pitchCenter.setValue(initialPitchCenter, juce::dontSendNotification);
    pitchCenter.setMixedRange(pitchCenterRange.getStart(), pitchCenterRange.getEnd());
    pitchCenter.onDragStart = [this] { sliderDragging = true; };
    pitchCenter.onDragEnd = [this] { sliderDragging = false; refreshCorrection(); };
    pitchCenter.onValueChange = [this] { centerEdited = true; previewCorrection(); };

    pitchDriftLabel.setText(TR("quantize.pitch_drift"), juce::dontSendNotification);
    pitchDriftLabel.setFont(AppFont::getFont(15.0f));
    pitchDriftLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    pitchDrift.setRange(0.0, 100.0, 1.0);
    pitchDrift.setTextValueSuffix(" %");
    // Plain read-only value label, as in Melodyne's macro dialog.
    pitchDrift.setTextBoxStyle(juce::Slider::TextBoxRight, true, valueBoxWidth, 22);
    pitchDrift.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    pitchDrift.setValue(initialPitchDrift, juce::dontSendNotification);
    pitchDrift.setMixedRange(pitchDriftRange.getStart(), pitchDriftRange.getEnd());
    pitchDrift.onDragStart = [this] { sliderDragging = true; };
    pitchDrift.onDragEnd = [this] { sliderDragging = false; refreshCorrection(); };
    pitchDrift.onValueChange = [this] { driftEdited = true; previewCorrection(); };

    snapToScale.setToggleState(initialSnapToScale, juce::dontSendNotification);
    snapToScale.onClick = [this] { previewCorrection(); };

    snapToScale.setButtonText(TR("quantize.snap_to_scale"));

    setupButton(cancelButton, TR("dialog.cancel"));
    setupButton(okButton, TR("dialog.ok"));
    cancelButton.onClick = [this] { finish(false); };
    okButton.onClick = [this] { finish(true); };

    addAndMakeVisible(pitchCenterLabel);
    addAndMakeVisible(pitchCenter);
    addAndMakeVisible(pitchDriftLabel);
    addAndMakeVisible(pitchDrift);
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
    area.removeFromTop(6);
    row = area.removeFromTop(32);
    pitchDriftLabel.setBounds(row.removeFromLeft(94));
    pitchDrift.setBounds(row);
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
    if (accepted) refreshCorrection();
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
    if (finished)
      return;
    if (onPreview)
      onPreview(static_cast<float>(pitchCenter.getValue()), centerEdited,
                snapToScale.getToggleState(),
                static_cast<float>(pitchDrift.getValue()), driftEdited);
    previewPending = true;
    if (!sliderDragging)
      refreshCorrection();
  }

  void refreshCorrection()
  {
    if (!previewPending || finished)
      return;
    previewPending = false;
    if (onRefreshPreview)
      onRefreshPreview();
  }

  std::function<void(float, bool, bool, float, bool)> onPreview;
  std::function<void(bool)> onComplete;
  std::function<void()> onRefreshPreview;
  bool sliderDragging = false;
  bool previewPending = false;
  juce::Label pitchCenterLabel;
  MacroSlider pitchCenter;
  juce::Label pitchDriftLabel;
  MacroSlider pitchDrift;
  bool centerEdited = false;
  bool driftEdited = false;
  static constexpr int valueBoxWidth = 88;
  StyledToggleButton snapToScale;
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
               juce::Range<float> pitchCenterRange, float initialPitchCenter,
               juce::Range<float> pitchDriftRange, float initialPitchDrift,
               bool initialSnapToScale,
               std::function<void(float, bool, bool, float, bool)> onPreview,
               std::function<void(bool)> onComplete,
               std::function<void()> onRefreshPreview)
{
  if (parent == nullptr)
    return;

  dismissPopup();

  constexpr int width = 374;
  constexpr int height = 124;
  auto *content = new Content(pitchCenterRange, initialPitchCenter,
                              pitchDriftRange, initialPitchDrift, initialSnapToScale,
                              std::move(onPreview), std::move(onComplete),
                              std::move(onRefreshPreview));
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
