#pragma once

#include "../JuceHeader.h"
#include <cmath>
#include <limits>
#include <vector>

/**
 * Represents a single note/pitch segment.
 *
 * Timing model:
 * - srcStartFrame/srcEndFrame: Position in original waveform (fixed after detection)
 * - startFrame/endFrame: Position in output timeline
 *
 * Pitch model:
 * - midiNote: The base pitch of the note (can be changed by dragging)
 * - deltaPitch: Per-frame deviation from base pitch (preserved during drag)
 * - f0Values: Original F0 values from detection (for reference)
 *
 * When dragging a note up/down:
 * - midiNote changes
 * - deltaPitch stays the same
 * - Actual pitch = midiNote + deltaPitch[frame]
 *
 */
class Note
{
public:
    Note() = default;
    Note(int startFrame, int endFrame, float midiNote);

    // Source frame range (position in original waveform, fixed after detection)
    int getSrcStartFrame() const { return srcStartFrame; }
    int getSrcEndFrame() const { return srcEndFrame; }
    void setSrcStartFrame(int frame) { srcStartFrame = frame; }
    void setSrcEndFrame(int frame) { srcEndFrame = frame; }
    int getSrcDurationFrames() const { return srcEndFrame - srcStartFrame; }

    // Destination frame range (position in output timeline, can be changed)
    int getStartFrame() const { return startFrame; }
    int getEndFrame() const { return endFrame; }
    void setStartFrame(int frame) { startFrame = frame; }
    void setEndFrame(int frame) { endFrame = frame; }
    int getDurationFrames() const { return endFrame - startFrame; }

    // Sub-frame timing preview used while dragging timing boundaries. These
    // values are deliberately transient: only mouse-up commits integer frame
    // positions to the project and undo history.
    float getVisualStartFrame() const
    {
        return std::isfinite(timingPreviewStartFrame)
                   ? timingPreviewStartFrame
                   : static_cast<float>(startFrame);
    }
    float getVisualEndFrame() const
    {
        return std::isfinite(timingPreviewEndFrame)
                   ? timingPreviewEndFrame
                   : static_cast<float>(endFrame);
    }
    float getVisualDurationFrames() const
    {
        return getVisualEndFrame() - getVisualStartFrame();
    }
    void setTimingPreviewStartFrame(float frame)
    {
        timingPreviewStartFrame = frame;
    }
    void setTimingPreviewEndFrame(float frame)
    {
        timingPreviewEndFrame = frame;
    }
    void clearTimingPreview()
    {
        timingPreviewStartFrame = std::numeric_limits<float>::quiet_NaN();
        timingPreviewEndFrame = std::numeric_limits<float>::quiet_NaN();
    }
    bool hasTimingPreview() const
    {
        return std::isfinite(timingPreviewStartFrame) ||
               std::isfinite(timingPreviewEndFrame);
    }

    // Pitch
    float getMidiNote() const { return midiNote; }
    // Regular pitch edits become the source for the next Pitch Center macro
    // pass. The macro uses the dedicated setter below so that repeatedly
    // previewing/applying correction never compounds its own output.
    void setMidiNote(float note)
    {
        midiNote = note;
        lastNonMacroMidiNote = note;
    }
    float getLastNonMacroMidiNote() const { return lastNonMacroMidiNote; }
    void setLastNonMacroMidiNote(float note) { lastNonMacroMidiNote = note; }
    void setMidiNoteFromPitchCorrection(float note) { midiNote = note; }
    float getOriginalMidiNote() const { return originalMidiNote; }
    void setOriginalMidiNote(float note) { originalMidiNote = note; }
    float getPitchOffset() const { return pitchOffset; }
    void setPitchOffset(float offset) { pitchOffset = offset; }
    float getAdjustedMidiNote() const { return midiNote + pitchOffset; }
    float getVolumeDb() const { return volumeDb; }
    void setVolumeDb(float db) { volumeDb = db; }

    // Delta pitch (per-frame deviation from base pitch in semitones)
    const std::vector<float>& getDeltaPitch() const { return deltaPitch; }
    void setDeltaPitch(std::vector<float> delta) { deltaPitch = std::move(delta); }
    bool hasDeltaPitch() const { return !deltaPitch.empty(); }

    // Original delta pitch (pristine curve from analysis, never modified)
    const std::vector<float>& getOriginalDeltaPitch() const { return originalDeltaPitch; }
    void setOriginalDeltaPitch(std::vector<float> delta) { originalDeltaPitch = std::move(delta); }
    bool hasOriginalDeltaPitch() const { return !originalDeltaPitch.empty(); }

    // Optional committed per-frame contour. This is the editable source used
    // by pitch tools after a destructive/baked curve operation, while
    // originalDeltaPitch remains the immutable analysis result.
    const std::vector<float>& getBakedDeltaPitch() const { return bakedDeltaPitch; }
    void setBakedDeltaPitch(std::vector<float> delta) { bakedDeltaPitch = std::move(delta); }
    void clearBakedDeltaPitch() { bakedDeltaPitch.clear(); }
    bool hasBakedDeltaPitch() const { return !bakedDeltaPitch.empty(); }
    const std::vector<float>& getActiveDeltaPitch() const
    {
        if (hasBakedDeltaPitch())
            return bakedDeltaPitch;
        if (hasOriginalDeltaPitch())
            return originalDeltaPitch;
        return deltaPitch;
    }

