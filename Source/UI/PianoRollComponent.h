#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/BasePitchPreview.h"
#include "../Undo/UndoActions.h"
#include "Commands.h"
#include "PianoRoll/BoxSelector.h"
#include "PianoRoll/CoordinateMapper.h"
#include "PianoRoll/GridRenderer.h"
#include "PianoRoll/NoteRenderer.h"
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

#include <memory>
#include <optional>

class PitchUndoManager;
class PianoRollInteractionContext;

// Interaction handler forward declarations
class InteractionHandler;
class LoopDragHandler;
class SelectHandler;
class DrawHandler;
class SplitHandler;

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
  static constexpr int headerHeight = timelineHeight + loopTimelineHeight;
  static constexpr juce::int64 minDragRepaintInterval = 16; // ~60fps max

  PianoRollComponent();
  ~PianoRollComponent() override;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseMove(const juce::MouseEvent &e) override;
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
  double getCursorTime() const { return cursorTime; }

  // Zoom with optional center point
  void setPixelsPerSecond(float pps, bool centerOnCursor = false);
  void setPixelsPerSemitone(float pps, float anchorContentY = -1.0f);
  float getPixelsPerSecond() const { return pixelsPerSecond; }
  float getPixelsPerSemitone() const { return pixelsPerSemitone; }

  // Scale-grid visualization
  void setScaleMode(ScaleMode mode);
  void setScaleRootNote(int noteInOctave);
  void setScaleRootPreview(std::optional<int> noteInOctave);
  void setScaleModePreview(std::optional<ScaleMode> mode);
  void setSnapToSemitoneDrag(bool enabled);
  void setDragSnapMode(DragSnapMode mode);
  void setPitchReferenceHz(int hz);
  void setTimelineDisplayMode(TimelineDisplayMode mode);
  void setTimelineBeatSignature(int numerator, int denominator);
  void setTimelineTempoBpm(double bpm);
  void setTimelineGridDivision(TimelineGridDivision division);
  void setTimelineSnapCycle(bool enabled);

  // Scroll
  void setScrollX(double x);
  double getScrollX() const { return scrollX; }
  void setScrollY(double y);
  double getScrollY() const { return scrollY; }
  double getTimelineDuration() const;
  void centerOnPitchRange(float minMidi, float maxMidi);
  bool centerOnCurrentPitchRange();
  void fitPitchRangeToView(float minMidi, float maxMidi);
  int getVisibleContentWidth() const;
  int getVisibleContentHeight() const;
  void setHorizontalScrollBarVisible(bool shouldShow);

  // Edit mode
  void setEditMode(EditMode mode);
  EditMode getEditMode() const { return editMode; }

  // Cancel current drawing operation (used when undo is triggered during
  // drawing)
  void cancelDrawing();

  // View settings
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
  std::function<void(Note *)> onNoteSelected;
  std::function<void()> onPitchEdited;
  std::function<void()> onPitchEditFinished; // Called when dragging ends
  std::function<void(const Note &)> onNoteDragAudition;
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
  void drawAudioSourceRegionOverlay(juce::Graphics &g);
  void drawLoopOverlay(juce::Graphics &g);
  void drawGameChunksDebugOverlay(juce::Graphics &g);
  void drawGameValuesDebugOverlay(juce::Graphics &g);
  void updatePitchToolHandlesFromSelection();

  float midiToY(float midiNote) const;
  float yToMidi(float y) const;
  float timeToX(double time) const;
  double xToTime(float x) const;
  double getTimelineQuarterNoteSeconds() const;
  double getTimelineBeatSeconds() const;
  double getTimelineBarSeconds() const;
  double getTimelineGridSeconds() const;
  bool shouldSnapCycleToGrid() const;
  double snapTimeToTimelineGrid(double timeSeconds) const;
  bool isCanvasPoint(const juce::MouseEvent &e) const;
  bool isModifierZoomDrag(const juce::MouseEvent &e) const;
  void applyModifierZoomDrag(const juce::MouseEvent &e);
  bool isModifierPanDrag(const juce::MouseEvent &e) const;
  void applyModifierPanDrag(const juce::MouseEvent &e);
  void updateMouseCursorForEditMode();

  Note *findNoteAt(float x, float y);
  Note *findPreviewButtonNoteAt(float x, float y) const;
  juce::Rectangle<float> getPreviewButtonBounds(const Note &note) const;
  juce::Rectangle<float> getNoteHoverShadowBounds(const Note &note) const;
  juce::Rectangle<float> getResetButtonBounds(const Note &note) const;
  juce::Rectangle<float> getPreviewHoverBounds(const Note &note) const;
  juce::Rectangle<int> getPreviewButtonLocalBounds(const Note &note) const;
  juce::Rectangle<int> getResetButtonLocalBounds(const Note &note) const;
  void updatePreviewButtonBounds();
  void triggerPreviewForNote(Note &note);
  void resetNoteEdits(Note &note);
  void setHoveredNote(Note *note);
  void updateScrollBars();
  bool nudgeSelectedNotesBySemitones(int semitoneDelta);
  void reapplyBasePitchForNote(
      Note *note); // Recalculate F0 from base pitch + delta after undo/redo

  Project *project = nullptr;
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
  std::unique_ptr<PitchCurveRenderer> pitchCurveRenderer;
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
  bool &timelineSnapCycle = viewState.timelineSnapCycle;
  ScaleMode &selectedScaleMode = viewState.selectedScaleMode;
  int &selectedScaleRootNote = viewState.selectedScaleRootNote;
  std::optional<int> &previewScaleRootNote = viewState.previewScaleRootNote;
  std::optional<ScaleMode> &previewScaleMode = viewState.previewScaleMode;

  // Interaction handlers (state machine pattern)
  std::unique_ptr<LoopDragHandler> loopDragHandler_;
  std::unique_ptr<SelectHandler> selectHandler_;
  std::unique_ptr<DrawHandler> drawHandler_;
  std::unique_ptr<SplitHandler> splitHandler_;
  InteractionHandler *currentHandler_ = nullptr;

  // Scrollbars
  juce::ScrollBar horizontalScrollBar{false};
  juce::ScrollBar verticalScrollBar{true};
  bool showHorizontalScrollBar = true;

public:
  void invalidateWaveformCache();
  void invalidateBasePitchCache();

private:
  // Mouse drag throttling
  juce::int64 lastDragRepaintTime = 0;
  bool modifierZoomDragActive = false;
  juce::Point<float> modifierZoomLastPosition;
  bool modifierPanDragActive = false;
  juce::Point<float> modifierPanLastPosition;
  bool pianoKeyAuditionMouseDown = false;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};
