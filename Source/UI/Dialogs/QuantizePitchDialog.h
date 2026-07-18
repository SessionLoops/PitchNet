#pragma once

#include "../../JuceHeader.h"
#include <functional>

namespace QuantizePitchDialog {

// The preview callback is invoked while either correction control changes.
// The completion callback receives true only when the user explicitly
// confirms the edit.
void showPopup(juce::Component *parent,
               juce::Rectangle<int> anchorBounds,
               float initialPitchCenter,
               std::function<void(float, bool)> onPreview,
               std::function<void(bool)> onComplete);

// Cancels the active preview, if any, and removes its popup.
void dismissPopup();

} // namespace QuantizePitchDialog
