#include "PianoRollComponent.h"
#include "../Utils/BasePitchCurve.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/TimecodeFont.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/ScaleUtils.h"
#include "PianoRoll/States/LoopDragHandler.h"
#include "PianoRoll/States/SelectHandler.h"
#include "PianoRoll/States/DrawHandler.h"
#if PITCHNET_ENABLE_STRETCH
#include "PianoRoll/States/StretchHandler.h"
#endif
#include "PianoRoll/States/SplitHandler.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace
{
  juce::Colour getScaleAccentColour(ScaleMode mode)
  {
    switch (mode)
    {
    case ScaleMode::None:
      return APP_COLOR_PRIMARY;
    case ScaleMode::Major:
      return juce::Colour(0xFF74A9FFu);
    case ScaleMode::Minor:
      return juce::Colour(0xFFB689FFu);
    case ScaleMode::Dorian:
      return juce::Colour(0xFF5BD0C0u);
    case ScaleMode::Phrygian:
      return juce::Colour(0xFFFF8C77u);
    case ScaleMode::Lydian:
      return juce::Colour(0xFFFFD166u);
    case ScaleMode::Mixolydian:
      return juce::Colour(0xFF75D0FFu);
    case ScaleMode::Locrian:
      return juce::Colour(0xFF9FA9BFu);
    case ScaleMode::Chromatic:
      return APP_COLOR_PRIMARY;
    }
    return APP_COLOR_PRIMARY;
  }

  bool isBlackKey(int noteInOctave)
  {
    return noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
           noteInOctave == 8 || noteInOctave == 10;
  }

  enum class ScaleToneState
  {
    Root,
    InScale,
    OutOfScale
  };

  ScaleToneState getScaleToneState(ScaleMode mode, int noteInOctave, int rootNote)
  {
    if (mode == ScaleMode::None || rootNote < 0)
      return ScaleToneState::InScale;

    const int normalizedNote = (noteInOctave % 12 + 12) % 12;
    const int normalizedRoot = (rootNote % 12 + 12) % 12;

    if (mode == ScaleMode::Chromatic)
      return ScaleToneState::InScale;
    if (normalizedNote == normalizedRoot)
      return ScaleToneState::Root;
    if (ScaleUtils::isPitchClassInScale(mode, normalizedNote, normalizedRoot))
      return ScaleToneState::InScale;
    return ScaleToneState::OutOfScale;
  }

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

  bool isMultipleOf(double value, double step)
  {
    if (step <= 0.0)
      return false;
    const double ratio = value / step;
    return std::abs(ratio - std::round(ratio)) < 1.0e-4;
  }
}

PianoRollComponent::PianoRollComponent()
{
  // Initialize modular components
  coordMapper = std::make_unique<CoordinateMapper>();
  renderer = std::make_unique<PianoRollRenderer>();
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
#if PITCHNET_ENABLE_STRETCH
  stretchHandler_ = std::make_unique<StretchHandler>(*this);
#endif
  splitHandler_ = std::make_unique<SplitHandler>(*this);
  currentHandler_ = selectHandler_.get();

  // Wire up components
  renderer->setCoordinateMapper(coordMapper.get());
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
  auto thumbColor = APP_COLOR_PRIMARY.withAlpha(0.8f);
  auto trackColor = juce::Colours::transparentBlack;

  horizontalScrollBar.setColour(juce::ScrollBar::thumbColourId, thumbColor);
  horizontalScrollBar.setColour(juce::ScrollBar::trackColourId, trackColor);
  verticalScrollBar.setColour(juce::ScrollBar::thumbColourId, thumbColor);
  verticalScrollBar.setColour(juce::ScrollBar::trackColourId, trackColor);

  // Set initial scroll range
  verticalScrollBar.setRangeLimits(0, (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) *
                                          pixelsPerSemitone);
  verticalScrollBar.setCurrentRange(0, 500);

  // Default view centered on C3-C4 (MIDI 48-60)
  centerOnPitchRange(48.0f, 60.0f);

  // Enable keyboard focus for shortcuts
  setWantsKeyboardFocus(true);

  // No extra controls here; overview lives outside the piano roll.
}

PianoRollComponent::~PianoRollComponent()
{
  horizontalScrollBar.removeListener(this);
  verticalScrollBar.removeListener(this);
}

int PianoRollComponent::getVisibleContentWidth() const
{
  return std::max(0, getWidth() - pianoKeysWidth - 14);
}

int PianoRollComponent::getVisibleContentHeight() const
{
  return std::max(0, getHeight() - headerHeight - 14);
}

void PianoRollComponent::paint(juce::Graphics &g)
{
  updatePitchToolHandlesFromSelection();

  // Apply rounded corner clipping
  const float cornerRadius = 8.0f;
  juce::Path clipPath;
  clipPath.addRoundedRectangle(getLocalBounds().toFloat(), cornerRadius);
  g.reduceClipRegion(clipPath);

  // Background (solid to keep grid clean)
  g.fillAll(APP_COLOR_BACKGROUND);

  constexpr int scrollBarSize = 8;
  auto contentBounds = getLocalBounds();

  // Create clipping region for main area (below timelines)
  auto mainArea = contentBounds
                      .withTrimmedLeft(pianoKeysWidth)
                      .withTrimmedTop(headerHeight)
                      .withTrimmedBottom(scrollBarSize)
                      .withTrimmedRight(scrollBarSize);

  // Draw background waveform (only horizontal scroll, fills visible height)
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    drawBackgroundWaveform(g, mainArea);
  }

  // Draw scrolled content (grid, notes, pitch curves, handles)
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    g.setOrigin(pianoKeysWidth - static_cast<int>(scrollX),
                headerHeight - static_cast<int>(scrollY));

    drawGrid(g);
    drawGameChunksDebugOverlay(g);
    drawLoopOverlay(g);
    drawNotes(g, NoteRenderPass::Body);
    drawPitchCurves(g);
    drawNotes(g, NoteRenderPass::Overlay);
#if PITCHNET_ENABLE_STRETCH
    drawStretchGuides(g);
#endif
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
    float cursorTop = 0.0f;
    float cursorBottom =
        static_cast<float>(getHeight() - scrollBarSize); // Exclude scrollbar

    // Only draw if cursor is in visible area
    if (x >= pianoKeysWidth && x < getWidth() - scrollBarSize)
    {
      g.setColour(APP_COLOR_PRIMARY);
      g.fillRect(x - 0.5f, cursorTop, 1.0f, cursorBottom);

      // Draw triangle playhead indicator at top of timeline
      constexpr float triangleWidth = 10.0f;
      constexpr float triangleHeight = 8.0f;
      juce::Path triangle;
      triangle.addTriangle(x - triangleWidth * 0.5f, 0.0f, // Top-left
                           x + triangleWidth * 0.5f, 0.0f, // Top-right
                           x, triangleHeight               // Bottom-center (pointing down)
      );
      g.fillPath(triangle);
    }
  }

  // Draw piano keys
  drawPianoKeys(g);
}

void PianoRollComponent::resized()
{
  auto bounds = getLocalBounds();
  constexpr int scrollBarSize = 8;

  horizontalScrollBar.setBounds(
      pianoKeysWidth, bounds.getHeight() - scrollBarSize,
      bounds.getWidth() - pianoKeysWidth - scrollBarSize, scrollBarSize);

  verticalScrollBar.setBounds(
      bounds.getWidth() - scrollBarSize, headerHeight, scrollBarSize,
      bounds.getHeight() - scrollBarSize - headerHeight);

  updateScrollBars();
}

