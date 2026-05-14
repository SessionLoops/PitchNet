#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Utils/ScaleUtils.h"
#include "../../Utils/UI/Theme.h"

namespace pianoRollView
{
inline juce::Colour getScaleAccentColour(ScaleMode mode)
{
  switch (mode)
  {
  case ScaleMode::None:
    return APP_COLOR_PRIMARY;
  case ScaleMode::Major:
    return juce::Colour(0xFF74A9FFu);
  case ScaleMode::Minor:
    return juce::Colour(0xFFB689FFu);
  case ScaleMode::Dorian:
    return juce::Colour(0xFF5BD0C0u);
  case ScaleMode::Phrygian:
    return juce::Colour(0xFFFF8C77u);
  case ScaleMode::Lydian:
    return juce::Colour(0xFFFFD166u);
  case ScaleMode::Mixolydian:
    return juce::Colour(0xFF75D0FFu);
  case ScaleMode::Locrian:
    return juce::Colour(0xFF9FA9BFu);
  case ScaleMode::Chromatic:
    return APP_COLOR_PRIMARY;
  }
  return APP_COLOR_PRIMARY;
}

inline bool isBlackKey(int noteInOctave)
{
  return noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
         noteInOctave == 8 || noteInOctave == 10;
}

enum class ScaleToneState
{
  Root,
  InScale,
  OutOfScale
};

inline ScaleToneState getScaleToneState(ScaleMode mode, int noteInOctave, int rootNote)
{
  if (mode == ScaleMode::None || rootNote < 0)
    return ScaleToneState::InScale;

  const int normalizedNote = (noteInOctave % 12 + 12) % 12;
  const int normalizedRoot = (rootNote % 12 + 12) % 12;

  if (mode == ScaleMode::Chromatic)
    return ScaleToneState::InScale;
  if (normalizedNote == normalizedRoot)
    return ScaleToneState::Root;
  if (ScaleUtils::isPitchClassInScale(mode, normalizedNote, normalizedRoot))
    return ScaleToneState::InScale;
  return ScaleToneState::OutOfScale;
}

inline bool isMultipleOf(double value, double step)
{
  if (step <= 0.0)
    return false;
  const double ratio = value / step;
  return std::abs(ratio - std::round(ratio)) < 1.0e-4;
}
} // namespace pianoRollView
