#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

#include <memory>
#include <set>
#include <vector>

/**
 * Draws a polyphonic take's notes in the piano roll, in the vocal editor's look
 * (docs/polyphonic-integration-plan.md, feature review #1-#3):
 *
 *  - each active note is a blob around its pitch, its width following the
 *    note's OWN level per analysis frame (its partials' model amplitudes),
 *    scaled once per take so the loudest moment matches the mix's envelope;
 *  - tuning colours as for vocal notes (notecolours), from the note's median
 *    pitch;
 *  - selection shown by an outline that follows the note's shape, no box;
 *  - no pitch line: notes are edited as whole units (feature review #2).
 *
 * Read-only in M2: selection, no hover state, no edits. Works in world coordinates,
 * like NoteRenderer (the caller has applied the scroll offset).
 */
class PolyphonicNoteLayer
{
public:
  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }

  /** Binds a project; clears the selection when its take changes. */
  void setProject(Project *project);

  /** True when the bound project has a polyphonic take to draw. */
  bool isActive() const;

  void draw(juce::Graphics &g, int componentWidth);

  /** The active note under a world-space point, or -1. */
  int noteAt(float worldX, float worldY);

  const std::set<int> &getSelection() const { return selection; }
  /** Selects `note` (or clears the selection for -1); `additive` toggles it. */
  void select(int note, bool additive);
  void selectAll();
  void clearSelection() { selection.clear(); }

private:
  struct NoteShape
  {
    int note = -1;          // index into analysis->notes
    int firstFrame = 0;     // first frame of `level`
    std::vector<float> level; // display level 0..1 per analysis frame
    float medianMidi = 0.0f;
  };

  void rebuildIfNeeded();
  float frameSeconds(double frame) const;
  float halfHeightAt(const NoteShape &shape, int frame, float waveHeight) const;

  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;

  std::shared_ptr<const polyphonic::Analysis> builtFor;
  std::vector<NoteShape> shapes;
  std::set<int> selection;
};
