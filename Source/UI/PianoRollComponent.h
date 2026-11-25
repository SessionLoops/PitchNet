#pragma once

#include "../JuceHeader.h"
#include "../Audio/IO/MidiReference.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/Localization.h"
#include "../Utils/BasePitchPreview.h"
#include "../Undo/UndoActions.h"
#include "Commands.h"
#include "PianoRoll/BoxSelector.h"
#include "PianoRoll/ChordLane.h"
#include "PianoRoll/CoordinateMapper.h"
#include "PianoRoll/GridRenderer.h"
#include "PianoRoll/NoteRenderer.h"
#include "PianoRoll/PolyphonicNoteLayer.h"
#include "PianoRoll/NoteSplitter.h"
#include "PianoRoll/PianoKeysRenderer.h"
#include "PianoRoll/PitchCurveRenderer.h"
#include "PianoRoll/PitchEditor.h"
#include "PianoRoll/TimelineRenderer.h"
#include "PianoRoll/WaveformBackgroundRenderer.h"
#include "PianoRoll/PitchToolController.h"
#include "PianoRoll/PitchToolHandles.h"
#include "PianoRoll/PianoRollViewState.h"
#include "PianoRoll/ScrollZoomController.h"
#include "Buttons.h"
#include "RegionPreview.h"

#include <memory>
#include <optional>

class PitchUndoManager;
class PianoRollInteractionContext;

// Interaction handler forward declarations
class InteractionHandler;
class LoopDragHandler;
class SelectHandler;
class SplitHandler;
class PitchDrawingHandler;
class TimingHandler;
class PitchDrawingConfirmationPanel;

/**
 * Piano roll component for displaying and editing notes.
 * Supports DPI-aware scaling for multi-monitor setups.
 */
