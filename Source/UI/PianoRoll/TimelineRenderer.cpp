#include "TimelineRenderer.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/TimecodeFont.h"
#include "../../Utils/UI/Theme.h"

void TimelineRenderer::drawTimeline(juce::Graphics &g, const TimelineParams &params)
{
  if (!coordMapper)
    return;

  constexpr int scrollBarSize = 8;
  const int pianoKeysWidth = CoordinateMapper::pianoKeysWidth;
  const int timelineHeight = CoordinateMapper::timelineHeight;
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const double scrollX = coordMapper->getScrollX();

  auto timelineArea = juce::Rectangle<int>(
      pianoKeysWidth, 0, params.componentWidth - pianoKeysWidth - scrollBarSize,
      timelineHeight);

  // Background
  g.setColour(APP_COLOR_TIMELINE);
  g.fillRect(timelineArea);

  // Bottom border
  g.setColour(APP_COLOR_GRID_BAR);
  g.drawHorizontalLine(timelineHeight - 1, static_cast<float>(pianoKeysWidth),
                       static_cast<float>(params.componentWidth - scrollBarSize));

  const float duration = project ? project->getAudioData().getDuration() : 60.0f;
  g.setFont(TimecodeFont::getBoldFont(12.0f));

  if (params.displayMode == TimelineDisplayMode::Beats)
  {
    const double beatSeconds = params.beatSeconds;
    const double barSeconds = params.barSeconds;
    if (beatSeconds > 1.0e-6 && barSeconds > 1.0e-6)
    {
      const int beatsPerBar = juce::jmax(1, params.beatNumerator);
      const float pixelsPerBeat = static_cast<float>(beatSeconds * pixelsPerSecond);
      int beatStep = 1;
      while (pixelsPerBeat * static_cast<float>(beatStep) < 20.0f && beatStep < 64)
        beatStep *= 2;

      const int firstBeat = std::max(
          0, static_cast<int>(std::floor((scrollX / pixelsPerSecond) / beatSeconds)));
      const int lastBeat = static_cast<int>(
                               std::ceil((scrollX + timelineArea.getWidth()) / pixelsPerSecond / beatSeconds)) +
                           beatStep;

      for (int beatIndex = firstBeat; beatIndex <= lastBeat; beatIndex += beatStep)
      {
        const double time = static_cast<double>(beatIndex) * beatSeconds;
        if (time > duration + beatSeconds)
          break;

        const float x =
            pianoKeysWidth + static_cast<float>(time * pixelsPerSecond) -
            static_cast<float>(scrollX);
        if (x < pianoKeysWidth - 50 || x > params.componentWidth)
          continue;

        const bool isBarLine = (beatIndex % beatsPerBar) == 0;
        const int tickHeight = isBarLine ? 9 : 4;
        g.setColour(isBarLine ? APP_COLOR_GRID_BAR : APP_COLOR_GRID);
        g.drawVerticalLine(static_cast<int>(x),
                           static_cast<float>(timelineHeight - tickHeight),
                           static_cast<float>(timelineHeight - 1));

        if (isBarLine)
        {
          const int bar = beatIndex / beatsPerBar + 1;
          g.setColour(APP_COLOR_TEXT_MUTED);
          g.drawText("Bar " + juce::String(bar), static_cast<int>(x) + 3, 2, 64,
                     timelineHeight - 4, juce::Justification::centredLeft, false);
        }
        else if (beatStep == 1 && pixelsPerBeat >= 58.0f)
        {
          const int beatInBar = (beatIndex % beatsPerBar) + 1;
          const int bar = beatIndex / beatsPerBar + 1;
          g.setColour(APP_COLOR_TEXT_MUTED.withAlpha(0.8f));
          g.drawText(juce::String::formatted("%d.%d", bar, beatInBar),
                     static_cast<int>(x) + 3, 2, 48, timelineHeight - 4,
                     juce::Justification::centredLeft, false);
        }
      }
      return;
    }
  }

  // Time mode labels/ticks.
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

  for (float time = 0.0f; time <= duration + secondsPerTick; time += secondsPerTick)
  {
    float x =
        pianoKeysWidth + time * pixelsPerSecond - static_cast<float>(scrollX);

    if (x < pianoKeysWidth - 50 || x > params.componentWidth)
      continue;

    bool isMajor = std::fmod(time, secondsPerTick * 2.0f) < 0.001f;
    int tickHeight = isMajor ? 8 : 4;

    g.setColour(APP_COLOR_GRID_BAR);
    g.drawVerticalLine(static_cast<int>(x),
                       static_cast<float>(timelineHeight - tickHeight),
                       static_cast<float>(timelineHeight - 1));

    if (isMajor)
    {
      int minutes = static_cast<int>(time) / 60;
      int seconds = static_cast<int>(time) % 60;
      int tenths = static_cast<int>((time - std::floor(time)) * 10);

      juce::String label;
      if (minutes > 0)
        label = juce::String::formatted("%d:%02d", minutes, seconds);
      else if (secondsPerTick < 1.0f)
        label = juce::String::formatted("%d.%d", seconds, tenths);
      else
        label = juce::String::formatted("%ds", seconds);

      g.setColour(APP_COLOR_TEXT_MUTED);
      g.drawText(label, static_cast<int>(x) + 3, 2, 50, timelineHeight - 4,
                 juce::Justification::centredLeft, false);
    }
  }
}

