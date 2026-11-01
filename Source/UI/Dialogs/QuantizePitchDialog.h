#pragma once

#include "../../JuceHeader.h"
#include <functional>

namespace QuantizePitchDialog {

// The ranges are the lowest/highest values across the target notes; a
// non-empty range is shown as "low % - high %" until that slider is changed.
// The initial values (the targets' average) position the thumbs.
// Preview arguments: pitch center %, center edited, snap to scale,
// pitch drift %, drift edited. Drift is correction strength: 0% original
// (default), 100% removed. An unedited slider keeps each note's own value.
// The completion callback receives true only when the user explicitly
// confirms the edit. onPreview updates note positions during dragging;
// onRefreshPreview rebuilds curves and requests audio after release (or a non-drag edit).
void showPopup(juce::Component *parent,
               juce::Rectangle<int> anchorBounds,
               juce::Range<float> pitchCenterRange, float initialPitchCenter,
               juce::Range<float> pitchDriftRange, float initialPitchDrift,
               bool initialSnapToScale,
               std::function<void(float, bool, bool, float, bool)> onPreview,
               std::function<void(bool)> onComplete,
               std::function<void()> onRefreshPreview);

// Cancels the active preview, if any, and removes its popup.
void dismissPopup();

} // namespace QuantizePitchDialog
