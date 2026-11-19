// A polyphonic take survives a project file (format 6): its notes and their
// levels come back exactly (so the masks rebuilt from them with
// polyphonic::restore are the analysed ones, and the take is drawn the same
// before they are), with the analysis parameters and every kind of edit, through
// JSON and the binary archive; a not-yet-restored take is written back as it
// was read, and older files load without one.
#include "../Source/Models/ProjectSerializer.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr double kRate = 44100.0;

void addTone(std::vector<float>& x, double midi, double start, double seconds, double gain)
{
    const double f0 = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
    const int s0 = static_cast<int>(start * kRate), n = static_cast<int>(seconds * kRate);
    for (int h = 1; h <= 12 && h * f0 < 15000.0; ++h)
        for (int i = 0; i < n && s0 + i < static_cast<int>(x.size()); ++i)
        {
            const double t = i / kRate;
            const double env = std::min(1.0, t / 0.005) * std::exp(-t * (0.8 + 0.15 * h))
                               * std::min(1.0, (seconds - t) / 0.02);
            x[static_cast<size_t>(s0 + i)] += static_cast<float>(gain / h * env * std::sin(2.0 * juce::MathConstants<double>::pi * h * f0 * t + 0.37 * h * h));
        }
}

bool sameNotes(const std::vector<polyphonic::Note>& a, const std::vector<polyphonic::Note>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.id != y.id || x.startFrame != y.startFrame || x.endFrame != y.endFrame || x.parent != y.parent
            || x.active != y.active || x.inharmonicity != y.inharmonicity || x.probability != y.probability
            || x.energy != y.energy || x.f0 != y.f0)
            return false;
    }
    return true;
}

bool sameMasks(const polyphonic::Analysis& a, const polyphonic::Analysis& b)
{
    if (a.masks.size() != b.masks.size())
        return false;
    for (size_t i = 0; i < a.masks.size(); ++i)
    {
        if (a.masks[i].weights != b.masks[i].weights || a.masks[i].partials.size() != b.masks[i].partials.size())
            return false;
        for (size_t q = 0; q < a.masks[i].partials.size(); ++q)
        {
            const auto& p = a.masks[i].partials[q];
            const auto& r = b.masks[i].partials[q];
            if (p.frame != r.frame || p.lo != r.lo || p.count != r.count || p.amplitude != r.amplitude)
                return false;
        }
    }
    return true;
}

Project::PolyphonicEdits sampleEdits(const polyphonic::Analysis& analysis)
{
    // One of each kind of edit, on the first active notes.
    std::vector<int> active;
    for (size_t i = 0; i < analysis.notes.size(); ++i)
        if (analysis.notes[i].active)
            active.push_back(static_cast<int>(i));
    CHECK(active.size() >= 3);
    Project::PolyphonicEdits edits;
    edits[active[0]].semitones = 2.0f;
    edits[active[0]].gainDb = -3.5f;
    edits[active[1]].mute = true;
    edits[active[2]].timeOffsetSeconds = 0.125;
    edits[active[2]].stretch = 1.75f;
    edits[active[2]].semitones = -0.37f; // cents
    edits[active[2]].takeBackground = false;
    edits[active[2]].pitchCurve = {0.0f, 0.25f, 0.5f};
    return edits;
}
} // namespace

