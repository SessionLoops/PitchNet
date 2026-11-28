// Frames between notes must keep their analysed pitch through every rebuild.
//
// rebuildCurvesFromSource() gives every frame a delta, gaps between notes
// included. rebuildBaseFromNotes() used to reset all frames to 0 and refill
// only note frames, so a full rebuild (pitch drawing preview/cancel, undo,
// ...) snapped gap frames onto the base transition: visible one- or
// two-frame spikes at note boundaries.

#include "../Source/Models/Project.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/PitchCurveProcessor.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
constexpr int kFrames = 60;

// Two notes, 57 and 64, with a two-frame gap (frames 30 and 31) in which
// the singer slides between them.
void setUp(Project& project)
{
    project.addNote(Note(0, 30, 57.0f));
    project.addNote(Note(32, kFrames, 64.0f));

    auto& audio = project.getAudioData();
    audio.melSpectrogram.assign(kFrames, std::vector<float>(1, 0.0f));
    audio.voicedMask.assign(kFrames, true);
    audio.denseF0.resize(kFrames);
    for (int i = 0; i < kFrames; ++i)
    {
        // Wobbly slide from 57 to 64 so no frame equals its base pitch.
        const float t = static_cast<float>(i) / (kFrames - 1);
        const float midi = 57.0f + 7.0f * t + 0.3f * std::sin(i * 0.7f);
        audio.denseF0[static_cast<size_t>(i)] = midiToFreq(midi);
    }
    audio.f0 = audio.denseF0;
    PitchCurveProcessor::rebuildCurvesFromSource(project, audio.denseF0);
}

bool sameCurve(const std::vector<float>& a, const std::vector<float>& b,
               float toleranceHz)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > toleranceHz)
        {
            std::cerr << "  frame " << i << ": " << a[i] << " vs " << b[i] << "\n";
            return false;
        }
    return true;
}
} // namespace

int main()
{
    {
        Project project;
        setUp(project);
        const auto analysed = project.getAudioData().f0;

        PitchCurveProcessor::rebuildBaseFromNotes(project);
        CHECK(sameCurve(project.getAudioData().f0, analysed, 0.01f));

        // Repeated rebuilds are stable too.
        PitchCurveProcessor::rebuildBaseFromNotes(project);
        CHECK(sameCurve(project.getAudioData().f0, analysed, 0.01f));
    }

    {
        Project project;
        setUp(project);
        const auto analysed = project.getAudioData().f0;

        std::vector<Note*> both;
        for (auto& note : project.getNotes())
            both.push_back(&note);
        PitchCurveProcessor::rebuildBaseFromNotesForDrag(project, both);
        CHECK(sameCurve(project.getAudioData().f0, analysed, 0.01f));
    }

    {
        // Moving a note and moving it back restores the gap as well.
        Project project;
        setUp(project);
        const auto analysed = project.getAudioData().f0;
        auto& second = project.getNotes()[1];
        second.setMidiNote(66.0f);
        PitchCurveProcessor::rebuildBaseFromNotes(project);
        second.setMidiNote(64.0f);
        PitchCurveProcessor::rebuildBaseFromNotes(project);
        CHECK(sameCurve(project.getAudioData().f0, analysed, 0.01f));
    }

    std::cout << "Pitch curve gaps survive rebuilds\n";
}
