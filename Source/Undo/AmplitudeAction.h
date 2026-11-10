#pragma once

#include "GainEditAction.h"

// Gain-only edits operate on the existing composite, without pitch synthesis.
// A note volume change; see GainEditAction.
class AmplitudeAction : public GainEditAction
{
public:
    AmplitudeAction(Project& project, std::vector<Note*> notes,
                    std::vector<float> before, std::vector<float> after,
                    std::function<void()> changed, juce::String name = "Change Amplitude")
        : GainEditAction(project, makeSetState(notes, std::move(before), std::move(after)),
                         span(project, notes).first, span(project, notes).second,
                         std::move(changed),
                         std::move(name)) {}

private:
    static std::pair<int, int> span(const Project& project,
                                    const std::vector<Note*>& notes)
    {
        return projectgain::noteGainSpan(
            project, std::vector<const Note*>(notes.begin(), notes.end()));
    }

    static SetState makeSetState(std::vector<Note*> notes, std::vector<float> before,
                                 std::vector<float> after)
    {
        return [notes = std::move(notes), before = std::move(before),
                after = std::move(after)](bool toAfter)
        {
            const auto& volume = toAfter ? after : before;
            for (size_t i = 0; i < notes.size() && i < volume.size(); ++i)
                if (notes[i])
                {
                    notes[i]->setVolumeDb(volume[i]);
                    notes[i]->setRenderedEdit(!notes[i]->isNeutralForOriginalWaveform());
                }
        };
    }
};
