#pragma once

#include "../JuceHeader.h"
#include "Note.h"
#include <vector>
#include <memory>
#include <utility>

/**
 * Container for audio data and extracted features.
 */
struct AudioData
{
    struct SegmentDebugEvent
    {
        int startFrame = 0;
        int endFrame = 0;
        int attachedStartFrame = 0;
        float midiNote = 0.0f;
        bool isRest = false;
        float durationSeconds = 0.0f;
        int durationFrames = 0;
    };

    struct SegmentDebugChunk
    {
        int chunkIndex = 0;
        int startFrame = 0;
        int endFrame = 0;
        int shortRestThreshold = 0;
        std::vector<SegmentDebugEvent> events;
    };

    juce::AudioBuffer<float> waveform;
    juce::AudioBuffer<float> originalWaveform; // pristine copy for blend (never modified after analysis)

    int sampleRate = 44100;

    // Extracted features
    std::vector<std::vector<float>> melSpectrogram;      // [T, NUM_MELS]
    std::vector<float> f0;                               // [T] (composed: base + delta, dense)
    std::vector<float> baseF0;                           // [T] (cached base pitch in Hz)
    std::vector<float> basePitch;                        // [T] base pitch in MIDI (dense)
    std::vector<float> deltaPitch;                       // [T] delta pitch in MIDI (dense)
    std::vector<bool> voicedMask;                        // [T] uv mask (true = voiced, F0-based)
    std::vector<bool> vadMask;                           // [T] energy-based VAD (true = has audio energy, captures consonants)
    std::vector<std::pair<int, int>> segmentChunkRanges; // [N] GAME slicer chunks in frame range [start, end)
    std::vector<SegmentDebugChunk> segmentDebugChunks;   // raw GAME outputs for debug visualization

    float getDuration() const
    {
        if (waveform.getNumSamples() == 0)
            return 0.0f;
        return static_cast<float>(waveform.getNumSamples()) / sampleRate;
    }

    int getNumFrames() const
    {
        return static_cast<int>(melSpectrogram.size());
    }
};

/**
 * Loop playback range in seconds.
 */
struct LoopRange
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    bool enabled = false;

    bool isValid() const { return enabled && endSeconds > startSeconds; }
};

/**
 * Scale mode used for piano-roll grid coloring.
 */
enum class ScaleMode : int
{
    None = -1,
    Chromatic = 0,
    Major,
    Minor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian
};

/**
 * How double-click snap resolves target pitch.
 */
enum class DoubleClickSnapMode : int
{
    PitchCenter = 0, // Active scale when available, otherwise semitone
    NearestSemitone, // Always nearest semitone
    NearestScale     // Only nearest note in active scale
};

/**
 * Timeline ruler mode.
 */
enum class TimelineDisplayMode : int
{
    Beats = 0,
    Time
};

/**
 * Beat-grid subdivision expressed as note denominator (1/x).
 */
enum class TimelineGridDivision : int
{
    Whole = 1,
    Half = 2,
    Quarter = 4,
    Eighth = 8,
    Sixteenth = 16,
    ThirtySecond = 32
};

/**
 * Project data container.
 */
class Project
{
public:
    Project();
    ~Project() = default;

    // File operations
    void setFilePath(const juce::File &file) { filePath = file; }
    juce::File getFilePath() const { return filePath; }
    void setProjectFilePath(const juce::File &file) { projectFilePath = file; }
    juce::File getProjectFilePath() const { return projectFilePath; }
    void setAudioSha256(const juce::String &sha) { audioSha256 = sha; }
    juce::String getAudioSha256() const { return audioSha256; }
    juce::String getName() const { return name; }
    void setName(const juce::String &n) { name = n; }

    // Audio data
    AudioData &getAudioData() { return audioData; }
    const AudioData &getAudioData() const { return audioData; }

    // Notes
    std::vector<Note> &getNotes() { return notes; }
    const std::vector<Note> &getNotes() const { return notes; }
    void addNote(Note note) { notes.push_back(std::move(note)); }
    void clearNotes() { notes.clear(); }

    Note *getNoteAtFrame(int frame);
    std::vector<Note *> getNotesInRange(int startFrame, int endFrame);
    std::vector<Note *> getSelectedNotes();
    bool removeNoteByStartFrame(int startFrame);
    std::vector<Note *> getDirtyNotes();
    void selectAllNotes(bool includeRests = false);
    void deselectAllNotes();
    void clearAllDirty();

