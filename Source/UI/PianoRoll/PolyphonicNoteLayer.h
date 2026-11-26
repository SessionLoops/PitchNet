#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Models/PolyphonicAssignment.h"
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
 * Note assignment mode (setAssignmentMode, Melodyne's Note Assignment Mode)
 * edits the detection instead of the sound: it shows the take as analysed
 * (edits are hidden until the mode is left), each candidate the analysis kept
 * as a dashed outline, like a deleted note, and for the note under the pointer (or the one
 * selected) its harmonic series and its own candidates. Double-click turns a
 * note into a candidate or a candidate into a note, Delete deactivates the
 * selection, and dragging a note up or down moves it to a candidate it
 * overlaps or to one of its harmonics (x2, x3, x4) or subharmonics (/2, /3,
 * /4), where it snaps; anywhere else it springs back. With the Split tool
 * (setSplitTool) a click splits the note under the pointer there, and a click
 * on the separation between two consecutive notes (polyphonicassignment::
 * mergePartner) merges them, as the vocal editor's Split tool does. Each
 * change goes out through onAssignmentCommitted (one undo step).
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

  // ---- note assignment (see the class comment) ----
  void setAssignmentMode(bool enabled);
  bool isAssignmentMode() const { return assignmentMode; }
  /** The note assignment slider's bracket: the note filter's probability a
   *  candidate needs to be shown (0: all). */
  void setCandidateThreshold(float probability);
  /** The slider's ball being dragged: notes drawn as active at or above this
   *  probability (nullopt: as assigned). Nothing changes until commitThreshold. */
  void setThresholdPreview(std::optional<float> probability);
  /** The ball let go: every note at or above `probability` active, every other
   *  one a candidate (polyphonicassignment::notesAtThreshold), as one step. */
  void commitThreshold(float probability);
  /** The pointer moved (world coordinates; no position: it left the canvas).
   *  True if what is highlighted changed (repaint). */
  bool setHoverPoint(std::optional<juce::Point<float>> worldPoint);
  /** Deactivates the selected active notes (Delete in assignment mode). */
  void deactivateSelection();
  /** Assignment mode's Split tool (the editor's Split mode) instead of Select. */
  void setSplitTool(bool enabled);
  /** For the cursor: the pointer would split a note / merge two. */
  bool isOverSplitPoint() const { return assignmentMode && splitTool && splitHover.note >= 0; }
  bool isOverMergeBoundary() const { return assignmentMode && splitTool && mergeHover.second >= 0; }

  /** What assignment mode plays while a note or candidate is pressed (Live
   *  Audition): a pressed note's own separated sound, unedited, while it is
   *  not dragged anywhere; otherwise a tone (notetone) along a pitch curve
   *  with a level: a pressed candidate's, the candidate a dragged note would
   *  snap to, or the dragged note moved by the harmonic it would snap to. A
   *  note without masks yet (being rebuilt) is a tone too. */
  struct AssignmentAudition
  {
    int note = -1;           // the note or candidate it is about; -1: nothing
    bool separated = false;  // its separated sound (else the tone below)
    int startFrame = 0;
    std::vector<float> f0;   // Hz per frame from startFrame
    std::vector<float> level;
    int candidate = -1;      // what it stands for, to tell a change
    float ratio = 1.0f;
  };
  AssignmentAudition getAssignmentAudition() const;
  /** The audition changed: a note or candidate was pressed, or a drag found
   *  (or lost) a pitch to snap to. Released: onPolyphonicDragFinished. */
  std::function<void()> onAssignmentAuditionChanged;

  /** A change of assignment, as one step. */
  struct AssignmentChange
  {
    std::vector<polyphonic::Note> notesAfter;
    Edits editsAfter;        // the edits of notes whose assignment changed are dropped
    float thresholdAfter;    // the slider's ball; unchanged for a change by hand
    /** Levels to draw the new notes with until their masks are rebuilt (split,
     *  merge); nullopt: polyphonicassignment::apply's own. */
    std::optional<std::vector<std::vector<float>>> levelsAfter;
    juce::String name;
  };
  std::function<void(const AssignmentChange &)> onAssignmentCommitted;
  /** Track mode's pinned tracks: notes drawn as outlines in this colour only
   *  (nullopt: the normal look). */
  void setOutlineColour(std::optional<juce::Colour> colour) { outlineColour = colour; }

  // Notes are named by their keys in the project's edits: an analysis note's
  // index, or a copy's key (polyphonic::isCopyKey, below -1). -1 is no note.

  /** The note under a world-space point (where its edit puts it), or -1. */
  int noteAt(float worldX, float worldY);
  /** True over either end of a note that can be stretched (for the cursor). */
  bool isOverStretchEdge(float worldX, float worldY);

  // ---- gestures (world coordinates) ----
  /** Drag snapping: on/off (Alt inverts it for one drag) and the snap itself,
   *  raw semitone offset + the note's pitch -> snapped offset. */
  void setDragSnap(bool enabled, std::function<float(float, float)> snapOffset);
  /** Time snapping (the Time card's Drag Snap, Alt inverting it): a dragged
   *  time in seconds and the drag's modifiers -> the time to use. */
  void setTimeSnap(std::function<double(double, const juce::ModifierKeys &)> snapTime);
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
  /** Delete: removes the selected copies (pasted notes) and mutes the other
   *  selected notes, or unmutes them when every one of them is muted. */
  void toggleMuteSelection();
  /** Clears the selected notes' edits and removes the selected copies. */
  void restoreSelection();
  /** What Restore Original can put back on its own (as the vocal editor's
   *  restore menu): pitch (shift and pitch curve), amplitude (level) or timing
   *  (move and stretch; not for copies, whose place is where they were
   *  pasted); All is restoreSelection(). */
  enum class RestoreAspect
  {
    pitch,
    amplitude,
    timing,
    all
  };
  /** True when some selected note has that kind of edit to restore. */
  bool selectionHasEdits(RestoreAspect aspect) const;
  void restoreSelection(RestoreAspect aspect);
  /** Drops copies that no longer exist (an undone paste) from the selection. */
  void dropMissingCopies();

  // ---- clipboard (the context menu's Cut / Copy / Paste) ----
  // Copy takes the selected notes that sound (not deleted ones) as they
  // sound: each its source note and its edit, with where it starts. Paste
  // puts copies of them (NoteEdit::copyOf, keyed from polyphonic::
  // kFirstCopyKey down) on top of the take: the earliest at the given time,
  // the others at their distances from it, each with its pitch, level and
  // length; the originals stay. The pasted notes are then selected and edited
  // like any other; Delete and Restore Original remove them. Paste only goes
  // into the take the notes came from, while they are still the same notes.
  bool canCopySelection() const;
  bool canPaste() const;
  void copySelection();
  /** Copies the selection, then deletes it (as Delete, but always muting), as one step. */
  void cutSelection();
  /** Pastes at `seconds` (project time), as one step. */
  void pasteAt(double seconds);

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
    preview, // a toolbar box is being dragged
    assign,  // assignment mode: a note dragged to another pitch
    listen   // assignment mode: a candidate held down (heard as a tone)
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
  juce::Rectangle<float> noteBounds(int key, const NoteShape &shape) const;
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
  void dragStretch(float worldX, const juce::ModifierKeys &mods);
  /** dt, changed so `seconds + dt` lands where time snapping puts it. */
  double snappedDelta(double seconds, double dt, const juce::ModifierKeys &mods) const;
  /** Stretches one note so the grabbed end moves by dt seconds; true if its edit changed. */
  bool stretchNoteBy(int note, double dt);
  /** Hands `working` to the project and reports the change (if any) as one step. */
  void commit(const juce::String &name);
  void selectionChanged();
  /** The selected notes, by where they start playing. */
  std::vector<int> selectionInTimeOrder() const;
  /** Delete (muteAll false) or Cut (true), named `name` (empty: by what it did). */
  void deleteSelection(bool muteAll, const juce::String &name);
  /** The analysis note a key plays: itself, or a copy's source (-1 if gone). */
  int sourceOf(int key) const;
  /** Every note drawn, as (key, shape): the notes, then the copies on top. */
  std::vector<std::pair<int, const NoteShape *>> drawnNotes() const;

  // ---- note assignment (PolyphonicNoteLayerAssignment.cpp) ----
  /** Where an assignment drag would put the note: on a candidate (its
   *  index) or on a harmonic of itself (ratio), or nowhere. */
  struct AssignTarget
  {
    int candidate = -1;
    float ratio = 0.0f;
    float midi = 0.0f;
    bool valid() const { return candidate >= 0 || ratio > 0.0f; }
  };
  void buildCandidateShapes(const std::vector<std::vector<float>> &levels, float scale,
                            float fallbackLevel);
  /** The note (active or candidate) under a world point, or -1. */
  int assignmentHitAt(float worldX, float worldY) const;
  /** The note whose harmonics and candidates are shown: the one under the
   *  pointer, else the only selected note, else -1. */
  int assignmentFocus() const;
  bool overlapsInTime(int a, int b) const;
  /** `b` sounds with `a` on one of a's harmonics or subharmonics (x1..x8). */
  bool harmonicallyRelated(int a, int b) const;

  /** Which candidates are shown: above the threshold, and not a duplicate of an
   *  overlapping note or of a more probable overlapping candidate at the same
   *  pitch. Recomputed when the take or the amount changes. */
  void updateVisibleCandidates();
  AssignTarget assignTargetFor(int note, float midi) const;
  void drawAssignment(juce::Graphics &g, float visibleStart, float visibleEnd);
  void assignmentMouseDown(float worldX, float worldY, const juce::ModifierKeys &mods);
  void assignmentMouseDrag(float worldX, float worldY);
  void assignmentMouseUp();
  bool assignmentDoubleClick(float worldX, float worldY);
  /** Hands a new assignment of the take's notes over as one step. */
  void commitAssignment(std::vector<polyphonic::Note> notes, const juce::String &name,
                        std::optional<float> threshold = std::nullopt);
  /** Hands a split or merge over as one step; its notes are then selected. */
  void commitSeparation(polyphonicassignment::Separation separation, const juce::String &name);
  /** The split or merge under a world point (Split tool). */
  void updateSeparationHover(float worldX, float worldY);
  void drawSeparations(juce::Graphics &g, float visibleStart, float visibleEnd);
  /** The x of the separation before `second` (its first frame). */
  float separationX(int second) const;
  /** Drawn as active: as assigned, or as the ball being dragged would have it. */
  bool shownActive(int note) const;

  bool assignmentMode = false;
  std::vector<NoteShape> candidateShapes; // the inactive notes, for assignment mode
  std::vector<bool> candidateVisible;     // per candidateShapes entry
  float candidateThreshold = 0.05f;       // the slider's bracket
  std::optional<float> thresholdPreview;  // the slider's ball while dragged
  std::vector<int> candidateIndex;        // analysis note -> index into candidateShapes, or -1
  int hoverNote = -1;                     // assignment mode: note or candidate under the pointer
  bool splitTool = false;
  struct SplitHover
  {
    int note = -1;
    int frame = 0; // the first frame of the second part
  } splitHover;
  struct MergeHover
  {
    int first = -1;
    int second = -1;
  } mergeHover;
  std::optional<std::vector<int>> selectAfterRebuild; // a split or merge's notes
  float assignDragMidi = 0.0f;            // where an assignment drag has the note now
  AssignTarget assignTarget;

  std::optional<juce::Colour> outlineColour;
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;

  std::shared_ptr<const polyphonic::Analysis> builtFor;
  std::vector<NoteShape> shapes;
  std::vector<int> shapeIndex; // analysis note -> index into shapes, or -1
  std::set<int> selection;

  bool dragSnapEnabled = true;
  std::function<float(float, float)> snapOffset;
  std::function<double(double, const juce::ModifierKeys &)> snapTime;
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
