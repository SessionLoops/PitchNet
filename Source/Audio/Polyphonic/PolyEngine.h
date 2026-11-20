#pragma once

#include <algorithm>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

/**
 * Polyphonic engine - note analysis and per-note resynthesis of polyphonic audio.
 *
 * Built from the multi-pitch estimation and source-separation literature:
 *
 *   analysis   STFT -> instantaneous frequency per bin -> spectral peaks weighted
 *              by tonality -> log-frequency (cents) field -> harmonic-sum
 *              salience (Klapuri 2006 weights) -> iterative estimation and
 *              cancellation (cf. Klapuri 2003): take the strongest pitch, follow
 *              it in time, cancel part of its harmonic energy (bounded by a
 *              spectral-smoothness envelope), repeat; merge rediscovered notes
 *   notes      per-candidate f0 tracks and inharmonicity; a candidate whose own
 *              harmonic energy decays and rises again is split (a repeated note)
 *   filter     candidates are scored as real notes or harmonic ghosts by
 *              gradient-boosted trees over harmonic-relationship, energy and
 *              (optionally) Basic Pitch note-posterior features
 *              (PolyPurifierModel.h); rejected candidates stay available as
 *              inactive note candidates
 *   model      a pitch-informed harmonic NMF (cf. Ewert & Mueller 2012) gives each
 *              note its share of the bins around its partials
 *   render     an edited note's share is moved in frequency with phase-coherent
 *              accumulation (Laroche & Dolson 1999), re-gained, muted or moved in
 *              time; everything else passes through untouched, so an unedited
 *              render reproduces the input.
 *
 * Only pitched notes are modelled. Transients and other unpitched sound are never
 * detected as objects of their own. An edit with NoteEdit::takeBackground (the
 * default) attaches the nearby ones to its note; without it they stay in the
 * recording where they are.
 *
 * Self-contained C++17 without JUCE. Analysis and rendering are offline
 * operations for a worker thread, not the audio thread. See docs/polyphonic-editing.md.
 */
namespace polyphonic
{

/** Candidate search strategy. NeuralGuided is experimental and needs neural evidence. */
enum class DetectionMode { Harmonic, NeuralGuided };

struct Params
{
    // ---- framing (scaled with the sample rate by Params::forSampleRate) ----
    double sampleRate = 44100.0;
    int fftSize = 4096;
    int hop = 512;

    // ---- spectral peaks ----
    float peakAbsDb = -90.0f;          // absolute floor (sinusoid amplitude, dBFS)
    float peakRelDb = -75.0f;          // relative to the loudest peak of the frame
    float peakOverNoiseDb = 4.0f;      // above the median-filtered noise floor
    int noiseMedianBins = 41;
    float tonalDevBins = 0.35f;        // IF disagreement scale for tonality

    // ---- cents field ----
    float fRef = 27.5f;
    float centsStep = 10.0f;
    float fTop = 11000.0f;
    float spreadCents = 50.0f;         // deposit half width (about a half tone)
    float whitenNu = 0.33f;            // Klapuri spectral whitening exponent

    // ---- relevance landscape ----
    float f0Min = 30.0f;
    float f0Max = 2200.0f;
    int numHarmonicsRelevance = 16;
    float alpha = 27.0f;
    float beta = 320.0f;

    // ---- iterative estimation and cancellation ----
    float subtractFraction = 0.5f;     // G: share of the energy withdrawn per pass
    float stopEnergy = 0.10f;          // stop when this share of the field energy remains
    float stopRelativePeak = 0.04f;    // ... or when the best maximum falls this low
    float crestRelative = 0.10f;       // abandon a crest below 10 % of its peak
    float crestJumpCents = 50.0f;      // ... or on a pitch jump above 50 cents
    int maxIterations = 4000;
    int numHarmonicsSubtract = 40;

    // ---- candidate notes ----
    float minNoteSeconds = 0.07f;
    float minRelativeStrength = 0.02f;
    int mergeGapFrames = 3;
    float activationThreshold = 0.5f;  // purifier probability for an active note (harmonic detector)
    // The guided detector has its own classifier; validation F1 is flat from 0.60 to 0.80
    // and 0.60 separates best (docs/testing/polyphonic-detector-research.md, threshold tuning).
    float guidedActivationThreshold = 0.6f;
    float activationThresholdFor(DetectionMode mode) const
    {
        return mode == DetectionMode::NeuralGuided ? guidedActivationThreshold : activationThreshold;
    }

