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
    : macroParameters(std::make_shared<MacroParameters>())
{
}

void Project::addNote(Note note)
{
    // Timeline consumers (including note preview context) expect neighboring
    // notes to also be neighboring elements. A split creates its tail after
    // all existing notes, so appending it would break that invariant.
    const int startFrame = note.getStartFrame();
    const auto insertAt = std::upper_bound(
        notes.begin(), notes.end(), startFrame,
        [](int start, const Note &existing)
        {
            return start < existing.getStartFrame();
        });
    notes.insert(insertAt, std::move(note));
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
    bool changed = false;
    if (mode != ScaleMode::None && mode != ScaleMode::Chromatic &&
        preferredScaleMode != mode)
    {
        preferredScaleMode = mode;
        changed = true;
    }

    if (macroParameters->scaleMode != mode)
    {
        macroParameters->scaleMode = mode;
        changed = true;
    }

    if (changed)
        modified = true;
}

void Project::setPreferredScaleMode(ScaleMode mode)
{
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic ||
        preferredScaleMode == mode)
        return;

    preferredScaleMode = mode;
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
    const int normalized = juce::jlimit(430, 450, hz);
    if (macroParameters->pitchReferenceHz == normalized)
        return;

    macroParameters->pitchReferenceHz = normalized;
    modified = true;
}

void Project::setSnapToSemitones(bool enabled)
{
    if (macroParameters->snapToSemitones == enabled)
        return;

    macroParameters->snapToSemitones = enabled;
    modified = true;
}

void Project::setDragSnapMode(DragSnapMode mode)
{
    if (macroParameters->dragSnapMode == mode)
        return;

    macroParameters->dragSnapMode = mode;
    modified = true;
}

void Project::setTimelineDisplayMode(TimelineDisplayMode mode)
{
    if (macroParameters->timelineDisplayMode == mode)
        return;

    macroParameters->timelineDisplayMode = mode;
    modified = true;
}

void Project::setTimelineBeatSignature(int numerator, int denominator)
{
    const int normalizedNumerator = normalizeBeatNumerator(numerator);
    const int normalizedDenominator = normalizeBeatDenominator(denominator);

    if (macroParameters->timelineBeatNumerator == normalizedNumerator &&
        macroParameters->timelineBeatDenominator == normalizedDenominator)
        return;

    macroParameters->timelineBeatNumerator = normalizedNumerator;
    macroParameters->timelineBeatDenominator = normalizedDenominator;
    modified = true;
}

void Project::setTimelineTempoBpm(double bpm)
{
    const double normalized = juce::jlimit(20.0, 300.0, bpm);
    if (std::abs(macroParameters->timelineTempoBpm - normalized) < 1.0e-6)
        return;

    macroParameters->timelineTempoBpm = normalized;
    modified = true;
}

void Project::setTimelineGridDivision(TimelineGridDivision division)
{
    const auto normalized = normalizeGridDivision(division);
    if (macroParameters->timelineGridDivision == normalized)
        return;

    macroParameters->timelineGridDivision = normalized;
    modified = true;
}

void Project::setTimelineSnapCycle(bool enabled)
{
    if (macroParameters->timelineSnapCycle == enabled)
        return;

    macroParameters->timelineSnapCycle = enabled;
    modified = true;
}

void Project::setMacroParameters(std::shared_ptr<MacroParameters> parameters)
{
    if (parameters)
        macroParameters = std::move(parameters);
}
