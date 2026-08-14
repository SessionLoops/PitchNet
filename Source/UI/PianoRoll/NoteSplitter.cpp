#include "NoteSplitter.h"
#include "../../Utils/Constants.h"
#include <algorithm>
#include <cmath>

namespace
{
float calculatePitchCenter(const AudioData& audioData, int startFrame,
                           int endFrame, float fallback)
{
    const int f0Size = static_cast<int>(audioData.f0.size());
    const int start = std::clamp(startFrame, 0, f0Size);
    const int end = std::clamp(endFrame, start, f0Size);

    double midiSum = 0.0;
    int voicedCount = 0;
    for (int frame = start; frame < end; ++frame) {
        const bool voiced = audioData.voicedMask.empty() ||
                            (frame < static_cast<int>(audioData.voicedMask.size()) &&
                             audioData.voicedMask[static_cast<size_t>(frame)]);
        const float f0 = audioData.f0[static_cast<size_t>(frame)];
        if (voiced && f0 > 0.0f && std::isfinite(f0)) {
            midiSum += freqToMidi(f0);
            ++voicedCount;
        }
    }

    return voicedCount > 0
               ? static_cast<float>(midiSum / voicedCount)
               : fallback;
}

void rebasePitchDeviation(Note& note, float amount)
{
    if (std::abs(amount) < 0.0001f)
        return;

    if (note.hasOriginalDeltaPitch()) {
        auto delta = note.getOriginalDeltaPitch();
        for (auto& value : delta)
            value += amount;
        note.setOriginalDeltaPitch(std::move(delta));
    }

    if (note.hasDeltaPitch()) {
        auto delta = note.getDeltaPitch();
        for (auto& value : delta)
            value += amount;
        note.setDeltaPitch(std::move(delta));
    }

    if (note.hasBakedDeltaPitch()) {
        auto delta = note.getBakedDeltaPitch();
        for (auto& value : delta)
            value += amount;
        note.setBakedDeltaPitch(std::move(delta));
    }
}
}

Note* NoteSplitter::findNoteAt(float x, float y) {
    if (!project || !coordMapper)
        return nullptr;

    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();

    for (auto& note : project->getNotes()) {
        if (note.isRest())
            continue;

        float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
        float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
        float noteY = coordMapper->midiToY(note.getAdjustedMidiNote());
        float noteH = pixelsPerSemitone;

        if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH) {
            return &note;
        }
    }

    return nullptr;
}

