#include "../Source/Audio/IO/MidiReference.h"
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
