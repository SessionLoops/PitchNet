#pragma once

#include "UndoableAction.h"
#include "../Audio/ProjectGain.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/NoteGainCurve.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <memory>
#include <vector>

/**
 * A gain-only edit - a note's volume or sibilant balance, a breath note's
 * level - applied straight onto the rendered audio, without synthesis: the
 * project's whole gain curve (projectgain::regions) before and after the
 * edit, applied as their ratio over the samples the edit can reach. Every
 * gain floors at -60 dB, so the ratio always exists and undo is exact.
 */
class GainEditAction : public UndoableAction
{
public:
    // Puts the project into its state before (false) or after (true) the edit.
    using SetState = std::function<void(bool after)>;

    GainEditAction(Project& project, SetState setState, int firstSample,
                   int lastSample, std::function<void()> changed,
                   juce::String name)
        : project(project), setState(std::move(setState)),
          firstSample(firstSample), lastSample(lastSample),
          changed(std::move(changed)), name(std::move(name)) {}

    void undo() override { apply(false); }
    void redo() override { apply(true); }
    bool requiresAudioResynthesis() const override { return false; }
    juce::String getName() const override { return name; }

    /**
     * Run `change` on the project and carry its gain change over to the
     * rendered audio in [firstSample, lastSample), plus the curve's smoothing.
     */
    static void applyChange(Project& project, const std::function<void()>& change,
                            int firstSample, int lastSample)
    {
        auto& audio = project.getAudioData();
        const int total = audio.waveform.getNumSamples();
        const int hop = HOP_SIZE;
        const int half = std::max(64, hop / 2) / 2;
        const int start = std::clamp(firstSample - half, 0, total);
        const int end = std::clamp(lastSample + half, start, total);
        if (end <= start)
        {
            change();
            return;
        }
        const auto before = projectgain::regions(project);
        change();
        const auto ratio = projectgain::changeRatio(
            before, projectgain::regions(project), start, end - start, total);
        for (int ch = 0; ch < audio.waveform.getNumChannels(); ++ch)
        {
            auto* samples = audio.waveform.getWritePointer(ch);
            for (int i = start; i < end; ++i)
                samples[i] *= ratio[static_cast<size_t>(i - start)];
        }
    }

private:
    void apply(bool after)
    {
        setState(!after);
        applyChange(project, [this, after] { setState(after); }, firstSample,
                    lastSample);
        project.setModified(true);
        if (changed)
            changed();
    }

    Project& project;
    SetState setState;
    int firstSample;
    int lastSample;
    std::function<void()> changed;
    juce::String name;
};

namespace gainedit
{
/** The samples the notes span. */
inline std::pair<int, int> sampleRange(const std::vector<Note*>& notes)
{
    int first = INT_MAX;
    int last = INT_MIN;
    for (const auto* note : notes)
        if (note)
        {
            first = std::min(first, note->getStartFrame() * HOP_SIZE);
            last = std::max(last, note->getEndFrame() * HOP_SIZE);
        }
    return first <= last ? std::pair<int, int>{first, last}
                         : std::pair<int, int>{0, 0};
}

/** Volume (dB) and sibilant balance of notes, before and after. */
inline std::unique_ptr<GainEditAction> noteGains(
    Project& project, std::vector<Note*> notes, std::vector<float> volumeBefore,
    std::vector<float> volumeAfter, std::vector<float> balanceBefore,
    std::vector<float> balanceAfter, std::function<void()> changed,
    juce::String name)
{
    const auto [first, last] = projectgain::noteGainSpan(
        project, std::vector<const Note*>(notes.begin(), notes.end()));
    auto setState = [notes, volumeBefore, volumeAfter, balanceBefore,
                     balanceAfter](bool after)
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (!notes[i])
                continue;
            const auto& volume = after ? volumeAfter : volumeBefore;
            const auto& balance = after ? balanceAfter : balanceBefore;
            if (i < volume.size())
                notes[i]->setVolumeDb(volume[i]);
            if (i < balance.size())
                notes[i]->setSibilantBalance(balance[i]);
            notes[i]->setRenderedEdit(!notes[i]->isNeutralForOriginalWaveform());
        }
    };
    return std::make_unique<GainEditAction>(project, std::move(setState), first,
                                            last, std::move(changed),
                                            std::move(name));
}

/** One breath note's level change. */
struct BreathChange
{
    int sourceFirst = 0; // source frames whose gain is set, [first, last)
    int sourceLast = 0;
    int firstSample = 0; // where the breath plays
    int lastSample = 0;
    float before = 0.0f;
    float after = 0.0f;
};

/** Levels of breath notes (AudioData::breathGainDb), before and after. */
inline std::unique_ptr<GainEditAction> breathGains(
    Project& project, std::vector<BreathChange> changes,
    std::function<void()> changed, juce::String name)
{
    int first = INT_MAX;
    int last = INT_MIN;
    for (const auto& change : changes)
    {
        first = std::min(first, change.firstSample);
        last = std::max(last, change.lastSample);
    }
    if (first > last)
        first = last = 0;
    auto setState = [&project, changes](bool after)
    {
        auto& gains = project.getAudioData().breathGainDb;
        for (const auto& change : changes)
        {
            const int from = std::max(0, change.sourceFirst);
            const int to = change.sourceLast;
            if (to <= from)
                continue;
            const float db = after ? change.after : change.before;
            if (static_cast<int>(gains.size()) < to)
                gains.resize(static_cast<size_t>(to), 0.0f);
            std::fill(gains.begin() + from, gains.begin() + to, db);
        }
        while (!gains.empty() && gains.back() == 0.0f)
            gains.pop_back();
    };
    return std::make_unique<GainEditAction>(project, std::move(setState), first,
                                            last, std::move(changed),
                                            std::move(name));
}
} // namespace gainedit
