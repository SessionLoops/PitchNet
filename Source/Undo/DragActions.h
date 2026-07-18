#pragma once

#include "UndoableAction.h"
#include "F0FrameEdit.h"
#include "../Models/Note.h"
#include <vector>
#include <functional>

/**
 * Action for dragging a note to change pitch (MIDI note + F0 values).
 */
class NotePitchDragAction : public UndoableAction
{
public:
    NotePitchDragAction(Note *note, std::vector<float> *f0Array,
                        float oldMidi, float newMidi,
                        std::vector<F0FrameEdit> f0Edits,
                        std::function<void(Note *)> onNoteChanged = nullptr)
        : note(note), f0Array(f0Array), oldMidi(oldMidi), newMidi(newMidi),
          f0Edits(std::move(f0Edits)), onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (note)
        {
            note->setMidiNote(oldMidi);
            note->markDirty();
            note->markSynthDirty();
        }
        if (f0Array)
        {
            for (const auto &e : f0Edits)
            {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.oldF0;
            }
        }
        if (onNoteChanged && note)
        {
            onNoteChanged(note);
        }
    }

    void redo() override
    {
        if (note)
        {
            note->setMidiNote(newMidi);
            note->markDirty();
            note->markSynthDirty();
        }
        if (f0Array)
        {
            for (const auto &e : f0Edits)
            {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.newF0;
            }
        }
        if (onNoteChanged && note)
        {
            onNoteChanged(note);
        }
    }

    juce::String getName() const override { return "Drag Note Pitch"; }

private:
    Note *note;
    std::vector<float> *f0Array;
    float oldMidi;
    float newMidi;
    std::vector<F0FrameEdit> f0Edits;
    std::function<void(Note *)> onNoteChanged;
};

/**
 * Action for dragging multiple notes to change pitch.
 */
class MultiNotePitchDragAction : public UndoableAction
{
public:
    MultiNotePitchDragAction(std::vector<Note *> notes, std::vector<float> *f0Array,
                             std::vector<float> oldMidis, float pitchDelta,
                             std::vector<F0FrameEdit> f0Edits,
                             std::function<void(const std::vector<Note *> &)> onNotesChanged = nullptr)
        : notes(std::move(notes)), f0Array(f0Array), oldMidis(std::move(oldMidis)),
          pitchDelta(pitchDelta), f0Edits(std::move(f0Edits)), onNotesChanged(onNotesChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldMidis.size(); ++i)
        {
            if (notes[i])
            {
                notes[i]->setMidiNote(oldMidis[i]);
                notes[i]->markDirty();
                notes[i]->markSynthDirty();
            }
        }
        if (f0Array)
        {
            for (const auto &e : f0Edits)
            {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.oldF0;
            }
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldMidis.size(); ++i)
        {
            if (notes[i])
            {
                notes[i]->setMidiNote(oldMidis[i] + pitchDelta);
                notes[i]->markDirty();
                notes[i]->markSynthDirty();
            }
        }
        if (f0Array)
        {
            for (const auto &e : f0Edits)
            {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.newF0;
            }
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Drag Multiple Notes"; }

private:
    std::vector<Note *> notes;
    std::vector<float> *f0Array;
    std::vector<float> oldMidis;
    float pitchDelta;
    std::vector<F0FrameEdit> f0Edits;
    std::function<void(const std::vector<Note *> &)> onNotesChanged;
};

/**
 * Action for nudging selected notes by keyboard (up/down, octave).
 */
class MultiNoteMidiNudgeAction : public UndoableAction
{
public:
    MultiNoteMidiNudgeAction(std::vector<Note *> notes,
                             std::vector<float> oldMidis,
                             std::vector<float> newMidis,
                             std::function<void(const std::vector<Note *> &)> onNotesChanged = nullptr)
        : notes(std::move(notes)),
          oldMidis(std::move(oldMidis)),
          newMidis(std::move(newMidis)),
          onNotesChanged(std::move(onNotesChanged)) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldMidis.size(); ++i)
        {
            if (!notes[i])
                continue;
            notes[i]->setMidiNote(oldMidis[i]);
            notes[i]->markDirty();
            notes[i]->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size() && i < newMidis.size(); ++i)
        {
            if (!notes[i])
                continue;
            notes[i]->setMidiNote(newMidis[i]);
            notes[i]->markDirty();
            notes[i]->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Nudge Note Pitch"; }

private:
    std::vector<Note *> notes;
    std::vector<float> oldMidis;
    std::vector<float> newMidis;
    std::function<void(const std::vector<Note *> &)> onNotesChanged;
};

/**
 * Undoable pitch-center correction.  Besides the affected note centres, this
 * preserves the per-project Pitch Center setting shown by the dialog.
 */
class PitchCenterCorrectionAction : public UndoableAction
{
public:
    using ChangeCallback = std::function<void(float, const std::vector<Note *> &)>;

    PitchCenterCorrectionAction(std::vector<Note *> notes,
                                std::vector<float> oldMidis,
                                std::vector<float> newMidis,
                                float oldPitchCenter,
                                float newPitchCenter,
                                ChangeCallback onChanged = nullptr)
        : notes(std::move(notes)), oldMidis(std::move(oldMidis)),
          newMidis(std::move(newMidis)), oldPitchCenter(oldPitchCenter),
          newPitchCenter(newPitchCenter), onChanged(std::move(onChanged)) {}

    void undo() override { apply(oldMidis, oldPitchCenter); }
    void redo() override { apply(newMidis, newPitchCenter); }
    juce::String getName() const override { return "Correct Pitch Center"; }

private:
    void apply(const std::vector<float> &midis, float pitchCenter)
    {
        for (size_t i = 0; i < notes.size() && i < midis.size(); ++i)
        {
            if (!notes[i]) continue;
            notes[i]->setMidiNote(midis[i]);
            notes[i]->markDirty();
            notes[i]->markSynthDirty();
        }
        if (onChanged) onChanged(pitchCenter, notes);
    }

    std::vector<Note *> notes;
    std::vector<float> oldMidis, newMidis;
    float oldPitchCenter = 0.0f, newPitchCenter = 0.0f;
    ChangeCallback onChanged;
};
