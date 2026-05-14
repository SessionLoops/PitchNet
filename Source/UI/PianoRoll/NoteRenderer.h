#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

// Forward declarations to avoid header cycles. Implementations include the
// full handler headers in NoteRenderer.cpp.
class SelectHandler;
class SplitHandler;
class PitchEditor;

/**
 * Draws note bodies (with inline waveform), selection outlines, delta-scale
 * and delta-offset handles, drag-position labels, and the split-guide marker.
 *
 * Coupled to the interaction state of the piano roll, which is supplied via
 * handler pointers rather than copied into a params struct.
 */
class NoteRenderer
{
public:
  enum class Pass
  {
    Body,
    Overlay
  };

  NoteRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *m) { coordMapper = m; }
  void setProject(Project *p) { project = p; }
  void setSelectHandler(SelectHandler *h) { selectHandler = h; }
  void setSplitHandler(SplitHandler *h) { splitHandler = h; }
  void setPitchEditor(PitchEditor *e) { pitchEditor = e; }

  void draw(juce::Graphics &g, Pass pass, bool splitModeActive,
            int componentWidth);

private:
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;
  SelectHandler *selectHandler = nullptr;
  SplitHandler *splitHandler = nullptr;
  PitchEditor *pitchEditor = nullptr;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteRenderer)
};
