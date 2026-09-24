#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Undo/PitchDrawingAction.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

int main() {
    Project project;
    Note original(0, 20, 69);
    original.setOriginalDeltaPitch(std::vector<float>(20, 0));
    original.setDeltaPitch(original.getOriginalDeltaPitch());
    project.addNote(original);
    auto& audio = project.getAudioData();
    audio.melSpectrogram.assign(20, std::vector<float>(1, 0));
    audio.f0.assign(20, 440);
    audio.basePitch.assign(20, 69);
    audio.deltaPitch.assign(20, 0);
    audio.voicedMask.assign(20, true);
    auto& note = project.getNotes()[0];
    const auto params = TransformParams::fromNote(note);
    const std::vector<float> contour(20, 1.0f);
    PitchDrawingAction action(&project, {{&note, params, {}}},
                            {{&note, params, contour}});
    action.redo();
    CHECK(note.getBakedDeltaPitch() == contour);
    CHECK(!note.isNeutralForOriginalWaveform());
    CHECK(project.hasF0DirtyRange());
    CHECK(std::abs(audio.f0[10] - midiToFreq(70)) < 0.01f);
    action.undo();
    CHECK(note.isNeutralForOriginalWaveform());
    CHECK(note.getOriginalDeltaPitch() == original.getOriginalDeltaPitch());
    CHECK(std::abs(audio.f0[10] - 440) < 0.01f);
    action.redo();
    CHECK(!note.isNeutralForOriginalWaveform());

    Project jsonRestored;
    CHECK(ProjectSerializer::fromJson(jsonRestored,
                                       ProjectSerializer::toJson(project)));
    CHECK(jsonRestored.getNotes()[0].getBakedDeltaPitch() == contour);
    CHECK(!jsonRestored.getNotes()[0].isNeutralForOriginalWaveform());
    for (auto mode : {ProjectSerializer::BinaryArchiveMode::selfContained,
                      ProjectSerializer::BinaryArchiveMode::hostBackedARA}) {
        juce::MemoryBlock archive;
        CHECK(ProjectSerializer::toBinaryArchive(project, archive, mode));
        Project restored;
        CHECK(ProjectSerializer::fromBinaryArchive(restored, archive.getData(), archive.getSize()));
        CHECK(restored.getNotes()[0].getBakedDeltaPitch() == contour);
        CHECK(!restored.getNotes()[0].isNeutralForOriginalWaveform());
    }
    std::cout << "Pitch drawing undo/redo, neutrality and persistence passed\n";
}
