#include "PitchCurveRenderer.h"
#include "PitchEditor.h"
#include "States/SelectHandler.h"
#include "../../Utils/BasePitchCurve.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>

void PitchCurveRenderer::invalidateBasePitchCache()
{
  cacheInvalidated = true;
  cachedNoteCount = 0;
  cachedBasePitch.clear();
  cachedBasePitch.shrink_to_fit();
}

void PitchCurveRenderer::updateBasePitchCacheIfNeeded()
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
  const int totalFrames = static_cast<int>(audioData.f0.size());

  size_t currentNoteCount = 0;
  for (const auto &note : notes)
  {
    if (!note.isRest())
      ++currentNoteCount;
  }

  // Note: precise check would compare note positions/pitches but is expensive.
  if (cacheInvalidated || cachedNoteCount != currentNoteCount ||
      cachedTotalFrames != totalFrames || cachedBasePitch.empty())
  {
    if (currentNoteCount > 0 && totalFrames > 0)
    {
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
        cachedBasePitch =
            BasePitchCurve::generateForNotes(noteSegments, totalFrames);
        cachedNoteCount = currentNoteCount;
        cachedTotalFrames = totalFrames;
        cacheInvalidated = false;
      }
      else
      {
        cachedBasePitch.clear();
        cachedNoteCount = 0;
        cachedTotalFrames = 0;
        cacheInvalidated = false;
      }
    }
    else
    {
      cachedBasePitch.clear();
      cachedNoteCount = 0;
      cachedTotalFrames = 0;
      cacheInvalidated = false;
    }
  }
}