void PianoRollComponent::drawBackgroundWaveform(
    juce::Graphics &g, const juce::Rectangle<int> &visibleArea)
{
  if (!project)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.waveform.getNumSamples() == 0)
    return;

  // Check if we can use cached waveform
  bool cacheValid = waveformCache.isValid() &&
                    std::abs(cachedScrollX - scrollX) < 1.0 &&
                    std::abs(cachedPixelsPerSecond - pixelsPerSecond) < 0.01f &&
                    cachedWidth == visibleArea.getWidth() &&
                    cachedHeight == visibleArea.getHeight();

  if (cacheValid)
  {
    g.drawImageAt(waveformCache, visibleArea.getX(), visibleArea.getY());
    return;
  }

  // Render waveform to cache
  waveformCache = juce::Image(juce::Image::ARGB, visibleArea.getWidth(),
                              visibleArea.getHeight(), true);
  juce::Graphics cacheGraphics(waveformCache);

  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();

  // Draw waveform filling the visible area height
  float visibleHeight = static_cast<float>(visibleArea.getHeight());
  float centerY = visibleHeight * 0.5f;
  float waveformHeight = visibleHeight * 0.8f;

  juce::Path waveformPath;
  int visibleWidth = visibleArea.getWidth();

  waveformPath.startNewSubPath(0.0f, centerY);

  // Draw only the visible portion
  for (int px = 0; px < visibleWidth; ++px)
  {
    double time = (scrollX + px) / pixelsPerSecond;
    int startSample = static_cast<int>(time * SAMPLE_RATE);
    int endSample =
        static_cast<int>((time + 1.0 / pixelsPerSecond) * SAMPLE_RATE);

    startSample = std::max(0, std::min(startSample, numSamples - 1));
    endSample = std::max(startSample + 1, std::min(endSample, numSamples));

    float maxVal = 0.0f;
    for (int i = startSample; i < endSample; ++i)
      maxVal = std::max(maxVal, std::abs(samples[i]));

    float y = centerY - maxVal * waveformHeight * 0.5f;
    waveformPath.lineTo(static_cast<float>(px), y);
  }

  // Bottom half (reverse)
  for (int px = visibleWidth - 1; px >= 0; --px)
  {
    double time = (scrollX + px) / pixelsPerSecond;
    int startSample = static_cast<int>(time * SAMPLE_RATE);
    int endSample =
        static_cast<int>((time + 1.0 / pixelsPerSecond) * SAMPLE_RATE);

    startSample = std::max(0, std::min(startSample, numSamples - 1));
    endSample = std::max(startSample + 1, std::min(endSample, numSamples));

    float maxVal = 0.0f;
    for (int i = startSample; i < endSample; ++i)
      maxVal = std::max(maxVal, std::abs(samples[i]));

    float y = centerY + maxVal * waveformHeight * 0.5f;
    waveformPath.lineTo(static_cast<float>(px), y);
  }

  waveformPath.closeSubPath();

  cacheGraphics.setColour(APP_COLOR_WAVEFORM);
  cacheGraphics.fillPath(waveformPath);

  // Update cache metadata
  cachedScrollX = scrollX;
  cachedPixelsPerSecond = pixelsPerSecond;
  cachedWidth = visibleArea.getWidth();
  cachedHeight = visibleArea.getHeight();

  // Draw cached image
  g.drawImageAt(waveformCache, visibleArea.getX(), visibleArea.getY());
}

