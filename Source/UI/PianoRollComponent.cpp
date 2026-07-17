#include "PianoRollComponent.h"
#include "../Utils/BasePitchCurve.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/TimecodeFont.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/ScaleUtils.h"
#include "PianoRoll/PianoRollViewHelpers.h"
#include "PianoRoll/VisualWaveformEnvelope.h"
#include "PianoRoll/States/LoopDragHandler.h"
#include "PianoRoll/States/SelectHandler.h"
#include "PianoRoll/States/DrawHandler.h"
#include "PianoRoll/States/SplitHandler.h"
#include "BinaryData.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace
{
  using pianoRollView::getScaleAccentColour;
  using pianoRollView::isBlackKey;
  using pianoRollView::ScaleToneState;
  using pianoRollView::getScaleToneState;
  using pianoRollView::isMultipleOf;

  int normalizeTimelineBeatDenominator(int denominator)
  {
    denominator = juce::jlimit(1, 32, denominator);
    int normalized = 1;
    while (normalized < denominator)
      normalized <<= 1;
    const int lower = normalized >> 1;
    if (lower >= 1 && (denominator - lower) < (normalized - denominator))
      normalized = lower;
    return juce::jlimit(1, 32, normalized);
  }

  double gridDivisionToQuarterNotes(TimelineGridDivision division)
  {
    switch (division)
    {
    case TimelineGridDivision::Whole:
      return 4.0;
    case TimelineGridDivision::Half:
      return 2.0;
    case TimelineGridDivision::Quarter:
      return 1.0;
    case TimelineGridDivision::Eighth:
      return 0.5;
    case TimelineGridDivision::Sixteenth:
      return 0.25;
    case TimelineGridDivision::ThirtySecond:
      return 0.125;
    }
    return 1.0;
  }
}

PianoRollComponent::PianoRollComponent()
{
  // Initialize modular components
  coordMapper = std::make_unique<CoordinateMapper>();
  pianoKeysRenderer = std::make_unique<PianoKeysRenderer>();
  gridRenderer = std::make_unique<GridRenderer>();
  timelineRenderer = std::make_unique<TimelineRenderer>();
  waveformBackgroundRenderer = std::make_unique<WaveformBackgroundRenderer>();
  noteRenderer = std::make_unique<NoteRenderer>();
  pitchCurveRenderer = std::make_unique<PitchCurveRenderer>();
  scrollZoomController = std::make_unique<ScrollZoomController>();
  pitchEditor = std::make_unique<PitchEditor>();
  boxSelector = std::make_unique<BoxSelector>();
  noteSplitter = std::make_unique<NoteSplitter>();
  pitchToolHandles = std::make_unique<PitchToolHandles>();
  pitchToolController = std::make_unique<PitchToolController>();
  interactionContext = std::make_unique<PianoRollInteractionContext>(*this);

  // Initialize interaction handlers
  loopDragHandler_ = std::make_unique<LoopDragHandler>(*this);
  selectHandler_ = std::make_unique<SelectHandler>(*this);
  drawHandler_ = std::make_unique<DrawHandler>(*this);
  splitHandler_ = std::make_unique<SplitHandler>(*this);
  currentHandler_ = selectHandler_.get();

  // Wire up components
  pianoKeysRenderer->setCoordinateMapper(coordMapper.get());
  gridRenderer->setCoordinateMapper(coordMapper.get());
  timelineRenderer->setCoordinateMapper(coordMapper.get());
  waveformBackgroundRenderer->setCoordinateMapper(coordMapper.get());
  noteRenderer->setCoordinateMapper(coordMapper.get());
  noteRenderer->setSelectHandler(selectHandler_.get());
  noteRenderer->setSplitHandler(splitHandler_.get());
  noteRenderer->setPitchEditor(pitchEditor.get());
  noteRenderer->setBoxSelector(boxSelector.get());
  pitchCurveRenderer->setCoordinateMapper(coordMapper.get());
  pitchCurveRenderer->setSelectHandler(selectHandler_.get());
  pitchCurveRenderer->setPitchEditor(pitchEditor.get());
  scrollZoomController->setCoordinateMapper(coordMapper.get());
  pitchEditor->setCoordinateMapper(coordMapper.get());
  noteSplitter->setCoordinateMapper(coordMapper.get());

  // Setup scrollZoomController callbacks
  scrollZoomController->onRepaintNeeded = [this]()
  { repaint(); };
  scrollZoomController->onZoomChanged = [this](float pps)
  {
    if (onZoomChanged)
      onZoomChanged(pps);
  };
  scrollZoomController->onScrollChanged = [this](double x)
  {
    if (onScrollChanged)
      onScrollChanged(x);
  };

  // Setup pitchEditor callbacks
  pitchEditor->onNoteSelected = [this](Note *note)
  {
    if (onNoteSelected)
      onNoteSelected(note);
  };
  pitchEditor->onPitchEdited = [this]()
  {
    repaint();
    if (onPitchEdited)
      onPitchEdited();
  };
  pitchEditor->onPitchEditFinished = [this]()
  {
    if (onPitchEditFinished)
      onPitchEditFinished();
  };
  pitchEditor->onBasePitchCacheInvalidated = [this]()
  {
    invalidateBasePitchCache();
  };

  // Setup pitchToolController callbacks
  pitchToolController->onPitchEdited = [this]()
  {
    repaint();
    if (onPitchEdited)
      onPitchEdited();
  };

  // Setup noteSplitter callbacks
  noteSplitter->onNoteSplit = [this]()
  {
    invalidateBasePitchCache();
    repaint();
  };

  addAndMakeVisible(horizontalScrollBar);
  addAndMakeVisible(verticalScrollBar);

  // Use scrollZoomController's scrollbars
  addAndMakeVisible(scrollZoomController->getHorizontalScrollBar());
  addAndMakeVisible(scrollZoomController->getVerticalScrollBar());

  horizontalScrollBar.addListener(this);
  verticalScrollBar.addListener(this);

  // Style scrollbars to match theme
  auto thumbColor = juce::Colour(0xFF565656u);
  auto trackColor = juce::Colour(0xFF0D0B0Bu);

  horizontalScrollBar.setColour(juce::ScrollBar::thumbColourId, thumbColor);
  horizontalScrollBar.setColour(juce::ScrollBar::trackColourId, trackColor);
  horizontalScrollBar.setColour(juce::ScrollBar::backgroundColourId, trackColor);
  verticalScrollBar.setColour(juce::ScrollBar::thumbColourId, thumbColor);
  verticalScrollBar.setColour(juce::ScrollBar::trackColourId, trackColor);
  verticalScrollBar.setColour(juce::ScrollBar::backgroundColourId, trackColor);

  // Set initial scroll range
  verticalScrollBar.setRangeLimits(0, (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) *
                                          pixelsPerSemitone);
  verticalScrollBar.setCurrentRange(0, 500);

  // Default view centered on C3-C4 (MIDI 48-60)
  centerOnPitchRange(48.0f, 60.0f);

  // Enable keyboard focus for shortcuts
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);

  // No extra controls here; overview lives outside the piano roll.
  int previewImageSize = 0;
  if (const auto *previewImageData =
          BinaryData::getNamedResource("preview_png", previewImageSize))
  {
    previewButton.setImage(
        juce::ImageFileFormat::loadFrom(previewImageData, previewImageSize));
    previewButtonWidth = std::max(1, previewButton.getWidth());
    previewButtonHeight = std::max(1, previewButton.getHeight());
  }
  previewButton.setVisible(false);
  previewButton.onClick = [this]()
  {
    if (hoveredNote)
      triggerPreviewForNote(*hoveredNote);
  };
  addAndMakeVisible(previewButton);
}

PianoRollComponent::~PianoRollComponent()
{
  horizontalScrollBar.removeListener(this);
  verticalScrollBar.removeListener(this);
}

int PianoRollComponent::getVisibleContentWidth() const
{
  constexpr int verticalScrollBarSize = 8;
  return std::max(0, getWidth() - pianoKeysWidth - verticalScrollBarSize);
}

int PianoRollComponent::getVisibleContentHeight() const
{
  constexpr int horizontalScrollBarSize = 8;
  return std::max(0, getHeight() - headerHeight -
                         (showHorizontalScrollBar ? horizontalScrollBarSize : 0));
}

void PianoRollComponent::setHorizontalScrollBarVisible(bool shouldShow)
{
  if (showHorizontalScrollBar == shouldShow)
    return;

  showHorizontalScrollBar = shouldShow;
  horizontalScrollBar.setVisible(showHorizontalScrollBar);
  resized();
  repaint();
}