bool NoteSplitter::splitNoteAtFrame(Note* note, int splitFrame) {
    if (!note || !project)
        return false;

    int startFrame = note->getStartFrame();
    int endFrame = note->getEndFrame();
    const float previousPitchCenter = note->getAdjustedMidiNote();

    // Keep at least one analysis frame on each side of the split.
    if (splitFrame <= startFrame || splitFrame >= endFrame)
        return false;

    // Store original note data for undo
    Note originalNote = *note;

    // Create the second note (right part)
    Note secondNote;
    secondNote.setStartFrame(splitFrame);
    secondNote.setEndFrame(endFrame);
    const int srcStartFrame = note->getSrcStartFrame();
    const int srcEndFrame = note->getSrcEndFrame();
    const double splitRatio = static_cast<double>(splitFrame - startFrame) /
                              static_cast<double>(endFrame - startFrame);
    const int sourceDuration = srcEndFrame - srcStartFrame;
    const int sourceSplitCandidate =
        srcStartFrame + static_cast<int>(
                            std::lround(splitRatio * sourceDuration));
    const int srcSplitFrame = sourceDuration >= 2
                                  ? std::clamp(sourceSplitCandidate,
                                               srcStartFrame + 1,
                                               srcEndFrame - 1)
                                  : std::clamp(sourceSplitCandidate,
                                               srcStartFrame, srcEndFrame);
    secondNote.setSrcStartFrame(srcSplitFrame);
    secondNote.setSrcEndFrame(srcEndFrame);
    secondNote.setMidiNote(note->getMidiNote());
    secondNote.setLyric(note->getLyric());
    secondNote.setPhoneme(note->getPhoneme());
    secondNote.setVolumeDb(note->getVolumeDb());
    secondNote.setSelected(note->isSelected());
    secondNote.setTiltLeft(note->getTiltLeft());
    secondNote.setTiltRight(note->getTiltRight());
    secondNote.setVibrato(note->getVibrato());
    secondNote.setSmoothLeftFrames(note->getSmoothLeftFrames());
    secondNote.setSmoothRightFrames(note->getSmoothRightFrames());
    secondNote.setDeltaScale(note->getDeltaScale());
    secondNote.setDeltaOffset(note->getDeltaOffset());
    secondNote.setPitchOffset(0.0f);

    // Split originalDeltaPitch if available.
    if (note->hasOriginalDeltaPitch()) {
        const auto& origDelta = note->getOriginalDeltaPitch();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(origDelta.size())));
        std::vector<float> leftDelta(origDelta.begin(), origDelta.begin() + splitOffset);
        std::vector<float> rightDelta(origDelta.begin() + splitOffset, origDelta.end());
        note->setOriginalDeltaPitch(std::move(leftDelta));
        secondNote.setOriginalDeltaPitch(std::move(rightDelta));
    } else {
        // Fallback: extract from global deltaPitch if originalDeltaPitch is missing
        auto& audioData = project->getAudioData();
        const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
        if (totalFrames > 0) {
            int leftLen = splitFrame - startFrame;
            int rightLen = endFrame - splitFrame;
            if (leftLen > 0) {
                std::vector<float> leftDelta(static_cast<size_t>(leftLen));
                for (int i = 0; i < leftLen; ++i) {
                    int gIdx = startFrame + i;
                    if (gIdx >= 0 && gIdx < totalFrames)
                        leftDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(gIdx)];
                }
                note->setOriginalDeltaPitch(std::move(leftDelta));
            }
            if (rightLen > 0) {
                std::vector<float> rightDelta(static_cast<size_t>(rightLen));
                for (int i = 0; i < rightLen; ++i) {
                    int gIdx = splitFrame + i;
                    if (gIdx >= 0 && gIdx < totalFrames)
                        rightDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(gIdx)];
                }
                secondNote.setOriginalDeltaPitch(std::move(rightDelta));
            }
        }
    }

    // Keep per-note pitch data aligned with the new frame ranges.
    if (note->hasDeltaPitch()) {
        const auto& delta = note->getDeltaPitch();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::clamp(splitOffset, 0, static_cast<int>(delta.size()));
        std::vector<float> leftDelta(delta.begin(), delta.begin() + splitOffset);
        std::vector<float> rightDelta(delta.begin() + splitOffset, delta.end());
        note->setDeltaPitch(std::move(leftDelta));
        secondNote.setDeltaPitch(std::move(rightDelta));
    }

    if (note->hasBakedDeltaPitch()) {
        const auto& delta = note->getBakedDeltaPitch();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::clamp(splitOffset, 0, static_cast<int>(delta.size()));
        std::vector<float> leftDelta(delta.begin(), delta.begin() + splitOffset);
        std::vector<float> rightDelta(delta.begin() + splitOffset, delta.end());
        note->setBakedDeltaPitch(std::move(leftDelta));
        secondNote.setBakedDeltaPitch(std::move(rightDelta));
    }

    const auto& f0Values = note->getF0Values();
    if (!f0Values.empty()) {
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::clamp(splitOffset, 0, static_cast<int>(f0Values.size()));
        std::vector<float> leftF0(f0Values.begin(), f0Values.begin() + splitOffset);
        std::vector<float> rightF0(f0Values.begin() + splitOffset, f0Values.end());
        note->setF0Values(std::move(leftF0));
        secondNote.setF0Values(std::move(rightF0));
    }

    // Recalculate each half's center from its own voiced pitch frames. Rebase
    // its deviation curve by the opposite amount so splitting is pitch-neutral.
    const auto& audioData = project->getAudioData();
    const float leftPitchCenter = calculatePitchCenter(
        audioData, startFrame, splitFrame, previousPitchCenter);
    const float rightPitchCenter = calculatePitchCenter(
        audioData, splitFrame, endFrame, previousPitchCenter);

    rebasePitchDeviation(*note, previousPitchCenter - leftPitchCenter);
    rebasePitchDeviation(secondNote, previousPitchCenter - rightPitchCenter);
    note->setMidiNote(leftPitchCenter);
    note->setOriginalMidiNote(leftPitchCenter);
    note->setPitchOffset(0.0f);
    secondNote.setMidiNote(rightPitchCenter);
    secondNote.setOriginalMidiNote(rightPitchCenter);
    secondNote.setPitchOffset(0.0f);

    // Modify the first note (left part)
    note->setEndFrame(splitFrame);
    note->setSrcEndFrame(srcSplitFrame);

    // Splitting is a structural edit, so both segments inherit the source
    // note's processing state rather than treating the new tail as clean.
    const bool sourceIsDirty = originalNote.isDirty();
    const bool sourceIsSynthDirty = originalNote.isSynthDirty();
    const bool sourceHasRenderedEdit = originalNote.hasRenderedEdit();
    note->setDirty(sourceIsDirty);
    note->setSynthDirty(sourceIsSynthDirty);
    note->setRenderedEdit(sourceHasRenderedEdit);
    secondNote.setDirty(sourceIsDirty);
    secondNote.setSynthDirty(sourceIsSynthDirty);
    secondNote.setRenderedEdit(sourceHasRenderedEdit);

    // Save first note BEFORE addNote (addNote may invalidate note pointer due to vector reallocation)
    Note firstNote = *note;

    // Add the second note to project
    project->addNote(secondNote);

    // Create undo action - don't pass callback to avoid lifetime issues
    // UI refresh is handled by UndoManager's onUndoRedo callback
    if (undoManager) {
        auto action = std::make_unique<NoteSplitAction>(
            project, originalNote, firstNote, secondNote, nullptr);
        undoManager->addAction(std::move(action));
    }

    if (onNoteSplit)
        onNoteSplit();

    return true;
}

