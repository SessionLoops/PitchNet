#include "SettingsOverlay.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"

//==============================================================================
// SettingsOverlay
//==============================================================================

SettingsOverlay::SettingsOverlay(SettingsManager *settingsManager,
                                 juce::AudioDeviceManager *audioDeviceManager) {
  setOpaque(false);
  setInterceptsMouseClicks(true, true);
  setWantsKeyboardFocus(true);

  settingsComponent =
      std::make_unique<SettingsComponent>(settingsManager, audioDeviceManager);
  addAndMakeVisible(settingsComponent.get());

  closeButton.setColour(juce::TextButton::buttonColourId,
                        APP_COLOR_SURFACE);
  closeButton.setColour(juce::TextButton::textColourOffId,
                        APP_COLOR_TEXT_PRIMARY);
  closeButton.setColour(juce::TextButton::buttonOnColourId,
                        APP_COLOR_SURFACE_RAISED);
  closeButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
  closeButton.setLookAndFeel(&DarkLookAndFeel::getInstance());
  closeButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  closeButton.onClick = [this]() { closeIfPossible(); };
  addAndMakeVisible(closeButton);
}

SettingsOverlay::~SettingsOverlay() { closeButton.setLookAndFeel(nullptr); }

void SettingsOverlay::paint(juce::Graphics &g) {
  g.fillAll(APP_COLOR_OVERLAY_DIM);

  if (!contentBounds.isEmpty()) {
    juce::DropShadow shadow(APP_COLOR_OVERLAY_SHADOW, 24, {0, 8});
    shadow.drawForRectangle(g, contentBounds);
  }
}

void SettingsOverlay::resized() {
  auto bounds = getLocalBounds();

  if (settingsComponent != nullptr) {
    const int preferredWidth = settingsComponent->getWidth();
    const int preferredHeight = settingsComponent->getHeight();
    const int maxWidth = juce::jmax(420, bounds.getWidth() - 80);
    const int maxHeight = juce::jmax(320, bounds.getHeight() - 80);
    const int contentWidth = juce::jmin(preferredWidth, maxWidth);
    const int contentHeight = juce::jmin(preferredHeight, maxHeight);
    contentBounds = juce::Rectangle<int>(0, 0, contentWidth, contentHeight)
                        .withCentre(bounds.getCentre());
    settingsComponent->setBounds(contentBounds);

    const int buttonSize = 24;
    closeButton.setBounds(contentBounds.getRight() - buttonSize - 10,
                          contentBounds.getY() + 8, buttonSize, buttonSize);
  }
}

void SettingsOverlay::mouseDown(const juce::MouseEvent &e) {
  if (!contentBounds.contains(e.getPosition()))
    closeIfPossible();
}

bool SettingsOverlay::keyPressed(const juce::KeyPress &key) {
  if (key == juce::KeyPress::escapeKey) {
    closeIfPossible();
    return true;
  }
  return false;
}

void SettingsOverlay::closeIfPossible() {
  if (onClose)
    onClose();
}

//==============================================================================
// SettingsDialog
//==============================================================================

SettingsDialog::SettingsDialog(SettingsManager *settingsManager,
                               juce::AudioDeviceManager *audioDeviceManager)
    : DialogWindow(TR("settings.title"), APP_COLOR_BACKGROUND, true) {
  // Set opaque before any other operations - this must be done first
  setOpaque(true);

  // Create and configure content component
  settingsComponent =
      std::make_unique<SettingsComponent>(settingsManager, audioDeviceManager);

  // Ensure content component is also opaque before setting it
  if (settingsComponent)
    settingsComponent->setOpaque(true);

  // Set content before native title bar
  setContentOwned(settingsComponent.get(), false);

  // Now set native title bar after content is set and opaque
  setUsingNativeTitleBar(true);

  // Set window properties
  setResizable(false, false);

  int dialogWidth = 460;
  int dialogHeight = audioDeviceManager != nullptr ? 600 : 320;
  if (settingsComponent != nullptr) {
    dialogWidth = settingsComponent->getWidth();
    dialogHeight = settingsComponent->getHeight();
  }
  centreWithSize(dialogWidth, dialogHeight);
}

void SettingsDialog::closeButtonPressed() { setVisible(false); }

void SettingsDialog::paint(juce::Graphics &g) {
  g.fillAll(APP_COLOR_BACKGROUND);
}
