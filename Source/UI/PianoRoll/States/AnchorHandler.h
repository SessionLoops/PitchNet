#pragma once

#include "InteractionHandler.h"
#include "../../../Utils/TransformParams.h"

#include <vector>

class AnchorHandler final : public InteractionHandler
{
public:
  explicit AnchorHandler(PianoRollComponent& owner);

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