bool NoteSplitter::splitNoteAtX(Note* note, float x) {
    if (!note || !coordMapper)
        return false;

    // Convert X coordinate to frame
    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    double time = x / pixelsPerSecond;
    int frame = static_cast<int>(time * SAMPLE_RATE / HOP_SIZE);

    return splitNoteAtFrame(note, frame);
}

std::pair<Note*, Note*> NoteSplitter::findMergeableNotesAt(float x, float y)
{
    if (!project || !coordMapper)
        return {};

    const auto &chunkRanges = project->getAudioData().segmentChunkRanges;
    const auto getRegionIndex = [&chunkRanges](const Note &note)
    {
        if (chunkRanges.empty())
            return 0;

        const int midpoint = note.getSrcStartFrame() +
                             std::max(0, note.getSrcDurationFrames()) / 2;
        for (size_t i = 0; i < chunkRanges.size(); ++i)
            if (midpoint >= chunkRanges[i].first && midpoint < chunkRanges[i].second)
                return static_cast<int>(i);
        return -1;
    };

    auto &notes = project->getNotes();
    const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
    constexpr float boundaryHitPadding = 5.0f;

    for (size_t i = 0; i < notes.size(); ++i)
    {
        auto &left = notes[i];
        if (left.isRest())
            continue;

        for (size_t j = i + 1; j < notes.size(); ++j)
        {
            auto &right = notes[j];
            if (right.isRest())
                continue;

            // Only expose merge on a true split: the output and source frame
            // ranges must remain continuous across the boundary.
            if (left.getEndFrame() != right.getStartFrame() ||
                left.getSrcEndFrame() != right.getSrcStartFrame() ||
                getRegionIndex(left) < 0 || getRegionIndex(left) != getRegionIndex(right))
                break;

            const float boundaryX = 0.5f * framesToSeconds(
                left.getEndFrame() + right.getStartFrame()) * pixelsPerSecond;
            const float lineTop = std::min(
                coordMapper->midiToY(left.getAdjustedMidiNote()),
                coordMapper->midiToY(right.getAdjustedMidiNote()));
            const float lineBottom = std::max(
                coordMapper->midiToY(left.getAdjustedMidiNote()),
                coordMapper->midiToY(right.getAdjustedMidiNote())) + pixelsPerSemitone;

            if (std::abs(x - boundaryX) <= boundaryHitPadding &&
                y >= lineTop && y <= lineBottom)
                return {&left, &right};

            break;
        }
    }

    return {};
}

