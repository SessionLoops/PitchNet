#include "NoteSplitter.h"
#include "../../Utils/Constants.h"
#include <algorithm>

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

    // Ensure split point is within note bounds (with margin)
    if (splitFrame <= startFrame + 5 || splitFrame >= endFrame - 5)
        return false;

    // Store original note data for undo
    Note originalNote = *note;

    // Ensure clip waveform exists before splitting
    if (!note->hasClipWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.waveform.getNumSamples() > 0) {
            int startSample = startFrame * HOP_SIZE;
            int endSample = endFrame * HOP_SIZE;
            startSample = std::max(0, std::min(startSample, audioData.waveform.getNumSamples()));
            endSample = std::max(startSample, std::min(endSample, audioData.waveform.getNumSamples()));
            std::vector<float> clip;
            clip.reserve(static_cast<size_t>(endSample - startSample));
            const float* src = audioData.waveform.getReadPointer(0);
            for (int i = startSample; i < endSample; ++i)
                clip.push_back(src[i]);
            note->setClipWaveform(std::move(clip));
        }
    }

    // Ensure source clip waveform exists before splitting
    if (!note->hasSrcClipWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.originalWaveform.getNumSamples() > 0) {
            int srcStart = note->getSrcStartFrame() * HOP_SIZE;
            int srcEnd = note->getSrcEndFrame() * HOP_SIZE;
            srcStart = std::max(0, std::min(srcStart, audioData.originalWaveform.getNumSamples()));
            srcEnd = std::max(srcStart, std::min(srcEnd, audioData.originalWaveform.getNumSamples()));
            std::vector<float> srcClip;
            srcClip.reserve(static_cast<size_t>(srcEnd - srcStart));
            const float* origSrc = audioData.originalWaveform.getReadPointer(0);
            for (int i = srcStart; i < srcEnd; ++i)
                srcClip.push_back(origSrc[i]);
            note->setSrcClipWaveform(std::move(srcClip));
        }
    }

    // Ensure clip mel exists before splitting
    if (!note->hasClipMel()) {
        auto& audioData = project->getAudioData();
        if (!audioData.melSpectrogram.empty()) {
            int melSize = static_cast<int>(audioData.melSpectrogram.size());
            int melStart = std::max(0, std::min(startFrame, melSize));
            int melEnd = std::max(melStart, std::min(endFrame, melSize));
            if (melEnd > melStart) {
                std::vector<std::vector<float>> melClip(
                    audioData.melSpectrogram.begin() + melStart,
                    audioData.melSpectrogram.begin() + melEnd);
                note->setClipMel(std::move(melClip));
            }
        }
    }

    // Create the second note (right part)
    Note secondNote;
    secondNote.setStartFrame(splitFrame);
    secondNote.setEndFrame(endFrame);
    secondNote.setSrcStartFrame(splitFrame);
    secondNote.setSrcEndFrame(endFrame);
    secondNote.setMidiNote(note->getMidiNote());
    secondNote.setLyric(note->getLyric());
    secondNote.setPitchOffset(0.0f);

    // Split clip waveform if available
    if (note->hasClipWaveform()) {
        const auto& clip = note->getClipWaveform();
        int splitOffset = (splitFrame - startFrame) * HOP_SIZE;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(clip.size())));
        std::vector<float> leftClip(clip.begin(), clip.begin() + splitOffset);
        std::vector<float> rightClip(clip.begin() + splitOffset, clip.end());
        note->setClipWaveform(std::move(leftClip));
        secondNote.setClipWaveform(std::move(rightClip));
    }

    // Split source clip waveform if available
    if (note->hasSrcClipWaveform()) {
        const auto& srcClip = note->getSrcClipWaveform();
        int splitOffset = (splitFrame - startFrame) * HOP_SIZE;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(srcClip.size())));
        std::vector<float> leftSrcClip(srcClip.begin(), srcClip.begin() + splitOffset);
        std::vector<float> rightSrcClip(srcClip.begin() + splitOffset, srcClip.end());
        note->setSrcClipWaveform(std::move(leftSrcClip));
        secondNote.setSrcClipWaveform(std::move(rightSrcClip));
    }

    // Split clip mel if available
    if (note->hasClipMel()) {
        const auto& mel = note->getClipMel();
        int splitOffset = splitFrame - startFrame;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(mel.size())));
        std::vector<std::vector<float>> leftMel(mel.begin(), mel.begin() + splitOffset);
        std::vector<std::vector<float>> rightMel(mel.begin() + splitOffset, mel.end());
        note->setClipMel(std::move(leftMel));
        secondNote.setClipMel(std::move(rightMel));
    }

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

    // Modify the first note (left part)
    note->setEndFrame(splitFrame);
    note->setSrcEndFrame(splitFrame);

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
