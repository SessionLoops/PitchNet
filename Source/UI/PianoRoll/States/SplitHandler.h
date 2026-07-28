#pragma once

#include "InteractionHandler.h"

class Note;
class NoteSplitter;

/**
 * Handles note splitting interactions in Split edit mode.
 * Manages the split guide line and executes note splits on click.
 */
class SplitHandler : public InteractionHandler {
public:
  explicit SplitHandler(PianoRollComponent &owner);

  bool mouseDown(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  void mouseMove(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool isActive() const override { return false; }
  void cancel() override;

  // Accessors for rendering
  float getSplitGuideX() const { return splitGuideX; }
  Note *getSplitGuideNote() const { return splitGuideNote; }
  bool isHoveringMergeBoundary() const { return mergeFirstNote != nullptr; }
  bool isMergeBoundary(const Note *first, const Note *second) const
  {
    return mergeFirstNote == first && mergeSecondNote == second;
  }
  void clearGuide();

private:
  float splitGuideX = -1.0f;
  Note *splitGuideNote = nullptr;
  Note *mergeFirstNote = nullptr;
  Note *mergeSecondNote = nullptr;
};
