#pragma once

#include "InteractionHandler.h"
#include "../../../Utils/TransformParams.h"

#include <algorithm>
#include <utility>
#include <vector>

class PitchDrawingHandler final : public InteractionHandler
{
public:
  explicit PitchDrawingHandler(PianoRollComponent& owner);

  bool mouseDown(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  bool mouseDrag(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent& e, float worldX,
               float worldY) override;
  void mouseMove(const juce::MouseEvent& e, float worldX,
                 float worldY) override;
  void mouseDoubleClick(const juce::MouseEvent& e, float worldX,
                        float worldY) override;
  void draw(juce::Graphics& g) override;
  bool isActive() const override;
  void cancel() override;

  bool hasAnchors() const { return !anchors.empty(); }
  bool isHoveringAnchor() const { return hoveredAnchorId >= 0; }
  bool isPointerOverPitchRegion() const { return pointerOverPitchRegion; }
  bool isPitchRegionAtWorldX(float worldX) const;
  bool removeAnchorAt(float worldX, float worldY);
  const std::vector<float>& getOriginalMidiCurve() const { return originalMidiCurve; }
  const std::vector<float>& getPreviewMidiCurve() const { return previewMidiCurve; }
  // Timeline frames the preview replaces, first to last anchor inclusive;
  // {0, -1} (empty) with no anchors.
  std::pair<int, int> getDrawnFrameRange() const
  {
    if (anchors.empty())
      return {0, -1};
    const auto [first, last] = std::minmax_element(
        anchors.begin(), anchors.end(),
        [](const Anchor& a, const Anchor& b) { return a.frame < b.frame; });
    return {first->frame, last->frame};
  }
  void clearHover();
  bool apply();

private:
  struct Anchor
  {
    int id = 0;
    int frame = 0;
    float midi = 60.0f;
  };

  struct OriginalNoteState
  {
    Note* note = nullptr;
    TransformParams params;
    std::vector<float> bakedDeltaPitch;
  };

  int frameFromWorldX(float worldX) const;
  float midiFromWorldY(float worldY) const;
  juce::Point<float> anchorPosition(const Anchor& anchor) const;
  int hitTest(float worldX, float worldY) const;
  Anchor* findAnchorById(int id);
  const Anchor* findAnchorById(int id) const;
  bool frameBelongsToPitchNote(int frame) const;
  void captureOriginalCurveIfNeeded();
  void sortAnchors();
  void rebuildPreview();
  void updateAffectedNotePositions();
  void applyPreviewToProject();
  void restoreOriginalNoteStates(bool requestRender);
  const OriginalNoteState* originalStateFor(const Note& note) const;
  void clearState(bool restoreNoteStates);
  void notifyStateChanged();

  std::vector<Anchor> anchors;
  std::vector<OriginalNoteState> originalNoteStates;
  std::vector<float> originalMidiCurve;
  std::vector<float> previewMidiCurve;
  int nextAnchorId = 1;
  int hoveredAnchorId = -1;
  int activeAnchorId = -1;
  bool dragging = false;
  bool dragMoved = false;
  bool pointerOverPitchRegion = false;
  int previewDirtyStart = -1;
  int previewDirtyEnd = -1;
};