void TimelineRenderer::drawLoopTimeline(juce::Graphics &g, const LoopParams &params)
{
  if (!coordMapper)
    return;

  constexpr int scrollBarSize = 8;
  const int pianoKeysWidth = CoordinateMapper::pianoKeysWidth;
  const int timelineHeight = CoordinateMapper::timelineHeight;
  const int loopTimelineHeight = CoordinateMapper::loopTimelineHeight;
  const int headerHeight = CoordinateMapper::headerHeight;
  const double scrollX = coordMapper->getScrollX();

  auto loopArea = juce::Rectangle<int>(
      pianoKeysWidth, timelineHeight,
      params.componentWidth - pianoKeysWidth - scrollBarSize, loopTimelineHeight);

  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRect(loopArea);

  g.setColour(APP_COLOR_GRID_BAR);
  g.drawHorizontalLine(headerHeight - 1,
                       static_cast<float>(pianoKeysWidth),
                       static_cast<float>(params.componentWidth - scrollBarSize));

  double loopStartSeconds = params.loopStartSeconds;
  double loopEndSeconds = params.loopEndSeconds;
  if (loopStartSeconds > loopEndSeconds)
    std::swap(loopStartSeconds, loopEndSeconds);

  if (loopEndSeconds <= loopStartSeconds)
    return;

  const float startX =
      static_cast<float>(pianoKeysWidth) + coordMapper->timeToX(loopStartSeconds) -
      static_cast<float>(scrollX);
  const float endX =
      static_cast<float>(pianoKeysWidth) + coordMapper->timeToX(loopEndSeconds) -
      static_cast<float>(scrollX);

  auto range = juce::Rectangle<float>(
      startX, static_cast<float>(timelineHeight), endX - startX,
      static_cast<float>(loopTimelineHeight));

  const auto baseColor = APP_COLOR_PRIMARY;
  const auto fillColor =
      params.loopEnabled ? baseColor.withAlpha(0.25f) : baseColor.withAlpha(0.12f);
  const auto edgeColor =
      params.loopEnabled ? baseColor : APP_COLOR_BORDER;

  g.setColour(fillColor);
  g.fillRect(range);

  g.setColour(edgeColor);
  g.drawLine(startX, static_cast<float>(timelineHeight), startX,
             static_cast<float>(headerHeight - 1), 1.5f);
  g.drawLine(endX, static_cast<float>(timelineHeight), endX,
             static_cast<float>(headerHeight - 1), 1.5f);

  constexpr float flagWidth = 6.0f;
  constexpr float flagHeight = 6.0f;
  constexpr float flagTop = 0.0f;

  const float flagY = static_cast<float>(timelineHeight) + flagTop;

  juce::Path startFlag;
  startFlag.addTriangle(startX, flagY, startX, flagY + flagHeight,
                        startX - flagWidth, flagY + flagHeight);
  g.fillPath(startFlag);

  juce::Path endFlag;
  endFlag.addTriangle(endX, flagY, endX, flagY + flagHeight,
                      endX + flagWidth, flagY + flagHeight);
  g.fillPath(endFlag);
}
