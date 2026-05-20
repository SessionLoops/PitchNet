#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

class OverviewPanel : public juce::Component {
public:
  OverviewPanel() = default;
  struct ViewState {
    double totalTime = 0.0;
    double cursorTime = 0.0;
    double scrollX = 0.0;
    float pixelsPerSecond = 0.0f;
    int visibleWidth = 0;
  };

  void setProject(Project *proj) {
    project = proj;
    repaint();
  }
  void setDrawBackground(bool shouldDraw) {
    drawBackground = shouldDraw;
    repaint();
  }
  void setShowSegmentsDebug(bool show) {
    showSegmentsDebug = show;
    repaint();
  }

  std::function<ViewState()> getViewState;
  std::function<void(double)> onScrollXChanged;
  std::function<void(float)> onZoomChanged;

  void paint(juce::Graphics &g) override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseMove(const juce::MouseEvent &e) override;
  void mouseExit(const juce::MouseEvent &e) override;

private:
  enum class DragMode { None, Move, ResizeLeft, ResizeRight };

  struct ViewportInfo {
    bool valid = false;
    double totalTime = 0.0;
    double startTime = 0.0;
    double endTime = 0.0;
    float startX = 0.0f;
    float endX = 0.0f;
    juce::Rectangle<float> rect;
  };

  ViewportInfo computeViewport() const;
  double timeForX(float x, const juce::Rectangle<float> &content) const;
  juce::Rectangle<float> getContentBounds() const;
  void updateCursor(DragMode mode);

  Project *project = nullptr;
  bool drawBackground = true;
  bool showSegmentsDebug = false;
  DragMode dragMode = DragMode::None;
  float dragStartX = 0.0f;
  double dragStartStartTime = 0.0;
  double dragStartEndTime = 0.0;
  double dragStartVisibleTime = 0.0;

  static constexpr int padding = 6;
  static constexpr float handleHitWidth = 6.0f;
  static constexpr float minViewportPixels = 12.0f;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OverviewPanel)
};
