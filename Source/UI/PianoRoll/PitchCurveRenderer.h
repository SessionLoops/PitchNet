#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

#include <vector>

class SelectHandler;
class PitchEditor;

/**
 * Draws the F0 / delta / base pitch curves over the grid, and owns the
 * generated-base-pitch cache. Cache is regenerated on demand when notes change
 * (detected by note-count + total-frame mismatch) or when externally
 * invalidated.
 */
class PitchCurveRenderer
{
public:
  struct Params
  {
    bool showDeltaPitch;
    bool showBasePitch;
    bool showUvInterpolationDebug;
    bool showActualF0Debug;
    bool hidePitchCurves = false;
    int componentWidth;
  };

  PitchCurveRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *m) { coordMapper = m; }
  void setProject(Project *p)
  {
    project = p;
    invalidateBasePitchCache();
  }
  void setSelectHandler(SelectHandler *h) { selectHandler = h; }
  void setPitchEditor(PitchEditor *e) { pitchEditor = e; }

  void draw(juce::Graphics &g, const Params &params);

  // Public to allow external invalidation (State handlers, undo operations).
  void invalidateBasePitchCache();

private:
  void updateBasePitchCacheIfNeeded();

  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;
  SelectHandler *selectHandler = nullptr;
  PitchEditor *pitchEditor = nullptr;

  std::vector<float> cachedBasePitch;
  size_t cachedNoteCount = 0;
  int cachedTotalFrames = 0;
  bool cacheInvalidated = true;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchCurveRenderer)
};
