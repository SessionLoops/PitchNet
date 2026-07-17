#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Note.h"
#include "../../Utils/FourierPitchFilter.h"
#include <vector>
#include <functional>

/**
 * Modal dialog for FFT-based pitch filtering with spectrum visualization.
 */
class FourierFilterDialog : public juce::Component {
public:
  using ApplyCallback = std::function<void(float lowpassHz, float highpassHz)>;

  /**
   * @param selectedNotes Notes to filter
   * @param frameRateHz Sample rate of pitch curves (typically ~86.13 Hz)
   * @param onApply Callback when user clicks OK
   */
  FourierFilterDialog(const std::vector<Note*>& selectedNotes,
                      float frameRateHz,
                      ApplyCallback onApply);

  ~FourierFilterDialog() override;

  void paint(juce::Graphics& g) override;
  void resized() override;

  /**
   * Show dialog modally (blocking).
   * @return true if user clicked OK, false if cancelled
   */
  static bool showDialog(const std::vector<Note*>& selectedNotes,
                         float frameRateHz,
                         ApplyCallback onApply,
                         juce::Component* parent = nullptr);

private:
  std::vector<Note*> notes;
  float frameRateHz;
  ApplyCallback applyCallback;

  // Filter results cache
  FourierPitchFilter::FilterResult filterResult;
  std::vector<float> originalPitchCurve;  // Combined from all notes

  // UI Components
  juce::Slider lowpassSlider;
  juce::Label lowpassLabel;
  juce::Slider highpassSlider;
  juce::Label highpassLabel;
  juce::TextButton okButton{"OK"};
  juce::TextButton cancelButton{"Cancel"};

  // Spectrum viewport bounds
  juce::Rectangle<int> spectrumBounds;

  void updateFilter();
  void drawSpectrum(juce::Graphics& g, const juce::Rectangle<int>& bounds);
  void onSliderValueChanged();
  void onOkClicked();
  void onCancelClicked();

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FourierFilterDialog)
};
