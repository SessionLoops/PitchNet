#include "PolyphonicSelectionBar.h"

#include "Components/AppFont.h"
#include "../Utils/Localization.h"
#include "../Utils/ScaleUtils.h"

#include <cmath>

namespace
{
constexpr int kPad = 12;          // inside the capsule, left and right
constexpr int kGap = 12;          // between the boxes
// One width for all three, set by the longest value ("-24.0 dB").
constexpr int kBoxWidth = 66;
constexpr int kBoxHeight = 24;
const juce::String kDiffer = juce::String::fromUTF8("\xe2\x80\x93"); // en dash

const char *const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

juce::String noteName(int midi)
{
    const int pitchClass = ((midi % 12) + 12) % 12;
    const int octave = static_cast<int>(std::floor(midi / 12.0)) - 1;
    return juce::String(kNames[pitchClass]) + juce::String(octave);
}

/** "E4", "Eb4", "f#3", "C-1" -> MIDI; nullopt when it is not a note name. */
std::optional<int> parseNoteName(juce::String text)
{
    text = text.trim().toUpperCase();
    if (text.isEmpty())
        return std::nullopt;
    static const int letters[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const auto first = text[0];
    if (first < 'A' || first > 'G')
        return std::nullopt;
    int pitchClass = letters[first - 'A'];
    int i = 1;
    if (i < text.length() && (text[i] == '#' || text[i] == 'B'))
    {
        // "B" after a letter is a flat ("EB4"); a lone "B4" was handled as the letter.
        pitchClass += text[i] == '#' ? 1 : -1;
        ++i;
    }
    const auto octaveText = text.substring(i).trim();
    if (octaveText.isEmpty() || !octaveText.containsOnly("-0123456789"))
        return std::nullopt;
    return (octaveText.getIntValue() + 1) * 12 + pitchClass;
}

/** "+2", "-1" -> 2, -1; nullopt without a sign (an absolute value). */
std::optional<int> parseRelative(const juce::String &text)
{
    const auto t = text.trim();
    if (t.isEmpty() || (t[0] != '+' && t[0] != '-'))
        return std::nullopt;
    const auto digits = t.substring(1).trim();
    if (digits.isEmpty() || !digits.containsOnly("0123456789"))
        return std::nullopt;
    return t[0] == '+' ? digits.getIntValue() : -digits.getIntValue();
}
} // namespace

PolyphonicSelectionBar::PolyphonicSelectionBar()
{
    for (auto *box : {&noteBox, &centBox, &levelBox})
        setupBox(*box);

    noteBox.setRange(0.0, 127.0, 1.0);
    noteBox.textFromValueFunction = [this](double value) {
        return !state.hasSelection || (state.notesDiffer && !noteTouched)
                   ? kDiffer
                   : noteName(static_cast<int>(std::lround(value)));
    };
    noteBox.valueFromTextFunction = [this](const juce::String &text) {
        if (const auto relative = parseRelative(text))
            return static_cast<double>(nearestNote() + *relative);
        if (const auto midi = parseNoteName(text))
            return static_cast<double>(*midi);
        return noteBox.getValue(); // not understood: no change
    };

    // Wide enough that a drag can move a note across a semitone in cents.
    centBox.setRange(-99.0, 99.0, 1.0);
    centBox.textFromValueFunction = [this](double value) {
        if (!state.hasSelection || (state.centsDiffer && !centTouched))
            return kDiffer;
        const int cents = static_cast<int>(std::lround(value));
        return (cents > 0 ? "+" : "") + juce::String(cents) + " ct";
    };
    centBox.valueFromTextFunction = [this](const juce::String &text) {
        const auto t = text.trim().removeCharacters("ctCT ");
        if (t.isEmpty() || !t.containsOnly("+-0123456789"))
            return centBox.getValue();
        return static_cast<double>(t.getIntValue());
    };

    levelBox.setRange(-24.0, 12.0, 0.1);
    levelBox.setDoubleClickReturnValue(true, 0.0);
    levelBox.textFromValueFunction = [this](double value) {
        if (!state.levelDb || (std::isnan(*state.levelDb) && !levelTouched))
            return kDiffer;
        // One decimal, signed like the cents: "+3.0 dB", "0.0 dB", "-6.5 dB".
        const double rounded = std::round(value * 10.0) / 10.0;
        return (rounded > 0.0 ? "+" : "") + juce::String(rounded, 1) + " dB";
    };
    levelBox.valueFromTextFunction = [this](const juce::String &text) {
        const auto t = text.trim().removeCharacters("dBdb ");
        if (t.isEmpty() || !t.containsOnly("+-.0123456789"))
            return levelBox.getValue();
        return static_cast<double>(t.getFloatValue());
    };

    noteBox.onDragStart = [this] { noteTouched = true; };
    centBox.onDragStart = [this] { centTouched = true; };
    levelBox.onDragStart = [this] { levelTouched = true; };
    noteBox.onDragEnd = [this] { commitNote(); };
    centBox.onDragEnd = [this] { commitCent(); };
    levelBox.onDragEnd = [this] { commitLevel(); };
    // A typed value (or a double-click reset) arrives without a drag.
    // During a drag the notes follow live (a preview, no render); a typed
    // value (or a double-click reset) arrives without a drag and commits.
    noteBox.onValueChange = [this] {
        if (updating)
            return;
        noteTouched = true;
        if (noteBox.isMouseButtonDown(true))
        {
            if (onPreviewShift && state.hasSelection)
                onPreviewShift(static_cast<float>(std::lround(noteBox.getValue()) - nearestNote()));
        }
        else
            commitNote();
    };
    centBox.onValueChange = [this] {
        if (updating)
            return;
        centTouched = true;
        if (centBox.isMouseButtonDown(true))
        {
            if (onPreviewShift && state.hasSelection)
                onPreviewShift(static_cast<float>(std::lround(centBox.getValue()) - centsFromNearest()) / 100.0f);
        }
        else
            commitCent();
    };
    levelBox.onValueChange = [this] {
        if (updating)
            return;
        levelTouched = true;
        if (levelBox.isMouseButtonDown(true))
        {
            if (onPreviewLevel && state.levelDb.has_value())
                onPreviewLevel(static_cast<float>(levelBox.getValue()));
        }
        else
            commitLevel();
    };

    refreshLocalisedText();
    setState({});
}

PolyphonicSelectionBar::~PolyphonicSelectionBar() = default;

void PolyphonicSelectionBar::setupBox(SliderBox &box)
{
    box.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 60, kBoxHeight - 2);
    box.setScrollWheelEnabled(false);
    box.setColour(juce::Slider::backgroundColourId, juce::Colour(0xFF30302Eu));
    box.setColour(juce::Slider::trackColourId, juce::Colour(0xFF30302Eu));
    box.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    box.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    box.setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xFFE6E6E6u));
    addAndMakeVisible(box);
}

