#pragma once

#include "../../JuceHeader.h"
#include <optional>
#include <vector>

namespace MidiReference
{
struct Note
{
    double startSeconds;
    double endSeconds;
    int midiPitch;
};

struct Track
{
    int number;
    juce::String name;
    std::vector<Note> notes;
};

// Track numbers are one-based file indices. Tracks without notes are omitted.
std::vector<Track> readTracks(juce::InputStream& input);

// All times are relative to the guide's start. Different pitches tied for
// the greatest overlap have no unambiguous target.
std::optional<int> findPitchWithGreatestOverlap(const std::vector<Note>& notes,
                                                double startSeconds, double endSeconds);
}