int main()
{
    // A short polyphonic take, analysed as the editor does.
    std::vector<float> mono(static_cast<size_t>(2.0 * kRate), 0.0f);
    addTone(mono, 60, 0.2, 1.5, 0.2);
    addTone(mono, 64, 0.2, 1.5, 0.2);
    addTone(mono, 67, 0.8, 1.0, 0.2);
    auto params = polyphonic::Params::forSampleRate(kRate);
    params.detectionMode = polyphonic::DetectionMode::NeuralGuided; // saved as "guided"
    const auto analysis = polyphonic::analyze(mono.data(), static_cast<int>(mono.size()), params);

    Project project;
    auto take = std::make_shared<PolyphonicTake>();
    take->analysis = std::make_shared<const polyphonic::Analysis>(analysis);
    take->source.setSize(1, static_cast<int>(mono.size()));
    take->source.copyFrom(0, 0, mono.data(), static_cast<int>(mono.size()));
    take->sourceRate = kRate;
    project.setPolyphonicTake(take);
    const auto edits = sampleEdits(analysis);
    project.setPolyphonicEdits(edits);

    const auto checkLoaded = [&](const Project& loaded, const char* what) {
        const auto pending = loaded.getPendingPolyphonicRestore();
        CHECK(pending != nullptr);
        CHECK(sameNotes(pending->notes, analysis.notes));
        CHECK(pending->params.sampleRate == params.sampleRate);
        CHECK(pending->params.hop == params.hop);
        CHECK(pending->params.fftSize == params.fftSize);
        CHECK(pending->params.detectionMode == polyphonic::DetectionMode::NeuralGuided);
        CHECK(loaded.getPolyphonicEdits() == edits);
        CHECK(!loaded.isPolyphonic()); // until it is restored
        // The levels the piano roll draws from, so an opened take looks as it
        // will once restored; the take shown meanwhile carries them.
        CHECK(pending->levels == noteLevelsFromMasks(analysis));
        const auto restoring = makeRestoringTake(*pending, static_cast<int>(mono.size()));
        CHECK(restoring->restoring && noteLevels(*restoring) == noteLevelsFromMasks(analysis));
        CHECK(restoring->analysis->numFrames == analysis.numFrames);
        CHECK(restoring->analysis->numBins == analysis.numBins);
        std::printf("%s: %zu notes and %zu edits back exactly\n", what, pending->notes.size(),
                    loaded.getPolyphonicEdits().size());
    };

    // JSON, as the .pitchnet file is written and read.
    {
        const auto text = juce::JSON::toString(ProjectSerializer::toJson(project, true, true), true);
        Project loaded;
        CHECK(ProjectSerializer::fromJson(loaded, juce::JSON::parse(text)));
        checkLoaded(loaded, "JSON");

        // The masks rebuilt from the saved notes are the analysed ones.
        const auto pending = loaded.getPendingPolyphonicRestore();
        const auto restored = polyphonic::restore(mono.data(), static_cast<int>(mono.size()),
                                                  pending->params, pending->notes);
        CHECK(sameMasks(restored, analysis));
        std::printf("JSON: masks rebuilt from the saved notes are identical\n");

        // Saved again before it is restored: written back as it was read.
        const auto again = juce::JSON::toString(ProjectSerializer::toJson(loaded, true, true), true);
        Project reloaded;
        CHECK(ProjectSerializer::fromJson(reloaded, juce::JSON::parse(again)));
        checkLoaded(reloaded, "JSON, saved again before the restore");
    }

    // The binary archive (plug-in state) carries the same section.
    {
        juce::MemoryBlock block;
        CHECK(ProjectSerializer::toBinaryArchive(project, block));
        Project loaded;
        CHECK(ProjectSerializer::fromBinaryArchive(loaded, block.getData(), block.getSize()));
        checkLoaded(loaded, "binary archive");
    }

    // A vocal project writes no section, and a file without one loads without
    // a take or edits.
    {
        Project vocal;
        const auto json = ProjectSerializer::toJson(vocal, false, false);
        CHECK(!json.hasProperty("polyphonic"));
        Project loaded;
        loaded.setPolyphonicEdits(edits); // stale state must not survive the load
        CHECK(ProjectSerializer::fromJson(loaded, json));
        CHECK(loaded.getPendingPolyphonicRestore() == nullptr);
        CHECK(loaded.getPolyphonicEdits().empty());
        std::printf("vocal project: no polyphonic section, nothing restored\n");
    }

    // A malformed section is ignored, not fatal.
    {
        auto json = ProjectSerializer::toJson(project, false, false);
        auto* section = json.getProperty("polyphonic", {}).getDynamicObject();
        CHECK(section != nullptr);
        section->setProperty("notes", "not base64 at all");
        Project loaded;
        CHECK(ProjectSerializer::fromJson(loaded, json));
        CHECK(loaded.getPendingPolyphonicRestore() == nullptr);
        std::printf("malformed section: the project still loads, without the take\n");
    }

    std::printf("PolyphonicSerializationTests passed\n");
    return 0;
}
