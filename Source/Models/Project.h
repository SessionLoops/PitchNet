#pragma once

#include "../JuceHeader.h"
#include "../Audio/IO/MidiReference.h"
#include "AnalysisMode.h"
#include "PolyphonicTake.h"
#include "Note.h"
#include <cstdint>
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
    double timelineOffsetSeconds = 0.0;
    // Absolute host-timeline ranges occupied by ARA playback regions.
    // Empty for standalone/non-ARA projects.
    std::vector<std::pair<double, double>> playbackRegionRanges;

    // Extracted features
    std::vector<std::vector<float>> melSpectrogram;      // [T, NUM_MELS]
    // Pitch analysis stages, all aligned to the vocoder/mel frame grid.
    std::vector<float> rawF0;                            // [T] detector observations (0 = unvoiced)
    std::vector<float> cleanedF0;                        // [T] rawF0 after isolated-jump repair
    std::vector<float> denseF0;                          // [T] cleanedF0 with log-domain UV interpolation
    std::vector<float> f0;                               // [T] current composed/edited dense F0
    std::vector<float> baseF0;                           // [T] (cached base pitch in Hz)
    std::vector<float> basePitch;                        // [T] base pitch in MIDI (dense)
    std::vector<float> deltaPitch;                       // [T] delta pitch in MIDI (dense)
    std::vector<bool> voicedMask;                        // [T] uv mask (true = voiced, F0-based)
    std::vector<bool> vadMask;                           // [T] energy-based VAD (true = has audio energy, captures consonants)
    // [T] breathsibilance::Label per frame (0 none, 1 sibilance, 2 breath).
    // Derived from originalWaveform + voicedMask, so it is not saved with a
    // project; breathsibilance::ensure() rebuilds it after a load.
    std::vector<std::uint8_t> breathSibilance;
    // [T or shorter] gain of the breath notes in dB per source frame (0, or
    // past the end, = unchanged). A user edit, so it is saved with a project
    // (as runs); see breathnotes (BreathNotes.h).
    std::vector<float> breathGainDb;
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
    Blues,
    Dorian,
    HarmonicMinor,
    Locrian,
    Lydian,
    MajorPentatonic,
    MelodicMinor,
    MinorPentatonic,
    Mixolydian,
    Phrygian,
    PhrygianDominant,
    WholeTone
};

enum class DragSnapMode : int
{
    Chromatic = 0,
    Scale
};

enum class DoubleClickSnapMode : int
{
    PitchCenter = 0,
    Scale,
    MidiReference
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

struct MacroParameters
{
    ScaleMode scaleMode = ScaleMode::Chromatic;
    int projectKeyRoot = 0;
    ScaleMode projectKeyMode = ScaleMode::Major;
    bool projectKeyInitialized = false;
    int pitchReferenceHz = 440;
    bool snapToSemitones = false;
    // Drag snap for polyphonic takes, on by default (feature review #20): a
    // plugin instance shares these parameters across its projects and a track
    // can mix vocal and polyphonic takes, so each mode keeps its own on/off.
    bool snapToSemitonesPolyphonic = true;
    DragSnapMode dragSnapMode = DragSnapMode::Chromatic;
    DoubleClickSnapMode doubleClickSnapMode = DoubleClickSnapMode::PitchCenter;
    MidiReference::State midiReference;
    TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Beats;
    int timelineBeatNumerator = 4;
    int timelineBeatDenominator = 4;
    double timelineTempoBpm = 120.0;
    TimelineGridDivision timelineGridDivision = TimelineGridDivision::Quarter;
    // Time-card Drag Snap, kept per mode like snapToSemitones above: off for
    // vocal takes, on by default for polyphonic ones.
    bool timelineDragSnap = false;
    bool timelineDragSnapPolyphonic = true;
    // Where bar 1 starts, in timeline seconds (the beat grid is shifted by
    // it). Set by tempo detection in the standalone app; 0 otherwise.
    double timelineBeatOriginSeconds = 0.0;
};

/**
 * Project data container.
 */
class Project
{
public:
    Project();
    ~Project() = default;

    // Presentation state saved with the project; zero size uses app defaults.
    int uiWidth = 0;
    int uiHeight = 0;
    bool thumbnailVisible = false;

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
    void addNote(Note note);
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

    // Per-project pitch-center correction strength (0-100%).  ARA regions
    // carry their own Project, so this remains independent per region.
    float getPitchCenter() const { return pitchCenter; }
    void setPitchCenter(float amount)
    {
        const float normalized = juce::jlimit(0.0f, 100.0f, amount);
        if (std::abs(pitchCenter - normalized) > 0.0001f)
        {
            pitchCenter = normalized;
            modified = true;
        }
    }

