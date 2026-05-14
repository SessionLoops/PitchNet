#include "PianoRollInteractionContext.h"
#include "../PianoRollComponent.h"

PianoRollInteractionContext::PianoRollInteractionContext(
    PianoRollComponent &component)
    : project(component.project), undoManager(component.undoManager),
      coordMapper(component.coordMapper), pitchEditor(component.pitchEditor),
      boxSelector(component.boxSelector), noteSplitter(component.noteSplitter),
      pitchToolHandles(component.pitchToolHandles),
      pitchToolController(component.pitchToolController),
      viewState(component.viewState),
      hoveredPitchToolHandle(component.hoveredPitchToolHandle),
      pixelsPerSecond(component.pixelsPerSecond),
      pixelsPerSemitone(component.pixelsPerSemitone),
      scrollX(component.scrollX),
      snapToSemitoneDrag(component.snapToSemitoneDrag),
      pitchReferenceHz(component.pitchReferenceHz),
      selectedScaleMode(component.selectedScaleMode),
      selectedScaleRootNote(component.selectedScaleRootNote),
      doubleClickSnapMode(component.doubleClickSnapMode),
      lastDragRepaintTime(component.lastDragRepaintTime),
      onNoteSelected(component.onNoteSelected),
      onPitchEdited(component.onPitchEdited),
      onPitchEditFinished(component.onPitchEditFinished),
      onLoopRangeChanged(component.onLoopRangeChanged),
      onReinterpolateUV(component.onReinterpolateUV), component(component)
{
}

void PianoRollInteractionContext::repaint() { component.repaint(); }

void PianoRollInteractionContext::setMouseCursor(
    const juce::MouseCursor &cursorType)
{
  component.setMouseCursor(cursorType);
}

float PianoRollInteractionContext::midiToY(float midiNote) const
{
  return component.midiToY(midiNote);
}

float PianoRollInteractionContext::yToMidi(float y) const
{
  return component.yToMidi(y);
}

float PianoRollInteractionContext::timeToX(double time) const
{
  return component.timeToX(time);
}

double PianoRollInteractionContext::xToTime(float x) const
{
  return component.xToTime(x);
}

double PianoRollInteractionContext::snapTimeToTimelineGrid(
    double timeSeconds) const
{
  return component.snapTimeToTimelineGrid(timeSeconds);
}

#if PITCHNET_ENABLE_STRETCH
StretchMode PianoRollInteractionContext::getEffectiveStretchMode(
    const juce::ModifierKeys &mods) const
{
  return component.getEffectiveStretchMode(mods);
}
#endif

Note *PianoRollInteractionContext::findNoteAt(float x, float y)
{
  return component.findNoteAt(x, y);
}

std::vector<Note *> PianoRollInteractionContext::getSelectedNotes() const
{
  return component.getSelectedNotes();
}

void PianoRollInteractionContext::updatePitchToolHandlesFromSelection()
{
  component.updatePitchToolHandlesFromSelection();
}

void PianoRollInteractionContext::invalidateWaveformCache()
{
  component.invalidateWaveformCache();
}

void PianoRollInteractionContext::invalidateBasePitchCache()
{
  component.invalidateBasePitchCache();
}
