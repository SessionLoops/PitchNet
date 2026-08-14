#pragma once

#include "InteractionHandler.h"
#include "../../../Undo/TimingAction.h"

#include <vector>

class TimingHandler final : public InteractionHandler
{
public:
  explicit TimingHandler(PianoRollComponent& owner);

  bool mouseDown(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  bool mouseDrag(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent& e, float worldX,
               float worldY) override;
  void mouseMove(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  void draw(juce::Graphics& g) override;
  bool isActive() const override { return dragging; }
  void cancel() override;

private:
  struct Boundary
  {
    float frame = 0.0f;
    Note* left = nullptr;
    Note* right = nullptr;
    Note* restCompanion = nullptr;
    float top = 0.0f;
    float bottom = 0.0f;
  };

  std::vector<Boundary> buildBoundaries() const;
  int findBoundary(float worldX, float worldY,
                   const std::vector<Boundary>& boundaries) const;
  std::vector<Note*> collectAffectedNotes(const Boundary& boundary) const;
  std::vector<NoteTimingState> captureAffected(const Boundary& boundary) const;
  float clampFrame(const Boundary& boundary, float requestedFrame) const;
  void applyPreviewFrame(float frame);
  void clearAffectedPreviews();

  Boundary activeBoundary;
  std::vector<NoteTimingState> before;
  float previewBoundaryFrame = 0.0f;
  float hoveredBoundaryFrame = -1.0f;
  bool dragging = false;
};
