#include "IncrementalSynthesizer.h"
#include "../../Utils/Constants.h"
#include "../../Utils/Localization.h"
#include "../../Utils/TimingRegionUtils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
constexpr int kAdjacentFrameTolerance = 1;

struct EditedClusterSelection {
  std::vector<uint8_t> members;
  int startFrame = -1;
  int endFrame = -1;
};

struct PreservedTimingSpan {
  int sourceStart = 0;
  int sourceEnd = 0;
  int destinationStart = 0;
  int destinationEnd = 0;
  bool leading = false;
};

struct AudioMovePatch {
  int destinationStartSample = 0;
  std::vector<float> samples;
  int clearStartSample = -1;
  int clearEndSample = -1;
  bool releaseBeforeClear = false;
  bool attackAfterClear = false;
};

struct CommitFrameRange {
  int start = 0;
  int end = 0;
};

bool hasTimingEdit(const Note &note) {
  return note.getStartFrame() != note.getSrcStartFrame() ||
         note.getEndFrame() != note.getSrcEndFrame();
}

// A timing edit needs a broad waveform commit only while its output position
// differs from the position already represented in the composite waveform.
// A note can remain timing-edited after that render, so comparing against the
// immutable source bounds here would incorrectly broaden later pitch commits.
bool hasPendingTimingPositionChange(const Note &note) {
  return note.getStartFrame() != note.getRenderedStartFrame() ||
         note.getEndFrame() != note.getRenderedEndFrame();
}

std::vector<PreservedTimingSpan>
getPreservedTimingSpans(const Project &project, const Note &note) {
  std::vector<PreservedTimingSpan> spans;
  if (note.isRest())
    return spans;

  const auto region = timingRegions::getSourceRegion(project, note);
  if (note.getStartFrame() != note.getSrcStartFrame() &&
      timingRegions::isFirstNote(project, note)) {
    const int offset = note.getStartFrame() - note.getSrcStartFrame();
    if (region.start < note.getSrcStartFrame())
      spans.push_back({region.start, note.getSrcStartFrame(),
                       region.start + offset, note.getStartFrame(), true});
  }
  if (note.getEndFrame() != note.getSrcEndFrame() &&
      timingRegions::isLastNote(project, note)) {
    const int offset = note.getEndFrame() - note.getSrcEndFrame();
    if (note.getSrcEndFrame() < region.end)
      spans.push_back({note.getSrcEndFrame(), region.end, note.getEndFrame(),
                       region.end + offset, false});
  }
  return spans;
}

std::vector<float> interpolateMelFrame(
    const std::vector<std::vector<float>> &mel, double sourceFrame) {
  if (mel.empty())
    return {};

  sourceFrame = std::clamp(sourceFrame, 0.0,
                           static_cast<double>(mel.size() - 1));
  const int left = static_cast<int>(std::floor(sourceFrame));
  const int right = std::min(left + 1, static_cast<int>(mel.size()) - 1);
  const float amount = static_cast<float>(sourceFrame - left);
  const size_t bins = std::min(mel[static_cast<size_t>(left)].size(),
                               mel[static_cast<size_t>(right)].size());
  std::vector<float> frame(bins, 0.0f);
  for (size_t bin = 0; bin < bins; ++bin)
    frame[bin] = mel[static_cast<size_t>(left)][bin] * (1.0f - amount) +
                 mel[static_cast<size_t>(right)][bin] * amount;
  return frame;
}

void retimeMelInterval(const std::vector<std::vector<float>> &sourceMel,
                       int sourceStart, int sourceEnd, int destinationStart,
                       int destinationEnd, int rangeStart, int rangeEnd,
                       std::vector<std::vector<float>> &melRange) {
  const int sourceLength = sourceEnd - sourceStart;
  const int destinationLength = destinationEnd - destinationStart;
  if (sourceLength <= 0 || destinationLength <= 0)
    return;

  const int overlapStart = std::max(rangeStart, destinationStart);
  const int overlapEnd = std::min(rangeEnd, destinationEnd);
  for (int frame = overlapStart; frame < overlapEnd; ++frame) {
    const double normalized =
        (static_cast<double>(frame - destinationStart) + 0.5) /
        static_cast<double>(destinationLength);
    const double sourceFrame = static_cast<double>(sourceStart) +
                               normalized * sourceLength - 0.5;
    melRange[static_cast<size_t>(frame - rangeStart)] =
        interpolateMelFrame(sourceMel, sourceFrame);
  }
}

void moveMelInterval(const std::vector<std::vector<float>> &sourceMel,
                     const PreservedTimingSpan &span, int rangeStart,
                     int rangeEnd,
                     std::vector<std::vector<float>> &melRange) {
  const int overlapStart = std::max(rangeStart, span.destinationStart);
  const int overlapEnd = std::min(rangeEnd, span.destinationEnd);
  for (int frame = overlapStart; frame < overlapEnd; ++frame) {
    const int sourceFrame = span.sourceStart + frame - span.destinationStart;
    if (sourceFrame >= 0 && sourceFrame < static_cast<int>(sourceMel.size()))
      melRange[static_cast<size_t>(frame - rangeStart)] =
          sourceMel[static_cast<size_t>(sourceFrame)];
  }
}

void applyNoteTimingToMel(const Project &project, int rangeStart,
                          int rangeEnd,
                          std::vector<std::vector<float>> &melRange) {
  const auto &sourceMel = project.getAudioData().melSpectrogram;
  for (const auto &note : project.getNotes()) {
    if (note.isRest() || !hasTimingEdit(note))
      continue;

    for (const auto &span : getPreservedTimingSpans(project, note))
      moveMelInterval(sourceMel, span, rangeStart, rangeEnd, melRange);

    retimeMelInterval(sourceMel, note.getSrcStartFrame(),
                      note.getSrcEndFrame(), note.getStartFrame(),
                      note.getEndFrame(), rangeStart, rangeEnd, melRange);
  }
}

void moveSourceFrameInterval(const PreservedTimingSpan &span, int rangeStart,
                             int rangeEnd,
                             std::vector<double> &sourceFrameMap) {
  const int overlapStart = std::max(rangeStart, span.destinationStart);
  const int overlapEnd = std::min(rangeEnd, span.destinationEnd);
  for (int frame = overlapStart; frame < overlapEnd; ++frame)
    sourceFrameMap[static_cast<size_t>(frame - rangeStart)] =
        static_cast<double>(span.sourceStart + frame - span.destinationStart);
}

void retimeSourceFrameInterval(int sourceStart, int sourceEnd,
                               int destinationStart, int destinationEnd,
                               int rangeStart, int rangeEnd,
                               std::vector<double> &sourceFrameMap) {
  const int sourceLength = sourceEnd - sourceStart;
  const int destinationLength = destinationEnd - destinationStart;
  if (sourceLength <= 0 || destinationLength <= 0)
    return;

  const int overlapStart = std::max(rangeStart, destinationStart);
  const int overlapEnd = std::min(rangeEnd, destinationEnd);
  for (int frame = overlapStart; frame < overlapEnd; ++frame) {
    const double normalized =
        (static_cast<double>(frame - destinationStart) + 0.5) /
        static_cast<double>(destinationLength);
    sourceFrameMap[static_cast<size_t>(frame - rangeStart)] =
        static_cast<double>(sourceStart) + normalized * sourceLength - 0.5;
  }
}