void PianoRollComponent::paint(juce::Graphics &g)
{
  updatePitchToolHandlesFromSelection();

  // Background (solid to keep grid clean)
  g.fillAll(APP_COLOR_BACKGROUND);
  g.setColour(juce::Colour(0xFF232323u));
  g.fillRect(0, 0, getWidth(), headerHeight);

  const int horizontalScrollBarSize = showHorizontalScrollBar ? 8 : 0;
  constexpr int verticalScrollBarSize = 8;
  auto contentBounds = getLocalBounds();

  g.setColour(juce::Colour(0xFF0D0B0Bu));
  g.fillRect(getWidth() - verticalScrollBarSize, 0, verticalScrollBarSize,
             headerHeight);
  g.fillRect(getWidth() - verticalScrollBarSize, headerHeight,
             verticalScrollBarSize,
             getHeight() - headerHeight - horizontalScrollBarSize);
  if (showHorizontalScrollBar)
  {
    g.fillRect(0, getHeight() - horizontalScrollBarSize, pianoKeysWidth,
               horizontalScrollBarSize);
    g.fillRect(pianoKeysWidth, getHeight() - horizontalScrollBarSize,
               getWidth() - pianoKeysWidth - verticalScrollBarSize,
               horizontalScrollBarSize);
    g.fillRect(getWidth() - verticalScrollBarSize,
               getHeight() - horizontalScrollBarSize,
               verticalScrollBarSize, horizontalScrollBarSize);
  }

  // Create clipping region for main area (below timelines)
  auto mainArea = contentBounds
                      .withTrimmedLeft(pianoKeysWidth)
                      .withTrimmedTop(headerHeight)
                      .withTrimmedBottom(horizontalScrollBarSize)
                      .withTrimmedRight(verticalScrollBarSize);

  // Draw row backgrounds first, then waveform, then grid lines/content.
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    g.setOrigin(pianoKeysWidth - static_cast<int>(scrollX),
                headerHeight - static_cast<int>(scrollY));
    drawGrid(g, true, false);
  }

  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    drawBackgroundWaveform(g, mainArea);
  }

  // Draw scrolled content (grid lines, notes, pitch curves, handles)
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    g.setOrigin(pianoKeysWidth - static_cast<int>(scrollX),
                headerHeight - static_cast<int>(scrollY));

    drawGrid(g, false, true);
    drawAudioSourceRegionOverlay(g);
    drawLoopOverlay(g);
    drawGameChunksDebugOverlay(g);
    drawNotes(g, NoteRenderPass::Body);
    drawNotes(g, NoteRenderPass::HoverShadow);
    drawNotes(g, NoteRenderPass::HoveredBody);
    drawPitchCurves(g);
    drawNotes(g, NoteRenderPass::Overlay);
    drawGameValuesDebugOverlay(g);
    drawSelectionRect(g);

    // Draw pitch tool handles in world space (transform applied by g.setOrigin above)
    if (editMode == EditMode::Select && pitchToolHandles && !pitchToolHandles->isEmpty())
    {
      pitchToolHandles->draw(g);
    }
  }

  // Draw timeline (above grid, scrolls horizontally)
  drawTimeline(g);
  drawLoopTimeline(g);

  // Draw unified cursor line (spans from timeline through grid)
  {
    float x = static_cast<float>(pianoKeysWidth) + timeToX(cursorTime) -
              static_cast<float>(scrollX);
    float cursorTop = static_cast<float>(headerHeight);
    float cursorBottom =
        static_cast<float>(getHeight() -
                           horizontalScrollBarSize); // Exclude scrollbar

    // Only draw if cursor is in visible area
    if (x >= pianoKeysWidth && x < getWidth() - verticalScrollBarSize)
    {
      g.setColour(juce::Colour(0xFFC8C7C7u));
      g.fillRect(x - 0.5f, cursorTop, 1.0f, cursorBottom - cursorTop);
    }
  }

  // Draw piano keys
  drawPianoKeys(g);

  g.setColour(juce::Colour(0xFF0D0B0Bu));
  g.drawVerticalLine(pianoKeysWidth, 0.0f,
                     static_cast<float>(getHeight() - horizontalScrollBarSize));

  const auto canvasBorderBounds = getLocalBounds()
                                      .withTrimmedRight(verticalScrollBarSize)
                                      .withTrimmedBottom(horizontalScrollBarSize);
  g.setColour(juce::Colour(0xFF3C3C3Cu));
  g.drawRect(canvasBorderBounds, 1);
}

void PianoRollComponent::resized()
{
  auto bounds = getLocalBounds();
  const int horizontalScrollBarSize = showHorizontalScrollBar ? 8 : 0;
  constexpr int verticalScrollBarSize = 8;

  if (showHorizontalScrollBar)
  {
    horizontalScrollBar.setBounds(
        pianoKeysWidth, bounds.getHeight() - horizontalScrollBarSize,
        bounds.getWidth() - pianoKeysWidth - verticalScrollBarSize,
        horizontalScrollBarSize);
  }
  else
  {
    horizontalScrollBar.setBounds({});
  }

  verticalScrollBar.setBounds(
      bounds.getWidth() - verticalScrollBarSize, headerHeight,
      verticalScrollBarSize,
      bounds.getHeight() - horizontalScrollBarSize - headerHeight);

  updateScrollBars();
  updatePreviewButtonBounds();
}

void PianoRollComponent::drawBackgroundWaveform(
    juce::Graphics &g, const juce::Rectangle<int> &visibleArea)
{
  waveformBackgroundRenderer->draw(g, visibleArea);
}

void PianoRollComponent::invalidateWaveformCache()
{
  waveformBackgroundRenderer->invalidateCache();
}

void PianoRollComponent::invalidateBasePitchCache()
{
  pitchCurveRenderer->invalidateBasePitchCache();
}

void PianoRollComponent::drawGrid(juce::Graphics &g, bool drawRowBackgrounds,
                                  bool drawGridLines)
{
  GridRenderer::Params params;
  const bool scaleViewActive =
      selectedScaleMode != ScaleMode::None &&
      selectedScaleMode != ScaleMode::Chromatic;
  params.scaleMode = scaleViewActive
                         ? previewScaleMode.value_or(selectedScaleMode)
                         : selectedScaleMode;
  params.scaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  params.pitchAxisOffsetSemitones =
      ScaleUtils::getReferenceOffsetSemitones(pitchReferenceHz);
  params.timelineDisplayMode = timelineDisplayMode;
  params.gridSeconds = getTimelineGridSeconds();
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.timelineDuration = getTimelineDuration();
  params.componentWidth = getWidth();
  params.visibleContentWidth = getVisibleContentWidth();
  params.visibleContentHeight = getVisibleContentHeight();
  params.drawRowBackgrounds = drawRowBackgrounds;
  params.drawGridLines = drawGridLines;
  gridRenderer->draw(g, params);
}

void PianoRollComponent::drawAudioSourceRegionOverlay(juce::Graphics &g)
{
  if (!project)
    return;

  const auto &audioData = project->getAudioData();
  const double duration = static_cast<double>(audioData.getDuration());
  if (audioData.waveform.getNumSamples() <= 0 || duration <= 0.0)
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  auto drawRange = [&](double startSeconds, double endSeconds,
                       bool drawBoundaries) {
    startSeconds = std::max(0.0, startSeconds);
    endSeconds = std::max(startSeconds, endSeconds);
    if (endSeconds <= startSeconds)
      return;
    const float startX = timeToX(startSeconds);
    const float endX = timeToX(endSeconds);
    g.setColour(juce::Colours::white.withAlpha(0.04f));
    g.fillRect(startX, 0.0f, endX - startX, height);
    if (drawBoundaries) {
      g.setColour(juce::Colours::white.withAlpha(0.25f));
      g.fillRect(startX - 0.5f, 0.0f, 1.0f, height);
      g.fillRect(endX - 0.5f, 0.0f, 1.0f, height);
    }
  };

  if (!audioData.playbackRegionRanges.empty()) {
    for (const auto &[start, end] : audioData.playbackRegionRanges)
      drawRange(start, end, true);
  } else {
    // Standalone is one continuous source: shade all visible bars as its
    // region and do not show a false audio-end boundary in the canvas.
    const double visibleEndSeconds =
        xToTime(static_cast<float>(scrollX + getVisibleContentWidth()));
    drawRange(audioData.timelineOffsetSeconds,
              std::max(duration, visibleEndSeconds), false);
  }
}

void PianoRollComponent::drawLoopOverlay(juce::Graphics &g)
{
  if (!project)
    return;

  double loopStartSeconds = 0.0;
  double loopEndSeconds = 0.0;
  bool loopEnabled = false;
  if (loopDragHandler_ && loopDragHandler_->isDragging())
  {
    loopStartSeconds = loopDragHandler_->getDragStartSeconds();
    loopEndSeconds = loopDragHandler_->getDragEndSeconds();
    loopEnabled = true;
  }
  else
  {
    const auto &loopRange = project->getLoopRange();
    loopStartSeconds = loopRange.startSeconds;
    loopEndSeconds = loopRange.endSeconds;
    loopEnabled = loopRange.enabled;
  }

  if (loopStartSeconds > loopEndSeconds)
    std::swap(loopStartSeconds, loopEndSeconds);

  if (loopEndSeconds <= loopStartSeconds)
    return;

  if (!loopEnabled)
    return;

  const float startX = timeToX(loopStartSeconds);
  const float endX = timeToX(loopEndSeconds);

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  g.setColour(juce::Colours::white.withAlpha(0.04f));
  g.fillRect(startX, 0.0f, endX - startX, height);
}

void PianoRollComponent::drawGameChunksDebugOverlay(juce::Graphics &g)
{
  if (!showSegmentsDebug || !project)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.segmentChunkRanges.empty())
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  g.setColour(juce::Colours::orange.withAlpha(0.10f));
  for (const auto &range : audioData.segmentChunkRanges)
  {
    int startFrame = std::max(0, range.first);
    int endFrame = std::max(startFrame, range.second);
    if (endFrame <= startFrame)
      continue;

    const float x1 = framesToSeconds(startFrame) * pixelsPerSecond;
    const float x2 = framesToSeconds(endFrame) * pixelsPerSecond;
    g.fillRect(x1, 0.0f, std::max(1.0f, x2 - x1), height);
  }

  g.setColour(juce::Colours::orange.withAlpha(0.75f));
  for (const auto &range : audioData.segmentChunkRanges)
  {
    int startFrame = std::max(0, range.first);
    int endFrame = std::max(startFrame, range.second);
    if (endFrame <= startFrame)
      continue;

    const float x = framesToSeconds(startFrame) * pixelsPerSecond;
    g.drawVerticalLine(static_cast<int>(x), 0.0f, height);
  }
}

