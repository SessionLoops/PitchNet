#pragma once

#include "InteractionHandler.h"
#include "../../../Undo/TimingAction.h"

#include <utility>
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
  bool isActive() const override { return dragging || selecting; }
  /** True over a boundary a drag would move (Alt there inverts Drag Snap). */
  bool isOverBoundary(float worldX, float worldY) const;
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

  struct BoundaryKey
  {
    Note* left = nullptr;
    Note* right = nullptr;
  };

  std::vector<Boundary> buildBoundaries() const;
  int findBoundary(float worldX, float worldY,
                   const std::vector<Boundary>& boundaries) const;
  std::vector<Note*> collectAffectedNotes(const Boundary& boundary) const;
  std::vector<Note*> collectAffectedNotes(
      const std::vector<Boundary>& boundaries) const;
  std::vector<NoteTimingState> captureAffected(
      const std::vector<Boundary>& boundaries) const;
  std::pair<float, float> getGroupDeltaRange(
      const std::vector<Boundary>& boundaries) const;
  void applyPreviewDelta(float delta);
  /** Source frames from the note's start to its vowel: the leading consonant
   *  (sibilance, breath, unvoiced onset) that the note includes. 0 when the
   *  note starts on its vowel. */
  int vowelLeadSourceFrames(const Note& note) const;
  /** The boundary frame a drag to `rawFrame` snaps to. A boundary that
   *  starts a note puts that note's vowel on the grid, so its consonant sits
   *  before the beat the way it is sung; any other boundary snaps itself. */
  float snapBoundaryFrame(float rawFrame, const juce::ModifierKeys& mods) const;
  void clearAffectedPreviews();
  static BoundaryKey keyFor(const Boundary& boundary);
  static bool keysMatch(const BoundaryKey& a, const BoundaryKey& b);
  bool isSelected(const Boundary& boundary) const;
  void addSelected(const Boundary& boundary);
  void removeSelected(const Boundary& boundary);
  std::vector<Boundary> resolveSelection(
      const std::vector<Boundary>& boundaries) const;
  void updateMarqueeSelection(float worldX);

  Boundary activeBoundary;
  std::vector<Boundary> dragBoundaries;
  std::vector<BoundaryKey> selectedBoundaries;
  std::vector<BoundaryKey> selectionBaseline;
  std::vector<NoteTimingState> before;
  float previewDelta = 0.0f;
  float minimumDelta = 0.0f;
  float maximumDelta = 0.0f;
  float hoveredBoundaryFrame = -1.0f;
  float selectionStartX = 0.0f;
  float selectionCurrentX = 0.0f;
  bool dragging = false;
  bool selecting = false;
};
