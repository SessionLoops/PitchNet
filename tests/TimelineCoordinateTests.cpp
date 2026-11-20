#include <juce_core/juce_core.h>
#include "../Source/UI/PianoRoll/CoordinateMapper.h"
#include "TestAssert.h"

namespace {
bool near(double a, double b) { return std::abs(a - b) < 1.0e-6; }
}

int main() {
  CoordinateMapper mapper;
  mapper.setPixelsPerSecond(100.0f);

  // A half-beat placement must snap to the host grid, not the source grid.
  mapper.setTimelineDisplayOffset(0.25);
  CHECK(near(mapper.snapProjectTimeToTimelineGrid(0.30, 0.5), 0.25));
  CHECK(near(mapper.projectToTimeline(
                 mapper.snapProjectTimeToTimelineGrid(0.30, 0.5)), 0.5));
  mapper.setTimelineDisplayOffset(-4.25);
  CHECK(near(mapper.snapProjectTimeToTimelineGrid(4.8, 0.5), 4.75));

  // Track mode needs negative project coordinates to show earlier host time.
  mapper.setTimelineDisplayOffset(30.0);
  mapper.setViewStartSeconds(mapper.timelineToProject(0.0));
  mapper.setScrollX(-2500.0);
  CHECK(near(mapper.getScrollX(), -2500.0));
  const float hostTenX = mapper.timeToX(mapper.timelineToProject(10.0));
  CHECK(near(mapper.worldToScreenX(hostTenX),
             CoordinateMapper::pianoKeysWidth + 500.0));
  CHECK(near(mapper.screenToWorldX(mapper.worldToScreenX(hostTenX)), hostTenX));
  mapper.setScrollX(-4000.0);
  CHECK(near(mapper.getScrollX(), -3000.0));

  // A trimmed take can instead put host zero AFTER the source's zero.
  mapper.setTimelineDisplayOffset(-4.25);
  mapper.setViewStartSeconds(mapper.timelineToProject(0.0));
  mapper.setScrollX(0.0);
  CHECK(near(mapper.getScrollX(), 425.0));

  // Standalone/Clip mode retains the original zero-based coordinate contract.
  mapper.setTimelineDisplayOffset(0.0);
  mapper.setViewStartSeconds(0.0);
  mapper.setScrollX(-100.0);
  CHECK(near(mapper.getScrollX(), 0.0));
  CHECK(near(mapper.snapProjectTimeToTimelineGrid(0.30, 0.5), 0.5));

  // A detected downbeat moves the beat grid: bar 1 at 0.37 s, half-second
  // grid lines at 0.37 + k * 0.5 (before bar 1 too).
  mapper.setBeatGridOrigin(0.37);
  CHECK(near(mapper.projectToBeatGrid(0.37), 0.0));
  CHECK(near(mapper.beatGridToProject(1.0), 1.37));
  CHECK(near(mapper.snapProjectTimeToTimelineGrid(0.80, 0.5), 0.87));
  CHECK(near(mapper.snapProjectTimeToTimelineGrid(0.10, 0.5), -0.13) ||
        near(mapper.snapProjectTimeToTimelineGrid(0.10, 0.5), 0.37));
  CHECK(near(mapper.snapProjectTimeToTimelineGrid(0.20, 0.5), 0.37));
  mapper.setBeatGridOrigin(0.0);
  return 0;
}
