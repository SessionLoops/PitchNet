#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

/**
 * Draws the main scrollable grid (horizontal pitch lines + vertical beat/time
 * markers) with optional scale-color row overlays.
 *
 * Stateless beyond the CoordinateMapper pointer; the component computes scale
 * state and timeline timing values and passes them in as a Params struct.
 */
class GridRenderer
{
public:
  struct Params
  {
    ScaleMode scaleMode;
    int scaleRootNote;
    bool showScaleColors;
    TimelineDisplayMode timelineDisplayMode;
    // Pre-computed timing values from the component (depend on tempo + signature).
    double gridSeconds;
    double beatSeconds;
    double barSeconds;
    // Scrollable canvas duration. In ARA this includes one display bar after
    // the project's audio end.
    double timelineDuration;
    // Visible content sizes from the component.
    int componentWidth;
    int visibleContentWidth;
    int visibleContentHeight;
    bool drawRowBackgrounds = true;
    bool drawGridLines = true;
  };

  GridRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }
  void setProject(Project *proj) { project = proj; }

  void draw(juce::Graphics &g, const Params &params);

private:
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GridRenderer)
};
