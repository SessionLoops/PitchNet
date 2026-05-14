#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

/**
 * Draws the left-gutter piano keys panel, including scale color overlays.
 * Stateless beyond the CoordinateMapper pointer; the component owns the
 * scale-mode state and resolves preview vs selected before passing it in.
 */
class PianoKeysRenderer
{
public:
  PianoKeysRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }

  void draw(juce::Graphics &g,
            int componentHeight,
            ScaleMode activeScaleMode,
            int activeScaleRootNote,
            bool showScaleColors);

private:
  CoordinateMapper *coordMapper = nullptr;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoKeysRenderer)
};
