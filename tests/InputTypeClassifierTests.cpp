// Input-type classifier (Source/Audio/Analysis): features, decision rules, excerpt selection and,
// given model paths, YAMNet and Basic Pitch on real audio. No JUCE dependency.
//
//   InputTypeClassifierTests
//   InputTypeClassifierTests <yamnet.onnx> [<basic_pitch_nmp.onnx> [vocal=<file.wav> | polyphonic=<file.wav> ...]]
//
// With labelled WAV files, each one must be routed to its label using Basic Pitch and YAMNet
// (no SwiftF0 here: the pitch cues abstain).
#include "../Source/Audio/Analysis/InputTypeClassifier.h"
#include "../Source/Audio/Analysis/YamnetVoiceTagger.h"
#include "../Source/Audio/Polyphonic/BasicPitchEvidence.h"
#include "../tools/polyphonic/WavIO.h"
#include "TestAssert.h"

#if defined(HAVE_ONNXRUNTIME) && defined(ORT_API_MANUAL_INIT)
#include <onnxruntime_cxx_api.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{
using namespace inputtype;
constexpr double kPi = 3.14159265358979323846;
constexpr int kKeys = 88;
constexpr double kFrameSeconds = 256.0 / 16000.0;

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

polyphonic::NoteEvidence evidence(int frames)
{
    polyphonic::NoteEvidence ev;
    for (int f = 0; f < frames; ++f)
        ev.frameTimes.push_back(static_cast<float>(f * 256.0 / 22050.0));
    ev.note.assign(static_cast<size_t>(frames) * kKeys, 0.0f);
    return ev;
}

void setNote(polyphonic::NoteEvidence& ev, int frame, int key, float v)
{
    ev.note[static_cast<size_t>(frame) * kKeys + static_cast<size_t>(key)] = v;
}

void testPolyphony()
{
    CHECK(polyphony(polyphonic::NoteEvidence{}).activeFrames == 0);
    CHECK(polyphony(polyphonic::NoteEvidence{}).poly2 == 0.0f);

    // One note smeared into its upper neighbour is still one note.
    auto mono = evidence(100);
    for (int f = 0; f < 100; ++f)
    {
        setNote(mono, f, 40, 0.9f);
        setNote(mono, f, 41, 0.6f);
    }
    const auto m = polyphony(mono);
    CHECK(m.activeFrames == 100);
    CHECK(m.poly2 == 0.0f);
    CHECK(m.meanNotes == 1.0f);

    // A plateau of two equal neighbours counts both (as the probe does).
    auto plateau = evidence(1);
    setNote(plateau, 0, 10, 0.8f);
    setNote(plateau, 0, 11, 0.8f);
    CHECK(polyphony(plateau).poly2 == 1.0f);

    // Keys at both ends of the range count; nothing wraps around.
    auto edges = evidence(1);
    setNote(edges, 0, 0, 0.7f);
    setNote(edges, 0, kKeys - 1, 0.9f);
    CHECK(polyphony(edges).meanNotes == 2.0f);

    // 30 of 100 active frames hold a chord; 50 silent frames do not count.
    auto chords = evidence(150);
    for (int f = 0; f < 100; ++f)
    {
        setNote(chords, f, 39, 0.8f);
        if (f < 30)
        {
            setNote(chords, f, 43, 0.7f);
            setNote(chords, f, 46, 0.7f);
        }
    }
    for (int f = 100; f < 150; ++f)
        setNote(chords, f, 50, 0.5f);   // at the threshold: not active
    const auto c = polyphony(chords);
    CHECK(c.activeFrames == 100);
    CHECK(near(c.poly2, 0.30, 1e-6));
    CHECK(near(c.meanNotes, (70 * 1 + 30 * 3) / 100.0, 1e-6));

    // Malformed evidence (fewer note rows than times) reads only whole rows.
    auto shortRows = evidence(10);
    shortRows.note.resize(5 * kKeys + 3);
    setNote(shortRows, 0, 20, 0.9f);
    CHECK(polyphony(shortRows).activeFrames == 1);
}

