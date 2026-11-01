#include "PianoRollWorkspaceView.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Constants.h"
#include "../Utils/Localization.h"
#include "BinaryData.h"
#include "Dialogs/QuantizePitchDialog.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/ScaleUtils.h"
#include "../Utils/PitchMacroScope.h"
#include "../Undo/UndoActions.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

void PianoRollWorkspaceView::FloatingZoomSliderLookAndFeel::drawLinearSlider(
    juce::Graphics &g,
    int x,
    int y,
    int width,
    int height,
    float sliderPos,
    float minSliderPos,
    float maxSliderPos,
    const juce::Slider::SliderStyle style,
    juce::Slider &slider)
{
  juce::ignoreUnused(minSliderPos, maxSliderPos, slider);

  const auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                             static_cast<float>(y),
                                             static_cast<float>(width),
                                             static_cast<float>(height));
  const auto thumbRadius = 4.5f;
  const auto trackHeight = 2.0f;

  if (style == juce::Slider::LinearVertical)
  {
    const auto track = juce::Rectangle<float>(
        bounds.getCentreX() - trackHeight * 0.5f,
        bounds.getY(),
        trackHeight,
        juce::jmax(0.0f, bounds.getHeight()));

    g.setColour(juce::Colour(0xFF494949));
    g.fillRoundedRectangle(track, trackHeight * 0.5f);

    const auto clampedSliderPos = juce::jlimit(track.getY() + thumbRadius,
                                               track.getBottom() - thumbRadius,
                                               sliderPos);
    g.setColour(juce::Colour(0xFF9B9B9B));
    g.fillEllipse(bounds.getCentreX() - thumbRadius,
                  clampedSliderPos - thumbRadius,
                  thumbRadius * 2.0f,
                  thumbRadius * 2.0f);
    return;
  }

  const auto track = juce::Rectangle<float>(bounds.getX(),
                                           bounds.getCentreY() - trackHeight * 0.5f,
                                           juce::jmax(0.0f, bounds.getWidth()),
                                           trackHeight);

  g.setColour(juce::Colour(0xFF494949));
  g.fillRoundedRectangle(track, trackHeight * 0.5f);

  const auto clampedSliderPos = juce::jlimit(track.getX() + thumbRadius,
                                             track.getRight() - thumbRadius,
                                             sliderPos);
  g.setColour(juce::Colour(0xFF9B9B9B));
  g.fillEllipse(clampedSliderPos - thumbRadius,
                bounds.getCentreY() - thumbRadius,
                thumbRadius * 2.0f,
                thumbRadius * 2.0f);
}

void PianoRollWorkspaceView::FloatingControlBackground::paint(juce::Graphics &g)
{
  g.setColour(juce::Colour(0xFF0D0B0B));
  g.fillRoundedRectangle(getLocalBounds().toFloat(), 5.0f);
}