    // Vocal pipeline or polyphonic engine (AnalysisMode.h). Until M2 every
    // take is still analysed as vocal; the state records the classification
    // and the mode that will apply.
    const AnalysisModeState &getAnalysisMode() const { return analysisMode; }
    void setAnalysisMode(const AnalysisModeState &state)
    {
        if (state != analysisMode)
        {
            analysisMode = state;
            modified = true;
        }
    }

    // The polyphonic engine's take (PolyphonicTake.h); null for vocal takes.
    // Shared between copies of the Project: replace, never mutate in place.
    std::shared_ptr<const PolyphonicTake> getPolyphonicTake() const { return polyphonicTake; }
    void setPolyphonicTake(std::shared_ptr<const PolyphonicTake> take) { polyphonicTake = std::move(take); }
    bool isPolyphonic() const { return polyphonicTake != nullptr && polyphonicTake->hasAnalysis(); }

    // The user's edits of a polyphonic take, keyed by index into its
    // analysis->notes. Only non-identity edits are kept (setPolyphonicEdits
    // drops the rest), so an empty map means the take is untouched.
    using PolyphonicEdits = std::map<int, polyphonic::NoteEdit>;
    const PolyphonicEdits &getPolyphonicEdits() const { return polyphonicEdits; }
    void setPolyphonicEdits(PolyphonicEdits edits)
    {
        for (auto it = edits.begin(); it != edits.end();)
            it = it->second.isIdentity() ? edits.erase(it) : std::next(it);
        if (edits != polyphonicEdits)
        {
            polyphonicEdits = std::move(edits);
            modified = true;
        }
    }

    // A saved polyphonic take waiting to be rebuilt (set when a project is
    // read, cleared once its analysis is restored); null otherwise.
    std::shared_ptr<const PolyphonicRestore> getPendingPolyphonicRestore() const { return pendingPolyphonicRestore; }
    void setPendingPolyphonicRestore(std::shared_ptr<const PolyphonicRestore> restore) { pendingPolyphonicRestore = std::move(restore); }

    // The latest render of the polyphonic take with its edits, in the take's
    // channels at the project rate (EditorController::renderPolyphonicAsync);
    // null until the first render. Playback only: not saved.
    std::shared_ptr<const juce::AudioBuffer<float>> getPolyphonicPlayback() const { return polyphonicPlayback; }
    void setPolyphonicPlayback(std::shared_ptr<const juce::AudioBuffer<float>> buffer) { polyphonicPlayback = std::move(buffer); }

    // What the audio engine plays, at getAudioData().sampleRate: for a
    // polyphonic take its latest render, or its own channels before the first
    // one (stereo stays stereo); otherwise the (mono) waveform.
    const juce::AudioBuffer<float> &getPlaybackBuffer() const
    {
        if (isPolyphonic())
        {
            if (polyphonicPlayback)
                return *polyphonicPlayback;
            if (polyphonicTake->source.getNumSamples() > 0)
                return polyphonicTake->source;
        }
        return audioData.waveform;
    }

    // Get adjusted F0 with all modifications applied
    std::vector<float> getAdjustedF0() const;

    // Get adjusted F0 for a specific frame range
    std::vector<float> getAdjustedF0ForRange(int startFrame, int endFrame) const;

    // Get frame range that needs resynthesis (based on dirty notes)
    // Returns {-1, -1} if no dirty notes
    std::pair<int, int> getDirtyFrameRange() const;

    // Check if any notes are dirty
    bool hasDirtyNotes() const;

    // F0 curve dirty tracking (for pitch tools and undo/redo)
    void setF0DirtyRange(int startFrame, int endFrame);

    // The one F0 dirty range for note edits that re-render audio (pitch drag,
    // nudge, snap, pitch macro, pitch tools, formant, curve drawing) and for
    // their undo, redo and Restore. Pass the edited notes' own frame span; it
    // is padded by 60 frames each side. This range sets the synthesiser's
    // render window, so an edit, its undo and its Restore must mark it the
    // same way or the re-rendered audio around the note differs and clicks.
    void markNoteEditDirtyRange(int startFrame, int endFrame);
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
    ScaleMode getScaleMode() const {
        const auto mode = macroParameters->scaleMode;
        return mode == ScaleMode::None || mode == ScaleMode::Chromatic ? mode : getPreferredScaleMode();
    }
    void setScaleMode(ScaleMode mode);
    ScaleMode getPreferredScaleMode() const { return useProjectKey ? getProjectKeyMode() : preferredScaleMode; }
    void setPreferredScaleMode(ScaleMode mode);
    int getScaleRootNote() const { return useProjectKey ? getProjectKeyRoot() : scaleRootNote; }
    void setScaleRootNote(int noteInOctave);
    void setDetectedKey(int root, ScaleMode mode) {
        detectedKeyRoot = juce::jlimit(0, 11, root);
        detectedKeyMode = mode;
        modified = true;
    }
    int getDetectedScaleRootNote() const { return detectedKeyRoot; }
    ScaleMode getDetectedScaleMode() const { return detectedKeyMode; }
    int getRegionKeyRoot() const { return scaleRootNote; }
    ScaleMode getRegionKeyMode() const { return preferredScaleMode; }
    bool getUseProjectKey() const { return useProjectKey; }
    void setUseProjectKey(bool inherit);
    int getProjectKeyRoot() const { return macroParameters->projectKeyRoot; }
    ScaleMode getProjectKeyMode() const { return macroParameters->projectKeyMode; }
    void setProjectKey(int root, ScaleMode mode);