// Pitch tracks at SwiftF0's frame rate.
std::vector<float> steppedNotes(int frames, double detuneCents, double jitterCents, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> jitter(0.0, jitterCents);
    const int notes[] = {0, 2, 4, 5, 7, 9, 11, 12};
    std::vector<float> f0(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        const double cents = 100.0 * notes[(i / 40) % 8] + detuneCents + jitter(rng);
        f0[static_cast<size_t>(i)] = static_cast<float>(261.63 * std::pow(2.0, cents / 1200.0));
    }
    return f0;
}

std::vector<float> sungPhrase(int frames)
{
    // Notes joined by 60 ms portamento, 5.5 Hz vibrato of +-40 cents, a little drift.
    std::vector<float> f0(static_cast<size_t>(frames));
    const int notes[] = {0, 3, 5, 7, 5, 3, 0, -2};
    for (int i = 0; i < frames; ++i)
    {
        const int n = i / 50, pos = i % 50;
        const double a = 100.0 * notes[n % 8], b = 100.0 * notes[(n + 1) % 8];
        const double glide = pos >= 46 ? (pos - 46) / 4.0 : 0.0;
        const double t = i * kFrameSeconds;
        const double cents = a + (b - a) * glide + 40.0 * std::sin(2 * kPi * 5.5 * t) + 8.0 * std::sin(2 * kPi * 0.3 * t);
        f0[static_cast<size_t>(i)] = static_cast<float>(220.0 * std::pow(2.0, cents / 1200.0));
    }
    return f0;
}

void testPitchCues()
{
    const std::vector<bool> all(400, true);

    // In-tune instrument: on the grid, holds its notes.
    const auto inTune = pitchCuesFromVoiced(steppedNotes(400, 0.0, 1.0, 1), all);
    CHECK(inTune.voicedFrames == 400);
    CHECK(inTune.offgridCents < 3.0f);
    CHECK(inTune.glide < 0.03f);
    CHECK(inTune.verdict == CueVerdict::NotVoiceLike);

    // The take's own tuning is removed: 30 cents sharp throughout is still on its grid,
    // and so is 50 cents (the worst case for a fixed A440 grid).
    for (double detune : {30.0, -30.0, 50.0})
    {
        const auto detuned = pitchCuesFromVoiced(steppedNotes(400, detune, 1.0, 2), all);
        CHECK(detuned.offgridCents < 3.0f);
        CHECK(detuned.verdict == CueVerdict::NotVoiceLike);
    }

    // Singing: vibrato and portamento.
    const auto sung = pitchCuesFromVoiced(sungPhrase(400), all);
    CHECK(sung.offgridCents >= 11.0f);
    CHECK(sung.glide >= 0.05f);
    CHECK(sung.verdict == CueVerdict::VoiceLike);

    // The cues are weak: a pitch alternating by 25 cents frame to frame is off-grid and
    // "glides", so it passes. Raising glideMinCents above the step rejects it.
    auto quarterTone = steppedNotes(400, 0.0, 0.5, 3);
    for (size_t i = 0; i < quarterTone.size(); i += 2)
        quarterTone[i] *= static_cast<float>(std::pow(2.0, 25.0 / 1200.0));
    const auto steadyOff = pitchCuesFromVoiced(quarterTone, all);
    CHECK(steadyOff.offgridCents >= 11.0f);
    CHECK(steadyOff.glide > 0.05f);   // 25-cent alternation is a "glide" frame to frame
    Config strictGlide;
    strictGlide.glideMinCents = 30.0f;
    CHECK(pitchCuesFromVoiced(quarterTone, all, strictGlide).verdict == CueVerdict::NotVoiceLike);

    // Unvoiced frames are ignored entirely, including for glide pairs across a gap.
    auto gapped = steppedNotes(400, 0.0, 1.0, 4);
    std::vector<bool> voiced(400, true);
    for (int i = 100; i < 300; ++i)
    {
        voiced[static_cast<size_t>(i)] = false;
        gapped[static_cast<size_t>(i)] = 333.0f + static_cast<float>(i);   // garbage pitch
    }
    const auto g = pitchCuesFromVoiced(gapped, voiced);
    CHECK(g.voicedFrames == 200);
    CHECK(g.offgridCents < 3.0f);
    CHECK(g.glide < 0.03f);

    // Too little pitch abstains.
    const std::vector<bool> few(29, true);
    CHECK(pitchCuesFromVoiced(sungPhrase(29), few).verdict == CueVerdict::TooLittlePitch);
    CHECK(pitchCuesFromVoiced({}, {}).verdict == CueVerdict::TooLittlePitch);
    CHECK(pitchCuesFromVoiced({}, {}).offgridCents == 0.0f);
}

