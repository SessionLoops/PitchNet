#include "MidiExporter.h"
#include "../../Utils/Constants.h"
#include <algorithm>

bool MidiExporter::hasExportableNotes(const std::vector<Note>& notes) {
    return std::any_of(notes.begin(), notes.end(),
                       [](const Note& note) { return !note.isRest(); });
}

bool MidiExporter::exportToFile(const std::vector<Note>& notes,
                                const juce::File& file,
                                const ExportOptions& options) {
    if (!hasExportableNotes(notes))
        return false;

    auto midiFile = createMidiFile(notes, options);

    // Write to file
    juce::FileOutputStream outputStream(file);
    if (!outputStream.openedOk())
        return false;

    return midiFile.writeTo(outputStream);
}

juce::MidiFile MidiExporter::createMidiFile(const std::vector<Note>& notes,
                                            const ExportOptions& options) {
    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(options.ticksPerQuarterNote);

    juce::MidiMessageSequence track;

    // Add tempo event at the beginning
    if (options.includeTempoTrack) {
        // MIDI tempo is in microseconds per quarter note
        const double microsecondsPerQuarter = 60000000.0 / options.tempo;
        auto tempoEvent = juce::MidiMessage::tempoMetaEvent(
            static_cast<int>(microsecondsPerQuarter));
        tempoEvent.setTimeStamp(0);
        track.addEvent(tempoEvent);
    }

    // Convert each note to MIDI events
    for (const auto& note : notes) {
        // Skip rest notes
        if (note.isRest())
            continue;

        // Get adjusted pitch (includes user edits: midiNote + pitchOffset)
        float adjustedPitch = note.getAdjustedMidiNote();

        // Quantize to nearest semitone if requested
        int midiNoteValue = options.quantizePitch
            ? clampMidiNote(std::round(adjustedPitch))
            : clampMidiNote(adjustedPitch);

        // Convert frame positions to MIDI ticks
        int startTick = frameToTicks(note.getStartFrame(), options.tempo, options.ticksPerQuarterNote);
        int endTick = frameToTicks(note.getEndFrame(), options.tempo, options.ticksPerQuarterNote);

        // Ensure minimum note duration (at least 1 tick)
        if (endTick <= startTick)
            endTick = startTick + 1;

        // Note On
        auto noteOn = juce::MidiMessage::noteOn(
            options.channel + 1, // MIDI channels are 1-based in JUCE API
            midiNoteValue,
            static_cast<juce::uint8>(options.velocity));
        noteOn.setTimeStamp(startTick);
        track.addEvent(noteOn);

        // Note Off
        auto noteOff = juce::MidiMessage::noteOff(
            options.channel + 1,
            midiNoteValue);
        noteOff.setTimeStamp(endTick);
        track.addEvent(noteOff);
    }

    // Sort events by timestamp
    track.sort();

    // Update matched note on/off pairs
    track.updateMatchedPairs();

    // Add End of Track meta event (required by MIDI spec)
    // Must be placed after the last event
    double lastEventTime = track.getEndTime();
    auto endOfTrack = juce::MidiMessage::endOfTrack();
    endOfTrack.setTimeStamp(lastEventTime);
    track.addEvent(endOfTrack);

    // Add track to file
    midiFile.addTrack(track);

    return midiFile;
}

int MidiExporter::frameToTicks(int frame, float tempo, int ppq) {
    // Frame to seconds: frame * HOP_SIZE / SAMPLE_RATE
    double seconds = framesToSeconds(frame);
    return secondsToTicks(seconds, tempo, ppq);
}

int MidiExporter::secondsToTicks(double seconds, float tempo, int ppq) {
    // beats = seconds * (tempo / 60)
    // ticks = beats * ppq
    double beats = seconds * (tempo / 60.0);
    return static_cast<int>(beats * ppq);
}

int MidiExporter::clampMidiNote(float midiNote) {
    int note = static_cast<int>(midiNote);
    return juce::jlimit(0, 127, note);
}
