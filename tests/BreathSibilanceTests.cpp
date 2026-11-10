#include "../Source/Audio/BreathNotes.h"
#include "../Source/Audio/BreathSibilanceAnalysis.h"
#include "../Source/Audio/BreathSibilanceDetector.h"
#include "../Source/Audio/NoteEdges.h"
#include "../Source/Audio/NoteTiming.h"
#include "../Source/Audio/ProjectGain.h"
#include "../Source/Audio/Synthesis/BreathSibilanceProtection.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Models/ProjectTimelineOffset.h"
#include "../Source/Undo/GainEditAction.h"
#include "TestAssert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

// Synthetic takes for the breath/sibilance detector. Each segment is written
// with its own voicing so the detector sees what a pitch detector would
// report; the tests then check the labels inside every segment.

namespace
{
constexpr double kRate = 44100.0;
constexpr int kHop = 512;
constexpr double kPi = 3.14159265358979323846;

using breathsibilance::Breath;
using breathsibilance::None;
using breathsibilance::Sibilance;

struct Take
{
  std::vector<float> samples;
  std::vector<bool> voiced; // per frame
  std::mt19937 rng{1234};

  int frames() const { return static_cast<int>(samples.size()) / kHop + 1; }

  // Frame range [first, last) whose centres fall inside [start, end) samples.
  static std::pair<int, int> framesOf(int start, int end)
  {
    return {(start + kHop - 1) / kHop, (end + kHop - 1) / kHop};
  }

  void markVoiced(int start, int end)
  {
    voiced.resize(static_cast<size_t>(frames()), false);
    const auto [first, last] = framesOf(start, end);
    for (int f = first; f < last; ++f)
      voiced[static_cast<size_t>(f)] = true;
  }

  // Steady sung vowel: harmonics with formant-like amplitudes.
  std::pair<int, int> vowel(double seconds, double f0, float amplitude)
  {
    const int start = static_cast<int>(samples.size());
    const int count = static_cast<int>(seconds * kRate);
    for (int i = 0; i < count; ++i)
    {
      const double t = i / kRate;
      double value = 0.0;
      for (int h = 1; h * f0 < 5000.0; ++h)
      {
        const double f = h * f0;
        const double formant = std::exp(-std::pow((f - 700.0) / 400.0, 2.0)) +
                               0.5 * std::exp(-std::pow((f - 1200.0) / 500.0, 2.0)) +
                               0.08;
        value += formant / h * std::sin(2.0 * kPi * f * t);
      }
      samples.push_back(static_cast<float>(amplitude * value));
    }
    markVoiced(start, start + count);
    return {start, start + count};
  }

  // White noise through a resonator bank: the shape of a fricative or a
  // breath, depending on the centre frequencies.
  std::pair<int, int> noise(double seconds, std::vector<double> centresHz,
                            double bandwidthHz, float amplitude)
  {
    const int start = static_cast<int>(samples.size());
    const int count = static_cast<int>(seconds * kRate);
    std::normal_distribution<double> white(0.0, 1.0);
    struct Resonator
    {
      double a1, a2, gain, y1 = 0.0, y2 = 0.0;
    };
    std::vector<Resonator> bank;
    for (double centre : centresHz)
    {
      const double r = std::exp(-kPi * bandwidthHz / kRate);
      bank.push_back({2.0 * r * std::cos(2.0 * kPi * centre / kRate), -r * r,
                      1.0 - r});
    }
    for (int i = 0; i < count; ++i)
    {
      const double x = white(rng);
      double value = 0.0;
      for (auto &res : bank)
      {
        const double y = res.gain * x + res.a1 * res.y1 + res.a2 * res.y2;
        res.y2 = res.y1;
        res.y1 = y;
        value += y;
      }
      // Short fades so the burst has no click at either end.
      const double fade = std::min({1.0, i / 220.0, (count - 1 - i) / 220.0});
      samples.push_back(static_cast<float>(amplitude * fade * value));
    }
    voiced.resize(static_cast<size_t>(frames()), false);
    return {start, start + count};
  }

  std::pair<int, int> silence(double seconds)
  {
    const int start = static_cast<int>(samples.size());
    const int count = static_cast<int>(seconds * kRate);
    // A quiet room rather than digital zero, like a real take.
    std::normal_distribution<double> hiss(0.0, 1.0);
    for (int i = 0; i < count; ++i)
      samples.push_back(static_cast<float>(2.0e-5 * hiss(rng)));
    voiced.resize(static_cast<size_t>(frames()), false);
    return {start, start + count};
  }

  std::vector<std::uint8_t> detect(float gain = 1.0f) const
  {
    std::vector<float> scaled(samples);
    for (auto &s : scaled)
      s *= gain;
    auto mask = voiced;
    mask.resize(static_cast<size_t>(frames()), false);
    return breathsibilance::detect(scaled.data(),
                                   static_cast<int>(scaled.size()), kRate,
                                   kHop, frames(), mask);
  }
};

// Share of the frames well inside [start, end) samples carrying `label`.
// The frames within `marginFrames` of either edge are skipped: they
// straddle the segment boundary.
double shareOf(const std::vector<std::uint8_t> &labels,
               std::pair<int, int> span, std::uint8_t label,
               int marginFrames = 2)
{
  auto [first, last] = Take::framesOf(span.first, span.second);
  first += marginFrames;
  last -= marginFrames;
  if (last <= first)
    return 0.0;
  int hits = 0;
  for (int f = first; f < last; ++f)
    hits += labels[static_cast<size_t>(f)] == label ? 1 : 0;
  return static_cast<double>(hits) / (last - first);
}

int countAll(const std::vector<std::uint8_t> &labels, std::uint8_t label)
{
  int count = 0;
  for (auto value : labels)
    count += value == label ? 1 : 0;
  return count;
}

int countVoicedNoise(const std::vector<std::uint8_t> &labels,
                     const std::vector<bool> &voiced)
{
  int count = 0;
  for (size_t f = 0; f < labels.size() && f < voiced.size(); ++f)
    count += voiced[f] && labels[f] != None ? 1 : 0;
  return count;
}

// Frame masks for the note-edge tests, painted run by run.
struct EdgeFrames
{
  int count;
  std::vector<bool> voiced, vad;
  std::vector<std::uint8_t> labels;

