#pragma once

#include "../Polyphonic/PolyEngine.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace polyphonic
{
class BasicPitchEvidence;
}

/**
 * Chooses the analysis pipeline for a track or audio source: the vocal pipeline
 * (SwiftF0/FCPE + GAME + PC-NSF-HiFiGAN) for a single voice, the polyphonic
 * engine for everything else - chords, stacked or doubled vocals, choirs and
 * monophonic instruments, whose edits the voice-only vocoder cannot resynthesise.
 *
 * Three checks, in order (docs/testing/vocal-detection-research.md):
 *
 *   1. Polyphony gate. The share of note-active Basic Pitch frames holding two or
 *      more notes (`poly2`). At or above polyphonyThreshold the input is
 *      polyphonic, voice or not.
 *   2. Voice identity. YAMNet's maximum score over the speech and singing classes
 *      (YamnetVoiceTagger). At or above voiceThreshold it is a voice.
 *   3. Pitch cues. SwiftF0's off-semitone spread and glide share. They decide
 *      on their own when no tagger is available. In the tagger's unsure band
 *      around voiceThreshold they can only confirm a voice the tagger already
 *      leans towards (score >= voiceThreshold): they are a weak cue, which
 *      in-tune sample-library instruments fail but a real violin or sax with
 *      vibrato can pass, and an instrument sent to the voice-only vocoder is the
 *      costlier mistake.
 *
 * Under minAudibleSeconds of sound nothing is measured. Whatever cannot be
 * decided goes to the polyphonic engine, which can represent any input; the
 * user can switch the track back.
 *
 * The feature functions and decide() have no model or JUCE dependency, so they
 * can be tested on synthetic input. classify() runs the models.
 */
namespace inputtype
{

enum class Pipeline : std::uint8_t
{
    Vocal,
    Polyphonic
};

/** Why decide() chose its pipeline. */
enum class Reason : std::uint8_t
{
    NoAudio,              // nothing audible: polyphonic by default
    TooShort,             // under minAudibleSeconds of sound: polyphonic by default
    Polyphony,            // poly2 at or above the gate
    TaggerVoice,          // tagger score clearly above voiceThreshold
    TaggerNotVoice,       // tagger score clearly below voiceThreshold
    TaggerUnsureCuesVoice,        // tagger unsure but >= voiceThreshold; pitch cues voice-like
    TaggerUnsureCuesNotVoice,     // tagger unsure; pitch cues not voice-like
    TaggerUnsure,                 // tagger unsure, cues no help (too little pitch, or score below voiceThreshold): polyphonic
    CuesVoice,            // no tagger; pitch cues voice-like
    CuesNotVoice,         // no tagger; pitch cues not voice-like
    Undecided             // no tagger, too little pitch: polyphonic by default
};

const char* toString(Pipeline pipeline);
const char* toString(Reason reason);

struct Config
{
    // Polyphony gate. Measured on 84 files: solo singing max 0.050, instrument
    // mixtures median 0.838, a drum loop 0.143.
    float polyphonyThreshold = 0.15f;
    float noteActiveThreshold = 0.5f;   // Basic Pitch note posterior
    int minNoteActiveFrames = 10;       // fewer and the gate stays open (~0.12 s)

    // Voice identity (YAMNet). Measured: lowest voice 0.156, highest non-voice
    // 0.017; 0.05 sits between them on a log scale. Provisional until the
    // corpus holds real solo instruments with vibrato.
    float voiceThreshold = 0.05f;
    // Inside [unsureLow, unsureHigh] the input is a voice only if the score is
    // >= voiceThreshold and the pitch cues are voice-like; otherwise polyphonic.
    float unsureLow = 0.025f;
    float unsureHigh = 0.10f;

    // Pitch cues (SwiftF0). Measured on solo singing: offgrid >= 12.5 cents and
    // glide >= 0.075; instruments offgrid <= 9.7 and glide <= 0.023.
    float voicedConfidence = 0.9f;
    float loudRangeDb = 40.0f;          // frames within this of the loudest count
    float offgridVoiceCents = 11.0f;
    float glideVoiceShare = 0.05f;
    float glideMinCents = 15.0f;
    float glideMaxCents = 300.0f;
    int minVoicedFrames = 30;           // ~0.5 s of pitch; fewer and the cues abstain

