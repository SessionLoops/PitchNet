#include "NoteRenderer.h"
#include "BoxSelector.h"
#include "PitchEditor.h"
#include "States/SelectHandler.h"
#include "States/SplitHandler.h"
#include "VisualWaveformEnvelope.h"
#include "../../Utils/Constants.h"
#include "../../Utils/ScaleUtils.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>

namespace
{
struct NoteGradientColours
{
  juce::Colour centre;
  juce::Colour side;
};

float getPitchCenterAmount(float midi, int pitchReferenceHz)
{
  const float pitchCenter =
      ScaleUtils::snapMidiToSemitone(midi, pitchReferenceHz);
  const float distanceFromCenter = std::abs(midi - pitchCenter);

  return juce::jlimit(0.0f, 1.0f, distanceFromCenter / 0.5f);
}

juce::Colour interpolateLayeredColour(juce::Colour first,
                                      juce::Colour second,
                                      juce::Colour third,
                                      juce::Colour fourth,
                                      float amount)
{
  if (amount < 1.0f / 3.0f)
    return first.interpolatedWith(second, amount * 3.0f);

  if (amount < 2.0f / 3.0f)
    return second.interpolatedWith(third, amount * 3.0f - 1.0f);

  return third.interpolatedWith(fourth, amount * 3.0f - 2.0f);
}

NoteGradientColours getNoteGradientColours(float midi, int pitchReferenceHz)
{
  static const juce::Colour inTuneCentre(0xFF1983E0u);
  static const juce::Colour inTuneSide(0xFF0021E2u);
  static const juce::Colour firstLayerCentre(0xFFD66CFFu);
  static const juce::Colour firstLayerSide(0xFF971CFFu);
  static const juce::Colour secondLayerCentre(0xFFFF90EDu);
  static const juce::Colour secondLayerSide(0xFFF624B7u);
  static const juce::Colour outOfTuneCentre(0xFFF95D5Du);
  static const juce::Colour outOfTuneSide(0xFFFF0000u);

  const float amount = getPitchCenterAmount(midi, pitchReferenceHz);

  return {
      interpolateLayeredColour(inTuneCentre, firstLayerCentre,
                               secondLayerCentre, outOfTuneCentre, amount),
      interpolateLayeredColour(inTuneSide, firstLayerSide, secondLayerSide,
                               outOfTuneSide, amount)};
}

void setNoteGradientFill(juce::Graphics &g,
                         const juce::Rectangle<float> &bounds,
                         float centreY,
                         const NoteGradientColours &colours)
{
  const float height = bounds.getHeight();
  if (height <= 0.0f || !std::isfinite(height))
  {
    g.setColour(colours.centre);
    return;
  }

  const float top = bounds.getY();
  const float bottom = bounds.getBottom();
  const float gradientCentre =
      juce::jlimit(0.0f, 1.0f, (centreY - top) / height);

  juce::ColourGradient gradient(colours.side, bounds.getCentreX(), top,
                                colours.side, bounds.getCentreX(), bottom,
                                false);
  gradient.addColour(gradientCentre, colours.centre);
  g.setGradientFill(gradient);
}
} // namespace

