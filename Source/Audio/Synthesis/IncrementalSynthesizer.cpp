#include "IncrementalSynthesizer.h"
#include "../../Utils/Localization.h"
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

    // A dirty neutral note is being reset. It must discard its cache and split
    // the edited cluster instead of pulling its neighbours into the new pass.
    if (note.isDirty() && note.isNeutralForOriginalWaveform() &&
        !isF0Anchor(note))
      return false;

    return isF0Anchor(note) || note.hasSynthWaveform() ||
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

  std::vector<std::vector<float>> melRange;

  if (melRange.empty()) {
    melRange.assign(audioData.melSpectrogram.begin() + startFrame,
                    audioData.melSpectrogram.begin() + endFrame);
  }
  std::vector<float> adjustedF0Range =
      project->getAdjustedF0ForRange(startFrame, endFrame);

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
       originalSegment = std::move(originalSegment)](
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

          // Distribute synthesized audio into per-note synthWaveforms.
          // Each note gets the slice of targetSegment corresponding to its
          // output frame range [startFrame, endFrame), PLUS margin samples on
          // each side so that composeGlobalWaveform() can crossfade with real
          // audio instead of held-value extrapolation at note boundaries.
          constexpr int kSynthMarginSamples = 512; // margin each side
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

            // A normal neutral anchor is a reset, so restore original audio.
            // A direct-F0 anchor can be neutral in its note parameters while
            // still carrying a real curve edit and therefore needs a cache.
            if (note.isNeutralForOriginalWaveform() && !isF0Anchor) {
              note.discardSynthWaveform();
              continue;
            }

            // Full note range in samples (the "body")
            const int noteStartSample = noteStart * hopSize;
            const int noteEndSample = noteEnd * hopSize;
            const int noteSamples = noteEndSample - noteStartSample;
            if (noteSamples <= 0)
              continue;

            // Compute margin: how far we can extend into targetSegment
            // beyond the note's body on each side.
            const int targetStartSample = capturedStartFrame * hopSize;
            const int targetEndSample = targetStartSample + samplesToWrite;

            // Left margin: extend before noteStartSample
            const int leftMarginAvail = noteStartSample - targetStartSample;
            const int leftMargin = std::max(0, std::min(kSynthMarginSamples, leftMarginAvail));

            // Right margin: extend after noteEndSample
            const int rightMarginAvail = targetEndSample - noteEndSample;
            const int rightMargin = std::max(0, std::min(kSynthMarginSamples, rightMarginAvail));

            // Total synth vector: [preroll | body | postroll]
            const int totalSynthLen = leftMargin + noteSamples + rightMargin;
            std::vector<float> noteSynth(static_cast<size_t>(totalSynthLen), 0.0f);

            // Copy from targetSegment: the extended region
            // [noteStartSample - leftMargin, noteEndSample + rightMargin) in global coords
            // maps to targetSegment[(noteStartSample - leftMargin - targetStartSample) ..]
            const int extGlobalStart = noteStartSample - leftMargin;
            const int extLocalSrc = extGlobalStart - targetStartSample;

            // The overlap between [extGlobalStart, noteEndSample+rightMargin) and
            // [capturedStartFrame*hopSize, capturedStartFrame*hopSize + samplesToWrite)
            // determines what we can actually copy from targetSegment.
            const int copyStart = std::max(0, extLocalSrc);
            const int copyEnd = std::min(samplesToWrite,
                extLocalSrc + totalSynthLen);
            const int dstOffset = copyStart - extLocalSrc;

            for (int i = copyStart; i < copyEnd; ++i) {
              const int dstIdx = dstOffset + (i - copyStart);
              if (dstIdx >= 0 && dstIdx < totalSynthLen) {
                noteSynth[static_cast<size_t>(dstIdx)] =
                    targetSegment[static_cast<size_t>(i)];
              }
            }

            // For parts of the note body outside the synthesis range, derive
            // the immutable source clip directly from originalWaveform.
            const auto &originalWaveform = audioData.originalWaveform;
            if (originalWaveform.getNumChannels() > 0 &&
                originalWaveform.getNumSamples() > 0) {
              const int originalSamples = originalWaveform.getNumSamples();
              const int srcStartSample = std::clamp(
                  note.getSrcStartFrame() * hopSize, 0, originalSamples);
              const int srcEndSample = std::clamp(
                  note.getSrcEndFrame() * hopSize, srcStartSample,
                  originalSamples);
              const int srcFrames = note.getSrcEndFrame() - note.getSrcStartFrame();
              const int dstFrames = note.getEndFrame() - note.getStartFrame();
              const int srcSamples = srcEndSample - srcStartSample;
              const float *originalSamplesData =
                  originalWaveform.getReadPointer(0);

              for (int i = 0; i < noteSamples; ++i) {
                const int globalSample = noteStartSample + i;
                const int globalFrame = (globalSample / hopSize);
                // Skip samples already covered by synthesis
                if (globalFrame >= overlapStart && globalFrame < overlapEnd)
                  continue;

                // Map destination sample to source sample.
                float srcPos;
                if (dstFrames > 0 && srcFrames > 0) {
                  srcPos = static_cast<float>(i) * static_cast<float>(srcSamples) /
                           static_cast<float>(noteSamples);
                } else {
                  srcPos = static_cast<float>(i);
                }
                int srcIdx = static_cast<int>(srcPos);
                if (srcIdx >= 0 && srcIdx < srcSamples) {
                  noteSynth[static_cast<size_t>(leftMargin + i)] =
                      originalSamplesData[srcStartSample + srcIdx];
                }
              }
            }

            note.setSynthWaveform(std::move(noteSynth), leftMargin);
          }

          // Compose the global waveform from per-note synthWaveforms
          capturedProject->composeGlobalWaveform();

          isBusy = false;
          juce::MessageManager::callAsync(
              [capturedProject, onComplete]() {
                capturedProject->clearAllDirty();
                if (onComplete) onComplete(true);
              });
        }).detach();
      });
}