void PianoRollComponent::drawGameValuesDebugOverlay(juce::Graphics &g)
{
  if (!showGameValuesDebug || !project)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.segmentDebugChunks.empty())
    return;

  const int totalFrames = static_cast<int>(audioData.f0.size());
  if (totalFrames <= 0)
    return;

  const int visibleStartFrame = std::max(
      0, static_cast<int>(scrollX / pixelsPerSecond * audioData.sampleRate /
                          HOP_SIZE));
  const int visibleEndFrame = std::min(
      totalFrames, static_cast<int>((scrollX + getVisibleContentWidth()) /
                                    pixelsPerSecond * audioData.sampleRate /
                                    HOP_SIZE) +
                       1);
  if (visibleEndFrame <= visibleStartFrame)
    return;

  const float contentHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  g.setFont(juce::FontOptions(10.5f));
  const int maxChunks = 60;
  int chunksDrawn = 0;

  for (const auto &chunk : audioData.segmentDebugChunks)
  {
    const int startFrame = std::max(0, chunk.startFrame);
    const int endFrame = std::max(startFrame, chunk.endFrame);
    if (endFrame <= startFrame)
      continue;
    if (endFrame <= visibleStartFrame || startFrame >= visibleEndFrame)
      continue;

    int noteCount = 0;
    int restCount = 0;
    int eventLabelsInChunk = 0;

    const float x1 = framesToSeconds(startFrame) * pixelsPerSecond;
    const float x2 = framesToSeconds(endFrame) * pixelsPerSecond;
    const float width = x2 - x1;
    if (width < 8.0f)
      continue;

    const float chunkSeconds =
        static_cast<float>(endFrame - startFrame) * HOP_SIZE /
        static_cast<float>(audioData.sampleRate);

    // Chunk boundary and chunk-level debug label.
    g.setColour(juce::Colours::orange.withAlpha(0.78f));
    g.drawVerticalLine(static_cast<int>(x1), 0.0f, contentHeight);

    // Raw GAME event markers/labels inside this chunk.
    for (size_t i = 0; i < chunk.events.size(); ++i)
    {
      const auto &ev = chunk.events[i];
      if (ev.endFrame <= startFrame || ev.startFrame >= endFrame)
        continue;

      const int overlapStart = std::max(startFrame, ev.startFrame);
      const int overlapEnd = std::min(endFrame, ev.endFrame);
      if (overlapEnd <= overlapStart)
        continue;

      const float ex1 = framesToSeconds(overlapStart) * pixelsPerSecond;
      const float ex2 = framesToSeconds(overlapEnd) * pixelsPerSecond;
      const float ew = std::max(1.0f, ex2 - ex1);

      if (ev.isRest)
      {
        ++restCount;
        // Red: rest segments placed on nearby note lane (not at top).
        float anchorMidi = 60.0f;
        bool foundAnchor = false;
        for (int k = static_cast<int>(i) - 1; k >= 0; --k)
        {
          if (!chunk.events[static_cast<size_t>(k)].isRest)
          {
            anchorMidi = chunk.events[static_cast<size_t>(k)].midiNote;
            foundAnchor = true;
            break;
          }
        }
        if (!foundAnchor)
        {
          for (size_t k = i + 1; k < chunk.events.size(); ++k)
          {
            if (!chunk.events[k].isRest)
            {
              anchorMidi = chunk.events[k].midiNote;
              foundAnchor = true;
              break;
            }
          }
        }
        anchorMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                                  static_cast<float>(MAX_MIDI_NOTE),
                                  anchorMidi);
        const float yCenter =
            midiToY(anchorMidi) + pixelsPerSemitone * 0.5f;
        const float restBandHeight = std::max(6.0f, pixelsPerSemitone * 0.62f);
        const float restBandTop = yCenter - restBandHeight * 0.5f;
        g.setColour(juce::Colours::red.withAlpha(0.55f));
        g.fillRect(ex1, restBandTop, ew, restBandHeight);
        g.setColour(juce::Colours::red.withAlpha(0.95f));
        g.drawVerticalLine(static_cast<int>(ex1), restBandTop,
                           restBandTop + restBandHeight);

        if (ew > 40.0f)
        {
          juce::String restTag = "rest";
          if (i == 0)
            restTag = "pre-rest";
          else if (i + 1 == chunk.events.size())
            restTag = "post-rest";
          g.setColour(juce::Colours::white.withAlpha(0.95f));
          g.drawFittedText(restTag + " d:" + juce::String(overlapEnd - overlapStart),
                           static_cast<int>(ex1) + 2,
                           static_cast<int>(restBandTop),
                           static_cast<int>(ew) - 3,
                           static_cast<int>(restBandHeight),
                           juce::Justification::centredLeft, 1, 0.85f);
        }
        else if (ew > 12.0f)
        {
          g.setColour(juce::Colours::white.withAlpha(0.95f));
          g.drawFittedText("R", static_cast<int>(ex1) + 1,
                           static_cast<int>(restBandTop),
                           static_cast<int>(ew) - 1,
                           static_cast<int>(restBandHeight),
                           juce::Justification::centredLeft, 1, 1.0f);
        }
        continue;
      }

      ++noteCount;

      // Black: midi segments (placed on their pitch row).
      const float noteMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                                          static_cast<float>(MAX_MIDI_NOTE),
                                          ev.midiNote);
      const float yCenter = midiToY(noteMidi) + pixelsPerSemitone * 0.5f;
      const float h = std::max(6.0f, pixelsPerSemitone * 0.72f);
      const float ny = yCenter - h * 0.5f;
      g.setColour(juce::Colours::black.withAlpha(0.84f));
      g.fillRoundedRectangle(ex1, ny, ew, h, 2.0f);

      if (ew > 60.0f && eventLabelsInChunk < 80)
      {
        const juce::String noteLabel =
            "ev#" + juce::String(static_cast<int>(i)) + " m:" +
            juce::String(ev.midiNote, 2) + " f:" + juce::String(overlapStart) +
            "-" + juce::String(overlapEnd) + " d:" +
            juce::String(overlapEnd - overlapStart) + " att:" +
            juce::String(ev.attachedStartFrame) + " durS:" +
            juce::String(ev.durationSeconds, 3);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawFittedText(noteLabel, static_cast<int>(ex1) + 3,
                         static_cast<int>(ny) - 15,
                         static_cast<int>(std::min(320.0f, ew)), 13,
                         juce::Justification::centredLeft, 1, 0.70f);
        ++eventLabelsInChunk;
      }
      else if (ew > 22.0f)
      {
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawFittedText("m:" + juce::String(ev.midiNote, 1),
                         static_cast<int>(ex1) + 2, static_cast<int>(ny),
                         static_cast<int>(ew) - 3, static_cast<int>(h),
                         juce::Justification::centredLeft, 1, 0.9f);
      }
      else if (ew > 8.0f)
      {
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawVerticalLine(static_cast<int>(ex1 + 0.5f), ny, ny + h);
      }
    }

    const juce::String label =
        "S" + juce::String(chunk.chunkIndex) + " f:" +
        juce::String(startFrame) + "-" + juce::String(endFrame) + " len:" +
        juce::String(endFrame - startFrame) + "f/" +
        juce::String(chunkSeconds, 2) + "s n:" + juce::String(noteCount) +
        " r:" + juce::String(restCount) + " ev:" +
        juce::String(static_cast<int>(chunk.events.size())) + " rstTh:" +
        juce::String(chunk.shortRestThreshold);

    const int textX = static_cast<int>(x1 + 3.0f);
    const int textY = 16;
    const int textWidth = std::max(40, static_cast<int>(width - 6.0f));
    const int textHeight = 14;

    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRect(static_cast<float>(textX), static_cast<float>(textY),
               static_cast<float>(textWidth), static_cast<float>(textHeight));
    g.setColour(juce::Colours::white.withAlpha(0.96f));
    g.drawFittedText(label, textX + 2, textY, textWidth - 4, textHeight,
                     juce::Justification::centredLeft, 1, 0.8f);

    ++chunksDrawn;
    if (chunksDrawn >= maxChunks)
      break;
  }
}

void PianoRollComponent::drawTimeline(juce::Graphics &g)
{
  TimelineRenderer::TimelineParams params;
  params.displayMode = timelineDisplayMode;
  params.beatNumerator = timelineBeatNumerator;
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.componentWidth = getWidth();
  timelineRenderer->drawTimeline(g, params);
}

void PianoRollComponent::drawLoopTimeline(juce::Graphics &g)
{
  TimelineRenderer::LoopParams params;
  params.displayMode = timelineDisplayMode;
  params.beatNumerator = timelineBeatNumerator;
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.componentWidth = getWidth();
  params.loopStartSeconds = 0.0;
  params.loopEndSeconds = 0.0;
  params.loopEnabled = false;
  // Match prior behaviour: if there's no project, draw only the empty gutter.
  if (project)
  {
    if (loopDragHandler_ && loopDragHandler_->isDragging())
    {
      params.loopStartSeconds = loopDragHandler_->getDragStartSeconds();
      params.loopEndSeconds = loopDragHandler_->getDragEndSeconds();
      params.loopEnabled = true;
    }
    else
    {
      const auto &loopRange = project->getLoopRange();
      params.loopStartSeconds = loopRange.startSeconds;
      params.loopEndSeconds = loopRange.endSeconds;
      params.loopEnabled = loopRange.enabled;
    }
  }
  timelineRenderer->drawLoopTimeline(g, params);
}

void PianoRollComponent::drawNotes(juce::Graphics &g, NoteRenderPass pass)
{
  auto rendererPass = NoteRenderer::Pass::Overlay;
  if (pass == NoteRenderPass::Body)
    rendererPass = NoteRenderer::Pass::Body;
  else if (pass == NoteRenderPass::HoverShadow)
    rendererPass = NoteRenderer::Pass::HoverShadow;
  else if (pass == NoteRenderPass::HoveredBody)
    rendererPass = NoteRenderer::Pass::HoveredBody;

  const bool splitModeActive = editMode == EditMode::Split;
  noteRenderer->setHoveredNote(hoveredNote);
  noteRenderer->setPreviewPlaybackState(previewPlaybackActive,
                                        previewPlaybackStartFrame,
                                        previewPlaybackEndFrame,
                                        previewPlaybackCurrentTime);
  noteRenderer->draw(g, rendererPass, splitModeActive, getWidth());
}

void PianoRollComponent::drawPitchCurves(juce::Graphics &g)
{
  PitchCurveRenderer::Params params;
  params.showDeltaPitch = showDeltaPitch;
  params.showBasePitch = showBasePitch;
  params.showUvInterpolationDebug = showUvInterpolationDebug;
  params.showActualF0Debug = showActualF0Debug;
  params.hidePitchCurves = false;
  params.componentWidth = getWidth();
  pitchCurveRenderer->draw(g, params);
}

