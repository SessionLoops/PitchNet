#include "Project.h"
#include "../Utils/Constants.h"
#include "../Utils/PitchCurveProcessor.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float twoPi = 6.2831853071795864769f;

    int normalizeBeatNumerator(int numerator)
    {
        return juce::jlimit(1, 32, numerator);
    }

    int normalizeBeatDenominator(int denominator)
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

    TimelineGridDivision normalizeGridDivision(TimelineGridDivision division)
    {
        switch (division)
        {
        case TimelineGridDivision::Whole:
        case TimelineGridDivision::Half:
        case TimelineGridDivision::Quarter:
        case TimelineGridDivision::Eighth:
        case TimelineGridDivision::Sixteenth:
        case TimelineGridDivision::ThirtySecond:
            return division;
        default:
            return TimelineGridDivision::Quarter;
        }
    }
}

Project::Project()
{
}

Note *Project::getNoteAtFrame(int frame)
{
    for (auto &note : notes)
    {
        if (note.containsFrame(frame))
            return &note;
    }
    return nullptr;
}

std::vector<Note *> Project::getNotesInRange(int startFrame, int endFrame)
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.getStartFrame() < endFrame && note.getEndFrame() > startFrame)
            result.push_back(&note);
    }
    return result;
}

std::vector<Note *> Project::getSelectedNotes()
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.isSelected())
            result.push_back(&note);
    }
    return result;
}

bool Project::removeNoteByStartFrame(int startFrame)
{
    for (auto it = notes.begin(); it != notes.end(); ++it)
    {
        if (it->getStartFrame() == startFrame)
        {
            notes.erase(it);
            return true;
        }
    }
    return false;
}

void Project::deselectAllNotes()
{
    for (auto &note : notes)
        note.setSelected(false);
}

void Project::selectAllNotes(bool includeRests)
{
    for (auto &note : notes)
    {
        if (!includeRests && note.isRest())
            continue;
        note.setSelected(true);
    }
}

std::vector<Note *> Project::getDirtyNotes()
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.isDirty())
            result.push_back(&note);
    }
    return result;
}

void Project::clearAllDirty()
{
    for (auto &note : notes)
        note.clearDirty();
    // Also clear F0 dirty range
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
}

bool Project::hasDirtyNotes() const
{
    for (const auto &note : notes)
    {
        if (note.isDirty())
            return true;
    }
    return false;
}

void Project::setF0DirtyRange(int startFrame, int endFrame)
{
    if (f0DirtyStart < 0 || startFrame < f0DirtyStart)
        f0DirtyStart = startFrame;
    if (f0DirtyEnd < 0 || endFrame > f0DirtyEnd)
        f0DirtyEnd = endFrame;
}

void Project::clearF0DirtyRange()
{
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
}

bool Project::hasF0DirtyRange() const
{
    return f0DirtyStart >= 0 && f0DirtyEnd >= 0;
}

std::pair<int, int> Project::getF0DirtyRange() const
{
    return {f0DirtyStart, f0DirtyEnd};
}

std::pair<int, int> Project::getDirtyFrameRange() const
{
    int minStart = -1;
    int maxEnd = -1;

    // Check dirty notes
    for (const auto &note : notes)
    {
        if (note.isDirty())
        {
            if (minStart < 0 || note.getStartFrame() < minStart)
                minStart = note.getStartFrame();
            if (maxEnd < 0 || note.getEndFrame() > maxEnd)
                maxEnd = note.getEndFrame();
        }
    }

    // Also include F0 dirty range from Draw mode edits
    if (f0DirtyStart >= 0)
    {
        if (minStart < 0 || f0DirtyStart < minStart)
            minStart = f0DirtyStart;
    }
    if (f0DirtyEnd >= 0)
    {
        if (maxEnd < 0 || f0DirtyEnd > maxEnd)
            maxEnd = f0DirtyEnd;
    }

    return {minStart, maxEnd};
}

