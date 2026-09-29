#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"
#include "../RegionPreview.h"

class OverviewPanel : public juce::Component {
public:
  OverviewPanel() = default;
  struct ViewState {
    double totalTime = 0.0;
    double cursorTime = 0.0;
    double scrollX = 0.0;
    float pixelsPerSecond = 0.0f;
    int visibleWidth = 0;
    // Track mode: totalTime, cursorTime and scrollX are relative to
    // viewStartSeconds (project time at the thumbnail's left edge); region
    // previews are placed through displayOffset (host = project + offset),
    // and the active project is drawn only within its span.
    bool trackMode = false;
    double viewStartSeconds = 0.0;
    double displayOffset = 0.0;
    bool hasActiveSpan = false;
    double activeSpanStart = 0.0;
    double activeSpanEnd = 0.0;
  };

  void setProject(Project *proj) {
    project = proj;
    invalidateThumbnailCache();
  }
  void invalidateThumbnailCache() {
    staticCache = {};
    cacheDirty = true;
    repaint();
  }
  void setDrawBackground(bool shouldDraw) {
    drawBackground = shouldDraw;
    invalidateThumbnailCache();
  }
  // ARA: every region of the track. The active one gets the region backdrop,
  // the others are drawn dimmed.
  void setRegionPreviews(MainViewRegionPreviewList previews) {
    regionPreviews = std::move(previews);
    invalidateThumbnailCache();
  }
  void setShowSegmentsDebug(bool show) {
    showSegmentsDebug = show;
    invalidateThumbnailCache();
  }

  std::function<ViewState()> getViewState;
  std::function<void(double)> onScrollXChanged;
  std::function<void(float)> onZoomChanged;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void repaintPlayhead(double previousTime, double newTime);
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
  juce::Rectangle<int> getPlayheadRepaintBounds(double time) const;
  void paintStaticContent(juce::Graphics &g);
  void paintViewport(juce::Graphics &g);
  void paintPlayhead(juce::Graphics &g);
  void updateCursor(DragMode mode);

  Project *project = nullptr;
  MainViewRegionPreviewList regionPreviews;
  juce::Image staticCache;
  bool cacheDirty = true;
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