PianoRollWorkspaceView::PianoRollWorkspaceView(PianoRollComponent &piano)
    : pianoRoll(piano)
{
  pianoCard.setPadding(0);
  pianoCard.setCornerRadius(0.0f);
  pianoCard.setBorderColour(juce::Colours::transparentBlack);
  pianoCard.setContentComponent(&pianoRoll);

  overviewCard.setPadding(0);
  overviewCard.setCornerRadius(0.0f);
  overviewCard.setBackgroundColour(juce::Colour(0xFF191717u));
  overviewCard.setBorderColour(juce::Colour(0xFF0D0B0Bu));
  overviewCard.setContentComponent(&overviewPanel);
  overviewPanel.setDrawBackground(false);

  // The thumbnail works in time relative to the canvas's timeline start
  // (0 in Clip mode, the song start in Track mode).
  overviewPanel.getViewState = [this]()
  {
    OverviewPanel::ViewState state;
    const double viewStart = pianoRoll.getTimelineStart();
    const float pps = pianoRoll.getPixelsPerSecond();
    state.totalTime = pianoRoll.getTimelineLength();
    state.cursorTime = pianoRoll.getCursorTime() - viewStart;
    state.scrollX = pianoRoll.getScrollX() - viewStart * pps;
    state.pixelsPerSecond = pps;
    state.visibleWidth = pianoRoll.getVisibleContentWidth();
    state.trackMode = pianoRoll.isTrackViewMode();
    state.viewStartSeconds = viewStart;
    state.displayOffset = pianoRoll.projectToTimeline(0.0);
    if (auto *project = pianoRoll.getProject())
    {
      const auto &ranges = project->getAudioData().playbackRegionRanges;
      if (!ranges.empty() && ranges.front().second > ranges.front().first)
      {
        state.hasActiveSpan = true;
        state.activeSpanStart = ranges.front().first;
        state.activeSpanEnd = ranges.front().second;
      }
    }
    return state;
  };
  overviewPanel.onScrollXChanged = [this](double x)
  {
    pianoRoll.setScrollX(x + pianoRoll.getTimelineStart() *
                                 pianoRoll.getPixelsPerSecond());
    if (pianoRoll.onScrollChanged)
      pianoRoll.onScrollChanged(pianoRoll.getScrollX());
  };
  overviewPanel.onZoomChanged = [this](float pps)
  {
    pianoRoll.setPixelsPerSecond(pps, false);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };
  zoomXSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  zoomXSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomXSlider.setLookAndFeel(&floatingZoomSliderLookAndFeel);
  zoomXSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 0.1);
  zoomXSlider.setValue(pianoRoll.getPixelsPerSecond(),
                       juce::dontSendNotification);
  zoomXSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSecond(static_cast<float>(zoomXSlider.getValue()),
                                 true);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };

  zoomYSlider.setSliderStyle(juce::Slider::LinearVertical);
  zoomYSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomYSlider.setLookAndFeel(&floatingZoomSliderLookAndFeel);
  zoomYSlider.setRange(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, 0.1);
  zoomYSlider.setValue(pianoRoll.getPixelsPerSemitone(),
                       juce::dontSendNotification);
  zoomYSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSemitone(static_cast<float>(zoomYSlider.getValue()),
                                   static_cast<float>(pianoRoll.getVisibleContentHeight()) *
                                       0.5f);
  };

  autoZoomButton.setImage(juce::ImageFileFormat::loadFrom(
      BinaryData::autozoom_png, static_cast<size_t>(BinaryData::autozoom_pngSize)));
  autoZoomButton.setTooltip(TR("tooltip.vertical_auto_zoom"));
  autoZoomButton.onClick = [this]()
  {
    if (onAutoZoomRequested)
      onAutoZoomRequested();
  };

  overviewToggleButton.setImage(juce::ImageFileFormat::loadFrom(
      BinaryData::thumbnail_png, static_cast<size_t>(BinaryData::thumbnail_pngSize)));
  overviewToggleButton.setClickingTogglesState(true);
  overviewToggleButton.setToggleState(overviewVisible,
                                      juce::dontSendNotification);
  overviewToggleButton.onClick = [this]()
  {
    overviewVisible = overviewToggleButton.getToggleState();
    updateOverviewVisibility();
    if (onOverviewVisibilityChanged)
      onOverviewVisibilityChanged(overviewVisible);
  };

  addAndMakeVisible(pianoCard);
  addAndMakeVisible(overviewCard);
  addAndMakeVisible(zoomXBackground);
  addAndMakeVisible(zoomYBackground);
  addAndMakeVisible(autoZoomButton);
  addAndMakeVisible(overviewToggleButton);
  addAndMakeVisible(zoomXSlider);
  addAndMakeVisible(zoomYSlider);

  overviewCard.setVisible(false);
  overviewPanel.setVisible(false);
  pianoRoll.setHorizontalScrollBarVisible(true);
  startTimerHz(30);
}

PianoRollWorkspaceView::~PianoRollWorkspaceView()
{
  zoomXSlider.setLookAndFeel(nullptr);
  zoomYSlider.setLookAndFeel(nullptr);
}

void PianoRollWorkspaceView::paint(juce::Graphics &g)
{
  juce::ignoreUnused(g);
}

