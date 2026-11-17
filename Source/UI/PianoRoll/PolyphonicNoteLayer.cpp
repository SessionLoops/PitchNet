#include "PolyphonicNoteLayer.h"

#include "NoteColours.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>

using namespace notecolours;

namespace
{
// The selected note's outline.
constexpr float kSelectedOutlineWidth = 1.5f;
constexpr float kSelectedOutlineAlpha = 0.85f;

// The mix's display envelope at each analysis frame, as the vocal blobs
// measure it (VisualWaveformEnvelope: RMS over ~12 ms, x1.75, capped at 1).
std::vector<float> mixEnvelope(const juce::AudioBuffer<float> &waveform,
                               double sampleRate, int hop, int numFrames)
{
  std::vector<float> env(static_cast<size_t>(std::max(0, numFrames)), 0.0f);
  const int n = waveform.getNumSamples();
  if (n <= 0 || sampleRate <= 0.0)
    return env;
  const float *x = waveform.getReadPointer(0);
  const int half = std::max(1, static_cast<int>(std::lround(sampleRate * 0.006)));
  for (int f = 0; f < numFrames; ++f)
  {
    const int centre = f * hop;
    const int a = std::max(0, centre - half);
    const int b = std::min(n, centre + half);
    if (b <= a)
      continue;
    double sum = 0.0;
    for (int i = a; i < b; ++i)
      sum += static_cast<double>(x[i]) * x[i];
    env[static_cast<size_t>(f)] =
        std::min(1.0f, static_cast<float>(std::sqrt(sum / (b - a))) * 1.75f);
  }
  return env;
}
} // namespace

void PolyphonicNoteLayer::setProject(Project *newProject)
{
  project = newProject;
  rebuildIfNeeded();
}

bool PolyphonicNoteLayer::isActive() const
{
  return project != nullptr && project->isPolyphonic();
}

float PolyphonicNoteLayer::frameSeconds(double frame) const
{
  return builtFor ? static_cast<float>(builtFor->frameToSeconds(frame)) : 0.0f;
}

void PolyphonicNoteLayer::rebuildIfNeeded()
{
  const auto take = project != nullptr ? project->getPolyphonicTake() : nullptr;
  const auto analysis = take != nullptr ? take->analysis : nullptr;
  if (analysis == builtFor)
    return;

  builtFor = analysis;
  shapes.clear();
  selection.clear();
  if (!analysis)
    return;

  const int numFrames = analysis->numFrames;
  std::vector<double> total(static_cast<size_t>(std::max(0, numFrames)), 0.0);

  // Each active note's own level per frame: the root of its partials' summed
  // squared model amplitudes. Masks start a few frames before the note.
  for (size_t i = 0; i < analysis->notes.size() && i < analysis->masks.size(); ++i)
  {
    const auto &note = analysis->notes[i];
    if (!note.active)
      continue;
    NoteShape shape;
    shape.note = static_cast<int>(i);
    shape.medianMidi = note.medianMidi();
    shape.firstFrame = note.startFrame;
    shape.level.assign(static_cast<size_t>(std::max(1, note.numFrames())), 0.0f);
    std::vector<double> energy(shape.level.size(), 0.0);
    for (const auto &partial : analysis->masks[i].partials)
    {
      const int k = partial.frame - shape.firstFrame;
      if (k < 0 || k >= static_cast<int>(energy.size()))
        continue;
      energy[static_cast<size_t>(k)] +=
          static_cast<double>(partial.amplitude) * partial.amplitude;
    }
    for (size_t k = 0; k < energy.size(); ++k)
    {
      shape.level[k] = static_cast<float>(std::sqrt(energy[k]));
      const int f = shape.firstFrame + static_cast<int>(k);
      if (f >= 0 && f < numFrames)
        total[static_cast<size_t>(f)] += energy[k];
    }
    shapes.push_back(std::move(shape));
  }

  // One scale for the take: the loudest moment of all notes together matches
  // the loudest moment of the mix's display envelope, so blob sizes compare
  // with vocal blobs and with each other.
  const auto &audioData = project->getAudioData();
  const auto mix = mixEnvelope(audioData.waveform, audioData.sampleRate,
                               analysis->params.hop, numFrames);
  float mixPeak = 0.0f;
  double modelPeak = 0.0;
  for (int f = 0; f < numFrames; ++f)
  {
    mixPeak = std::max(mixPeak, mix[static_cast<size_t>(f)]);
    modelPeak = std::max(modelPeak, std::sqrt(total[static_cast<size_t>(f)]));
  }
  const float scale = modelPeak > 0.0 ? mixPeak / static_cast<float>(modelPeak) : 0.0f;
  for (auto &shape : shapes)
    for (auto &v : shape.level)
      v = std::min(1.0f, v * scale);
}

