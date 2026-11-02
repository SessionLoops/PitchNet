// polyphonic_cli - analyse a WAV file into notes and render per-note edits.
//
//   polyphonic_cli in.wav [options]
//     --model basic_pitch_nmp.onnx       neural note evidence (build with ONNX Runtime)
//     --evidence file.ev                 precomputed evidence (see --dump-evidence)
//     --candidates                       also list rejected candidates
//     --json notes.json                  notes and f0 curves
//     --edit INDEX:SEMITONES[:GAIN_DB[:mute]]   (repeatable) pitch / gain / mute a note
//     --move INDEX:SECONDS               (repeatable) move a note in time
//     --stretch INDEX:RATIO              (repeatable) lengthen (> 1) or shorten (< 1) a note
//     --pad SECONDS                      append silence first (room for lengthened notes)
//     --out out.wav                      render with the edits (32-bit float WAV)
//     --solo INDEX solo.wav              one note's separated signal
//     --solo-all DIR                     every active note's separated signal
//     --dump-features file.tsv           purifier features per candidate (training)
//     --dump-evidence file.ev            the evidence computed with --model
#include "../../Source/Audio/Polyphonic/PolyEngine.h"
#include "../../Source/Audio/Polyphonic/BasicPitchEvidence.h"
#include "WavIO.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static bool readEvidence(const std::string& path, polyphonic::NoteEvidence& ev)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    int32_t frames = 0;
    f.read(reinterpret_cast<char*>(&frames), 4);
    ev.frameTimes.resize(static_cast<size_t>(frames));
    ev.note.resize(static_cast<size_t>(frames) * 88);
    f.read(reinterpret_cast<char*>(ev.frameTimes.data()), 4 * frames);
    f.read(reinterpret_cast<char*>(ev.note.data()), 4 * 88 * frames);
    return static_cast<bool>(f);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s in.wav [options]\n", argv[0]); return 2; }
    wavio::Audio in;
    std::string err;
    if (!wavio::read(argv[1], in, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::map<int, polyphonic::NoteEdit> edits;
    std::string outPath, jsonPath, evPath, soloPath, soloDir, modelPath, dumpEvidence, dumpFeatures;
    int soloIndex = -1;
    bool printCandidates = false;
    double padSeconds = 0.0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--out") outPath = next();
        else if (a == "--json") jsonPath = next();
        else if (a == "--evidence") evPath = next();
        else if (a == "--model") modelPath = next();
        else if (a == "--dump-evidence") dumpEvidence = next();
        else if (a == "--dump-features") dumpFeatures = next();
        else if (a == "--candidates") printCandidates = true;
        else if (a == "--solo") { soloIndex = std::atoi(next().c_str()); soloPath = next(); }
        else if (a == "--solo-all") soloDir = next();
        else if (a == "--pad") padSeconds = std::atof(next().c_str());
        else if (a == "--edit" || a == "--move" || a == "--stretch")
        {
            std::stringstream ss(next());
            std::string tok;
            std::vector<std::string> parts;
            while (std::getline(ss, tok, ':')) parts.push_back(tok);
            if (parts.size() < 2) continue;
            auto& e = edits[std::atoi(parts[0].c_str())];
            if (a == "--move") e.timeOffsetSeconds = std::atof(parts[1].c_str());
            else if (a == "--stretch") e.stretch = static_cast<float>(std::atof(parts[1].c_str()));
            else
            {
                e.semitones = static_cast<float>(std::atof(parts[1].c_str()));
                if (parts.size() > 2) e.gainDb = static_cast<float>(std::atof(parts[2].c_str()));
                if (parts.size() > 3 && parts[3] == "mute") e.mute = true;
            }
        }
    }
    if (padSeconds > 0.0)
        for (auto& ch : in.channels)
            ch.resize(ch.size() + static_cast<size_t>(padSeconds * in.sampleRate), 0.0f);
    std::vector<float> mono(static_cast<size_t>(in.numSamples()), 0.0f);
    for (const auto& ch : in.channels)
        for (size_t i = 0; i < mono.size(); ++i) mono[i] += ch[i] / static_cast<float>(in.channels.size());
    polyphonic::NoteEvidence ev;
    if (!evPath.empty() && !readEvidence(evPath, ev)) { std::fprintf(stderr, "cannot read evidence %s\n", evPath.c_str()); return 1; }
    if (!modelPath.empty())
    {
        polyphonic::BasicPitchEvidence bp;
        if (!bp.load(modelPath)) { std::fprintf(stderr, "cannot load %s: %s\n", modelPath.c_str(), bp.lastError().c_str()); return 1; }
        const auto e0 = std::chrono::steady_clock::now();
        ev = bp.compute(mono.data(), static_cast<int>(mono.size()), in.sampleRate);
        std::fprintf(stderr, "evidence: %.2f s, %zu frames\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - e0).count(), ev.frameTimes.size());
        if (!dumpEvidence.empty())
        {
            std::ofstream f(dumpEvidence, std::ios::binary);
            const int32_t frames = static_cast<int32_t>(ev.frameTimes.size());
            f.write(reinterpret_cast<const char*>(&frames), 4);
            f.write(reinterpret_cast<const char*>(ev.frameTimes.data()), 4 * frames);
            f.write(reinterpret_cast<const char*>(ev.note.data()), 4 * 88 * frames);
        }
    }
    auto params = polyphonic::Params::forSampleRate(in.sampleRate);
    params.exportFeatures = !dumpFeatures.empty();
    const auto t0 = std::chrono::steady_clock::now();
    const auto A = polyphonic::analyze(mono.data(), static_cast<int>(mono.size()), params, ev.empty() ? nullptr : &ev);
    const auto t1 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "analysis: %.2f s for %.2f s of audio, %zu notes/candidates\n",
                 std::chrono::duration<double>(t1 - t0).count(), in.numSamples() / static_cast<double>(in.sampleRate), A.notes.size());
    for (size_t i = 0; i < A.notes.size(); ++i)
    {
        const auto& n = A.notes[i];
        if (!n.active && !printCandidates) continue;
        std::printf("%s %3zu midi %6.2f  %6.3f-%6.3f s  B=%.5f p=%.2f%s\n", n.active ? "note" : "cand", i, n.medianMidi(),
                    A.frameToSeconds(n.startFrame), A.frameToSeconds(n.endFrame), n.inharmonicity, n.probability,
                    n.parent >= 0 ? (" parent " + std::to_string(n.parent)).c_str() : "");
    }
    if (!dumpFeatures.empty())
    {
        std::ofstream f(dumpFeatures);
        for (const auto& c : A.candidateFeatures)
        {
            f << A.frameToSeconds(c.startFrame) << '\t' << A.frameToSeconds(c.endFrame) << '\t' << c.midi << '\t' << c.probability;
            for (float v : c.x)
                f << '\t' << v;
            f << '\n';
        }
    }
    if (!jsonPath.empty())
    {
        std::ofstream j(jsonPath);
        j << "{\"hop\":" << params.hop << ",\"sampleRate\":" << params.sampleRate << ",\"notes\":[";
        for (size_t i = 0; i < A.notes.size(); ++i)
        {
            const auto& n = A.notes[i];
            j << (i ? "," : "") << "{\"start\":" << n.startFrame << ",\"end\":" << n.endFrame << ",\"active\":" << (n.active ? "true" : "false")
              << ",\"p\":" << n.probability << ",\"B\":" << n.inharmonicity << ",\"energy\":" << n.energy << ",\"f0\":[";
            for (size_t k = 0; k < n.f0.size(); ++k) j << (k ? "," : "") << n.f0[k];
            j << "]}";
        }
        j << "]}\n";
    }
    if (!outPath.empty())
    {
        const auto t2 = std::chrono::steady_clock::now();
        wavio::Audio out;
        out.sampleRate = in.sampleRate;
        out.channels = polyphonic::render(A, in.channels, edits);
        const auto t3 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "render: %.2f s\n", std::chrono::duration<double>(t3 - t2).count());
        wavio::writeFloat(outPath, out);
    }
    if (soloIndex >= 0 && !soloPath.empty())
    {
        wavio::Audio out;
        out.sampleRate = in.sampleRate;
        for (const auto& ch : in.channels) out.channels.push_back(polyphonic::extractNote(A, ch, soloIndex));
        wavio::writeFloat(soloPath, out);
    }
    if (!soloDir.empty())
    {
        for (size_t i = 0; i < A.notes.size(); ++i)
        {
            if (!A.notes[i].active) continue;
            wavio::Audio out;
            out.sampleRate = in.sampleRate;
            for (const auto& ch : in.channels) out.channels.push_back(polyphonic::extractNote(A, ch, static_cast<int>(i)));
            wavio::writeFloat(soloDir + "/note_" + std::to_string(i) + ".wav", out);
        }
    }
    return 0;
}