void PianoRollWorkspaceView::resized()
{
  auto bounds = getLocalBounds();

  const float progress = juce::jlimit(0.0f, 1.0f, overviewAnimationProgress);
  if (progress > 0.001f)
  {
    const int animatedOverviewHeight =
        static_cast<int>(std::round(static_cast<float>(overviewHeight) * progress));
    const int animatedGap =
        static_cast<int>(std::round(static_cast<float>(cardGap) * progress));
    const auto fullBounds = bounds;

    bounds.removeFromBottom(animatedOverviewHeight + animatedGap);

    auto overviewBounds = juce::Rectangle<int>(
        fullBounds.getX(),
        fullBounds.getBottom() - animatedOverviewHeight,
        fullBounds.getWidth(), overviewHeight);
    overviewBounds.removeFromLeft(thumbnailOuterHorizontalPadding);
    overviewBounds.removeFromRight(thumbnailOuterHorizontalPadding);
    overviewBounds.removeFromBottom(thumbnailOuterBottomPadding);
    overviewCard.setBounds(overviewBounds);
  }
  else
  {
    overviewCard.setBounds({});
  }

  pianoCard.setBounds(bounds);

  auto overlay = pianoCard.getBounds();
  const int sliderBottom = overlay.getBottom() - floatingControlInset - 3;
  const int sliderRight = overlay.getRight() - floatingControlInset;
  const int zoomXHeight = floatingSliderThickness;
  const int zoomXTop = sliderBottom - zoomXHeight;
  const int zoomYLeft = sliderRight - zoomSliderWidth - 5;
  const int controlColumnCentreX = zoomYLeft + zoomSliderWidth / 2;
  const int thumbnailButtonX = controlColumnCentreX - thumbnailButtonSize / 2;

  auto zoomXRect = juce::Rectangle<int>(
      thumbnailButtonX - overlayControlGap - zoomSliderLength, zoomXTop,
      zoomSliderLength, zoomXHeight);

  overviewToggleButton.setBounds(
      thumbnailButtonX,
      zoomXRect.getY() + (zoomXHeight - thumbnailButtonSize) / 2,
      thumbnailButtonSize, thumbnailButtonSize);

  autoZoomButton.setBounds(
      controlColumnCentreX - autoZoomButtonSize / 2,
      overviewToggleButton.getY() - autoZoomButtonSize - overlayVerticalGap,
      autoZoomButtonSize, autoZoomButtonSize);

  auto zoomYRect = juce::Rectangle<int>(
      zoomYLeft,
      autoZoomButton.getY() - overlayVerticalGap - zoomSliderHeight,
      zoomSliderWidth, zoomSliderHeight);

  zoomXSlider.setBounds(zoomXRect);
  zoomYSlider.setBounds(zoomYRect);

  zoomXBg = zoomXRect.toFloat();
  zoomYBg = zoomYRect.toFloat();
  toggleBg = overviewToggleButton.getBounds().toFloat();

  zoomXBackground.setBounds(zoomXBg.toNearestInt());
  zoomYBackground.setBounds(zoomYBg.toNearestInt());
}

void PianoRollWorkspaceView::setTrackViewModeAvailable(bool available)
{
  trackViewModeAvailable = available;
  if (!available && pianoRoll.isTrackViewMode())
    setTrackViewMode(false);
}

void PianoRollWorkspaceView::setTrackViewMode(bool track)
{
  track = track && trackViewModeAvailable;
  if (pianoRoll.isTrackViewMode() == track)
    return;
  pianoRoll.setTrackViewMode(track);
  overviewPanel.invalidateThumbnailCache();
}

void PianoRollWorkspaceView::setProject(Project *project)
{
  dismissPitchCenterPopup();
  overviewPanel.setProject(project);
  if (project != nullptr)
    setOverviewVisible(project->thumbnailVisible);
}

void PianoRollWorkspaceView::dismissPitchCenterPopup()
{
  QuantizePitchDialog::dismissPopup();
}