void testPitchCuesFromTrack()
{
    // 4 s at 44.1 kHz: a sung tone for 2 s, then -60 dB hiss that SwiftF0 calls voiced.
    constexpr int rate = 44100, frames = 250;
    std::vector<float> audio(static_cast<size_t>(4 * rate));
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.001f);
    for (size_t i = 0; i < audio.size(); ++i)
        audio[i] = i < static_cast<size_t>(2 * rate) ? 0.3f * static_cast<float>(std::sin(2 * kPi * 220.0 * i / rate)) : noise(rng);

    auto f0 = sungPhrase(frames);
    for (int i = 125; i < frames; ++i)
        f0[static_cast<size_t>(i)] = 440.0f;    // the hiss's "pitch": exactly on the grid
    std::vector<float> conf(frames, 0.95f);
    conf[10] = 0.5f;                            // below voicedConfidence

    const PitchTrack track{f0.data(), conf.data(), frames};
    const auto cues = pitchCues(track, audio.data(), static_cast<int>(audio.size()), rate);
    // 125 loud frames minus the low-confidence one; the quiet second half is not voiced.
    CHECK(cues.voicedFrames == 124);
    CHECK(cues.verdict == CueVerdict::VoiceLike);

    Config wide;
    wide.loudRangeDb = 120.0f;                  // count the hiss too
    CHECK(pitchCues(track, audio.data(), static_cast<int>(audio.size()), rate, wide).voicedFrames == 249);

    // A track longer than the audio: frames past the end are not voiced.
    std::vector<float> longF0 = sungPhrase(frames * 2), longConf(frames * 2, 0.95f);
    const PitchTrack longTrack{longF0.data(), longConf.data(), frames * 2};
    CHECK(pitchCues(longTrack, audio.data(), 2 * rate, rate).voicedFrames <= 126);

    // Missing pieces abstain rather than crash.
    CHECK(pitchCues(PitchTrack{}, audio.data(), 100, rate).verdict == CueVerdict::TooLittlePitch);
    CHECK(pitchCues(track, nullptr, 0, rate).verdict == CueVerdict::TooLittlePitch);
}

Features voiceFeatures(float score, CueVerdict cues)
{
    Features f;
    f.polyphonyMeasured = true;
    f.polyphony.activeFrames = 500;
    f.polyphony.poly2 = 0.02f;
    f.voiceScore = score;
    f.cues.verdict = cues;
    return f;
}