void PianoRollComponent::drawPianoKeys(juce::Graphics &g)
{
  ScaleMode activeScaleMode = ScaleMode::Chromatic;
  if (snapToSemitoneDrag && dragSnapMode == DragSnapMode::Scale)
  {
    const ScaleMode preferredMode =
        project != nullptr ? project->getPreferredScaleMode() : ScaleMode::Major;
    activeScaleMode = previewScaleMode.value_or(preferredMode);
  }
  const int activeScaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  const int horizontalScrollBarSize = showHorizontalScrollBar ? 8 : 0;
  pianoKeysRenderer->draw(g, getHeight(), horizontalScrollBarSize,
                          activeScaleMode, activeScaleRootNote,
                          ScaleUtils::getReferenceOffsetSemitones(
                              pitchReferenceHz));
}

float PianoRollComponent::midiToY(float midiNote) const
{
  return (MAX_MIDI_NOTE - midiNote) * pixelsPerSemitone;
}

float PianoRollComponent::yToMidi(float y) const
{
  return MAX_MIDI_NOTE - y / pixelsPerSemitone;
}

float PianoRollComponent::timeToX(double time) const
{
  return static_cast<float>(time * pixelsPerSecond);
}

double PianoRollComponent::xToTime(float x) const
{
  return x / pixelsPerSecond;
}

double PianoRollComponent::getTimelineQuarterNoteSeconds() const
{
  const double bpm = juce::jlimit(20.0, 300.0, timelineTempoBpm);
  return bpm > 0.0 ? 60.0 / bpm : (60.0 / 120.0);
}

double PianoRollComponent::getTimelineBeatSeconds() const
{
  const int denominator = normalizeTimelineBeatDenominator(timelineBeatDenominator);
  return getTimelineQuarterNoteSeconds() * (4.0 / static_cast<double>(denominator));
}

double PianoRollComponent::getTimelineBarSeconds() const
{
  const int numerator = juce::jmax(1, timelineBeatNumerator);
  return getTimelineBeatSeconds() * static_cast<double>(numerator);
}

double PianoRollComponent::getTimelineGridSeconds() const
{
  const double quarterNotes = gridDivisionToQuarterNotes(timelineGridDivision);
  return getTimelineQuarterNoteSeconds() * quarterNotes;
}

bool PianoRollComponent::shouldSnapCycleToGrid() const
{
  return timelineDisplayMode == TimelineDisplayMode::Beats &&
         timelineSnapCycle &&
         getTimelineGridSeconds() > 1.0e-6;
}

double PianoRollComponent::snapTimeToTimelineGrid(double timeSeconds) const
{
  if (!shouldSnapCycleToGrid())
    return std::max(0.0, timeSeconds);

  const double interval = getTimelineGridSeconds();
  const double snapped = std::round(timeSeconds / interval) * interval;
  return std::max(0.0, snapped);
}

bool PianoRollComponent::isCanvasPoint(const juce::MouseEvent &e) const
{
  return e.x >= pianoKeysWidth && e.y >= headerHeight &&
         e.x < pianoKeysWidth + getVisibleContentWidth() &&
         e.y < headerHeight + getVisibleContentHeight();
}

bool PianoRollComponent::isModifierZoomDrag(const juce::MouseEvent &e) const
{
#if JUCE_MAC
  return e.mods.isAltDown();
#else
  return e.mods.isCtrlDown();
#endif
}

void PianoRollComponent::applyModifierZoomDrag(const juce::MouseEvent &e)
{
  const float deltaX = e.position.x - modifierZoomLastPosition.x;
  const float deltaY = modifierZoomLastPosition.y - e.position.y;
  modifierZoomLastPosition = e.position;

  if (std::abs(deltaX) < 0.01f && std::abs(deltaY) < 0.01f)
    return;

  const float mouseX = static_cast<float>(e.x - pianoKeysWidth);
  const float mouseY = static_cast<float>(e.y - headerHeight);

  if (std::abs(deltaX) >= 0.01f)
  {
    const float zoomFactorX = std::pow(1.0065f, deltaX);
    const double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));
    const int visibleWidth = getVisibleContentWidth();
    const double totalTime = getTimelineDuration();
    const float minPpsX =
        (visibleWidth > 0 && totalTime > 0.0)
            ? std::max(MIN_PIXELS_PER_SECOND,
                       static_cast<float>(visibleWidth / totalTime))
            : MIN_PIXELS_PER_SECOND;
    const float newPpsX = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND,
                                       pixelsPerSecond * zoomFactorX);

    pixelsPerSecond = newPpsX;
    coordMapper->setPixelsPerSecond(newPpsX);

    const float newMouseX = static_cast<float>(timeAtMouse * newPpsX);
    scrollX = std::max(0.0, static_cast<double>(newMouseX - mouseX));
    coordMapper->setScrollX(scrollX);
  }

  if (std::abs(deltaY) >= 0.01f)
  {
    const float zoomFactorY = std::pow(1.0065f, deltaY);
    setPixelsPerSemitone(pixelsPerSemitone * zoomFactorY, mouseY);
  }

  updateScrollBars();
  repaint();

  if (std::abs(deltaX) >= 0.01f && onZoomChanged)
    onZoomChanged(pixelsPerSecond);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent &e)
{
  if (!project)
    return;

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  if (isCanvasPoint(e) && isModifierZoomDrag(e))
  {
    modifierZoomDragActive = true;
    modifierZoomLastPosition = e.position;
    return;
  }

  // Handle timeline clicks - seek to position
  if (e.y < timelineHeight && e.x >= pianoKeysWidth)
  {
    double time = std::max(0.0, xToTime(adjustedX));
    setCursorTime(time);
    if (onSeek)
      onSeek(time);
    return;
  }

  // Handle loop timeline drag (always active, priority over edit modes)
  if (loopDragHandler_->mouseDown(e, adjustedX, adjustedY))
    return;

  // Ignore clicks outside main area
  if (e.y < headerHeight || e.x < pianoKeysWidth)
    return;

  if (auto *note = findPreviewButtonNoteAt(adjustedX, adjustedY))
  {
    triggerPreviewForNote(*note);
    return;
  }

  // Delegate to current mode handler
  if (currentHandler_)
  {
    currentHandler_->mouseDown(e, adjustedX, adjustedY);
    updatePreviewButtonBounds();
  }
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent &e)
{
  if (modifierZoomDragActive)
  {
    applyModifierZoomDrag(e);
    return;
  }

  // Throttle repaints during drag to ~60fps max
  juce::int64 now = juce::Time::getMillisecondCounter();
  bool shouldRepaint = (now - lastDragRepaintTime) >= minDragRepaintInterval;
  juce::ignoreUnused(shouldRepaint);

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Loop drag has priority (always active)
  if (loopDragHandler_->mouseDrag(e, adjustedX, adjustedY))
  {
    if (shouldRepaint)
    {
      repaint();
      lastDragRepaintTime = now;
    }
    return;
  }

  // Delegate to current mode handler
  if (currentHandler_ && currentHandler_->mouseDrag(e, adjustedX, adjustedY))
  {
    updatePreviewButtonBounds();
    if (shouldRepaint)
    {
      repaint();
      lastDragRepaintTime = now;
    }
  }
}

void PianoRollComponent::mouseUp(const juce::MouseEvent &e)
{
  if (modifierZoomDragActive)
  {
    modifierZoomDragActive = false;
    return;
  }

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Loop drag has priority (always active)
  if (loopDragHandler_->mouseUp(e, adjustedX, adjustedY))
    return;

  // Delegate to current mode handler
  if (currentHandler_)
  {
    currentHandler_->mouseUp(e, adjustedX, adjustedY);
    updatePreviewButtonBounds();
  }
}

void PianoRollComponent::mouseMove(const juce::MouseEvent &e)
{
  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  Note *noteUnderMouse = nullptr;
  if (e.y >= headerHeight && e.x >= pianoKeysWidth)
    noteUnderMouse = findNoteAt(adjustedX, adjustedY);
  if (!noteUnderMouse)
    noteUnderMouse = findPreviewButtonNoteAt(adjustedX, adjustedY);
  if (!noteUnderMouse && hoveredNote &&
      getPreviewHoverBounds(*hoveredNote).contains(adjustedX, adjustedY))
  {
    noteUnderMouse = hoveredNote;
  }
  setHoveredNote(noteUnderMouse);

  // Loop timeline cursor handling (always active)
  loopDragHandler_->mouseMove(e, adjustedX, adjustedY);

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseMove(e, adjustedX, adjustedY);

  // Pitch tool handle hover (uses raw event coordinates, not world-adjusted)
  if (editMode == EditMode::Select && pitchToolHandles && !pitchToolHandles->isEmpty() &&
      e.y >= headerHeight && e.x >= pianoKeysWidth)
  {
    int hitIndex = pitchToolHandles->hitTest(e.position.x, e.position.y);
    if (hitIndex != hoveredPitchToolHandle)
    {
      hoveredPitchToolHandle = hitIndex;
      pitchToolHandles->setHoveredHandleIndex(hitIndex);
      repaint();
    }
  }
  else if (hoveredPitchToolHandle != -1)
  {
    hoveredPitchToolHandle = -1;
    if (pitchToolHandles)
      pitchToolHandles->setHoveredHandleIndex(-1);
    repaint();
  }
}