// The phase-vocoder counterpart of applyNoteTimingToMel(): the vocoder path
// retimes the mel it feeds the model, this path retimes where the analysis
// window reads from. Same spans, same order, so both engines place a
// retimed note identically.
void applyNoteTimingToSourceFrames(const Project &project, int rangeStart,
                                   int rangeEnd,
                                   std::vector<double> &sourceFrameMap) {
  for (const auto &note : project.getNotes()) {
    if (note.isRest() || !hasTimingEdit(note))
      continue;

    for (const auto &span : getPreservedTimingSpans(project, note))
      moveSourceFrameInterval(span, rangeStart, rangeEnd, sourceFrameMap);

    retimeSourceFrameInterval(note.getSrcStartFrame(), note.getSrcEndFrame(),
                              note.getStartFrame(), note.getEndFrame(),
                              rangeStart, rangeEnd, sourceFrameMap);
  }
}

float interpolateF0(const std::vector<float> &f0, double frame) {
  if (f0.empty())
    return 0.0f;

  frame = std::clamp(frame, 0.0, static_cast<double>(f0.size() - 1));
  const int left = static_cast<int>(std::floor(frame));
  const int right = std::min(left + 1, static_cast<int>(f0.size()) - 1);
  const float leftValue = f0[static_cast<size_t>(left)];
  const float rightValue = f0[static_cast<size_t>(right)];

  // Never interpolate across an unvoiced gap: averaging a real pitch with a
  // zero would halve it and swing the ratio wildly for one frame.
  if (leftValue <= 0.0f)
    return rightValue;
  if (rightValue <= 0.0f)
    return leftValue;

  const float amount = static_cast<float>(frame - left);
  return leftValue * (1.0f - amount) + rightValue * amount;
}

void applyPreservedTimingToF0(const Project &project, int rangeStart,
                              int rangeEnd,
                              std::vector<float> &adjustedF0Range) {
  const auto &audioData = project.getAudioData();
  const auto &sourceF0 =
      audioData.denseF0.empty() ? audioData.f0 : audioData.denseF0;
  if (sourceF0.empty() || adjustedF0Range.empty())
    return;

  for (const auto &note : project.getNotes()) {
    for (const auto &span : getPreservedTimingSpans(project, note)) {
      const int overlapStart = std::max(rangeStart, span.destinationStart);
      const int overlapEnd = std::min(rangeEnd, span.destinationEnd);
      for (int frame = overlapStart; frame < overlapEnd; ++frame) {
        const int sourceFrame =
            span.sourceStart + frame - span.destinationStart;
        if (sourceFrame >= 0 &&
            sourceFrame < static_cast<int>(sourceF0.size()))
          adjustedF0Range[static_cast<size_t>(frame - rangeStart)] =
              sourceF0[static_cast<size_t>(sourceFrame)];
      }
    }
  }
}

std::vector<CommitFrameRange>
collectCommitFrameRanges(const Project &project, bool hasDirtyNoteAnchors,
                         int f0DirtyStart, int f0DirtyEnd) {
  std::vector<CommitFrameRange> ranges;
  bool needsF0RangeCommit = !hasDirtyNoteAnchors;

  if (hasDirtyNoteAnchors) {
    for (const auto &note : project.getNotes()) {
      if (note.isRest() || !note.isDirty())
        continue;

      int start = note.getStartFrame();
      int end = note.getEndFrame();

      // Timing moves need their old and new output positions committed. A
      // previously rendered timing edit is not a pending move, so a later
      // pitch edit on that note keeps its waveform commit local.
      if (hasPendingTimingPositionChange(note)) {
        needsF0RangeCommit = true;
        start = std::min(
            {start, note.getSrcStartFrame(), note.getRenderedStartFrame()});
        end = std::max(
            {end, note.getSrcEndFrame(), note.getRenderedEndFrame()});
        for (const auto &span : getPreservedTimingSpans(project, note)) {
          start = std::min({start, span.sourceStart, span.destinationStart});
          end = std::max({end, span.sourceEnd, span.destinationEnd});
        }
      }

      if (end > start)
        ranges.push_back({start, end});
    }
  }

  // F0-only tools have no dirty-note anchor, so their edited curve range is
  // the commit range. Timing undo/reset also needs it to restore a vacated
  // destination. Ordinary pitch drags use this range only as render context;
  // committing it would replace neighboring notes' waveform.
  if (needsF0RangeCommit && f0DirtyStart >= 0 &&
      f0DirtyEnd > f0DirtyStart) {
    ranges.push_back({f0DirtyStart, f0DirtyEnd});
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const CommitFrameRange &a, const CommitFrameRange &b) {
              return a.start < b.start;
            });

  std::vector<CommitFrameRange> merged;
  for (const auto &range : ranges) {
    if (merged.empty() || range.start > merged.back().end) {
      merged.push_back(range);
    } else {
      merged.back().end = std::max(merged.back().end, range.end);
    }
  }
  return merged;
}

int findBestSpliceCenter(const float *existing,
                         const std::vector<float> &rendered,
                         int nominalCenter, int searchRadius,
                         int analysisHalfSamples) {
  const int sampleCount = static_cast<int>(rendered.size());
  if (existing == nullptr || sampleCount <= 0)
    return std::clamp(nominalCenter, 0, std::max(0, sampleCount - 1));

  const int firstCenter =
      std::max({1, analysisHalfSamples, nominalCenter - searchRadius});
  const int lastCenter = std::min(
      sampleCount - analysisHalfSamples - 1, nominalCenter + searchRadius);
  if (firstCenter > lastCenter)
    return std::clamp(nominalCenter, 0, sampleCount - 1);

  double bestScore = std::numeric_limits<double>::infinity();
  int bestCenter = std::clamp(nominalCenter, firstCenter, lastCenter);
  const double distanceScale = 1.0 / std::max(1, searchRadius);

  for (int center = firstCenter; center <= lastCenter; ++center) {
    double squaredError = 0.0;
    double signalEnergy = 1.0e-12;
    for (int offset = -analysisHalfSamples;
         offset <= analysisHalfSamples; ++offset) {
      const int sample = center + offset;
      const double weight =
          1.0 - static_cast<double>(std::abs(offset)) /
                    static_cast<double>(analysisHalfSamples + 1);
      const double oldSample = existing[sample];
      const double newSample = rendered[static_cast<size_t>(sample)];
      const double difference = oldSample - newSample;
      squaredError += weight * difference * difference;
      signalEnergy +=
          weight * (oldSample * oldSample + newSample * newSample);
    }

    const double oldValue = existing[center];
    const double newValue = rendered[static_cast<size_t>(center)];
    const double valueDifference = oldValue - newValue;
    const double valueEnergy =
        oldValue * oldValue + newValue * newValue + signalEnergy * 1.0e-3;

    const double oldSlope = existing[center] - existing[center - 1];
    const double newSlope = rendered[static_cast<size_t>(center)] -
                            rendered[static_cast<size_t>(center - 1)];
    const double slopeDifference = oldSlope - newSlope;
    const double slopeEnergy =
        oldSlope * oldSlope + newSlope * newSlope + signalEnergy * 1.0e-4;

    // Waveform agreement dominates. Matching the exact value and first
    // derivative at the splice center further suppresses residual clicks.
    const double score =
        squaredError / signalEnergy +
        0.25 * valueDifference * valueDifference / valueEnergy +
        0.10 * slopeDifference * slopeDifference / slopeEnergy +
        1.0e-6 * std::abs(center - nominalCenter) * distanceScale;
    if (score < bestScore) {
      bestScore = score;
      bestCenter = center;
    }
  }

  return bestCenter;
}

