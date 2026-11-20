#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

/**
 * Draws and edits a polyphonic take's notes in the piano roll, in the vocal
 * editor's look (docs/polyphonic-integration-plan.md, section 5 and feature
 * review #1-#3, #8, #9, #19, #20):
 *
 *  - each active note is a blob around its pitch, its width following the
 *    note's OWN level per analysis frame (its partials' model amplitudes),
 *    scaled once per take so the loudest moment matches the mix's envelope,
 *    and by the note's gain edit;
 *  - drawn where its edit puts it: shifted, moved, stretched or cut; a muted
 *    (deleted) note is a dashed outline that can be selected, unmuted with
 *    Delete or a double-click, or restored, but takes no other edit;
 *  - tuning colours as for vocal notes (notecolours), from the note's median
 *    pitch plus its edit;
 *  - selection shown by an outline that follows the note's shape, no box;
 *  - no pitch line and no hover state: notes are edited as whole units.
 *
 * Gestures, as in the Polyphonic Lab: drag a note to change its pitch (and,
 * once the drag goes sideways, its time), drag either end to stretch it (the
 * end of a selected note stretches the whole selection by the same time),
 * click / Shift- or Cmd-click / drag on empty space to select. Edits are made
 * on a working copy during a gesture and handed over once, at its end, through
 * onEditsCommitted (one undo step).
 *
 * Works in world coordinates, like NoteRenderer (the caller has applied the
 * scroll offset).
 */
class PolyphonicNoteLayer
{
public:
  using Edits = Project::PolyphonicEdits;

  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }

  /** Binds a project; clears the selection when its take changes. */
  void setProject(Project *project);

  /** True when the bound project has a polyphonic take to draw. */
  bool isActive() const;

  void draw(juce::Graphics &g, int componentWidth);
  /** Track mode's pinned tracks: notes drawn as outlines in this colour only
   *  (nullopt: the normal look). */
  void setOutlineColour(std::optional<juce::Colour> colour) { outlineColour = colour; }

  /** The active note under a world-space point (where its edit puts it), or -1. */
  int noteAt(float worldX, float worldY);
  /** True over either end of a note that can be stretched (for the cursor). */
  bool isOverStretchEdge(float worldX, float worldY);

  // ---- gestures (world coordinates) ----
  /** Drag snapping: on/off (Alt inverts it for one drag) and the snap itself,
   *  raw semitone offset + the note's pitch -> snapped offset. */
  void setDragSnap(bool enabled, std::function<float(float, float)> snapOffset);
  /** The pitch a double-click tunes a note to: (edited median pitch, start and
   *  end seconds) -> target pitch. */
  void setDoubleClickTarget(std::function<float(float, double, double)> target);

  void mouseDown(float worldX, float worldY, const juce::ModifierKeys &mods);
  void mouseDrag(float worldX, float worldY, const juce::ModifierKeys &mods);
  void mouseUp();
  /** Tunes the note under the point to its double-click target, or brings a
   *  deleted one back (as Delete does). True if a note was hit. */
  bool mouseDoubleClick(float worldX, float worldY);
  bool isDragging() const { return drag != Drag::none && drag != Drag::preview; }
  /** The notes a running drag is editing (empty otherwise), for live audition. */
  std::vector<int> getDraggedNotes() const;

  // ---- commands on the selection (each one undo step) ----
  /** Mutes the selection, or unmutes it when every selected note is muted. */
  void toggleMuteSelection();
  /** Clears the selected notes' edits. */
  void restoreSelection();
  void setSelectionGainDb(float gainDb);
  /** Moves every selected (unmuted) note's pitch by `semitones`, keeping their spread. */
  void shiftSelectionSemitones(float semitones, const juce::String &undoName);

  // A live preview of a toolbar box being dragged: the selection drawn as if
  // shifted (or re-gained), measured from the project's edits, with nothing
  // committed or rendered. endPreview() drops it; the box then commits the
  // real edit on release.
  void previewSelectionShift(float semitones);
  void previewSelectionGain(float gainDb);
  void endPreview();

  /** The selection's pitch for the toolbar readout: the first selected unmuted
   *  note's edited pitch (MIDI, A4 = 69), and whether the others differ from it
   *  in their nearest semitone or in their cents from it (on the grid of
   *  `referenceHz`). nullopt when no unmuted note is selected. */
  struct PitchReadout
  {
    float midi = 0.0f;
    bool notesDiffer = false;
    bool centsDiffer = false;
  };
  std::optional<PitchReadout> getSelectionPitch(int referenceHz) const;
  /** The selected unmuted notes' common gain in dB; nullopt when none is
   *  selected, NaN when they differ. */
  std::optional<float> getSelectionGainDb() const;

  /** The edits being drawn: a running gesture's working copy, else the project's. */
  const Edits &currentEdits() const;

  const std::set<int> &getSelection() const { return selection; }
  /** Selects `note` (or clears the selection for -1); `additive` toggles it. */
  void select(int note, bool additive);
  void selectAll();
  void clearSelection();

  /** A gesture or command changed the edits: before, after, undo name. */
  std::function<void(const Edits &, const Edits &, const juce::String &)> onEditsCommitted;
  /** A drag changed the working edits (repaint, live audition). */
  std::function<void()> onEditsChanging;
  std::function<void()> onSelectionChanged;

