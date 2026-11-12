#pragma once

#include "../../JuceHeader.h"
#include <memory>
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

struct Guide
{
    juce::String filename;
    std::vector<Track> tracks;
};

struct State
{
    std::shared_ptr<const Guide> guide;
    size_t selectedTrack = 0;
    bool visible = false;
};

// Track numbers are one-based file indices. Tracks without notes are omitted.
std::vector<Track> readTracks(juce::InputStream& input);

// All times are relative to the guide's start. Different pitches tied for
// the greatest overlap have no unambiguous target.
std::optional<int> findPitchWithGreatestOverlap(const std::vector<Note>& notes,
                                                double startSeconds, double endSeconds);
}