EditedClusterSelection
selectConnectedEditedNotes(const Project &project, bool hasDirtyNoteAnchors,
                           int f0DirtyStart, int f0DirtyEnd) {
  const auto &notes = project.getNotes();
  EditedClusterSelection selection;
  selection.members.assign(notes.size(), uint8_t{0});

  const bool hasF0DirtyRange = f0DirtyStart >= 0 && f0DirtyEnd >= 0;
  auto isF0Anchor = [&](const Note &note) {
    return !hasDirtyNoteAnchors && hasF0DirtyRange &&
           note.getStartFrame() < f0DirtyEnd &&
           note.getEndFrame() > f0DirtyStart;
  };
  auto isSeed = [&](const Note &note) {
    if (note.isRest())
      return false;
    if (isF0Anchor(note))
      return true;
    return note.isDirty() && !note.isNeutralForOriginalWaveform();
  };
  auto isEditedClusterCandidate = [&](const Note &note) {
    if (note.isRest())
      return false;

    // A dirty neutral note is being reset. It must split the edited cluster
    // instead of pulling its neighbours into the new pass.
    if (note.isDirty() && note.isNeutralForOriginalWaveform() &&
        !isF0Anchor(note))
      return false;

    return isF0Anchor(note) || note.hasRenderedEdit() ||
           !note.isNeutralForOriginalWaveform();
  };
  auto areAdjacent = [&](size_t leftIndex, size_t rightIndex) {
    const auto &left = notes[leftIndex];
    const auto &right = notes[rightIndex];
    return right.getStartFrame() - left.getEndFrame() <=
           kAdjacentFrameTolerance;
  };

  std::vector<size_t> sortedIndices;
  sortedIndices.reserve(notes.size());
  for (size_t i = 0; i < notes.size(); ++i)
    if (!notes[i].isRest())
      sortedIndices.push_back(i);
  std::sort(sortedIndices.begin(), sortedIndices.end(),
            [&](size_t a, size_t b) {
              return notes[a].getStartFrame() < notes[b].getStartFrame();
            });

  // Every dirty/F0-edited note seeds a component. Walk transitively through
  // adjacent edited notes so the vocoder receives one continuous phrase as
  // render context. A separate commit mask later limits waveform replacement
  // to the notes that were actually edited.
  for (size_t seedPosition = 0; seedPosition < sortedIndices.size();
       ++seedPosition) {
    const size_t seedIndex = sortedIndices[seedPosition];
    if (!isSeed(notes[seedIndex]))
      continue;

    selection.members[seedIndex] = 1;

    size_t leftPosition = seedPosition;
    while (leftPosition > 0) {
      const size_t candidateIndex = sortedIndices[leftPosition - 1];
      const size_t currentIndex = sortedIndices[leftPosition];
      if (!isEditedClusterCandidate(notes[candidateIndex]) ||
          !areAdjacent(candidateIndex, currentIndex))
        break;
      selection.members[candidateIndex] = 1;
      --leftPosition;
    }

    size_t rightPosition = seedPosition;
    while (rightPosition + 1 < sortedIndices.size()) {
      const size_t currentIndex = sortedIndices[rightPosition];
      const size_t candidateIndex = sortedIndices[rightPosition + 1];
      if (!isEditedClusterCandidate(notes[candidateIndex]) ||
          !areAdjacent(currentIndex, candidateIndex))
        break;
      selection.members[candidateIndex] = 1;
      ++rightPosition;
    }
  }

  for (size_t i = 0; i < notes.size(); ++i) {
    if (selection.members[i] == 0)
      continue;
    const auto &note = notes[i];
    if (selection.startFrame < 0 ||
        note.getStartFrame() < selection.startFrame)
      selection.startFrame = note.getStartFrame();
    if (selection.endFrame < 0 || note.getEndFrame() > selection.endFrame)
      selection.endFrame = note.getEndFrame();
  }

  return selection;
}
} // namespace

IncrementalSynthesizer::IncrementalSynthesizer() = default;

IncrementalSynthesizer::~IncrementalSynthesizer() { cancel(); }

void IncrementalSynthesizer::cancel() {
  if (cancelFlag)
    cancelFlag->store(true);
}

// ---------------------------------------------------------------------------
// computeSynthesisRange: find voiced segments overlapping dirty range,
// expand to include complete segments + padding.
// ---------------------------------------------------------------------------
std::pair<int, int>
IncrementalSynthesizer::computeSynthesisRange(int dirtyStart, int dirtyEnd) {
  if (!project)
    return {dirtyStart, dirtyEnd};

  auto &voicedMask = project->getAudioData().voicedMask;
  const int totalFrames = static_cast<int>(voicedMask.size());
  if (totalFrames == 0)
    return {dirtyStart, dirtyEnd};

  dirtyStart = std::max(0, dirtyStart);
  dirtyEnd = std::min(totalFrames, dirtyEnd);

  // Give the vocoder enough temporal context to stabilize local phase when
  // doing chunked re-synthesis.
  constexpr int kPadFrames = 24;
  // Bridge short UV gaps so adjacent notes around consonants are synthesized
  // together; this avoids junction phase resets between neighboring notes.
  constexpr int kGapBridgeFrames = 16;

  auto isVoiced = [&](int idx) -> bool {
    return idx >= 0 && idx < totalFrames && static_cast<bool>(voicedMask[idx]);
  };

  // Expand backward to include neighboring voiced segments across short gaps.
  int start = dirtyStart;
  int backGap = 0;
  while (start > 0) {
    if (isVoiced(start - 1)) {
      --start;
      backGap = 0;
      continue;
    }
    if (backGap < kGapBridgeFrames) {
      --start;
      ++backGap;
      continue;
    }
    break;
  }
  start = std::max(0, start - kPadFrames);

  // Expand forward to include neighboring voiced segments across short gaps.
  int end = dirtyEnd;
  int fwdGap = 0;
  while (end < totalFrames) {
    if (isVoiced(end)) {
      ++end;
      fwdGap = 0;
      continue;
    }
    if (fwdGap < kGapBridgeFrames) {
      ++end;
      ++fwdGap;
      continue;
    }
    break;
  }
  end = std::min(totalFrames, end + kPadFrames);

  return {start, end};
}

// ---------------------------------------------------------------------------
// generateBlendMask: per-sample blend factor from voicedMask.
// 1.0 = synthesized, 0.0 = original, smooth ramps at transitions.
// ---------------------------------------------------------------------------
std::vector<float>
IncrementalSynthesizer::generateBlendMask(int startFrame, int endFrame,
                                          int hopSize) {
  auto &voicedMask = project->getAudioData().voicedMask;
  const int totalFrames = static_cast<int>(voicedMask.size());
  const int numFrames = endFrame - startFrame;
  const int numSamples = numFrames * hopSize;

  // Step 1: stability-first frame mask.
  // Default to synthesized audio in the whole region to avoid internal
  // orig/synth combing artifacts at note junctions.
  std::vector<float> frameMask(numFrames, 1.0f);

  // Keep original audio only for long unvoiced runs (e.g. clear breaths/silence),
  // not for short UV gaps between notes.
  constexpr int kKeepOriginalUnvoicedFrames = 24;
  if (numFrames > 0 && totalFrames > 0) {
    int i = 0;
    while (i < numFrames) {
      const int gf = startFrame + i;
      const bool voiced =
          gf >= 0 && gf < totalFrames && static_cast<bool>(voicedMask[gf]);
      if (voiced) {
        ++i;
        continue;
      }

      const int runStart = i;
      while (i < numFrames) {
        const int g = startFrame + i;
        const bool v =
            g >= 0 && g < totalFrames && static_cast<bool>(voicedMask[g]);
        if (v)
          break;
        ++i;
      }
      const int runEnd = i;
      const int runLen = runEnd - runStart;
      if (runLen >= kKeepOriginalUnvoicedFrames) {
        for (int k = runStart; k < runEnd; ++k)
          frameMask[k] = 0.0f;
      }
    }
  }

  // Step 2: expand to per-sample (sample-and-hold)
  std::vector<float> mask(numSamples, 0.0f);
  for (int i = 0; i < numFrames; ++i) {
    int ss = i * hopSize;
    int se = std::min(ss + hopSize, numSamples);
    for (int s = ss; s < se; ++s)
      mask[s] = frameMask[i];
  }

  // Step 3: smooth transitions with linear ramp at frame boundaries
  constexpr int kMinRampSamples = 512;
  const int kRampSamples = std::max(kMinRampSamples, hopSize * 2);
  for (int i = 0; i < numFrames - 1; ++i) {
    if (frameMask[i] == frameMask[i + 1])
      continue;
    // Transition at frame boundary
    int center = (i + 1) * hopSize;
    int rampStart = std::max(0, center - kRampSamples / 2);
    int rampEnd = std::min(numSamples, center + kRampSamples / 2);
    float fromVal = frameMask[i];
    float toVal = frameMask[i + 1];
    for (int s = rampStart; s < rampEnd; ++s) {
      float t = static_cast<float>(s - rampStart) /
                static_cast<float>(rampEnd - rampStart);
      mask[s] = fromVal + (toVal - fromVal) * t;
    }
  }

  return mask;
}