void PianoRollComponent::mouseExit(const juce::MouseEvent &)
{
  if (previewButton.isMouseOverOrDragging())
    return;

  setHoveredNote(nullptr);
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent &e)
{
  if (!project)
    return;

  if (e.y >= timelineHeight && e.y < headerHeight && e.x >= pianoKeysWidth)
  {
    auto loopRange = project->getLoopRange();
    if (loopRange.endSeconds > loopRange.startSeconds)
    {
      project->setLoopEnabled(false);
      if (onLoopRangeChanged)
        onLoopRangeChanged(project->getLoopRange());
      repaint();
    }
    return;
  }

  // Ignore double-clicks outside main area
  if (e.y < headerHeight || e.x < pianoKeysWidth)
    return;

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseDoubleClick(e, adjustedX, adjustedY);
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent &e,
                                        const juce::MouseWheelDetails &wheel)
{
  float scrollMultiplier = wheel.isSmooth ? 200.0f : 80.0f;
  const int visibleHeight = getVisibleContentHeight();
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineDuration();
  const float minPpsForFill =
      visibleHeight > 0
          ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
          : MIN_PIXELS_PER_SEMITONE;
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime))
          : MIN_PIXELS_PER_SECOND;

  bool isOverPianoKeys = e.x < pianoKeysWidth;
  bool isOverTimeline = e.y < headerHeight;

  // Hover-based zoom (no modifier keys needed)
  if (!e.mods.isCommandDown() && !e.mods.isCtrlDown())
  {
    // Over piano keys: vertical zoom
    if (isOverPianoKeys)
    {
      float mouseY = e.y - headerHeight;

      float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
      if (zoomFactor < 1.0f)
      {
        const float range = minPps * 0.35f;
        const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (pixelsPerSemitone - minPps) / range) : 0.0f;
        zoomFactor = 1.0f + (zoomFactor - 1.0f) * t; // elastic resistance near min
      }
      float newPps = pixelsPerSemitone * zoomFactor;
      newPps = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, newPps);
      setPixelsPerSemitone(newPps, mouseY);
      return;
    }

    // Over timeline: horizontal zoom
    if (isOverTimeline)
    {
      // Calculate time at mouse position before zoom
      float mouseX = e.x - pianoKeysWidth;
      double timeAtMouse = (mouseX + scrollX) / pixelsPerSecond;

      float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
      float newPps = pixelsPerSecond * zoomFactor;
      newPps =
          juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, newPps);
      pixelsPerSecond = newPps;
      coordMapper->setPixelsPerSecond(newPps);

      // Adjust scroll position to keep time at mouse position fixed
      double newScrollX = timeAtMouse * pixelsPerSecond - mouseX;
      newScrollX = std::max(0.0, newScrollX);
      scrollX = newScrollX;
      coordMapper->setScrollX(newScrollX);

      updateScrollBars();
      repaint();
      if (onZoomChanged)
        onZoomChanged(pixelsPerSecond);
      return;
    }

    // Normal scrolling in grid area
    float deltaX = wheel.deltaX;
    float deltaY = wheel.deltaY;

    if (e.mods.isShiftDown() && std::abs(deltaX) < 0.001f)
    {
      deltaX = deltaY;
      deltaY = 0.0f;
    }

    if (std::abs(deltaX) > 0.001f)
    {
      double newScrollX = scrollX - deltaX * scrollMultiplier;
      newScrollX = std::max(0.0, newScrollX);
      horizontalScrollBar.setCurrentRangeStart(newScrollX);
    }

    if (std::abs(deltaY) > 0.001f)
    {
      double newScrollY = scrollY - deltaY * scrollMultiplier;
      verticalScrollBar.setCurrentRangeStart(newScrollY);
    }
    return;
  }

  // Key-based zoom in grid area
  if (e.mods.isCommandDown() || e.mods.isCtrlDown())
  {
    float zoomFactor = 1.0f + wheel.deltaY * 0.3f;

    if (e.mods.isShiftDown())
    {
      // Vertical zoom - center on mouse position
      float mouseY = static_cast<float>(e.y - headerHeight);
      float midiAtMouse = yToMidi(mouseY + static_cast<float>(scrollY));

      float newPps = pixelsPerSemitone * zoomFactor;
      if (zoomFactor < 1.0f)
      {
        const float range = minPps * 0.35f;
        const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (pixelsPerSemitone - minPps) / range) : 0.0f;
        newPps = pixelsPerSemitone * (1.0f + (zoomFactor - 1.0f) * t);
      }
      juce::ignoreUnused(midiAtMouse);
      setPixelsPerSemitone(newPps, mouseY);
    }
    else
    {
      // Horizontal zoom - center on mouse position
      float mouseX = static_cast<float>(e.x - pianoKeysWidth);
      double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));

      float newPps = pixelsPerSecond * zoomFactor;
      newPps =
          juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, newPps);

      // Adjust scroll to keep mouse position stable
      float newMouseX = static_cast<float>(timeAtMouse * newPps);
      scrollX = std::max(0.0, static_cast<double>(newMouseX - mouseX));
      coordMapper->setScrollX(scrollX);

      pixelsPerSecond = newPps;
      coordMapper->setPixelsPerSecond(newPps);
      updateScrollBars();
      repaint();

      if (onZoomChanged)
        onZoomChanged(pixelsPerSecond);
    }
  }
}