void testDecide()
{
    // Nothing audible.
    Features silent;
    silent.audible = false;
    silent.voiceScore = 0.9f;
    CHECK(decide(silent).reason == Reason::NoAudio);
    CHECK(decide(silent).pipeline == Pipeline::Polyphonic);

    // The polyphony gate beats a confident voice: stacked harmonies are polyphonic.
    auto choir = voiceFeatures(0.9f, CueVerdict::VoiceLike);
    choir.polyphony.poly2 = 0.15f;
    CHECK(decide(choir).pipeline == Pipeline::Polyphonic);
    CHECK(decide(choir).reason == Reason::Polyphony);
    CHECK(decide(choir).confident());
    choir.polyphony.poly2 = 0.149f;
    CHECK(decide(choir).pipeline == Pipeline::Vocal);

    // Too few note-active frames to trust the gate.
    auto blip = voiceFeatures(0.9f, CueVerdict::VoiceLike);
    blip.polyphony.poly2 = 1.0f;
    blip.polyphony.activeFrames = 9;
    CHECK(decide(blip).reason == Reason::TaggerVoice);
    blip.polyphony.activeFrames = 10;
    CHECK(decide(blip).reason == Reason::Polyphony);

    // Without Basic Pitch the gate is skipped.
    auto noGate = voiceFeatures(0.9f, CueVerdict::VoiceLike);
    noGate.polyphony.poly2 = 1.0f;
    noGate.polyphonyMeasured = false;
    CHECK(decide(noGate).reason == Reason::TaggerVoice);

    // A clear tagger score overrides the pitch cues in both directions: a violin's
    // vibrato does not make it a voice, an in-tune singer is still a voice.
    CHECK(decide(voiceFeatures(0.6f, CueVerdict::NotVoiceLike)).pipeline == Pipeline::Vocal);
    CHECK(decide(voiceFeatures(0.6f, CueVerdict::NotVoiceLike)).reason == Reason::TaggerVoice);
    CHECK(decide(voiceFeatures(0.001f, CueVerdict::VoiceLike)).pipeline == Pipeline::Polyphonic);
    CHECK(decide(voiceFeatures(0.001f, CueVerdict::VoiceLike)).reason == Reason::TaggerNotVoice);
    CHECK(decide(voiceFeatures(0.0f, CueVerdict::VoiceLike)).reason == Reason::TaggerNotVoice);
    // The measured extremes: lowest voice 0.156, highest non-voice 0.017.
    CHECK(decide(voiceFeatures(0.156f, CueVerdict::TooLittlePitch)).pipeline == Pipeline::Vocal);
    CHECK(decide(voiceFeatures(0.017f, CueVerdict::TooLittlePitch)).pipeline == Pipeline::Polyphonic);

    // Inside the unsure band [0.025, 0.10] it leans polyphonic: voice-like pitch cues
    // only confirm a score at or above voiceThreshold.
    for (float s : {0.05f, 0.07f, 0.10f})
    {
        const auto v = decide(voiceFeatures(s, CueVerdict::VoiceLike));
        CHECK(v.pipeline == Pipeline::Vocal);
        CHECK(v.reason == Reason::TaggerUnsureCuesVoice);
        CHECK(!v.confident());
    }
    for (float s : {0.025f, 0.035f, 0.049f})
    {
        const auto v = decide(voiceFeatures(s, CueVerdict::VoiceLike));
        CHECK(v.pipeline == Pipeline::Polyphonic);
        CHECK(v.reason == Reason::TaggerUnsure);
    }
    for (float s : {0.025f, 0.05f, 0.10f})
    {
        const auto n = decide(voiceFeatures(s, CueVerdict::NotVoiceLike));
        CHECK(n.pipeline == Pipeline::Polyphonic);
        CHECK(n.reason == Reason::TaggerUnsureCuesNotVoice);
        // Too little pitch: no confirmation, so polyphonic even above the threshold.
        const auto t = decide(voiceFeatures(s, CueVerdict::TooLittlePitch));
        CHECK(t.pipeline == Pipeline::Polyphonic);
        CHECK(t.reason == Reason::TaggerUnsure);
        CHECK(!t.confident());
    }

    // Under minAudibleSeconds nothing is trusted, however clear the features.
    auto shortVoice = voiceFeatures(0.9f, CueVerdict::VoiceLike);
    shortVoice.audibleSeconds = 2.9;
    CHECK(decide(shortVoice).pipeline == Pipeline::Polyphonic);
    CHECK(decide(shortVoice).reason == Reason::TooShort);
    CHECK(!decide(shortVoice).confident());
    shortVoice.audibleSeconds = 3.0;
    CHECK(decide(shortVoice).reason == Reason::TaggerVoice);
    shortVoice.audibleSeconds = -1.0;   // not measured: no length check
    CHECK(decide(shortVoice).reason == Reason::TaggerVoice);

    // No tagger (score < 0): pitch cues alone, polyphonic when they abstain.
    CHECK(decide(voiceFeatures(-1.0f, CueVerdict::VoiceLike)).reason == Reason::CuesVoice);
    CHECK(decide(voiceFeatures(-1.0f, CueVerdict::VoiceLike)).pipeline == Pipeline::Vocal);
    CHECK(!decide(voiceFeatures(-1.0f, CueVerdict::VoiceLike)).confident());
    CHECK(decide(voiceFeatures(-1.0f, CueVerdict::NotVoiceLike)).reason == Reason::CuesNotVoice);
    CHECK(decide(voiceFeatures(-1.0f, CueVerdict::TooLittlePitch)).reason == Reason::Undecided);
    CHECK(decide(voiceFeatures(-1.0f, CueVerdict::TooLittlePitch)).pipeline == Pipeline::Polyphonic);

    // Nothing measured at all.
    CHECK(decide(Features{}).pipeline == Pipeline::Polyphonic);
    CHECK(decide(Features{}).reason == Reason::Undecided);

    // Features are carried into the decision; every reason has a name.
    CHECK(decide(voiceFeatures(0.3f, CueVerdict::VoiceLike)).features.voiceScore == 0.3f);
    for (int r = 0; r <= static_cast<int>(Reason::Undecided); ++r)
        CHECK(std::strcmp(toString(static_cast<Reason>(r)), "?") != 0);
    CHECK(std::strcmp(toString(Pipeline::Vocal), "vocal") == 0);
}