void NoteRenderer::draw(juce::Graphics &g, Pass pass, bool splitModeActive,
                        int componentWidth)
{
  if (!project || !coordMapper)
    return;

  const bool drawBodies = pass == Pass::Body || pass == Pass::HoveredBody;
  const bool drawOverlays = pass == Pass::Overlay;
  const bool drawHoverShadow = pass == Pass::HoverShadow;
  const bool drawHoveredBody = pass == Pass::HoveredBody;

  if ((drawHoverShadow || drawHoveredBody) && hoveredNote == nullptr)
    return;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();
  const int pitchReferenceHz = project->getPitchReferenceHz();

  // Pre-allocated scratch buffers to avoid per-note heap allocations
  std::vector<float> waveValues;
  waveValues.reserve(2048);

  const bool isMultiDragging = pitchEditor && pitchEditor->isDraggingMultiNotes();
  const bool isSingleDragging =
      selectHandler && selectHandler->isSingleNoteDragging();
  const bool isDraggingNote = isSingleDragging || isMultiDragging;
  if (drawHoverShadow && isDraggingNote)
    return;

  const std::vector<Note *> *draggedNotes =
      isMultiDragging ? &pitchEditor->getDraggedNotes() : nullptr;
  Note *hoveredMultiDragNote =
      isMultiDragging ? pitchEditor->getHoveredMultiDragNote() : nullptr;
  int selectedNoteCount = 0;
  for (const auto &note : project->getNotes())
  {
    if (!note.isRest() && note.isSelected())
      ++selectedNoteCount;
  }
  const bool showSelectionStatus =
      selectedNoteCount > 1 ||
      (selectedNoteCount > 0 && boxSelector &&
       (boxSelector->isSelecting() || boxSelector->wasLastSelectionFromBox()));

  const auto &audioData = project->getAudioData();
  const float *globalSamples =
      (drawBodies || drawHoverShadow) && audioData.waveform.getNumSamples() > 0
          ? audioData.waveform.getReadPointer(0)
          : nullptr;
  int globalTotalSamples =
      (drawBodies || drawHoverShadow) ? audioData.waveform.getNumSamples() : 0;

  auto getHighlightedShadowBounds = [&](const Note &note, float x, float y,
                                        float renderedWidth, float noteWidth,
                                        float noteHeight)
  {
    auto shadowVisualBounds =
        juce::Rectangle<float>(x, y, renderedWidth, noteHeight);
    const float *samples = globalSamples;
    int totalSamples = globalTotalSamples;
    int startSample = 0;
    int endSample = 0;
    if (samples && totalSamples > 0)
    {
      startSample = static_cast<int>(framesToSeconds(note.getStartFrame()) *
                                     audioData.sampleRate);
      endSample = static_cast<int>(framesToSeconds(note.getEndFrame()) *
                                   audioData.sampleRate);
      startSample = std::max(0, std::min(startSample, totalSamples - 1));
      endSample = std::max(startSample + 1, std::min(endSample, totalSamples));
    }

    if (samples && totalSamples > 0 && noteWidth > 2.0f &&
        endSample > startSample)
    {
      const int shadowPointCount =
          std::max(2, std::min(512, static_cast<int>(std::ceil(noteWidth)) + 1));
      const auto shadowEnvelope = VisualWaveformEnvelope::build(
          samples, totalSamples, startSample, endSample, shadowPointCount,
          renderedWidth, audioData.sampleRate, pixelsPerSecond);
      const float maxSample =
          shadowEnvelope.empty()
              ? 0.0f
              : *std::max_element(shadowEnvelope.begin(), shadowEnvelope.end());

      const float centerY = y + noteHeight * 0.5f;
      const float waveHeight = noteHeight * 3.0f;
      shadowVisualBounds =
          juce::Rectangle<float>(x, centerY - maxSample * waveHeight * 0.5f,
                                 renderedWidth, maxSample * waveHeight);
    }

    return shadowVisualBounds.expanded(4.0f, 4.0f)
        .getSmallestIntegerContainer();
  };

  // Calculate visible time range for culling
  const double visibleStartTime = scrollX / pixelsPerSecond;
  const double visibleEndTime = (scrollX + componentWidth) / pixelsPerSecond;

  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;
    if (drawHoveredBody && &note != hoveredNote)
      continue;

    // Viewport culling: skip notes outside visible area
    const double noteStartTime = framesToSeconds(note.getStartFrame());
    const double noteEndTime = framesToSeconds(note.getEndFrame());
    if (noteEndTime < visibleStartTime || noteStartTime > visibleEndTime)
      continue;

    const float x = static_cast<float>(noteStartTime * pixelsPerSecond);
    const float w = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
    const float h = pixelsPerSemitone;
    const float renderedWidth = std::max(w, 4.0f);

    // Position at grid cell center for MIDI note, then offset by pitch adjustment
    const float baseGridCenterY =
        coordMapper->midiToY(note.getMidiNote()) + pixelsPerSemitone * 0.5f;
    const float pitchOffsetPixels = -note.getPitchOffset() * pixelsPerSemitone;
    const float y = baseGridCenterY + pitchOffsetPixels - h * 0.5f;
    const bool isPreviewPlaybackNote =
        previewPlaybackActive && note.getEndFrame() > previewStartFrame &&
        note.getStartFrame() < previewEndFrame;
    if (drawHoveredBody && isPreviewPlaybackNote)
      continue;

    if (drawHoverShadow)
    {
      if (&note != hoveredNote)
        continue;
      if (isPreviewPlaybackNote)
        return;

      const auto shadowBounds =
          getHighlightedShadowBounds(note, x, y, renderedWidth, w, h);
      g.setColour(juce::Colours::white.withAlpha(0.08f));
      g.fillRoundedRectangle(shadowBounds.toFloat(), 3.0f);
      return;
    }

    if (drawBodies)
    {
      const NoteGradientColours noteColours =
          getNoteGradientColours(note.getAdjustedMidiNote(), pitchReferenceHz);
      juce::Rectangle<float> noteVisualBounds(x, y, renderedWidth, h);

      const float *samples = globalSamples;
      int totalSamples = globalTotalSamples;
      int startSample = 0;
      int endSample = 0;
      if (samples && totalSamples > 0)
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
        const float centerY = y + h * 0.5f;
        const float waveHeight = h * 3.0f;
        const int pointCount =
            std::max(2, std::min(2048, static_cast<int>(std::ceil(w * 2.0f)) + 1));
        waveValues = VisualWaveformEnvelope::build(
            samples, totalSamples, startSample, endSample, pointCount, w,
            audioData.sampleRate, pixelsPerSecond);

        const size_t numPoints = waveValues.size();
        if (numPoints < 2)
        {
          if (isPreviewPlaybackNote)
            g.setColour(juce::Colours::white.withAlpha(0.20f));
          else
            setNoteGradientFill(g, noteVisualBounds,
                                noteVisualBounds.getCentreY(), noteColours);

          g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);

          if (isPreviewPlaybackNote)
          {
            const float previewProgressX =
                static_cast<float>(previewCurrentTime * pixelsPerSecond);
            const float clipRight =
                juce::jlimit(x, x + renderedWidth, previewProgressX);
            if (clipRight > x)
            {
              juce::Graphics::ScopedSaveState previewClipState(g);
              g.reduceClipRegion(
                  juce::Rectangle<float>(x, y, clipRight - x, h)
                      .getSmallestIntegerContainer());
              setNoteGradientFill(g, noteVisualBounds,
                                  noteVisualBounds.getCentreY(), noteColours);
              g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
            }
          }

          noteVisualBounds = {x, y, renderedWidth, h};
        }
        else
        {
          auto catmullRom = [](float t, float p0, float p1, float p2,
                               float p3) -> float
          {
            const float t2 = t * t;
            const float t3 = t2 * t;
            return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                           (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                           (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
          };

          juce::Path waveformPath;

          waveformPath.startNewSubPath(
              x, centerY - waveValues[0] * waveHeight * 0.5f);

          constexpr int curveSegments = 4;
          for (size_t i = 0; i + 1 < numPoints; ++i)
          {
            const float px1 = (static_cast<float>(i) /
                               static_cast<float>(numPoints - 1)) *
                              w;
            const float px2 = (static_cast<float>(i + 1) /
                               static_cast<float>(numPoints - 1)) *
                              w;

            const size_t idx0 = (i > 0) ? i - 1 : i;
            const size_t idx1 = i;
            const size_t idx2 = i + 1;
            const size_t idx3 = (i + 2 < numPoints) ? i + 2 : i + 1;

            const float val0 = waveValues[idx0];
            const float val1 = waveValues[idx1];
            const float val2 = waveValues[idx2];
            const float val3 = waveValues[idx3];

            for (int seg = 1; seg <= curveSegments; ++seg)
            {
              const float t =
                  static_cast<float>(seg) / static_cast<float>(curveSegments);
              const float px = px1 + (px2 - px1) * t;
              const float val = catmullRom(t, val0, val1, val2, val3);
              const float yPos = centerY - val * waveHeight * 0.5f;
              waveformPath.lineTo(x + px, yPos);
            }
          }

          waveformPath.lineTo(x + w, centerY + waveValues[numPoints - 1] *
                                                   waveHeight * 0.5f);

          for (int i = static_cast<int>(numPoints) - 2; i >= 0; --i)
          {
            const float px1 = (static_cast<float>(i + 1) /
                               static_cast<float>(numPoints - 1)) *
                              w;
            const float px2 =
                (static_cast<float>(i) / static_cast<float>(numPoints - 1)) *
                w;

            const size_t idx0 = (i + 2 < numPoints) ? i + 2 : i + 1;
            const size_t idx1 = i + 1;
            const size_t idx2 = i;
            const size_t idx3 = (i > 0) ? i - 1 : i;

            const float val0 = waveValues[idx0];
            const float val1 = waveValues[idx1];
            const float val2 = waveValues[idx2];
            const float val3 = waveValues[idx3];

            for (int seg = 1; seg <= curveSegments; ++seg)
            {
              const float t =
                  static_cast<float>(seg) / static_cast<float>(curveSegments);
              const float px = px1 + (px2 - px1) * t;
              const float val = catmullRom(t, val0, val1, val2, val3);
              const float yPos = centerY + val * waveHeight * 0.5f;
              waveformPath.lineTo(x + px, yPos);
            }
          }

          waveformPath.closeSubPath();
          noteVisualBounds = waveformPath.getBounds();

          if (isPreviewPlaybackNote)
          {
            g.setColour(juce::Colours::white.withAlpha(0.20f));
            g.fillPath(waveformPath);

            const float previewProgressX =
                static_cast<float>(previewCurrentTime * pixelsPerSecond);
            const float clipRight =
                juce::jlimit(x, x + renderedWidth, previewProgressX);
            if (clipRight > x)
            {
              juce::Graphics::ScopedSaveState previewClipState(g);
              const auto clipBounds =
                  juce::Rectangle<float>(x - 2.0f, y - h * 2.0f,
                                         (clipRight - x) + 2.0f, h * 5.0f)
                      .getSmallestIntegerContainer();
              g.reduceClipRegion(clipBounds);
              setNoteGradientFill(g, noteVisualBounds, centerY, noteColours);
              g.fillPath(waveformPath);
            }
          }
          else
          {
            setNoteGradientFill(g, noteVisualBounds, centerY, noteColours);
            g.fillPath(waveformPath);
          }
        }
      }
      else
      {
        if (isPreviewPlaybackNote)
          g.setColour(juce::Colours::white.withAlpha(0.20f));
        else
          setNoteGradientFill(g, noteVisualBounds, noteVisualBounds.getCentreY(),
                              noteColours);

        g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);

        if (isPreviewPlaybackNote)
        {
          const float previewProgressX =
              static_cast<float>(previewCurrentTime * pixelsPerSecond);
          const float clipRight =
              juce::jlimit(x, x + renderedWidth, previewProgressX);
          if (clipRight > x)
          {
            juce::Graphics::ScopedSaveState previewClipState(g);
            g.reduceClipRegion(
                juce::Rectangle<float>(x, y, clipRight - x, h)
                    .getSmallestIntegerContainer());
            setNoteGradientFill(g, noteVisualBounds,
                                noteVisualBounds.getCentreY(), noteColours);
            g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
          }
        }

        noteVisualBounds = {x, y, renderedWidth, h};
      }

      if (showSelectionStatus && note.isSelected())
      {
        const auto outlineBounds = noteVisualBounds.expanded(1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.72f));
        g.drawRoundedRectangle(outlineBounds, 2.0f, 1.5f);
      }
    }

    const bool isSingleDragged =
        isSingleDragging && selectHandler->getDraggedNote() == &note;
    const bool isMultiDragged =
        isMultiDragging && draggedNotes &&
        std::find(draggedNotes->begin(), draggedNotes->end(), &note) !=
            draggedNotes->end();
    const bool shouldShowPitchTip =
        isSingleDragged || (isMultiDragged && hoveredMultiDragNote == &note);
    if (drawOverlays && shouldShowPitchTip)
    {
      const float deltaSemitones = note.getPitchOffset();
      const juce::String prefix = deltaSemitones > 0.0f ? "+" : "";
      const juce::String label =
          prefix + juce::String(deltaSemitones, 1) + " st";

      constexpr float labelWidth = 60.0f;
      constexpr float labelHeight = 20.0f;
      const float labelX = x + renderedWidth * 0.5f - labelWidth * 0.5f;
      const auto shadowBounds =
          getHighlightedShadowBounds(note, x, y, renderedWidth, w, h);
      const float labelY =
          shadowBounds.toFloat().getY() - labelHeight + 5.0f;

      g.setColour(juce::Colour(0xFF2E2E2Du));
      g.fillRoundedRectangle(labelX, labelY, labelWidth, labelHeight, 4.0f);
      g.setColour(juce::Colour(0xFFEFEFEFu));
      g.setFont(juce::Font("Montserrat", "Regular", 11.0f).withPointHeight(11.0f));
      g.drawFittedText(label, static_cast<int>(labelX),
                       static_cast<int>(labelY),
                       static_cast<int>(labelWidth),
                       static_cast<int>(labelHeight),
                       juce::Justification::centred, 1);
    }
  }

  // In split mode, show the boundary between each consecutive pair of notes
  // in the same audio region. Span the full pitch interval so the connector
  // reaches from the higher note's top to the lower note's bottom.
  if (drawOverlays && splitModeActive)
  {
    std::vector<const Note *> timelineNotes;
    timelineNotes.reserve(project->getNotes().size());
    for (const auto &note : project->getNotes())
    {
      if (!note.isRest())
        timelineNotes.push_back(&note);
    }
    std::sort(timelineNotes.begin(), timelineNotes.end(),
              [](const Note *left, const Note *right)
              {
                return left->getStartFrame() < right->getStartFrame();
              });

    const auto &chunkRanges = audioData.segmentChunkRanges;
    auto getRegionIndex = [&chunkRanges](const Note &note)
    {
      if (chunkRanges.empty())
        return 0;

      const int midpoint = note.getStartFrame() +
                           std::max(0, note.getDurationFrames()) / 2;
      for (size_t i = 0; i < chunkRanges.size(); ++i)
      {
        if (midpoint >= chunkRanges[i].first && midpoint < chunkRanges[i].second)
          return static_cast<int>(i);
      }
      return -1;
    };

    g.setColour(juce::Colour(0xFF9A9A9Au)); // Pitch-grid note-name colour
    for (size_t i = 0; i + 1 < timelineNotes.size(); ++i)
    {
      const auto &left = *timelineNotes[i];
      const auto &right = *timelineNotes[i + 1];
      const int leftRegion = getRegionIndex(left);
      if (leftRegion < 0 || leftRegion != getRegionIndex(right))
        continue;

      const float boundaryX =
          0.5f * framesToSeconds(left.getEndFrame() + right.getStartFrame()) *
          pixelsPerSecond;
      if (boundaryX < static_cast<float>(scrollX) ||
          boundaryX > static_cast<float>(scrollX + componentWidth))
        continue;

      const float leftY = coordMapper->midiToY(left.getAdjustedMidiNote());
      const float rightY = coordMapper->midiToY(right.getAdjustedMidiNote());
      const float lineTop = std::min(leftY, rightY);
      const float lineBottom = std::max(leftY, rightY) + pixelsPerSemitone;
      g.drawLine(boundaryX, lineTop, boundaryX, lineBottom, 1.5f);
    }
  }

  // Draw split guide line when in split mode and hovering over a note
  if (drawOverlays && splitModeActive && splitHandler &&
      splitHandler->getSplitGuideNote() &&
      splitHandler->getSplitGuideX() >= 0)
  {
    auto *guideNote = splitHandler->getSplitGuideNote();
    const float guideX = splitHandler->getSplitGuideX();
    const int guideFrame = secondsToFrames(guideX / pixelsPerSecond);

    if (guideFrame > guideNote->getStartFrame() &&
        guideFrame < guideNote->getEndFrame())
    {
      const float noteY = coordMapper->midiToY(guideNote->getAdjustedMidiNote());
      const float noteH = pixelsPerSemitone;

      g.setColour(APP_COLOR_SECONDARY);
      constexpr float dashLength = 4.0f;
      for (float dy = 0; dy < noteH; dy += dashLength * 2)
      {
        const float segmentLength = std::min(dashLength, noteH - dy);
        g.drawLine(guideX, noteY + dy, guideX,
                   noteY + dy + segmentLength, 2.0f);
      }
    }
  }
}
