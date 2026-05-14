#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "BoxSelector.h"
#include "CoordinateMapper.h"
#include "NoteSplitter.h"
#include "PianoRollViewState.h"
#include "PitchEditor.h"
#include "PitchToolController.h"
#include "PitchToolHandles.h"

#include <functional>
#include <memory>

class PianoRollComponent;
class PitchUndoManager;

class PianoRollInteractionContext
{
public:
  explicit PianoRollInteractionContext(PianoRollComponent &component);

  Project *&project;
  PitchUndoManager *&undoManager;

  std::unique_ptr<CoordinateMapper> &coordMapper;
  std::unique_ptr<PitchEditor> &pitchEditor;
  std::unique_ptr<BoxSelector> &boxSelector;
  std::unique_ptr<NoteSplitter> &noteSplitter;
  std::unique_ptr<PitchToolHandles> &pitchToolHandles;
  std::unique_ptr<PitchToolController> &pitchToolController;

  PianoRollViewState &viewState;
  int &hoveredPitchToolHandle;
  float &pixelsPerSecond;
  float &pixelsPerSemitone;
  double &scrollX;
  bool &snapToSemitoneDrag;
  int &pitchReferenceHz;
  ScaleMode &selectedScaleMode;
  int &selectedScaleRootNote;
  DoubleClickSnapMode &doubleClickSnapMode;

  juce::int64 &lastDragRepaintTime;
  static constexpr juce::int64 minDragRepaintInterval = 16;

  std::function<void(Note *)> &onNoteSelected;
  std::function<void()> &onPitchEdited;
  std::function<void()> &onPitchEditFinished;
  std::function<void(const LoopRange &)> &onLoopRangeChanged;
  std::function<void(int, int)> &onReinterpolateUV;

  void repaint();
  void setMouseCursor(const juce::MouseCursor &cursorType);

  float midiToY(float midiNote) const;
  float yToMidi(float y) const;
  float timeToX(double time) const;
  double xToTime(float x) const;
  double snapTimeToTimelineGrid(double timeSeconds) const;

  Note *findNoteAt(float x, float y);
  std::vector<Note *> getSelectedNotes() const;
  void updatePitchToolHandlesFromSelection();
  void invalidateWaveformCache();
  void invalidateBasePitchCache();

private:
  PianoRollComponent &component;
};
