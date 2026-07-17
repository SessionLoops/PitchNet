#pragma once

#include "../../Models/Project.h"
#include "../../Utils/Constants.h"

#include <optional>

enum class EditMode
{
  Select,
  Draw,
  Split
};

struct PianoRollViewState
{
  int hoveredPitchToolHandle = -1;

  float pixelsPerSecond = DEFAULT_PIXELS_PER_SECOND;
  float pixelsPerSemitone = DEFAULT_PIXELS_PER_SEMITONE;

  double cursorTime = 0.0;
  double scrollX = 0.0;
  double scrollY = 0.0;

  EditMode editMode = EditMode::Select;

  bool showDeltaPitch = true;
  bool showBasePitch = false;
  bool showSegmentsDebug = false;
  bool showGameValuesDebug = false;
  bool showUvInterpolationDebug = false;
  bool showActualF0Debug = false;
  bool snapToSemitoneDrag = false;
  DragSnapMode dragSnapMode = DragSnapMode::Chromatic;

  int pitchReferenceHz = 440;
  TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Beats;
  int timelineBeatNumerator = 4;
  int timelineBeatDenominator = 4;
  double timelineTempoBpm = 120.0;
  TimelineGridDivision timelineGridDivision = TimelineGridDivision::Quarter;
  bool timelineSnapCycle = false;

  ScaleMode selectedScaleMode = ScaleMode::Chromatic;
  int selectedScaleRootNote = 0;
  std::optional<int> previewScaleRootNote;
  std::optional<ScaleMode> previewScaleMode;

  bool cacheInvalidated = true;
};
