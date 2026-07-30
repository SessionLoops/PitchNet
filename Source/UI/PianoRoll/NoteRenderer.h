#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

// Forward declarations to avoid header cycles. Implementations include the
// full handler headers in NoteRenderer.cpp.
class SelectHandler;
class SplitHandler;
class PitchEditor;
class BoxSelector;
class PitchToolController;

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
    Overlay,
    HoverShadow,
    HoveredBody
  };

  NoteRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *m) { coordMapper = m; }
  void setProject(Project *p) { project = p; }
  void setSelectHandler(SelectHandler *h) { selectHandler = h; }
  void setSplitHandler(SplitHandler *h) { splitHandler = h; }
  void setPitchEditor(PitchEditor *e) { pitchEditor = e; }
  void setPitchToolController(PitchToolController *c) { pitchToolController = c; }
  void setBoxSelector(BoxSelector *b) { boxSelector = b; }
  void setHoveredNote(Note *note) { hoveredNote = note; }
  void setShowNoteFramesDebug(bool show) { showNoteFramesDebug = show; }
  void setPreviewPlaybackState(bool active, int startFrame, int endFrame,
                               double currentTime)
  {
    previewPlaybackActive = active;
    previewStartFrame = startFrame;
    previewEndFrame = endFrame;
    previewCurrentTime = currentTime;
  }

  void draw(juce::Graphics &g, Pass pass, bool splitModeActive,
            int componentWidth);

private:
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;
  SelectHandler *selectHandler = nullptr;
  SplitHandler *splitHandler = nullptr;
  PitchEditor *pitchEditor = nullptr;
  PitchToolController *pitchToolController = nullptr;
  BoxSelector *boxSelector = nullptr;
  Note *hoveredNote = nullptr;
  bool showNoteFramesDebug = false;
  bool previewPlaybackActive = false;
  int previewStartFrame = 0;
  int previewEndFrame = 0;
  double previewCurrentTime = 0.0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteRenderer)
};
