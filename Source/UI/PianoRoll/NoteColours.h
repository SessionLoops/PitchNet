#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/ScaleUtils.h"

#include <cmath>

/**
 * Note body colours: how far a note's pitch sits from its semitone, blue in
 * tune through purple and pink to red half a semitone off, as a vertical
 * gradient from the centre colour at the pitch line to the side colour at the
 * edges. Shared by the vocal NoteRenderer and the PolyphonicNoteLayer so both
 * kinds of note look the same.
 */
namespace notecolours
{
struct NoteGradientColours
{
  juce::Colour centre;
  juce::Colour side;
};


inline float getPitchCenterAmount(float midi, int pitchReferenceHz)
{
  const float pitchCenter =
      ScaleUtils::snapMidiToSemitone(midi, pitchReferenceHz);
  const float distanceFromCenter = std::abs(midi - pitchCenter);

  return juce::jlimit(0.0f, 1.0f, distanceFromCenter / 0.5f);
}

inline juce::Colour interpolateLayeredColour(juce::Colour first,
                                      juce::Colour second,
                                      juce::Colour third,
                                      juce::Colour fourth,
                                      float amount)
{
  if (amount < 1.0f / 3.0f)
    return first.interpolatedWith(second, amount * 3.0f);

  if (amount < 2.0f / 3.0f)
    return second.interpolatedWith(third, amount * 3.0f - 1.0f);

  return third.interpolatedWith(fourth, amount * 3.0f - 2.0f);
}

inline NoteGradientColours getNoteGradientColours(float midi, int pitchReferenceHz)
{
  static const juce::Colour inTuneCentre(0xFF1983E0u);
  static const juce::Colour inTuneSide(0xFF0021E2u);
  static const juce::Colour firstLayerCentre(0xFFD66CFFu);
  static const juce::Colour firstLayerSide(0xFF971CFFu);
  static const juce::Colour secondLayerCentre(0xFFFF90EDu);
  static const juce::Colour secondLayerSide(0xFFF624B7u);
  static const juce::Colour outOfTuneCentre(0xFFF95D5Du);
  static const juce::Colour outOfTuneSide(0xFFFF0000u);

  const float amount = getPitchCenterAmount(midi, pitchReferenceHz);

  return {
      interpolateLayeredColour(inTuneCentre, firstLayerCentre,
                               secondLayerCentre, outOfTuneCentre, amount),
      interpolateLayeredColour(inTuneSide, firstLayerSide, secondLayerSide,
                               outOfTuneSide, amount)};
}

inline void setNoteGradientFill(juce::Graphics &g,
                         const juce::Rectangle<float> &bounds,
                         float centreY,
                         const NoteGradientColours &colours)
{
  const float height = bounds.getHeight();
  if (height <= 0.0f || !std::isfinite(height))
  {
    g.setColour(colours.centre);
    return;
  }

  const float top = bounds.getY();
  const float bottom = bounds.getBottom();
  const float gradientCentre =
      juce::jlimit(0.0f, 1.0f, (centreY - top) / height);

  juce::ColourGradient gradient(colours.side, bounds.getCentreX(), top,
                                colours.side, bounds.getCentreX(), bottom,
                                false);
  gradient.addColour(gradientCentre, colours.centre);
  g.setGradientFill(gradient);
}
} // namespace notecolours
