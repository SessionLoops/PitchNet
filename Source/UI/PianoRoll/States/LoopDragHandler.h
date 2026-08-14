#pragma once

#include "InteractionHandler.h"

/**
 * Handles loop range drag interactions on the loop timeline.
 * Always active (priority over edit modes) when clicking on the loop timeline area.
 */
class LoopDragHandler : public InteractionHandler {
public:
  explicit LoopDragHandler(PianoRollComponent &owner);

  bool mouseDown(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseDrag(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent &e, float worldX,
               float worldY) override;
  void mouseMove(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool isActive() const override;
  void cancel() override;

  // Accessors for rendering (drawLoopOverlay / drawLoopTimeline)
  bool isDragging() const { return loopDragMode != LoopDragMode::None; }
  bool isHoveringHandle() const { return hoveringHandle; }
  double getDragStartSeconds() const { return loopDragStartSeconds; }
  double getDragEndSeconds() const { return loopDragEndSeconds; }

private:
  enum class LoopDragMode { None, Create, ResizeStart, ResizeEnd, Move };

  LoopDragMode loopDragMode = LoopDragMode::None;
  float loopDragStartX = 0.0f;
  double loopDragStartSeconds = 0.0;
  double loopDragEndSeconds = 0.0;
  double loopDragAnchorSeconds = 0.0;
  double loopDragOriginalStart = 0.0;
  double loopDragOriginalEnd = 0.0;
  bool hoveringHandle = false;
  static constexpr float loopHandleHitPadding = 6.0f;
};
