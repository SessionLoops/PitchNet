#include "IncrementalSynthesizer.h"
#include "../../Utils/Localization.h"
#include "../../Utils/TimingRegionUtils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

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
};

bool hasTimingEdit(const Note &note) {
  return note.getStartFrame() != note.getSrcStartFrame() ||
         note.getEndFrame() != note.getSrcEndFrame();
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
  // adjacent edited notes so a whole connected phrase is refreshed from one
  // continuous vocoder pass, not merely the immediate neighbours.
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
void IncrementalSynthesizer::synthesizeRegion(ProgressCallback onProgress,
                                              CompleteCallback onComplete) {
  if (!project || !vocoder) {
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

  if (!vocoder->isLoaded()) {
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

  // Generate blend mask before async call (voicedMask is stable here)
  int hopSize = vocoder->getHopSize();
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
  // exact waveform copies. These patches are captured before the async render
  // so repeated edits and undo always read from immutable source audio.
  std::vector<AudioMovePatch> audioMovePatches;
  if (origWaveform.getNumChannels() > 0) {
    const float *origPtr = origWaveform.getReadPointer(0);
    for (const auto &note : project->getNotes()) {
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
        if (span.leading && span.destinationStart > span.sourceStart) {
          patch.clearStartSample = sourceStartSample;
          patch.clearEndSample = destinationStartSample;
        } else if (!span.leading &&
                   span.destinationEnd < span.sourceEnd) {
          patch.clearStartSample = span.destinationEnd * hopSize;
          patch.clearEndSample = sourceEndSample;
        }
        audioMovePatches.push_back(std::move(patch));
      }
    }
  }

  std::vector<std::vector<float>> melRange;

  if (melRange.empty()) {
    melRange.assign(audioData.melSpectrogram.begin() + startFrame,
                    audioData.melSpectrogram.begin() + endFrame);
  }
  applyNoteTimingToMel(*project, startFrame, endFrame, melRange);
  std::vector<float> adjustedF0Range =
      project->getAdjustedF0ForRange(startFrame, endFrame);
  applyPreservedTimingToF0(*project, startFrame, endFrame, adjustedF0Range);

  if (melRange.empty() || adjustedF0Range.empty()) {
    if (onComplete)
      onComplete(false);
    return;
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


  vocoder->inferAsync(
      std::move(melRange), std::move(adjustedF0Range),
      [this, capturedCancelFlag, capturedProject, capturedStartFrame,
       capturedEndFrame, hopSize, currentJobId, onComplete,
       hasDirtyNoteAnchors, f0DirtyStart, f0DirtyEnd,
       capturedNoteCount, clusterMembers = std::move(editedClusters.members),
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
                     clusterMembers, blendMask, originalSegment,
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
          if (capturedProject->getNotes().size() != capturedNoteCount ||
              clusterMembers.size() != capturedNoteCount) {
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

          // Commit the continuously rendered range directly into the existing
          // composite waveform. The synthesis range already includes voiced
          // context and padding; a short edge fade joins it to preserved audio
          // outside the affected range without rebuilding the whole project.
          constexpr int kPatchFadeSamples = 512;
          const bool fadeLeft = startSample > 0;
          const bool fadeRight = startSample + samplesToWrite < totalSamples;
          const int fadeSamples =
              std::min(kPatchFadeSamples, samplesToWrite / 2);
          for (int channel = 0; channel < audioData.waveform.getNumChannels();
               ++channel) {
            auto *destination =
                audioData.waveform.getWritePointer(channel, startSample);
            for (int sample = 0; sample < samplesToWrite; ++sample) {
              float blend = 1.0f;
              if (fadeLeft && fadeSamples > 0 && sample < fadeSamples) {
                const float t = static_cast<float>(sample) /
                                static_cast<float>(fadeSamples);
                blend = std::min(blend, t * t * (3.0f - 2.0f * t));
              }
              if (fadeRight && fadeSamples > 0 &&
                  sample >= samplesToWrite - fadeSamples) {
                const float t = static_cast<float>(samplesToWrite - 1 - sample) /
                                static_cast<float>(fadeSamples);
                blend = std::min(blend, t * t * (3.0f - 2.0f * t));
              }

              const float current = destination[sample];
              const float rendered = targetSegment[static_cast<size_t>(sample)];
              destination[sample] = current + blend * (rendered - current);
            }
          }

          // Apply fixed-length attached audio after the synthesized patch so
          // it remains sample-identical and is not affected by vocoder or
          // patch-edge fades. Clear only the vacated outer edge when the
          // region contracts away from its original boundary.
          for (int channel = 0; channel < audioData.waveform.getNumChannels();
               ++channel) {
            auto *destination = audioData.waveform.getWritePointer(channel);
            for (const auto &patch : audioMovePatches) {
              if (patch.clearStartSample >= 0 &&
                  patch.clearEndSample > patch.clearStartSample) {
                const int clearStart =
                    std::clamp(patch.clearStartSample, 0, totalSamples);
                const int clearEnd =
                    std::clamp(patch.clearEndSample, clearStart, totalSamples);
                std::fill(destination + clearStart, destination + clearEnd,
                          0.0f);
              }
            }
            for (const auto &patch : audioMovePatches) {
              const int copyStart = std::clamp(
                  patch.destinationStartSample, 0, totalSamples);
              const int sourceOffset =
                  copyStart - patch.destinationStartSample;
              const int copyCount = std::min(
                  static_cast<int>(patch.samples.size()) - sourceOffset,
                  totalSamples - copyStart);
              if (copyCount > 0)
                std::copy(patch.samples.begin() + sourceOffset,
                          patch.samples.begin() + sourceOffset + copyCount,
                          destination + copyStart);
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
          for (size_t noteIndex = 0; noteIndex < notes.size(); ++noteIndex) {
            auto &note = notes[noteIndex];
            if (note.isRest())
              continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = note.getEndFrame();
            const int overlapStart = std::max(capturedStartFrame, noteStart);
            const int overlapEnd = std::min(capturedEndFrame, noteEnd);
            if (overlapEnd <= overlapStart)
              continue;

            // Refresh every member of an adjacency-connected edited cluster
            // from this same continuous pass. Anchors outside a cluster (most
            // notably neutral reset notes) still need commit handling.
            const bool isF0Anchor =
                !hasDirtyNoteAnchors && overlapsF0DirtyRange(note);
            const bool shouldCommit =
                isCommitAnchor(note) || clusterMembers[noteIndex] != 0;
            if (!shouldCommit)
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
      });
}
