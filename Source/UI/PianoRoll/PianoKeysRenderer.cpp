#include "PianoKeysRenderer.h"
#include "PianoRollViewHelpers.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

void PianoKeysRenderer::draw(juce::Graphics &g,
                             int componentHeight,
                             ScaleMode activeScaleMode,
                             int activeScaleRootNote,
                             bool showScaleColors)
{
  if (!coordMapper)
    return;

  constexpr int scrollBarSize = 8;
  const int pianoKeysWidth = CoordinateMapper::pianoKeysWidth;
  const int headerHeight = CoordinateMapper::headerHeight;

  auto keyArea = juce::Rectangle<int>(
      0, headerHeight, pianoKeysWidth,
      juce::jmax(0, componentHeight - headerHeight - scrollBarSize));

  // Background
  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRect(keyArea);

  static const char *noteNames[] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
  const bool showScaleOverlay =
      showScaleColors && activeScaleMode != ScaleMode::None &&
      activeScaleMode != ScaleMode::Chromatic &&
      activeScaleRootNote >= 0;
  const juce::Colour scaleAccent =
      pianoRollView::getScaleAccentColour(activeScaleMode);

  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  // Use truncated scrollY to match grid origin (which uses static_cast<int>(scrollY))
  const int scrollYInt = static_cast<int>(coordMapper->getScrollY());

  for (int midi = MIN_MIDI_NOTE; midi <= MAX_MIDI_NOTE; ++midi)
  {
    float y = coordMapper->midiToY(static_cast<float>(midi)) -
              static_cast<float>(scrollYInt) + headerHeight;
    int noteInOctave = (midi % 12 + 12) % 12;

    bool isBlack = pianoRollView::isBlackKey(noteInOctave);
    const auto toneState = pianoRollView::getScaleToneState(
        activeScaleMode, noteInOctave, activeScaleRootNote);

    juce::Colour keyFill = isBlack ? APP_COLOR_PIANO_BLACK : APP_COLOR_PIANO_WHITE;
    if (showScaleOverlay)
    {
      if (toneState == pianoRollView::ScaleToneState::OutOfScale)
      {
        keyFill = APP_COLOR_PIANO_BLACK;
      }
      else
      {
        keyFill = APP_COLOR_PIANO_WHITE;
        keyFill = keyFill.interpolatedWith(
            scaleAccent,
            toneState == pianoRollView::ScaleToneState::Root ? 0.32f : 0.16f);
      }
    }

    g.setColour(keyFill);
    g.fillRect(0.0f, y, static_cast<float>(pianoKeysWidth - 2),
               pixelsPerSemitone - 1);

    if (showScaleOverlay)
    {
      if (toneState == pianoRollView::ScaleToneState::Root)
      {
        g.setColour(scaleAccent.withAlpha(0.95f));
        g.fillRect(0.0f, y, 3.0f, pixelsPerSemitone - 1);
      }
      else if (toneState == pianoRollView::ScaleToneState::InScale)
      {
        g.setColour(scaleAccent.withAlpha(0.55f));
        g.fillRect(0.0f, y, 2.0f, pixelsPerSemitone - 1);
      }
    }

    int octave = midi / 12 - 1;
    juce::String noteName =
        juce::String(noteNames[noteInOctave]) + juce::String(octave);

    juce::Colour textColour = isBlack ? APP_COLOR_PIANO_TEXT_DIM
                                      : APP_COLOR_PIANO_TEXT;
    if (showScaleOverlay)
    {
      if (toneState == pianoRollView::ScaleToneState::Root)
        textColour = APP_COLOR_TEXT_PRIMARY;
      else if (toneState == pianoRollView::ScaleToneState::OutOfScale)
        textColour = textColour.withMultipliedAlpha(0.72f);
    }
    g.setColour(textColour);
    g.setFont(13.0f);
    g.drawText(noteName, pianoKeysWidth - 36, static_cast<int>(y), 32,
               static_cast<int>(pixelsPerSemitone),
               juce::Justification::centred);
  }
}