void testSelectExcerpt()
{
    constexpr int rate = 1000;   // 0.5 s blocks of 500 samples
    Config c;
    CHECK(selectExcerpt(nullptr, 10, rate, c).empty());
    std::vector<float> silence(10 * rate, 0.0f);
    CHECK(selectExcerpt(silence.data(), static_cast<int>(silence.size()), rate, c).empty());

    // 1 s silence, 1 s tone, 1 s at -50 dB (more than 40 dB down), 1 s tone; the
    // two tones' blocks are kept, in order.
    std::vector<float> x(4 * rate, 0.0f);
    for (int i = 0; i < 4 * rate; ++i)
    {
        const float tone = static_cast<float>(std::sin(2 * kPi * 50.0 * i / rate));
        if (i >= rate && i < 2 * rate)
            x[static_cast<size_t>(i)] = 0.5f * tone;
        else if (i >= 2 * rate && i < 3 * rate)
            x[static_cast<size_t>(i)] = 0.5f * 0.00316f * tone;
        else if (i >= 3 * rate)
            x[static_cast<size_t>(i)] = -0.5f * tone;
    }
    const auto e = selectExcerpt(x.data(), static_cast<int>(x.size()), rate, c);
    CHECK(e.size() == static_cast<size_t>(2 * rate));
    CHECK(e[0] == x[static_cast<size_t>(rate)]);
    CHECK(e[static_cast<size_t>(rate) + 7] == x[static_cast<size_t>(3 * rate) + 7]);

    // A quiet but uniform recording is kept whole: the range is relative to its loudest block...
    std::vector<float> quiet(x.size());
    for (size_t i = 0; i < quiet.size(); ++i)
        quiet[i] = 0.001f * static_cast<float>(std::sin(2 * kPi * 50.0 * i / rate));   // -63 dBFS
    CHECK(selectExcerpt(quiet.data(), static_cast<int>(quiet.size()), rate, c).size() == quiet.size());
    // ...down to the absolute silence floor.
    for (auto& v : quiet)
        v *= 0.01f;   // -103 dBFS
    CHECK(selectExcerpt(quiet.data(), static_cast<int>(quiet.size()), rate, c).empty());

    // The length limit cuts mid-block; a partial final block is kept.
    c.excerptSeconds = 1.2;
    CHECK(selectExcerpt(x.data(), static_cast<int>(x.size()), rate, c).size() == 1200);
    Config d;
    const auto tail = selectExcerpt(x.data(), rate + 100, rate, d);   // 1.1 s: the tone's first 100 samples
    CHECK(tail.size() == 100);
}

