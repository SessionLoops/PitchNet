// Input-type classifier: vocal pipeline or polyphonic engine. See InputTypeClassifier.h.
// Feature definitions follow tools/polyphonic/research/vocal_probe.py, where the thresholds were measured.

#include "InputTypeClassifier.h"

#include "../Polyphonic/BasicPitchEvidence.h"
#include "YamnetVoiceTagger.h"

#include <algorithm>
#include <cmath>

namespace inputtype
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kKeys = 88;
// SwiftF0 geometry (SwiftF0PitchDetector.h), restated to keep this file free of JUCE.
constexpr double kSwiftRate = 16000.0;
constexpr double kSwiftHop = 256.0;
constexpr double kLevelWindow = 1024.0;   // 64 ms at 16 kHz

float median(std::vector<float> v)
{
    if (v.empty())
        return 0.0f;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    if (v.size() % 2 == 1)
        return v[mid];
    const float upper = v[mid];
    const float lower = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5f * (lower + upper);
}

double blockDb(const float* x, int n)
{
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(sum / std::max(1, n) + 1e-12);
}
} // namespace

const char* toString(Pipeline pipeline)
{
    return pipeline == Pipeline::Vocal ? "vocal" : "polyphonic";
}

const char* toString(Reason reason)
{
    switch (reason)
    {
        case Reason::NoAudio: return "no audible audio";
        case Reason::TooShort: return "too little audio to classify";
        case Reason::Polyphony: return "several notes at once";
        case Reason::TaggerVoice: return "tagger: voice";
        case Reason::TaggerNotVoice: return "tagger: not a voice";
        case Reason::TaggerUnsureCuesVoice: return "tagger unsure, pitch cues: voice";
        case Reason::TaggerUnsureCuesNotVoice: return "tagger unsure, pitch cues: not a voice";
        case Reason::TaggerUnsure: return "tagger unsure";
        case Reason::CuesVoice: return "no tagger, pitch cues: voice";
        case Reason::CuesNotVoice: return "no tagger, pitch cues: not a voice";
        case Reason::Undecided: return "undecided";
    }
    return "?";
}

Polyphony polyphony(const polyphonic::NoteEvidence& evidence, const Config& config)
{
    Polyphony result;
    const size_t frames = std::min(evidence.frameTimes.size(), evidence.note.size() / kKeys);
    int multi = 0;
    long total = 0;
    for (size_t f = 0; f < frames; ++f)
    {
        const float* row = evidence.note.data() + f * kKeys;
        int count = 0;
        for (int k = 0; k < kKeys; ++k)
        {
            const float v = row[k];
            if (v <= config.noteActiveThreshold)
                continue;
            const bool leftOk = k == 0 || v >= row[k - 1];
            const bool rightOk = k == kKeys - 1 || v >= row[k + 1];
            if (leftOk && rightOk)
                ++count;
        }
        if (count >= 1)
        {
            ++result.activeFrames;
            total += count;
            if (count >= 2)
                ++multi;
        }
    }
    if (result.activeFrames > 0)
    {
        result.poly2 = static_cast<float>(multi) / static_cast<float>(result.activeFrames);
        result.meanNotes = static_cast<float>(total) / static_cast<float>(result.activeFrames);
    }
    return result;
}