void PianoRollComponent::mouseMagnify(const juce::MouseEvent &e,
                                      float scaleFactor)
{
  // Pinch-to-zoom on trackpad - horizontal zoom, center on mouse position
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineDuration();
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime))
          : MIN_PIXELS_PER_SECOND;
  float mouseX = static_cast<float>(e.x - pianoKeysWidth);
  double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));

  float newPps = pixelsPerSecond * scaleFactor;
  newPps = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, newPps);

  // Adjust scroll to keep mouse position stable
  float newMouseX = static_cast<float>(timeAtMouse * newPps);
  scrollX = std::max(0.0, static_cast<double>(newMouseX - mouseX));
  coordMapper->setScrollX(scrollX);

  pixelsPerSecond = newPps;
  coordMapper->setPixelsPerSecond(newPps);
  updateScrollBars();
  repaint();

  if (onZoomChanged)
    onZoomChanged(pixelsPerSecond);
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar *scrollBar,
                                        double newRangeStart)
{
  if (scrollBar == &horizontalScrollBar)
  {
    const double totalWidth = getTimelineDuration() * pixelsPerSecond;
    const double maxScrollX =
        std::max(0.0, totalWidth - static_cast<double>(getVisibleContentWidth()));
    scrollX = juce::jlimit(0.0, maxScrollX, newRangeStart);
    coordMapper->setScrollX(scrollX);

    // Notify scroll changed for synchronization
    if (onScrollChanged)
      onScrollChanged(scrollX);
  }
  else if (scrollBar == &verticalScrollBar)
  {
    scrollY = newRangeStart;
    coordMapper->setScrollY(newRangeStart);
  }
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setProject(Project *proj)
{
  liveTimelineEndSeconds = 0.0;
  liveRecordingSampleRate = 0.0;
  project = proj;
  hoveredNote = nullptr;
  setPreviewPlaybackState(false, 0, 0);
  selectedScaleMode =
      project != nullptr ? project->getScaleMode() : ScaleMode::Chromatic;
  selectedScaleRootNote = project != nullptr ? project->getScaleRootNote() : 0;
  pitchReferenceHz = project != nullptr ? project->getPitchReferenceHz() : 440;
  snapToSemitoneDrag = project != nullptr ? project->getSnapToSemitones() : false;
  dragSnapMode = project != nullptr
                     ? project->getDragSnapMode()
                     : DragSnapMode::Chromatic;
  timelineDisplayMode = project != nullptr
                            ? project->getTimelineDisplayMode()
                            : TimelineDisplayMode::Beats;
  timelineBeatNumerator = project != nullptr ? project->getTimelineBeatNumerator() : 4;
  timelineBeatDenominator =
      project != nullptr ? project->getTimelineBeatDenominator() : 4;
  timelineTempoBpm = project != nullptr ? project->getTimelineTempoBpm() : 120.0;
  timelineGridDivision = project != nullptr
                             ? project->getTimelineGridDivision()
                             : TimelineGridDivision::Quarter;
  timelineSnapCycle = project != nullptr ? project->getTimelineSnapCycle() : false;
  previewScaleRootNote.reset();
  previewScaleMode.reset();

  // Update modular components
  gridRenderer->setProject(proj);
  timelineRenderer->setProject(proj);
  waveformBackgroundRenderer->setProject(proj); // also clears its cache
  noteRenderer->setProject(proj);
  pitchCurveRenderer->setProject(proj); // also clears its cache
  scrollZoomController->setProject(proj);
  pitchEditor->setProject(proj);
  pitchEditor->setSnapToSemitoneDragEnabled(snapToSemitoneDrag);
  pitchEditor->setDragSnapMode(dragSnapMode);
  pitchEditor->setPitchReferenceHz(pitchReferenceHz);
  noteSplitter->setProject(proj);
  pitchToolController->setProject(proj);

  // Note: waveform and base-pitch caches are cleared by their renderers'
  // setProject calls above.

  updatePitchToolHandlesFromSelection();

  updateScrollBars();
  repaint();
}

void PianoRollComponent::beginLiveRecordingWaveform(
    double sampleRate, double timelineOffsetSeconds)
{
  waveformBackgroundRenderer->beginLiveWaveform(sampleRate,
                                                timelineOffsetSeconds);
  liveRecordingSampleRate = sampleRate;
  liveTimelineEndSeconds = std::max(0.0, timelineOffsetSeconds);
  updateScrollBars();
  repaint();
}

void PianoRollComponent::appendLiveRecordingWaveform(
    const juce::AudioBuffer<float> &buffer)
{
  waveformBackgroundRenderer->appendLiveWaveform(buffer);
  if (liveRecordingSampleRate > 0.0)
    liveTimelineEndSeconds +=
        static_cast<double>(buffer.getNumSamples()) / liveRecordingSampleRate;
  updateScrollBars();
  repaint();
}

void PianoRollComponent::setScaleMode(ScaleMode mode)
{
  if (selectedScaleMode == mode && !previewScaleMode.has_value())
    return;

  selectedScaleMode = mode;
  if (project != nullptr)
    project->setScaleMode(mode);
  previewScaleMode.reset();
  repaint();
}

void PianoRollComponent::setScaleRootNote(int noteInOctave)
{
  const int normalized = juce::jlimit(-1, 11, noteInOctave);
  const bool changed = selectedScaleRootNote != normalized;
  if (!changed && !previewScaleRootNote.has_value())
    return;

  selectedScaleRootNote = normalized;
  if (project != nullptr && changed)
    project->setScaleRootNote(normalized);
  previewScaleRootNote.reset();
  repaint();
}

void PianoRollComponent::setScaleRootPreview(std::optional<int> noteInOctave)
{
  std::optional<int> normalizedPreview;
  if (noteInOctave.has_value())
    normalizedPreview = juce::jlimit(-1, 11, *noteInOctave);

  if (previewScaleRootNote == normalizedPreview)
    return;

  previewScaleRootNote = normalizedPreview;
  repaint();
}

void PianoRollComponent::setScaleModePreview(std::optional<ScaleMode> mode)
{
  if (mode.has_value() &&
      (selectedScaleMode == ScaleMode::Chromatic ||
       selectedScaleMode == ScaleMode::None) &&
      !(snapToSemitoneDrag && dragSnapMode == DragSnapMode::Scale))
    mode.reset();

  if (previewScaleMode == mode)
    return;

  previewScaleMode = mode;
  repaint();
}

void PianoRollComponent::setSnapToSemitoneDrag(bool enabled)
{
  if (snapToSemitoneDrag == enabled)
    return;

  snapToSemitoneDrag = enabled;
  if (project != nullptr)
    project->setSnapToSemitones(enabled);
  pitchEditor->setSnapToSemitoneDragEnabled(enabled);
  repaint();
}

void PianoRollComponent::setDragSnapMode(DragSnapMode mode)
{
  if (dragSnapMode == mode)
    return;

  dragSnapMode = mode;
  if (project != nullptr)
    project->setDragSnapMode(mode);
  pitchEditor->setDragSnapMode(mode);
  repaint();
}

void PianoRollComponent::setPitchReferenceHz(int hz)
{
  const int normalized = juce::jlimit(430, 450, hz);
  if (pitchReferenceHz == normalized)
    return;

  pitchReferenceHz = normalized;
  if (project != nullptr)
    project->setPitchReferenceHz(normalized);
  pitchEditor->setPitchReferenceHz(normalized);
  repaint();
}

void PianoRollComponent::setTimelineDisplayMode(TimelineDisplayMode mode)
{
  if (timelineDisplayMode == mode)
    return;

  timelineDisplayMode = mode;
  if (project != nullptr)
    project->setTimelineDisplayMode(mode);
  repaint();
}

void PianoRollComponent::setTimelineBeatSignature(int numerator, int denominator)
{
  const int normalizedNumerator = juce::jlimit(1, 32, numerator);
  const int normalizedDenominator = normalizeTimelineBeatDenominator(denominator);
  if (timelineBeatNumerator == normalizedNumerator &&
      timelineBeatDenominator == normalizedDenominator)
    return;

  timelineBeatNumerator = normalizedNumerator;
  timelineBeatDenominator = normalizedDenominator;
  if (project != nullptr)
    project->setTimelineBeatSignature(normalizedNumerator, normalizedDenominator);
  repaint();
}

void PianoRollComponent::setTimelineTempoBpm(double bpm)
{
  const double normalized = juce::jlimit(20.0, 300.0, bpm);
  if (std::abs(timelineTempoBpm - normalized) < 1.0e-6)
    return;

  timelineTempoBpm = normalized;
  if (project != nullptr)
    project->setTimelineTempoBpm(normalized);
  repaint();
}

void PianoRollComponent::setTimelineGridDivision(TimelineGridDivision division)
{
  if (timelineGridDivision == division)
    return;

  timelineGridDivision = division;
  if (project != nullptr)
    project->setTimelineGridDivision(division);
  repaint();
}

void PianoRollComponent::setTimelineSnapCycle(bool enabled)
{
  if (timelineSnapCycle == enabled)
    return;

  timelineSnapCycle = enabled;
  if (project != nullptr)
    project->setTimelineSnapCycle(enabled);
}

void PianoRollComponent::setUndoManager(PitchUndoManager *manager)
{
  undoManager = manager;
  pitchEditor->setUndoManager(manager);
  noteSplitter->setUndoManager(manager);
}

bool PianoRollComponent::nudgeSelectedNotesBySemitones(int semitoneDelta)
{
  if (project == nullptr || semitoneDelta == 0)
    return false;

  auto selectedNotes = project->getSelectedNotes();
  if (selectedNotes.empty())
    return false;

  constexpr float minMidi = static_cast<float>(MIN_MIDI_NOTE);
  constexpr float maxMidi = static_cast<float>(MAX_MIDI_NOTE);

  std::vector<Note *> notesToMove;
  std::vector<float> oldMidis;
  std::vector<float> newMidis;
  notesToMove.reserve(selectedNotes.size());
  oldMidis.reserve(selectedNotes.size());
  newMidis.reserve(selectedNotes.size());

  int dirtyStartFrame = std::numeric_limits<int>::max();
  int dirtyEndFrame = std::numeric_limits<int>::min();

  for (auto *note : selectedNotes)
  {
    if (!note || note->isRest())
      continue;

    const float oldMidi = note->getMidiNote();
    const float offset = note->getPitchOffset();
    const float oldAdjustedMidi = oldMidi + offset;
    const float movedAdjustedMidi =
        juce::jlimit(minMidi, maxMidi,
                     oldAdjustedMidi + static_cast<float>(semitoneDelta));
    const float movedMidi = movedAdjustedMidi - offset;

    if (std::abs(movedMidi - oldMidi) <= 1.0e-6f)
      continue;

    notesToMove.push_back(note);
    oldMidis.push_back(oldMidi);
    newMidis.push_back(movedMidi);
    dirtyStartFrame = std::min(dirtyStartFrame, note->getStartFrame());
    dirtyEndFrame = std::max(dirtyEndFrame, note->getEndFrame());
  }

  if (notesToMove.empty())
    return false;

  auto rebuildAndNotify =
      [this, dirtyStartFrame, dirtyEndFrame](const std::vector<Note *> &)
  {
    if (project == nullptr)
      return;

    PitchCurveProcessor::rebuildBaseFromNotes(*project);
    invalidateBasePitchCache();

    const int f0Size = static_cast<int>(project->getAudioData().f0.size());
    if (f0Size > 0 && dirtyStartFrame <= dirtyEndFrame)
    {
      const int smoothStart = std::max(0, dirtyStartFrame - 60);
      const int smoothEnd = std::min(f0Size, dirtyEndFrame + 60);
      project->setF0DirtyRange(smoothStart, smoothEnd);
    }

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();

    repaint();
  };

  if (undoManager)
  {
    auto *projectPtr = project;
    auto action = std::make_unique<MultiNoteMidiNudgeAction>(
        notesToMove, oldMidis, newMidis,
        [projectPtr, dirtyStartFrame,
         dirtyEndFrame](const std::vector<Note *> &notes)
        {
          if (!projectPtr)
            return;
          PitchCurveProcessor::rebuildBaseFromNotes(*projectPtr);
          for (auto *note : notes)
            if (note)
              note->markSynthDirty();
          const int f0Size = static_cast<int>(
              projectPtr->getAudioData().f0.size());
          projectPtr->setF0DirtyRange(
              std::max(0, dirtyStartFrame - 60),
              std::min(f0Size, dirtyEndFrame + 60));
        });
    undoManager->addAction(std::move(action));
  }

  for (size_t i = 0; i < notesToMove.size(); ++i)
  {
    notesToMove[i]->setMidiNote(newMidis[i]);
    notesToMove[i]->markDirty();
    notesToMove[i]->markSynthDirty();
  }

  rebuildAndNotify(notesToMove);
  return true;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress &key)
{
  const auto mods = key.getModifiers();
  if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
    return false;

  const int keyCode = key.getKeyCode();
  if (keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey)
  {
    const int direction = keyCode == juce::KeyPress::upKey ? 1 : -1;
    const int step = mods.isShiftDown() ? 12 : 1;
    return nudgeSelectedNotesBySemitones(direction * step);
  }

  return false;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress &key,
                                    juce::Component *)
{
  return keyPressed(key);
}

void PianoRollComponent::focusLost(FocusChangeType cause)
{
  juce::ignoreUnused(cause);
  // Don't automatically re-grab focus - let the host manage focus normally
  // Focus will be re-acquired when user clicks on the piano roll
}

void PianoRollComponent::focusGained(FocusChangeType cause)
{
  juce::ignoreUnused(cause);
  // Focus gained - nothing special needed
}

void PianoRollComponent::setCursorTime(double time)
{
  if (std::abs(cursorTime - time) < 0.0001)
    return; // Skip if no change

  // Calculate dirty rectangle for cursor position
  // Include full height so the old and new cursor lines are fully repainted.
  auto getCursorRect = [this](double t) -> juce::Rectangle<int>
  {
    float x =
        static_cast<float>(t * pixelsPerSecond - scrollX) + pianoKeysWidth;
    constexpr int cursorHalfWidth = 2;
    int rectX = static_cast<int>(x) - cursorHalfWidth;
    int rectWidth = cursorHalfWidth * 2 + 1;
    return juce::Rectangle<int>(rectX, 0, rectWidth, getHeight());
  };

  // Repaint OLD cursor position (the current cursorTime that's about to change)
  repaint(getCursorRect(cursorTime));

  // Update cursor time
  cursorTime = time;

  // Repaint NEW cursor position
  repaint(getCursorRect(cursorTime));

  if (onCursorMoved)
    onCursorMoved();
}

void PianoRollComponent::setPixelsPerSecond(float pps, bool centerOnCursor)
{
  float oldPps = pixelsPerSecond;
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineDuration();
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime))
          : MIN_PIXELS_PER_SECOND;
  float newPps =
      juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, pps);

  if (std::abs(oldPps - newPps) < 0.01f)
    return; // No significant change

  if (centerOnCursor)
  {
    // Calculate cursor position relative to view
    float cursorX = static_cast<float>(cursorTime * oldPps);
    float cursorRelativeX = cursorX - static_cast<float>(scrollX);

    // Calculate new scroll position to keep cursor at same relative position
    float newCursorX = static_cast<float>(cursorTime * newPps);
    scrollX = static_cast<double>(newCursorX - cursorRelativeX);
    scrollX = std::max(0.0, scrollX);
    coordMapper->setScrollX(scrollX);
  }

  pixelsPerSecond = newPps;
  coordMapper->setPixelsPerSecond(newPps);
  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();

  // Don't call onZoomChanged here to avoid infinite recursion
  // The caller is responsible for synchronizing other components
}

