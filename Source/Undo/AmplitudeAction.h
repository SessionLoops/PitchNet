#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/NoteGainCurve.h"
#include <functional>

// Gain-only edits operate on the existing composite, without pitch synthesis.
class AmplitudeAction : public UndoableAction
{
public:
    AmplitudeAction(Project& project, std::vector<Note*> notes,
                    std::vector<float> before, std::vector<float> after,
                    std::function<void()> changed)
        : project(project), notes(std::move(notes)), before(std::move(before)),
          after(std::move(after)), changed(std::move(changed)) {}

    void undo() override { apply(after, before); }
    void redo() override { apply(before, after); }
    bool requiresAudioResynthesis() const override { return false; }
    juce::String getName() const override { return "Change Amplitude"; }

private:
    void apply(const std::vector<float>& from, const std::vector<float>& to)
    {
        auto& audio = project.getAudioData();
        const int total = audio.waveform.getNumSamples();
        const int hop = HOP_SIZE;
        const int half = std::max(64, hop / 2) / 2;
        int start = total, end = 0;
        for (const auto* note : notes)
            if (note)
            {
                start = std::min(start, note->getStartFrame() * hop - half);
                end = std::max(end, note->getEndFrame() * hop + half);
            }
        start = std::clamp(start, 0, total);
        end = std::clamp(end, start, total);
        std::vector<NoteGainCurve::Region> oldRegions, newRegions;
        for (const auto& note : project.getNotes())
        {
            if (note.isRest()) continue;
            float oldDb = note.getVolumeDb(), newDb = oldDb;
            for (size_t i = 0; i < notes.size(); ++i)
                if (notes[i] == &note) { oldDb = from[i]; newDb = to[i]; break; }
            oldRegions.push_back({note.getStartFrame() * hop, note.getEndFrame() * hop, oldDb});
            newRegions.push_back({note.getStartFrame() * hop, note.getEndFrame() * hop, newDb});
        }
        const auto oldGain = NoteGainCurve::build(oldRegions, start, end - start, total, hop);
        const auto newGain = NoteGainCurve::build(newRegions, start, end - start, total, hop);
        for (int ch = 0; ch < audio.waveform.getNumChannels(); ++ch)
        {
            auto* samples = audio.waveform.getWritePointer(ch);
            for (int i = start; i < end; ++i)
                samples[i] *= newGain[static_cast<size_t>(i - start)] /
                              oldGain[static_cast<size_t>(i - start)];
        }
        for (size_t i = 0; i < notes.size(); ++i)
            if (notes[i])
            {
                notes[i]->setVolumeDb(to[i]);
                notes[i]->setRenderedEdit(!notes[i]->isNeutralForOriginalWaveform());
            }
        if (changed) changed();
    }

    Project& project;
    std::vector<Note*> notes;
    std::vector<float> before, after;
    std::function<void()> changed;
};
