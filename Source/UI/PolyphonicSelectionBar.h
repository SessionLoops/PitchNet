#pragma once

#include "../JuceHeader.h"
#include "Sliders.h"

#include <functional>
#include <optional>

/**
 * The selected polyphonic notes' Note, Cent and Level, in the centre of the
 * main toolbar (polyphonic takes only, where the vocal edit tools would be).
 *
 *  - Note: the nearest semitone's name ("E4"). Drag it, type a name, or type
 *    +N / -N; every selected note moves by the same number of semitones.
 *  - Cent: the cents from that semitone (-50...+49). Drag or type; every
 *    selected note moves by the same number of cents.
 *  - Level: the gain in dB, set on every selected note.
 *
 * No labels: each box shows its unit ("E4", "+12 ct", "-3.0 dB") and its
 * tooltip names it. With nothing selected every box shows "-". With several
 * notes, Note and Cent show the first selected note's values, or "-" where
 * the notes differ; a change is applied relative to it, so the
 * notes keep their spread. Each box commits once, when the mouse is released
 * or a typed value is entered (one undo step, one render), never per step of
 * a drag; the scroll wheel is off for the same reason.
 */
class PolyphonicSelectionBar : public juce::Component
{
public:
    PolyphonicSelectionBar();
    ~PolyphonicSelectionBar() override;

    struct State
    {
        bool hasSelection = false;    // any unmuted note selected
        float midi = 0.0f;            // the first selected note's edited pitch (A4 = 69)
        int referenceHz = 440;
        bool notesDiffer = false;
        bool centsDiffer = false;
        std::optional<float> levelDb; // nullopt: none; NaN: the notes differ
    };
    void setState(const State& state);

    /** Moves every selected note by this many semitones (cents / 100 for the Cent box). */
    std::function<void(float semitones, bool fromCents)> onShiftSemitones;
    std::function<void(float gainDb)> onLevelChanged;
    // While a box is dragged: the change so far, to draw the notes live (no
    // render); onPreviewEnd when the drag ends, before any commit.
    std::function<void(float semitones)> onPreviewShift;
    std::function<void(float gainDb)> onPreviewLevel;
    std::function<void()> onPreviewEnd;

    /** The width the bar needs at its font size. */
    int getPreferredWidth() const;

    void resized() override;
    void refreshLocalisedText();

private:
    void setupBox(SliderBox& box);
    void commitNote();
    void commitCent();
    void commitLevel();
    int nearestNote() const;  // of state.midi, on the reference grid
    int centsFromNearest() const;

    State state;
    bool updating = false;
    bool noteTouched = false;  // dragged or typed since the last setState: shows its value, not "-"
    bool centTouched = false;
    bool levelTouched = false;

    SliderBox noteBox { "Note" };
    SliderBox centBox { "Cent" };
    SliderBox levelBox { "Level" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PolyphonicSelectionBar)
};
