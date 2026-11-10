#include "PitchCurveRenderer.h"
#include "PitchEditor.h"
#include "States/SelectHandler.h"
#include "../../Audio/BreathSibilanceAnalysis.h"
#include "../../Utils/BasePitchCurve.h"
#include "../../Utils/PitchCurveProcessor.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>

void PitchCurveRenderer::beginMacroPreview(const std::vector<Note*>& notes)
{
  if (!project || !macroPreview.empty()) return;
  updateBasePitchCacheIfNeeded();
  for (const auto* note : notes)
    if (note)
    {
      macroPreview.emplace(note, MacroPreview{note->getMidiNote(), note->getPitchDrift(),
          PitchCurveProcessor::getDriftPreviewResponse(*project, *note),
          note->getMidiNote(), note->getPitchDrift()});
      macroNotes.push_back(note);
    }
  std::sort(macroNotes.begin(), macroNotes.end(), [](const Note* a, const Note* b)
  { return a->getStartFrame() < b->getStartFrame(); });
  lastMacroPreviewUpdate = 0.0;
}

float PitchCurveRenderer::macroOffset(const Note& note, int frame, bool includeDrift) const
{
  const auto it = macroPreview.find(&note);
  if (it == macroPreview.end()) return 0.0f;
  const auto& preview = it->second;
  float offset = preview.displayMidi - preview.midi;
  const int local = frame - note.getStartFrame();
  if (includeDrift && local >= 0 && local < static_cast<int>(preview.driftResponse.size()))
    offset += (preview.displayDrift - preview.drift) * preview.driftResponse[static_cast<size_t>(local)];
  return offset;
}

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

  const double now = juce::Time::getMillisecondCounterHiRes();
  if (!macroPreview.empty() && now - lastMacroPreviewUpdate >= 33.0)
  {
    for (auto& entry : macroPreview)
    {
      entry.second.displayMidi = entry.first->getMidiNote();
      entry.second.displayDrift = entry.first->getPitchDrift();
    }
    lastMacroPreviewUpdate = now;
  }
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
    const auto curveColour =
        greyscale ? APP_COLOR_PITCH_CURVE.withSaturation(0.0f)
                  : APP_COLOR_PITCH_CURVE;
    g.setColour(curveColour.withMultipliedAlpha(
        juce::jlimit(0.0f, 1.0f, params.pitchCurveAlpha)));

    juce::Path path;
    bool pathStarted = false;
    auto strokeCurrentPath = [&]()
    {
      if (pathStarted)
        g.strokePath(path, juce::PathStrokeType(2.0f));
      path.clear();
      pathStarted = false;
    };

    // Breath and sibilance frames break the curve. Inside the dashNoise range
    // they are drawn dashed instead, one run at a time: createDashedStroke
    // joins the separate sub-paths of a single path with straight lines.
    juce::Path dashedRun;
    bool dashedStarted = false;
    auto strokeDashedRun = [&]()
    {
      if (dashedStarted)
      {
        const float dashLengths[] = {5.0f, 4.0f};
        juce::Path dashes;
        juce::PathStrokeType(2.0f).createDashedStroke(dashes, dashedRun,
                                                      dashLengths, 2);
        g.fillPath(dashes);
      }
      dashedRun.clear();
      dashedStarted = false;
    };
    juce::Point<float> previousPoint;
    bool hasPreviousPoint = false;
    bool previousWasDashed = false;
    auto breakCurve = [&]()
    {
      strokeCurrentPath();
      strokeDashedRun();
      hasPreviousPoint = false;
    };

    const auto &chunkRanges = audioData.segmentChunkRanges;
    auto getRegionIndex = [&chunkRanges](const Note &note)
    {
      if (chunkRanges.empty())
        return 0;

      const int midpoint = note.getSrcStartFrame() +
                           std::max(0, note.getSrcDurationFrames()) / 2;
      for (size_t i = 0; i < chunkRanges.size(); ++i)
      {
        if (midpoint >= chunkRanges[i].first &&
            midpoint < chunkRanges[i].second)
          return static_cast<int>(i);
      }
      return -1;
    };

    bool hasPreviousNote = false;
    float previousVisualEndFrame = -1.0f;
    int previousRegion = -1;

    // Only build path geometry for notes that can touch the viewport. Without
    // this, every repaint (drag, zoom, scroll) walked and stroked the pitch
    // curve of the entire project, which scales with song length and is
    // especially costly with the Direct2D renderer (geometry is rebuilt and
    // tessellated per frame). Path coordinates are in world space, so the
    // visible range is [scrollX, scrollX + componentWidth].
    constexpr float cullPaddingPx = 16.0f;
    const float cullLeft = static_cast<float>(scrollX) - cullPaddingPx;
    const float cullRight = static_cast<float>(scrollX) +
                            static_cast<float>(params.componentWidth) +
                            cullPaddingPx;

    for (const auto &note : project->getNotes())
    {
      if (note.isRest())
        continue;

      {
        const float cullStartFrame = note.getVisualStartFrame();
        const float cullEndFrame = note.getVisualEndFrame();
        const float noteLeft =
            framesToSeconds(std::min(cullStartFrame, cullEndFrame) - 1.0f) *
            pixelsPerSecond;
        const float noteRight =
            framesToSeconds(std::max(cullStartFrame, cullEndFrame) + 1.0f) *
            pixelsPerSecond;
        if (noteRight < cullLeft || noteLeft > cullRight)
        {
          // Break the polyline here so the next visible note does not join
          // across the skipped (off-screen) span.
          breakCurve();
          hasPreviousNote = false;
          continue;
        }
      }

      const int startFrame = note.getStartFrame();
      const int endFrame =
          std::min(note.getEndFrame(), static_cast<int>(audioData.f0.size()));
      const float visualStartFrame = note.getVisualStartFrame();
      const float visualEndFrame = note.getVisualEndFrame();
      const int currentRegion = getRegionIndex(note);
      const bool continuesPreviousPath =
          hasPreviousNote &&
          std::abs(visualStartFrame - previousVisualEndFrame) < 0.001f &&
          currentRegion >= 0 && currentRegion == previousRegion;

      if (!continuesPreviousPath)
        breakCurve();

      // The curve is laid out over the note's committed frames and read
      // through its time map (NoteTiming.h), as the synthesizer reads it.
      // While a timing drag is in progress, each frame is drawn where the
      // same recorded frame will play: a stretched vowel spreads out, the
      // consonants keep their width.
      const auto committed = notetiming::forNote(audioData, note);
      const bool previewing = note.hasTimingPreview() &&
                              note.getSrcDurationFrames() > 0 &&
                              note.getDurationFrames() > 0 &&
                              visualEndFrame > visualStartFrame;
      const auto onScreen = previewing
                                ? notetiming::forNoteOnScreen(audioData, note)
                                : notetiming::TimeMap{};
      const auto visualFrameOf = [&](int frame)
      {
        if (!previewing)
          return static_cast<float>(frame);
        return static_cast<float>(
            onScreen.destinationAt(
                committed.sourceAt(static_cast<double>(frame) + 0.5)) -
            0.5);
      };
      const auto committedFrameOf = [&](double visualFrame)
      {
        if (!previewing)
          return visualFrame;
        return committed.destinationAt(onScreen.sourceAt(visualFrame + 0.5)) -
               0.5;
      };

      // Clip long notes to the viewport before constructing the path.
      int firstVisible = startFrame;
      int lastVisible = endFrame;
      if (visualEndFrame > visualStartFrame)
      {
        const double left = scrollX / pixelsPerSecond * audioData.sampleRate / HOP_SIZE;
        const double right = (scrollX + params.componentWidth) / pixelsPerSecond
                             * audioData.sampleRate / HOP_SIZE;
        firstVisible = std::max(startFrame, static_cast<int>(
            std::floor(committedFrameOf(left))) - 1);
        lastVisible = std::min(endFrame, static_cast<int>(
            std::ceil(committedFrameOf(right))) + 1);
      }
      for (int i = firstVisible; i < lastVisible; ++i)
      {
        float finalMidi = 0.0f;
        if (params.midiCurveOverride &&
            i < static_cast<int>(params.midiCurveOverride->size()))
        {
          finalMidi = (*params.midiCurveOverride)[static_cast<size_t>(i)] +
                      globalOffset;
        }
        else
        {
          const float baseMidi =
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
          finalMidi = baseMidi + deltaMidi + globalOffset + macroOffset(note, i, true);
        }

        const bool noise = breathsibilance::isNoise(
            breathsibilance::labelAtNoteFrame(audioData, committed, i));
        const bool dashed = noise && i >= params.dashNoiseFirstFrame &&
                            i <= params.dashNoiseLastFrame;
        if (finalMidi > 0.0f && (!noise || dashed))
        {
          const float x = framesToSeconds(visualFrameOf(i)) * pixelsPerSecond;
          const float y = coordMapper->midiToY(finalMidi) +
                          pixelsPerSemitone * 0.5f;
          const juce::Point<float> point(x, y);
          if (dashed || (hasPreviousPoint && previousWasDashed))
          {
            // Any segment touching a dashed frame is dashed, so the solid and
            // dashed runs meet without a gap.
            if (!dashedStarted)
            {
              dashedRun.startNewSubPath(hasPreviousPoint ? previousPoint
                                                         : point);
              dashedStarted = true;
            }
            dashedRun.lineTo(point);
            if (dashed)
            {
              strokeCurrentPath();
            }
            else
            {
              strokeDashedRun();
              path.startNewSubPath(point);
              pathStarted = true;
            }
          }
          else if (!pathStarted)
          {
            path.startNewSubPath(point);
            pathStarted = true;
          }
          else
          {
            path.lineTo(point);
          }
          previousPoint = point;
          previousWasDashed = dashed;
          hasPreviousPoint = true;
        }
        else
        {
          breakCurve();
        }
      }

      hasPreviousNote = true;
      previousVisualEndFrame = visualEndFrame;
      previousRegion = currentRegion;
    }

    strokeCurrentPath();
    strokeDashedRun();
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

      g.setColour((greyscale ? APP_COLOR_SECONDARY.withSaturation(0.0f)
                             : APP_COLOR_SECONDARY)
                      .withAlpha(0.6f));
      juce::Path basePath;
      bool basePathStarted = false;

      for (int i = visStartFrame; i < visEndFrame; ++i)
      {
        if (i >= 0 && i < static_cast<int>(basePitchCurve.size()))
        {
          float baseMidi = basePitchCurve[static_cast<size_t>(i)];
          if (!macroNotes.empty())
          {
            auto it = std::upper_bound(macroNotes.begin(), macroNotes.end(), i,
                [](int frame, const Note* note) { return frame < note->getStartFrame(); });
            if (it != macroNotes.begin())
            {
              const auto* note = *std::prev(it);
              if (i < note->getEndFrame())
                baseMidi += macroOffset(*note, i, false);
            }
          }
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
