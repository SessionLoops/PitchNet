#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Undo/PitchDrawingAction.h"
#include "../Source/Utils/Constants.h"
#include <cassert>
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
    assert(note.getBakedDeltaPitch() == contour);
    assert(!note.isNeutralForOriginalWaveform());
    assert(project.hasF0DirtyRange());
    assert(std::abs(audio.f0[10] - midiToFreq(70)) < 0.01f);
    action.undo();
    assert(note.isNeutralForOriginalWaveform());
    assert(note.getOriginalDeltaPitch() == original.getOriginalDeltaPitch());
    assert(std::abs(audio.f0[10] - 440) < 0.01f);
    action.redo();
    assert(!note.isNeutralForOriginalWaveform());

    Project jsonRestored;
    assert(ProjectSerializer::fromJson(jsonRestored,
                                       ProjectSerializer::toJson(project)));
    assert(jsonRestored.getNotes()[0].getBakedDeltaPitch() == contour);
    assert(!jsonRestored.getNotes()[0].isNeutralForOriginalWaveform());
    for (auto mode : {ProjectSerializer::BinaryArchiveMode::selfContained,
                      ProjectSerializer::BinaryArchiveMode::hostBackedARA}) {
        juce::MemoryBlock archive;
        assert(ProjectSerializer::toBinaryArchive(project, archive, mode));
        Project restored;
        assert(ProjectSerializer::fromBinaryArchive(restored, archive.getData(), archive.getSize()));
        assert(restored.getNotes()[0].getBakedDeltaPitch() == contour);
        assert(!restored.getNotes()[0].isNeutralForOriginalWaveform());
    }
    std::cout << "Pitch drawing undo/redo, neutrality and persistence passed\n";
}