void testClassifyWithoutModels()
{
    constexpr int rate = 16000;
    const int frames = 400;
    std::vector<float> audio(static_cast<size_t>(frames * 256 + 1024));
    for (size_t i = 0; i < audio.size(); ++i)
        audio[i] = 0.3f * static_cast<float>(std::sin(2 * kPi * 220.0 * i / rate));
    auto f0 = sungPhrase(frames);
    std::vector<float> conf(frames, 0.95f);
    const PitchTrack track{f0.data(), conf.data(), frames};

    // Unloaded models are skipped.
    polyphonic::BasicPitchEvidence bp;
    YamnetVoiceTagger tagger;
    CHECK(!tagger.isLoaded());
    CHECK(tagger.voiceScore(audio.data(), 100, rate) < 0.0f);
    const auto d = classify(audio.data(), static_cast<int>(audio.size()), rate, track, &bp, &tagger);
    CHECK(!d.features.polyphonyMeasured);
    CHECK(d.features.voiceScore < 0.0f);
    CHECK(d.reason == Reason::CuesVoice);
    CHECK(d.pipeline == Pipeline::Vocal);
    CHECK(classify(audio.data(), static_cast<int>(audio.size()), rate, track, nullptr, nullptr).reason == Reason::CuesVoice);

    auto flat = steppedNotes(frames, 0.0, 1.0, 9);
    const PitchTrack flatTrack{flat.data(), conf.data(), frames};
    CHECK(classify(audio.data(), static_cast<int>(audio.size()), rate, flatTrack, nullptr, nullptr).reason == Reason::CuesNotVoice);

    CHECK(classify(nullptr, 0, rate, track, nullptr, nullptr).reason == Reason::NoAudio);
    // Under 3 s of audio: not classified, models not run.
    const auto tooShort = classify(audio.data(), 3 * rate - 100, rate, track, &bp, &tagger);
    CHECK(tooShort.reason == Reason::TooShort);
    CHECK(tooShort.pipeline == Pipeline::Polyphonic);
    CHECK(near(tooShort.features.audibleSeconds, (3.0 * rate - 100) / rate, 1e-9));
    CHECK(classify(audio.data(), 3 * rate, rate, track, nullptr, nullptr).reason == Reason::CuesVoice);
    CHECK(classify(audio.data(), static_cast<int>(audio.size()), rate, PitchTrack{}, nullptr, nullptr).reason == Reason::Undecided);

    // voiceScore over a score vector: the maximum over the voice classes only.
    CHECK(YamnetVoiceTagger::voiceClasses().size() == 15);
    std::vector<float> scores(YamnetVoiceTagger::kNumClasses, 0.0f);
    scores[137] = 0.99f;   // Music
    scores[28] = 0.2f;     // Mantra
    scores[3] = 0.1f;      // Narration
    CHECK(YamnetVoiceTagger::voiceScore(scores) == 0.2f);
    CHECK(YamnetVoiceTagger::voiceScore(std::vector<float>(10, 1.0f)) == -1.0f);
}