    // ---- sound model / masks ----
    int nmfIterations = 40;
    int nmfHarmonics = 40;
    int nmfHalfWidth = 3;
    float nmfSegmentSeconds = 0.5f;
    float nmfMaxHz = 12000.0f;
    int preFrames = 3;                 // masks start this many frames before a note begins
    int postFrames = 3;

    // ---- rendering ----
    float stretchAttackSeconds = 0.08f; // a stretched note's first part keeps its speed
    float cutFadeSeconds = 0.15f;      // a shortened note fades out over this long before its new end
    // Lengthening a DECAYING note (piano, harp): its level falls faster than this after the attack
    // (dB per analysis frame, from its own partials), so instead of being slowed down it plays as
    // recorded up to its release, rings on at each partial's own decay rate for the extra length,
    // then plays its release. 0 turns this off (every lengthened note is time-stretched).
    float naturalDecaySlopeDb = -0.05f;
    float releaseDropDb = 3.0f;        // frames this far below the note's decay line are its release

    // ---- diagnostics ----
    bool exportFeatures = false;       // fill Analysis::candidateFeatures (purifier training)

    // Opt-in research detector; without evidence it uses the original DSP-only path.
    DetectionMode detectionMode = DetectionMode::Harmonic;

    /** Framing that keeps ~93 ms windows and 8x overlap at any sample rate. */
    static Params forSampleRate(double rate)
    {
        Params p;
        p.sampleRate = rate;
        p.fftSize = rate > 64000.0 ? 8192 : 4096;
        p.hop = p.fftSize / 8;
        return p;
    }
};

/** One note. Frames are analysis hops. */
struct Note
{
    int id = 0;
    int startFrame = 0;
    int endFrame = 0;                  // inclusive
    std::vector<float> f0;             // Hz, one value per frame startFrame..endFrame
    float inharmonicity = 0.0f;        // B in f_h = h f0 sqrt(1 + B h^2)
    double energy = 0.0;               // energy withdrawn during extraction
    float probability = 1.0f;          // purifier score
    bool active = true;                // false: candidate only (kept for "note assignment")
    int parent = -1;                   // for inactive overtone candidates: the note they belong to

    float medianMidi() const;
    int numFrames() const { return endFrame - startFrame + 1; }
    float f0At(int frame) const;       // clamped to the note's own range
};

/** A note's claim on one partial in one frame: weights for bins lo..lo+weights.size()-1. */
struct PartialMask
{
    int frame = 0;
    int harmonic = 0;
    int lo = 0;
    int count = 0;
    int weightOffset = 0;              // into NoteMasks::weights
    float freqHz = 0.0f;               // measured partial frequency
    float amplitude = 0.0f;            // model amplitude (for formant handling)
};

struct NoteMasks
{
    std::vector<PartialMask> partials; // sorted by frame, then harmonic
    std::vector<float> weights;
};

/** Purifier input for one candidate (before fragment merging), for training and diagnostics. */
struct CandidateFeatures
{
    int startFrame = 0;
    int endFrame = 0;
    float midi = 0.0f;
    float probability = 0.0f;
    std::vector<float> x;              // feature vector in the order of PolyPurifierModel.h
};

struct Analysis
{
    Params params;
    int numSamples = 0;
    int numFrames = 0;
    int numBins = 0;
    std::vector<Note> notes;           // active notes followed by inactive candidates
    std::vector<NoteMasks> masks;      // one entry per note in `notes` (inactive: empty)
    std::vector<CandidateFeatures> candidateFeatures; // only with Params::exportFeatures