    // Global settings
    float getGlobalPitchOffset() const { return globalPitchOffset; }
    void setGlobalPitchOffset(float offset) { globalPitchOffset = offset; }

    float getFormantShift() const { return formantShift; }
    void setFormantShift(float shift) { formantShift = shift; }

    float getVolume() const { return volume; }
    void setVolume(float vol) { volume = vol; }

    // Get adjusted F0 with all modifications applied
    std::vector<float> getAdjustedF0() const;

    // Get adjusted F0 for a specific frame range
    std::vector<float> getAdjustedF0ForRange(int startFrame, int endFrame) const;

    // Get frame range that needs resynthesis (based on dirty notes)
    // Returns {-1, -1} if no dirty notes
    std::pair<int, int> getDirtyFrameRange() const;

    // Check if any notes are dirty
    bool hasDirtyNotes() const;

    // Compose the global waveform from originalWaveform + per-note synthWaveforms.
    // Fills audioData.waveform with originalWaveform as base, then overlays each
    // note's synthWaveform at its output position with edge crossfades.
    void composeGlobalWaveform();

    // F0 direct edit dirty tracking (for Draw mode)
    void setF0DirtyRange(int startFrame, int endFrame);
    void clearF0DirtyRange();
    bool hasF0DirtyRange() const;
    std::pair<int, int> getF0DirtyRange() const;

    // Modified state
    bool isModified() const { return modified; }
    void setModified(bool mod) { modified = mod; }

    // Loop range
    const LoopRange &getLoopRange() const { return loopRange; }
    void setLoopRange(double startSeconds, double endSeconds);
    void setLoopEnabled(bool enabled);
    void clearLoopRange();

    // Piano-roll scale visualization
    ScaleMode getScaleMode() const { return scaleMode; }
    void setScaleMode(ScaleMode mode);
    int getScaleRootNote() const { return scaleRootNote; }
    void setScaleRootNote(int noteInOctave);
    int getPitchReferenceHz() const { return pitchReferenceHz; }
    void setPitchReferenceHz(int hz);
    bool getShowScaleColors() const { return showScaleColors; }
    void setShowScaleColors(bool enabled);
    bool getSnapToSemitones() const { return snapToSemitones; }
    void setSnapToSemitones(bool enabled);
    DoubleClickSnapMode getDoubleClickSnapMode() const { return doubleClickSnapMode; }
    void setDoubleClickSnapMode(DoubleClickSnapMode mode);

    // Timeline/grid settings
    TimelineDisplayMode getTimelineDisplayMode() const { return timelineDisplayMode; }
    void setTimelineDisplayMode(TimelineDisplayMode mode);
    int getTimelineBeatNumerator() const { return timelineBeatNumerator; }
    int getTimelineBeatDenominator() const { return timelineBeatDenominator; }
    void setTimelineBeatSignature(int numerator, int denominator);
    double getTimelineTempoBpm() const { return timelineTempoBpm; }
    void setTimelineTempoBpm(double bpm);
    TimelineGridDivision getTimelineGridDivision() const { return timelineGridDivision; }
    void setTimelineGridDivision(TimelineGridDivision division);
    bool getTimelineSnapCycle() const { return timelineSnapCycle; }
    void setTimelineSnapCycle(bool enabled);

private:
    juce::String name = "Untitled";
    juce::File filePath;
    juce::File projectFilePath;
    juce::String audioSha256;

    AudioData audioData;
    std::vector<Note> notes;

    float globalPitchOffset = 0.0f;
    float formantShift = 0.0f;
    float volume = 0.0f; // dB

    // F0 direct edit dirty range
    int f0DirtyStart = -1;
    int f0DirtyEnd = -1;

    bool modified = false;

    LoopRange loopRange;
    ScaleMode scaleMode = ScaleMode::None;
    int scaleRootNote = -1; // -1 = none, 0 = C, 1 = C#, ..., 11 = B
    int pitchReferenceHz = 440;
    bool showScaleColors = true;
    bool snapToSemitones = false;
    DoubleClickSnapMode doubleClickSnapMode = DoubleClickSnapMode::PitchCenter;

    TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Beats;
    int timelineBeatNumerator = 4;
    int timelineBeatDenominator = 4;
    double timelineTempoBpm = 120.0;
    TimelineGridDivision timelineGridDivision = TimelineGridDivision::Quarter;
    bool timelineSnapCycle = false;
};