void PianoRollWorkspaceView::showPitchCenterPopup()
{
  dismissPitchCenterPopup();
  auto* activeProject = pianoRoll.getProject();
  if (!activeProject) return;
  const bool track = pianoRoll.isTrackViewMode() && bool(getTrackPitchMacroTargets);
  auto targets = track ? getTrackPitchMacroTargets()
                       : std::vector<TrackPitchMacroTarget>{{activeProject, {}, {}, {}}};
  if (targets.empty()) return;

  struct NoteCenter {
    Note* note;
    float midiNote, sourceMidiNote, pitchDrift, strength;
    float finalMidi = 0.0f, finalDrift = 1.0f, finalStrength = 0.0f;
    float renderedMidi = midiNote, renderedDrift = pitchDrift;
  };
  struct State {
    TrackPitchMacroTarget target;
    float originalPitchCenter;
    std::vector<NoteCenter> notes;
    float finalPitchCenter = 0.0f;
  };
  auto states = std::make_shared<std::vector<State>>();
  for (auto& target : targets) {
    State state{target, target.project->getPitchCenter(), {}};
    for (auto* note : collectPitchMacroNotes(*target.project, track, target.frames))
      state.notes.push_back({note, note->getMidiNote(), note->getLastNonMacroMidiNote(),
                             note->getPitchDrift(), note->getMacroPitchCenter()});
    states->push_back(std::move(state));
  }
  auto amount = std::make_shared<float>(activeProject->getPitchCenter());
  auto previewEdited = std::make_shared<bool>(false);
  auto centerWasEdited = std::make_shared<bool>(false);
  const bool initialSnapToScale = pitchCenterSnapToScale;
  auto safeThis = juce::Component::SafePointer<PianoRollWorkspaceView>(this);
  const auto refreshCurves = [safeThis, states] {
    if (!safeThis) return;
    safeThis->pianoRoll.endMacroCurvePreview();
    for (auto& state : *states) {
      if (state.target.isValid && !state.target.isValid()) continue;
      PitchCurveProcessor::rebuildBaseFromNotes(*state.target.project);
      if (state.target.onChanged) state.target.onChanged(false);
    }
    safeThis->pianoRoll.invalidateBasePitchCache();
    safeThis->pianoRoll.repaint();
    safeThis->refreshOverview();
  };
  // Track the last requested sound, rather than comparing with popup-open
  // values: a later drag can return to the original and still need a render.
  const auto renderPreview = [safeThis, states] {
    if (!safeThis) return;
    for (auto& state : *states) {
      if (state.target.isValid && !state.target.isValid()) continue;
      int first = std::numeric_limits<int>::max(), last = -1;
      for (auto& center : state.notes) {
        const float midi = center.note->getMidiNote(), drift = center.note->getPitchDrift();
        if (std::abs(midi - center.renderedMidi) <= 0.0001f &&
            std::abs(drift - center.renderedDrift) <= 0.0001f) continue;
        center.renderedMidi = midi;
        center.renderedDrift = drift;
        center.note->markDirty();
        center.note->markSynthDirty();
        first = std::min(first, center.note->getStartFrame());
        last = std::max(last, center.note->getEndFrame());
      }
      if (last < first) continue;
      state.target.project->markNoteEditDirtyRange(first, last);
      if (state.target.project == safeThis->pianoRoll.getProject()) {
        if (safeThis->pianoRoll.onPitchEdited) safeThis->pianoRoll.onPitchEdited();
        if (safeThis->pianoRoll.onPitchPreviewRenderRequested)
          safeThis->pianoRoll.onPitchPreviewRenderRequested();
      } else if (state.target.onChanged) state.target.onChanged(true);
    }
  };
  const auto apply = [safeThis, states](bool redo) {
    if (!safeThis) return;
    safeThis->pianoRoll.endMacroCurvePreview();
    bool activeChanged = false;
    for (auto& state : *states) {
      if (state.target.isValid && !state.target.isValid()) continue;
      auto* project = state.target.project;
      int first = std::numeric_limits<int>::max(), last = -1;
      for (const auto& center : state.notes) {
        center.note->setMacroPitchCenter(redo ? center.finalStrength : center.strength);
        if (std::abs(center.finalMidi - center.midiNote) <= 0.0001f &&
            std::abs(center.finalDrift - center.pitchDrift) <= 0.0001f) continue;
        center.note->setMidiNoteFromPitchCorrection(redo ? center.finalMidi : center.midiNote);
        center.note->setPitchDrift(redo ? center.finalDrift : center.pitchDrift);
        center.note->markDirty();
        center.note->markSynthDirty();
        first = std::min(first, center.note->getStartFrame());
        last = std::max(last, center.note->getEndFrame());
      }
      project->setPitchCenter(redo ? state.finalPitchCenter : state.originalPitchCenter);
      project->setModified(true);
      if (last >= first) {
        PitchCurveProcessor::rebuildBaseFromNotes(*project);
        project->markNoteEditDirtyRange(first, last);
      }
      if (project == safeThis->pianoRoll.getProject()) activeChanged = true;
      else if (state.target.onChanged) state.target.onChanged(true);
    }
    safeThis->pianoRoll.invalidateBasePitchCache();
    safeThis->pianoRoll.repaint();
    safeThis->refreshOverview();
    if (activeChanged && safeThis->pianoRoll.onPitchEdited)
      safeThis->pianoRoll.onPitchEdited();
    if (activeChanged && safeThis->pianoRoll.onPitchEditFinished)
      safeThis->pianoRoll.onPitchEditFinished();
  };
  class MacroAction final : public UndoableAction {
  public:
    MacroAction(std::function<void(bool)> callback, bool trackWide)
        : apply(std::move(callback)), trackWide(trackWide) {}
    void undo() override { apply(false); }
    void redo() override { apply(true); }
    juce::String getName() const override { return "Pitch Macro"; }
    bool requiresAudioResynthesis() const override { return false; }
    bool ownsProjectContext() const override { return trackWide; }
  private:
    std::function<void(bool)> apply;
    bool trackWide;
  };
  // Like Melodyne, show the targets' own values, as a range when they differ,
  // with the thumb at their average.
  float centerLow = 100.0f, centerHigh = 0.0f, driftLow = 100.0f, driftHigh = 0.0f;
  float centerSum = 0.0f, driftSum = 0.0f;
  int noteCount = 0;
  for (const auto& state : *states)
    for (const auto& center : state.notes) {
      const float strength = std::round(center.strength);
      const float drift = std::round(
          juce::jlimit(0.0f, 100.0f, (1.0f - center.pitchDrift) * 100.0f));
      centerLow = std::min(centerLow, strength);
      centerHigh = std::max(centerHigh, strength);
      driftLow = std::min(driftLow, drift);
      driftHigh = std::max(driftHigh, drift);
      centerSum += strength;
      driftSum += drift;
      ++noteCount;
    }
  const float centerMean = noteCount > 0 ? std::round(centerSum / noteCount) : 0.0f;
  const float driftMean = noteCount > 0 ? std::round(driftSum / noteCount) : 0.0f;
  if (centerHigh < centerLow) centerLow = centerHigh = 0.0f;
  if (driftHigh < driftLow) driftLow = driftHigh = 0.0f;
  const auto anchor = juce::Rectangle<int>(pianoCard.getRight(), pianoCard.getY(), 0, 0);
  QuantizePitchDialog::showPopup(this, anchor, {centerLow, centerHigh}, centerMean,
      {driftLow, driftHigh}, driftMean,
      pitchCenterSnapToScale,
      [this, states, targets, amount, previewEdited, centerWasEdited, initialSnapToScale](
          float value, bool centerEdited, bool snapToScale, float drift, bool driftEdited) {
        *previewEdited = true;
        if (centerEdited) {
          *centerWasEdited = true;
          *amount = value;
        }
        pitchCenterSnapToScale = snapToScale;
        // Until Pitch Center or the snap mode changes, each note keeps its own
        // correction, so a drift-only edit never re-corrects pitch.
        const bool recomputePitch = centerEdited || snapToScale != initialSnapToScale;
        std::vector<Note*> previewNotes;
        for (const auto& state : *states)
          if (state.target.project == pianoRoll.getProject())
            for (const auto& center : state.notes) previewNotes.push_back(center.note);
        pianoRoll.beginMacroCurvePreview(previewNotes);
        pianoRoll.beginInactiveMacroPreview(targets);
        for (auto& state : *states) {
          if (state.target.isValid && !state.target.isValid()) continue;
          const auto selectedScaleMode = state.target.project->getScaleMode();
          const auto correctionScaleMode =
              (selectedScaleMode == ScaleMode::None ||
               selectedScaleMode == ScaleMode::Chromatic)
                  ? state.target.project->getPreferredScaleMode()
                  : selectedScaleMode;
          // Pitch Center is a correction strength: 0% allows the maximum
          // possible deviation (0.5 semitones), while 100% allows none. Notes
          // already inside the remaining allowance are untouched; larger ones
          // are brought down to it while retaining their direction.
          for (const auto &center : state.notes)
          {
            if (center.note == nullptr) continue;
            if (driftEdited)
              center.note->setPitchDrift(1.0f - drift / 100.0f);
            if (!recomputePitch)
            {
              center.note->setMidiNoteFromPitchCorrection(center.midiNote);
              center.note->setMacroPitchCenter(center.strength);
              continue;
            }
            const float amount = centerEdited ? value : center.strength;
            const float maximumDeviation = (100.0f - amount) * 0.005f;
            center.note->setMacroPitchCenter(amount);
            // Calculate from the latest regular edit, never from an earlier
            // macro result. This lets a lower amount restore that edit without
            // compounding correction or discarding edits made between passes.
            const float sourceMidi = center.sourceMidiNote;
            const float corrected = snapToScale
                ? ScaleUtils::snapMidiToScale(sourceMidi, correctionScaleMode,
                                               state.target.project->getScaleRootNote(),
                                               state.target.project->getPitchReferenceHz())
                : std::round(sourceMidi);
            const float signedDeviation = sourceMidi - corrected;
            if (snapToScale)
            {
              // Scale mode interpolates every note toward its closest scale tone:
              // 100% reaches it, while lower values retain that fraction of the
              // note's original scale-distance.
              center.note->setMidiNoteFromPitchCorrection(
                  corrected + signedDeviation *
                  ((100.0f - amount) / 100.0f));
            }
            else if (std::abs(signedDeviation) > maximumDeviation)
              center.note->setMidiNoteFromPitchCorrection(
                  corrected + std::copysign(maximumDeviation, signedDeviation));
            else
              center.note->setMidiNoteFromPitchCorrection(sourceMidi);
          }
        }
        pianoRoll.repaint();
      },
      [safeThis, states, amount, previewEdited, centerWasEdited, refreshCurves, renderPreview,
       apply, track](bool accepted) {
        if (!safeThis) return;
        if (accepted && !*previewEdited) return;
        safeThis->pianoRoll.endMacroCurvePreview();
        bool changed = false;
        for (auto& state : *states) {
          if (state.target.isValid && !state.target.isValid()) continue;
          state.finalPitchCenter = *centerWasEdited ? *amount : state.originalPitchCenter;
          changed |= std::abs(state.finalPitchCenter - state.originalPitchCenter) > 0.0001f;
          for (auto& center : state.notes) {
            center.finalMidi = center.note->getMidiNote();
            center.finalDrift = center.note->getPitchDrift();
            center.finalStrength = center.note->getMacroPitchCenter();
            changed |= std::abs(center.finalMidi - center.midiNote) > 0.0001f ||
                       std::abs(center.finalDrift - center.pitchDrift) > 0.0001f ||
                       std::abs(center.finalStrength - center.strength) > 0.0001f;
            if (!accepted) {
              center.note->setMidiNoteFromPitchCorrection(center.midiNote);
              center.note->setPitchDrift(center.pitchDrift);
              center.note->setMacroPitchCenter(center.strength);
            }
          }
        }
        if (!accepted) {
          refreshCurves();
          renderPreview(); // Replace any auditioned audio with the original.
          return;
        }
        if (!changed) return;
        // The release callback already requested the final audio. Commit only
        // settings/history here, without re-dirtying notes or rendering again.
        for (auto& state : *states) {
          if (state.target.isValid && !state.target.isValid()) continue;
          state.target.project->setPitchCenter(state.finalPitchCenter);
          state.target.project->setModified(true);
          if (state.target.onCommit) state.target.onCommit();
        }
        if (safeThis->pianoRoll.onPitchEditCommitted)
          safeThis->pianoRoll.onPitchEditCommitted();
        if (auto* undo = safeThis->pianoRoll.getUndoManager())
          undo->addAction(std::make_unique<MacroAction>(apply, track));
      }, [refreshCurves, renderPreview] {
        refreshCurves();
        renderPreview();
      });
}

