#pragma once

#include "../../JuceHeader.h"

#include <functional>
#include <memory>

/** Audio handed to the Polyphonic Lab: any channel count (the first two are used), any sample rate. */
struct PolyLabSource
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    juce::String name;
};

/**
 * Polyphonic Lab (experimental): a test bench for the polyphonic engine in
 * Source/Audio/Polyphonic. It analyses a recording into notes, lets you
 * change single notes inside chords (pitch, timing, mute), A/B the result
 * against the original while it plays, solo a selection, and export a WAV.
 *
 * The Lab is separate from the PitchNet project: it never touches the project,
 * its undo history or its render. It plays through the app's audio device as an
 * extra callback, so it is only offered in the standalone app.
 */
class PolyLabWindow : public juce::DocumentWindow
{
public:
    /** Fills `source` with the open project's audio; returns false when there is none. */
    using ProjectAudioProvider = std::function<bool(PolyLabSource& source)>;

    PolyLabWindow(juce::AudioDeviceManager& deviceManager, ProjectAudioProvider projectAudio);
    ~PolyLabWindow() override;

    /** Shows the window; analyses the project's audio if nothing is loaded yet. */
    void open();

    void closeButtonPressed() override;

private:
    class Content;
    std::unique_ptr<Content> content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PolyLabWindow)
};