std::vector<float> Project::getAdjustedF0() const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Compose base + delta as dense curve; UV blending is handled downstream
    // by synthesis masks, so we do not zero F0 here.
    std::vector<float> adjustedF0 = PitchCurveProcessor::composeF0(*this,
                                                                   /*applyUvMask=*/false,
                                                                   globalPitchOffset);

    // Apply vibrato per note on top of composed curve
    for (const auto &note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int start = std::max(0, note.getStartFrame());
        const int end = std::min(note.getEndFrame(), static_cast<int>(adjustedF0.size()));

        for (int i = start; i < end; ++i)
        {
            if (i < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[i])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(i - start) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(i)] *= std::pow(2.0f, vib / 12.0f);
        }
    }

    return adjustedF0;
}

std::vector<float> Project::getAdjustedF0ForRange(int startFrame, int endFrame) const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Clamp range
    startFrame = std::max(0, startFrame);
    endFrame = std::min(endFrame, static_cast<int>(audioData.basePitch.size()));

    if (startFrame >= endFrame)
        return {};

    const int rangeSize = endFrame - startFrame;
    std::vector<float> adjustedF0(static_cast<size_t>(rangeSize), 0.0f);

    for (int i = 0; i < rangeSize; ++i)
    {
        const int globalIdx = startFrame + i;
        const float base = audioData.basePitch[static_cast<size_t>(globalIdx)];
        const float delta = (globalIdx < static_cast<int>(audioData.deltaPitch.size()))
                                ? audioData.deltaPitch[static_cast<size_t>(globalIdx)]
                                : 0.0f;
        float midi = base + delta + globalPitchOffset;
        adjustedF0[static_cast<size_t>(i)] = midiToFreq(midi);
    }

    // Apply vibrato for overlapping notes
    for (const auto &note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int overlapStart = std::max(note.getStartFrame(), startFrame);
        const int overlapEnd = std::min(note.getEndFrame(), endFrame);
        for (int frame = overlapStart; frame < overlapEnd; ++frame)
        {
            const int localIdx = frame - startFrame;
            if (frame < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[frame])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(frame - note.getStartFrame()) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(localIdx)] *= std::pow(2.0f, vib / 12.0f);
        }
    }

    return adjustedF0;
}

void Project::setLoopRange(double startSeconds, double endSeconds)
{
    if (startSeconds > endSeconds)
        std::swap(startSeconds, endSeconds);

    const double duration = audioData.getDuration();
    if (duration > 0.0)
    {
        startSeconds = juce::jlimit(0.0, duration, startSeconds);
        endSeconds = juce::jlimit(0.0, duration, endSeconds);
    }

    loopRange.startSeconds = startSeconds;
    loopRange.endSeconds = endSeconds;
    loopRange.enabled = loopRange.endSeconds > loopRange.startSeconds;
}

void Project::setLoopEnabled(bool enabled)
{
    if (enabled && loopRange.endSeconds <= loopRange.startSeconds)
        loopRange.enabled = false;
    else
        loopRange.enabled = enabled;
}

void Project::clearLoopRange()
{
    loopRange = {};
}

void Project::setScaleMode(ScaleMode mode)
{
    if (scaleMode == mode)
        return;

    scaleMode = mode;
    modified = true;
}

void Project::setScaleRootNote(int noteInOctave)
{
    const int normalized = juce::jlimit(-1, 11, noteInOctave);
    if (scaleRootNote == normalized)
        return;

    scaleRootNote = normalized;
    modified = true;
}

void Project::setPitchReferenceHz(int hz)
{
    const int normalized = juce::jlimit(380, 480, hz);
    if (pitchReferenceHz == normalized)
        return;

    pitchReferenceHz = normalized;
    modified = true;
}

void Project::setShowScaleColors(bool enabled)
{
    if (showScaleColors == enabled)
        return;

    showScaleColors = enabled;
    modified = true;
}

void Project::setSnapToSemitones(bool enabled)
{
    if (snapToSemitones == enabled)
        return;

    snapToSemitones = enabled;
    modified = true;
}

void Project::setDoubleClickSnapMode(DoubleClickSnapMode mode)
{
    if (doubleClickSnapMode == mode)
        return;

    doubleClickSnapMode = mode;
    modified = true;
}

void Project::setTimelineDisplayMode(TimelineDisplayMode mode)
{
    if (timelineDisplayMode == mode)
        return;

    timelineDisplayMode = mode;
    modified = true;
}