void PianoRollWorkspaceView::setRegionPreviews(
    MainViewRegionPreviewList previews)
{
  // The overview shows this track only; pinned tracks are canvas-only.
  overviewPanel.setRegionPreviews(withoutPinnedRegions(previews));
}

void PianoRollWorkspaceView::refreshOverview()
{
  if (overviewVisible)
    overviewPanel.invalidateThumbnailCache();
}

void PianoRollWorkspaceView::repaintOverviewViewport()
{
  if (overviewVisible)
    overviewPanel.repaint();
}

void PianoRollWorkspaceView::setShowSegmentsDebug(bool show)
{
  overviewPanel.setShowSegmentsDebug(show);
}

void PianoRollWorkspaceView::setOverviewVisible(bool visible)
{
  overviewVisible = visible;
  overviewToggleButton.setToggleState(visible, juce::dontSendNotification);
  overviewAnimationActive = false;
  overviewAnimationProgress = visible ? 1.0f : 0.0f;
  overviewCard.setVisible(visible);
  overviewPanel.setVisible(visible);
  pianoRoll.setHorizontalScrollBarVisible(!visible);
  resized();
  repaint();
}

void PianoRollWorkspaceView::updateOverviewVisibility()
{
  overviewAnimationStartProgress = overviewAnimationProgress;
  overviewAnimationStartMs = juce::Time::getMillisecondCounter();
  overviewAnimationActive = true;

  if (overviewVisible)
  {
    overviewCard.setVisible(true);
    overviewPanel.setVisible(true);
    pianoRoll.setHorizontalScrollBarVisible(false);
  }

  resized();
  repaint();
}

