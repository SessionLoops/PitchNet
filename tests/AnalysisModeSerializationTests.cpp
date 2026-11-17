// The analysis mode survives every project format: JSON files, the binary
// archive (plug-in state and ARA modification archives embed the same JSON),
// and older files without it load as vocal and unclassified.
#include "../Source/Models/ProjectSerializer.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>

namespace
{
AnalysisModeState sample()
{
    AnalysisModeState s;
    s.mode = AnalysisMode::Polyphonic;
    s.userChosen = true;
    s.classified = true;
    s.detected = AnalysisMode::Vocal;
    s.reason = inputtype::Reason::TaggerUnsureCuesVoice;
    s.confident = false;
    s.voiceScore = 0.0731f;
    s.poly2 = 0.0125f;
    s.audibleSeconds = 28.5;
    s.detector = PolyphonicDetector::Guided;
    return s;
}

bool same(const AnalysisModeState& a, const AnalysisModeState& b)
{
    return a.mode == b.mode && a.userChosen == b.userChosen && a.classified == b.classified
           && a.detected == b.detected && a.reason == b.reason && a.confident == b.confident
           && std::fabs(a.voiceScore - b.voiceScore) < 1e-6f && std::fabs(a.poly2 - b.poly2) < 1e-6f
           && std::fabs(a.audibleSeconds - b.audibleSeconds) < 1e-9 && a.detector == b.detector;
}
} // namespace

int main()
{
    // Direct conversion.
    CHECK(same(ProjectSerializer::analysisModeFromJson(ProjectSerializer::analysisModeToJson(sample())), sample()));
    CHECK(same(ProjectSerializer::analysisModeFromJson(ProjectSerializer::analysisModeToJson({})), {}));

    // Missing or malformed: the default state.
    CHECK(same(ProjectSerializer::analysisModeFromJson({}), {}));
    CHECK(same(ProjectSerializer::analysisModeFromJson(juce::var("polyphonic")), {}));
    // Unknown identifiers keep their defaults; the rest is still read.
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("mode", "choir");
        obj->setProperty("reason", "somethingNew");
        obj->setProperty("userChosen", true);
        const auto s = ProjectSerializer::analysisModeFromJson(juce::var(obj));
        CHECK(s.mode == AnalysisMode::Vocal);
        CHECK(s.reason == inputtype::Reason::Undecided);
        CHECK(s.userChosen);
    }

    // Through a whole project, JSON and binary archive.
    Project project;
    project.setAnalysisMode(sample());
    CHECK(project.isModified());
    {
        Project restored;
        CHECK(ProjectSerializer::fromJson(restored, ProjectSerializer::toJson(project)));
        CHECK(same(restored.getAnalysisMode(), sample()));
    }
    {
        juce::MemoryBlock archive;
        CHECK(ProjectSerializer::toBinaryArchive(project, archive));
        Project restored;
        CHECK(ProjectSerializer::fromBinaryArchive(restored, archive.getData(), archive.getSize()));
        CHECK(same(restored.getAnalysisMode(), sample()));
    }

    // A project saved before the classifier existed: no "analysisMode" key.
    {
        auto json = ProjectSerializer::toJson(project);
        json.getDynamicObject()->removeProperty("analysisMode");
        Project restored;
        restored.setAnalysisMode(sample());   // must be reset, not kept
        CHECK(ProjectSerializer::fromJson(restored, json));
        CHECK(same(restored.getAnalysisMode(), {}));
    }

    // Copies carry it (the plug-in snapshots whole projects).
    {
        Project copy(project);
        CHECK(same(copy.getAnalysisMode(), sample()));
    }

    std::puts("Analysis mode serialization tests passed");
    return 0;
}