void PolyphonicSelectionBar::refreshLocalisedText()
{
    noteBox.setTooltip(TR("tooltip.selection_note"));
    centBox.setTooltip(TR("tooltip.selection_cent"));
    levelBox.setTooltip(TR("tooltip.selection_level"));
    resized();
}

int PolyphonicSelectionBar::nearestNote() const
{
    const float offset = ScaleUtils::getReferenceOffsetSemitones(state.referenceHz);
    const int totalCents = static_cast<int>(std::lround((state.midi - offset) * 100.0f));
    return static_cast<int>(std::floor((totalCents + 50) / 100.0));
}

int PolyphonicSelectionBar::centsFromNearest() const
{
    const float offset = ScaleUtils::getReferenceOffsetSemitones(state.referenceHz);
    const int totalCents = static_cast<int>(std::lround((state.midi - offset) * 100.0f));
    return totalCents - nearestNote() * 100;
}

void PolyphonicSelectionBar::setState(const State &next)
{
    state = next;
    noteTouched = centTouched = levelTouched = false;
    updating = true;
    for (auto *box : {&noteBox, &centBox})
        box->setEnabled(state.hasSelection);
    levelBox.setEnabled(state.levelDb.has_value());
    noteBox.setValue(state.hasSelection ? nearestNote() : 60, juce::dontSendNotification);
    centBox.setValue(state.hasSelection ? centsFromNearest() : 0, juce::dontSendNotification);
    const float level = state.levelDb && std::isfinite(*state.levelDb) ? *state.levelDb : 0.0f;
    levelBox.setValue(level, juce::dontSendNotification);
    // The text depends on the flags as well as the value.
    for (auto *box : {&noteBox, &centBox, &levelBox})
        box->updateText();
    updating = false;
}

void PolyphonicSelectionBar::commitNote()
{
    if (onPreviewEnd)
        onPreviewEnd();
    if (updating || !state.hasSelection)
        return;
    const int delta = static_cast<int>(std::lround(noteBox.getValue())) - nearestNote();
    if (delta != 0 && onShiftSemitones)
        onShiftSemitones(static_cast<float>(delta), false);
    else
        setState(state); // put the text back ("-" for differing notes)
}

void PolyphonicSelectionBar::commitCent()
{
    if (onPreviewEnd)
        onPreviewEnd();
    if (updating || !state.hasSelection)
        return;
    const int delta = static_cast<int>(std::lround(centBox.getValue())) - centsFromNearest();
    if (delta != 0 && onShiftSemitones)
        onShiftSemitones(static_cast<float>(delta) / 100.0f, true);
    else
        setState(state);
}

void PolyphonicSelectionBar::commitLevel()
{
    if (onPreviewEnd)
        onPreviewEnd();
    if (updating || !state.levelDb.has_value())
        return;
    const float value = static_cast<float>(levelBox.getValue());
    const bool same = std::isfinite(*state.levelDb) && std::abs(*state.levelDb - value) < 0.05f;
    if (!same && onLevelChanged)
        onLevelChanged(value);
    else
        setState(state);
}

int PolyphonicSelectionBar::getPreferredWidth() const
{
    return kPad * 2 + kGap * 2 + kBoxWidth * 3;
}

void PolyphonicSelectionBar::resized()
{
    auto area = getLocalBounds().reduced(kPad, 0);
    const int boxY = (getHeight() - kBoxHeight) / 2;
    for (auto *box : {&noteBox, &centBox, &levelBox})
    {
        box->setBounds(area.removeFromLeft(kBoxWidth).withY(boxY).withHeight(kBoxHeight));
        area.removeFromLeft(kGap);
    }
}