    // Pitch tool transformation parameters (non-destructive)
    float getTiltLeft() const { return tiltLeft; }
    void setTiltLeft(float tilt) { tiltLeft = tilt; }
    float getTiltRight() const { return tiltRight; }
    void setTiltRight(float tilt) { tiltRight = tilt; }
    // Scales deviations from the note center: 1.0 preserves the recorded
    // contour, 0.0 flattens it, and negative values invert it. The range is
    // limited to -10.0 through 10.0 (-1000% through 1000%).
    float getVibrato() const { return vibrato; }
    void setVibrato(float scale) { vibrato = juce::jlimit(-10.0f, 10.0f, scale); }
    int getSmoothLeftFrames() const { return smoothLeftFrames; }
    void setSmoothLeftFrames(int frames) { smoothLeftFrames = frames; }
    int getSmoothRightFrames() const { return smoothRightFrames; }
    void setSmoothRightFrames(int frames) { smoothRightFrames = frames; }

    // Delta scale/offset (post-transformation, from delta control handles)
    float getDeltaScale() const { return deltaScale; }
    void setDeltaScale(float scale) { deltaScale = scale; }
    float getDeltaOffset() const { return deltaOffset; }
    void setDeltaOffset(float offset) { deltaOffset = offset; }

    // F0 values (original detected values)
    const std::vector<float>& getF0Values() const { return f0Values; }
    void setF0Values(std::vector<float> values) { f0Values = std::move(values); }
    std::vector<float> getAdjustedF0() const;

    // Get F0 values based on current midiNote + deltaPitch
    std::vector<float> computeF0FromDelta() const;

    // True after this note has contributed to the rendered composite waveform.
    // This lightweight state replaces the former per-note audio cache and lets
    // incremental synthesis refresh connected edited-note clusters.
    bool hasRenderedEdit() const { return renderedEdit; }
    void setRenderedEdit(bool rendered) { renderedEdit = rendered; }

    // Synth dirty flag (needs re-synthesis; separate from display dirty flag)
    bool isSynthDirty() const { return synthDirty; }
    void setSynthDirty(bool d) { synthDirty = d; }
    void markSynthDirty() { synthDirty = true; }

    // Selection
    bool isSelected() const { return selected; }
    void setSelected(bool sel) { selected = sel; }

    // Dirty flag (for incremental synthesis)
    bool isDirty() const { return dirty; }
    void setDirty(bool d) { dirty = d; }
    void markDirty() { dirty = true; }
    void clearDirty() { dirty = false; }

    // Rest note (no pitch, just a placeholder for silence)
    bool isRest() const { return rest; }
    void setRest(bool r) { rest = r; }

    // Lyric (character/syllable for this note)
    juce::String getLyric() const { return lyric; }
    void setLyric(const juce::String& text) { lyric = text; }
    bool hasLyric() const { return lyric.isNotEmpty(); }

    // Phoneme (pronunciation for this note)
    juce::String getPhoneme() const { return phoneme; }
    void setPhoneme(const juce::String& ph) { phoneme = ph; }
    bool hasPhoneme() const { return phoneme.isNotEmpty(); }

    // Check if frame is within note
    bool containsFrame(int frame) const;
    bool isNeutralForOriginalWaveform() const;

private:
    // Source position (in original waveform, fixed after detection)
    int srcStartFrame = 0;
    int srcEndFrame = 0;

    // Destination position in the output timeline
    int startFrame = 0;
    int endFrame = 0;
    float timingPreviewStartFrame = std::numeric_limits<float>::quiet_NaN();
    float timingPreviewEndFrame = std::numeric_limits<float>::quiet_NaN();

    float midiNote = 60.0f;
    float lastNonMacroMidiNote = 60.0f;
    float originalMidiNote = 60.0f;
    float pitchOffset = 0.0f;
    float volumeDb = 0.0f; // Per-note gain in dB (0 = unity)

    std::vector<float> deltaPitch;  // Per-frame deviation from midiNote in semitones
    std::vector<float> originalDeltaPitch;  // Pristine curve from analysis (never modified)
    std::vector<float> bakedDeltaPitch; // Optional committed editable contour

    // Pitch tool transformation parameters (non-destructive, stored as parameters)
    float tiltLeft = 0.0f;           // Tilt amount at left edge (semitones)
    float tiltRight = 0.0f;          // Tilt amount at right edge (semitones)
    float vibrato = 1.0f;            // 1.0=original, 0.0=flat, >1.0=amplify, <0.0=invert
    int smoothLeftFrames = 0;        // Smoothing transition length at left boundary
    int smoothRightFrames = 0;       // Smoothing transition length at right boundary

    // Post-transformation scale/offset from delta control handles
    float deltaScale = 1.0f;        // Applied after all other transformations (1.0=unchanged)
    float deltaOffset = 0.0f;       // Added after scale (0.0=unchanged)

    std::vector<float> f0Values;
    bool renderedEdit = false; // Audio for this note is present in the composite
    bool selected = false;
    bool dirty = false;       // For incremental synthesis (display/trigger)
    bool synthDirty = true;   // Needs re-synthesis (separate from display dirty)
    bool rest = false;        // Rest note (silence placeholder)

    juce::String lyric;   // Lyric text (e.g., "a", "SP" for silence)
    juce::String phoneme; // Phoneme (e.g., "a", "sp", for pronunciation)
};