void testModels(int argc, char** argv)
{
#if defined(HAVE_ONNXRUNTIME) && defined(ORT_API_MANUAL_INIT)
    // The main build on macOS resolves the API at run time (OnnxRuntime::initialise(), which needs
    // JUCE). This test links ONNX Runtime directly, so take the API from the linked library.
    Ort::InitApi(OrtGetApiBase()->GetApi(ORT_API_VERSION));
#endif
    YamnetVoiceTagger tagger;
    CHECK(tagger.load(argv[1]));
    CHECK(tagger.lastError().empty());

    // Synthetic non-voices score far below the threshold.
    constexpr int rate = 44100;
    std::vector<float> sine(3 * rate), noise(3 * rate);
    std::mt19937 rng(11);
    std::normal_distribution<float> gauss(0.0f, 0.2f);
    for (size_t i = 0; i < sine.size(); ++i)
    {
        sine[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * 1000.0 * i / rate));
        noise[i] = gauss(rng);
    }
    const auto s = tagger.scores(sine.data(), static_cast<int>(sine.size()), rate);
    CHECK(s.size() == static_cast<size_t>(YamnetVoiceTagger::kNumClasses));
    std::printf("YAMNet voice score: 1 kHz sine %.4f, white noise %.4f\n", YamnetVoiceTagger::voiceScore(s),
                tagger.voiceScore(noise.data(), static_cast<int>(noise.size()), rate));
    CHECK(YamnetVoiceTagger::voiceScore(s) < 0.025f);
    CHECK(tagger.voiceScore(noise.data(), static_cast<int>(noise.size()), rate) < 0.025f);
    // Shorter than one patch still scores (zero-padded inside the model); classify() would not
    // ask for it (minAudibleSeconds), but the tagger itself must not fail.
    CHECK(tagger.voiceScore(sine.data(), 2000, rate) >= 0.0f);
    const std::atomic<bool> cancel{true};
    CHECK(tagger.scores(sine.data(), static_cast<int>(sine.size()), rate, &cancel).empty());

    polyphonic::BasicPitchEvidence bp;
    const bool haveBp = argc > 2 && bp.load(argv[2]);
    if (argc > 2)
        CHECK(haveBp);

    int failures = 0;
    for (int a = 3; a < argc; ++a)
    {
        const std::string arg = argv[a];
        const auto eq = arg.find('=');
        CHECK(eq != std::string::npos);
        const std::string label = arg.substr(0, eq), path = arg.substr(eq + 1);
        CHECK(label == "vocal" || label == "polyphonic");
        wavio::Audio wav;
        std::string err;
        CHECK(wavio::read(path, wav, &err));
        std::vector<float> mono(static_cast<size_t>(wav.numSamples()), 0.0f);
        for (const auto& ch : wav.channels)
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] += ch[i] / static_cast<float>(wav.channels.size());
        const auto excerpt = selectExcerpt(mono.data(), static_cast<int>(mono.size()), wav.sampleRate);
        const auto d = classify(excerpt.data(), static_cast<int>(excerpt.size()), wav.sampleRate, PitchTrack{},
                                haveBp ? &bp : nullptr, &tagger);
        const bool ok = std::string(toString(d.pipeline)) == label;
        failures += ok ? 0 : 1;
        std::printf("%s %-10s poly2 %.3f voice %.3f -> %-10s (%s)  %s\n", ok ? "ok  " : "FAIL", label.c_str(),
                    d.features.polyphonyMeasured ? d.features.polyphony.poly2 : -1.0f, d.features.voiceScore,
                    toString(d.pipeline), toString(d.reason), path.c_str());
    }
    CHECK(failures == 0);
}
} // namespace

int main(int argc, char** argv)
{
    testPolyphony();
    testPitchCues();
    testPitchCuesFromTrack();
    testDecide();
    testSelectExcerpt();
    testClassifyWithoutModels();
    if (argc > 1)
        testModels(argc, argv);
    std::puts("Input type classifier tests passed");
    return 0;
}