PitchCues pitchCuesFromVoiced(const std::vector<float>& f0Hz, const std::vector<bool>& voiced, const Config& config)
{
    PitchCues cues;
    const size_t n = std::min(f0Hz.size(), voiced.size());
    std::vector<double> cents(n, 0.0);
    for (size_t i = 0; i < n; ++i)
        cents[i] = 1200.0 * std::log2(std::max(1.0, static_cast<double>(f0Hz[i])) / 440.0);

    // Glide: adjacent voiced pairs moving between glideMinCents and glideMaxCents.
    int pairs = 0, gliding = 0;
    for (size_t i = 1; i < n; ++i)
        if (voiced[i] && voiced[i - 1])
        {
            ++pairs;
            const double d = std::fabs(cents[i] - cents[i - 1]);
            if (d > config.glideMinCents && d < config.glideMaxCents)
                ++gliding;
        }
    cues.glide = pairs > 0 ? static_cast<float>(gliding) / static_cast<float>(pairs) : 0.0f;

    // Off-grid spread: distance from the semitone grid after removing the take's
    // own tuning (circular mean over the 100-cent cycle).
    double sx = 0.0, sy = 0.0;
    for (size_t i = 0; i < n; ++i)
        if (voiced[i])
        {
            ++cues.voicedFrames;
            const double a = 2.0 * kPi * cents[i] / 100.0;
            sx += std::cos(a);
            sy += std::sin(a);
        }
    if (cues.voicedFrames > 0)
    {
        const double tuning = std::atan2(sy, sx) * 100.0 / (2.0 * kPi);
        std::vector<float> dev;
        dev.reserve(static_cast<size_t>(cues.voicedFrames));
        for (size_t i = 0; i < n; ++i)
            if (voiced[i])
            {
                double d = std::fmod(cents[i] - tuning + 50.0, 100.0);
                if (d < 0.0)
                    d += 100.0;
                dev.push_back(static_cast<float>(std::fabs(d - 50.0)));
            }
        cues.offgridCents = median(std::move(dev));
    }

    if (cues.voicedFrames < config.minVoicedFrames)
        cues.verdict = CueVerdict::TooLittlePitch;
    else if (cues.offgridCents >= config.offgridVoiceCents && cues.glide >= config.glideVoiceShare)
        cues.verdict = CueVerdict::VoiceLike;
    else
        cues.verdict = CueVerdict::NotVoiceLike;
    return cues;
}

PitchCues pitchCues(const PitchTrack& track, const float* mono, int numSamples, double sampleRate, const Config& config)
{
    if (track.f0Hz == nullptr || track.confidence == nullptr || track.numFrames <= 0 || mono == nullptr
        || numSamples <= 0 || sampleRate <= 0)
        return pitchCuesFromVoiced({}, {}, config);
    const auto frames = static_cast<size_t>(track.numFrames);
    const double hop = kSwiftHop * sampleRate / kSwiftRate;
    const int window = std::max(1, static_cast<int>(std::lround(kLevelWindow * sampleRate / kSwiftRate)));
    std::vector<double> db(frames, -240.0);
    double loudest = -240.0;
    for (size_t i = 0; i < frames; ++i)
    {
        const int start = static_cast<int>(std::lround(static_cast<double>(i) * hop));
        if (start >= numSamples)
            break;
        db[i] = blockDb(mono + start, std::min(window, numSamples - start));
        loudest = std::max(loudest, db[i]);
    }
    std::vector<float> f0(track.f0Hz, track.f0Hz + frames);
    std::vector<bool> voiced(frames, false);
    for (size_t i = 0; i < frames; ++i)
        voiced[i] = track.confidence[i] > config.voicedConfidence && db[i] > loudest - config.loudRangeDb;
    return pitchCuesFromVoiced(f0, voiced, config);
}

bool Decision::confident() const
{
    switch (reason)
    {
        case Reason::Polyphony:
        case Reason::TaggerVoice:
        case Reason::TaggerNotVoice:
            return true;
        default:
            return false;
    }
}

