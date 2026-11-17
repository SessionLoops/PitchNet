// Analysis mode (Source/Models/AnalysisMode.h): recording a classifier decision,
// the user's override, the track majority and the saved identifiers. No JUCE
// dependency; AnalysisModeSerializationTests covers the JSON round trip.
#include "../Source/Models/AnalysisMode.h"
#include "TestAssert.h"

#include <cstdio>
#include <cstring>

namespace
{
using inputtype::Pipeline;
using inputtype::Reason;

inputtype::Decision decision(Pipeline pipeline, Reason reason, float voice, float poly2, double seconds)
{
    inputtype::Decision d;
    d.pipeline = pipeline;
    d.reason = reason;
    d.features.voiceScore = voice;
    d.features.polyphony.poly2 = poly2;
    d.features.polyphonyMeasured = poly2 >= 0.0f;
    d.features.audibleSeconds = seconds;
    return d;
}

void testRecordDecision()
{
    AnalysisModeState s;
    CHECK(s.mode == AnalysisMode::Vocal && !s.classified && !s.userChosen);

    analysismode::recordDecision(s, decision(Pipeline::Polyphonic, Reason::Polyphony, -1.0f, 0.84f, 30.0));
    CHECK(s.classified);
    CHECK(s.detected == AnalysisMode::Polyphonic);
    CHECK(s.mode == AnalysisMode::Polyphonic);
    CHECK(s.reason == Reason::Polyphony);
    CHECK(s.confident);
    CHECK(s.poly2 == 0.84f);
    CHECK(s.voiceScore == -1.0f);
    CHECK(s.audibleSeconds == 30.0);

    // Not measured polyphony is stored as -1, not as the struct's 0.
    analysismode::recordDecision(s, decision(Pipeline::Vocal, Reason::CuesVoice, -1.0f, -1.0f, 12.0));
    CHECK(s.poly2 == -1.0f);
    CHECK(s.mode == AnalysisMode::Vocal);
    CHECK(!s.confident);   // pitch cues alone

    // The user's choice is never replaced by a new classification.
    s.userChosen = true;
    s.mode = AnalysisMode::Vocal;
    analysismode::recordDecision(s, decision(Pipeline::Polyphonic, Reason::TaggerNotVoice, 0.01f, 0.0f, 20.0));
    CHECK(s.detected == AnalysisMode::Polyphonic);
    CHECK(s.mode == AnalysisMode::Vocal);
    CHECK(s.userChosen);

    // Too short to classify: recorded as such, polyphonic by default.
    AnalysisModeState t;
    analysismode::recordDecision(t, decision(Pipeline::Polyphonic, Reason::TooShort, -1.0f, -1.0f, 1.2));
    CHECK(t.mode == AnalysisMode::Polyphonic && t.reason == Reason::TooShort && !t.confident);
}

void testTrackMode()
{
    using AM = AnalysisMode;
    CHECK(analysismode::trackMode({}) == AM::Polyphonic);
    CHECK(analysismode::trackMode({{AM::Vocal, 10.0}}) == AM::Vocal);
    // Duration-weighted, not a head count: one long sung take outweighs two short chord clips.
    CHECK(analysismode::trackMode({{AM::Vocal, 60.0}, {AM::Polyphonic, 8.0}, {AM::Polyphonic, 8.0}}) == AM::Vocal);
    CHECK(analysismode::trackMode({{AM::Vocal, 5.0}, {AM::Vocal, 5.0}, {AM::Polyphonic, 30.0}}) == AM::Polyphonic);
    // A tie goes to the engine that can represent any input.
    CHECK(analysismode::trackMode({{AM::Vocal, 10.0}, {AM::Polyphonic, 10.0}}) == AM::Polyphonic);
    // Unmeasured lengths carry no weight.
    CHECK(analysismode::trackMode({{AM::Vocal, -1.0}, {AM::Vocal, 0.0}}) == AM::Polyphonic);
}

void testIdentifiers()
{
    for (int r = 0; r <= static_cast<int>(Reason::Undecided); ++r)
    {
        const auto reason = static_cast<Reason>(r);
        Reason parsed = Reason::NoAudio;
        CHECK(analysismode::parse(analysismode::id(reason), parsed));
        CHECK(parsed == reason);
        for (int q = 0; q < r; ++q)
            CHECK(std::strcmp(analysismode::id(reason), analysismode::id(static_cast<Reason>(q))) != 0);
    }
    for (auto mode : {AnalysisMode::Vocal, AnalysisMode::Polyphonic})
    {
        AnalysisMode parsed = mode == AnalysisMode::Vocal ? AnalysisMode::Polyphonic : AnalysisMode::Vocal;
        CHECK(analysismode::parse(analysismode::id(mode), parsed) && parsed == mode);
    }
    for (auto detector : {PolyphonicDetector::Harmonic, PolyphonicDetector::Guided})
    {
        PolyphonicDetector parsed = detector == PolyphonicDetector::Harmonic ? PolyphonicDetector::Guided
                                                                            : PolyphonicDetector::Harmonic;
        CHECK(analysismode::parse(analysismode::id(detector), parsed) && parsed == detector);
    }
    // The saved strings are a file format: they must not change.
    CHECK(std::strcmp(analysismode::id(AnalysisMode::Polyphonic), "polyphonic") == 0);
    CHECK(std::strcmp(analysismode::id(PolyphonicDetector::Guided), "guided") == 0);
    CHECK(std::strcmp(analysismode::id(Reason::TaggerVoice), "taggerVoice") == 0);
    // Unknown or missing text leaves the value alone.
    AnalysisMode keep = AnalysisMode::Polyphonic;
    CHECK(!analysismode::parse("choir", keep) && keep == AnalysisMode::Polyphonic);
    CHECK(!analysismode::parse(nullptr, keep) && keep == AnalysisMode::Polyphonic);
}
} // namespace

int main()
{
    testRecordDecision();
    testTrackMode();
    testIdentifiers();
    std::puts("Analysis mode tests passed");
    return 0;
}