private:
  struct NoteShape
  {
    int note = -1;            // index into analysis->notes
    int firstFrame = 0;       // first frame of `level`
    std::vector<float> level; // display level 0..1 per analysis frame
    float peakLevel = 0.0f;
    float medianMidi = 0.0f;
  };

  enum class Drag
  {
    none,
    notes,
    stretch,
    band,
    preview // a toolbar box is being dragged
  };

  void rebuildIfNeeded();
  const NoteShape *shapeFor(int note) const;
  polyphonic::NoteEdit editOf(int note) const;
  float halfHeightAt(const NoteShape &shape, int frame, float waveHeight,
                     float gain) const;
  float waveHeight() const;
  float rowCentre(float midi) const;
  double secondsPerFrame() const;
  double takeSeconds() const;
  /** Seconds at which `note`'s (source) frame plays under `edit`. */
  double playedAt(int note, const polyphonic::NoteEdit &edit, double frame) const;
  /** The note's edited extent in world space (for culling and band selection). */
  juce::Rectangle<float> noteBounds(const NoteShape &shape) const;
  juce::Path notePath(const NoteShape &shape, const polyphonic::NoteEdit &edit) const;
  double clampOffset(int note, double offset) const;
  bool canStretch(int note, const polyphonic::NoteEdit &edit) const;

  struct Edge
  {
    int note = -1;
    bool start = false; // the left end: the note's end stays put
  };
  Edge edgeAt(float worldX, float worldY);

  void dragNotes(float worldX, float worldY, const juce::ModifierKeys &mods);
  void dragStretch(float worldX);
  /** Stretches one note so the grabbed end moves by dt seconds; true if its edit changed. */
  bool stretchNoteBy(int note, double dt);
  /** Hands `working` to the project and reports the change (if any) as one step. */
  void commit(const juce::String &name);
  void selectionChanged();

  std::optional<juce::Colour> outlineColour;
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;

  std::shared_ptr<const polyphonic::Analysis> builtFor;
  std::vector<NoteShape> shapes;
  std::vector<int> shapeIndex; // analysis note -> index into shapes, or -1
  std::set<int> selection;

  bool dragSnapEnabled = true;
  std::function<float(float, float)> snapOffset;
  std::function<float(float, double, double)> doubleClickTarget;

  Drag drag = Drag::none;
  juce::Point<float> dragStart;
  bool dragMoved = false;
  bool timeEngaged = false;  // the drag moves the notes in time
  bool pitchEngaged = false; // the drag changes their pitch
  int pressedNote = -1;
  int pendingSingle = -1; // a click on a selected note without a drag selects just it
  int stretchNote = -1;               // the note whose end was grabbed
  std::vector<int> stretchTargets;    // the notes the stretch applies to
  bool stretchFromStart = false;
  Edits before;   // the project's edits when the gesture began
  Edits working;  // the gesture's edits
  std::set<int> bandBase;
  juce::Rectangle<float> band;
};