class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener,
                           public juce::KeyListener,
                           public juce::TooltipClient
{
  friend class PianoRollInteractionContext;

public:
  using juce::Component::keyPressed;

  static constexpr int pianoKeysWidth = 30;
  static constexpr int timelineHeight = 18;
  static constexpr int loopTimelineHeight = 16;
  static constexpr int rulerHeight = timelineHeight + loopTimelineHeight;
  static constexpr int chordLaneHeight = CoordinateMapper::chordLaneHeight;
  /** Where the canvas starts: below the ruler, and below the chord track
   *  when one is shown (polyphonic takes). */
  int getHeaderHeight() const;
  static constexpr juce::int64 minDragRepaintInterval = 16; // ~60fps max

  PianoRollComponent();
  ~PianoRollComponent() override;

  void paint(juce::Graphics &g) override;
  void setMidiReferenceNotes(const std::vector<MidiReference::Note>& notes, bool visible = true);
  void resized() override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseMove(const juce::MouseEvent &e) override;
  void mouseEnter(const juce::MouseEvent &e) override;
  void mouseExit(const juce::MouseEvent &e) override;
  juce::String getTooltip() override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override;
  void mouseMagnify(const juce::MouseEvent &e, float scaleFactor) override;
  void modifierKeysChanged(const juce::ModifierKeys &modifiers) override;

  // Focus handling - re-grab focus when lost (important for plugin mode)
  void focusLost(FocusChangeType cause) override;
  void focusGained(FocusChangeType cause) override;

  // KeyListener
  bool keyPressed(const juce::KeyPress &key) override;
  bool keyPressed(const juce::KeyPress &key,
                  juce::Component *originatingComponent) override;

  void requestCanvasKeyboardFocus();

  // ScrollBar::Listener
  void scrollBarMoved(juce::ScrollBar *scrollBar,
                      double newRangeStart) override;

  // Project
  void setProject(Project *proj);
  void beginLiveRecordingWaveform(double sampleRate,
                                  double timelineOffsetSeconds);
  void appendLiveRecordingWaveform(const juce::AudioBuffer<float> &buffer);
  // Extends the plugin canvas to include the furthest host playhead position.
  bool extendTimelineTo(double endSeconds);
  Project *getProject() const { return project; }
  std::vector<Note *> getSelectedNotes() const;
  PianoRollInteractionContext &getInteractionContext()
  {
    return *interactionContext;
  }

  // Undo Manager
  void setUndoManager(PitchUndoManager *manager);
  PitchUndoManager *getUndoManager() const { return undoManager; }

  // Cursor
  void setCursorTime(double time);
  void setPlaybackActive(bool active);
  double getCursorTime() const { return cursorTime; }

  // Zoom with optional center point
  void setPixelsPerSecond(float pps, bool centerOnCursor = false);
  void setPixelsPerSemitone(float pps, float anchorContentY = -1.0f);
  float getPixelsPerSecond() const { return pixelsPerSecond; }
  float getPixelsPerSemitone() const { return pixelsPerSemitone; }

  // Visual height multiplier for note waveform blobs (1.0 = default).
  void setBlobHeightScale(float scale);
  float getBlobHeightScale() const;

  // Scale-grid visualization
  void setScaleMode(ScaleMode mode);
  void setScaleRootNote(int noteInOctave);
  void refreshScaleFromProject();
  void setScaleRootPreview(std::optional<int> noteInOctave);
  void setScaleModePreview(std::optional<ScaleMode> mode);
  void setSnapToSemitoneDrag(bool enabled);
  void setDragSnapMode(DragSnapMode mode);
  void setPitchReferenceHz(int hz);
  void setTimelineDisplayMode(TimelineDisplayMode mode);

  // Where the drawn content currently sits on the host timeline. Display only:
  // nothing in the project moves. May be negative. See CoordinateMapper.
  void setTimelineDisplayOffset(double seconds);
  double projectToTimeline(double projectSeconds) const;
  double timelineToProject(double timelineSeconds) const;
  void setTimelineBeatSignature(int numerator, int denominator);
  void setTimelineTempoBpm(double bpm);
  /** Where bar 1 starts (timeline seconds); ignored in Track mode, which
   *  follows the host's grid. */
  void setTimelineBeatOrigin(double seconds);
  void setTimelineGridDivision(TimelineGridDivision division);
  void setTimelineDragSnap(bool enabled);

  // Enable/disable cycle (loop) range editing on the timeline strip.
  // Turned off in non-ARA plugin mode, where the host transport is not ours
  // to drive.
  void setCycleEditEnabled(bool enabled);
  bool isCycleEditEnabled() const { return cycleEditEnabled; }

  // Scroll
  void setScrollX(double x);
  double getScrollX() const { return scrollX; }
  void setScrollY(double y);
  double getScrollY() const { return scrollY; }
  // End of the scrollable timeline, in the canvas's (project) time.
  double getTimelineDuration() const;
  // Start of the scrollable timeline: 0 in Clip mode; the host's time zero,
  // expressed in project time, in Track mode (may be negative).
  double getTimelineStart() const;
  double getTimelineLength() const;
  // Host-timeline seconds at the left edge of the view.
  double getHostScrollSeconds() const;

  // Track mode (ARA): show the whole track on the host timeline. The canvas
  // stays in the active modification's time; only its scrollable range
  // widens to the song, the active take is clipped to its region, and the
  // other regions (setRegionPreviews) are drawn and clickable. Clip mode is
  // the plain source view.
  void setTrackViewMode(bool shouldShowTrack);
  bool isTrackViewMode() const { return trackViewMode; }
  void centerOnPitchRange(float minMidi, float maxMidi);
  bool centerOnCurrentPitchRange();
  void fitPitchRangeToView(float minMidi, float maxMidi);
  int getVisibleContentWidth() const;
  int getVisibleContentHeight() const;
  void setHorizontalScrollBarVisible(bool shouldShow);

  // Edit mode
  void setEditMode(EditMode mode);
  EditMode getEditMode() const { return editMode; }

  // Cancel any transient pitch edit before undo/redo changes project history.
  void cancelPitchDrawingPreview();

  // View settings
  void setNoiseVisibility(bool show);
  void setShowDeltaPitch(bool show)
  {
    showDeltaPitch = show;
    repaint();
  }
  void setShowBasePitch(bool show)
  {
    showBasePitch = show;
    repaint();
  }
  void setShowSegmentsDebug(bool show)
  {
    showSegmentsDebug = show;
    repaint();
  }
  void setShowGameValuesDebug(bool show)
  {
    showGameValuesDebug = show;
    repaint();
  }
  void setShowNoteFramesDebug(bool show)
  {
    showNoteFramesDebug = show;
    repaint();
  }
  void setShowUvInterpolationDebug(bool show)
  {
    showUvInterpolationDebug = show;
    repaint();
  }
  void setShowActualF0Debug(bool show)
  {
    showActualF0Debug = show;
    repaint();
  }
  void setShowCleanedF0Debug(bool show)
  {
    showCleanedF0Debug = show;
    repaint();
  }
  void setShowVocoderF0Debug(bool show)
  {
    showVocoderF0Debug = show;
    repaint();
  }
  bool getShowDeltaPitch() const { return showDeltaPitch; }
  bool getShowBasePitch() const { return showBasePitch; }

  // Callbacks
  std::function<void(EditMode)> onEditModeRequested;
  std::function<void()> onUndoRequested;
  std::function<void()> onRedoRequested;
  std::function<void(Note *)> onNoteSelected;
  std::function<void()> onPitchEdited;
  std::function<void()> onPitchEditFinished; // Called when dragging ends
  std::function<void()> onAmplitudeEdited; // Gain commit, undo, or redo
  // Restore actions are applied here and then handed to the host exactly as
  // an undo/redo is: same UI refresh, same resynthesis trigger, nothing sent
  // to the plugin before the render lands. The argument mirrors
  // UndoableAction::requiresAudioResynthesis().
  std::function<void(bool requiresResynthesis)> onHistoryActionApplied;
  std::function<void()> onPitchPreviewRenderRequested;
  std::function<void()> onPitchEditCommitted;
  std::function<void(const Note &)> onNoteDragAudition;
  // A polyphonic take: a drag changed these notes (live audition), the drag
  // ended, and the selection changed (the parameters panel's Level field).
  std::function<void(const std::vector<int> &)> onPolyphonicDragAudition;
  std::function<void()> onPolyphonicDragFinished;
  std::function<void()> onPolyphonicSelectionChanged;
  // Note assignment changed the take's notes (polyphonicassignment::apply,
  // also on undo and redo): its masks have to be rebuilt.
  std::function<void()> onPolyphonicAssignmentChanged;
  // Note assignment mode: what a held note or candidate plays changed
  // (PolyphonicNoteLayer::getAssignmentAudition); ends with
  // onPolyphonicDragFinished.
  std::function<void()> onPolyphonicAssignmentAudition;
  std::function<void()> onNoteDragAuditionFinished;
  std::function<void(int midiNote)> onPianoKeyAudition;
  std::function<void()> onPianoKeyAuditionFinished;
  std::function<void()> onCursorMoved;
  std::function<void(double)> onSeek;
  // Invoked for a double-click on unoccupied canvas space. The owner seeks
  // before toggling transport so playback starts or stops at that position.
  std::function<void(double)> onCanvasEmptyDoubleClick;
  std::function<void(float)> onZoomChanged;
  std::function<void(double)> onScrollChanged;
  std::function<void(const LoopRange &)> onLoopRangeChanged;
  std::function<void(int, int)> onPreviewRegionRequested;
  std::function<void(int, int)>
      onReinterpolateUV; // Called to re-infer UV regions (startFrame, endFrame)

  void setPreviewPlaybackState(bool active, int startFrame, int endFrame);
  void setPreviewPlaybackPosition(double timeSeconds);

  // ARA: every region of the track, drawn behind the editable project. The
  // active entry keeps the region backdrop; the others are dimmed read-only
  // previews. A click inside an inactive region fires onInactiveRegionClicked
  // so the owner can make it active; if the owner swaps the project in
  // synchronously, the same click continues into the new region (selecting
  // or dragging the note under the pointer).
  void setRegionPreviews(MainViewRegionPreviewList previews);
  std::function<void(const juce::String &)> onInactiveRegionClicked;

private:
  enum class NoteRenderPass
  {
    Body,
    Overlay,
    HoverShadow,
    HoveredBody
  };

  void drawBackgroundWaveform(juce::Graphics &g,
                              const juce::Rectangle<int> &visibleArea);
  void drawGrid(juce::Graphics &g, bool drawRowBackgrounds = true,
                bool drawGridLines = true);
  void drawTimeline(juce::Graphics &g);
  void drawLoopTimeline(juce::Graphics &g);
  void drawNotes(juce::Graphics &g, NoteRenderPass pass);
  void drawPitchCurves(juce::Graphics &g);
  void drawPianoKeys(juce::Graphics &g);
  void drawSelectionRect(juce::Graphics &g); // Box selection rectangle
  // A polyphonic take's right-click menu: Restore Original for the selection.
  void showPolyphonicMenu(float worldX, float worldY);
  /** A text field over the chord at a project time, holding its name: type
   *  another chord (polyphonic::parseChord) and press Return to retune the
   *  take there (retuneChordSpan); Escape or clicking away cancels. */
  void showChordEditor(double projectSeconds);
  void commitChordEditor();
  void closeChordEditor(bool refocus);
  void layOutChordEditor();
  std::unique_ptr<juce::TextEditor> chordEditor;
  double chordEditStart = 0.0, chordEditEnd = 0.0;
  juce::String chordEditInitial;
  bool chordEditHost = false;
  /** Retunes the take's notes from `startSeconds` to `endSeconds` to the
   *  chord `to` (polyphonic::retuneToChord over each detected chord there):
   *  the original chords' tones move, other notes stay. One undo step. */
  void retuneChordSpan(double startSeconds, double endSeconds,
                       const polyphonic::Chord &to, const juce::String &chordName);
  void showChordMessage(double startSeconds, double endSeconds, const juce::String &text);
  // Split / merge in the chord track (a chord's name edits it, see
  // showChordEditor). Not with a host chord track: its segmentation is the
  // host's.
  ChordLane::Hit chordLaneHit(int screenX);
  /** Where a click at a project time splits its chord: the nearest grid line
   *  in Beats mode, the time itself in Time mode; none on its edges. */
  std::optional<double> chordSplitTime(double projectSeconds, const polyphonic::ChordSpan &span) const;
  void splitChordAt(double projectSeconds);
  void mergeChordsAt(double boundarySeconds);
  /** Sets the chord segmentation as one undo step (empty: detect again). */
  void setChordSegmentation(std::vector<double> boundaries, const juce::String &name);
  void updateChordLaneHover(const juce::MouseEvent &e);
  // Dragging the first chord's start or the last chord's end (one undo step
  // at mouse up; the segmentation follows the pointer meanwhile).
  void beginChordEdgeDrag(bool start);
  void dragChordEdge(const juce::MouseEvent &e);
  void finishChordEdgeDrag();
  bool chordEdgeDragging = false;
  bool chordEdgeIsStart = false;
  std::vector<double> chordEdgeBefore, chordEdgeBase;
  std::unique_ptr<juce::BubbleMessageComponent> chordMessage;
  void drawAudioSourceRegionOverlay(juce::Graphics &g);
  void drawRegionPreviews(juce::Graphics &g);
  // Track mode: inactive regions' notes and pitch curves, drawn with the same
  // renderers as the active region and then greyed.
  // Notes of pinned tracks: inactive-region bodies, outlined in track colour.
  void drawPinnedTrackNotes(juce::Graphics &g,
                            const juce::Rectangle<int> &mainArea);
  void drawInactiveRegionEdits(juce::Graphics &g,
                               const juce::Rectangle<int> &mainArea);
  const MainViewRegionPreview *findActiveRegionPreview() const;
  bool previewContains(const MainViewRegionPreview &region,
                       double projectSeconds) const;
  double clampScrollX(double x) const;
  // The active region's span in project time, only in Track mode.
  std::optional<std::pair<double, double>> getTrackModeActiveSpan() const;
  const MainViewRegionPreview* findTrackRegionAtPointer(juce::Point<float>, bool notesOnly = false) const;
  bool prepareTrackPointer(juce::Point<float>, bool notesOnly = false);
  void drawLoopOverlay(juce::Graphics &g);
  void drawGameChunksDebugOverlay(juce::Graphics &g);
  void drawBreathNotes(juce::Graphics &g);
  void drawMidiReference(juce::Graphics &g) const;
  std::pair<double, double> getMidiReferenceSpan() const;
  float getDoubleClickSnapTarget(const Note& note) const;
  // The same for any pitch sounding from startSeconds to endSeconds (project
  // time); polyphonic notes use it.
  float getDoubleClickSnapTarget(float pitch, double startSeconds, double endSeconds) const;
  void drawGameValuesDebugOverlay(juce::Graphics &g);
  void updatePitchToolHandlesFromSelection();
  void updatePitchDrawingConfirmationPopup();

  float midiToY(float midiNote) const;
  float yToMidi(float y) const;
  float timeToX(double time) const;
  double xToTime(float x) const;
  double getTimelineQuarterNoteSeconds() const;
  double getTimelineBeatSeconds() const;
  double getTimelineBarSeconds() const;
  double getTimelineGridSeconds() const;
  bool canSnapTimeToTimelineGrid() const;
  void refreshBeatGridOrigin();
  /** Drag Snap for timing drags (cycle, vocal note boundaries, polyphonic
   *  notes): on when the setting is on, Alt inverts it for the drag. */
  bool shouldSnapTimeDrag(const juce::ModifierKeys &mods) const;
  /** A dragged project time, snapped to the grid when shouldSnapTimeDrag(). */
  double snapDragTime(double timeSeconds, const juce::ModifierKeys &mods) const;
  double snapTimeToTimelineGrid(double timeSeconds,
                                const juce::ModifierKeys &mods) const;
  /** Alt over something it snaps (a note, a note end, a timing boundary)
   *  controls that drag's snapping instead of starting an Alt-drag pan. */
  bool altControlsDragSnap(float worldX, float worldY);
  bool isCanvasPoint(const juce::MouseEvent &e) const;
  /** Whether any note is selected where Cmd/Ctrl-click multi-selects (the
   *  Select tool, track view targets, or a polyphonic take). */
  bool hasMultiSelectableSelection() const;
  /** The zoom modifier (Cmd on macOS, Ctrl elsewhere) is down and is not
   *  taken over by multi-selection. */
  bool isZoomModifierDown(const juce::ModifierKeys &mods) const;
  bool isModifierZoomDrag(const juce::MouseEvent &e) const;
  void applyModifierZoomDrag(const juce::MouseEvent &e);
  bool isModifierPanDrag(const juce::MouseEvent &e) const;
  void applyModifierPanDrag(const juce::MouseEvent &e);
  void updateMouseCursorForEditMode();

  Note *findNoteAt(float x, float y);
  Note *findPreviewButtonNoteAt(float x, float y) const;

  // Breath notes (breathnotes, BreathNotes.h): the one at a world position
  // (its key, or -1). They are not selected: a press on one starts dragging
  // its level, and a double-click resets it.
  int findBreathAt(float x, float y) const;
  void beginBreathDrag(int key, float worldY);
  void dragBreaths(float worldY);
  void releaseBreaths();
  bool isDraggingBreaths() const { return breathDragging; }
  void resetBreathLevel(int key);
  juce::Rectangle<float> getPreviewButtonBounds(const Note &note) const;
  juce::Rectangle<float> getNoteHoverShadowBounds(const Note &note) const;
  juce::Rectangle<float> getResetButtonBounds(const Note &note) const;
  juce::Rectangle<float> getPreviewHoverBounds(const Note &note) const;
  juce::Rectangle<int> getPreviewButtonLocalBounds(const Note &note) const;
  juce::Rectangle<int> getResetButtonLocalBounds(const Note &note) const;
  void updatePreviewButtonBounds();
  void triggerPreviewForNote(Note &note);
  std::vector<Note *> getResetTargetNotes(Note &note) const;
  enum class NoteRestoreMode { Pitch, Formant, Amplitude, All };
  void resetNoteEdits(Note &note, NoteRestoreMode mode = NoteRestoreMode::Pitch);
  std::unique_ptr<UndoableAction> createResetTimingAction(Note &note);
  void resetNoteTiming(Note &note);
  // Runs a Restore action, records it, and reports it via
  // onHistoryActionApplied so it is processed like undo/redo.
  void applyRestoreAction(std::unique_ptr<UndoableAction> action);
  void showResetMenu(Note &note);
  void setHoveredNote(Note *note);
  void updateScrollBars();
  bool nudgeSelectedNotesBySemitones(int semitoneDelta);
  void reapplyBasePitchForNote(
      Note *note); // Recalculate F0 from base pitch + delta after undo/redo

  Project *project = nullptr;
  std::vector<MidiReference::Note> midiReferenceNotes;
  bool midiReferenceVisible = false;
  PitchUndoManager *undoManager = nullptr;

  // New modular components
  std::unique_ptr<CoordinateMapper> coordMapper;
  std::unique_ptr<PianoKeysRenderer> pianoKeysRenderer;
  std::unique_ptr<GridRenderer> gridRenderer;
  std::unique_ptr<TimelineRenderer> timelineRenderer;
  std::unique_ptr<WaveformBackgroundRenderer> waveformBackgroundRenderer;
  double liveTimelineEndSeconds = 0.0;
  double hostTimelineEndSeconds = 0.0;
  double liveRecordingSampleRate = 0.0;
  std::unique_ptr<NoteRenderer> noteRenderer;
  // A polyphonic take's notes (docs/polyphonic-integration-plan.md); when it
  // is active it replaces the vocal notes, pitch curve, breath notes, tools
  // and handles on the canvas.
  std::unique_ptr<PolyphonicNoteLayer> polyphonicLayer =
      std::make_unique<PolyphonicNoteLayer>();
  // The chord track under the ruler, for polyphonic takes.
  std::unique_ptr<ChordLane> chordLane = std::make_unique<ChordLane>();
  /** Shows the chord track exactly when a polyphonic take is active; the
   *  canvas moves down by its height. */
  void syncChordLaneVisibility();
  /** The beat grid chords are detected on: the timeline's beat in Beats
   *  mode, none (free time) in Time mode. */
  ChordLane::Grid getChordGrid() const;

public:
  bool isPolyphonicActive() const
  {
    return polyphonicLayer != nullptr && polyphonicLayer->isActive();
  }
  /** Select All for a polyphonic take (its notes are not Project notes). */
  void selectAllPolyphonicNotes()
  {
    polyphonicLayer->selectAll();
    repaint();
  }
  PolyphonicNoteLayer &getPolyphonicLayer() { return *polyphonicLayer; }
  /** Note assignment mode for a polyphonic take (Melodyne's): the take as
   *  analysed, with its candidates, and gestures that edit the detection. */
  void setPolyphonicAssignmentMode(bool enabled)
  {
    polyphonicLayer->setAssignmentMode(enabled);
    repaint();
  }
  bool isPolyphonicAssignmentMode() const { return polyphonicLayer->isAssignmentMode(); }
  /** The host's chord track (ARA), in host timeline seconds; empty: none,
   *  and the chord track shows the chords detected in the take. */
  void setHostChords(std::vector<polyphonic::ChordSpan> timelineChords)
  {
    chordLane->setHostChords(std::move(timelineChords));
    repaint();
  }
  /** Whether the chord track is shown (a polyphonic take). */
  bool isChordLaneVisible() const { return coordMapper->isChordLaneVisible(); }
  /** The take's length changed (a polyphonic render grew or shrank it). */
  void refreshTimelineExtent()
  {
    updateScrollBars();
    repaint();
  }

private:
  std::unique_ptr<PitchCurveRenderer> pitchCurveRenderer;
  std::unique_ptr<NoteRenderer> previewNoteRenderer;
  // Track mode: inactive and pinned clips with a polyphonic take.
  std::unique_ptr<PolyphonicNoteLayer> previewPolyphonicLayer =
      std::make_unique<PolyphonicNoteLayer>();
  std::unique_ptr<PitchCurveRenderer> previewCurveRenderer;
  struct InactiveLayerKey
  {
    double scrollX = 0.0, scrollY = 0.0, displayOffset = 0.0;
    float pixelsPerSecond = 0.0f, pixelsPerSemitone = 0.0f;
    float blobHeightScale = 1.0f;
    int width = 0, height = 0;
    const void *previews = nullptr;
    bool showDeltaPitch = false, showBasePitch = false;
    bool operator==(const InactiveLayerKey &o) const
    {
      return scrollX == o.scrollX && scrollY == o.scrollY &&
             displayOffset == o.displayOffset &&
             pixelsPerSecond == o.pixelsPerSecond &&
             pixelsPerSemitone == o.pixelsPerSemitone &&
             blobHeightScale == o.blobHeightScale && width == o.width &&
             height == o.height && previews == o.previews &&
             showDeltaPitch == o.showDeltaPitch &&
             showBasePitch == o.showBasePitch;
    }
  };
  struct InactiveMacroPreview {
    struct Binding {
      Note* display;
      const Note* source;
      float originalMidi, originalDrift;
    };
    std::shared_ptr<Project> project;
    std::function<bool()> isValid;
    std::vector<Binding> bindings;
    std::unique_ptr<PitchCurveRenderer> renderer;
  };
  std::vector<InactiveMacroPreview> inactiveMacroPreviews;
  double lastInactiveMacroUpdate = 0.0;
  struct TrackGestureNote {
    Note* note;
    Project* owner;
    float midi, finalMidi;
  };
  bool trackGestureActive = false, trackBoxSelecting = false;
  juce::Point<float> trackGestureStart, trackGestureEnd;
  std::vector<TrackPitchMacroTarget> trackGestureTargets;
  std::vector<TrackGestureNote> trackGestureNotes;
  std::vector<const Note*> trackBoxInitialSelection;
  std::vector<juce::Rectangle<float>> trackNoteBounds(const Note&, const Project&) const;
  void syncTrackPreviewSelection();
  InactiveLayerKey inactiveLayerKey;
  juce::Image inactiveLayer;
  InactiveLayerKey pinnedLayerKey;
  juce::Image pinnedLayer;
  std::unique_ptr<ScrollZoomController> scrollZoomController;
  std::unique_ptr<PitchEditor> pitchEditor;
  std::unique_ptr<BoxSelector> boxSelector;
  std::unique_ptr<NoteSplitter> noteSplitter;
  std::unique_ptr<PitchToolHandles> pitchToolHandles;
  std::unique_ptr<PitchToolController> pitchToolController;

  std::unique_ptr<PianoRollInteractionContext> interactionContext;

  PianoRollViewState viewState;
  int &hoveredPitchToolHandle = viewState.hoveredPitchToolHandle;
  Note *hoveredNote = nullptr;
  // Breath notes: selected and hovered keys, and a level drag in progress.
  struct BreathDragItem
  {
    int key = -1;
    int sourceFirst = 0;
    int sourceLast = 0;
    int firstSample = 0;
    int lastSample = 0;
    float startDb = 0.0f;
    float db = 0.0f;
  };
  bool showBreathsAndSibilance = true;
  int hoveredBreath = -1;
  // While a breath's level is dragged: the gain the drag will apply to the
  // rendered audio, per sample, for the background waveform (worked out once
  // per drag step).
  const VisualWaveformEnvelope::GainCurve *breathGainPreview() const;
  mutable VisualWaveformEnvelope::GainCurve breathPreview;
  mutable bool breathPreviewValid = false;
  bool breathDragging = false;
  float breathDragStartY = 0.0f;
  int breathDragGrabbed = -1;
  std::vector<BreathDragItem> breathDragItems;
  Button previewButton;
  Button resetButton;
  int previewButtonWidth = 28;
  int previewButtonHeight = 28;
  int resetButtonWidth = 28;
  int resetButtonHeight = 28;
  bool previewPlaybackActive = false;
  int previewPlaybackStartFrame = 0;
  int previewPlaybackEndFrame = 0;
  double previewPlaybackCurrentTime = 0.0;

  float &pixelsPerSecond = viewState.pixelsPerSecond;
  float &pixelsPerSemitone = viewState.pixelsPerSemitone;

  double &cursorTime = viewState.cursorTime;
  bool playbackActive = false;
  double &scrollX = viewState.scrollX;
  double &scrollY = viewState.scrollY;

  // Edit mode
  EditMode &editMode = viewState.editMode;
  juce::MouseCursor splitMouseCursor;
  juce::MouseCursor mergeMouseCursor;
  juce::MouseCursor zoomMouseCursor;

  // View settings
  bool &showDeltaPitch = viewState.showDeltaPitch;
  bool &showBasePitch = viewState.showBasePitch;
  bool &showSegmentsDebug = viewState.showSegmentsDebug;
  bool &showGameValuesDebug = viewState.showGameValuesDebug;
  bool &showNoteFramesDebug = viewState.showNoteFramesDebug;
  bool &showUvInterpolationDebug = viewState.showUvInterpolationDebug;
  bool &showActualF0Debug = viewState.showActualF0Debug;
  bool &showCleanedF0Debug = viewState.showCleanedF0Debug;
  bool &showVocoderF0Debug = viewState.showVocoderF0Debug;
  bool &snapToSemitoneDrag = viewState.snapToSemitoneDrag;
  DragSnapMode &dragSnapMode = viewState.dragSnapMode;
  int &pitchReferenceHz = viewState.pitchReferenceHz;
  TimelineDisplayMode &timelineDisplayMode = viewState.timelineDisplayMode;
  int &timelineBeatNumerator = viewState.timelineBeatNumerator;
  int &timelineBeatDenominator = viewState.timelineBeatDenominator;
  double &timelineTempoBpm = viewState.timelineTempoBpm;
  TimelineGridDivision &timelineGridDivision = viewState.timelineGridDivision;
  bool &timelineDragSnap = viewState.timelineDragSnap;
  bool &cycleEditEnabled = viewState.cycleEditEnabled;
  ScaleMode &selectedScaleMode = viewState.selectedScaleMode;
  int &selectedScaleRootNote = viewState.selectedScaleRootNote;
  std::optional<int> &previewScaleRootNote = viewState.previewScaleRootNote;
  std::optional<ScaleMode> &previewScaleMode = viewState.previewScaleMode;

  // Interaction handlers (state machine pattern)
  std::unique_ptr<LoopDragHandler> loopDragHandler_;
  std::unique_ptr<SelectHandler> selectHandler_;
  std::unique_ptr<SplitHandler> splitHandler_;
  std::unique_ptr<PitchDrawingHandler> pitchDrawingHandler_;
  std::unique_ptr<TimingHandler> timingHandler_;
  InteractionHandler *currentHandler_ = nullptr;

  std::unique_ptr<PitchDrawingConfirmationPanel> pitchDrawingConfirmationPanel;

  // Scrollbars
  juce::ScrollBar horizontalScrollBar{false};
  juce::ScrollBar verticalScrollBar{true};
  bool showHorizontalScrollBar = true;

public:
  void invalidateWaveformCache();
  void invalidateBasePitchCache();
  void beginMacroCurvePreview(const std::vector<Note*>& notes);
  void endMacroCurvePreview();
  std::function<std::vector<TrackPitchMacroTarget>()> getTrackEditTargets;
  bool beginTrackGesture(const juce::MouseEvent&);
  void updateTrackGesture(const juce::MouseEvent&);
  void finishTrackGesture();
  void beginInactiveMacroPreview(const std::vector<TrackPitchMacroTarget>& targets);

private:
  // Mouse drag throttling
  juce::int64 lastDragRepaintTime = 0;
  bool modifierZoomDragActive = false;
  juce::Point<float> modifierZoomLastPosition;
  bool modifierPanDragActive = false;
  juce::Point<float> modifierPanLastPosition;
  bool pianoKeyAuditionMouseDown = false;
  bool middleButtonScrubActive = false;
  MainViewRegionPreviewList regionPreviews;
  bool trackViewMode = false;
  double regionPreviewsEndSeconds = 0.0;
  // Set when a click only activated another region (its project was not
  // ready yet), so the rest of that gesture must not edit anything.
  bool swallowGestureUntilMouseUp = false;

  // Re-applies the hover-button tooltips after a language change; the menus
  // and handle labels are built on demand, so they need nothing here.
  void refreshLocalisedText();

  LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};
