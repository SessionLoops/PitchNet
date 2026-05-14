#include "NoteRenderer.h"
#include "PitchEditor.h"
#include "States/SelectHandler.h"
#include "States/SplitHandler.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>

void NoteRenderer::draw(juce::Graphics &g, Pass pass, bool splitModeActive,
                        int componentWidth)
{
  if (!project || !coordMapper)
    return;

  const bool drawBodies = pass == Pass::Body;
  const bool drawOverlays = pass == Pass::Overlay;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();

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
  const double visibleStartTime = scrollX / pixelsPerSecond;
  const double visibleEndTime = (scrollX + componentWidth) / pixelsPerSecond;

  for (auto &note : project->getNotes())
  {
    if (note.isRest())
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

    if (drawBodies)
    {
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
        const int numNoteSamples = endSample - startSample;
        const int samplesPerPixel = std::max(1, static_cast<int>(numNoteSamples / w));

        const float centerY = y + h * 0.5f;
        const float waveHeight = h * 3.0f;

        waveValues.clear();
        const float step = std::max(0.5f, w / 1024.0f);

        for (float px = 0; px <= w; px += step)
        {
          const int sampleIdx =
              startSample + static_cast<int>((px / w) * numNoteSamples);
          const int sampleEnd = std::min(sampleIdx + samplesPerPixel, endSample);

          float maxVal = 0.0f;
          for (int i = sampleIdx; i < sampleEnd; ++i)
            maxVal = std::max(maxVal, std::abs(samples[i]));

          waveValues.push_back(maxVal);
        }

        if (waveValues.size() > 2)
        {
          smoothed.resize(waveValues.size());
          smoothed[0] = waveValues[0];
          for (size_t i = 1; i + 1 < waveValues.size(); ++i)
          {
            smoothed[i] = (waveValues[i - 1] * 0.25f + waveValues[i] * 0.5f +
                           waveValues[i + 1] * 0.25f);
          }
          smoothed[waveValues.size() - 1] = waveValues[waveValues.size() - 1];
          waveValues = std::move(smoothed);
        }

        const size_t numPoints = waveValues.size();
        if (numPoints < 2)
        {
          g.setColour(noteColor.withAlpha(0.85f));
          g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
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

          g.setColour(noteColor.withAlpha(0.85f));
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
          g.fillPath(waveformPath);

          g.setColour(noteColor.brighter(0.2f));
          g.strokePath(waveformPath,
                       juce::PathStrokeType(1.2f,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
        }
      }
      else
      {
        g.setColour(noteColor.withAlpha(0.85f));
        g.fillRoundedRectangle(x, y, renderedWidth, h, 2.0f);
      }
    }

    if (drawOverlays && note.isSelected() && selectHandler)
    {
      drawSelectedNoteOutline(x, y, renderedWidth, h);

      const auto handleBounds =
          getDeltaScaleHandleBounds(x, y, renderedWidth, h);
      const bool handleActive =
          selectHandler->getIsDeltaScaleDragging() &&
          std::find(selectHandler->getDeltaScaleTargetNotes().begin(),
                    selectHandler->getDeltaScaleTargetNotes().end(),
                    &note) != selectHandler->getDeltaScaleTargetNotes().end();
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

      if (selectHandler->getIsDeltaScaleDragging() &&
          selectHandler->getDeltaScaleFactor() > 0.0f)
      {
        const juce::String factorText =
            "x" + juce::String(selectHandler->getDeltaScaleFactor(), 2);
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
          selectHandler->getIsDeltaOffsetDragging() &&
          std::find(selectHandler->getDeltaOffsetTargetNotes().begin(),
                    selectHandler->getDeltaOffsetTargetNotes().end(),
                    &note) != selectHandler->getDeltaOffsetTargetNotes().end();
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

      if (selectHandler->getIsDeltaOffsetDragging())
      {
        const juce::String prefix =
            selectHandler->getDeltaOffsetSemitones() >= 0.0f ? "+" : "";
        const juce::String offsetText =
            prefix + juce::String(selectHandler->getDeltaOffsetSemitones(), 2) + " st";
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

    const bool isSingleDragged =
        selectHandler && selectHandler->isSingleNoteDragging() &&
        selectHandler->getDraggedNote() == &note;
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
  if (drawOverlays && splitModeActive && splitHandler &&
      splitHandler->getSplitGuideNote() &&
      splitHandler->getSplitGuideX() >= 0)
  {
    auto *guideNote = splitHandler->getSplitGuideNote();
    const float guideX = splitHandler->getSplitGuideX();
    const float noteStartTime = framesToSeconds(guideNote->getStartFrame());
    const float noteEndTime = framesToSeconds(guideNote->getEndFrame());
    const float noteStartX = static_cast<float>(noteStartTime * pixelsPerSecond);
    const float noteEndX = static_cast<float>(noteEndTime * pixelsPerSecond);

    if (guideX > noteStartX + 5 && guideX < noteEndX - 5)
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
