#include "GridRenderer.h"
#include "PianoRollViewHelpers.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

void GridRenderer::draw(juce::Graphics &g, const Params &params)
{
  if (!coordMapper)
    return;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();
  const double scrollY = coordMapper->getScrollY();

  float duration = project ? project->getAudioData().getDuration()
                           : DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
  if (duration <= 0.0f)
    duration = DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
  const float width =
      std::max(duration * pixelsPerSecond, static_cast<float>(params.componentWidth));
  const float height = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  // Only draw the visible area to avoid spending time on off-screen rows/columns.
  const float visibleStartX = juce::jlimit(0.0f, width, static_cast<float>(scrollX));
  const float visibleEndX = juce::jlimit(
      0.0f, width, visibleStartX + static_cast<float>(params.visibleContentWidth) + 2.0f);
  const float visibleTopY = juce::jlimit(0.0f, height, static_cast<float>(scrollY));
  const float visibleBottomY = juce::jlimit(
      0.0f, height,
      visibleTopY + static_cast<float>(params.visibleContentHeight) + pixelsPerSemitone);

  if (visibleEndX <= visibleStartX || visibleBottomY <= visibleTopY)
    return;

  const int visibleTopMidi = juce::jlimit(
      MIN_MIDI_NOTE, MAX_MIDI_NOTE,
      static_cast<int>(std::ceil(coordMapper->yToMidi(visibleTopY))));
  const int visibleBottomMidi = juce::jlimit(
      MIN_MIDI_NOTE, MAX_MIDI_NOTE,
      static_cast<int>(std::floor(coordMapper->yToMidi(visibleBottomY))));
  const int startMidi = juce::jlimit(MIN_MIDI_NOTE, MAX_MIDI_NOTE,
                                     visibleBottomMidi - 1);
  const int endMidi = juce::jlimit(MIN_MIDI_NOTE, MAX_MIDI_NOTE,
                                   visibleTopMidi + 1);

  const bool showScaleOverlay =
      params.showScaleColors && params.scaleMode != ScaleMode::None &&
      params.scaleMode != ScaleMode::Chromatic &&
      params.scaleRootNote >= 0;
  const juce::Colour scaleAccent =
      pianoRollView::getScaleAccentColour(params.scaleMode);

  const auto whiteKeyRowColour = juce::Colour(0xFF1C1C1Bu);
  const auto blackKeyRowColour = juce::Colour(0xFF151515u);

  if (params.drawRowBackgrounds)
  {
    for (int midi = startMidi; midi <= endMidi; ++midi)
    {
      const int noteInOctave = (midi % 12 + 12) % 12;
      g.setColour(pianoRollView::isBlackKey(noteInOctave) ? blackKeyRowColour
                                                          : whiteKeyRowColour);
      const float y = coordMapper->midiToY(static_cast<float>(midi));
      g.fillRect(visibleStartX, y, visibleEndX - visibleStartX,
                 pixelsPerSemitone);
    }
  }

  if (params.drawRowBackgrounds && showScaleOverlay)
  {
    const auto rootRowColour = scaleAccent.withAlpha(0.24f);
    const auto inScaleRowColour = scaleAccent.withAlpha(0.08f);
    const auto outOfScaleRowColour = juce::Colours::black.withAlpha(0.20f);

    for (int midi = startMidi; midi <= endMidi; ++midi)
    {
      const int noteInOctave = (midi % 12 + 12) % 12;
      const auto toneState = pianoRollView::getScaleToneState(
          params.scaleMode, noteInOctave, params.scaleRootNote);
      g.setColour(toneState == pianoRollView::ScaleToneState::Root
                      ? rootRowColour
                      : (toneState == pianoRollView::ScaleToneState::InScale
                             ? inScaleRowColour
                             : outOfScaleRowColour));
      const float y = coordMapper->midiToY(static_cast<float>(midi));
      g.fillRect(visibleStartX, y, visibleEndX - visibleStartX, pixelsPerSemitone);
    }
  }

  if (!params.drawGridLines)
    return;

  // Horizontal pitch lines.
  for (int midi = startMidi; midi <= endMidi; ++midi)
  {
    const float y = coordMapper->midiToY(static_cast<float>(midi));
    const int noteInOctave = (midi % 12 + 12) % 12;

    if (!showScaleOverlay)
    {
      g.setColour(noteInOctave == 0 ? APP_COLOR_GRID_BAR : APP_COLOR_GRID);
    }
    else if (pianoRollView::getScaleToneState(params.scaleMode, noteInOctave,
                                              params.scaleRootNote) ==
             pianoRollView::ScaleToneState::Root)
    {
      g.setColour(scaleAccent.withAlpha(0.70f));
    }
    else if (pianoRollView::getScaleToneState(params.scaleMode, noteInOctave,
                                              params.scaleRootNote) ==
             pianoRollView::ScaleToneState::InScale)
    {
      g.setColour(APP_COLOR_GRID.interpolatedWith(scaleAccent, 0.40f));
    }
    else
    {
      g.setColour(APP_COLOR_GRID.darker(0.25f));
    }

    g.drawHorizontalLine(static_cast<int>(y), visibleStartX, visibleEndX);
  }

  if (params.timelineDisplayMode == TimelineDisplayMode::Beats)
  {
    if (params.gridSeconds > 1.0e-6 && params.beatSeconds > 1.0e-6 &&
        params.barSeconds > 1.0e-6)
    {
      const double visibleStartTime = visibleStartX / pixelsPerSecond;
      const double visibleEndTime = visibleEndX / pixelsPerSecond;
      const int firstGrid = std::max(
          0,
          static_cast<int>(std::floor(visibleStartTime / params.gridSeconds)) - 1);

      for (int i = firstGrid;; ++i)
      {
        const double time = static_cast<double>(i) * params.gridSeconds;
        if (time > visibleEndTime + params.gridSeconds)
          break;

        const float x = static_cast<float>(time * pixelsPerSecond);
        if (x < visibleStartX - 1.0f || x > visibleEndX + 1.0f)
          continue;

        const bool isBarLine = pianoRollView::isMultipleOf(time, params.barSeconds);
        if (isBarLine)
        {
          g.setColour(APP_COLOR_GRID_BAR);
        }
        else if (pianoRollView::isMultipleOf(time, params.beatSeconds))
        {
          g.setColour(showScaleOverlay
                          ? APP_COLOR_GRID.interpolatedWith(scaleAccent, 0.20f)
                          : APP_COLOR_GRID);
        }
        else
        {
          g.setColour(APP_COLOR_GRID.darker(0.2f));
        }
        if (isBarLine)
          g.fillRect(x - 0.5f, visibleTopY, 2.0f,
                     visibleBottomY - visibleTopY);
        else
          g.drawVerticalLine(static_cast<int>(x), visibleTopY, visibleBottomY);
      }
    }
  }
  else
  {
    float secondsPerTick;
    if (pixelsPerSecond >= 200.0f)
      secondsPerTick = 0.5f;
    else if (pixelsPerSecond >= 100.0f)
      secondsPerTick = 1.0f;
    else if (pixelsPerSecond >= 50.0f)
      secondsPerTick = 2.0f;
    else if (pixelsPerSecond >= 25.0f)
      secondsPerTick = 5.0f;
    else
      secondsPerTick = 10.0f;

    const float secondsPerLine = secondsPerTick * 2.0f;
    const float pixelsPerLine = secondsPerLine * pixelsPerSecond;
    if (pixelsPerLine > 1.0e-4f)
    {
      g.setColour(showScaleOverlay
                      ? APP_COLOR_GRID.interpolatedWith(scaleAccent, 0.20f)
                      : APP_COLOR_GRID);
      const int firstLine =
          std::max(0, static_cast<int>(std::floor(visibleStartX / pixelsPerLine)));
      for (float x = firstLine * pixelsPerLine; x <= visibleEndX; x += pixelsPerLine)
        g.drawVerticalLine(static_cast<int>(x), visibleTopY, visibleBottomY);
    }
  }
}
