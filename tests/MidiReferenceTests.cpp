#include "../Source/Audio/IO/MidiReference.h"
#include "../Source/Models/ProjectSerializer.h"
#include "TestAssert.h"
#include <cmath>

namespace
{
void addEvent(juce::MidiMessageSequence& track, juce::MidiMessage message, double tick)
{
    message.setTimeStamp(tick);
    track.addEvent(message);
}

void addNote(juce::MidiMessageSequence& track, int pitch, double startTick, double endTick)
{
    addEvent(track, juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(100)), startTick);
    addEvent(track, juce::MidiMessage::noteOff(1, pitch), endTick);
}

std::vector<MidiReference::Track> read(const juce::MidiFile& file, int format)
{
    juce::MemoryOutputStream output;
    CHECK(file.writeTo(output, format));
    juce::MemoryInputStream input(output.getData(), output.getDataSize(), false);
    return MidiReference::readTracks(input);
}

void checkRestoredReference(const Project& project, bool visible)
{
    const auto& state = project.getMidiReference();
    CHECK(state.guide != nullptr);
    CHECK(state.guide->filename == "Missing Guide.mid");
    CHECK(state.guide->tracks.size() == 2);
    CHECK(state.selectedTrack == 1);
    CHECK(state.visible == visible);
    const auto& lead = state.guide->tracks[0];
    CHECK(lead.number == 2 && lead.name == "Lead");
    CHECK(lead.notes.size() == 1 && lead.notes[0].midiPitch == 64);
    CHECK(std::abs(lead.notes[0].startSeconds - 0.25) < 1e-8);
    CHECK(std::abs(lead.notes[0].endSeconds - 1.50) < 1e-8);
    const auto& selected = state.guide->tracks[state.selectedTrack];
    CHECK(selected.number == 3 && selected.notes.size() == 1);
    CHECK(MidiReference::findPitchWithGreatestOverlap(selected.notes, 0.0, 0.5) == 67);
    CHECK(project.getDoubleClickSnapMode() == DoubleClickSnapMode::MidiReference);
}
}

int main()
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote(480);
    juce::MidiMessageSequence tempo;
    addEvent(tempo, juce::MidiMessage::tempoMetaEvent(500000), 0);
    addEvent(tempo, juce::MidiMessage::tempoMetaEvent(1000000), 480);
    file.addTrack(tempo);

    juce::MidiMessageSequence lead;
    addEvent(lead, juce::MidiMessage::textMetaEvent(3, "Lead"), 0);
    addNote(lead, 64, 240, 960);
    addEvent(lead, juce::MidiMessage::noteOn(1, 65, static_cast<juce::uint8>(100)), 960);
    file.addTrack(lead);

    juce::MidiMessageSequence harmony;
    addNote(harmony, 67, 0, 480);
    file.addTrack(harmony);

    const auto tracks = read(file, 1);
    CHECK(tracks.size() == 2);
    CHECK(tracks[0].number == 2 && tracks[0].name == "Lead");
    CHECK(tracks[0].notes.size() == 1 && tracks[0].notes[0].midiPitch == 64);
    CHECK(std::abs(tracks[0].notes[0].startSeconds - 0.25) < 1e-8);
    CHECK(std::abs(tracks[0].notes[0].endSeconds - 1.50) < 1e-8);
    CHECK(tracks[1].number == 3 && tracks[1].notes[0].midiPitch == 67);

    Project saved;
    const auto midiGuide = std::make_shared<MidiReference::Guide>(
        MidiReference::Guide{ "Missing Guide.mid", tracks });
    CHECK(saved.setMidiReference({ midiGuide, 1, true }));
    saved.setDoubleClickSnapMode(DoubleClickSnapMode::MidiReference);

    Project reopened;
    const auto json = juce::JSON::toString(ProjectSerializer::toJson(saved));
    CHECK(ProjectSerializer::fromJson(reopened, juce::JSON::parse(json)));
    checkRestoredReference(reopened, true);

    CHECK(saved.setMidiReference({ midiGuide, 1, false }));
    using ArchiveMode = ProjectSerializer::BinaryArchiveMode;
    for (const auto mode : { ArchiveMode::selfContained, ArchiveMode::hostBackedARA })
    {
        juce::MemoryBlock archive;
        CHECK(ProjectSerializer::toBinaryArchive(saved, archive, mode));
        Project restored;
        CHECK(ProjectSerializer::fromBinaryArchive(restored, archive.getData(), archive.getSize()));
        checkRestoredReference(restored, false);
    }

    // Older project state has no guide and must clear one in a reused Project.
    CHECK(ProjectSerializer::fromJson(reopened, ProjectSerializer::toJson(Project{})));
    CHECK(!reopened.getMidiReference().guide);
    CHECK(!reopened.getMidiReference().visible);

    juce::MidiFile independent;
    independent.setTicksPerQuarterNote(480);
    addEvent(lead, juce::MidiMessage::tempoMetaEvent(1000000), 0);
    independent.addTrack(lead);
    independent.addTrack(harmony);
    const auto patterns = read(independent, 2);
    CHECK(patterns.size() == 2);
    CHECK(std::abs(patterns[0].notes[0].endSeconds - 2.0) < 1e-8);
    CHECK(std::abs(patterns[1].notes[0].endSeconds - 0.5) < 1e-8);

    juce::MidiFile empty;
    empty.setTicksPerQuarterNote(480);
    empty.addTrack(tempo);
    CHECK(read(empty, 0).empty());
    const char garbage[] = "Invalid MIDI file";
    juce::MemoryInputStream badInput(garbage, sizeof(garbage), false);
    CHECK(MidiReference::readTracks(badInput).empty());

    const std::vector<MidiReference::Note> guide{{0.0, 0.5, 60}, {0.5, 1.0, 64}};
    CHECK(MidiReference::findPitchWithGreatestOverlap(guide, 0.4, 0.9) == 64);
    CHECK(!MidiReference::findPitchWithGreatestOverlap(guide, 0.25, 0.75));
    CHECK(!MidiReference::findPitchWithGreatestOverlap(guide, 1.0, 1.5));
}