    int getPitchReferenceHz() const { return macroParameters->pitchReferenceHz; }
    void setPitchReferenceHz(int hz);
    // Drag snap for this project's kind of take (vocal or polyphonic).
    bool getSnapToSemitones() const { return getSnapToSemitonesFor(isPolyphonic()); }
    void setSnapToSemitones(bool enabled) { setSnapToSemitonesFor(isPolyphonic(), enabled); }
    bool getSnapToSemitonesFor(bool polyphonic) const
    {
        return polyphonic ? macroParameters->snapToSemitonesPolyphonic
                          : macroParameters->snapToSemitones;
    }
    void setSnapToSemitonesFor(bool polyphonic, bool enabled);
    DragSnapMode getDragSnapMode() const { return macroParameters->dragSnapMode; }
    void setDragSnapMode(DragSnapMode mode);
    DoubleClickSnapMode getDoubleClickSnapMode() const { return macroParameters->doubleClickSnapMode; }
    void setDoubleClickSnapMode(DoubleClickSnapMode mode);
    const MidiReference::State& getMidiReference() const { return macroParameters->midiReference; }
    bool setMidiReference(MidiReference::State state);

    // Timeline/grid settings
    TimelineDisplayMode getTimelineDisplayMode() const { return macroParameters->timelineDisplayMode; }
    void setTimelineDisplayMode(TimelineDisplayMode mode);
    int getTimelineBeatNumerator() const { return macroParameters->timelineBeatNumerator; }
    int getTimelineBeatDenominator() const { return macroParameters->timelineBeatDenominator; }
    void setTimelineBeatSignature(int numerator, int denominator);
    double getTimelineTempoBpm() const { return macroParameters->timelineTempoBpm; }
    void setTimelineTempoBpm(double bpm);
    TimelineGridDivision getTimelineGridDivision() const { return macroParameters->timelineGridDivision; }
    void setTimelineGridDivision(TimelineGridDivision division);
    double getTimelineBeatOriginSeconds() const { return macroParameters->timelineBeatOriginSeconds; }
    void setTimelineBeatOriginSeconds(double seconds);
    bool getTimelineDragSnap() const { return getTimelineDragSnapFor(isPolyphonic()); }
    void setTimelineDragSnap(bool enabled) { setTimelineDragSnapFor(isPolyphonic(), enabled); }
    bool getTimelineDragSnapFor(bool polyphonic) const
    {
        return polyphonic ? macroParameters->timelineDragSnapPolyphonic
                          : macroParameters->timelineDragSnap;
    }
    void setTimelineDragSnapFor(bool polyphonic, bool enabled);

    std::shared_ptr<MacroParameters> getMacroParameters() const
    {
        return macroParameters;
    }
    void setMacroParameters(std::shared_ptr<MacroParameters> parameters);

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
    float pitchCenter = 0.0f;
    AnalysisModeState analysisMode;
    std::shared_ptr<const PolyphonicTake> polyphonicTake;
    PolyphonicEdits polyphonicEdits;
    std::shared_ptr<const PolyphonicRestore> pendingPolyphonicRestore;
    std::shared_ptr<const juce::AudioBuffer<float>> polyphonicPlayback;

    // F0 direct edit dirty range
    int f0DirtyStart = -1;
    int f0DirtyEnd = -1;

    bool modified = false;

    LoopRange loopRange;
    bool useProjectKey = true;
    int detectedKeyRoot = 0;
    ScaleMode detectedKeyMode = ScaleMode::Major;
    ScaleMode preferredScaleMode = ScaleMode::Major;
    int scaleRootNote = 0; // 0 = C, 1 = C#, ..., 11 = B
    std::shared_ptr<MacroParameters> macroParameters;
};