void PianoRollComponent::drawGrid(juce::Graphics &g)
{
  const float duration = project ? project->getAudioData().getDuration() : 60.0f;
  const float width =
      std::max(duration * pixelsPerSecond, static_cast<float>(getWidth()));
  const float height = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  // Only draw the visible area to avoid spending time on off-screen rows/columns.
  const float visibleStartX = juce::jlimit(0.0f, width, static_cast<float>(scrollX));
  const float visibleEndX = juce::jlimit(
      0.0f, width, visibleStartX + static_cast<float>(getVisibleContentWidth()) + 2.0f);
  const float visibleTopY = juce::jlimit(0.0f, height, static_cast<float>(scrollY));
  const float visibleBottomY = juce::jlimit(
      0.0f, height,
      visibleTopY + static_cast<float>(getVisibleContentHeight()) + pixelsPerSemitone);

  if (visibleEndX <= visibleStartX || visibleBottomY <= visibleTopY)
    return;

  const int visibleTopMidi = juce::jlimit(
      MIN_MIDI_NOTE, MAX_MIDI_NOTE,
      static_cast<int>(std::ceil(yToMidi(visibleTopY))));
  const int visibleBottomMidi = juce::jlimit(
      MIN_MIDI_NOTE, MAX_MIDI_NOTE,
      static_cast<int>(std::floor(yToMidi(visibleBottomY))));
  const int startMidi = juce::jlimit(MIN_MIDI_NOTE, MAX_MIDI_NOTE,
                                     visibleBottomMidi - 1);
  const int endMidi = juce::jlimit(MIN_MIDI_NOTE, MAX_MIDI_NOTE,
                                   visibleTopMidi + 1);

  const ScaleMode activeScaleMode = previewScaleMode.value_or(selectedScaleMode);
  const int activeScaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  const bool showScaleOverlay =
      showScaleColors && activeScaleMode != ScaleMode::None &&
      activeScaleMode != ScaleMode::Chromatic &&
      activeScaleRootNote >= 0;
  const juce::Colour scaleAccent = getScaleAccentColour(activeScaleMode);

  if (showScaleOverlay)
  {
    const auto rootRowColour = scaleAccent.withAlpha(0.24f);
    const auto inScaleRowColour = scaleAccent.withAlpha(0.08f);
    const auto outOfScaleRowColour = juce::Colours::black.withAlpha(0.20f);

    for (int midi = startMidi; midi <= endMidi; ++midi)
    {
      const int noteInOctave = (midi % 12 + 12) % 12;
      const auto toneState =
          getScaleToneState(activeScaleMode, noteInOctave, activeScaleRootNote);
      g.setColour(toneState == ScaleToneState::Root
                      ? rootRowColour
                      : (toneState == ScaleToneState::InScale ? inScaleRowColour
                                                              : outOfScaleRowColour));
      const float y = midiToY(static_cast<float>(midi));
      g.fillRect(visibleStartX, y, visibleEndX - visibleStartX, pixelsPerSemitone);
    }
  }

  if (!showScaleOverlay)
  {
    // Chromatic mode keeps the traditional piano black-key shading.
    g.setColour(APP_COLOR_SELECTION_OVERLAY);
    for (int midi = startMidi; midi <= endMidi; ++midi)
    {
      const int noteInOctave = (midi % 12 + 12) % 12;
      if (isBlackKey(noteInOctave))
      {
        const float y = midiToY(static_cast<float>(midi));
        g.fillRect(visibleStartX, y, visibleEndX - visibleStartX, pixelsPerSemitone);
      }
    }
  }

  // Horizontal pitch lines.
  for (int midi = startMidi; midi <= endMidi; ++midi)
  {
    const float y = midiToY(static_cast<float>(midi));
    const int noteInOctave = (midi % 12 + 12) % 12;

    if (!showScaleOverlay)
    {
      g.setColour(noteInOctave == 0 ? APP_COLOR_GRID_BAR : APP_COLOR_GRID);
    }
    else if (getScaleToneState(activeScaleMode, noteInOctave,
                               activeScaleRootNote) == ScaleToneState::Root)
    {
      g.setColour(scaleAccent.withAlpha(0.70f));
    }
    else if (getScaleToneState(activeScaleMode, noteInOctave,
                               activeScaleRootNote) == ScaleToneState::InScale)
    {
      g.setColour(APP_COLOR_GRID.interpolatedWith(scaleAccent, 0.40f));
    }
    else
    {
      g.setColour(APP_COLOR_GRID.darker(0.25f));
    }

    g.drawHorizontalLine(static_cast<int>(y), visibleStartX, visibleEndX);
  }

  if (timelineDisplayMode == TimelineDisplayMode::Beats)
  {
    const double gridSeconds = getTimelineGridSeconds();
    const double beatSeconds = getTimelineBeatSeconds();
    const double barSeconds = getTimelineBarSeconds();
    if (gridSeconds > 1.0e-6 && beatSeconds > 1.0e-6 && barSeconds > 1.0e-6)
    {
      const double visibleStartTime = visibleStartX / pixelsPerSecond;
      const double visibleEndTime = visibleEndX / pixelsPerSecond;
      const int firstGrid = std::max(
          0, static_cast<int>(std::floor(visibleStartTime / gridSeconds)) - 1);

      for (int i = firstGrid;; ++i)
      {
        const double time = static_cast<double>(i) * gridSeconds;
        if (time > visibleEndTime + gridSeconds)
          break;

        const float x = static_cast<float>(time * pixelsPerSecond);
        if (x < visibleStartX - 1.0f || x > visibleEndX + 1.0f)
          continue;

        if (isMultipleOf(time, barSeconds))
        {
          g.setColour(APP_COLOR_GRID_BAR);
        }
        else if (isMultipleOf(time, beatSeconds))
        {
          g.setColour(showScaleOverlay
                          ? APP_COLOR_GRID.interpolatedWith(scaleAccent, 0.20f)
                          : APP_COLOR_GRID);
        }
        else
        {
          g.setColour(APP_COLOR_GRID.darker(0.2f));
        }
        g.drawVerticalLine(static_cast<int>(x), visibleTopY, visibleBottomY);
      }
    }
  }
  else
  {
    // Time mode keeps simple second-based spacing.
    const float secondsPerLine = pixelsPerSecond >= 180.0f  ? 0.25f
                                 : pixelsPerSecond >= 90.0f ? 0.5f
                                 : pixelsPerSecond >= 45.0f ? 1.0f
                                 : pixelsPerSecond >= 22.0f ? 2.0f
                                                            : 5.0f;
    const float pixelsPerLine = secondsPerLine * pixelsPerSecond;
    if (pixelsPerLine > 1.0e-4f)
    {
      g.setColour(showScaleOverlay
                      ? APP_COLOR_GRID.interpolatedWith(scaleAccent, 0.20f)
                      : APP_COLOR_GRID);
      const int firstLine =
          std::max(0, static_cast<int>(std::floor(visibleStartX / pixelsPerLine)));
      for (float x = firstLine * pixelsPerLine; x <= visibleEndX; x += pixelsPerLine)
        g.drawVerticalLine(static_cast<int>(x), visibleTopY, visibleBottomY);
    }
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

  const float startX = timeToX(loopStartSeconds);
  const float endX = timeToX(loopEndSeconds);

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const auto baseColor = APP_COLOR_PRIMARY;
  const auto fillColor =
      loopEnabled ? baseColor.withAlpha(0.08f) : baseColor.withAlpha(0.04f);

  g.setColour(fillColor);
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
  constexpr int scrollBarSize = 8;
  auto timelineArea = juce::Rectangle<int>(
      pianoKeysWidth, 0, getWidth() - pianoKeysWidth - scrollBarSize,
      timelineHeight);

  // Background
  g.setColour(APP_COLOR_TIMELINE);
  g.fillRect(timelineArea);

  // Bottom border
  g.setColour(APP_COLOR_GRID_BAR);
  g.drawHorizontalLine(timelineHeight - 1, static_cast<float>(pianoKeysWidth),
                       static_cast<float>(getWidth() - scrollBarSize));

  const float duration = project ? project->getAudioData().getDuration() : 60.0f;
  g.setFont(TimecodeFont::getBoldFont(12.0f));

  if (timelineDisplayMode == TimelineDisplayMode::Beats)
  {
    const double beatSeconds = getTimelineBeatSeconds();
    const double barSeconds = getTimelineBarSeconds();
    if (beatSeconds > 1.0e-6 && barSeconds > 1.0e-6)
    {
      const int beatsPerBar = juce::jmax(1, timelineBeatNumerator);
      const float pixelsPerBeat = static_cast<float>(beatSeconds * pixelsPerSecond);
      int beatStep = 1;
      while (pixelsPerBeat * static_cast<float>(beatStep) < 20.0f && beatStep < 64)
        beatStep *= 2;

      const int firstBeat = std::max(
          0, static_cast<int>(std::floor((scrollX / pixelsPerSecond) / beatSeconds)));
      const int lastBeat = static_cast<int>(
                               std::ceil((scrollX + timelineArea.getWidth()) / pixelsPerSecond / beatSeconds)) +
                           beatStep;

      for (int beatIndex = firstBeat; beatIndex <= lastBeat; beatIndex += beatStep)
      {
        const double time = static_cast<double>(beatIndex) * beatSeconds;
        if (time > duration + beatSeconds)
          break;

        const float x =
            pianoKeysWidth + static_cast<float>(time * pixelsPerSecond) - static_cast<float>(scrollX);
        if (x < pianoKeysWidth - 50 || x > getWidth())
          continue;

        const bool isBarLine = (beatIndex % beatsPerBar) == 0;
        const int tickHeight = isBarLine ? 9 : 4;
        g.setColour(isBarLine ? APP_COLOR_GRID_BAR : APP_COLOR_GRID);
        g.drawVerticalLine(static_cast<int>(x),
                           static_cast<float>(timelineHeight - tickHeight),
                           static_cast<float>(timelineHeight - 1));

        if (isBarLine)
        {
          const int bar = beatIndex / beatsPerBar + 1;
          g.setColour(APP_COLOR_TEXT_MUTED);
          g.drawText("Bar " + juce::String(bar), static_cast<int>(x) + 3, 2, 64,
                     timelineHeight - 4, juce::Justification::centredLeft, false);
        }
        else if (beatStep == 1 && pixelsPerBeat >= 58.0f)
        {
          const int beatInBar = (beatIndex % beatsPerBar) + 1;
          const int bar = beatIndex / beatsPerBar + 1;
          g.setColour(APP_COLOR_TEXT_MUTED.withAlpha(0.8f));
          g.drawText(juce::String::formatted("%d.%d", bar, beatInBar),
                     static_cast<int>(x) + 3, 2, 48, timelineHeight - 4,
                     juce::Justification::centredLeft, false);
        }
      }
      return;
    }
  }

  // Time mode labels/ticks.
  float secondsPerTick;
  if (pixelsPerSecond >= 200.0f)
    secondsPerTick = 0.5f;
  else if (pixelsPerSecond >= 100.0f)
    secondsPerTick = 1.0f;
  else if (pixelsPerSecond >= 50.0f)
    secondsPerTick = 2.0f;
  else if (pixelsPerSecond >= 25.0f)
    secondsPerTick = 5.0f;
  else
    secondsPerTick = 10.0f;

  for (float time = 0.0f; time <= duration + secondsPerTick; time += secondsPerTick)
  {
    float x =
        pianoKeysWidth + time * pixelsPerSecond - static_cast<float>(scrollX);

    if (x < pianoKeysWidth - 50 || x > getWidth())
      continue;

    bool isMajor = std::fmod(time, secondsPerTick * 2.0f) < 0.001f;
    int tickHeight = isMajor ? 8 : 4;

    g.setColour(APP_COLOR_GRID_BAR);
    g.drawVerticalLine(static_cast<int>(x),
                       static_cast<float>(timelineHeight - tickHeight),
                       static_cast<float>(timelineHeight - 1));

    if (isMajor)
    {
      int minutes = static_cast<int>(time) / 60;
      int seconds = static_cast<int>(time) % 60;
      int tenths = static_cast<int>((time - std::floor(time)) * 10);

      juce::String label;
      if (minutes > 0)
        label = juce::String::formatted("%d:%02d", minutes, seconds);
      else if (secondsPerTick < 1.0f)
        label = juce::String::formatted("%d.%d", seconds, tenths);
      else
        label = juce::String::formatted("%ds", seconds);

      g.setColour(APP_COLOR_TEXT_MUTED);
      g.drawText(label, static_cast<int>(x) + 3, 2, 50, timelineHeight - 4,
                 juce::Justification::centredLeft, false);
    }
  }
}

void PianoRollComponent::drawLoopTimeline(juce::Graphics &g)
{
  constexpr int scrollBarSize = 8;
  auto loopArea = juce::Rectangle<int>(
      pianoKeysWidth, timelineHeight,
      getWidth() - pianoKeysWidth - scrollBarSize, loopTimelineHeight);

  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRect(loopArea);

  g.setColour(APP_COLOR_GRID_BAR);
  g.drawHorizontalLine(headerHeight - 1,
                       static_cast<float>(pianoKeysWidth),
                       static_cast<float>(getWidth() - scrollBarSize));

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

  const float startX =
      static_cast<float>(pianoKeysWidth) + timeToX(loopStartSeconds) -
      static_cast<float>(scrollX);
  const float endX =
      static_cast<float>(pianoKeysWidth) + timeToX(loopEndSeconds) -
      static_cast<float>(scrollX);

  auto range = juce::Rectangle<float>(
      startX, static_cast<float>(timelineHeight), endX - startX,
      static_cast<float>(loopTimelineHeight));

  const auto baseColor = APP_COLOR_PRIMARY;
  const auto fillColor =
      loopEnabled ? baseColor.withAlpha(0.25f) : baseColor.withAlpha(0.12f);
  const auto edgeColor =
      loopEnabled ? baseColor : APP_COLOR_BORDER;

  g.setColour(fillColor);
  g.fillRect(range);

  g.setColour(edgeColor);
  g.drawLine(startX, static_cast<float>(timelineHeight), startX,
             static_cast<float>(headerHeight - 1), 1.5f);
  g.drawLine(endX, static_cast<float>(timelineHeight), endX,
             static_cast<float>(headerHeight - 1), 1.5f);

  constexpr float flagWidth = 6.0f;
  constexpr float flagHeight = 6.0f;
  constexpr float flagTop = 0.0f;

  const float flagY = static_cast<float>(timelineHeight) + flagTop;

  juce::Path startFlag;
  startFlag.addTriangle(startX, flagY, startX, flagY + flagHeight,
                        startX - flagWidth, flagY + flagHeight);
  g.fillPath(startFlag);

  juce::Path endFlag;
  endFlag.addTriangle(endX, flagY, endX, flagY + flagHeight,
                      endX + flagWidth, flagY + flagHeight);
  g.fillPath(endFlag);
}

void PianoRollComponent::drawNotes(juce::Graphics &g, NoteRenderPass pass)
{
  if (!project)
    return;

  const bool drawBodies = pass == NoteRenderPass::Body;
  const bool drawOverlays = pass == NoteRenderPass::Overlay;

  // Pre-allocated scratch buffers to avoid per-note heap allocations
  std::vector<float> waveValues;
  std::vector<float> smoothed;
  waveValues.reserve(2048);
  smoothed.reserve(2048);

  const bool isMultiDragging = pitchEditor && pitchEditor->isDraggingMultiNotes();
  const std::vector<Note *> *draggedNotes =
      isMultiDragging ? &pitchEditor->getDraggedNotes() : nullptr;

  auto drawSelectedNoteOutline = [&g](float x, float y, float w, float h)
  {
    constexpr float localOutlinePadding = 2.0f;
    constexpr float outlineThickness = 1.5f;
    constexpr float outlineCornerRadius = 3.5f;

    g.setColour(APP_COLOR_PRIMARY.withAlpha(0.95f));
    g.drawRoundedRectangle(
        x - localOutlinePadding, y - localOutlinePadding,
        w + localOutlinePadding * 2.0f, h + localOutlinePadding * 2.0f,
        outlineCornerRadius, outlineThickness);
  };
  auto getDeltaScaleHandleBounds = [](float x, float y, float w,
                                      float h) -> juce::Rectangle<float>
  {
    constexpr float localOutlinePadding = 2.0f;
    constexpr float localHandleWidth = 18.0f;
    constexpr float localHandleHeight = 10.0f;
    constexpr float localHandleGap = 4.0f;
    constexpr float localHandleSpacing = 6.0f;
    const float centerX = x + w * 0.5f;
    const float groupWidth = localHandleWidth * 2.0f + localHandleSpacing;
    const float groupLeft = centerX - groupWidth * 0.5f;
    const float handleX = groupLeft;
    const float handleY =
        y + h + localOutlinePadding + localHandleGap;
    return {handleX, handleY, localHandleWidth, localHandleHeight};
  };
  auto getDeltaOffsetHandleBounds = [](float x, float y, float w,
                                       float h) -> juce::Rectangle<float>
  {
    constexpr float localOutlinePadding = 2.0f;
    constexpr float localHandleWidth = 18.0f;
    constexpr float localHandleHeight = 10.0f;
    constexpr float localHandleGap = 4.0f;
    constexpr float localHandleSpacing = 6.0f;
    const float centerX = x + w * 0.5f;
    const float groupWidth = localHandleWidth * 2.0f + localHandleSpacing;
    const float groupLeft = centerX - groupWidth * 0.5f;
    const float handleX = groupLeft + localHandleWidth + localHandleSpacing;
    const float handleY =
        y + h + localOutlinePadding + localHandleGap;
    return {handleX, handleY, localHandleWidth, localHandleHeight};
  };

  const auto &audioData = project->getAudioData();
  const float *globalSamples =
      drawBodies && audioData.waveform.getNumSamples() > 0
          ? audioData.waveform.getReadPointer(0)
          : nullptr;
  int globalTotalSamples = drawBodies ? audioData.waveform.getNumSamples() : 0;

  // Calculate visible time range for culling
  double visibleStartTime = scrollX / pixelsPerSecond;
  double visibleEndTime = (scrollX + getWidth()) / pixelsPerSecond;

  for (auto &note : project->getNotes())
  {
    // Skip rest notes (they have no pitch)
    if (note.isRest())
      continue;

    // Viewport culling: skip notes outside visible area
    double noteStartTime = framesToSeconds(note.getStartFrame());
    double noteEndTime = framesToSeconds(note.getEndFrame());
    if (noteEndTime < visibleStartTime || noteStartTime > visibleEndTime)
      continue;

    float x = static_cast<float>(noteStartTime * pixelsPerSecond);
    float w = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
    float h = pixelsPerSemitone;
    const float renderedWidth = std::max(w, 4.0f);

    // Position at grid cell center for MIDI note, then offset by pitch
    // adjustment
    float baseGridCenterY =
        midiToY(note.getMidiNote()) + pixelsPerSemitone * 0.5f;
    float pitchOffsetPixels = -note.getPitchOffset() * pixelsPerSemitone;
    float y = baseGridCenterY + pitchOffsetPixels - h * 0.5f;

    if (drawBodies)
    {
      // Note color based on pitch
      juce::Colour noteColor = note.isSelected()
                                   ? APP_COLOR_NOTE_SELECTED
                                   : APP_COLOR_NOTE_NORMAL;

      const float *samples = globalSamples;
      int totalSamples = globalTotalSamples;
      int startSample = 0;
      int endSample = 0;
      const auto &clipWaveform = note.getClipWaveform();
      if (!clipWaveform.empty())
      {
        samples = clipWaveform.data();
        totalSamples = static_cast<int>(clipWaveform.size());
        startSample = 0;
        endSample = totalSamples;
      }
      else if (samples && totalSamples > 0)
      {
        startSample = static_cast<int>(framesToSeconds(note.getStartFrame()) *
                                       audioData.sampleRate);
        endSample = static_cast<int>(framesToSeconds(note.getEndFrame()) *
                                     audioData.sampleRate);
        startSample = std::max(0, std::min(startSample, totalSamples - 1));
        endSample = std::max(startSample + 1, std::min(endSample, totalSamples));
      }

      if (samples && totalSamples > 0 && w > 2.0f && endSample > startSample)
      {
        // Draw waveform slice inside note
        int numNoteSamples = endSample - startSample;
        int samplesPerPixel = std::max(1, static_cast<int>(numNoteSamples / w));

        float centerY = y + h * 0.5f;
        float waveHeight = h * 3.0f;

        // Build waveform data with increased resolution for smoother curves
        waveValues.clear();
        // Increase point density for smoother curves (up to 800 points)
        float step = std::max(0.5f, w / 1024.0f);

        for (float px = 0; px <= w; px += step)
        {
          int sampleIdx =
              startSample + static_cast<int>((px / w) * numNoteSamples);
          int sampleEnd = std::min(sampleIdx + samplesPerPixel, endSample);

          float maxVal = 0.0f;
          for (int i = sampleIdx; i < sampleEnd; ++i)
            maxVal = std::max(maxVal, std::abs(samples[i]));

          waveValues.push_back(maxVal);
        }

        // Apply smoothing filter to reduce aliasing artifacts
        if (waveValues.size() > 2)
        {
          smoothed.resize(waveValues.size());
          smoothed[0] = waveValues[0];
          for (size_t i = 1; i + 1 < waveValues.size(); ++i)
          {
            // Simple 3-point moving average for gentle smoothing
            smoothed[i] = (waveValues[i - 1] * 0.25f + waveValues[i] * 0.5f +
                           waveValues[i + 1] * 0.25f);
          }
          smoothed[waveValues.size() - 1] = waveValues[waveValues.size() - 1];
          waveValues = std::move(smoothed);
        }

        size_t numPoints = waveValues.size();
        if (numPoints < 2)
        {
          // Fallback for very short notes
          g.setColour(noteColor.withAlpha(0.85f));
          g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
        }
        else
        {
          // Helper function for Catmull-Rom spline interpolation
          auto catmullRom = [](float t, float p0, float p1, float p2,
                               float p3) -> float
          {
            // Catmull-Rom spline: smooth interpolation between p1 and p2
            float t2 = t * t;
            float t3 = t2 * t;
            return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                           (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                           (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
          };

          // Draw filled waveform using smooth curves
          g.setColour(noteColor.withAlpha(0.85f));
          juce::Path waveformPath;

          // Build top curve with Catmull-Rom spline
          waveformPath.startNewSubPath(
              x, centerY - waveValues[0] * waveHeight * 0.5f);

          // Use cubic curves for smooth interpolation
          const int curveSegments =
              4; // Interpolate 4 points between each pair
          for (size_t i = 0; i + 1 < numPoints; ++i)
          {
            float px1 = (static_cast<float>(i) /
                         static_cast<float>(numPoints - 1)) *
                        w;
            float px2 = (static_cast<float>(i + 1) /
                         static_cast<float>(numPoints - 1)) *
                        w;

            // Get control points for spline
            size_t idx0 = (i > 0) ? i - 1 : i;
            size_t idx1 = i;
            size_t idx2 = i + 1;
            size_t idx3 = (i + 2 < numPoints) ? i + 2 : i + 1;

            float val0 = waveValues[idx0];
            float val1 = waveValues[idx1];
            float val2 = waveValues[idx2];
            float val3 = waveValues[idx3];

            // Draw smooth curve segment
            for (int seg = 1; seg <= curveSegments; ++seg)
            {
              float t =
                  static_cast<float>(seg) / static_cast<float>(curveSegments);
              float px = px1 + (px2 - px1) * t;
              float val = catmullRom(t, val0, val1, val2, val3);
              float yPos = centerY - val * waveHeight * 0.5f;
              waveformPath.lineTo(x + px, yPos);
            }
          }

          // Build bottom curve (mirror of top)
          waveformPath.lineTo(x + w, centerY + waveValues[numPoints - 1] *
                                                   waveHeight * 0.5f);

          for (int i = static_cast<int>(numPoints) - 2; i >= 0; --i)
          {
            float px1 = (static_cast<float>(i + 1) /
                         static_cast<float>(numPoints - 1)) *
                        w;
            float px2 =
                (static_cast<float>(i) / static_cast<float>(numPoints - 1)) *
                w;

            size_t idx0 = (i + 2 < numPoints) ? i + 2 : i + 1;
            size_t idx1 = i + 1;
            size_t idx2 = i;
            size_t idx3 = (i > 0) ? i - 1 : i;

            float val0 = waveValues[idx0];
            float val1 = waveValues[idx1];
            float val2 = waveValues[idx2];
            float val3 = waveValues[idx3];

            for (int seg = 1; seg <= curveSegments; ++seg)
            {
              float t =
                  static_cast<float>(seg) / static_cast<float>(curveSegments);
              float px = px1 + (px2 - px1) * t;
              float val = catmullRom(t, val0, val1, val2, val3);
              float yPos = centerY + val * waveHeight * 0.5f;
              waveformPath.lineTo(x + px, yPos);
            }
          }

          waveformPath.closeSubPath();
          g.fillPath(waveformPath);

          // Reuse the same closed path for the outline stroke
          g.setColour(noteColor.brighter(0.2f));
          g.strokePath(waveformPath,
                       juce::PathStrokeType(1.2f,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
        }
      }
      else
      {
        // Fallback: simple rectangle for very short notes
        g.setColour(noteColor.withAlpha(0.85f));
        g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
      }
    }

    if (drawOverlays && note.isSelected())
    {
      drawSelectedNoteOutline(x, y, renderedWidth, h);

      const auto handleBounds =
          getDeltaScaleHandleBounds(x, y, renderedWidth, h);
      const bool handleActive =
          selectHandler_->getIsDeltaScaleDragging() &&
          std::find(selectHandler_->getDeltaScaleTargetNotes().begin(),
                    selectHandler_->getDeltaScaleTargetNotes().end(),
                    &note) != selectHandler_->getDeltaScaleTargetNotes().end();
      g.setColour(handleActive ? APP_COLOR_PRIMARY.brighter(0.1f)
                               : APP_COLOR_PRIMARY.withAlpha(0.9f));
      g.fillRoundedRectangle(handleBounds, 2.5f);
      g.setColour(juce::Colours::white.withAlpha(0.95f));
      g.drawRoundedRectangle(handleBounds, 2.5f, 1.0f);

      const float cx = handleBounds.getCentreX();
      const float top = handleBounds.getY() + 2.0f;
      const float bottom = handleBounds.getBottom() - 2.0f;
      g.drawLine(cx, top + 2.0f, cx, bottom - 2.0f, 1.0f);
      juce::Path upArrow;
      upArrow.addTriangle(cx, top, cx - 2.5f, top + 3.5f, cx + 2.5f, top + 3.5f);
      g.fillPath(upArrow);
      juce::Path downArrow;
      downArrow.addTriangle(cx, bottom, cx - 2.5f, bottom - 3.5f,
                            cx + 2.5f, bottom - 3.5f);
      g.fillPath(downArrow);

      if (selectHandler_->getIsDeltaScaleDragging() && selectHandler_->getDeltaScaleFactor() > 0.0f)
      {
        const juce::String factorText = "x" + juce::String(selectHandler_->getDeltaScaleFactor(), 2);
        const float infoW = 44.0f;
        const float infoH = 14.0f;
        const float infoX = handleBounds.getCentreX() - infoW * 0.5f;
        const float infoY = handleBounds.getBottom() + 2.0f;
        g.setColour(juce::Colours::black.withAlpha(0.72f));
        g.fillRoundedRectangle(infoX, infoY, infoW, infoH, 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.setFont(juce::FontOptions(10.0f));
        g.drawFittedText(factorText, static_cast<int>(infoX),
                         static_cast<int>(infoY), static_cast<int>(infoW),
                         static_cast<int>(infoH), juce::Justification::centred,
                         1);
      }

      const auto offsetHandleBounds =
          getDeltaOffsetHandleBounds(x, y, renderedWidth, h);
      const bool offsetHandleActive =
          selectHandler_->getIsDeltaOffsetDragging() &&
          std::find(selectHandler_->getDeltaOffsetTargetNotes().begin(),
                    selectHandler_->getDeltaOffsetTargetNotes().end(),
                    &note) != selectHandler_->getDeltaOffsetTargetNotes().end();
      g.setColour(offsetHandleActive ? APP_COLOR_PRIMARY.brighter(0.1f)
                                     : APP_COLOR_PRIMARY.withAlpha(0.9f));
      g.fillRoundedRectangle(offsetHandleBounds, 2.5f);
      g.setColour(juce::Colours::white.withAlpha(0.95f));
      g.drawRoundedRectangle(offsetHandleBounds, 2.5f, 1.0f);
      g.setFont(juce::FontOptions(9.0f));
      g.drawFittedText("+/-", static_cast<int>(offsetHandleBounds.getX()),
                       static_cast<int>(offsetHandleBounds.getY()),
                       static_cast<int>(offsetHandleBounds.getWidth()),
                       static_cast<int>(offsetHandleBounds.getHeight()),
                       juce::Justification::centred, 1);

      if (selectHandler_->getIsDeltaOffsetDragging())
      {
        const juce::String prefix = selectHandler_->getDeltaOffsetSemitones() >= 0.0f ? "+" : "";
        const juce::String offsetText =
            prefix + juce::String(selectHandler_->getDeltaOffsetSemitones(), 2) + " st";
        const float infoW = 56.0f;
        const float infoH = 14.0f;
        const float infoX = offsetHandleBounds.getCentreX() - infoW * 0.5f;
        const float infoY = offsetHandleBounds.getBottom() + 2.0f;
        g.setColour(juce::Colours::black.withAlpha(0.72f));
        g.fillRoundedRectangle(infoX, infoY, infoW, infoH, 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.setFont(juce::FontOptions(10.0f));
        g.drawFittedText(offsetText, static_cast<int>(infoX),
                         static_cast<int>(infoY), static_cast<int>(infoW),
                         static_cast<int>(infoH), juce::Justification::centred,
                         1);
      }
    }

    const bool isSingleDragged = selectHandler_->isSingleNoteDragging() && selectHandler_->getDraggedNote() == &note;
    const bool isMultiDragged =
        isMultiDragging && draggedNotes &&
        std::find(draggedNotes->begin(), draggedNotes->end(), &note) !=
            draggedNotes->end();
    if (drawOverlays && (isSingleDragged || isMultiDragged))
    {
      const float deltaSemitones = note.getPitchOffset();
      if (std::abs(deltaSemitones) >= 0.01f)
      {
        const juce::String prefix = deltaSemitones >= 0.0f ? "+" : "";
        const juce::String label =
            prefix + juce::String(deltaSemitones, 1) + " st";

        constexpr float labelHeight = 16.0f;
        constexpr float margin = 4.0f;
        const float labelWidth =
            std::max(44.0f, static_cast<float>(label.length()) * 7.2f);
        const float labelX = x + renderedWidth * 0.5f - labelWidth * 0.5f;
        const bool moveUp = deltaSemitones > 0.0f;
        const float labelY = moveUp ? (y - labelHeight - margin) : (y + h + margin);

        g.setColour(juce::Colours::black.withAlpha(0.72f));
        g.fillRoundedRectangle(labelX, labelY, labelWidth, labelHeight, 4.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(11.0f));
        g.drawFittedText(label, static_cast<int>(labelX),
                         static_cast<int>(labelY),
                         static_cast<int>(labelWidth),
                         static_cast<int>(labelHeight),
                         juce::Justification::centred, 1);
      }
    }
  }

  // Draw split guide line when in split mode and hovering over a note
  if (drawOverlays && editMode == EditMode::Split && splitHandler_ &&
      splitHandler_->getSplitGuideNote() &&
      splitHandler_->getSplitGuideX() >= 0)
  {
    auto *guideNote = splitHandler_->getSplitGuideNote();
    float guideX = splitHandler_->getSplitGuideX();
    float noteStartTime = framesToSeconds(guideNote->getStartFrame());
    float noteEndTime = framesToSeconds(guideNote->getEndFrame());
    float noteStartX = static_cast<float>(noteStartTime * pixelsPerSecond);
    float noteEndX = static_cast<float>(noteEndTime * pixelsPerSecond);

    // Only draw if guide is within note bounds (with margin)
    if (guideX > noteStartX + 5 && guideX < noteEndX - 5)
    {
      float noteY = midiToY(guideNote->getAdjustedMidiNote());
      float noteH = pixelsPerSemitone;

      // Draw dashed vertical line
      g.setColour(APP_COLOR_SECONDARY);
      float dashLength = 4.0f;
      for (float dy = 0; dy < noteH; dy += dashLength * 2)
      {
        float segmentLength = std::min(dashLength, noteH - dy);
        g.drawLine(guideX, noteY + dy, guideX,
                   noteY + dy + segmentLength, 2.0f);
      }
    }
  }
}

#if PITCHNET_ENABLE_STRETCH
void PianoRollComponent::drawStretchGuides(juce::Graphics &g)
{
  if (!project || editMode != EditMode::Stretch || !stretchHandler_)
    return;

  auto boundaries = stretchHandler_->collectStretchBoundaries();
  if (boundaries.empty())
    return;

  const auto &dragState = stretchHandler_->getDragState();
  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const int hoveredIdx = stretchHandler_->getHoveredBoundaryIndex();

  for (size_t i = 0; i < boundaries.size(); ++i)
  {
    int frame = boundaries[i].frame;
    const bool isActive =
        dragState.active && boundaries[i].left == dragState.boundary.left &&
        boundaries[i].right == dragState.boundary.right;
    if (isActive)
      frame = dragState.currentBoundary;

    float x = framesToSeconds(frame) * pixelsPerSecond;

    const bool isHovered = static_cast<int>(i) == hoveredIdx;
    float alpha = isHovered || isActive ? 0.8f : 0.35f;
    float thickness = isHovered || isActive ? 2.0f : 1.0f;

    g.setColour(APP_COLOR_PRIMARY.withAlpha(alpha));
    g.drawLine(x, 0.0f, x, height, thickness);
  }
}
#endif

void PianoRollComponent::drawPitchCurves(juce::Graphics &g)
{
  if (!project)
    return;

  // Hide pitch curves in Parameter mode to avoid visual clashing
  // with the HNSep overlay that occupies the same viewport area.
  if (editMode == EditMode::Parameter)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.f0.empty())
    return;

  // Get global pitch offset (applied to display only)
  float globalOffset = project->getGlobalPitchOffset();

  // Draw pitch curves per note with their pitch offsets applied (delta pitch)
  if (showDeltaPitch)
  {
    g.setColour(APP_COLOR_PITCH_CURVE);
    if (showUvInterpolationDebug)
    {
      const double visibleStartTime = scrollX / pixelsPerSecond;
      const double visibleEndTime = (scrollX + getWidth()) / pixelsPerSecond;
      const int visStartFrame = std::max(
          0,
          static_cast<int>(visibleStartTime * audioData.sampleRate / HOP_SIZE));
      const int visEndFrame = std::min(
          static_cast<int>(audioData.f0.size()),
          static_cast<int>(visibleEndTime * audioData.sampleRate / HOP_SIZE) + 1);

      const auto &chunkRanges = audioData.segmentChunkRanges;
      size_t chunkIdx = 0;

      juce::Path path;
      bool pathStarted = false;
      for (int i = visStartFrame; i < visEndFrame; ++i)
      {
        bool inChunk = true;
        if (!chunkRanges.empty())
        {
          while (chunkIdx < chunkRanges.size() &&
                 chunkRanges[chunkIdx].second <= i)
            ++chunkIdx;
          inChunk = chunkIdx < chunkRanges.size() &&
                    chunkRanges[chunkIdx].first <= i &&
                    chunkRanges[chunkIdx].second > i;
        }
        if (!inChunk)
        {
          pathStarted = false;
          continue;
        }

        float baseMidi =
            (i < static_cast<int>(audioData.basePitch.size()))
                ? audioData.basePitch[static_cast<size_t>(i)]
                : ((i < static_cast<int>(audioData.f0.size()) &&
                    audioData.f0[static_cast<size_t>(i)] > 0.0f)
                       ? freqToMidi(audioData.f0[static_cast<size_t>(i)])
                       : 0.0f);
        float deltaMidi = (i < static_cast<int>(audioData.deltaPitch.size()))
                              ? audioData.deltaPitch[static_cast<size_t>(i)]
                              : 0.0f;
        float finalMidi = baseMidi + deltaMidi + globalOffset;

        if (finalMidi <= 0.0f)
        {
          pathStarted = false;
          continue;
        }

        float x = framesToSeconds(i) * pixelsPerSecond;
        float y = midiToY(finalMidi) + pixelsPerSemitone * 0.5f;
        if (!pathStarted)
        {
          path.startNewSubPath(x, y);
          pathStarted = true;
        }
        else
        {
          path.lineTo(x, y);
        }
      }
      g.strokePath(path, juce::PathStrokeType(2.0f));
    }
    else
    {
      const bool useLiveBasePreview =
          (selectHandler_->isSingleNoteDragging() || pitchEditor->isDraggingMultiNotes());
      const auto &draggedNotes = pitchEditor->getDraggedNotes();

      for (const auto &note : project->getNotes())
      {
        if (note.isRest())
          continue;

        const bool isDraggedNote =
            (selectHandler_->isSingleNoteDragging() && selectHandler_->getDraggedNote() == &note) ||
            (pitchEditor->isDraggingMultiNotes() &&
             std::find(draggedNotes.begin(), draggedNotes.end(), &note) !=
                 draggedNotes.end());
        const bool applyNoteOffset = !(useLiveBasePreview && isDraggedNote);

        juce::Path path;
        bool pathStarted = false;

        int startFrame = note.getStartFrame();
        int endFrame =
            std::min(note.getEndFrame(), static_cast<int>(audioData.f0.size()));

        for (int i = startFrame; i < endFrame; ++i)
        {
          float baseMidi =
              (i < static_cast<int>(audioData.basePitch.size()))
                  ? audioData.basePitch[static_cast<size_t>(i)]
                  : ((i < static_cast<int>(audioData.f0.size()) &&
                      audioData.f0[static_cast<size_t>(i)] > 0.0f)
                         ? freqToMidi(audioData.f0[static_cast<size_t>(i)])
                         : 0.0f);
          if (applyNoteOffset)
            baseMidi += note.getPitchOffset();

          float deltaMidi = (i < static_cast<int>(audioData.deltaPitch.size()))
                                ? audioData.deltaPitch[static_cast<size_t>(i)]
                                : 0.0f;
          float finalMidi = baseMidi + deltaMidi + globalOffset;

          if (finalMidi > 0.0f)
          {
            float x = framesToSeconds(i) * pixelsPerSecond;
            float y = midiToY(finalMidi) + pixelsPerSemitone * 0.5f;
            if (!pathStarted)
            {
              path.startNewSubPath(x, y);
              pathStarted = true;
            }
            else
            {
              path.lineTo(x, y);
            }
          }
        }

        if (pathStarted)
          g.strokePath(path, juce::PathStrokeType(2.0f));
      }
    }
  }

  if (showActualF0Debug)
  {
    const double visibleStartTime = scrollX / pixelsPerSecond;
    const double visibleEndTime = (scrollX + getWidth()) / pixelsPerSecond;
    const int visStartFrame =
        std::max(0, static_cast<int>(visibleStartTime * audioData.sampleRate /
                                     HOP_SIZE));
    const int visEndFrame = std::min(
        static_cast<int>(audioData.f0.size()),
        static_cast<int>(visibleEndTime * audioData.sampleRate / HOP_SIZE) + 1);

    g.setColour(juce::Colours::aqua.withAlpha(0.90f));
    juce::Path actualPath;
    bool pathStarted = false;

    for (int i = visStartFrame; i < visEndFrame; ++i)
    {
      const float f0 = audioData.f0[static_cast<size_t>(i)];
      if (f0 <= 0.0f)
      {
        if (pathStarted)
        {
          g.strokePath(actualPath, juce::PathStrokeType(1.7f));
          actualPath.clear();
          pathStarted = false;
        }
        continue;
      }

      const float midi = freqToMidi(f0) + globalOffset;
      const float x = framesToSeconds(i) * pixelsPerSecond;
      const float y = midiToY(midi) + pixelsPerSemitone * 0.5f;
      if (!pathStarted)
      {
        actualPath.startNewSubPath(x, y);
        pathStarted = true;
      }
      else
      {
        actualPath.lineTo(x, y);
      }
    }

    if (pathStarted)
      g.strokePath(actualPath, juce::PathStrokeType(1.7f));
  }

  // Draw base pitch curve as dashed line
  // Use cached base pitch to avoid expensive recalculation on every repaint
  if (showBasePitch)
  {
    const bool useLiveBasePreview =
        (selectHandler_->isSingleNoteDragging() || pitchEditor->isDraggingMultiNotes());
    if (!useLiveBasePreview)
    {
      updateBasePitchCacheIfNeeded();
    }

    const auto &basePitchCurve =
        useLiveBasePreview ? audioData.basePitch : cachedBasePitch;
    if (!basePitchCurve.empty())
    {
      // Calculate visible frame range
      double visibleStartTime = scrollX / pixelsPerSecond;
      double visibleEndTime = (scrollX + getWidth()) / pixelsPerSecond;
      int visStartFrame =
          std::max(0, static_cast<int>(visibleStartTime * audioData.sampleRate /
                                       HOP_SIZE));
      int visEndFrame = std::min(
          static_cast<int>(basePitchCurve.size()),
          static_cast<int>(visibleEndTime * audioData.sampleRate / HOP_SIZE) +
              1);

      // Draw base pitch curve with dashed line
      g.setColour(
          APP_COLOR_SECONDARY.withAlpha(0.6f));
      juce::Path basePath;
      bool basePathStarted = false;

      for (int i = visStartFrame; i < visEndFrame; ++i)
      {
        if (i >= 0 && i < static_cast<int>(basePitchCurve.size()))
        {
          float baseMidi = basePitchCurve[static_cast<size_t>(i)];
          if (baseMidi > 0.0f)
          {
            float x = framesToSeconds(i) * pixelsPerSecond;
            float y = midiToY(baseMidi) +
                      pixelsPerSemitone * 0.5f; // Center in grid cell

            if (!basePathStarted)
            {
              basePath.startNewSubPath(x, y);
              basePathStarted = true;
            }
            else
            {
              basePath.lineTo(x, y);
            }
          }
          else if (basePathStarted)
          {
            // Break path at unvoiced regions - draw current segment before
            // breaking
            juce::Path dashedPath;
            juce::PathStrokeType stroke(1.5f);
            const float dashLengths[] = {4.0f, 4.0f}; // 4px dash, 4px gap
            stroke.createDashedStroke(dashedPath, basePath, dashLengths, 2);
            g.strokePath(dashedPath, juce::PathStrokeType(1.5f));
            basePath.clear();
            basePathStarted = false;
          }
        }
      }

      if (basePathStarted)
      {
        // Use dashed stroke for base pitch curve
        juce::Path dashedPath;
        juce::PathStrokeType stroke(1.5f);
        const float dashLengths[] = {4.0f, 4.0f}; // 4px dash, 4px gap
        stroke.createDashedStroke(dashedPath, basePath, dashLengths, 2);
        g.strokePath(dashedPath, juce::PathStrokeType(1.5f));
      }
    }
  }
}

void PianoRollComponent::drawCursor(juce::Graphics &g)
{
  float x = timeToX(cursorTime);
  float height = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  g.setColour(APP_COLOR_PRIMARY);
  g.fillRect(x - 0.5f, 0.0f, 1.0f, height);
}

void PianoRollComponent::drawPianoKeys(juce::Graphics &g)
{
  constexpr int scrollBarSize = 8;
  auto keyArea = getLocalBounds()
                     .withWidth(pianoKeysWidth)
                     .withTrimmedTop(headerHeight)
                     .withTrimmedBottom(scrollBarSize);

  // Background
  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRect(keyArea);

  static const char *noteNames[] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
  const ScaleMode activeScaleMode = previewScaleMode.value_or(selectedScaleMode);
  const int activeScaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  const bool showScaleOverlay =
      showScaleColors && activeScaleMode != ScaleMode::None &&
      activeScaleMode != ScaleMode::Chromatic &&
      activeScaleRootNote >= 0;
  const juce::Colour scaleAccent = getScaleAccentColour(activeScaleMode);

  // Draw each key
  // Use truncated scrollY to match grid origin (which uses
  // static_cast<int>(scrollY))
  int scrollYInt = static_cast<int>(scrollY);
  for (int midi = MIN_MIDI_NOTE; midi <= MAX_MIDI_NOTE; ++midi)
  {
    float y = midiToY(static_cast<float>(midi)) -
              static_cast<float>(scrollYInt) + headerHeight;
    int noteInOctave = (midi % 12 + 12) % 12;

    // Check if it's a black key
    bool isBlack = isBlackKey(noteInOctave);
    const auto toneState =
        getScaleToneState(activeScaleMode, noteInOctave, activeScaleRootNote);

    juce::Colour keyFill = isBlack ? APP_COLOR_PIANO_BLACK : APP_COLOR_PIANO_WHITE;
    if (showScaleOverlay)
    {
      if (toneState == ScaleToneState::OutOfScale)
      {
        keyFill = APP_COLOR_PIANO_BLACK;
      }
      else
      {
        keyFill = APP_COLOR_PIANO_WHITE;
        keyFill = keyFill.interpolatedWith(scaleAccent,
                                           toneState == ScaleToneState::Root ? 0.32f : 0.16f);
      }
    }

    g.setColour(keyFill);

    g.fillRect(0.0f, y, static_cast<float>(pianoKeysWidth - 2),
               pixelsPerSemitone - 1);

    if (showScaleOverlay)
    {
      if (toneState == ScaleToneState::Root)
      {
        g.setColour(scaleAccent.withAlpha(0.95f));
        g.fillRect(0.0f, y, 3.0f, pixelsPerSemitone - 1);
      }
      else if (toneState == ScaleToneState::InScale)
      {
        g.setColour(scaleAccent.withAlpha(0.55f));
        g.fillRect(0.0f, y, 2.0f, pixelsPerSemitone - 1);
      }
    }

    // Draw note name for all notes
    int octave = midi / 12 - 1;
    juce::String noteName =
        juce::String(noteNames[noteInOctave]) + juce::String(octave);

    juce::Colour textColour = isBlack ? APP_COLOR_PIANO_TEXT_DIM
                                      : APP_COLOR_PIANO_TEXT;
    if (showScaleOverlay)
    {
      if (toneState == ScaleToneState::Root)
        textColour = APP_COLOR_TEXT_PRIMARY;
      else if (toneState == ScaleToneState::OutOfScale)
        textColour = textColour.withMultipliedAlpha(0.72f);
    }
    g.setColour(textColour);
    g.setFont(13.0f);
    g.drawText(noteName, pianoKeysWidth - 36, static_cast<int>(y), 32,
               static_cast<int>(pixelsPerSemitone),
               juce::Justification::centred);
  }
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

void PianoRollComponent::mouseDown(const juce::MouseEvent &e)
{
  if (!project)
    return;

  // Grab keyboard focus so shortcuts work after mouse operations
  grabKeyboardFocus();

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

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

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseDown(e, adjustedX, adjustedY);
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent &e)
{
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
    if (shouldRepaint)
    {
      repaint();
      lastDragRepaintTime = now;
    }
  }
}

void PianoRollComponent::mouseUp(const juce::MouseEvent &e)
{
  // Ensure keyboard focus is maintained after mouse operations
  grabKeyboardFocus();

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Loop drag has priority (always active)
  if (loopDragHandler_->mouseUp(e, adjustedX, adjustedY))
    return;

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseUp(e, adjustedX, adjustedY);
}

void PianoRollComponent::mouseMove(const juce::MouseEvent &e)
{
  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

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

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent &e)
{
  if (!project)
    return;

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
  const double totalTime = project ? project->getAudioData().getDuration() : 0.0;
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
  const double totalTime = project ? project->getAudioData().getDuration() : 0.0;
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
    scrollX = newRangeStart;
    coordMapper->setScrollX(newRangeStart);

    // Notify scroll changed for synchronization
    if (onScrollChanged)
      onScrollChanged(scrollX);
  }
  else if (scrollBar == &verticalScrollBar)
  {
    scrollY = newRangeStart;
    coordMapper->setScrollY(newRangeStart);
  }
  repaint();
}

void PianoRollComponent::setProject(Project *proj)
{
  project = proj;
  selectedScaleMode =
      project != nullptr ? project->getScaleMode() : ScaleMode::None;
  selectedScaleRootNote = project != nullptr ? project->getScaleRootNote() : -1;
  pitchReferenceHz = project != nullptr ? project->getPitchReferenceHz() : 440;
  showScaleColors = project != nullptr ? project->getShowScaleColors() : true;
  snapToSemitoneDrag = project != nullptr ? project->getSnapToSemitones() : false;
  doubleClickSnapMode = project != nullptr
                            ? project->getDoubleClickSnapMode()
                            : DoubleClickSnapMode::PitchCenter;
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
  renderer->setProject(proj);
  scrollZoomController->setProject(proj);
  pitchEditor->setProject(proj);
  pitchEditor->setSnapToSemitoneDragEnabled(snapToSemitoneDrag);
  pitchEditor->setPitchReferenceHz(pitchReferenceHz);
  noteSplitter->setProject(proj);
  pitchToolController->setProject(proj);

  // Clear all caches when project changes to free memory
  invalidateBasePitchCache();
  waveformCache = juce::Image(); // Clear waveform cache
  cachedScrollX = -1.0;
  cachedPixelsPerSecond = -1.0f;
  cachedWidth = 0;
  cachedHeight = 0;

  updatePitchToolHandlesFromSelection();

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
  if (previewScaleMode == mode)
    return;

  previewScaleMode = mode;
  repaint();
}

void PianoRollComponent::setShowScaleColors(bool enabled)
{
  if (showScaleColors == enabled)
    return;

  showScaleColors = enabled;
  if (project != nullptr)
    project->setShowScaleColors(enabled);
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
}

void PianoRollComponent::setPitchReferenceHz(int hz)
{
  const int normalized = juce::jlimit(380, 480, hz);
  if (pitchReferenceHz == normalized)
    return;

  pitchReferenceHz = normalized;
  if (project != nullptr)
    project->setPitchReferenceHz(normalized);
  pitchEditor->setPitchReferenceHz(normalized);
}

void PianoRollComponent::setDoubleClickSnapMode(DoubleClickSnapMode mode)
{
  if (doubleClickSnapMode == mode)
    return;

  doubleClickSnapMode = mode;
  if (project != nullptr)
    project->setDoubleClickSnapMode(mode);
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
    auto action = std::make_unique<MultiNoteMidiNudgeAction>(
        notesToMove, oldMidis, newMidis,
        [rebuildAndNotify](const std::vector<Note *> &notes)
        {
          rebuildAndNotify(notes);
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
  // Include timeline area (from 0) and extra width for triangle indicator
  auto getCursorRect = [this](double t) -> juce::Rectangle<int>
  {
    float x =
        static_cast<float>(t * pixelsPerSecond - scrollX) + pianoKeysWidth;
    constexpr int triangleHalfWidth = 6; // Half of triangle width + margin
    int rectX = static_cast<int>(x) - triangleHalfWidth;
    int rectWidth =
        triangleHalfWidth * 2 + 2; // Full triangle width + cursor line
    // Start from 0 (top of timeline) to include triangle indicator
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
  const double totalTime = project ? project->getAudioData().getDuration() : 0.0;
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
  repaint();
}

void PianoRollComponent::setScrollX(double x)
{
  if (std::abs(scrollX - x) < 0.01)
    return; // No significant change

  scrollX = x;
  coordMapper->setScrollX(x);
  horizontalScrollBar.setCurrentRangeStart(x);

  // Don't call onScrollChanged here to avoid infinite recursion
  // The caller is responsible for synchronizing other components

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
  repaint();
}

void PianoRollComponent::setEditMode(EditMode mode)
{
  // Cancel active handler interaction if leaving its mode
#if PITCHNET_ENABLE_STRETCH
  if (editMode == EditMode::Stretch && mode != EditMode::Stretch &&
      stretchHandler_ && stretchHandler_->isActive())
  {
    stretchHandler_->cancel();
  }
#endif

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
#if PITCHNET_ENABLE_STRETCH
  case EditMode::Stretch:
    currentHandler_ = stretchHandler_.get();
    break;
#endif
  case EditMode::Split:
    currentHandler_ = splitHandler_.get();
    break;
  case EditMode::Parameter:
    currentHandler_ = nullptr; // Drawing handled by HNSepLaneComponent
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

void PianoRollComponent::updateScrollBars()
{
  if (project)
  {
    float totalWidth = project->getAudioData().getDuration() * pixelsPerSecond;
    float totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

    int visibleWidth = getVisibleContentWidth();
    int visibleHeight = getVisibleContentHeight();

    horizontalScrollBar.setRangeLimits(0, totalWidth);
    horizontalScrollBar.setCurrentRange(scrollX, visibleWidth);

    verticalScrollBar.setRangeLimits(0, totalHeight);
    verticalScrollBar.setCurrentRange(scrollY, visibleHeight);
  }
}

void PianoRollComponent::updateBasePitchCacheIfNeeded()
{
  if (!project)
  {
    cachedBasePitch.clear();
    cachedNoteCount = 0;
    cachedTotalFrames = 0;
    return;
  }

  const auto &notes = project->getNotes();
  const auto &audioData = project->getAudioData();
  int totalFrames = static_cast<int>(audioData.f0.size());

  // Check if cache is valid
  size_t currentNoteCount = 0;
  for (const auto &note : notes)
  {
    if (!note.isRest())
    {
      currentNoteCount++;
    }
  }

  // Invalidate cache if notes changed or total frames changed or explicitly
  // invalidated For performance, we only check note count and total frames A
  // more precise check would compare note positions/pitches, but that's
  // expensive
  if (cacheInvalidated || cachedNoteCount != currentNoteCount ||
      cachedTotalFrames != totalFrames || cachedBasePitch.empty())
  {
    // Only regenerate if we have notes and frames
    if (currentNoteCount > 0 && totalFrames > 0)
    {
      // Collect all notes
      std::vector<BasePitchCurve::NoteSegment> noteSegments;
      noteSegments.reserve(currentNoteCount);
      for (const auto &note : notes)
      {
        if (!note.isRest())
        {
          noteSegments.push_back(
              {note.getStartFrame(), note.getEndFrame(), note.getMidiNote()});
        }
      }

      if (!noteSegments.empty())
      {
        // Generate smoothed base pitch curve (expensive operation, cached)
        // This is only called when notes change, not on every repaint
        cachedBasePitch =
            BasePitchCurve::generateForNotes(noteSegments, totalFrames);
        cachedNoteCount = currentNoteCount;
        cachedTotalFrames = totalFrames;
        cacheInvalidated = false; // Mark cache as valid
      }
      else
      {
        cachedBasePitch.clear();
        cachedNoteCount = 0;
        cachedTotalFrames = 0;
        cacheInvalidated = false; // Mark as processed (even if empty)
      }
    }
    else
    {
      cachedBasePitch.clear();
      cachedNoteCount = 0;
      cachedTotalFrames = 0;
      cacheInvalidated = false; // Mark as processed (even if empty)
    }
  }
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
  g.setColour(APP_COLOR_SELECTION_HIGHLIGHT);
  g.fillRect(rect);

  // Draw border
  g.setColour(APP_COLOR_SELECTION_HIGHLIGHT_STRONG);
  g.drawRect(rect, 1.0f);
}