void PianoRollWorkspaceView::updateOverviewAnimation()
{
  if (!overviewAnimationActive)
    return;

  const float target = overviewVisible ? 1.0f : 0.0f;
  const auto now = juce::Time::getMillisecondCounter();
  const float elapsed =
      static_cast<float>(now - overviewAnimationStartMs) /
      static_cast<float>(overviewAnimationMs);
  const float t = juce::jlimit(0.0f, 1.0f, elapsed);
  const float eased = 1.0f - std::pow(1.0f - t, 3.0f);

  overviewAnimationProgress =
      overviewAnimationStartProgress +
      (target - overviewAnimationStartProgress) * eased;

  if (t >= 1.0f)
  {
    overviewAnimationProgress = target;
    overviewAnimationActive = false;

    if (!overviewVisible)
    {
      overviewCard.setVisible(false);
      overviewPanel.setVisible(false);
      pianoRoll.setHorizontalScrollBarVisible(true);
    }
  }

  resized();
  repaint();
}

void PianoRollWorkspaceView::timerCallback()
{
  updateOverviewAnimation();

  const float pps = pianoRoll.getPixelsPerSecond();
  if (std::abs(zoomXSlider.getValue() - pps) > 0.05)
    zoomXSlider.setValue(pps, juce::dontSendNotification);

  const float ppsY = pianoRoll.getPixelsPerSemitone();
  if (std::abs(zoomYSlider.getValue() - ppsY) > 0.05)
    zoomYSlider.setValue(ppsY, juce::dontSendNotification);

  // Relative to the timeline start, like the thumbnail's view state.
  const double cursorTime =
      pianoRoll.getCursorTime() - pianoRoll.getTimelineStart();
  if (overviewVisible && std::abs(lastOverviewCursorTime - cursorTime) > 0.0001)
  {
    const double previousCursorTime = lastOverviewCursorTime;
    lastOverviewCursorTime = cursorTime;
    overviewPanel.repaintPlayhead(previousCursorTime, cursorTime);
  }
}

void PianoRollWorkspaceView::refreshLocalisedText()
{
  autoZoomButton.setTooltip(TR("tooltip.vertical_auto_zoom"));
}