void Project::setTimelineBeatSignature(int numerator, int denominator)
{
    const int normalizedNumerator = normalizeBeatNumerator(numerator);
    const int normalizedDenominator = normalizeBeatDenominator(denominator);

    if (timelineBeatNumerator == normalizedNumerator &&
        timelineBeatDenominator == normalizedDenominator)
        return;

    timelineBeatNumerator = normalizedNumerator;
    timelineBeatDenominator = normalizedDenominator;
    modified = true;
}

void Project::setTimelineTempoBpm(double bpm)
{
    const double normalized = juce::jlimit(20.0, 300.0, bpm);
    if (std::abs(timelineTempoBpm - normalized) < 1.0e-6)
        return;

    timelineTempoBpm = normalized;
    modified = true;
}

void Project::setTimelineGridDivision(TimelineGridDivision division)
{
    const auto normalized = normalizeGridDivision(division);
    if (timelineGridDivision == normalized)
        return;

    timelineGridDivision = normalized;
    modified = true;
}

void Project::setTimelineSnapCycle(bool enabled)
{
    if (timelineSnapCycle == enabled)
        return;

    timelineSnapCycle = enabled;
    modified = true;
}

// ---------------------------------------------------------------------------
// composeGlobalWaveform: rebuild audioData.waveform from originalWaveform +
// per-note synthWaveforms, mapping each segment (gap/note) from its source
// position in originalWaveform to its output position in the timeline.
//
// This ensures that non-note regions (breaths, consonants, silence) shift
// along with notes during ripple stretch, and the buffer grows as needed.
// ---------------------------------------------------------------------------
void Project::composeGlobalWaveform()
{
    auto &waveform = audioData.waveform;
    const auto &origWaveform = audioData.originalWaveform;

    const int numChannels = waveform.getNumChannels();
    const int origSamples = origWaveform.getNumSamples();
    if (numChannels == 0 || origSamples == 0)
        return;

    // --- Step 1: Collect non-rest notes sorted by output position ----------
    std::vector<const Note *> sortedNotes;
    sortedNotes.reserve(notes.size());
    for (const auto &note : notes)
    {
        if (!note.isRest())
            sortedNotes.push_back(&note);
    }
    std::sort(sortedNotes.begin(), sortedNotes.end(),
              [](const Note *a, const Note *b)
              {
                  return a->getStartFrame() < b->getStartFrame();
              });

    // --- Step 2: Compute required output buffer length --------------------
    // Output must hold all shifted notes + trailing gap after the last note.
    int requiredSamples = origSamples;
    if (!sortedNotes.empty())
    {
        const auto *last = sortedNotes.back();
        // Last note end (account for synthWaveform that may differ from frame range)
        int lastEnd = last->getEndFrame() * HOP_SIZE;
        if (last->hasSynthWaveform())
        {
            int synthEnd = last->getStartFrame() * HOP_SIZE + static_cast<int>(last->getSynthWaveform().size());
            lastEnd = std::max(lastEnd, synthEnd);
        }
        requiredSamples = std::max(requiredSamples, lastEnd);
        // Trailing gap: original audio after last note's source end, placed
        // after last note's output end.
        int srcTrailLen = std::max(0,
                                   origSamples - last->getSrcEndFrame() * HOP_SIZE);
        requiredSamples = std::max(requiredSamples,
                                   last->getEndFrame() * HOP_SIZE + srcTrailLen);
    }

    // Resize waveform buffer if needed (grow only — shrinking left to caller)
    if (waveform.getNumSamples() < requiredSamples)
        waveform.setSize(numChannels, requiredSamples, false, true, false);
    const int totalSamples = waveform.getNumSamples();

    // --- Step 3: Zero the output ------------------------------------------
    waveform.clear();

    // Helper: copy from origWaveform[srcOff .. srcOff+len) to
    //         waveform[dstOff .. dstOff+len) with bounds clamping.
    auto copyFromOrig = [&](int srcOff, int dstOff, int len)
    {
        if (len <= 0)
            return;
        if (srcOff < 0)
        {
            len += srcOff;
            dstOff -= srcOff;
            srcOff = 0;
        }
        if (dstOff < 0)
        {
            len += dstOff;
            srcOff -= dstOff;
            dstOff = 0;
        }
        if (len <= 0)
            return;
        len = std::min(len, origSamples - srcOff);
        len = std::min(len, totalSamples - dstOff);
        if (len <= 0)
            return;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float *src = origWaveform.getReadPointer(
                std::min(ch, std::max(0, origWaveform.getNumChannels() - 1)));
            float *dst = waveform.getWritePointer(ch);
            std::copy(src + srcOff, src + srcOff + len, dst + dstOff);
        }
    };

    // Helper: time-stretch (linear interpolation) from origWaveform[srcOff..srcOff+srcLen)
    //         into waveform[dstOff..dstOff+dstLen).  Handles srcLen != dstLen gracefully
    //         so that gaps/segments are never truncated with trailing zeros.
    auto stretchFromOrig = [&](int srcOff, int dstOff, int srcLen, int dstLen)
    {
        if (srcLen <= 0 || dstLen <= 0)
            return;
        // Clamp source range
        if (srcOff < 0)
        {
            srcLen += srcOff;
            srcOff = 0;
        }
        srcLen = std::min(srcLen, origSamples - srcOff);
        if (srcLen <= 0)
            return;
        // Clamp destination range
        if (dstOff < 0)
        {
            dstLen += dstOff;
            dstOff = 0;
        }
        dstLen = std::min(dstLen, totalSamples - dstOff);
        if (dstLen <= 0)
            return;

        // If lengths match, straight copy is fine (no interpolation overhead)
        if (srcLen == dstLen)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float *src = origWaveform.getReadPointer(
                    std::min(ch, std::max(0, origWaveform.getNumChannels() - 1)));
                float *dst = waveform.getWritePointer(ch);
                std::copy(src + srcOff, src + srcOff + srcLen, dst + dstOff);
            }
            return;
        }

        // Linear interpolation resample: map each dst sample to a fractional src position
        const double ratio = (srcLen <= 1) ? 0.0
                                           : static_cast<double>(srcLen - 1) / static_cast<double>(dstLen - 1);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float *src = origWaveform.getReadPointer(
                std::min(ch, std::max(0, origWaveform.getNumChannels() - 1)));
            float *dst = waveform.getWritePointer(ch);
            for (int i = 0; i < dstLen; ++i)
            {
                const double srcPos = static_cast<double>(i) * ratio;
                const int idx = static_cast<int>(srcPos);
                const float frac = static_cast<float>(srcPos - idx);
                const int s0 = srcOff + std::min(idx, srcLen - 1);
                const int s1 = srcOff + std::min(idx + 1, srcLen - 1);
                dst[dstOff + i] = src[s0] + frac * (src[s1] - src[s0]);
            }
        }
    };

    // --- Step 4: Place segments via src→dst coordinate mapping -------------
    // Timeline = [leading gap][note0][gap01][note1]...[trailing gap]
    // Each segment is mapped from its source position (originalWaveform) to
    // its output position, so gaps shift together with notes.

    if (sortedNotes.empty())
    {
        // No notes — copy entire original
        copyFromOrig(0, 0, origSamples);
    }
    else
    {
        // Leading gap: orig[0..firstNote.srcStart) → out[0..firstNote.start)
        {
            int srcLen = sortedNotes[0]->getSrcStartFrame() * HOP_SIZE;
            int dstLen = sortedNotes[0]->getStartFrame() * HOP_SIZE;
            stretchFromOrig(0, 0, srcLen, dstLen);
        }

        for (size_t i = 0; i < sortedNotes.size(); ++i)
        {
            const auto *note = sortedNotes[i];

            // Always place original audio as base layer for every note region.
            // For notes with synthWaveform, this provides a smooth base that
            // the crossfade in Step 5 blends against at note boundaries,
            // avoiding the synth→silence→synth discontinuity.
            {
                int srcStart = note->getSrcStartFrame() * HOP_SIZE;
                int srcLen = (note->getSrcEndFrame() - note->getSrcStartFrame()) * HOP_SIZE;
                int dstStart = note->getStartFrame() * HOP_SIZE;
                int dstLen = (note->getEndFrame() - note->getStartFrame()) * HOP_SIZE;
                stretchFromOrig(srcStart, dstStart, srcLen, dstLen);
            }

            // Gap after this note → before next note (or trailing gap)
            int gapSrcStart = note->getSrcEndFrame() * HOP_SIZE;
            int gapDstStart = note->getEndFrame() * HOP_SIZE;
            int gapSrcEnd, gapDstEnd;
            if (i + 1 < sortedNotes.size())
            {
                gapSrcEnd = sortedNotes[i + 1]->getSrcStartFrame() * HOP_SIZE;
                gapDstEnd = sortedNotes[i + 1]->getStartFrame() * HOP_SIZE;
            }
            else
            {
                // Trailing gap: preserve original gap length (1:1 copy),
                // don't stretch to fill the entire buffer which may be
                // larger than needed from a previous longer stretch.
                gapSrcEnd = origSamples;
                gapDstEnd = gapDstStart + std::max(0, gapSrcEnd - gapSrcStart);
            }
            int gapSrcLen = gapSrcEnd - gapSrcStart;
            int gapDstLen = gapDstEnd - gapDstStart;
            if (gapSrcLen > 0 && gapDstLen > 0)
                stretchFromOrig(gapSrcStart, gapDstStart,
                                gapSrcLen, gapDstLen);
        }
    }

    // --- Step 5: Overlay synthWaveforms with adaptive edge crossfade ------
    // Pre-compute adjacency: collect synth notes and check which edges face
    // another synth note vs. a gap.  Adjacent-synth edges use a much shorter
    // crossfade so we don't linger in the base (wrong-pitch) audio.
    //
    // Each synthWaveform may contain margin samples (preroll/postroll) beyond
    // the note body, allowing real-audio crossfade at boundaries.
    struct SynthNoteInfo
    {
        const Note *note;
        int bodyStartSample;             // noteStart * HOP_SIZE (where the body begins in global coords)
        int bodySamples;                 // note body length in samples
        int preroll;                     // margin samples before body in synthWaveform
        int synthTotalLen;               // total synthWaveform length
        bool leftAdjacentSynth = false;  // previous synth note is adjacent
        bool rightAdjacentSynth = false; // next synth note is adjacent
    };

    std::vector<SynthNoteInfo> synthInfos;
    synthInfos.reserve(sortedNotes.size());
    for (const auto *n : sortedNotes)
    {
        if (!n->hasSynthWaveform())
            continue;
        SynthNoteInfo si;
        si.note = n;
        si.bodyStartSample = n->getStartFrame() * HOP_SIZE;
        si.bodySamples = (n->getEndFrame() - n->getStartFrame()) * HOP_SIZE;
        si.preroll = n->getSynthPreroll();
        si.synthTotalLen = static_cast<int>(n->getSynthWaveform().size());
        if (si.synthTotalLen <= 0)
            continue;
        synthInfos.push_back(si);
    }

    // Detect adjacency: two synth notes are "adjacent" when the gap between
    // them is at most kAdjacencyThreshold samples.
    constexpr int kAdjacencyThreshold = HOP_SIZE; // 1 frame tolerance
    for (size_t si = 0; si + 1 < synthInfos.size(); ++si)
    {
        auto &curr = synthInfos[si];
        auto &next = synthInfos[si + 1];
        int currBodyEnd = curr.bodyStartSample + curr.bodySamples;
        int nextBodyStart = next.bodyStartSample;
        if (nextBodyStart - currBodyEnd <= kAdjacencyThreshold)
        {
            curr.rightAdjacentSynth = true;
            next.leftAdjacentSynth = true;
        }
    }

    constexpr int kGapFadeSamples = 512;   // ~11.6ms — crossfade to base at gaps
    constexpr int kSpliceFadeSamples = 64; // ~1.5ms  — minimal fade at synth-synth edges

    for (const auto &si : synthInfos)
    {
        const auto &synthWave = si.note->getSynthWaveform();
        const int preroll = si.preroll;
        // The synthWaveform global start (including preroll margin)
        const int synthGlobalStart = si.bodyStartSample - preroll;
        const int synthTotalLen = si.synthTotalLen;

        // Choose fade lengths per edge
        const int leftFadeReq = si.leftAdjacentSynth ? kSpliceFadeSamples : kGapFadeSamples;
        const int rightFadeReq = si.rightAdjacentSynth ? kSpliceFadeSamples : kGapFadeSamples;
        const int maxFade = std::max(1, si.bodySamples / 4);
        const int leftFade = std::min(leftFadeReq, maxFade);
        const int rightFade = std::min(rightFadeReq, maxFade);

        // We overlay only the body portion [preroll .. preroll+bodySamples)
        // The margin is reserved for Step 6 crossfade.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float *dst = waveform.getWritePointer(ch);

            for (int i = 0; i < si.bodySamples; ++i)
            {
                const int globalIdx = si.bodyStartSample + i;
                if (globalIdx < 0 || globalIdx >= totalSamples)
                    continue;

                // Asymmetric edge envelope
                float env = 1.0f;
                if (i < leftFade && leftFade > 1)
                {
                    const float t = static_cast<float>(i) /
                                    static_cast<float>(leftFade);
                    env = t * t * (3.0f - 2.0f * t); // smoothstep
                }
                else if (i >= si.bodySamples - rightFade && rightFade > 1)
                {
                    const int fromEnd = si.bodySamples - 1 - i;
                    const float t = static_cast<float>(fromEnd) /
                                    static_cast<float>(rightFade);
                    env = t * t * (3.0f - 2.0f * t);
                }

                const int synthIdx = preroll + i;
                if (synthIdx < 0 || synthIdx >= si.synthTotalLen)
                    continue;

                const float base = dst[globalIdx];
                const float synth = synthWave[static_cast<size_t>(synthIdx)];
                dst[globalIdx] = base + env * (synth - base);
            }
        }
    }

    // --- Step 6: Direct synth-to-synth crossfade at adjacent boundaries ---
    // At adjacent synth note boundaries, both synthWaveforms now contain real
    // margin audio (preroll of next note, postroll of current note) from the
    // same continuous synthesis pass.  We crossfade these real signals instead
    // of using held-value extrapolation, eliminating the remaining pop.
    constexpr int kSpliceHalf = 64; // 64 samples each side = 128 total ≈ 2.9ms

    for (size_t si = 0; si + 1 < synthInfos.size(); ++si)
    {
        const auto &curr = synthInfos[si];
        const auto &next = synthInfos[si + 1];
        if (!curr.rightAdjacentSynth)
            continue; // not adjacent

        const auto &sw1 = curr.note->getSynthWaveform();
        const auto &sw2 = next.note->getSynthWaveform();
        const int n1BodyStart = curr.bodyStartSample;
        const int n1BodyLen = curr.bodySamples;
        const int n1Preroll = curr.preroll;
        const int n1TotalLen = curr.synthTotalLen;
        const int n2BodyStart = next.bodyStartSample;
        const int n2Preroll = next.preroll;
        const int n2TotalLen = next.synthTotalLen;

        // Global start of each synth (including margins)
        const int n1GlobalStart = n1BodyStart - n1Preroll;
        const int n2GlobalStart = n2BodyStart - n2Preroll;

        // Junction: where note1 body ends
        const int junction = n1BodyStart + n1BodyLen;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float *dst = waveform.getWritePointer(ch);

            for (int k = -kSpliceHalf; k < kSpliceHalf; ++k)
            {
                const int pos = junction + k;
                if (pos < 0 || pos >= totalSamples)
                    continue;

                // Crossfade parameter: 0.0 at left edge → 1.0 at right edge
                const float t_raw = static_cast<float>(k + kSpliceHalf) /
                                    static_cast<float>(2 * kSpliceHalf);
                const float t = t_raw * t_raw * (3.0f - 2.0f * t_raw);

                // synth1 value — use real margin data if available
                const int s1idx = pos - n1GlobalStart;
                float val1;
                if (s1idx >= 0 && s1idx < n1TotalLen)
                {
                    val1 = sw1[static_cast<size_t>(s1idx)];
                }
                else
                {
                    // Fallback: clamp to nearest valid sample
                    val1 = sw1[static_cast<size_t>(std::clamp(s1idx, 0, n1TotalLen - 1))];
                }

                // synth2 value — use real margin data if available
                const int s2idx = pos - n2GlobalStart;
                float val2;
                if (s2idx >= 0 && s2idx < n2TotalLen)
                {
                    val2 = sw2[static_cast<size_t>(s2idx)];
                }
                else
                {
                    // Fallback: clamp to nearest valid sample
                    val2 = sw2[static_cast<size_t>(std::clamp(s2idx, 0, n2TotalLen - 1))];
                }

                dst[pos] = val1 * (1.0f - t) + val2 * t;
            }
        }
    }
}