  explicit EdgeFrames(int frames)
      : count(frames), voiced(static_cast<size_t>(frames), false),
        vad(static_cast<size_t>(frames), false),
        labels(static_cast<size_t>(frames), None)
  {
  }
  EdgeFrames &paint(int start, int end, bool isVoiced, bool hasEnergy,
                    std::uint8_t label)
  {
    for (int f = start; f < end; ++f)
    {
      voiced[static_cast<size_t>(f)] = isVoiced;
      vad[static_cast<size_t>(f)] = hasEnergy;
      labels[static_cast<size_t>(f)] = label;
    }
    return *this;
  }
  EdgeFrames &vowel(int s, int e) { return paint(s, e, true, true, None); }
  EdgeFrames &sibilant(int s, int e) { return paint(s, e, false, true, Sibilance); }
  EdgeFrames &quietSibilant(int s, int e) { return paint(s, e, false, false, Sibilance); }
  EdgeFrames &plosive(int s, int e) { return paint(s, e, false, true, None); }
  EdgeFrames &breath(int s, int e) { return paint(s, e, false, true, Breath); }

  std::vector<noteedges::Span> refine(std::vector<noteedges::Span> notes) const
  {
    return noteedges::attachConsonants(std::move(notes), voiced, vad, labels,
                                       count);
  }
};

bool sameSpans(const std::vector<noteedges::Span> &actual,
               const std::vector<noteedges::Span> &expected)
{
  bool same = actual.size() == expected.size();
  for (size_t i = 0; same && i < actual.size(); ++i)
    same = actual[i].start == expected[i].start &&
           actual[i].end == expected[i].end;
  if (!same)
  {
    std::printf("  got:");
    for (const auto &s : actual)
      std::printf(" [%d,%d)", s.start, s.end);
    std::printf("\n  expected:");
    for (const auto &s : expected)
      std::printf(" [%d,%d)", s.start, s.end);
    std::printf("\n");
  }
  return same;
}

void noteEdgeTests()
{
  // An "s" between two vowels that GAME left at the end of the first note
  // moves to the note it leads into.
  CHECK(sameSpans(EdgeFrames(80).vowel(10, 30).sibilant(30, 36).vowel(36, 60)
                      .refine({{10, 36}, {36, 60}}),
                  {{10, 30}, {30, 60}}));

  // A word-final "s" before an inhale stays with its note; the breath GAME
  // put in the note is cut off, and the next note does not reach across it.
  CHECK(sameSpans(EdgeFrames(100).vowel(10, 30).sibilant(30, 35)
                      .breath(38, 58).vowel(62, 90)
                      .refine({{10, 40}, {60, 90}}),
                  {{10, 38}, {60, 90}}));

  // A pause between a coda and the next onset splits them: each "s" goes
  // to its own vowel.
  CHECK(sameSpans(EdgeFrames(80).vowel(10, 30).sibilant(30, 34)
                      .sibilant(44, 48).vowel(48, 70)
                      .refine({{10, 48}, {48, 70}}),
                  {{10, 44}, {44, 70}}));

  // "st": the silent closure inside the cluster is bridged.
  CHECK(sameSpans(EdgeFrames(80).vowel(10, 38).sibilant(40, 44)
                      .plosive(47, 48).vowel(48, 70)
                      .refine({{10, 48}, {48, 70}}),
                  {{10, 40}, {40, 70}}));

  // A quiet "s", below the energy gate but labelled, still counts.
  CHECK(sameSpans(EdgeFrames(70).vowel(10, 30).quietSibilant(33, 40)
                      .vowel(40, 60)
                      .refine({{10, 40}, {40, 60}}),
                  {{10, 33}, {33, 60}}));

  // Legato: the voice runs from one note into the next, nothing moves.
  CHECK(sameSpans(EdgeFrames(80).vowel(10, 70).refine({{10, 40}, {40, 70}}),
                  {{10, 40}, {40, 70}}));

  // A breath GAME left in a note's head is cut off.
  CHECK(sameSpans(EdgeFrames(90).vowel(10, 30).breath(30, 50).vowel(52, 80)
                      .refine({{10, 30}, {48, 80}}),
                  {{10, 30}, {50, 80}}));

  // Long clusters are capped (26 frames of onset, then 22 of coda).
  CHECK(sameSpans(EdgeFrames(100).vowel(10, 20).sibilant(20, 70)
                      .vowel(70, 90)
                      .refine({{10, 70}, {70, 90}}),
                  {{10, 44}, {44, 90}}));

  // A note without voice keeps its span; the next onset stops at it.
  CHECK(sameSpans(EdgeFrames(60).plosive(10, 20).sibilant(21, 25)
                      .vowel(25, 40)
                      .refine({{10, 20}, {25, 40}}),
                  {{10, 20}, {21, 40}}));

  // First and last notes take their onset and coda too.
  CHECK(sameSpans(EdgeFrames(60).sibilant(5, 10).vowel(10, 40)
                      .sibilant(40, 46)
                      .refine({{10, 40}}),
                  {{5, 46}}));
}

bool near(double a, double b, double tolerance = 1e-9)
{
  return std::abs(a - b) <= tolerance;
}

// Stretching a note: the vowel takes the change, consonants keep their
// recorded length and land on whole frames.
void timeMapTests()
{
  using notetiming::TimeMap;

  // "s" + vowel + "t": source frames [100, 134).
  EdgeFrames frames(200);
  frames.sibilant(100, 106).vowel(106, 130).plosive(130, 134);

  // Moved, not stretched: every frame plays its own source frame.
  {
    const auto map =
        TimeMap::build(frames.voiced, frames.labels, 100, 134, 150, 184);
    for (int d = 150; d < 184; ++d)
    {
      CHECK(map.unscaledSourceFrame(d) == d - 50);
      CHECK(near(map.sourceFrameAt(d), d - 50));
    }
  }

  // Twice as long: the "s" and the "t" keep 6 and 4 frames, the vowel goes
  // from 24 to 58.
  {
    const auto map =
        TimeMap::build(frames.voiced, frames.labels, 100, 134, 200, 268);
    CHECK(map.segments().size() == 3);
    for (int d = 200; d < 206; ++d)
      CHECK(map.unscaledSourceFrame(d) == d - 100);
    for (int d = 264; d < 268; ++d)
      CHECK(map.unscaledSourceFrame(d) == d - 134);
    for (int d = 206; d < 264; ++d)
      CHECK(map.unscaledSourceFrame(d) == -1);
    CHECK(near(map.destinationAt(106), 206));
    CHECK(near(map.destinationAt(130), 264));
    CHECK(near(map.sourceAt(235), 118)); // the middle of the vowel
    for (double x = 200.0; x <= 268.0; x += 0.25)
      CHECK(near(map.destinationAt(map.sourceAt(x)), x, 1e-9));
  }

  // A consonant between two vowels still lands on whole frames: the vowels
  // share 28 frames as 16 + 12 (largest remainder of 16.47 + 11.53).
  {
    EdgeFrames mid(60);
    mid.vowel(10, 20).sibilant(20, 25).vowel(25, 32);
    const auto map = TimeMap::build(mid.voiced, mid.labels, 10, 32, 0, 33);
    for (int d = 16; d < 21; ++d)
      CHECK(map.unscaledSourceFrame(d) == d + 4);
    CHECK(near(map.segments().back().destinationEnd, 33));
  }

  // Shortened: the vowel gives up its length first, down to 30% of it (9 of
  // 30 frames here); past that, the consonants shrink with it.
  {
    EdgeFrames squeeze(60);
    squeeze.sibilant(0, 6).vowel(6, 36).sibilant(36, 40);
    const auto kept = TimeMap::build(squeeze.voiced, squeeze.labels, 0, 40, 0, 19);
    CHECK(kept.unscaledSourceFrame(0) == 0);
    CHECK(kept.unscaledSourceFrame(18) == 39);
    CHECK(near(kept.destinationAt(36), 15));
    const auto shrunk =
        TimeMap::build(squeeze.voiced, squeeze.labels, 0, 40, 0, 15);
    // 6 + 9 + 4 frames scaled to 15.
    CHECK(near(shrunk.destinationAt(6), 6.0 * 15 / 19));
    CHECK(near(shrunk.destinationAt(36), 15.0 * 15 / 19));
    for (int d = 0; d < 15; ++d)
      CHECK(shrunk.unscaledSourceFrame(d) == -1);
  }

  // No vowel, or only a blip of voicing: an even stretch, as before.
  {
    EdgeFrames none(40);
    none.sibilant(0, 8).vowel(8, 10).sibilant(10, 20);
    const auto map = TimeMap::build(none.voiced, none.labels, 0, 20, 0, 40);
    CHECK(map.segments().size() == 1);
    CHECK(near(map.sourceAt(20), 10));
  }

  // A one-frame voicing dropout stays inside the vowel; an "s" there does not.
  {
    EdgeFrames dropout(40);
    dropout.vowel(0, 10).vowel(11, 20);
    CHECK(TimeMap::build(dropout.voiced, dropout.labels, 0, 20, 0, 40)
              .segments()
              .size() == 1);
    dropout.sibilant(10, 11);
    const auto map =
        TimeMap::build(dropout.voiced, dropout.labels, 0, 20, 0, 40);
    CHECK(map.segments().size() == 3);
    bool found = false;
    for (int d = 0; d < 40; ++d)
      found = found || map.unscaledSourceFrame(d) == 10;
    CHECK(found);
  }

  // A note's contour follows its audio. Stored at the recorded length, a
  // curve of source frame numbers reads back as the frame each destination
  // frame plays; drawn on the stretched note and stretched again, it still
  // lands on the same audio.
  {
    AudioData audio;
    audio.voicedMask = frames.voiced;
    audio.breathSibilance = frames.labels;
    Note note(100, 134, 60.0f);
    note.setStartFrame(200);
    note.setEndFrame(268);
    std::vector<float> recorded(34);
    for (int i = 0; i < 34; ++i)
      recorded[static_cast<size_t>(i)] = static_cast<float>(100 + i);
    CHECK(notetiming::curveOnNote(audio, note, recorded).size() == 68);
    CHECK(notetiming::curveOnNote(audio, Note(100, 134, 60.0f), recorded) ==
          recorded);

    const auto map = notetiming::forNote(audio, note);
    const auto laidOut = notetiming::curveOnNote(audio, note, recorded);
    for (int d = 200; d < 268; ++d)
    {
      const double expected = std::clamp(map.sourceFrameAt(d), 100.0, 133.0);
      CHECK(near(laidOut[static_cast<size_t>(d - 200)], expected, 1e-4));
      if (map.unscaledSourceFrame(d) >= 0)
        CHECK(laidOut[static_cast<size_t>(d - 200)] ==
              static_cast<float>(map.unscaledSourceFrame(d)));
    }

    Note longer = note;
    longer.setEndFrame(302);
    const auto fromRecorded = notetiming::curveOnNote(audio, longer, recorded);
    const auto fromDrawn = notetiming::curveOnNote(audio, longer, laidOut);
    CHECK(fromRecorded.size() == 102 && fromDrawn.size() == 102);
    for (size_t i = 0; i < fromRecorded.size(); ++i)
      CHECK(near(fromRecorded[i], fromDrawn[i], 0.5));

    // Where those curves divide when the note is cut at a recorded frame.
    CHECK(notetiming::curveIndexAtSource(audio, note, 34, 120) == 20);
    CHECK(notetiming::curveIndexAtSource(audio, note, 68, 106) == 6);
    CHECK(notetiming::curveIndexAtSource(audio, note, 68, 130) == 64);
  }

  // The blend keeps the recording for a moved note's "s", copied from where
  // it was recorded, and leaves the stretched vowel synthesized.
  {
    constexpr int hop = 8;
    std::vector<float> recording(200 * hop);
    for (size_t i = 0; i < recording.size(); ++i)
      recording[i] = static_cast<float>(i);
    const auto map =
        TimeMap::build(frames.voiced, frames.labels, 100, 134, 200, 268);
    // Render range: frames [190, 280).
    std::vector<float> mask(90 * hop, 1.0f);
    std::vector<float> original(90 * hop, -1.0f);
    breathsibilance::protectMovedNoiseInBlend(
        mask, original, recording.data(), static_cast<int>(recording.size()),
        frames.labels, map, 200, 268, 190, 90, hop, 2);
    // The "s" plays destination frames [200, 206): kept from its own samples,
    // inside the note and away from the fades.
    for (int sample = (200 - 190) * hop + 2; sample < (206 - 190) * hop - hop / 2 - 2;
         ++sample)
    {
      CHECK(mask[static_cast<size_t>(sample)] == 0.0f);
      CHECK(original[static_cast<size_t>(sample)] ==
            static_cast<float>(sample + 190 * hop - 100 * hop));
    }
    // Before the note, the vowel and the unlabelled "t" stay synthesized.
    for (int sample = 0; sample < (200 - 190) * hop; ++sample)
      CHECK(mask[static_cast<size_t>(sample)] == 1.0f);
    for (int sample = (206 - 190) * hop; sample < (268 - 190) * hop; ++sample)
      CHECK(mask[static_cast<size_t>(sample)] == 1.0f);
  }
}

bool nearGain(double a, double b, double tolerance = 1e-4)
{
  return std::abs(a - b) <= tolerance;
}

// Breath notes and the sibilant balance: where they are, what gain they set,
// and that a gain edit on the rendered audio undoes exactly.
void gainTests()
{
  const int hop = HOP_SIZE;
  constexpr int frames = 260;

  // Note A [20, 60) with an "s" at [40, 46) and a sibilant just after it at
  // [60, 64); a breath [75, 90) leading into note B [100, 140); a stray
  // sibilant [160, 164) touching no note; a breath [200, 210) far from both.
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = static_cast<int>(kRate);
  audio.melSpectrogram.assign(frames, std::vector<float>(1, 0.0f));
  audio.voicedMask.assign(frames, false);
  audio.breathSibilance.assign(frames, None);
  const auto voice = [&](int from, int to)
  {
    for (int f = from; f < to; ++f)
      audio.voicedMask[static_cast<size_t>(f)] = true;
  };
  const auto label = [&](int from, int to, std::uint8_t value)
  {
    for (int f = from; f < to; ++f)
      audio.breathSibilance[static_cast<size_t>(f)] = value;
  };
  voice(20, 40);
  voice(46, 60);
  voice(100, 140);
  label(40, 46, Sibilance);
  label(60, 64, Sibilance);
  label(75, 90, Breath);
  label(160, 164, Sibilance);
  label(200, 210, Breath);
  project.addNote(Note(20, 60, 60.0f));
  project.addNote(Note(100, 140, 62.0f));
  Note *noteA = &project.getNotes()[0];
  Note *noteB = &project.getNotes()[1];

  // Rendered audio that is never near zero, so gains read as ratios.
  audio.waveform.setSize(1, frames * hop);
  std::mt19937 rng(99);
  std::uniform_real_distribution<float> spread(0.5f, 1.5f);
  for (int i = 0; i < audio.waveform.getNumSamples(); ++i)
    audio.waveform.setSample(0, i, 0.25f * spread(rng));
  juce::AudioBuffer<float> rendered;
  rendered.makeCopyOf(audio.waveform);
  const auto ratioAt = [&](int sample)
  {
    return static_cast<double>(audio.waveform.getSample(0, sample)) /
           rendered.getSample(0, sample);
  };
  const auto restored = [&]
  {
    for (int i = 0; i < audio.waveform.getNumSamples(); ++i)
      if (!nearGain(audio.waveform.getSample(0, i), rendered.getSample(0, i), 1e-6))
        return false;
    return true;
  };
  const auto gainAt = [&](int sample)
  {
    return static_cast<double>(NoteGainCurve::build(projectgain::regions(project),
                                                    sample, 1, frames * hop,
                                                    hop)[0]);
  };
  // Sample positions away from every edge and its smoothing.
  const int inS = 43 * hop;      // the "s" inside note A
  const int inVowel = 30 * hop;  // note A's vowel
  const int beforeA = 10 * hop;  // outside every note
  const int inBreath = 82 * hop; // the breath before note B
  const int inB = 120 * hop;     // note B

  // Balance -> {hatched, rest} gains: it only ever cuts, to -60 dB at 100%.
  {
    const double halfDb = 20.0 * std::log10(0.5); // -6.02 dB
    CHECK(projectgain::balanceGainsDb(0.0f) == std::make_pair(0.0f, 0.0f));
    CHECK(nearGain(projectgain::balanceGainsDb(-0.5f).first, halfDb));
    CHECK(projectgain::balanceGainsDb(-0.5f).second == 0.0f);
    CHECK(projectgain::balanceGainsDb(0.5f).first == 0.0f);
    CHECK(nearGain(projectgain::balanceGainsDb(0.5f).second, halfDb));
    CHECK(projectgain::balanceGainsDb(-1.0f) == std::make_pair(-60.0f, 0.0f));
    CHECK(projectgain::balanceGainsDb(1.0f) == std::make_pair(0.0f, -60.0f));
    Note clamped;
    clamped.setSibilantBalance(-3.0f);
    CHECK(clamped.getSibilantBalance() == -1.0f);
    clamped.setSibilantBalance(2.0f);
    CHECK(clamped.getSibilantBalance() == 1.0f);
  }

  // Hatched frames are where a note plays labelled audio, centre-aligned.
  {
    const auto hatched = projectgain::hatchedSamples(audio, *noteA);
    CHECK(hatched.size() == 1);
    CHECK(hatched[0] == std::make_pair(40 * hop - hop / 2, 46 * hop - hop / 2));
    CHECK(projectgain::hatchedSamples(audio, *noteB).empty());
    CHECK(!projectgain::hasHatchedFrames(audio, *noteB));

    // Stretched to twice its length, the "s" keeps its six frames; the vowels
    // around it take the extra length.
    Note stretched(20, 60, 60.0f);
    stretched.setEndFrame(100);
    const auto moved = projectgain::hatchedSamples(audio, stretched);
    CHECK(moved.size() == 1);
    CHECK(moved[0].second - moved[0].first == 6 * hop);
    CHECK(moved[0].first == 64 * hop - hop / 2);
  }

  // The balance's gain over the note: negative cuts the "s", positive the
  // rest; at 0 nothing changes.
  {
    CHECK(nearGain(gainAt(inS), 1.0) && nearGain(gainAt(inVowel), 1.0));
    noteA->setSibilantBalance(-0.5f);
    CHECK(nearGain(gainAt(inS), 0.5) && nearGain(gainAt(inVowel), 1.0));
    CHECK(nearGain(gainAt(beforeA), 1.0) && nearGain(gainAt(inB), 1.0));
    noteA->setSibilantBalance(0.5f);
    CHECK(nearGain(gainAt(inS), 1.0) && nearGain(gainAt(inVowel), 0.5));
    noteA->setSibilantBalance(1.0f);
    CHECK(nearGain(gainAt(inS), 1.0) && nearGain(gainAt(inVowel), 0.001, 1e-6));
    // With the note's volume on top, the two multiply.
    noteA->setVolumeDb(20.0f * std::log10(0.5f));
    noteA->setSibilantBalance(-0.5f);
    CHECK(nearGain(gainAt(inS), 0.25) && nearGain(gainAt(inVowel), 0.5));
    noteA->setVolumeDb(0.0f);
    noteA->setSibilantBalance(0.0f);
  }

  // Breath notes: every breath is one; other noise only where it touches a
  // note.
  const auto findAt = [](const std::vector<breathnotes::Placement> &placements,
                         float start) -> const breathnotes::Placement *
  {
    for (const auto &placement : placements)
      if (placement.start == start)
        return &placement;
    return nullptr;
  };
  {
    const auto placements = breathnotes::place(project);
    CHECK(placements.size() == 4);
    const auto *tail = findAt(placements, 60.0f);
    CHECK(tail && tail->drawn && !tail->breath && tail->anchor == noteA);
    CHECK(tail->left == 60.0f && tail->right == 63.5f); // meets note A's end

    const auto *breath = findAt(placements, 75.0f);
    CHECK(breath && breath->drawn && breath->breath && breath->anchor == noteB);
    CHECK(breath->left == 74.5f && breath->right == 100.0f); // meets note B
    CHECK(breath->key() == 75);
    CHECK(breathnotes::find(placements, 75) == breath);

    const auto *stray = findAt(placements, 160.0f);
    CHECK(stray && !stray->drawn);
    CHECK(breathnotes::find(placements, 160) == nullptr);

    // Out of reach of every note it still shows, at the nearest note's pitch.
    const auto *alone = findAt(placements, 200.0f);
    CHECK(alone && alone->drawn && alone->breath && alone->anchor == noteB);
    CHECK(alone->left == 199.5f && alone->right == 209.5f);

    // Level: stored per source frame across the note, read at its first
    // breath frame, applied over its whole extent.
    CHECK(breathnotes::gainDb(audio, *breath) == 0.0f);
    CHECK(breathnotes::gainRegions(project).empty());
    breathnotes::setGainDb(audio, *breath, -12.0f);
    CHECK(audio.breathGainDb.size() == 100);
    CHECK(std::all_of(audio.breathGainDb.begin() + 75, audio.breathGainDb.end(),
                      [](float db) { return db == -12.0f; }));
    CHECK(breathnotes::gainDb(audio, *breath) == -12.0f);
    CHECK(breathnotes::gainDb(audio, *alone) == 0.0f);
    const auto regions = breathnotes::gainRegions(project);
    CHECK(regions.size() == 1);
    CHECK(regions[0].start == static_cast<int>(std::lround(74.5 * hop)) &&
          regions[0].end == 100 * hop && regions[0].db == -12.0f);
    CHECK(nearGain(gainAt(inBreath), std::pow(10.0, -12.0 / 20.0)));
    CHECK(nearGain(gainAt(inB), 1.0));

    // Note B moved back over most of the breath: what is left of it still
    // meets the note and keeps its level.
    noteB->setStartFrame(80);
    noteB->setEndFrame(120);
    const auto clipped = breathnotes::place(project);
    const auto *rest = breathnotes::find(clipped, 75);
    CHECK(rest && rest->start == 75.0f && rest->end == 80.0f);
    CHECK(rest->right == 80.0f && rest->anchor == noteB);
    CHECK(breathnotes::gainDb(audio, *rest) == -12.0f);
    noteB->setStartFrame(100);
    noteB->setEndFrame(140);

    // Back to 0 dB leaves nothing stored.
    breathnotes::setGainDb(audio, *breath, 0.0f);
    CHECK(audio.breathGainDb.empty());
  }

  // A balance edit applied to the rendered audio, and undone exactly.
  {
    auto action = gainedit::noteGains(project, {noteA}, {0.0f}, {0.0f}, {0.0f},
                                      {-0.5f}, nullptr, "Change Sibilant Balance");
    CHECK(!action->requiresAudioResynthesis());
    action->redo();
    CHECK(noteA->getSibilantBalance() == -0.5f && noteA->hasRenderedEdit());
    CHECK(nearGain(ratioAt(inS), 0.5) && nearGain(ratioAt(inVowel), 1.0));
    CHECK(ratioAt(beforeA) == 1.0 && ratioAt(inB) == 1.0);
    action->undo();
    CHECK(noteA->getSibilantBalance() == 0.0f && !noteA->hasRenderedEdit());
    CHECK(restored());

    auto cutVowel = gainedit::noteGains(project, {noteA}, {0.0f}, {0.0f}, {0.0f},
                                        {1.0f}, nullptr, "Change Sibilant Balance");
    cutVowel->redo();
    CHECK(nearGain(ratioAt(inS), 1.0) && nearGain(ratioAt(inVowel), 0.001, 1e-6));
    cutVowel->undo();
    CHECK(restored());
  }

  // Double-click to -100%, then drag back up to -64%. The audio lands where a
  // single change to -64% would, and the preview - the change ratio over the
  // rendered audio - shows exactly that: no spike where the cut left the
  // edges of the "s" on the smoothed part of the curve.
  {
    const int first = 20 * hop - 128;
    const int count = (60 - 20) * hop + 256;
    auto mute = gainedit::noteGains(project, {noteA}, {0.0f}, {0.0f}, {0.0f},
                                    {-1.0f}, nullptr, "Change Sibilant Balance");
    mute->redo();
    CHECK(nearGain(ratioAt(inS), 0.001, 1e-6));

    const std::vector<std::pair<const Note *, Note>> muted{{noteA, *noteA}};
    noteA->setSibilantBalance(-0.64f);
    const auto ratio =
        projectgain::changeRatio(projectgain::regions(project, muted),
                                 projectgain::regions(project), first, count,
                                 frames * hop);
    noteA->setSibilantBalance(-1.0f);
    std::vector<float> preview(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
      preview[static_cast<size_t>(i)] =
          audio.waveform.getSample(0, first + i) * ratio[static_cast<size_t>(i)];

    auto raise = gainedit::noteGains(project, {noteA}, {0.0f}, {0.0f}, {-1.0f},
                                     {-0.64f}, nullptr, "Change Sibilant Balance");
    raise->redo();
    CHECK(nearGain(ratioAt(inS), 0.36) && nearGain(ratioAt(inVowel), 1.0));
    std::vector<float> twoSteps(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
      const float now = audio.waveform.getSample(0, first + i);
      twoSteps[static_cast<size_t>(i)] = now;
      CHECK(nearGain(preview[static_cast<size_t>(i)], now, 1e-6));
      // Every gain here is a cut: nothing ends up louder than it was.
      CHECK(std::abs(now) <=
            std::abs(rendered.getSample(0, first + i)) * (1.0f + 1e-5f));
    }
    raise->undo();
    mute->undo();
    CHECK(restored());

    auto once = gainedit::noteGains(project, {noteA}, {0.0f}, {0.0f}, {0.0f},
                                    {-0.64f}, nullptr, "Change Sibilant Balance");
    once->redo();
    for (int i = 0; i < count; ++i)
      CHECK(nearGain(audio.waveform.getSample(0, first + i),
                     twoSteps[static_cast<size_t>(i)], 1e-6));
    once->undo();
    CHECK(restored());
  }

  // The sibilant just after note A [60, 64) is not drawn: it belongs to the
  // note and takes its volume and its sibilant balance's cut.
  {
    const int inStray = 62 * hop;
    CHECK(nearGain(gainAt(inStray), 1.0));
    auto louder = gainedit::noteGains(project, {noteA}, {0.0f},
                                      {20.0f * std::log10(0.5f)}, {0.0f}, {0.0f},
                                      nullptr, "Change Amplitude");
    louder->redo();
    CHECK(nearGain(ratioAt(inStray), 0.5) && nearGain(ratioAt(inVowel), 0.5));
    CHECK(ratioAt(80 * hop) == 1.0); // the breath before note B
    louder->undo();
    CHECK(restored());

    auto cutS = gainedit::noteGains(project, {noteA}, {0.0f}, {0.0f}, {0.0f},
                                    {-0.5f}, nullptr, "Change Sibilant Balance");
    cutS->redo();
    CHECK(nearGain(ratioAt(inStray), 0.5) && nearGain(ratioAt(inS), 0.5));
    CHECK(nearGain(ratioAt(inVowel), 1.0));
    cutS->undo();
    CHECK(restored());
    // A cut of the rest of the note leaves it alone, as the "s" inside.
    noteA->setSibilantBalance(0.5f);
    CHECK(nearGain(gainAt(inStray), 1.0));
    noteA->setSibilantBalance(0.0f);
  }

  // A breath level edit, and undone exactly.
  {
    const auto placements = breathnotes::place(project);
    const auto *breath = breathnotes::find(placements, 75);
    CHECK(breath != nullptr);
    gainedit::BreathChange change{
        75, 100, static_cast<int>(std::lround(breath->left * hop)),
        static_cast<int>(std::lround(breath->right * hop)), 0.0f, -12.0f};
    auto action = gainedit::breathGains(project, {change}, nullptr,
                                        "Change Breath Level");
    action->redo();
    CHECK(breathnotes::gainDb(audio, *breath) == -12.0f);
    CHECK(nearGain(ratioAt(inBreath), std::pow(10.0, -12.0 / 20.0)));
    CHECK(ratioAt(inB) == 1.0 && ratioAt(inVowel) == 1.0);
    action->undo();
    CHECK(audio.breathGainDb.empty());
    CHECK(restored());
    action->redo();
  }

  // A level being dragged is previewed with the regions the edit will store.
  {
    const auto placements = breathnotes::place(project);
    const auto *breath = breathnotes::find(placements, 75);
    CHECK(breath != nullptr);
    const float stored = breathnotes::gainDb(audio, *breath);
    const auto preview = projectgain::regions(project, {}, {75, -3.0f});
    breathnotes::setGainDb(audio, *breath, -3.0f);
    const auto after = projectgain::regions(project);
    breathnotes::setGainDb(audio, *breath, stored);
    CHECK(preview.size() == after.size());
    for (size_t i = 0; i < preview.size() && i < after.size(); ++i)
      CHECK(preview[i].start == after[i].start &&
            preview[i].end == after[i].end && preview[i].db == after[i].db);
    // A breath with no level of its own gains a region.
    const auto *alone = findAt(placements, 200.0f);
    CHECK(alone != nullptr && breathnotes::gainDb(audio, *alone) == 0.0f);
    CHECK(projectgain::regions(project, {}, {alone->key(), -6.0f}).size() ==
          projectgain::regions(project).size() + 1);
  }

  // Both are saved with the project.
  {
    noteA->setSibilantBalance(-0.35f);
    Project reloaded;
    CHECK(ProjectSerializer::fromJson(
        reloaded, ProjectSerializer::toJson(project, true, true)));
    CHECK(reloaded.getNotes().size() == 2);
    CHECK(reloaded.getNotes()[0].getSibilantBalance() == -0.35f);
    CHECK(reloaded.getNotes()[1].getSibilantBalance() == 0.0f);
    CHECK(reloaded.getAudioData().breathGainDb == audio.breathGainDb);

    // Nothing is written for a project without either.
    noteA->setSibilantBalance(0.0f);
    audio.breathGainDb.clear();
    const auto plain = ProjectSerializer::toJson(project, true, true);
    CHECK(!plain.getDynamicObject()->hasProperty("breathGains"));
    Project reloadedPlain;
    CHECK(ProjectSerializer::fromJson(reloadedPlain, plain));
    CHECK(reloadedPlain.getAudioData().breathGainDb.empty());
    CHECK(reloadedPlain.getNotes()[0].getSibilantBalance() == 0.0f);
  }
}
} // namespace

int main()
{
  // A phrase with an "s" between two vowels, an inhale between phrases, an
  // aspirated "h" onset and a final "sh".
  Take take;
  take.silence(0.30);
  const auto vowelA = take.vowel(0.50, 220.0, 0.20f);
  const auto s = take.noise(0.12, {5500.0, 7500.0, 9000.0}, 1800.0, 0.08f);
  const auto vowelB = take.vowel(0.45, 247.0, 0.20f);
  take.silence(0.12);
  // Inhales measured on real takes centre around 2-3 kHz.
  const auto breath =
      take.noise(0.35, {1200.0, 2200.0, 3200.0}, 900.0, 0.012f);
  take.silence(0.08);
  const auto h = take.noise(0.06, {1200.0, 2200.0}, 600.0, 0.03f);
  const auto vowelC = take.vowel(0.55, 196.0, 0.20f);
  const auto sh = take.noise(0.14, {3000.0, 4200.0, 5600.0}, 1500.0, 0.06f);
  take.silence(0.40);

  const auto labels = take.detect();
  CHECK(static_cast<int>(labels.size()) == take.frames());

  // Sung audio is never labelled.
  CHECK(countVoicedNoise(labels, take.voiced) == 0);
  CHECK(shareOf(labels, vowelA, None) == 1.0);
  CHECK(shareOf(labels, vowelB, None) == 1.0);
  CHECK(shareOf(labels, vowelC, None) == 1.0);

  // Consonants next to singing are sibilance, including the aspirated h.
  CHECK(shareOf(labels, s, Sibilance) >= 0.9);
  CHECK(shareOf(labels, sh, Sibilance) >= 0.9);
  CHECK(shareOf(labels, h, Sibilance, 1) >= 0.5);

  // The inhale between phrases is a breath.
  CHECK(shareOf(labels, breath, Breath) >= 0.9);
  CHECK(shareOf(labels, breath, Sibilance) == 0.0);

  // Thresholds are relative to the take, so the input level does not matter.
  CHECK(take.detect(0.01f) == labels);
  CHECK(take.detect(3.0f) == labels);

  // A click far from any singing is neither.
  {
    Take clicky;
    clicky.silence(0.30);
    const auto click = clicky.noise(0.04, {6000.0, 9000.0}, 3000.0, 0.3f);
    clicky.silence(1.20);
    clicky.vowel(0.60, 220.0, 0.2f);
    clicky.silence(0.30);
    const auto clickLabels = clicky.detect();
    CHECK(shareOf(clickLabels, click, Sibilance, 0) == 0.0);
    CHECK(shareOf(clickLabels, click, Breath, 0) == 0.0);
  }

  // Noise with no singing anywhere near (over 1 s away) is not a breath.
  {
    Take room;
    room.vowel(0.50, 220.0, 0.2f);
    room.silence(1.50);
    const auto rustle =
        room.noise(0.40, {1200.0, 2200.0, 3200.0}, 900.0, 0.012f);
    room.silence(1.50);
    room.vowel(0.50, 220.0, 0.2f);
    const auto roomLabels = room.detect();
    CHECK(shareOf(roomLabels, rustle, Breath, 0) == 0.0);
  }

  // Degenerate input: nothing to label, nothing to crash on.
  {
    std::vector<float> zeros(44100, 0.0f);
    const int frames = static_cast<int>(zeros.size()) / kHop + 1;
    const auto silent = breathsibilance::detect(
        zeros.data(), static_cast<int>(zeros.size()), kRate, kHop, frames, {});
    CHECK(static_cast<int>(silent.size()) == frames);
    CHECK(countAll(silent, None) == frames);
    CHECK(breathsibilance::detect(nullptr, 0, kRate, kHop, 0, {}).empty());
    // A voicing mask shorter than the take counts the rest as unvoiced.
    const auto shortMask = breathsibilance::detect(
        take.samples.data(), static_cast<int>(take.samples.size()), kRate,
        kHop, take.frames(), std::vector<bool>(10, true));
    CHECK(static_cast<int>(shortMask.size()) == take.frames());
  }

  // Resynthesis keeps labelled frames as original audio.
  {
    constexpr int hop = 512;
    constexpr int frames = 40;
    constexpr int startFrame = 100;
    std::vector<std::uint8_t> noise(200, None);
    for (int f = 110; f < 118; ++f) // a sibilant inside the range
      noise[static_cast<size_t>(f)] = Sibilance;
    for (int f = 130; f < 150; ++f) // a breath running past its end
      noise[static_cast<size_t>(f)] = Breath;

    std::vector<float> mask(frames * hop, 1.0f);
    breathsibilance::protectNoiseInBlendMask(mask, noise, startFrame, frames,
                                             hop, 256);

    // The sibilant covers frames 10..17 of the range, centre-aligned:
    // samples [9.5, 17.5) hops, with 256-sample fades inside that span.
    const int sibFirst = 10 * hop - hop / 2;
    const int sibLast = 18 * hop - hop / 2;
    for (int i = 0; i < sibFirst; ++i)
      CHECK(mask[static_cast<size_t>(i)] == 1.0f);
    for (int i = sibFirst; i < sibFirst + 256; ++i)
    {
      CHECK(mask[static_cast<size_t>(i)] < 1.0f);
      CHECK(mask[static_cast<size_t>(i)] >= mask[static_cast<size_t>(i + 1)]);
    }
    for (int i = sibFirst + 256; i < sibLast - 256; ++i)
      CHECK(mask[static_cast<size_t>(i)] == 0.0f);
    for (int i = sibLast - 256; i < sibLast; ++i)
      CHECK(mask[static_cast<size_t>(i)] <= mask[static_cast<size_t>(i + 1)]);
    for (int i = sibLast; i < 30 * hop - hop / 2; ++i)
      CHECK(mask[static_cast<size_t>(i)] == 1.0f);

    // The breath runs off the end of the range: faded in, never faded out.
    const int breathFirst = 30 * hop - hop / 2;
    CHECK(mask[static_cast<size_t>(breathFirst)] > 0.0f);
    for (int i = breathFirst + 256; i < frames * hop; ++i)
      CHECK(mask[static_cast<size_t>(i)] == 0.0f);

    // A range starting inside a run keeps the original from its first sample.
    std::vector<float> cut(10 * hop, 1.0f);
    breathsibilance::protectNoiseInBlendMask(cut, noise, 112, 10, hop, 256);
    CHECK(cut.front() == 0.0f);

    // Existing original-side samples stay original; no labels, no change.
    std::vector<float> zeros(frames * hop, 0.0f);
    breathsibilance::protectNoiseInBlendMask(zeros, noise, startFrame, frames,
                                             hop, 256);
    for (float value : zeros)
      CHECK(value == 0.0f);
    std::vector<float> untouched(frames * hop, 1.0f);
    breathsibilance::protectNoiseInBlendMask(untouched, {}, startFrame, frames,
                                             hop, 256);
    for (float value : untouched)
      CHECK(value == 1.0f);
  }

  // Project data: labels are derived, not saved. They are rebuilt from the
  // pristine source wherever a project comes back without them, identical to
  // what the session had, and stay on the voicing grid when it is re-padded.
  {
    Project project;
    auto &audio = project.getAudioData();
    audio.sampleRate = static_cast<int>(kRate);
    audio.originalWaveform.setSize(1, static_cast<int>(take.samples.size()));
    audio.originalWaveform.copyFrom(0, 0, take.samples.data(),
                                    static_cast<int>(take.samples.size()));
    audio.waveform.makeCopyOf(audio.originalWaveform);
    audio.voicedMask = take.voiced;
    audio.voicedMask.resize(static_cast<size_t>(take.frames()), false);
    audio.vadMask = audio.voicedMask;
    audio.f0.assign(audio.voicedMask.size(), 220.0f);
    audio.basePitch.assign(audio.voicedMask.size(), 57.0f);
    audio.deltaPitch.assign(audio.voicedMask.size(), 0.0f);

    CHECK(breathsibilance::ensure(audio));
    CHECK(audio.breathSibilance == labels);
    CHECK(!breathsibilance::ensure(audio)); // already current

    // A note's timeline frames map back onto the audio it plays: in place,
    // moved, and stretched to twice its length. The piano roll leaves the
    // pitch curve out wherever this lands on breath or sibilance.
    {
      const auto [sFirst, sLast] = Take::framesOf(s.first, s.second);
      const int sourceStart = sFirst - 6;
      const int sourceEnd = sLast + 6;
      const Note inPlace(sourceStart, sourceEnd, 60.0f);
      Note moved(sourceStart, sourceEnd, 60.0f);
      moved.setStartFrame(sourceStart + 40);
      moved.setEndFrame(sourceEnd + 40);
      Note stretched(sourceStart, sourceEnd, 60.0f);
      stretched.setStartFrame(sourceStart + 40);
      stretched.setEndFrame(sourceStart + 40 + 2 * (sourceEnd - sourceStart));
      int noiseFrames = 0;
      for (int k = 0; k < sourceEnd - sourceStart; ++k)
      {
        const auto expected = labels[static_cast<size_t>(sourceStart + k)];
        noiseFrames += breathsibilance::isNoise(expected) ? 1 : 0;
        CHECK(breathsibilance::labelAtNoteFrame(audio, inPlace,
                                                sourceStart + k) == expected);
        CHECK(breathsibilance::labelAtNoteFrame(audio, moved,
                                                sourceStart + 40 + k) ==
              expected);
      }
      CHECK(noiseFrames > 0); // the "s" is inside the span
      // Stretched, the vowels around the "s" take the extra length; the "s"
      // keeps its frames.
      int stretchedNoise = 0;
      for (int d = stretched.getStartFrame(); d < stretched.getEndFrame(); ++d)
        stretchedNoise += breathsibilance::isNoise(
                              breathsibilance::labelAtNoteFrame(audio, stretched, d))
                              ? 1
                              : 0;
      CHECK(stretchedNoise == noiseFrames);
      CHECK(!breathsibilance::isNoise(breathsibilance::labelAtNoteFrame(
          audio, stretched, stretched.getStartFrame())));
      CHECK(!breathsibilance::isNoise(breathsibilance::labelAtNoteFrame(
          audio, stretched, stretched.getEndFrame() - 1)));
    }

    juce::MemoryBlock archive;
    CHECK(ProjectSerializer::toBinaryArchive(
        project, archive, ProjectSerializer::BinaryArchiveMode::selfContained));
    Project restored;
    CHECK(ProjectSerializer::fromBinaryArchive(restored, archive.getData(),
                                                archive.getSize()));
    CHECK(restored.getAudioData().breathSibilance == labels);

    Project restoredJson;
    CHECK(ProjectSerializer::fromJson(
        restoredJson, ProjectSerializer::toJson(project, true, true)));
    CHECK(restoredJson.getAudioData().breathSibilance == labels);

    // A host-backed archive has no source to label; ARA hydration does it.
    juce::MemoryBlock hostBacked;
    CHECK(ProjectSerializer::toBinaryArchive(
        project, hostBacked,
        ProjectSerializer::BinaryArchiveMode::hostBackedARA));
    Project sourceless;
    CHECK(ProjectSerializer::fromBinaryArchive(
        sourceless, hostBacked.getData(), hostBacked.getSize()));
    CHECK(sourceless.getAudioData().breathSibilance.empty());

    // Moving the take to a later host position shifts labels with voicing,
    // and breath levels with them.
    audio.breathGainDb.assign(12, 0.0f);
    std::fill(audio.breathGainDb.begin() + 8, audio.breathGainDb.end(), -6.0f);
    const auto breathGains = audio.breathGainDb;
    CHECK(repadProjectToTimelineOffset(project, 0.5));
    const auto pad = static_cast<size_t>(std::llround(0.5 * kRate / kHop));
    CHECK(audio.breathSibilance.size() == audio.voicedMask.size());
    CHECK(audio.breathSibilance.size() == pad + labels.size());
    CHECK(std::equal(labels.begin(), labels.end(),
                     audio.breathSibilance.begin() + static_cast<long>(pad)));
    CHECK(std::all_of(audio.breathSibilance.begin(),
                      audio.breathSibilance.begin() + static_cast<long>(pad),
                      [](std::uint8_t label) { return label == None; }));
    CHECK(audio.breathGainDb.size() == pad + breathGains.size());
    CHECK(std::equal(breathGains.begin(), breathGains.end(),
                     audio.breathGainDb.begin() + static_cast<long>(pad)));
  }

  noteEdgeTests();
  timeMapTests();
  gainTests();

  CHECK(breathsibilance::labelAt(labels, -1) == None);
  CHECK(breathsibilance::labelAt(labels, take.frames()) == None);
  CHECK(breathsibilance::isNoise(Sibilance) && breathsibilance::isNoise(Breath) &&
        !breathsibilance::isNoise(None));

  std::printf("BreathSibilanceTests passed\n");
  return 0;
}