float PolyphonicNoteLayer::halfHeightAt(const NoteShape &shape, int frame,
                                        float waveHeight) const
{
  const int k = juce::jlimit(0, static_cast<int>(shape.level.size()) - 1,
                             frame - shape.firstFrame);
  // At least half a pixel each side, so a near-silent stretch stays visible.
  return std::max(0.5f, shape.level[static_cast<size_t>(k)] * waveHeight * 0.5f);
}

void PolyphonicNoteLayer::draw(juce::Graphics &g, int componentWidth)
{
  rebuildIfNeeded();
  if (!builtFor || coordMapper == nullptr || shapes.empty())
    return;

  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const double scrollX = coordMapper->getScrollX();
  const double visibleStart = scrollX / pixelsPerSecond;
  const double visibleEnd = (scrollX + componentWidth) / pixelsPerSecond;
  const float waveHeight = pixelsPerSemitone * 3.0f * coordMapper->getBlobHeightScale();
  const int pitchReferenceHz = project->getPitchReferenceHz();

  auto rowCentre = [&](float midi) {
    return coordMapper->midiToY(midi) + pixelsPerSemitone * 0.5f;
  };

  for (const auto &shape : shapes)
  {
    const auto &note = builtFor->notes[static_cast<size_t>(shape.note)];
    const double start = frameSeconds(note.startFrame);
    const double end = frameSeconds(note.endFrame + 1);
    if (end < visibleStart || start > visibleEnd)
      continue;

    const float cy = rowCentre(shape.medianMidi);
    // Top edge left to right, bottom edge back: symmetric about the pitch row.
    juce::Path body;
    const int last = note.endFrame + 1;
    for (int f = note.startFrame; f <= last; ++f)
    {
      const float x = static_cast<float>(frameSeconds(f) * pixelsPerSecond);
      const float y = cy - halfHeightAt(shape, std::min(f, note.endFrame), waveHeight);
      if (f == note.startFrame)
        body.startNewSubPath(x, y);
      else
        body.lineTo(x, y);
    }
    for (int f = last; f >= note.startFrame; --f)
    {
      const float x = static_cast<float>(frameSeconds(f) * pixelsPerSecond);
      body.lineTo(x, cy + halfHeightAt(shape, std::min(f, note.endFrame), waveHeight));
    }
    body.closeSubPath();
    body = body.createPathWithRoundedCorners(2.0f);

    const auto colours = getNoteGradientColours(shape.medianMidi, pitchReferenceHz);
    setNoteGradientFill(g, body.getBounds(), cy, colours);
    g.fillPath(body);

    // Selection: an outline that follows the note's own shape, so the tuning
    // colours stay untouched and overlapping notes in a chord stay apart.
    if (selection.count(shape.note) > 0)
    {
      g.setColour(juce::Colours::white.withAlpha(kSelectedOutlineAlpha));
      g.strokePath(body, juce::PathStrokeType(kSelectedOutlineWidth,
                                              juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    }
  }
}

int PolyphonicNoteLayer::noteAt(float worldX, float worldY)
{
  rebuildIfNeeded();
  if (!builtFor || coordMapper == nullptr)
    return -1;
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const float waveHeight = pixelsPerSemitone * 3.0f * coordMapper->getBlobHeightScale();
  const double seconds = worldX / pixelsPerSecond;
  const int frame = static_cast<int>(std::floor(
      seconds * builtFor->params.sampleRate / builtFor->params.hop));

  int best = -1;
  float bestDistance = 0.0f;
  for (const auto &shape : shapes)
  {
    const auto &note = builtFor->notes[static_cast<size_t>(shape.note)];
    if (frame < note.startFrame || frame > note.endFrame)
      continue;
    const float cy = coordMapper->midiToY(shape.medianMidi) + pixelsPerSemitone * 0.5f;
    // The blob, but never less than the note's own semitone row: thin notes
    // stay easy to pick, and overlapping notes go to the nearest pitch.
    const float reach = std::max(halfHeightAt(shape, frame, waveHeight),
                                 pixelsPerSemitone * 0.5f);
    const float distance = std::abs(worldY - cy);
    if (distance <= reach && (best < 0 || distance < bestDistance))
    {
      best = shape.note;
      bestDistance = distance;
    }
  }
  return best;
}

void PolyphonicNoteLayer::select(int note, bool additive)
{
  if (!additive)
    selection.clear();
  if (note < 0)
    return;
  if (additive && selection.count(note))
    selection.erase(note);
  else
    selection.insert(note);
}

void PolyphonicNoteLayer::selectAll()
{
  rebuildIfNeeded();
  selection.clear();
  for (const auto &shape : shapes)
    selection.insert(shape.note);
}
