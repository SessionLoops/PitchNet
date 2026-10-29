#pragma once

#include "../../JuceHeader.h"
#include <functional>

/**
 * A button that hands the current notes to the host as a MIDI file.
 *
 * Dragging it out of the window performs an OS-level file drag, so the .mid
 * lands wherever it is dropped - a DAW track, the arrangement, Finder. A plain
 * click (no drag) falls back to the regular "save as" export.
 *
 * The component does not know about notes or projects: the owner supplies
 * onCreateDragFile, which writes a fresh MIDI file and returns it (or an
 * empty File when there is nothing to export).
 */
class MidiDragButton : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    MidiDragButton();
    ~MidiDragButton() override = default;

    void setButtonText(const juce::String& text);

    void paint(juce::Graphics& g) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    /** Writes the MIDI file to drag. Return an empty File to cancel the drag. */
    std::function<juce::File()> onCreateDragFile;
    /** Called on a click that did not turn into a drag. */
    std::function<void()> onClick;

private:
    void paintIcon(juce::Graphics& g, juce::Rectangle<float> area) const;

    juce::String buttonText;
    bool hovered = false;
    bool pressed = false;
    bool dragStarted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiDragButton)
};