bool NoteSplitter::mergeNotes(Note *first, Note *second)
{
    if (!project || !first || !second || first->isRest() || second->isRest() ||
        first->getEndFrame() != second->getStartFrame() ||
        first->getSrcEndFrame() != second->getSrcStartFrame())
        return false;

    const Note firstNote = *first;
    const Note secondNote = *second;
    Note mergedNote = firstNote;
    mergedNote.setEndFrame(secondNote.getEndFrame());
    mergedNote.setSrcEndFrame(secondNote.getSrcEndFrame());

    auto append = [](auto left, const auto &right)
    {
        left.insert(left.end(), right.begin(), right.end());
        return left;
    };
    mergedNote.setF0Values(append(firstNote.getF0Values(), secondNote.getF0Values()));

    // Restore a single pitch center over the merged duration. Each half may
    // have been rebased when it was split, so convert its deviations through
    // the current absolute pitch before expressing them around the new center.
    const float mergedPitchCenter = calculatePitchCenter(
        project->getAudioData(), mergedNote.getStartFrame(),
        mergedNote.getEndFrame(), firstNote.getAdjustedMidiNote());
    const auto mergeDeviation = [mergedPitchCenter](
                                  const std::vector<float> &firstValues,
                                  float firstCenter,
                                  const std::vector<float> &secondValues,
                                  float secondCenter)
    {
        std::vector<float> values;
        values.reserve(firstValues.size() + secondValues.size());
        for (const auto value : firstValues)
            values.push_back(value + firstCenter - mergedPitchCenter);
        for (const auto value : secondValues)
            values.push_back(value + secondCenter - mergedPitchCenter);
        return values;
    };
    mergedNote.setOriginalDeltaPitch(mergeDeviation(
        firstNote.getOriginalDeltaPitch(), firstNote.getOriginalMidiNote(),
        secondNote.getOriginalDeltaPitch(), secondNote.getOriginalMidiNote()));
    mergedNote.setDeltaPitch(mergeDeviation(
        firstNote.getDeltaPitch(), firstNote.getAdjustedMidiNote(),
        secondNote.getDeltaPitch(), secondNote.getAdjustedMidiNote()));
    if (firstNote.hasBakedDeltaPitch() || secondNote.hasBakedDeltaPitch()) {
        const auto& firstActive = firstNote.hasBakedDeltaPitch()
                                      ? firstNote.getBakedDeltaPitch()
                                      : firstNote.getOriginalDeltaPitch();
        const auto& secondActive = secondNote.hasBakedDeltaPitch()
                                       ? secondNote.getBakedDeltaPitch()
                                       : secondNote.getOriginalDeltaPitch();
        mergedNote.setBakedDeltaPitch(mergeDeviation(
            firstActive, firstNote.getAdjustedMidiNote(),
            secondActive, secondNote.getAdjustedMidiNote()));
    }
    mergedNote.setMidiNote(mergedPitchCenter);
    mergedNote.setOriginalMidiNote(mergedPitchCenter);
    mergedNote.setPitchOffset(0.0f);
    const bool isDirty = firstNote.isDirty() || secondNote.isDirty();
    const bool isSynthDirty = firstNote.isSynthDirty() || secondNote.isSynthDirty();
    mergedNote.setDirty(isDirty);
    mergedNote.setRenderedEdit(firstNote.hasRenderedEdit() ||
                               secondNote.hasRenderedEdit());
    if (isSynthDirty)
        mergedNote.markSynthDirty();
    else
        mergedNote.setSynthDirty(false);

    *first = mergedNote;
    project->removeNoteByStartFrame(secondNote.getStartFrame());

    if (undoManager)
        undoManager->addAction(std::make_unique<NoteMergeAction>(
            project, firstNote, secondNote, mergedNote, nullptr));

    if (onNoteSplit)
        onNoteSplit();

    return true;
}