void PitchCurveRenderer::draw(juce::Graphics &g, const Params &params)
{
  if (!project || !coordMapper)
    return;

  if (params.hidePitchCurves)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.f0.empty())
    return;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();

  const float globalOffset = project->getGlobalPitchOffset();

  // Draw the same composed pitch used by synthesis. basePitch already includes
  // any per-note pitch offset, including the live drag preview.
  if (params.showDeltaPitch)
  {
    g.setColour(APP_COLOR_PITCH_CURVE);

    juce::Path path;
    bool pathStarted = false;
    auto strokeCurrentPath = [&]()
    {
      if (pathStarted)
        g.strokePath(path, juce::PathStrokeType(2.0f));
      path.clear();
      pathStarted = false;
    };

    const auto &chunkRanges = audioData.segmentChunkRanges;
    auto getRegionIndex = [&chunkRanges](const Note &note)
    {
      if (chunkRanges.empty())
        return 0;

      const int midpoint = note.getStartFrame() +
                           std::max(0, note.getDurationFrames()) / 2;
      for (size_t i = 0; i < chunkRanges.size(); ++i)
      {
        if (midpoint >= chunkRanges[i].first &&
            midpoint < chunkRanges[i].second)
          return static_cast<int>(i);
      }
      return -1;
    };

    bool hasPreviousNote = false;
    int previousEndFrame = -1;
    int previousRegion = -1;

    for (const auto &note : project->getNotes())
    {
      if (note.isRest())
        continue;

      const int startFrame = note.getStartFrame();
      const int endFrame =
          std::min(note.getEndFrame(), static_cast<int>(audioData.f0.size()));
      const int currentRegion = getRegionIndex(note);
      const bool continuesPreviousPath =
          hasPreviousNote && startFrame == previousEndFrame &&
          currentRegion >= 0 && currentRegion == previousRegion;

      if (!continuesPreviousPath)
        strokeCurrentPath();

      for (int i = startFrame; i < endFrame; ++i)
      {
        float baseMidi =
            (i < static_cast<int>(audioData.basePitch.size()))
                ? audioData.basePitch[static_cast<size_t>(i)]
                : ((i < static_cast<int>(audioData.f0.size()) &&
                    audioData.f0[static_cast<size_t>(i)] > 0.0f)
                       ? freqToMidi(audioData.f0[static_cast<size_t>(i)])
                       : 0.0f);

        const float deltaMidi =
            (i < static_cast<int>(audioData.deltaPitch.size()))
                ? audioData.deltaPitch[static_cast<size_t>(i)]
                : 0.0f;
        const float finalMidi = baseMidi + deltaMidi + globalOffset;

        if (finalMidi > 0.0f)
        {
          const float x = framesToSeconds(i) * pixelsPerSecond;
          const float y = coordMapper->midiToY(finalMidi) +
                          pixelsPerSemitone * 0.5f;
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
        else
        {
          strokeCurrentPath();
        }
      }

      hasPreviousNote = true;
      previousEndFrame = endFrame;
      previousRegion = currentRegion;
    }

    strokeCurrentPath();
  }

  auto drawHzDebugCurve =
      [&](const std::vector<float> &curve, juce::Colour colour,
          float strokeWidth, float midiOffset = 0.0f)
  {
    if (curve.empty())
      return;

    const double visibleStartTime = scrollX / pixelsPerSecond;
    const double visibleEndTime = (scrollX + params.componentWidth) / pixelsPerSecond;
    const int visStartFrame =
        std::max(0, static_cast<int>(visibleStartTime * audioData.sampleRate /
                                     HOP_SIZE));
    const int visEndFrame = std::min(
        static_cast<int>(curve.size()),
        static_cast<int>(visibleEndTime * audioData.sampleRate / HOP_SIZE) + 1);

    g.setColour(colour);
    juce::Path path;
    bool pathStarted = false;

    for (int i = visStartFrame; i < visEndFrame; ++i)
    {
      const float f0 = curve[static_cast<size_t>(i)];
      if (f0 <= 0.0f)
      {
        pathStarted = false;
        continue;
      }

      const float midi = freqToMidi(f0) + midiOffset;
      const float x = framesToSeconds(i) * pixelsPerSecond;
      const float y = coordMapper->midiToY(midi) + pixelsPerSemitone * 0.5f;
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

    g.strokePath(path, juce::PathStrokeType(strokeWidth));
  };

  if (params.showActualF0Debug)
  {
    drawHzDebugCurve(audioData.rawF0, juce::Colours::aqua.withAlpha(0.90f),
                     1.7f);
  }

  if (params.showCleanedF0Debug)
  {
    drawHzDebugCurve(audioData.cleanedF0,
                     juce::Colours::orange.withAlpha(0.90f), 1.7f);
  }

  if (params.showUvInterpolationDebug)
  {
    drawHzDebugCurve(audioData.denseF0,
                     juce::Colours::magenta.withAlpha(0.86f), 1.7f);
  }

  if (params.showVocoderF0Debug)
  {
    const auto vocoderF0 = project->getAdjustedF0();
    if (!vocoderF0.empty())
      drawHzDebugCurve(vocoderF0, juce::Colours::lime.withAlpha(0.86f), 2.0f);
    else
      drawHzDebugCurve(audioData.f0, juce::Colours::lime.withAlpha(0.86f),
                       2.0f, globalOffset);
  }

  // Draw base pitch curve as dashed line. Uses cached base pitch to avoid
  // expensive recalculation on every repaint.
  if (params.showBasePitch)
  {
    const bool useLiveBasePreview =
        selectHandler && pitchEditor &&
        (selectHandler->isSingleNoteDragging() || pitchEditor->isDraggingMultiNotes());
    if (!useLiveBasePreview)
      updateBasePitchCacheIfNeeded();

    const auto &basePitchCurve =
        useLiveBasePreview ? audioData.basePitch : cachedBasePitch;
    if (!basePitchCurve.empty())
    {
      const double visibleStartTime = scrollX / pixelsPerSecond;
      const double visibleEndTime = (scrollX + params.componentWidth) / pixelsPerSecond;
      const int visStartFrame =
          std::max(0, static_cast<int>(visibleStartTime * audioData.sampleRate /
                                       HOP_SIZE));
      const int visEndFrame = std::min(
          static_cast<int>(basePitchCurve.size()),
          static_cast<int>(visibleEndTime * audioData.sampleRate / HOP_SIZE) + 1);

      g.setColour(APP_COLOR_SECONDARY.withAlpha(0.6f));
      juce::Path basePath;
      bool basePathStarted = false;

      for (int i = visStartFrame; i < visEndFrame; ++i)
      {
        if (i >= 0 && i < static_cast<int>(basePitchCurve.size()))
        {
          const float baseMidi = basePitchCurve[static_cast<size_t>(i)];
          if (baseMidi > 0.0f)
          {
            const float x = framesToSeconds(i) * pixelsPerSecond;
            const float y = coordMapper->midiToY(baseMidi) +
                            pixelsPerSemitone * 0.5f;

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
            // breaking.
            juce::Path dashedPath;
            juce::PathStrokeType stroke(1.5f);
            const float dashLengths[] = {4.0f, 4.0f};
            stroke.createDashedStroke(dashedPath, basePath, dashLengths, 2);
            g.strokePath(dashedPath, juce::PathStrokeType(1.5f));
            basePath.clear();
            basePathStarted = false;
          }
        }
      }

      if (basePathStarted)
      {
        juce::Path dashedPath;
        juce::PathStrokeType stroke(1.5f);
        const float dashLengths[] = {4.0f, 4.0f};
        stroke.createDashedStroke(dashedPath, basePath, dashLengths, 2);
        g.strokePath(dashedPath, juce::PathStrokeType(1.5f));
      }
    }
  }
}
