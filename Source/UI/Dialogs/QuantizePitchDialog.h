#pragma once

#include "../../JuceHeader.h"
#include <functional>

namespace QuantizePitchDialog {

// Preview arguments: pitch center %, snap to scale, pitch drift %, drift edited.
// Drift is correction strength: 0% original (default), 100% removed.
// The edited flag preserves mixed note drift values until the slider is changed.
// The completion callback receives true only when the user explicitly
// confirms the edit.
void showPopup(juce::Component *parent,
               juce::Rectangle<int> anchorBounds,
               float initialPitchCenter, float initialPitchDrift,
               bool initialSnapToScale,
               std::function<void(float, bool, float, bool)> onPreview,
               std::function<void(bool)> onComplete);

// Cancels the active preview, if any, and removes its popup.
void dismissPopup();

} // namespace QuantizePitchDialog
