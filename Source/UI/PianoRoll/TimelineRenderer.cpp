#include "TimelineRenderer.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/TimecodeFont.h"
#include "../../Utils/UI/Theme.h"

void TimelineRenderer::drawTimeline(juce::Graphics &g, const TimelineParams &params)
{
  if (!coordMapper)
    return;

  const auto markerColour = juce::Colour(0xFF0D0B0Bu);
  const auto markerTextColour = juce::Colour(0xFF9B9B9Bu);
  constexpr int scrollBarSize = 8;
  const int pianoKeysWidth = CoordinateMapper::pianoKeysWidth;
  const int timelineHeight = CoordinateMapper::timelineHeight;
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const int scrollX = static_cast<int>(coordMapper->getScrollX());

  auto timelineArea = juce::Rectangle<int>(
      pianoKeysWidth, 0, params.componentWidth - pianoKeysWidth - scrollBarSize,
      timelineHeight);

  juce::Graphics::ScopedSaveState savedState(g);
  g.reduceClipRegion(timelineArea);

  // Background
  g.setColour(juce::Colour(0xFF232323u));
  g.fillRect(timelineArea);

  // Bottom border
  g.setColour(juce::Colour(0xFF0D0B0Bu));
  g.drawHorizontalLine(timelineHeight - 1, static_cast<float>(pianoKeysWidth),
                       static_cast<float>(params.componentWidth - scrollBarSize));

  float duration = project ? project->getAudioData().getDuration()
                           : DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
  if (duration <= 0.0f)
    duration = DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
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
        if (x < pianoKeysWidth || x > timelineArea.getRight())
          continue;

        const bool isBarLine = (beatIndex % beatsPerBar) == 0;
        if (isBarLine)
        {
          g.setColour(markerColour);
          g.drawVerticalLine(static_cast<int>(x), 0.0f,
                             static_cast<float>(timelineHeight - 1));
        }

        if (isBarLine)
        {
          const int bar = beatIndex / beatsPerBar + 1;
          g.setColour(markerTextColour);
          g.drawText(juce::String(bar), static_cast<int>(x) + 3, 2, 64,
                     timelineHeight - 4, juce::Justification::centredLeft, false);
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

    if (x < pianoKeysWidth || x > timelineArea.getRight())
      continue;

    bool isMajor = std::fmod(time, secondsPerTick * 2.0f) < 0.001f;
    if (isMajor)
    {
      g.setColour(markerColour);
      g.drawVerticalLine(static_cast<int>(x), 0.0f,
                         static_cast<float>(timelineHeight - 1));
    }

    if (isMajor)
    {
      const int totalSeconds = static_cast<int>(std::floor(time));
      const int minutes = totalSeconds / 60;
      const int seconds = totalSeconds % 60;
      const juce::String label =
          juce::String::formatted("%d:%02d", minutes, seconds);

      g.setColour(markerTextColour);
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
  const auto markerColour = juce::Colour(0xFF0D0B0Bu);
  const int pianoKeysWidth = CoordinateMapper::pianoKeysWidth;
  const int timelineHeight = CoordinateMapper::timelineHeight;
  const int loopTimelineHeight = CoordinateMapper::loopTimelineHeight;
  const int headerHeight = CoordinateMapper::headerHeight;
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const int scrollX = static_cast<int>(coordMapper->getScrollX());

  auto loopClipArea = juce::Rectangle<int>(
      0, timelineHeight, params.componentWidth - scrollBarSize,
      loopTimelineHeight);
  auto loopArea = juce::Rectangle<int>(
      pianoKeysWidth, timelineHeight,
      params.componentWidth - pianoKeysWidth - scrollBarSize, loopTimelineHeight);

  juce::Graphics::ScopedSaveState savedState(g);
  g.reduceClipRegion(loopClipArea);

  g.setColour(juce::Colour(0xFF232323u));
  g.fillRect(loopArea);

  g.setColour(markerColour);
  g.drawHorizontalLine(headerHeight - 1,
                       0.0f,
                       static_cast<float>(params.componentWidth - scrollBarSize));

  const auto baseColor = juce::Colour(0xFF888888u);
  const auto edgeColor =
      params.loopEnabled ? baseColor : juce::Colour(0xFF3E3E3Eu);
  double loopStartSeconds = params.loopStartSeconds;
  double loopEndSeconds = params.loopEndSeconds;
  if (loopStartSeconds > loopEndSeconds)
    std::swap(loopStartSeconds, loopEndSeconds);
  const bool hasLoopRange = loopEndSeconds > loopStartSeconds;
  float startX = 0.0f;
  float endX = 0.0f;

  if (hasLoopRange)
  {
    startX =
        static_cast<float>(pianoKeysWidth) + coordMapper->timeToX(loopStartSeconds) -
        static_cast<float>(scrollX);
    endX =
        static_cast<float>(pianoKeysWidth) + coordMapper->timeToX(loopEndSeconds) -
        static_cast<float>(scrollX);

    auto range = juce::Rectangle<float>(
        startX, static_cast<float>(timelineHeight), endX - startX,
        static_cast<float>(loopTimelineHeight - 1));

    if (params.loopEnabled)
    {
      juce::Graphics::ScopedSaveState rangeClip(g);
      g.reduceClipRegion(loopArea);
      g.setColour(baseColor.withAlpha(0.5f));
      g.fillRect(range);
    }
  }

  float duration = project ? project->getAudioData().getDuration()
                           : DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
  if (duration <= 0.0f)
    duration = DEFAULT_EMPTY_PROJECT_DURATION_SECONDS;
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
                               std::ceil((scrollX + loopArea.getWidth()) / pixelsPerSecond / beatSeconds)) +
                           beatStep;

      for (int beatIndex = firstBeat; beatIndex <= lastBeat; beatIndex += beatStep)
      {
        const double time = static_cast<double>(beatIndex) * beatSeconds;
        if (time > duration + beatSeconds)
          break;

        const float x =
            pianoKeysWidth + static_cast<float>(time * pixelsPerSecond) -
            static_cast<float>(scrollX);
        if (x < pianoKeysWidth || x > loopArea.getRight())
          continue;

        const bool isBarLine = (beatIndex % beatsPerBar) == 0;
        const float tickTop = isBarLine
                                  ? static_cast<float>(timelineHeight)
                                  : static_cast<float>(timelineHeight) +
                                        static_cast<float>(loopTimelineHeight) * 0.5f;
        g.setColour(markerColour);
        g.drawVerticalLine(static_cast<int>(x), tickTop,
                           static_cast<float>(headerHeight - 1));
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

    for (float time = 0.0f; time <= duration + secondsPerTick; time += secondsPerTick)
    {
      const float x =
          pianoKeysWidth + time * pixelsPerSecond - static_cast<float>(scrollX);
      if (x < pianoKeysWidth || x > loopArea.getRight())
        continue;

      const bool isMajor = std::fmod(time, secondsPerTick * 2.0f) < 0.001f;
      if (!isMajor)
        continue;

      g.setColour(markerColour);
      g.drawVerticalLine(static_cast<int>(x), static_cast<float>(timelineHeight),
                         static_cast<float>(headerHeight - 1));
    }
  }

  if (!hasLoopRange)
    return;

  constexpr float flagWidth = 8.0f;
  constexpr float flagHeight = 8.0f;
  const float flagY = static_cast<float>(timelineHeight);

  juce::Graphics::ScopedSaveState flagClip(g);
  g.reduceClipRegion(loopArea);
  g.setColour(edgeColor);

  juce::Path startFlag;
  startFlag.addTriangle(startX, flagY, startX, flagY + flagHeight,
                        startX + flagWidth, flagY);
  g.fillPath(startFlag);

  juce::Path endFlag;
  endFlag.addTriangle(endX, flagY, endX, flagY + flagHeight,
                      endX - flagWidth, flagY);
  g.fillPath(endFlag);
}
