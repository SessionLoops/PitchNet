#include "MidiReference.h"

#include <algorithm>
#include <cmath>

namespace MidiReference
{
namespace
{
Track readTrack(const juce::MidiMessageSequence& events, int number)
{
    Track track{number, {}, {}};
    for (const auto* event : events)
    {
        const auto& message = event->message;
        if (track.name.isEmpty() && message.isTrackNameEvent())
            track.name = message.getTextFromTextMetaEvent().trim();
        if (!message.isNoteOn() || event->noteOffObject == nullptr)
            continue;

        const auto start = message.getTimeStamp();
        const auto end = event->noteOffObject->message.getTimeStamp();
        if (std::isfinite(start) && std::isfinite(end) && end > start)
            track.notes.push_back({start, end, message.getNoteNumber()});
    }
    return track;
}
}

std::vector<Track> readTracks(juce::InputStream& input)
{
    juce::MidiFile file;
    int format = 0;
    if (!file.readFrom(input, true, &format) || file.getTimeFormat() == 0)
        return {};

    // Format 2 tracks are independent sequences with their own tempo maps.
    const bool independentTempos = format == 2 && file.getTimeFormat() > 0;
    if (!independentTempos)
        file.convertTimestampTicksToSeconds();

    std::vector<Track> tracks;
    for (int index = 0; index < file.getNumTracks(); ++index)
    {
        const auto* events = file.getTrack(index);
        juce::MidiFile sequence;
        if (independentTempos)
        {
            sequence.setTicksPerQuarterNote(file.getTimeFormat());
            sequence.addTrack(*events);
            sequence.convertTimestampTicksToSeconds();
            events = sequence.getTrack(0);
        }

        auto track = readTrack(*events, index + 1);
        if (!track.notes.empty())
            tracks.push_back(std::move(track));
    }
    return tracks;
}

std::optional<int> findPitchWithGreatestOverlap(const std::vector<Note>& notes,
                                                double startSeconds, double endSeconds)
{
    constexpr double tieTolerance = 1e-8;
    double bestOverlap = 0.0;
    std::optional<int> bestPitch;
    bool ambiguous = false;
    for (const auto& note : notes)
    {
        const double overlap = std::min(endSeconds, note.endSeconds) -
                               std::max(startSeconds, note.startSeconds);
        if (overlap <= 0.0)
            continue;

        if (!bestPitch || overlap > bestOverlap + tieTolerance)
        {
            bestOverlap = overlap;
            bestPitch = note.midiPitch;
            ambiguous = false;
        }
        else if (std::abs(overlap - bestOverlap) <= tieTolerance && note.midiPitch != *bestPitch)
        {
            ambiguous = true;
        }
    }
    return ambiguous ? std::nullopt : bestPitch;
}
}
