#include "SplitHandler.h"
#include "../../PianoRollComponent.h"
#include "../NoteSplitter.h"

SplitHandler::SplitHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool SplitHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                             float worldY) {
  if (!owner_.isCanvasPoint(e))
    return false;

  if (mergeFirstNote && mergeSecondNote &&
      owner_.noteSplitter->mergeNotes(mergeFirstNote, mergeSecondNote))
  {
    clearGuide();
    return true;
  }

  Note *note = owner_.noteSplitter->findNoteAt(worldX, worldY);
  if (note) {
    owner_.noteSplitter->splitNoteAtX(note, worldX);
    return true;
  }
  return false;
}

void SplitHandler::mouseMove(const juce::MouseEvent &e, float worldX,
                             float worldY) {
  if (!owner_.isCanvasPoint(e)) {
    clearGuide();
    return;
  }

  if (!owner_.project) {
    clearGuide();
    return;
  }

  const auto mergeNotes = owner_.noteSplitter->findMergeableNotesAt(worldX, worldY);
  mergeFirstNote = mergeNotes.first;
  mergeSecondNote = mergeNotes.second;

  Note *note = mergeFirstNote ? nullptr : owner_.noteSplitter->findNoteAt(worldX, worldY);
  if (note) {
    splitGuideX = worldX;
    splitGuideNote = note;
  } else {
    splitGuideX = -1.0f;
    splitGuideNote = nullptr;
  }
  owner_.repaint();
}

void SplitHandler::cancel() { clearGuide(); }

void SplitHandler::clearGuide() {
  if (splitGuideX >= 0) {
    splitGuideX = -1.0f;
    splitGuideNote = nullptr;
    owner_.repaint();
  }
  mergeFirstNote = nullptr;
  mergeSecondNote = nullptr;
}
