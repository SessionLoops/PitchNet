#pragma once

#include "../../JuceHeader.h"

namespace WindowSizing {

struct Constraints {
  int minWidth = 960;
  int minHeight = 600;
  float initialMaxFraction = 0.92f;
  int initialMargin = 24;
};

constexpr int kDefaultWidth = 1000;
constexpr int kDefaultHeight = 628;

const juce::Displays::Display *getPrimaryDisplay();
const juce::Displays::Display *getDisplayForComponent(
    const juce::Component *component);

/**
 * Get initial window bounds.
 * The desiredWidth/Height are in logical pixels.
 */
juce::Rectangle<int> getInitialBounds(int desiredWidth, int desiredHeight,
                                      const juce::Displays::Display &display,
                                      const Constraints &constraints);

/**
 * Get clamped window size.
 */
juce::Point<int> getClampedSize(int desiredWidth, int desiredHeight,
                                const juce::Displays::Display &display,
                                const Constraints &constraints);

/**
 * Get maximum bounds for a display (the usable area).
 */
juce::Rectangle<int> getMaxBounds(const juce::Displays::Display &display);

} // namespace WindowSizing
