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
                           public juce::KeyListener
{
  friend class PianoRollInteractionContext;

public:
  using juce::Component::keyPressed;

  static constexpr int pianoKeysWidth = 60;
  static constexpr int timelineHeight = 24;
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
  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override;
  void mouseMagnify(const juce::MouseEvent &e, float scaleFactor) override;

  // Focus handling - re-grab focus when lost (important for plugin mode)
  void focusLost(FocusChangeType cause) override;
  void focusGained(FocusChangeType cause) override;

  // KeyListener
  bool keyPressed(const juce::KeyPress &key) override;
  bool keyPressed(const juce::KeyPress &key,
                  juce::Component *originatingComponent) override;

  // ScrollBar::Listener
  void scrollBarMoved(juce::ScrollBar *scrollBar,
                      double newRangeStart) override;

  // Project
  void setProject(Project *proj);
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
  void setShowScaleColors(bool enabled);
  void setSnapToSemitoneDrag(bool enabled);
  void setPitchReferenceHz(int hz);
  void setDoubleClickSnapMode(DoubleClickSnapMode mode);
  void setTimelineDisplayMode(TimelineDisplayMode mode);
  void setTimelineBeatSignature(int numerator, int denominator);
  void setTimelineTempoBpm(double bpm);
  void setTimelineGridDivision(TimelineGridDivision division);
  void setTimelineSnapCycle(bool enabled);

  // Scroll
  void setScrollX(double x);
  double getScrollX() const { return scrollX; }
  void centerOnPitchRange(float minMidi, float maxMidi);
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
  bool getShowDeltaPitch() const { return showDeltaPitch; }
  bool getShowBasePitch() const { return showBasePitch; }

  // Callbacks
  std::function<void(Note *)> onNoteSelected;
  std::function<void()> onPitchEdited;
  std::function<void()> onPitchEditFinished; // Called when dragging ends
  std::function<void()> onCursorMoved;
  std::function<void(double)> onSeek;
  std::function<void(float)> onZoomChanged;
  std::function<void(double)> onScrollChanged;
  std::function<void(const LoopRange &)> onLoopRangeChanged;
  std::function<void(int, int)>
      onReinterpolateUV; // Called to re-infer UV regions (startFrame, endFrame)

private:
  enum class NoteRenderPass
  {
    Body,
    Overlay
  };

  void drawBackgroundWaveform(juce::Graphics &g,
                              const juce::Rectangle<int> &visibleArea);
  void drawGrid(juce::Graphics &g);
  void drawTimeline(juce::Graphics &g);
  void drawLoopTimeline(juce::Graphics &g);
  void drawNotes(juce::Graphics &g, NoteRenderPass pass);
  void drawPitchCurves(juce::Graphics &g);
  void drawPianoKeys(juce::Graphics &g);
  void drawSelectionRect(juce::Graphics &g); // Box selection rectangle
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

  Note *findNoteAt(float x, float y);
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

  float &pixelsPerSecond = viewState.pixelsPerSecond;
  float &pixelsPerSemitone = viewState.pixelsPerSemitone;

  double &cursorTime = viewState.cursorTime;
  double &scrollX = viewState.scrollX;
  double &scrollY = viewState.scrollY;

  // Edit mode
  EditMode &editMode = viewState.editMode;

  // View settings
  bool &showDeltaPitch = viewState.showDeltaPitch;
  bool &showBasePitch = viewState.showBasePitch;
  bool &showSegmentsDebug = viewState.showSegmentsDebug;
  bool &showGameValuesDebug = viewState.showGameValuesDebug;
  bool &showUvInterpolationDebug = viewState.showUvInterpolationDebug;
  bool &showActualF0Debug = viewState.showActualF0Debug;
  bool &showScaleColors = viewState.showScaleColors;
  bool &snapToSemitoneDrag = viewState.snapToSemitoneDrag;
  int &pitchReferenceHz = viewState.pitchReferenceHz;
  DoubleClickSnapMode &doubleClickSnapMode = viewState.doubleClickSnapMode;
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

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};
