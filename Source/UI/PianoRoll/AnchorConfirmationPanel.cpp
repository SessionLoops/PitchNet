#include "AnchorConfirmationPanel.h"
#include "../Components/AppFont.h"
#include "../../Utils/UI/Theme.h"
#include "BinaryData.h"

AnchorConfirmationPanel::AnchorConfirmationPanel()
{
    prompt.setText("Apply new pitch curve?", juce::dontSendNotification);
    prompt.setFont(AppFont::getFont(11.0f).withPointHeight(11.0f));
    prompt.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    prompt.setJustificationType(juce::Justification::centredLeft);

    cancelButton.setImage(juce::ImageFileFormat::loadFrom(
        BinaryData::cancel_png, static_cast<size_t>(BinaryData::cancel_pngSize)));
    okButton.setImage(juce::ImageFileFormat::loadFrom(
        BinaryData::ok_png, static_cast<size_t>(BinaryData::ok_pngSize)));
    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    okButton.onClick = [this] { if (onApply) onApply(); };

    addAndMakeVisible(prompt);
    addAndMakeVisible(cancelButton);
    addAndMakeVisible(okButton);
}

AnchorConfirmationPanel::~AnchorConfirmationPanel() = default;

void AnchorConfirmationPanel::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF2E2E2D));
    g.fillRoundedRectangle(bounds, 8.0f);
    g.setColour(APP_COLOR_BORDER.withAlpha(0.65f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
}

void AnchorConfirmationPanel::resized()
{
    auto area = getLocalBounds().reduced(7, 2);
    constexpr int buttonSize = 26;
    constexpr int buttonGap = 0;
    auto buttons = area.removeFromRight(buttonSize * 2 + buttonGap);
    prompt.setBounds(area);
    cancelButton.setBounds(buttons.removeFromLeft(buttonSize)
                               .withSizeKeepingCentre(buttonSize, buttonSize));
    buttons.removeFromLeft(buttonGap);
    okButton.setBounds(buttons.withSizeKeepingCentre(buttonSize, buttonSize));
}
