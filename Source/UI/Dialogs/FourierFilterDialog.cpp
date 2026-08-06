#include "FourierFilterDialog.h"
#include <algorithm>
#include <cmath>
#include <utility>

FourierFilterDialog::FourierFilterDialog(
    const std::vector<Note*>& selectedNotes,
    float frameRateHz,
    ApplyCallback onApply)
  : notes(selectedNotes),
    frameRateHz(frameRateHz),
    applyCallback(onApply) {

  // Combine deltaPitch from all notes into single curve
  for (auto* note : notes) {
    if (note && !note->getActiveDeltaPitch().empty()) {
        const auto& dp = note->getActiveDeltaPitch();
        originalPitchCurve.insert(originalPitchCurve.end(), dp.begin(), dp.end());
    }
  }

  // Configure sliders
  lowpassSlider.setRange(0.1, 20.0, 0.1);
  lowpassSlider.setValue(8.0);  // Default
  lowpassSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  lowpassSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
  lowpassSlider.onValueChange = [this]() { onSliderValueChanged(); };
  addAndMakeVisible(lowpassSlider);

  highpassSlider.setRange(0.01, 10.0, 0.01);
  highpassSlider.setValue(0.5);  // Default
  highpassSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  highpassSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
  highpassSlider.onValueChange = [this]() { onSliderValueChanged(); };
  addAndMakeVisible(highpassSlider);

  // Labels
  lowpassLabel.setText("Lowpass (Hz):", juce::dontSendNotification);
  lowpassLabel.attachToComponent(&lowpassSlider, true);
  addAndMakeVisible(lowpassLabel);

  highpassLabel.setText("Highpass (Hz):", juce::dontSendNotification);
  highpassLabel.attachToComponent(&highpassSlider, true);
  addAndMakeVisible(highpassLabel);

  // Buttons
  okButton.onClick = [this]() { onOkClicked(); };
  addAndMakeVisible(okButton);

  cancelButton.onClick = [this]() { onCancelClicked(); };
  addAndMakeVisible(cancelButton);

  // Initial filter computation
  updateFilter();

  setSize(600, 500);
}

FourierFilterDialog::~FourierFilterDialog() {
}

void FourierFilterDialog::updateFilter() {
  float lowpassHz = (float)lowpassSlider.getValue();
  float highpassHz = (float)highpassSlider.getValue();

  if (!originalPitchCurve.empty()) {
      filterResult = FourierPitchFilter::filterPitchCurve(
          originalPitchCurve, lowpassHz, highpassHz, frameRateHz);
  }

  repaint();  // Redraw spectrum
}

void FourierFilterDialog::resized() {
  auto bounds = getLocalBounds().reduced(20);

  // Spectrum view (top 60%)
  spectrumBounds = bounds.removeFromTop(int(bounds.getHeight() * 0.6));

  bounds.removeFromTop(10);  // Spacing

  // Sliders (middle)
  auto sliderHeight = 30;
  lowpassSlider.setBounds(bounds.removeFromTop(sliderHeight).withTrimmedLeft(120));
  bounds.removeFromTop(10);
  highpassSlider.setBounds(bounds.removeFromTop(sliderHeight).withTrimmedLeft(120));

  bounds.removeFromTop(20);  // Spacing

  // Buttons (bottom)
  auto buttonRow = bounds.removeFromTop(30);
  auto buttonWidth = 100;
  cancelButton.setBounds(buttonRow.removeFromRight(buttonWidth));
  buttonRow.removeFromRight(10);  // Spacing
  okButton.setBounds(buttonRow.removeFromRight(buttonWidth));
}

void FourierFilterDialog::paint(juce::Graphics& g) {
  g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

  // Draw spectrum
  drawSpectrum(g, spectrumBounds);
}