void PianoRollComponent::setPixelsPerSemitone(float pps, float anchorContentY)
{
  const float oldPps = pixelsPerSemitone;
  const int visibleHeight = getVisibleContentHeight();
  const float minPpsForFill =
      visibleHeight > 0
          ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
          : MIN_PIXELS_PER_SEMITONE;
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);

  const float newPps = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, pps);
  if (std::abs(oldPps - newPps) < 0.01f)
    return;

  float effectiveAnchorY = anchorContentY;
  if (effectiveAnchorY < 0.0f)
    effectiveAnchorY = static_cast<float>(visibleHeight) * 0.5f;
  effectiveAnchorY = juce::jlimit(0.0f, static_cast<float>(visibleHeight),
                                  effectiveAnchorY);

  const float midiAtAnchor =
      MAX_MIDI_NOTE -
      (effectiveAnchorY + static_cast<float>(scrollY)) / oldPps;

  pixelsPerSemitone = newPps;
  coordMapper->setPixelsPerSemitone(pixelsPerSemitone);

  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY =
      std::max(0.0, totalHeight - static_cast<double>(visibleHeight));
  const double anchoredScrollY =
      (MAX_MIDI_NOTE - midiAtAnchor) * pixelsPerSemitone - effectiveAnchorY;
  scrollY = juce::jlimit(0.0, maxScrollY, anchoredScrollY);
  coordMapper->setScrollY(scrollY);

  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setScrollX(double x)
{
  const double totalWidth = getTimelineDuration() * pixelsPerSecond;
  const double maxScrollX =
      std::max(0.0, totalWidth - static_cast<double>(getVisibleContentWidth()));
  const double clampedX = juce::jlimit(0.0, maxScrollX, x);

  if (std::abs(scrollX - clampedX) < 0.01)
    return; // No significant change

  scrollX = clampedX;
  coordMapper->setScrollX(clampedX);
  horizontalScrollBar.setCurrentRangeStart(clampedX);

  // Don't call onScrollChanged here to avoid infinite recursion
  // The caller is responsible for synchronizing other components

  repaint();
  updatePreviewButtonBounds();
}

void PianoRollComponent::setScrollY(double y)
{
  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY = std::max(
      0.0, totalHeight - static_cast<double>(getVisibleContentHeight()));
  const double clampedY = juce::jlimit(0.0, maxScrollY, y);

  if (std::abs(scrollY - clampedY) < 0.01)
    return;

  scrollY = clampedY;
  coordMapper->setScrollY(clampedY);
  verticalScrollBar.setCurrentRangeStart(clampedY);
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::centerOnPitchRange(float minMidi, float maxMidi)
{
  // Calculate center MIDI note
  float centerMidi = (minMidi + maxMidi) / 2.0f;

  // Calculate Y position for center
  float centerY = midiToY(centerMidi);

  // Get visible height
  int visibleHeight = getHeight() - 8; // scrollbar height

  // Calculate scroll position to center the pitch range
  double newScrollY = centerY - visibleHeight / 2.0;

  // Clamp to valid range
  double totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  newScrollY =
      juce::jlimit(0.0, std::max(0.0, totalHeight - visibleHeight), newScrollY);

  scrollY = newScrollY;
  coordMapper->setScrollY(newScrollY);
  verticalScrollBar.setCurrentRangeStart(newScrollY);
  updatePreviewButtonBounds();
  repaint();
}

bool PianoRollComponent::centerOnCurrentPitchRange()
{
  if (project == nullptr)
    return false;

  float minMidi = std::numeric_limits<float>::max();
  float maxMidi = std::numeric_limits<float>::lowest();
  bool hasNotePitch = false;

  for (const auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;

    const float midi = note.getAdjustedMidiNote();
    minMidi = std::min(minMidi, midi);
    maxMidi = std::max(maxMidi, midi);
    hasNotePitch = true;

    for (const float delta : note.getDeltaPitch())
    {
      minMidi = std::min(minMidi, midi + delta);
      maxMidi = std::max(maxMidi, midi + delta);
    }
  }

  if (!hasNotePitch)
    return false;

  minMidi -= 1.0f;
  maxMidi += 1.0f;
  if (maxMidi - minMidi < 2.0f)
  {
    const float centerMidi = (minMidi + maxMidi) * 0.5f;
    minMidi = centerMidi - 1.0f;
    maxMidi = centerMidi + 1.0f;
  }

  centerOnPitchRange(minMidi, maxMidi);
  return true;
}

void PianoRollComponent::fitPitchRangeToView(float minMidi, float maxMidi)
{
  if (!std::isfinite(minMidi) || !std::isfinite(maxMidi))
    return;

  if (minMidi > maxMidi)
    std::swap(minMidi, maxMidi);

  minMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                         static_cast<float>(MAX_MIDI_NOTE), minMidi);
  maxMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                         static_cast<float>(MAX_MIDI_NOTE), maxMidi);

  const int visibleHeight = getVisibleContentHeight();
  if (visibleHeight <= 0)
    return;

  const float rangeSemitones = std::max(1.0f, maxMidi - minMidi + 1.0f);
  const float minPpsForFill =
      static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1);
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
  pixelsPerSemitone = juce::jlimit(
      minPps, MAX_PIXELS_PER_SEMITONE,
      static_cast<float>(visibleHeight) / rangeSemitones);
  coordMapper->setPixelsPerSemitone(pixelsPerSemitone);

  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY =
      std::max(0.0, totalHeight - static_cast<double>(visibleHeight));
  const double lowestNoteBottomY =
      (static_cast<double>(MAX_MIDI_NOTE) - static_cast<double>(minMidi) +
       1.0) *
      static_cast<double>(pixelsPerSemitone);

  scrollY = juce::jlimit(
      0.0, maxScrollY,
      lowestNoteBottomY - static_cast<double>(visibleHeight));
  coordMapper->setScrollY(scrollY);

  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setEditMode(EditMode mode)
{
  editMode = mode;

  // Clear split guide when leaving split mode
  if (mode != EditMode::Split && splitHandler_)
  {
    splitHandler_->clearGuide();
  }

  // Change cursor based on mode
  if (mode == EditMode::Draw)
  {
    // Create a custom pen cursor
    // Simple pen icon: 16x16 pixels with pen tip at bottom-left
    juce::Image penImage(juce::Image::ARGB, 16, 16, true);
    juce::Graphics g(penImage);

    // Draw a simple pen shape
    g.setColour(juce::Colours::white);
    // Pen body (diagonal line from top-right to bottom-left)
    g.drawLine(12.0f, 2.0f, 2.0f, 12.0f, 2.0f);
    // Pen tip (small triangle at bottom-left)
    juce::Path tip;
    tip.addTriangle(0.0f, 14.0f, 4.0f, 10.0f, 2.0f, 12.0f);
    g.fillPath(tip);

    // Set hotspot at pen tip (bottom-left corner)
    setMouseCursor(juce::MouseCursor(penImage, 0, 14));
  }
  else
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }

  // Update currentHandler_ based on the new mode
  switch (mode)
  {
  case EditMode::Select:
    currentHandler_ = selectHandler_.get();
    break;
  case EditMode::Draw:
    currentHandler_ = drawHandler_.get();
    break;
  case EditMode::Split:
    currentHandler_ = splitHandler_.get();
    break;
  }

  if (mode != EditMode::Select)
  {
    hoveredPitchToolHandle = -1;
    if (pitchToolHandles)
      pitchToolHandles->setHoveredHandleIndex(-1);
  }
  updatePitchToolHandlesFromSelection();

  repaint();
}

std::vector<Note *> PianoRollComponent::getSelectedNotes() const
{
  if (!project)
    return {};

  std::vector<Note *> selected;
  for (auto &note : project->getNotes())
  {
    if (note.isSelected())
      selected.push_back(&note);
  }
  return selected;
}

void PianoRollComponent::updatePitchToolHandlesFromSelection()
{
  if (!pitchToolHandles || !coordMapper)
    return;

  if (!project || editMode != EditMode::Select)
  {
    pitchToolHandles->clear();
    hoveredPitchToolHandle = -1;
    pitchToolHandles->setHoveredHandleIndex(-1);
    return;
  }

  pitchToolHandles->updateHandles(getSelectedNotes(), *coordMapper);
  if (hoveredPitchToolHandle >=
      static_cast<int>(pitchToolHandles->getHandles().size()))
  {
    hoveredPitchToolHandle = -1;
    pitchToolHandles->setHoveredHandleIndex(-1);
  }
}

Note *PianoRollComponent::findNoteAt(float x, float y)
{
  if (!project)
    return nullptr;

  for (auto &note : project->getNotes())
  {
    // Skip rest notes
    if (note.isRest())
      continue;

    float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
    float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
    float noteY = midiToY(note.getAdjustedMidiNote());
    float noteH = pixelsPerSemitone;

    if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH)
    {
      return &note;
    }
  }

  return nullptr;
}

juce::Rectangle<float>
PianoRollComponent::getPreviewButtonBounds(const Note &note) const
{
  if (!project)
    return {};

  const auto &audioData = project->getAudioData();
  const float noteStartTime = static_cast<float>(framesToSeconds(note.getStartFrame()));
  const float noteEndTime = static_cast<float>(framesToSeconds(note.getEndFrame()));
  const float x = noteStartTime * pixelsPerSecond;
  const float w = (noteEndTime - noteStartTime) * pixelsPerSecond;
  const float renderedWidth = std::max(w, 4.0f);
  const float h = pixelsPerSemitone;
  const float y = midiToY(note.getAdjustedMidiNote());

  juce::Rectangle<float> shadowBounds(x, y, renderedWidth, h);
  const float *samples = nullptr;
  int totalSamples = 0;
  int startSample = 0;
  int endSample = 0;
  const auto &clipWaveform = note.getClipWaveform();

  if (!clipWaveform.empty())
  {
    samples = clipWaveform.data();
    totalSamples = static_cast<int>(clipWaveform.size());
    endSample = totalSamples;
  }
  else if (audioData.waveform.getNumSamples() > 0)
  {
    samples = audioData.waveform.getReadPointer(0);
    totalSamples = audioData.waveform.getNumSamples();
    startSample = static_cast<int>(noteStartTime * audioData.sampleRate);
    endSample = static_cast<int>(noteEndTime * audioData.sampleRate);
    startSample = std::max(0, std::min(startSample, totalSamples - 1));
    endSample = std::max(startSample + 1, std::min(endSample, totalSamples));
  }

  if (samples && totalSamples > 0 && w > 2.0f && endSample > startSample)
  {
    const int pointCount =
        std::max(2, std::min(512, static_cast<int>(std::ceil(w)) + 1));
    const auto envelope = VisualWaveformEnvelope::build(
        samples, totalSamples, startSample, endSample, pointCount,
        renderedWidth, audioData.sampleRate, pixelsPerSecond);
    const float maxSample =
        envelope.empty()
            ? 0.0f
            : *std::max_element(envelope.begin(), envelope.end());

    const float centreY = y + h * 0.5f;
    const float waveHeight = h * 3.0f;
    shadowBounds = juce::Rectangle<float>(x, centreY - maxSample * waveHeight * 0.5f,
                                          renderedWidth, maxSample * waveHeight);
  }

  shadowBounds = shadowBounds.expanded(4.0f, 4.0f);

  const float buttonWidth = static_cast<float>(previewButtonWidth);
  const float buttonHeight = static_cast<float>(previewButtonHeight);
  const float buttonX = shadowBounds.getCentreX() - buttonWidth * 0.5f;
  const float buttonY = shadowBounds.getBottom() + 7.0f;
  return {buttonX, buttonY, buttonWidth, buttonHeight};
}