    double frameToSeconds(double frame) const { return frame * params.hop / params.sampleRate; }
};

/**
 * Optional neural note evidence on a semitone grid (MIDI 21..108): how likely each
 * semitone is sounding in each frame, e.g. the note posteriorgram of Spotify's
 * Basic Pitch model. Frame times are in seconds. When absent, the purifier uses
 * its DSP-only weights.
 */
struct NoteEvidence
{
    std::vector<float> frameTimes;     // seconds, ascending
    std::vector<float> note;           // [frames x 88]
    bool empty() const { return frameTimes.empty(); }
};

/** Per-note edit. Pitch offsets are in semitones relative to the analysed pitch. */
struct NoteEdit
{
    float semitones = 0.0f;
    std::vector<float> pitchCurve;     // optional per-frame offset (semitones), added to `semitones`
    float gainDb = 0.0f;
    bool mute = false;
    bool preserveFormants = false;     // instrument partial patterns are not formants; see docs
    double timeOffsetSeconds = 0.0;    // move the note
    // Length ratio, 0..4. Longer (> 1): the part after the attack is that many times as long and
    // slows down; the attack keeps its speed. Shorter (< 1): the note keeps that share of its length;
    // it plays as recorded and is cut, fading out just before its new end.
    float stretch = 1.0f;
    // The note also owns a part of what no note owns (the background around its harmonics, the
    // noise at its start, its decay for up to 2 s; see render()). A mute removes that part too, and a
    // move, pitch or gain change takes it along; a shortened note cuts it with the note (a lengthened
    // one leaves it in place). On by default, as in the editor: a muted piano note then also loses its
    // hammer noise, and a moved note takes its attack along. Off gives the narrower edit that leaves
    // everything outside the note's own partials in place.
    bool takeBackground = true;
    bool isIdentity() const
    {
        return !mute && semitones == 0.0f && pitchCurve.empty() && gainDb == 0.0f && timeOffsetSeconds == 0.0 && stretch == 1.0f;
    }
    bool operator==(const NoteEdit& o) const
    {
        return semitones == o.semitones && pitchCurve == o.pitchCurve && gainDb == o.gainDb && mute == o.mute
            && preserveFormants == o.preserveFormants && timeOffsetSeconds == o.timeOffsetSeconds
            && stretch == o.stretch && takeBackground == o.takeBackground;
    }
    bool operator!=(const NoteEdit& o) const { return !(*this == o); }
};

/**
 * Where a stretched note's frames go, in analysis frames before any move. Lengthened (ratio > 1):
 * frames up to `anchor` (the attack) keep their speed, later frames are spaced `ratio` times as far
 * apart. Shortened (ratio < 1): every frame keeps its place and the note ends at `end`, `ratio` of
 * the way from its first frame to its last; later frames are cut.
 */
struct StretchMap
{
    double anchor = 0.0;
    double ratio = 1.0;
    double end = 0.0;                  // last frame played when shortened
    bool cuts() const { return ratio < 1.0; }
    double outputFrame(double sourceFrame) const
    {
        if (cuts())
            return std::min(sourceFrame, end);
        return sourceFrame <= anchor ? sourceFrame : anchor + (sourceFrame - anchor) * ratio;
    }
    double sourceFrame(double outputFrame) const
    {
        if (cuts())
            return outputFrame;
        return outputFrame <= anchor ? outputFrame : anchor + (outputFrame - anchor) / ratio;
    }
};

/** The time map render() uses for `note` under `edit` (ratio 1 when the note is not stretched). */
StretchMap stretchMap(const Analysis& analysis, int note, const NoteEdit& edit);

using Progress = std::function<void(float)>;

/** Analyse mono audio. Returns notes, candidates, masks. */
Analysis analyze(const float* mono, int numSamples, const Params& params,
                 const NoteEvidence* evidence = nullptr,
                 const std::atomic<bool>* cancel = nullptr,
                 const Progress& progress = {});

/**
 * Rebuild an analysis from its notes without detecting notes again, e.g. when a saved project
 * is opened: the spectra and the active notes' masks are computed for this audio. Given the
 * audio, params and notes analyze() produced, the result equals that analysis (inactive notes
 * keep no masks, as there). `notes` keep their order, so edits keyed by note index still apply.
 */
Analysis restore(const float* mono, int numSamples, const Params& params, std::vector<Note> notes,
                 const std::atomic<bool>* cancel = nullptr, const Progress& progress = {});

/**
 * Render the edited result for each input channel (the channels the analysis
 * was made from, e.g. left/right; the masks come from their mono mix).
 * Notes without an entry in `edits` pass through untouched, and frames no edit
 * reaches are copied rather than transformed, so the cost follows the edited
 * notes rather than the length of the recording.
 *
 * A note's share of a bin is its model over the total model including the noise
 * floor, so an edit leaves the noise floor's share of its bins, and everything no note
 * owns, in place.
 * With NoteEdit::takeBackground (the default): what no note owns is split among the notes
 * sounding in each frame (by level and spectral shape; at an onset what is new goes to
 * the notes starting there, in proportion to each one's own spectrum; a claim fades out
 * over 2 s after a note ends). A muted note
 * removes its part, and a moved, re-pitched or re-gained note takes it along, each
 * partial's band moving with that partial. Muting every note then silences the frames
 * their claims cover.
 */
/**
 * Work a render does that depends only on the analysis and the audio, never on the edits (how
 * what no note owns is split among the notes, NoteEdit::takeBackground), kept so later renders
 * skip it. It fills in as frames are rendered, around the edited notes, and never changes the
 * output. It holds at most `maxBytes` (about 110 kB per frame of a dense passage, so 128 MB keeps
 * some 13 s at 44.1 kHz), dropping the least recently used frames first. Use one per analysis
 * and the channels it renders: it starts over by itself when the analysis object, the number of
 * channels or their length changes, but not when different audio of the same size is passed.
 * Not thread safe: one render at a time.
 */
struct RenderCache;
std::shared_ptr<RenderCache> makeRenderCache(size_t maxBytes = size_t{128} << 20);
/** What `cache` holds now, in bytes. */
size_t renderCacheBytes(const RenderCache& cache);

std::vector<std::vector<float>> render(const Analysis& analysis,
                                       const std::vector<std::vector<float>>& channels,
                                       const std::map<int, NoteEdit>& edits,
                                       const std::atomic<bool>* cancel = nullptr,
                                       RenderCache* cache = nullptr);

/**
 * The separated signal of a set of notes (no edits) for each input channel, for
 * auditioning a selection on its own. Empty `noteIndices` gives silence. With
 * `takeBackground` each note also brings its part of what no note owns (exactly what a
 * NoteEdit::takeBackground mute removes).
 */
std::vector<std::vector<float>> extractNotes(const Analysis& analysis,
                                             const std::vector<std::vector<float>>& channels,
                                             const std::vector<int>& noteIndices,
                                             const std::atomic<bool>* cancel = nullptr,
                                             bool takeBackground = false);

/** Part of a recording: `channels` hold the samples from `start` on. */
struct AudioSpan
{
    int start = 0;
    std::vector<std::vector<float>> channels;
};

/**
 * A set of notes on their own with their entries in `edits` applied (pitch, gain, mute, move,
 * stretch), for hearing an edit while it is being made. It is the same synthesis as render(): the
 * listed notes sound exactly as they will in the full render, minus everything else. Notes
 * without an edit come out as their separated signal (with their part of the background when
 * their entry sets NoteEdit::takeBackground, even if it changes nothing else); edits of notes not
 * listed are ignored.
 * Only the samples the notes reach are rendered and returned, so the cost follows the notes,
 * not the recording (an empty span when nothing sounds, e.g. a muted note).
 */
AudioSpan renderNotes(const Analysis& analysis,
                      const std::vector<std::vector<float>>& channels,
                      const std::vector<int>& noteIndices,
                      const std::map<int, NoteEdit>& edits,
                      const std::atomic<bool>* cancel = nullptr);

/** Samples [start, end) of a recording (empty when end <= start). */
struct SampleRange
{
    int start = 0;
    int end = 0;
    bool empty() const { return end <= start; }
};

/**
 * The samples of render() that can differ between `previousEdits` and `edits` over
 * `numSamples` of audio (the channels' length): the samples of every frame a changed note
 * reaches under its old or new edit. Outside the range the two renders are identical, and
 * renderRange() over it gives exactly the samples render(edits) has there, so the previous render
 * with this range replaced IS the new render. Empty when nothing changed. The cost of a
 * re-render then follows the change, not the number of edits or the length of the recording.
 */
SampleRange changedRange(const Analysis& analysis, int numSamples,
                         const std::map<int, NoteEdit>& previousEdits,
                         const std::map<int, NoteEdit>& edits);

/**
 * render() restricted to `range`: the samples [range.start, range.end) of the full render for
 * each channel, computed from only the frames that overlap them (empty channels for an empty
 * range; empty when cancelled). An edited note that starts before the range is first run over
 * its own earlier frames without output, so its phases match the full render's.
 */
std::vector<std::vector<float>> renderRange(const Analysis& analysis,
                                            const std::vector<std::vector<float>>& channels,
                                            const std::map<int, NoteEdit>& edits, SampleRange range,
                                            const std::atomic<bool>* cancel = nullptr,
                                            RenderCache* cache = nullptr);

/**
 * What changes in render() when the edits go from `previousEdits` to `edits`: add the span to the
 * previous render (its channels from span.start on) to get the new one. Only the changed notes
 * are synthesised, each as its new edit minus its old one, over changedRange(), so the cost
 * follows the change alone, not the other edited notes around it. Equal to the difference of the
 * two renders up to float rounding (about 1e-7 of full scale, below -130 dB); render() or
 * renderRange() give the exact result. Nothing changed: a span with no samples. Cancelled: a
 * span with no channels.
 */
AudioSpan renderChange(const Analysis& analysis, const std::vector<std::vector<float>>& channels,
                       const std::map<int, NoteEdit>& previousEdits, const std::map<int, NoteEdit>& edits,
                       const std::atomic<bool>* cancel = nullptr, RenderCache* cache = nullptr);

/** One note's separated signal (no edits), for auditioning/solo. */
std::vector<float> extractNote(const Analysis& analysis, const std::vector<float>& channel, int noteIndex);

} // namespace polyphonic