// ---------------------------------------------------------------------------
// synthesizeRegion: Voiced-Only Blend approach.
// ---------------------------------------------------------------------------
std::vector<double>
IncrementalSynthesizer::buildSourceFrameMap(int startFrame, int endFrame) const {
  std::vector<double> sourceFrameMap;
  if (!project || endFrame <= startFrame)
    return sourceFrameMap;

  sourceFrameMap.resize(static_cast<size_t>(endFrame - startFrame));
  for (int frame = startFrame; frame < endFrame; ++frame)
    sourceFrameMap[static_cast<size_t>(frame - startFrame)] =
        static_cast<double>(frame);

  applyNoteTimingToSourceFrames(*project, startFrame, endFrame, sourceFrameMap);
  return sourceFrameMap;
}

std::vector<float> IncrementalSynthesizer::buildPitchRatioCurve(
    int startFrame, int endFrame, const std::vector<double> &sourceFrameMap,
    const std::vector<float> &targetF0) const {
  std::vector<float> ratio;
  if (!project || endFrame <= startFrame)
    return ratio;

  const auto &audioData = project->getAudioData();
  const auto &sourceF0 =
      audioData.denseF0.empty() ? audioData.f0 : audioData.denseF0;

  const int numFrames = endFrame - startFrame;
  ratio.assign(static_cast<size_t>(numFrames), 1.0f);
  if (sourceF0.empty())
    return ratio;

  for (int frame = 0; frame < numFrames; ++frame) {
    const size_t index = static_cast<size_t>(frame);
    if (index >= targetF0.size() || index >= sourceFrameMap.size())
      break;

    const float target = targetF0[index];
    const float source = interpolateF0(sourceF0, sourceFrameMap[index]);

    // An unvoiced frame has no meaningful pitch to move. Those frames come
    // from the original side of the blend anyway, so leaving the ratio at
    // unity keeps the transform out of their way.
    if (target > 0.0f && source > 0.0f)
      ratio[index] = target / source;
  }

  return ratio;
}