juce::Rectangle<float>
PianoRollComponent::getPreviewHoverBounds(const Note &note) const
{
  const float noteStartTime =
      static_cast<float>(framesToSeconds(note.getStartFrame()));
  const float noteEndTime =
      static_cast<float>(framesToSeconds(note.getEndFrame()));
  const float noteX = noteStartTime * pixelsPerSecond;
  const float noteW = std::max((noteEndTime - noteStartTime) * pixelsPerSecond,
                               4.0f);
  const float noteY = midiToY(note.getAdjustedMidiNote());
  const juce::Rectangle<float> noteBounds(noteX, noteY, noteW,
                                          pixelsPerSemitone);

  return noteBounds.getUnion(getPreviewButtonBounds(note)).expanded(4.0f);
}

juce::Rectangle<int>
PianoRollComponent::getPreviewButtonLocalBounds(const Note &note) const
{
  const auto bounds =
      getPreviewButtonBounds(note)
          .translated(static_cast<float>(pianoKeysWidth) -
                          static_cast<float>(scrollX),
                      static_cast<float>(headerHeight) -
                          static_cast<float>(scrollY));
  return {static_cast<int>(std::round(bounds.getX())),
          static_cast<int>(std::round(bounds.getY())), previewButtonWidth,
          previewButtonHeight};
}

void PianoRollComponent::updatePreviewButtonBounds()
{
  const bool isChangingPitch =
      (selectHandler_ && (selectHandler_->isSingleNoteDragging() ||
                          selectHandler_->getIsDeltaScaleDragging() ||
                          selectHandler_->getIsDeltaOffsetDragging())) ||
      (pitchToolController && pitchToolController->isDragging()) ||
      (pitchEditor && (pitchEditor->isDraggingNote() ||
                       pitchEditor->isDraggingMultiNotes() ||
                       pitchEditor->isDrawingPitch()));

  if (!hoveredNote || !project || isChangingPitch)
  {
    previewButton.setVisible(false);
    return;
  }

  const auto bounds = getPreviewButtonLocalBounds(*hoveredNote);
  const int horizontalScrollBarSize = showHorizontalScrollBar ? 8 : 0;
  constexpr int verticalScrollBarSize = 8;
  const auto mainArea = getLocalBounds()
                            .withTrimmedLeft(pianoKeysWidth)
                            .withTrimmedTop(headerHeight)
                            .withTrimmedBottom(horizontalScrollBarSize)
                            .withTrimmedRight(verticalScrollBarSize);

  previewButton.setBounds(bounds);
  previewButton.setVisible(bounds.intersects(mainArea));
}

Note *PianoRollComponent::findPreviewButtonNoteAt(float x, float y) const
{
  if (!hoveredNote || !project)
    return nullptr;

  return getPreviewButtonBounds(*hoveredNote).contains(x, y) ? hoveredNote
                                                             : nullptr;
}

void PianoRollComponent::triggerPreviewForNote(Note &note)
{
  if (!project)
    return;

  auto &notes = project->getNotes();
  auto it = std::find_if(notes.begin(), notes.end(),
                         [&note](const Note &candidate)
                         { return &candidate == &note; });
  if (it == notes.end())
    return;

  const auto &chunkRanges = project->getAudioData().segmentChunkRanges;
  const int noteMidFrame = note.getStartFrame() +
                           std::max(0, note.getDurationFrames()) / 2;
  int regionStartFrame = std::numeric_limits<int>::min();
  int regionEndFrame = std::numeric_limits<int>::max();
  for (const auto &range : chunkRanges)
  {
    if (noteMidFrame >= range.first && noteMidFrame < range.second)
    {
      regionStartFrame = range.first;
      regionEndFrame = range.second;
      break;
    }
  }

  auto isInSameRegion = [&](const Note &candidate)
  {
    if (chunkRanges.empty())
      return true;

    const int candidateMidFrame =
        candidate.getStartFrame() +
        std::max(0, candidate.getDurationFrames()) / 2;
    return candidateMidFrame >= regionStartFrame &&
           candidateMidFrame < regionEndFrame;
  };

  Note *startNote = &note;
  Note *endNote = &note;

  for (auto prev = it; prev != notes.begin();)
  {
    --prev;
    if (chunkRanges.empty() || prev->getEndFrame() > regionStartFrame)
    {
      if (!prev->isRest() && isInSameRegion(*prev))
      {
        startNote = &(*prev);
        break;
      }
    }
    else
    {
      break;
    }
  }

  for (auto next = std::next(it); next != notes.end(); ++next)
  {
    if (!chunkRanges.empty() && next->getStartFrame() >= regionEndFrame)
      break;

    if (!next->isRest() && isInSameRegion(*next))
    {
      endNote = &(*next);
      break;
    }
  }

  const int startFrame = std::max(0, startNote->getStartFrame());
  const int endFrame = std::max(startFrame, endNote->getEndFrame());
  setPreviewPlaybackState(endFrame > startFrame, startFrame, endFrame);

  if (endFrame > startFrame && onPreviewRegionRequested)
    onPreviewRegionRequested(startFrame, endFrame);
}

void PianoRollComponent::setPreviewPlaybackState(bool active, int startFrame,
                                                 int endFrame)
{
  previewPlaybackActive = active && endFrame > startFrame;
  previewPlaybackStartFrame = previewPlaybackActive ? startFrame : 0;
  previewPlaybackEndFrame = previewPlaybackActive ? endFrame : 0;
  previewPlaybackCurrentTime =
      previewPlaybackActive ? framesToSeconds(startFrame) : 0.0;
  repaint();
}

void PianoRollComponent::setPreviewPlaybackPosition(double timeSeconds)
{
  if (!previewPlaybackActive)
    return;

  if (std::abs(previewPlaybackCurrentTime - timeSeconds) < 0.0001)
    return;

  previewPlaybackCurrentTime = timeSeconds;
  repaint();
}

void PianoRollComponent::setHoveredNote(Note *note)
{
  if (hoveredNote == note)
    return;

  hoveredNote = note;
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::updateScrollBars()
{
  if (project)
  {
    float totalWidth =
        static_cast<float>(getTimelineDuration()) * pixelsPerSecond;
    float totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

    int visibleWidth = getVisibleContentWidth();
    int visibleHeight = getVisibleContentHeight();
    const double maxScrollX =
        std::max(0.0, static_cast<double>(totalWidth) -
                          static_cast<double>(visibleWidth));
    scrollX = juce::jlimit(0.0, maxScrollX, scrollX);
    coordMapper->setScrollX(scrollX);

    horizontalScrollBar.setRangeLimits(0, totalWidth);
    horizontalScrollBar.setCurrentRange(scrollX, visibleWidth);

    verticalScrollBar.setRangeLimits(0, totalHeight);
    verticalScrollBar.setCurrentRange(scrollY, visibleHeight);
  }
}

double PianoRollComponent::getTimelineDuration() const
{
  const double projectDuration =
      project ? static_cast<double>(project->getAudioData().getDuration()) : 0.0;
  double timelineDuration = std::max(projectDuration, liveTimelineEndSeconds);

  // ARA region projects contain a playback range; give the editor one full bar
  // of writable-looking canvas after the real project end without extending
  // the underlying audio or playback range.
  if (project != nullptr &&
      !project->getAudioData().playbackRegionRanges.empty())
    timelineDuration += getTimelineBarSeconds();

  return timelineDuration;
}

void PianoRollComponent::reapplyBasePitchForNote(Note *note)
{
  if (!note || !project)
    return;

  auto &audioData = project->getAudioData();
  int startFrame = note->getStartFrame();
  int endFrame = note->getEndFrame();
  int f0Size = static_cast<int>(audioData.f0.size());

  // Reapply base + delta from dense curves
  for (int i = startFrame; i < endFrame && i < f0Size; ++i)
  {
    float base = (i < static_cast<int>(audioData.basePitch.size()))
                     ? audioData.basePitch[static_cast<size_t>(i)]
                     : 0.0f;
    float delta = (i < static_cast<int>(audioData.deltaPitch.size()))
                      ? audioData.deltaPitch[static_cast<size_t>(i)]
                      : 0.0f;
    audioData.f0[i] = midiToFreq(base + delta);
  }

  // Always set F0 dirty range for synthesis (needed for undo/redo to trigger
  // resynthesis)
  int smoothStart = std::max(0, startFrame - 60);
  int smoothEnd = std::min(f0Size, endFrame + 60);
  project->setF0DirtyRange(smoothStart, smoothEnd);

  // Trigger repaint
  repaint();
}

void PianoRollComponent::cancelDrawing()
{
  if (drawHandler_)
    drawHandler_->cancel();
}

void PianoRollComponent::drawSelectionRect(juce::Graphics &g)
{
  if (!boxSelector || !boxSelector->isSelecting())
    return;

  auto rect = boxSelector->getSelectionRect();

  // Draw semi-transparent fill
  g.setColour(juce::Colours::white.withAlpha(0.05f));
  g.fillRect(rect);

  // Draw border
  g.setColour(juce::Colours::white.withAlpha(0.25f));
  g.drawRect(rect, 1.0f);
}
