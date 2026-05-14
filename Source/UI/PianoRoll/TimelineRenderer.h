#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

/**
 * Draws the timeline header (bar/beat or seconds labels) and the loop gutter
 * below it.
 *
 * Stateless beyond the CoordinateMapper pointer; the component resolves
 * loop-vs-drag state and timeline timing values and passes them in.
 */
class TimelineRenderer
{
public:
  struct TimelineParams
  {
    TimelineDisplayMode displayMode;
    int beatNumerator;
    double beatSeconds;
    double barSeconds;
    int componentWidth;
  };

  struct LoopParams
  {
    int componentWidth;
    bool loopEnabled;
    double loopStartSeconds;
    double loopEndSeconds;
  };

  TimelineRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }
  void setProject(Project *proj) { project = proj; }

  void drawTimeline(juce::Graphics &g, const TimelineParams &params);
  void drawLoopTimeline(juce::Graphics &g, const LoopParams &params);

private:
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineRenderer)
};
