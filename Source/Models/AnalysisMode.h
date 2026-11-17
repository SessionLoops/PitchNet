#pragma once

#include "../Audio/Analysis/InputTypeClassifier.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

/**
 * Which analysis a take gets: the vocal pipeline or the polyphonic engine
 * (docs/polyphonic-integration-plan.md, section 1).
 *
 * The mode that applies is the TRACK's: in the standalone app and non-ARA
 * capture a take is a track; under ARA the track's mode is the
 * duration-weighted majority of its clips' classifications at the track's
 * first analysis, then sticky until the user overrides it. Every take keeps
 * its own classification as well, for the UI and diagnostics.
 *
 * No JUCE dependency: ProjectSerializer converts it to and from JSON with the
 * stable identifiers below.
 */
enum class AnalysisMode : std::uint8_t
{
    Vocal = 0,
    Polyphonic = 1
};

/** Candidate search of the polyphonic engine (polyphonic::DetectionMode). */
enum class PolyphonicDetector : std::uint8_t
{
    Harmonic = 0,
    Guided = 1
};

struct AnalysisModeState
{
    /** The mode that applies to this take: its track's mode. */
    AnalysisMode mode = AnalysisMode::Vocal;
    /** The user chose `mode`; it is never re-detected. */
    bool userChosen = false;

    /** This take's own classification (valid when `classified`). */
    bool classified = false;
    AnalysisMode detected = AnalysisMode::Vocal;
    inputtype::Reason reason = inputtype::Reason::Undecided;
    bool confident = false;
    float voiceScore = -1.0f;      // YAMNet voice score; negative: not measured
    float poly2 = -1.0f;           // Basic Pitch polyphony share; negative: not measured
    double audibleSeconds = -1.0;  // length of the excerpt that was classified

    /** Polyphonic takes: the engine's candidate search. */
    PolyphonicDetector detector = PolyphonicDetector::Harmonic;

    bool operator==(const AnalysisModeState& o) const
    {
        return mode == o.mode && userChosen == o.userChosen && classified == o.classified
               && detected == o.detected && reason == o.reason && confident == o.confident
               && voiceScore == o.voiceScore && poly2 == o.poly2 && audibleSeconds == o.audibleSeconds
               && detector == o.detector;
    }
    bool operator!=(const AnalysisModeState& o) const { return !(*this == o); }
};

namespace analysismode
{

inline AnalysisMode fromPipeline(inputtype::Pipeline pipeline)
{
    return pipeline == inputtype::Pipeline::Vocal ? AnalysisMode::Vocal : AnalysisMode::Polyphonic;
}

/**
 * Stores a classifier decision as the take's own classification. The mode that
 * applies follows it unless the user has chosen one; the track-level majority
 * (trackMode()) is applied by the caller where a track has several takes.
 */
inline void recordDecision(AnalysisModeState& state, const inputtype::Decision& decision)
{
    state.classified = true;
    state.detected = fromPipeline(decision.pipeline);
    state.reason = decision.reason;
    state.confident = decision.confident();
    state.voiceScore = decision.features.voiceScore;
    state.poly2 = decision.features.polyphonyMeasured ? decision.features.polyphony.poly2 : -1.0f;
    state.audibleSeconds = decision.features.audibleSeconds;
    if (!state.userChosen)
        state.mode = state.detected;
}

/**
 * A track's mode from its takes' classifications, each weighted by its length
 * in seconds: the mode with more audio wins; a tie, or nothing to weigh, gives
 * Polyphonic (the engine that can represent any input).
 */
inline AnalysisMode trackMode(const std::vector<std::pair<AnalysisMode, double>>& takes)
{
    double vocal = 0.0, polyphonic = 0.0;
    for (const auto& [mode, seconds] : takes)
        if (seconds > 0.0)
            (mode == AnalysisMode::Vocal ? vocal : polyphonic) += seconds;
    return vocal > polyphonic ? AnalysisMode::Vocal : AnalysisMode::Polyphonic;
}

// ---- stable identifiers for saving (never change existing strings) ----

inline const char* id(AnalysisMode mode)
{
    return mode == AnalysisMode::Polyphonic ? "polyphonic" : "vocal";
}

inline const char* id(PolyphonicDetector detector)
{
    return detector == PolyphonicDetector::Guided ? "guided" : "harmonic";
}

inline const char* id(inputtype::Reason reason)
{
    using R = inputtype::Reason;
    switch (reason)
    {
        case R::NoAudio: return "noAudio";
        case R::TooShort: return "tooShort";
        case R::Polyphony: return "polyphony";
        case R::TaggerVoice: return "taggerVoice";
        case R::TaggerNotVoice: return "taggerNotVoice";
        case R::TaggerUnsureCuesVoice: return "taggerUnsureCuesVoice";
        case R::TaggerUnsureCuesNotVoice: return "taggerUnsureCuesNotVoice";
        case R::TaggerUnsure: return "taggerUnsure";
        case R::CuesVoice: return "cuesVoice";
        case R::CuesNotVoice: return "cuesNotVoice";
        case R::Undecided: return "undecided";
    }
    return "undecided";
}

/** Parses an identifier written by id(); false (and `out` unchanged) when unknown. */
inline bool parse(const char* text, AnalysisMode& out)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "vocal") == 0) { out = AnalysisMode::Vocal; return true; }
    if (std::strcmp(text, "polyphonic") == 0) { out = AnalysisMode::Polyphonic; return true; }
    return false;
}

inline bool parse(const char* text, PolyphonicDetector& out)
{
    if (text == nullptr) return false;
    if (std::strcmp(text, "harmonic") == 0) { out = PolyphonicDetector::Harmonic; return true; }
    if (std::strcmp(text, "guided") == 0) { out = PolyphonicDetector::Guided; return true; }
    return false;
}

inline bool parse(const char* text, inputtype::Reason& out)
{
    if (text == nullptr) return false;
    for (int r = 0; r <= static_cast<int>(inputtype::Reason::Undecided); ++r)
    {
        const auto reason = static_cast<inputtype::Reason>(r);
        if (std::strcmp(text, id(reason)) == 0)
        {
            out = reason;
            return true;
        }
    }
    return false;
}

} // namespace analysismode