Decision decide(const Features& features, const Config& config)
{
    Decision d;
    d.features = features;
    auto choose = [&](Pipeline p, Reason r) {
        d.pipeline = p;
        d.reason = r;
        return d;
    };
    if (!features.audible)
        return choose(Pipeline::Polyphonic, Reason::NoAudio);
    // 1 ms of slack so that an excerpt of exactly minAudibleSeconds counts.
    if (features.audibleSeconds >= 0.0 && features.audibleSeconds + 1e-3 < config.minAudibleSeconds)
        return choose(Pipeline::Polyphonic, Reason::TooShort);

    // 1. Several notes at once: polyphonic, even if they are all voices.
    if (features.polyphonyMeasured && features.polyphony.activeFrames >= config.minNoteActiveFrames
        && features.polyphony.poly2 >= config.polyphonyThreshold)
        return choose(Pipeline::Polyphonic, Reason::Polyphony);

    const CueVerdict cues = features.cues.verdict;
    // 2. Voice identity from the tagger. Near its threshold the pitch cues can confirm a
    //    voice the tagger leans towards, never promote one it leans against.
    if (features.voiceScore >= 0.0f)
    {
        const float s = features.voiceScore;
        if (s > config.unsureHigh)
            return choose(Pipeline::Vocal, Reason::TaggerVoice);
        if (s < config.unsureLow)
            return choose(Pipeline::Polyphonic, Reason::TaggerNotVoice);
        if (cues == CueVerdict::NotVoiceLike)
            return choose(Pipeline::Polyphonic, Reason::TaggerUnsureCuesNotVoice);
        if (cues == CueVerdict::VoiceLike && s >= config.voiceThreshold)
            return choose(Pipeline::Vocal, Reason::TaggerUnsureCuesVoice);
        return choose(Pipeline::Polyphonic, Reason::TaggerUnsure);
    }

    // 3. No tagger: the pitch cues alone.
    if (cues == CueVerdict::VoiceLike)
        return choose(Pipeline::Vocal, Reason::CuesVoice);
    if (cues == CueVerdict::NotVoiceLike)
        return choose(Pipeline::Polyphonic, Reason::CuesNotVoice);
    return choose(Pipeline::Polyphonic, Reason::Undecided);
}

std::vector<float> selectExcerpt(const float* mono, int numSamples, double sampleRate, const Config& config)
{
    std::vector<float> out;
    if (mono == nullptr || numSamples <= 0 || sampleRate <= 0)
        return out;
    const int block = std::max(1, static_cast<int>(std::lround(config.excerptBlockSeconds * sampleRate)));
    const int blocks = (numSamples + block - 1) / block;
    std::vector<double> db(static_cast<size_t>(blocks));
    double loudest = -240.0;
    for (int b = 0; b < blocks; ++b)
    {
        const int start = b * block;
        db[static_cast<size_t>(b)] = blockDb(mono + start, std::min(block, numSamples - start));
        loudest = std::max(loudest, db[static_cast<size_t>(b)]);
    }
    const double floorDb = std::max(loudest - config.excerptRangeDb, static_cast<double>(config.silenceDbfs));
    const auto limit = static_cast<size_t>(std::max(0L, std::lround(config.excerptSeconds * sampleRate)));
    for (int b = 0; b < blocks && out.size() < limit; ++b)
    {
        if (db[static_cast<size_t>(b)] < floorDb)
            continue;
        const int start = b * block;
        const auto n = std::min(static_cast<size_t>(std::min(block, numSamples - start)), limit - out.size());
        out.insert(out.end(), mono + start, mono + start + n);
    }
    return out;
}

Decision classify(const float* excerpt, int numSamples, double sampleRate, const PitchTrack& track,
                  polyphonic::BasicPitchEvidence* basicPitch, YamnetVoiceTagger* tagger,
                  const Config& config, const std::atomic<bool>* cancel)
{
    Features f;
    if (excerpt == nullptr || numSamples <= 0 || sampleRate <= 0)
    {
        f.audible = false;
        return decide(f, config);
    }
    f.audibleSeconds = static_cast<double>(numSamples) / sampleRate;
    if (f.audibleSeconds + 1e-3 < config.minAudibleSeconds)
        return decide(f, config);
    auto cancelled = [&] { return cancel != nullptr && cancel->load(); };

    if (basicPitch != nullptr && basicPitch->isLoaded())
    {
        const auto evidence = basicPitch->compute(excerpt, numSamples, sampleRate, cancel);
        if (cancelled())
            return {};
        if (!evidence.empty())
        {
            f.polyphony = polyphony(evidence, config);
            f.polyphonyMeasured = true;
            // Polyphonic either way: skip the tagger.
            if (f.polyphony.activeFrames >= config.minNoteActiveFrames && f.polyphony.poly2 >= config.polyphonyThreshold)
                return decide(f, config);
        }
    }
    if (tagger != nullptr && tagger->isLoaded())
    {
        f.voiceScore = tagger->voiceScore(excerpt, numSamples, sampleRate, cancel);
        if (cancelled())
            return {};
    }
    f.cues = pitchCues(track, excerpt, numSamples, sampleRate, config);
    return decide(f, config);
}

} // namespace inputtype
