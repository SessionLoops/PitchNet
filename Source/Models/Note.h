#pragma once

#include "../JuceHeader.h"
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

    // Pitch
    float getMidiNote() const { return midiNote; }
    void setMidiNote(float note) { midiNote = note; }
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

    // Pitch tool transformation parameters (non-destructive)
    float getTiltLeft() const { return tiltLeft; }
    void setTiltLeft(float tilt) { tiltLeft = tilt; }
    float getTiltRight() const { return tiltRight; }
    void setTiltRight(float tilt) { tiltRight = tilt; }
    float getVarianceScale() const { return varianceScale; }
    void setVarianceScale(float scale) { varianceScale = scale; }
    int getSmoothLeftFrames() const { return smoothLeftFrames; }
    void setSmoothLeftFrames(int frames) { smoothLeftFrames = frames; }
    int getSmoothRightFrames() const { return smoothRightFrames; }
    void setSmoothRightFrames(int frames) { smoothRightFrames = frames; }

    // Delta scale/offset (post-transformation, from delta control handles)
    float getDeltaScale() const { return deltaScale; }
    void setDeltaScale(float scale) { deltaScale = scale; }
    float getDeltaOffset() const { return deltaOffset; }
    void setDeltaOffset(float offset) { deltaOffset = offset; }

    // Vibrato
    bool isVibratoEnabled() const { return vibratoEnabled; }
    void setVibratoEnabled(bool enabled) { vibratoEnabled = enabled; }
    float getVibratoRateHz() const { return vibratoRateHz; }
    void setVibratoRateHz(float hz) { vibratoRateHz = hz; }
    float getVibratoDepthSemitones() const { return vibratoDepthSemitones; }
    void setVibratoDepthSemitones(float semitones) { vibratoDepthSemitones = semitones; }
    float getVibratoPhaseRadians() const { return vibratoPhaseRadians; }
    void setVibratoPhaseRadians(float radians) { vibratoPhaseRadians = radians; }

    // F0 values (original detected values)
    const std::vector<float>& getF0Values() const { return f0Values; }
    void setF0Values(std::vector<float> values) { f0Values = std::move(values); }
    std::vector<float> getAdjustedF0() const;

    // Get F0 values based on current midiNote + deltaPitch
    std::vector<float> computeF0FromDelta() const;

    // Waveform clip (original samples for this note)
    const std::vector<float>& getClipWaveform() const { return clipWaveform; }
    void setClipWaveform(std::vector<float> samples) { clipWaveform = std::move(samples); }
    bool hasClipWaveform() const { return !clipWaveform.empty(); }

    // Source clip waveform (immutable original audio from originalWaveform, set once during segmentation)
    const std::vector<float>& getSrcClipWaveform() const { return srcClipWaveform; }
    void setSrcClipWaveform(std::vector<float> samples) { srcClipWaveform = std::move(samples); }
    bool hasSrcClipWaveform() const { return !srcClipWaveform.empty(); }

    // Synthesized waveform (vocoder output for this note, regenerated when synthDirty)
    // When synthPreroll > 0, the waveform contains extra leading samples before
    // the note's startFrame*HOP_SIZE, enabling real-audio crossfade at boundaries.
    const std::vector<float>& getSynthWaveform() const { return synthWaveform; }
    void setSynthWaveform(std::vector<float> samples) { synthWaveform = std::move(samples); synthPreroll = 0; synthDirty = false; }
    void setSynthWaveform(std::vector<float> samples, int preroll) { synthWaveform = std::move(samples); synthPreroll = preroll; synthDirty = false; }
    bool hasSynthWaveform() const { return !synthWaveform.empty(); }
    void clearSynthWaveform() { synthWaveform.clear(); synthPreroll = 0; synthDirty = true; }

    // Synth preroll: number of margin samples prepended before noteStart in synthWaveform.
    // synthWaveform[0..synthPreroll) covers audio BEFORE noteStart*HOP_SIZE.
    // synthWaveform[synthPreroll..synthPreroll+noteSamples) is the note body.
    // synthWaveform[synthPreroll+noteSamples..) is the postroll after noteEnd.
    int getSynthPreroll() const { return synthPreroll; }
    void setSynthPreroll(int preroll) { synthPreroll = preroll; }

    // Synth dirty flag (needs re-synthesis; separate from display dirty flag)
    bool isSynthDirty() const { return synthDirty; }
    void setSynthDirty(bool d) { synthDirty = d; }
    void markSynthDirty() { synthDirty = true; synthWaveform.clear(); synthPreroll = 0; }

    // Mel spectrogram clip (original mel frames for this note)
    const std::vector<std::vector<float>>& getClipMel() const { return clipMel; }
    void setClipMel(std::vector<std::vector<float>> mel) { clipMel = std::move(mel); }
    bool hasClipMel() const { return !clipMel.empty(); }

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

private:
    // Source position (in original waveform, fixed after detection)
    int srcStartFrame = 0;
    int srcEndFrame = 0;

    // Destination position in the output timeline
    int startFrame = 0;
    int endFrame = 0;

    float midiNote = 60.0f;
    float pitchOffset = 0.0f;
    float volumeDb = 0.0f; // Per-note gain in dB (0 = unity)

    std::vector<float> deltaPitch;  // Per-frame deviation from midiNote in semitones
    std::vector<float> originalDeltaPitch;  // Pristine curve from analysis (never modified)

    // Pitch tool transformation parameters (non-destructive, stored as parameters)
    float tiltLeft = 0.0f;           // Tilt amount at left edge (semitones)
    float tiltRight = 0.0f;          // Tilt amount at right edge (semitones)
    float varianceScale = 1.0f;      // Variance scaling factor (1.0=unchanged, 0.0=flat, >1.0=amplify, <0.0=invert)
    int smoothLeftFrames = 0;        // Smoothing transition length at left boundary
    int smoothRightFrames = 0;       // Smoothing transition length at right boundary

    // Post-transformation scale/offset from delta control handles
    float deltaScale = 1.0f;        // Applied after all other transformations (1.0=unchanged)
    float deltaOffset = 0.0f;       // Added after scale (0.0=unchanged)

    bool vibratoEnabled = false;
    float vibratoRateHz = 5.0f;
    float vibratoDepthSemitones = 0.0f;
    float vibratoPhaseRadians = 0.0f;

    std::vector<float> f0Values;
    std::vector<float> clipWaveform;
    std::vector<float> srcClipWaveform;  // Immutable original audio (from originalWaveform)
    std::vector<float> synthWaveform;    // Vocoder output (regenerated when synthDirty)
    int synthPreroll = 0;                // Margin samples prepended before noteStart in synthWaveform
    std::vector<std::vector<float>> clipMel;  // Mel spectrogram clip [T, numMels]

    bool selected = false;
    bool dirty = false;       // For incremental synthesis (display/trigger)
    bool synthDirty = true;   // Needs re-synthesis (separate from display dirty)
    bool rest = false;        // Rest note (silence placeholder)

    juce::String lyric;   // Lyric text (e.g., "a", "SP" for silence)
    juce::String phoneme; // Phoneme (e.g., "a", "sp", for pronunciation)
};