void FourierFilterDialog::drawSpectrum(juce::Graphics& g, const juce::Rectangle<int>& bounds) {
  g.setColour(juce::Colours::darkgrey);
  g.fillRect(bounds);

  g.setColour(juce::Colours::white);
  g.drawRect(bounds, 1);

  if (filterResult.magnitudeSpectrum.empty()) return;

  // Draw spectrum curve
  juce::Path spectrumPath;
  float maxMagnitude = 0.0f;
  
  if (!filterResult.magnitudeSpectrum.empty()) {
      maxMagnitude = *std::max_element(filterResult.magnitudeSpectrum.begin(),
                                             filterResult.magnitudeSpectrum.end());
  }
  
  if (maxMagnitude <= 0.0f) maxMagnitude = 1.0f;  // Avoid division by zero

  // Only draw up to Nyquist or reasonable limit? 
  // The magnitudeSpectrum usually goes up to Nyquist (frameRateHz / 2).
  
  for (size_t i = 0; i < filterResult.magnitudeSpectrum.size(); ++i) {
    float freq = filterResult.frequencyBins[i];
    float mag = filterResult.magnitudeSpectrum[i];

    // Map frequency to X (logarithmic scale usually better, but linear requested/simple for now as per instructions "visualize magnitude spectrum with frequency on X-axis")
    // Linear mapping: x = (freq / maxFreq) * width
    // Max freq is frameRateHz / 2.0f
    
    float x = bounds.getX() + (freq / (frameRateHz / 2.0f)) * bounds.getWidth();
    float y = bounds.getBottom() - (mag / maxMagnitude) * bounds.getHeight();
    
    // Clamp x to bounds just in case
    x = std::max((float)bounds.getX(), std::min((float)bounds.getRight(), x));
    y = std::max((float)bounds.getY(), std::min((float)bounds.getBottom(), y));

    if (i == 0) {
      spectrumPath.startNewSubPath(x, y);
    } else {
      spectrumPath.lineTo(x, y);
    }
  }

  g.setColour(juce::Colours::cyan);
  g.strokePath(spectrumPath, juce::PathStrokeType(2.0f));

  // Draw cutoff markers
  float lowpassHz = (float)lowpassSlider.getValue();
  float highpassHz = (float)highpassSlider.getValue();

  float lowpassX = bounds.getX() + (lowpassHz / (frameRateHz / 2.0f)) * bounds.getWidth();
  float highpassX = bounds.getX() + (highpassHz / (frameRateHz / 2.0f)) * bounds.getWidth();

  // Clamp markers
  lowpassX = std::max((float)bounds.getX(), std::min((float)bounds.getRight(), lowpassX));
  highpassX = std::max((float)bounds.getX(), std::min((float)bounds.getRight(), highpassX));

  g.setColour(juce::Colours::red.withAlpha(0.8f));
  g.drawVerticalLine((int)lowpassX, (float)bounds.getY(), (float)bounds.getBottom());
  g.drawText("LP: " + juce::String(lowpassHz, 1) + " Hz", (int)lowpassX + 2, bounds.getY() + 2, 60, 20, juce::Justification::left);

  g.setColour(juce::Colours::blue.withAlpha(0.8f));
  g.drawVerticalLine((int)highpassX, (float)bounds.getY(), (float)bounds.getBottom());
  g.drawText("HP: " + juce::String(highpassHz, 2) + " Hz", (int)highpassX + 2, bounds.getY() + 22, 60, 20, juce::Justification::left);
}

void FourierFilterDialog::onSliderValueChanged() {
    updateFilter();
}

void FourierFilterDialog::onOkClicked() {
  if (applyCallback) {
    applyCallback((float)lowpassSlider.getValue(), (float)highpassSlider.getValue());
  }
  // Close dialog (if modal)
  if (auto* parent = findParentComponentOfClass<juce::DialogWindow>()) {
    parent->exitModalState(1);  // Return code 1 = OK
  }
}

void FourierFilterDialog::onCancelClicked() {
  if (auto* parent = findParentComponentOfClass<juce::DialogWindow>()) {
    parent->exitModalState(0);  // Return code 0 = Cancel
  }
}

bool FourierFilterDialog::showDialog(const std::vector<Note*>& selectedNotes,
                                     float frameRateHz,
                                     ApplyCallback onApply,
                                     juce::Component* parent) {
  auto* dialog = new FourierFilterDialog(selectedNotes, frameRateHz, onApply);

  juce::DialogWindow::LaunchOptions options;
  options.content.setOwned(dialog);
  options.dialogTitle = "Fourier Pitch Filter";
  options.resizable = false;
  options.useNativeTitleBar = true;
  options.componentToCentreAround = parent;

  int result = options.runModal();
  return result == 1;  // true if OK clicked
}