void IncrementalSynthesizer::synthesizeRegion(ProgressCallback onProgress,
                                              CompleteCallback onComplete) {
  // PSOLA transforms the original waveform, so it needs neither the model nor
  // a loaded one, and it does not want the mel.
  const bool usePsola = synthesisEngine == SynthesisEngineType::Psola;

  if (!project || (!usePsola && !vocoder)) {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto &audioData = project->getAudioData();
  if (audioData.melSpectrogram.empty() || audioData.f0.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!usePsola && !vocoder->isLoaded()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (usePsola && audioData.originalWaveform.getNumSamples() == 0 &&
      audioData.waveform.getNumSamples() == 0) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!project->hasDirtyNotes() && !project->hasF0DirtyRange()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto [dirtyStart, dirtyEnd] = project->getDirtyFrameRange();
  if (dirtyStart < 0 || dirtyEnd < 0) {
    if (onComplete)
      onComplete(false);
    return;
  }
  const bool hasDirtyNoteAnchors = project->hasDirtyNotes();
  const auto [f0DirtyStart, f0DirtyEnd] = project->getF0DirtyRange();
  auto commitFrameRanges = collectCommitFrameRanges(
      *project, hasDirtyNoteAnchors, f0DirtyStart, f0DirtyEnd);
  if (commitFrameRanges.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }
  auto editedClusters = selectConnectedEditedNotes(
      *project, hasDirtyNoteAnchors, f0DirtyStart, f0DirtyEnd);
  if (editedClusters.startFrame >= 0)
    dirtyStart = std::min(dirtyStart, editedClusters.startFrame);
  if (editedClusters.endFrame >= 0)
    dirtyEnd = std::max(dirtyEnd, editedClusters.endFrame);

  // Compute synthesis range after expanding to complete connected edited-note
  // clusters, then add voiced-segment context and padding.
  auto [startFrame, endFrame] = computeSynthesisRange(dirtyStart, dirtyEnd);
  startFrame = std::max(0, startFrame);
  endFrame =
      std::min(static_cast<int>(audioData.melSpectrogram.size()), endFrame);

  if (startFrame >= endFrame) {
    if (onComplete)
      onComplete(false);
    return;
  }

  // Generate blend mask before async call (voicedMask is stable here).
  // Both engines share the analysis frame grid; the phase vocoder just may
  // not have a model to ask for it.
  const int hopSize = (vocoder != nullptr && vocoder->isLoaded())
                          ? vocoder->getHopSize()
                          : HOP_SIZE;
  std::vector<float> blendMask = generateBlendMask(startFrame, endFrame, hopSize);

  // Timing edits cannot be mixed with pristine samples at their destination
  // positions: even an originally unvoiced/consonant frame has moved. Keep
  // the entire retimed note on the synthesized side of the blend.
  for (const auto &note : project->getNotes()) {
    if (note.isRest() || !hasTimingEdit(note))
      continue;
    int timingStart = std::min(note.getSrcStartFrame(), note.getStartFrame());
    int timingEnd = std::max(note.getSrcEndFrame(), note.getEndFrame());
    for (const auto &span : getPreservedTimingSpans(*project, note)) {
      timingStart = std::min(
          {timingStart, span.sourceStart, span.destinationStart});
      timingEnd =
          std::max({timingEnd, span.sourceEnd, span.destinationEnd});
    }
    const int first = std::max(startFrame, timingStart);
    const int last = std::min(endFrame, timingEnd);
    const int firstSample = (first - startFrame) * hopSize;
    const int lastSample = (last - startFrame) * hopSize;
    for (int sample = std::max(0, firstSample);
         sample < std::min(static_cast<int>(blendMask.size()), lastSample);
         ++sample)
      blendMask[static_cast<size_t>(sample)] = 1.0f;
  }

  // Early exit: if blend mask is all-zero, nothing to synthesize
  bool hasVoiced = std::any_of(blendMask.begin(), blendMask.end(),
                               [](float v) { return v > 0.0f; });
  if (!hasVoiced) {
    project->clearAllDirty();
    if (onComplete)
      onComplete(true);
    return;
  }

  // Copy original waveform segment for blending
  const auto &origWaveform = audioData.originalWaveform.getNumSamples() > 0
                                 ? audioData.originalWaveform
                                 : audioData.waveform;
  int startSample = startFrame * hopSize;
  int numSynthSamples = (endFrame - startFrame) * hopSize;
  int totalOrigSamples = origWaveform.getNumSamples();

  std::vector<float> originalSegment(numSynthSamples, 0.0f);
  {
    const float *origPtr = origWaveform.getReadPointer(0);
    int copyLen = std::min(numSynthSamples,
                           std::max(0, totalOrigSamples - startSample));
    if (copyLen > 0 && startSample >= 0)
      std::copy(origPtr + startSample, origPtr + startSample + copyLen,
                originalSegment.begin());
  }

  // Preserve the audio attached outside a region's first/last pitched note as
  // immutable waveform patches. They are captured before the async render so
  // repeated edits and undo do not accumulate synthesis or splice artifacts.
  std::vector<AudioMovePatch> audioMovePatches;
  if (origWaveform.getNumChannels() > 0) {
    const float *origPtr = origWaveform.getReadPointer(0);
    std::vector<CommitFrameRange> clearFrameRanges;
    auto addClearFrameRange = [&](int start, int end) {
      start = std::clamp(start, 0, audioData.getNumFrames());
      end = std::clamp(end, start, audioData.getNumFrames());
      if (end > start)
        clearFrameRanges.push_back({start, end});
    };
    auto addClearPatch = [&](int startSample, int endSample,
                             bool releaseBefore, bool attackAfter) {
      startSample = std::clamp(startSample, 0, totalOrigSamples);
      endSample = std::clamp(endSample, startSample, totalOrigSamples);
      if (endSample <= startSample)
        return;

      AudioMovePatch patch;
      patch.clearStartSample = startSample;
      patch.clearEndSample = endSample;
      patch.releaseBeforeClear = releaseBefore;
      patch.attackAfterClear = attackAfter;
      audioMovePatches.push_back(std::move(patch));
    };

    for (const auto &note : project->getNotes()) {
      if (note.isRest())
        continue;

      const auto region = timingRegions::getSourceRegion(*project, note);

      // A region edge moving inward from its last rendered position vacates
      // that part of the composite. Compare against the rendered edge rather
      // than only the immutable source edge so extend-then-shorten sequences
      // clear the previous extension as well.
      if (note.isDirty() && timingRegions::isFirstNote(*project, note) &&
          note.getStartFrame() > note.getRenderedStartFrame()) {
        const int renderedOffset =
            note.getRenderedStartFrame() - note.getSrcStartFrame();
        const int destinationOffset =
            note.getStartFrame() - note.getSrcStartFrame();
        addClearFrameRange(region.start + renderedOffset,
                           region.start + destinationOffset);
      }
      if (note.isDirty() && timingRegions::isLastNote(*project, note) &&
          note.getEndFrame() < note.getRenderedEndFrame()) {
        const int destinationOffset =
            note.getEndFrame() - note.getSrcEndFrame();
        const int renderedOffset =
            note.getRenderedEndFrame() - note.getSrcEndFrame();
        addClearFrameRange(region.end + destinationOffset,
                           region.end + renderedOffset);
      }

      // Keep every currently vacated source interval clear. A later edit to an
      // adjacent region can otherwise blend immutable waveform samples back
      // into a gap created by an earlier timing edit.
      if (timingRegions::isFirstNote(*project, note) &&
          note.getStartFrame() > note.getSrcStartFrame()) {
        const int offset = note.getStartFrame() - note.getSrcStartFrame();
        addClearFrameRange(region.start, region.start + offset);
      }
      if (timingRegions::isLastNote(*project, note) &&
          note.getEndFrame() < note.getSrcEndFrame()) {
        const int offset = note.getEndFrame() - note.getSrcEndFrame();
        addClearFrameRange(region.end + offset, region.end);
      }

      if (!note.isDirty() || !hasTimingEdit(note))
        continue;

      for (const auto &span : getPreservedTimingSpans(*project, note)) {
        const int sourceStartSample = span.sourceStart * hopSize;
        const int sourceEndSample = span.sourceEnd * hopSize;
        const int destinationStartSample = span.destinationStart * hopSize;
        const int sampleCount = sourceEndSample - sourceStartSample;
        if (sampleCount <= 0 || sourceStartSample < 0 ||
            sourceEndSample > totalOrigSamples || destinationStartSample < 0 ||
            destinationStartSample + sampleCount > totalOrigSamples)
          continue;

        AudioMovePatch patch;
        patch.destinationStartSample = destinationStartSample;
        patch.samples.assign(origPtr + sourceStartSample,
                             origPtr + sourceEndSample);
        audioMovePatches.push_back(std::move(patch));
      }
    }

    // Merge overlapping clear requests, then remove any interval occupied by
    // the current region layout. This lets an adjacent region extend into a
    // previously vacated area while the remainder of the gap stays silent.
    std::sort(clearFrameRanges.begin(), clearFrameRanges.end(),
              [](const CommitFrameRange &a, const CommitFrameRange &b) {
                return a.start < b.start ||
                       (a.start == b.start && a.end < b.end);
              });
    std::vector<CommitFrameRange> mergedClearRanges;
    for (const auto &range : clearFrameRanges) {
      if (mergedClearRanges.empty() ||
          range.start > mergedClearRanges.back().end) {
        mergedClearRanges.push_back(range);
      } else {
        mergedClearRanges.back().end =
            std::max(mergedClearRanges.back().end, range.end);
      }
    }

    std::vector<CommitFrameRange> occupiedRegionRanges;
    for (int index = 0; index < timingRegions::regionCount(*project); ++index) {
      const auto region = timingRegions::regionAt(*project, index);
      const int currentStart =
          static_cast<int>(std::lround(timingRegions::visualStart(*project,
                                                                  region)));
      const int currentEnd =
          static_cast<int>(std::lround(timingRegions::visualEnd(*project,
                                                                region)));
      if (currentEnd > currentStart)
        occupiedRegionRanges.push_back({currentStart, currentEnd});
    }
    std::sort(occupiedRegionRanges.begin(), occupiedRegionRanges.end(),
              [](const CommitFrameRange &a, const CommitFrameRange &b) {
                return a.start < b.start;
              });

    std::vector<CommitFrameRange> unoccupiedClearRanges;
    for (const auto &clearRange : mergedClearRanges) {
      int cursor = clearRange.start;
      for (const auto &occupied : occupiedRegionRanges) {
        if (occupied.end <= cursor)
          continue;
        if (occupied.start >= clearRange.end)
          break;
        if (occupied.start > cursor)
          unoccupiedClearRanges.push_back(
              {cursor, std::min(occupied.start, clearRange.end)});
        cursor = std::max(cursor, occupied.end);
        if (cursor >= clearRange.end)
          break;
      }
      if (cursor < clearRange.end)
        unoccupiedClearRanges.push_back({cursor, clearRange.end});
    }

    // If a vacated interval lies between two current regions, clear the whole
    // current gap. The release and attack are then placed inside the adjacent
    // regions instead of leaving fade remnants in the gap itself.
    std::vector<CommitFrameRange> currentGapRanges;
    for (const auto &range : unoccupiedClearRanges) {
      CommitFrameRange gap = range;
      bool hasLeftRegion = false;
      bool hasRightRegion = false;
      for (const auto &occupied : occupiedRegionRanges) {
        if (occupied.end <= range.start) {
          gap.start = occupied.end;
          hasLeftRegion = true;
          continue;
        }
        if (occupied.start >= range.end) {
          gap.end = occupied.start;
          hasRightRegion = true;
          break;
        }
      }
      if (!hasLeftRegion)
        gap.start = range.start;
      if (!hasRightRegion)
        gap.end = range.end;
      if (gap.end > gap.start)
        currentGapRanges.push_back(gap);
    }
    std::sort(currentGapRanges.begin(), currentGapRanges.end(),
              [](const CommitFrameRange &a, const CommitFrameRange &b) {
                return a.start < b.start ||
                       (a.start == b.start && a.end < b.end);
              });
    std::vector<CommitFrameRange> mergedGapRanges;
    for (const auto &range : currentGapRanges) {
      if (mergedGapRanges.empty() || range.start > mergedGapRanges.back().end)
        mergedGapRanges.push_back(range);
      else
        mergedGapRanges.back().end =
            std::max(mergedGapRanges.back().end, range.end);
    }

    for (const auto &gap : mergedGapRanges) {
      const bool releaseBefore = std::any_of(
          occupiedRegionRanges.begin(), occupiedRegionRanges.end(),
          [&](const CommitFrameRange &occupied) {
            return occupied.end == gap.start;
          });
      const bool attackAfter = std::any_of(
          occupiedRegionRanges.begin(), occupiedRegionRanges.end(),
          [&](const CommitFrameRange &occupied) {
            return occupied.start == gap.end;
          });
      addClearPatch(gap.start * hopSize, gap.end * hopSize, releaseBefore,
                    attackAfter);
    }
  }

  std::vector<float> adjustedF0Range =
      project->getAdjustedF0ForRange(startFrame, endFrame);
  applyPreservedTimingToF0(*project, startFrame, endFrame, adjustedF0Range);

  if (adjustedF0Range.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  // Mel is the neural vocoder's input and is not cheap to copy, so the PSOLA
  // path skips building it entirely.
  std::vector<std::vector<float>> melRange;
  if (!usePsola) {
    melRange.assign(audioData.melSpectrogram.begin() + startFrame,
                    audioData.melSpectrogram.begin() + endFrame);
    applyNoteTimingToMel(*project, startFrame, endFrame, melRange);

    if (melRange.empty()) {
      if (onComplete)
        onComplete(false);
      return;
    }
  }

  // PSOLA's two input curves: where each output frame reads from in the
  // original audio, and how far its pitch has to move once it gets there.
  // Built here on the message thread while the project is stable.
  DspSynthesisRequest dspRequest;
  if (usePsola) {
    const auto sourceFrameMap = buildSourceFrameMap(startFrame, endFrame);
    auto pitchRatio =
        buildPitchRatioCurve(startFrame, endFrame, sourceFrameMap, adjustedF0Range);

    if (sourceFrameMap.empty() || pitchRatio.empty()) {
      if (onComplete)
        onComplete(false);
      return;
    }

    // Copy only the span the curves actually reach, plus a window of context
    // on each side, rather than the whole track on every keystroke.
    double minSourceFrame = sourceFrameMap.front();
    double maxSourceFrame = sourceFrameMap.front();
    for (double frame : sourceFrameMap) {
      minSourceFrame = std::min(minSourceFrame, frame);
      maxSourceFrame = std::max(maxSourceFrame, frame);
    }

    // Enough margin for the widest grain PSOLA can ask for at the lowest
    // pitch it will track, plus room for the mark search either side.
    const int context = 4096;
    const int totalSourceSamples = origWaveform.getNumSamples();
    const int sourceStart = std::clamp(
        static_cast<int>(std::floor(minSourceFrame)) * hopSize - context, 0,
        std::max(0, totalSourceSamples));
    const int sourceEnd = std::clamp(
        (static_cast<int>(std::ceil(maxSourceFrame)) + 1) * hopSize + context, 0,
        totalSourceSamples);

    if (sourceEnd <= sourceStart || origWaveform.getNumChannels() <= 0) {
      if (onComplete)
        onComplete(false);
      return;
    }

    const float *sourcePtr = origWaveform.getReadPointer(0);
    dspRequest.source.assign(sourcePtr + sourceStart, sourcePtr + sourceEnd);
    dspRequest.sourceStartSample = sourceStart;
    dspRequest.sourceFrame = sourceFrameMap;
    dspRequest.pitchRatio = std::move(pitchRatio);
    dspRequest.hopSize = hopSize;
    dspRequest.numOutputFrames = endFrame - startFrame;
    dspRequest.sampleRate = audioData.sampleRate;

    // PSOLA places one grain per pitch period, so it needs the original pitch
    // itself rather than only the ratio, and the voiced mask to know where a
    // period is meaningful at all. Absolute frame indexing, so a retimed note
    // reading outside the range still resolves.
    dspRequest.sourceF0 =
        audioData.denseF0.empty() ? audioData.f0 : audioData.denseF0;
    dspRequest.sourceVoiced.resize(audioData.voicedMask.size());
    for (size_t frame = 0; frame < audioData.voicedMask.size(); ++frame)
      dspRequest.sourceVoiced[frame] =
          audioData.voicedMask[frame] ? uint8_t{1} : uint8_t{0};
  }

  if (onProgress)
    onProgress(TR("progress.synthesizing"));

  // Cancel previous job
  if (cancelFlag)
    cancelFlag->store(true);
  cancelFlag = std::make_shared<std::atomic<bool>>(false);
  uint64_t currentJobId = ++jobId;
  isBusy = true;

  int capturedStartFrame = startFrame;
  int capturedEndFrame = endFrame;
  const size_t capturedNoteCount = project->getNotes().size();
  auto capturedCancelFlag = cancelFlag;
  auto capturedProject = project;


  // Everything past the render is engine-agnostic: both paths hand back the
  // same span of samples and share this callback verbatim.
  auto onRendered =
      [this, capturedCancelFlag, capturedProject, capturedStartFrame,
       capturedEndFrame, hopSize, currentJobId, onComplete,
       hasDirtyNoteAnchors, f0DirtyStart, f0DirtyEnd,
       capturedNoteCount, commitFrameRanges = std::move(commitFrameRanges),
       blendMask = std::move(blendMask),
       originalSegment = std::move(originalSegment),
       audioMovePatches = std::move(audioMovePatches)](
          std::vector<float> synthesizedAudio) {
        if (currentJobId != jobId.load())
          return;
        if (capturedCancelFlag->load()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }
        if (synthesizedAudio.empty()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }

        std::thread([this, capturedCancelFlag, capturedProject,
                     capturedStartFrame, capturedEndFrame, hopSize,
                     currentJobId, onComplete, hasDirtyNoteAnchors,
                     f0DirtyStart, f0DirtyEnd, capturedNoteCount,
                     commitFrameRanges, blendMask, originalSegment,
                     audioMovePatches,
                     synthesizedAudio = std::move(synthesizedAudio)]() mutable {
          if (currentJobId != jobId.load())
            return;
          if (capturedCancelFlag->load()) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          auto &audioData = capturedProject->getAudioData();
          if (capturedProject->getNotes().size() != capturedNoteCount) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }
          int totalSamples = audioData.waveform.getNumSamples();
          int startSample = capturedStartFrame * hopSize;
          int expectedSamples =
              (capturedEndFrame - capturedStartFrame) * hopSize;

          if (expectedSamples <= 0) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          // Resize synthesized audio to match expected
          synthesizedAudio.resize(static_cast<size_t>(expectedSamples), 0.0f);

          int samplesToWrite =
              std::min(expectedSamples, totalSamples - startSample);
          if (samplesToWrite <= 0) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          // Build blended target from model/original.
          std::vector<float> targetSegment(samplesToWrite, 0.0f);
          for (int i = 0; i < samplesToWrite; ++i) {
            const float b =
                (i < static_cast<int>(blendMask.size())) ? blendMask[i] : 0.0f;
            const float synth = synthesizedAudio[static_cast<size_t>(i)];
            const float orig = originalSegment[static_cast<size_t>(i)];
            targetSegment[static_cast<size_t>(i)] =
                b * synth + (1.0f - b) * orig;
          }

          // Apply per-note gain on top of the blended target.
          std::vector<float> sampleGain(static_cast<size_t>(samplesToWrite),
                                        1.0f);
          for (const auto &note : capturedProject->getNotes()) {
            if (note.isRest())
              continue;
            if (std::abs(note.getVolumeDb()) < 0.001f)
              continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = note.getEndFrame();
            const int overlapStart = std::max(capturedStartFrame, noteStart);
            const int overlapEnd = std::min(capturedEndFrame, noteEnd);
            if (overlapEnd <= overlapStart)
              continue;

            const int localStart = (overlapStart - capturedStartFrame) * hopSize;
            const int localEnd = (overlapEnd - capturedStartFrame) * hopSize;
            if (localStart >= samplesToWrite)
              continue;

            const float gain =
                juce::Decibels::decibelsToGain(note.getVolumeDb(), -60.0f);
            const int clampedStart = std::max(0, localStart);
            const int clampedEnd = std::min(samplesToWrite, localEnd);
            for (int i = clampedStart; i < clampedEnd; ++i) {
              sampleGain[static_cast<size_t>(i)] *= gain;
            }
          }
          for (int i = 0; i < samplesToWrite; ++i) {
            targetSegment[static_cast<size_t>(i)] *=
                sampleGain[static_cast<size_t>(i)];
          }

          // A neutral dirty note is an explicit reset. Restore its pristine
          // source body instead of committing a vocoder round-trip at neutral
          // pitch; short fades keep the reset continuous with any rendered
          // adjacent notes that remain in the composite.
          constexpr int kResetFadeSamples = 512;
          for (const auto &note : capturedProject->getNotes()) {
            if (note.isRest() || !note.isDirty() ||
                !note.isNeutralForOriginalWaveform())
              continue;

            const int localStart = std::max(
                0, (note.getStartFrame() - capturedStartFrame) * hopSize);
            const int localEnd = std::min(
                samplesToWrite,
                (note.getEndFrame() - capturedStartFrame) * hopSize);
            const int bodySamples = localEnd - localStart;
            if (bodySamples <= 0)
              continue;

            const int fadeSamples =
                std::min(kResetFadeSamples, bodySamples / 2);
            for (int sample = localStart; sample < localEnd; ++sample) {
              const int bodySample = sample - localStart;
              float resetBlend = 1.0f;
              if (fadeSamples > 0 && bodySample < fadeSamples) {
                const float t = static_cast<float>(bodySample) /
                                static_cast<float>(fadeSamples);
                resetBlend = t * t * (3.0f - 2.0f * t);
              }
              if (fadeSamples > 0 &&
                  bodySample >= bodySamples - fadeSamples) {
                const float t =
                    static_cast<float>(bodySamples - 1 - bodySample) /
                    static_cast<float>(fadeSamples);
                resetBlend = std::min(
                    resetBlend, t * t * (3.0f - 2.0f * t));
              }

              const auto index = static_cast<size_t>(sample);
              targetSegment[index] +=
                  resetBlend * (originalSegment[index] - targetSegment[index]);
            }
          }

          // The vocoder renders a broad, phase-stable context range, but only
          // commit the notes/F0 interval that actually changed. Search within
          // the existing boundary allowance for the point where the current
          // composite and new render best agree, then use a short smoothstep
          // transition there to avoid both clicks and long phasey overlaps.
          constexpr int kCommitBoundaryMarginSamples = 512;
          constexpr int kAdaptiveFadeHalfSamples = 128;
          constexpr int kSpliceAnalysisHalfSamples = 128;
          std::vector<float> commitMask(static_cast<size_t>(samplesToWrite),
                                        0.0f);
          const float *existingSamples =
              audioData.waveform.getReadPointer(0, startSample);
          auto smoothstep = [](float t) {
            t = std::clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
          };
          for (const auto &range : commitFrameRanges) {
            const int bodyStart =
                (range.start - capturedStartFrame) * hopSize;
            const int bodyEnd = (range.end - capturedStartFrame) * hopSize;
            const int bodySamples = bodyEnd - bodyStart;
            if (bodySamples <= 0)
              continue;

            const int fadeHalf =
                std::min(kAdaptiveFadeHalfSamples,
                         std::max(1, bodySamples / 4));
            const bool fadeLeft = range.start > 0;
            const bool fadeRight = range.end * hopSize < totalSamples;
            const int availableSearchRadius = std::max(
                0, kCommitBoundaryMarginSamples - fadeHalf);
            const int nonOverlappingSearchRadius =
                std::max(0, bodySamples / 2 - fadeHalf);
            const int searchRadius =
                std::min(availableSearchRadius, nonOverlappingSearchRadius);
            const int analysisHalf = std::min(
                kSpliceAnalysisHalfSamples,
                std::max(1, samplesToWrite / 2 - 1));

            const int leftCenter = fadeLeft
                                       ? findBestSpliceCenter(
                                             existingSamples, targetSegment,
                                             bodyStart, searchRadius,
                                             analysisHalf)
                                       : bodyStart;
            const int rightCenter = fadeRight
                                        ? findBestSpliceCenter(
                                              existingSamples, targetSegment,
                                              bodyEnd, searchRadius,
                                              analysisHalf)
                                        : bodyEnd;
            const int leftFadeStart = leftCenter - fadeHalf;
            const int leftFadeEnd = leftCenter + fadeHalf;
            const int rightFadeStart = rightCenter - fadeHalf;
            const int rightFadeEnd = rightCenter + fadeHalf;
            const int firstSample =
                std::max(0, fadeLeft ? leftFadeStart : bodyStart);
            const int lastSample = std::min(
                samplesToWrite, fadeRight ? rightFadeEnd : bodyEnd);

            for (int sample = firstSample; sample < lastSample; ++sample) {
              float leftBlend = 1.0f;
              if (fadeLeft && sample < leftFadeEnd)
                leftBlend = smoothstep(
                    static_cast<float>(sample - leftFadeStart) /
                    static_cast<float>(2 * fadeHalf));

              float rightBlend = 1.0f;
              if (fadeRight && sample >= rightFadeStart)
                rightBlend = smoothstep(
                    static_cast<float>(rightFadeEnd - sample) /
                    static_cast<float>(2 * fadeHalf));

              const float blend = std::min(leftBlend, rightBlend);
              auto &mask = commitMask[static_cast<size_t>(sample)];
              mask = std::max(mask, blend);
            }
          }

          for (int channel = 0; channel < audioData.waveform.getNumChannels();
               ++channel) {
            auto *destination =
                audioData.waveform.getWritePointer(channel, startSample);
            for (int sample = 0; sample < samplesToWrite; ++sample) {
              const float blend = commitMask[static_cast<size_t>(sample)];
              if (blend <= 0.0f)
                continue;
              const float current = destination[sample];
              const float rendered = targetSegment[static_cast<size_t>(sample)];
              destination[sample] = current + blend * (rendered - current);
            }
          }

          // Apply fixed-length audio attached to timing-region boundaries.
          // These joins happen after the synthesized commit, so they need
          // their own smoothing instead of hard clear/copy operations.
          constexpr int kTimingClearFadeSamples = 128;
          constexpr int kTimingPatchBoundaryMarginSamples = 512;
          constexpr int kTimingPatchFadeHalfSamples = 128;
          constexpr int kTimingPatchAnalysisHalfSamples = 128;

          // Vacated timing gaps are fully silent. Their click prevention is
          // applied as a release/attack inside the adjacent regions below.
          for (int channel = 0; channel < audioData.waveform.getNumChannels();
               ++channel) {
            auto *destination = audioData.waveform.getWritePointer(channel);
            for (const auto &patch : audioMovePatches) {
              if (patch.clearStartSample < 0 ||
                  patch.clearEndSample <= patch.clearStartSample)
                continue;

              const int clearStart =
                  std::clamp(patch.clearStartSample, 0, totalSamples);
              const int clearEnd =
                  std::clamp(patch.clearEndSample, clearStart, totalSamples);
              const int clearCount = clearEnd - clearStart;
              if (clearCount <= 0)
                continue;
              std::fill(destination + clearStart, destination + clearEnd,
                        0.0f);
            }
          }

          // Adaptively crossfade each moved patch at both ends. The shorter
          // fades stay inside the patch while the search remains bounded by
          // the same 512-sample timing margin.
          for (const auto &patch : audioMovePatches) {
            const int copyStart = std::clamp(
                patch.destinationStartSample, 0, totalSamples);
            const int sourceOffset = copyStart - patch.destinationStartSample;
            const int copyCount = std::min(
                static_cast<int>(patch.samples.size()) - sourceOffset,
                totalSamples - copyStart);
            if (copyCount <= 0)
              continue;

            std::vector<float> movedSamples(
                patch.samples.begin() + sourceOffset,
                patch.samples.begin() + sourceOffset + copyCount);
            const int fadeHalf =
                std::min(kTimingPatchFadeHalfSamples,
                         std::max(1, copyCount / 4));
            const int searchRadius = std::min(
                std::max(0, kTimingPatchBoundaryMarginSamples - 2 * fadeHalf),
                std::max(0, copyCount / 2 - 2 * fadeHalf));
            const int analysisHalf =
                std::min(kTimingPatchAnalysisHalfSamples, fadeHalf);
            const float *existing =
                audioData.waveform.getReadPointer(0, copyStart);
            const int leftCenter = findBestSpliceCenter(
                existing, movedSamples, fadeHalf, searchRadius, analysisHalf);
            const int rightCenter = findBestSpliceCenter(
                existing, movedSamples, copyCount - fadeHalf, searchRadius,
                analysisHalf);
            const int leftFadeStart = leftCenter - fadeHalf;
            const int leftFadeEnd = leftCenter + fadeHalf;
            const int rightFadeStart = rightCenter - fadeHalf;
            const int rightFadeEnd = rightCenter + fadeHalf;

            std::vector<float> patchMask(static_cast<size_t>(copyCount), 0.0f);
            for (int sample = std::max(0, leftFadeStart);
                 sample < std::min(copyCount, rightFadeEnd); ++sample) {
              float leftBlend = 1.0f;
              if (sample < leftFadeEnd)
                leftBlend = smoothstep(
                    static_cast<float>(sample - leftFadeStart) /
                    static_cast<float>(2 * fadeHalf));

              float rightBlend = 1.0f;
              if (sample >= rightFadeStart)
                rightBlend = smoothstep(
                    static_cast<float>(rightFadeEnd - sample) /
                    static_cast<float>(2 * fadeHalf));

              patchMask[static_cast<size_t>(sample)] =
                  std::min(leftBlend, rightBlend);
            }

            for (int channel = 0;
                 channel < audioData.waveform.getNumChannels(); ++channel) {
              auto *destination =
                  audioData.waveform.getWritePointer(channel, copyStart);
              for (int sample = 0; sample < copyCount; ++sample) {
                const float blend = patchMask[static_cast<size_t>(sample)];
                if (blend <= 0.0f)
                  continue;
                destination[sample] +=
                    blend * (movedSamples[static_cast<size_t>(sample)] -
                             destination[sample]);
              }
            }
          }

          // Shape audio on the occupied sides of each gap. Keeping these
          // envelopes outside the cleared samples guarantees that the gap
          // remains visually and audibly silent.
          for (int channel = 0; channel < audioData.waveform.getNumChannels();
               ++channel) {
            auto *destination = audioData.waveform.getWritePointer(channel);
            for (const auto &patch : audioMovePatches) {
              if (patch.clearStartSample < 0 ||
                  patch.clearEndSample <= patch.clearStartSample)
                continue;

              const int clearStart =
                  std::clamp(patch.clearStartSample, 0, totalSamples);
              const int clearEnd =
                  std::clamp(patch.clearEndSample, clearStart, totalSamples);
              if (patch.releaseBeforeClear && clearStart > 0) {
                const int fadeCount =
                    std::min(kTimingClearFadeSamples, clearStart);
                const int fadeStart = clearStart - fadeCount;
                const float denominator =
                    static_cast<float>(std::max(1, fadeCount - 1));
                for (int sample = 0; sample < fadeCount; ++sample) {
                  const float gain = smoothstep(
                      static_cast<float>(fadeCount - 1 - sample) /
                      denominator);
                  destination[fadeStart + sample] *= gain;
                }
              }
              if (patch.attackAfterClear && clearEnd < totalSamples) {
                const int fadeCount = std::min(kTimingClearFadeSamples,
                                               totalSamples - clearEnd);
                const float denominator =
                    static_cast<float>(std::max(1, fadeCount - 1));
                for (int sample = 0; sample < fadeCount; ++sample) {
                  const float gain = smoothstep(
                      static_cast<float>(sample) / denominator);
                  destination[clearEnd + sample] *= gain;
                }
              }
            }
          }

          // Persist only semantic render state. Previously this loop stored a
          // copy of the rendered samples on every note; the composite waveform
          // is now authoritative, while this flag keeps adjacency selection and
          // project edit detection independent of audio caches.
          const bool hasF0DirtyRange = f0DirtyStart >= 0 && f0DirtyEnd >= 0;
          auto overlapsF0DirtyRange = [&](const Note &note) {
            return hasF0DirtyRange && note.getStartFrame() < f0DirtyEnd &&
                   note.getEndFrame() > f0DirtyStart;
          };
          auto isCommitAnchor = [&](const Note &note) {
            if (note.isRest())
              return false;
            if (note.isDirty())
              return true;

            // Draw/F0 edits may not mark a specific note dirty. In that case,
            // notes overlapping the F0 dirty range are the edited anchors.
            return !hasDirtyNoteAnchors && overlapsF0DirtyRange(note);
          };

          auto &notes = capturedProject->getNotes();
          for (auto &note : notes) {
            if (note.isRest())
              continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = note.getEndFrame();
            const int overlapStart = std::max(capturedStartFrame, noteStart);
            const int overlapEnd = std::min(capturedEndFrame, noteEnd);
            if (overlapEnd <= overlapStart)
              continue;

            const bool isF0Anchor =
                !hasDirtyNoteAnchors && overlapsF0DirtyRange(note);
            if (!isCommitAnchor(note))
              continue;

            const bool isNeutralReset =
                note.isDirty() && note.isNeutralForOriginalWaveform() &&
                !isF0Anchor;
            note.setRenderedEdit(!isNeutralReset);
            note.setSynthDirty(false);
          }

          isBusy = false;
          juce::MessageManager::callAsync(
              [capturedProject, onComplete]() {
                capturedProject->clearAllDirty();
                if (onComplete) onComplete(true);
              });
        }).detach();
      };

  if (usePsola) {
    psola.renderAsync(std::move(dspRequest), std::move(onRendered),
                      capturedCancelFlag);
  } else {
    vocoder->inferAsync(std::move(melRange), std::move(adjustedF0Range),
                        std::move(onRendered));
  }
}