    // Less audible audio than this (after excerpt selection) is not classified.
    double minAudibleSeconds = 3.0;

    // Excerpt: at most this much of the source's audible audio, in blocks.
    double excerptSeconds = 30.0;
    double excerptBlockSeconds = 0.5;
    float excerptRangeDb = 40.0f;       // blocks this far below the loudest are dropped
    float silenceDbfs = -70.0f;         // blocks below this are dropped regardless
};

/** Polyphony from a Basic Pitch note posteriorgram. */
struct Polyphony
{
    float poly2 = 0.0f;       // share of note-active frames with >= 2 notes
    float meanNotes = 0.0f;   // mean notes per note-active frame
    int activeFrames = 0;     // frames with >= 1 note
};

/**
 * Counts, per frame, the local-maximum keys above the note threshold, so that the
 * smear of one note into its neighbouring semitone is not a second note.
 */
Polyphony polyphony(const polyphonic::NoteEvidence& evidence, const Config& config = {});

/** A SwiftF0 track: one frame per SwiftF0PitchDetector::HOP_SIZE at 16 kHz. */
struct PitchTrack
{
    const float* f0Hz = nullptr;        // Hz; any value where confidence is low
    const float* confidence = nullptr;  // SwiftF0PitchDetector::getLastConfidence()
    int numFrames = 0;
};

enum class CueVerdict : std::uint8_t
{
    TooLittlePitch,
    VoiceLike,
    NotVoiceLike
};

struct PitchCues
{
    float offgridCents = 0.0f;  // median distance from the take's own semitone grid
    float glide = 0.0f;         // share of adjacent voiced frame pairs moving 15-300 cents
    int voicedFrames = 0;
    CueVerdict verdict = CueVerdict::TooLittlePitch;
};

/**
 * Pitch cues from a SwiftF0 track of `mono` (any sample rate). Frame i is voiced
 * when its confidence exceeds voicedConfidence and the RMS of the 64 ms starting
 * at its hop is within loudRangeDb of the loudest frame.
 */
PitchCues pitchCues(const PitchTrack& track, const float* mono, int numSamples, double sampleRate,
                    const Config& config = {});

/** Pitch cues from voiced frames' pitch alone (Hz); exposed for tests. */
PitchCues pitchCuesFromVoiced(const std::vector<float>& f0Hz, const std::vector<bool>& voiced,
                              const Config& config = {});

/** Everything decide() looks at. A negative score or poly2 means "not measured". */
struct Features
{
    Polyphony polyphony;
    bool polyphonyMeasured = false;
    float voiceScore = -1.0f;
    PitchCues cues;
    bool audible = true;
    double audibleSeconds = -1.0;   // length of the excerpt; negative: not measured
};

struct Decision
{
    Pipeline pipeline = Pipeline::Polyphonic;
    Reason reason = Reason::Undecided;
    Features features;

    /** False when the choice rests on a default or on pitch cues alone. */
    bool confident() const;
};

Decision decide(const Features& features, const Config& config = {});

/**
 * Up to excerptSeconds of `mono`'s audible audio: excerptBlockSeconds blocks
 * within excerptRangeDb of the loudest block and above silenceDbfs, in order,
 * starting with the first audible block. Empty when nothing is audible.
 */
std::vector<float> selectExcerpt(const float* mono, int numSamples, double sampleRate,
                                 const Config& config = {});

class YamnetVoiceTagger;

/**
 * Runs the checks on an excerpt (from selectExcerpt()) and its SwiftF0 track.
 * `basicPitch` and `tagger` may be null or unloaded; the check they serve is
 * then skipped. Returns NoAudio for an empty excerpt, TooShort (without running
 * the models) for one under minAudibleSeconds, and a default Polyphonic decision
 * when cancelled.
 */
Decision classify(const float* excerpt, int numSamples, double sampleRate, const PitchTrack& track,
                  polyphonic::BasicPitchEvidence* basicPitch, YamnetVoiceTagger* tagger,
                  const Config& config = {}, const std::atomic<bool>* cancel = nullptr);

} // namespace inputtype
