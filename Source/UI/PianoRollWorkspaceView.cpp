#include "PianoRollWorkspaceView.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Constants.h"
#include "BinaryData.h"
#include "Dialogs/QuantizePitchDialog.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/ScaleUtils.h"
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

  overviewPanel.getViewState = [this]()
  {
    OverviewPanel::ViewState state;
    state.totalTime = pianoRoll.getTimelineDuration();
    state.cursorTime = pianoRoll.getCursorTime();
    state.scrollX = pianoRoll.getScrollX();
    state.pixelsPerSecond = pianoRoll.getPixelsPerSecond();
    state.visibleWidth = pianoRoll.getVisibleContentWidth();
    return state;
  };
  overviewPanel.onScrollXChanged = [this](double x)
  {
    pianoRoll.setScrollX(x);
    if (pianoRoll.onScrollChanged)
      pianoRoll.onScrollChanged(x);
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
  autoZoomButton.setTooltip("Vertical Auto-Zoom");
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

void PianoRollWorkspaceView::setProject(Project *project)
{
  dismissPitchCenterPopup();
  overviewPanel.setProject(project);
}

void PianoRollWorkspaceView::dismissPitchCenterPopup()
{
  QuantizePitchDialog::dismissPopup();
}

void PianoRollWorkspaceView::showPitchCenterPopup()
{
  auto *project = pianoRoll.getProject();
  if (project == nullptr)
    return;

  struct NoteCenter
  {
    Note *note;
    float midiNote;
    float originalMidiNote;
  };
  auto centers = std::make_shared<std::vector<NoteCenter>>();
  centers->reserve(project->getNotes().size());
  const bool hasSelectedNotes = std::any_of(
      project->getNotes().begin(), project->getNotes().end(),
      [](const Note &note) { return !note.isRest() && note.isSelected(); });
  for (auto &note : project->getNotes())
    if (!note.isRest() && (!hasSelectedNotes || note.isSelected()))
      centers->push_back({ &note, note.getMidiNote(), note.getOriginalMidiNote() });

  const float originalPitchCenter = project->getPitchCenter();
  auto previewPitchCenter = std::make_shared<float>(originalPitchCenter);

  const auto popupAnchor = juce::Rectangle<int>(pianoCard.getRight(), pianoCard.getY(), 0, 0);
  QuantizePitchDialog::showPopup(this, popupAnchor, originalPitchCenter,
      [this, project, centers, originalPitchCenter, previewPitchCenter](float amount,
                                                                          bool snapToScale)
      {
        *previewPitchCenter = amount;
        const auto selectedScaleMode = project->getScaleMode();
        const auto correctionScaleMode =
            (selectedScaleMode == ScaleMode::None ||
             selectedScaleMode == ScaleMode::Chromatic)
                ? project->getPreferredScaleMode()
                : selectedScaleMode;
        // Pitch Center is a correction strength: 0% allows the maximum
        // possible deviation (0.5 semitones), while 100% allows none. Notes
        // already inside the remaining allowance are untouched; larger ones
        // are brought down to it while retaining their direction.
        const float maximumDeviation = (100.0f - amount) * 0.005f;
        for (const auto &center : *centers)
        {
          if (center.note == nullptr) continue;
          // Always calculate from the analyzed pitch, rather than the current
          // corrected position. This allows a later lower correction amount to
          // restore a note's original deviation.
          const float sourceMidi = originalPitchCenter > 0.0001f
              ? center.originalMidiNote
              : center.midiNote;
          const float corrected = snapToScale
              ? ScaleUtils::snapMidiToScale(sourceMidi, correctionScaleMode,
                                             project->getScaleRootNote(),
                                             project->getPitchReferenceHz())
              : std::round(sourceMidi);
          const float signedDeviation = sourceMidi - corrected;
          if (snapToScale)
          {
            // Scale mode interpolates every note toward its closest scale tone:
            // 100% reaches it, while lower values retain that fraction of the
            // note's original scale-distance.
            center.note->setMidiNote(corrected + signedDeviation *
                ((100.0f - amount) / 100.0f));
          }
          else if (std::abs(signedDeviation) > maximumDeviation)
            center.note->setMidiNote(corrected + std::copysign(maximumDeviation,
                                                               signedDeviation));
          else
            center.note->setMidiNote(sourceMidi);
        }
        pianoRoll.repaint();
        refreshOverview();
      },
      [this, project, centers, originalPitchCenter, previewPitchCenter](bool accepted)
      {
        bool changed = false;
        int dirtyStart = std::numeric_limits<int>::max();
        int dirtyEnd = std::numeric_limits<int>::min();
        std::vector<Note *> changedNotes;
        std::vector<float> oldMidis;
        std::vector<float> newMidis;
        for (const auto &center : *centers)
        {
          if (center.note == nullptr) continue;
          if (accepted && std::abs(center.note->getMidiNote() - center.midiNote) > 0.0001f)
          {
            center.note->markDirty();
            center.note->markSynthDirty();
            dirtyStart = std::min(dirtyStart, center.note->getStartFrame());
            dirtyEnd = std::max(dirtyEnd, center.note->getEndFrame());
            changedNotes.push_back(center.note);
            oldMidis.push_back(center.midiNote);
            newMidis.push_back(center.note->getMidiNote());
            changed = true;
          }
          else
            center.note->setMidiNote(center.midiNote);
        }
        const bool pitchCenterChanged = accepted &&
            std::abs(*previewPitchCenter - originalPitchCenter) > 0.0001f;
        if (accepted)
          project->setPitchCenter(*previewPitchCenter);
        if (changed)
        {
          PitchCurveProcessor::rebuildBaseFromNotes(*project);
          pianoRoll.invalidateBasePitchCache();
          const int frameCount = static_cast<int>(project->getAudioData().f0.size());
          project->setF0DirtyRange(std::max(0, dirtyStart - 60),
                                   std::min(frameCount, dirtyEnd + 60));
          project->setModified(true);
        }

        if (changed || pitchCenterChanged)
        {
          if (auto *undoManager = pianoRoll.getUndoManager())
          {
            auto *projectPtr = project;
            undoManager->addAction(std::make_unique<PitchCenterCorrectionAction>(
                changedNotes, oldMidis, newMidis, originalPitchCenter,
                *previewPitchCenter,
                [this, projectPtr, dirtyStart, dirtyEnd](float pitchCenter,
                                                          const std::vector<Note *> &notes)
                {
                  if (projectPtr == nullptr)
                    return;
                  projectPtr->setPitchCenter(pitchCenter);
                  if (!notes.empty())
                  {
                    PitchCurveProcessor::rebuildBaseFromNotes(*projectPtr);
                    pianoRoll.invalidateBasePitchCache();
                    const int f0Size = static_cast<int>(projectPtr->getAudioData().f0.size());
                    projectPtr->setF0DirtyRange(std::max(0, dirtyStart - 60),
                                                 std::min(f0Size, dirtyEnd + 60));
                  }
                  projectPtr->setModified(true);
                  if (pianoRoll.onPitchEdited)
                    pianoRoll.onPitchEdited();
                  if (!notes.empty() && pianoRoll.onPitchEditFinished)
                    pianoRoll.onPitchEditFinished();
                  pianoRoll.repaint();
                  refreshOverview();
                }));
          }
        }
        pianoRoll.repaint();
        refreshOverview();
        if ((changed || pitchCenterChanged) && pianoRoll.onPitchEdited)
          pianoRoll.onPitchEdited();
        if (changed && pianoRoll.onPitchEditFinished)
          pianoRoll.onPitchEditFinished();
      });
}

void PianoRollWorkspaceView::refreshOverview()
{
  if (overviewVisible)
    overviewPanel.invalidateThumbnailCache();
}

void PianoRollWorkspaceView::setShowSegmentsDebug(bool show)
{
  overviewPanel.setShowSegmentsDebug(show);
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

  const double cursorTime = pianoRoll.getCursorTime();
  if (overviewVisible && std::abs(lastOverviewCursorTime - cursorTime) > 0.0001)
  {
    const double previousCursorTime = lastOverviewCursorTime;
    lastOverviewCursorTime = cursorTime;
    overviewPanel.repaintPlayhead(previousCursorTime, cursorTime);
  }
}
