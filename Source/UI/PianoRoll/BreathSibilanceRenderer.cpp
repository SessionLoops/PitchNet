#include "BreathSibilanceRenderer.h"

#include "../../Audio/BreathNotes.h"
#include "../../Audio/BreathSibilanceDetector.h"
#include "../../Audio/NoteTiming.h"
#include "VisualWaveformEnvelope.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace BreathSibilanceRenderer
{
namespace
{
constexpr float kHatchSpacing = 5.0f;

juce::Colour capsuleColour(std::uint8_t label)
{
  return label == breathsibilance::Breath ? juce::Colour(0xFF7FC8FFu)
                                          : juce::Colour(0xFFFFC46Bu);
}

// 45-degree lines across `area`, clipped to it.
void hatch(juce::Graphics &g, juce::Rectangle<float> area, juce::Colour colour)
{
  if (area.isEmpty())
    return;
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(area.getSmallestIntegerContainer());
  g.setColour(colour);
  const float height = area.getHeight();
  // Anchor the pattern to world x, so it does not crawl while scrolling or
  // when a run's edge moves.
  const float first =
      std::floor((area.getX() - height) / kHatchSpacing) * kHatchSpacing;
  for (float x = first; x < area.getRight(); x += kHatchSpacing)
    g.drawLine(x, area.getBottom(), x + height, area.getY(), 1.0f);
}

// Calls visit(start, end, label) for every run of equal noise labels inside
// [first, last) for which include(frame) holds.
template <typename Include, typename Visit>
void forEachNoiseRun(const std::vector<std::uint8_t> &labels, int first,
                     int last, Include include, Visit visit)
{
  first = std::max(0, first);
  last = std::min(last, static_cast<int>(labels.size()));
  int frame = first;
  while (frame < last)
  {
    const auto label = labels[static_cast<size_t>(frame)];
    if (!breathsibilance::isNoise(label) || !include(frame))
    {
      ++frame;
      continue;
    }
    const int start = frame;
    while (frame < last && labels[static_cast<size_t>(frame)] == label &&
           include(frame))
      ++frame;
    visit(start, frame, label);
  }
}

// Calls visit(left, right) with the x span of every noise run inside the
// note's body, [x, x + width]. Label frames go where the note plays them
// (NoteTiming.h), following a timing drag in progress: a stretched note
// keeps its consonants at their recorded width. Frame k is centred on its
// position, so it covers k +- 1/2.
template <typename Visit>
void forEachNoiseSpan(const AudioData &audioData, const Note &note, float x,
                      float width, Visit visit)
{
  const auto &labels = audioData.breathSibilance;
  const int sourceStart = note.getSrcStartFrame();
  const int sourceEnd = note.getSrcEndFrame();
  const float visualStart = note.getVisualStartFrame();
  const float visualLength = note.getVisualDurationFrames();
  if (labels.empty() || width <= 2.0f || sourceEnd <= sourceStart ||
      !(visualLength > 0.0f))
    return;
  const int first = std::max(0, sourceStart);
  const int last = std::min(sourceEnd, static_cast<int>(labels.size()));
  if (first >= last ||
      std::none_of(labels.begin() + first, labels.begin() + last,
                   [](std::uint8_t label)
                   { return breathsibilance::isNoise(label); }))
    return;

  const auto map = notetiming::forNoteOnScreen(audioData, note);
  const float pixelsPerFrame = width / visualLength;
  const auto xAt = [&](int sourceEdge)
  {
    return x + (static_cast<float>(map.destinationAt(sourceEdge)) - 0.5f -
                visualStart) *
                   pixelsPerFrame;
  };
  forEachNoiseRun(
      labels, sourceStart, sourceEnd, [](int) { return true; },
      [&](int start, int end, std::uint8_t) {
        const float left = std::max(x, xAt(start));
        const float right = std::min(x + width, xAt(end));
        if (right - left >= 1.0f)
          visit(left, right);
      });
}
} // namespace

juce::Path noiseBands(const AudioData &audioData, const Note &note, float x,
                      float width, float centreY, float minHeight)
{
  juce::Path bands;
  forEachNoiseSpan(audioData, note, x, width, [&](float left, float right) {
    bands.addRoundedRectangle(left, centreY - minHeight * 0.5f, right - left,
                              minHeight,
                              std::min(minHeight, right - left) * 0.5f);
  });
  return bands;
}

void hatchNoteBody(juce::Graphics &g, const AudioData &audioData,
                   const Note &note, const juce::Path &bodyShape, float x,
                   float width)
{
  if (bodyShape.isEmpty())
    return;
  // Most notes carry no noise frames; skip the path clip for them, it is the
  // expensive part and this runs for every visible note on every repaint.
  const auto &labels = audioData.breathSibilance;
  const int first = std::max(0, note.getSrcStartFrame());
  const int last =
      std::min(note.getSrcEndFrame(), static_cast<int>(labels.size()));
  if (first >= last ||
      std::none_of(labels.begin() + first, labels.begin() + last,
                   [](std::uint8_t label)
                   { return breathsibilance::isNoise(label); }))
    return;

  const auto bounds = bodyShape.getBounds();
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(bodyShape);
  forEachNoiseSpan(audioData, note, x, width, [&](float left, float right) {
    hatch(g, {left, bounds.getY(), right - left, bounds.getHeight()},
          juce::Colours::black.withAlpha(0.32f));
  });
}

namespace
{
// The band around its row a breath note answers clicks in.
float rowHeight(const CoordinateMapper &mapper)
{
  return std::max(5.0f, mapper.getPixelsPerSemitone() * 0.55f);
}

float rowCentre(const CoordinateMapper &mapper, const Note &anchor)
{
  return mapper.midiToY(anchor.getMidiNote()) +
         mapper.getPixelsPerSemitone() * 0.5f -
         anchor.getPitchOffset() * mapper.getPixelsPerSemitone();
}

double secondsPerFrame()
{
  return static_cast<double>(HOP_SIZE) / static_cast<double>(SAMPLE_RATE);
}

// The shape of noise outside the notes - a breath note, a stray sibilant:
// its waveform, like a note body, mirrored about the centre line, at least
// `minHalfHeight` either side so it stays visible. The envelope is read from
// the recording at the noise's source position, so it is right before and
// after an edit is rendered, and scaled by `gain`.
juce::Path noiseShape(const AudioData &audioData,
                      const breathnotes::Placement &placement,
                      const CoordinateMapper &mapper, float left, float right,
                      float centreY, float gain, float minHalfHeight)
{
  juce::Path shape;
  const float width = right - left;
  const bool fromRecording = audioData.originalWaveform.getNumSamples() > 0;
  const auto &buffer =
      fromRecording ? audioData.originalWaveform : audioData.waveform;
  const int total = buffer.getNumSamples();
  if (width < 2.0f || total <= 0 || buffer.getNumChannels() == 0 ||
      audioData.sampleRate <= 0)
  {
    shape.addRoundedRectangle(left, centreY - minHalfHeight,
                              std::max(2.0f, width), minHalfHeight * 2.0f,
                              minHalfHeight);
    return shape;
  }

  // The rendered audio already carries the level; the recording does not.
  const float firstFrame =
      fromRecording ? placement.left - placement.shift : placement.left;
  const float lastFrame =
      fromRecording ? placement.right - placement.shift : placement.right;
  const auto toSample = [&audioData, total](float frame)
  {
    return std::clamp(
        static_cast<int>(std::lround(frame * secondsPerFrame() *
                                     audioData.sampleRate)),
        0, total);
  };
  const int startSample = std::min(toSample(firstFrame), total - 1);
  const int endSample = std::max(startSample + 1, toSample(lastFrame));
  const int pointCount =
      std::max(2, std::min(2048, static_cast<int>(std::ceil(width)) + 1));
  // Only the noise's own samples: the envelope's window would otherwise
  // reach into the note it meets and draw that note's onset as a spike.
  const float *own = buffer.getReadPointer(0) + startSample;
  const auto values = VisualWaveformEnvelope::build(
      own, endSample - startSample, 0, endSample - startSample, pointCount,
      width, audioData.sampleRate, mapper.getPixelsPerSecond(), true, gain);

  const float halfHeight = mapper.getPixelsPerSemitone() * 3.0f *
                           mapper.getBlobHeightScale() * 0.5f;
  const auto half = [&](size_t i)
  { return std::max(minHalfHeight, values[i] * halfHeight); };
  const float step = width / static_cast<float>(values.size() - 1);
  shape.startNewSubPath(left, centreY - half(0));
  for (size_t i = 1; i < values.size(); ++i)
    shape.lineTo(left + step * static_cast<float>(i), centreY - half(i));
  for (size_t i = values.size(); i-- > 0;)
    shape.lineTo(left + step * static_cast<float>(i), centreY + half(i));
  shape.closeSubPath();
  return shape;
}

// Where a breath note is drawn, and its shape at level `db`.
struct BreathBody
{
  float left = 0.0f;
  float right = 0.0f;
  float centreY = 0.0f;
  juce::Path shape;
};

BreathBody breathBody(const AudioData &audioData,
                      const breathnotes::Placement &placement,
                      const CoordinateMapper &mapper, float db)
{
  BreathBody body;
  const float pixelsPerSecond = mapper.getPixelsPerSecond();
  body.left = static_cast<float>(placement.left * secondsPerFrame() *
                                 pixelsPerSecond);
  body.right = static_cast<float>(placement.right * secondsPerFrame() *
                                  pixelsPerSecond);
  body.centreY = rowCentre(mapper, *placement.anchor);
  // From the recording the level is applied here; the rendered audio
  // already carries the stored one.
  const bool fromRecording = audioData.originalWaveform.getNumSamples() > 0;
  const float storedDb = breathnotes::gainDb(audioData, placement);
  const float gain =
      std::pow(10.0f, (fromRecording ? db : db - storedDb) / 20.0f);
  // At the scale of the note bodies, so it reads as loud as it is, and at
  // least 1 px thick; the area around its row (breathArea) is what answers
  // the pointer.
  body.shape = noiseShape(audioData, placement, mapper, body.left, body.right,
                          body.centreY, gain, 0.5f);
  return body;
}

// Noise outside the notes - a breath note, a stray sibilant: its shape,
// hatched in `colour`.
void drawNoise(juce::Graphics &g, const juce::Path &shape, juce::Colour colour)
{
  g.setColour(colour.withAlpha(0.10f));
  g.fillPath(shape);
  {
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(shape);
    hatch(g, shape.getBounds(), colour.withAlpha(0.45f));
  }
  g.setColour(colour.withAlpha(0.50f));
  g.strokePath(shape, juce::PathStrokeType(1.0f));
}

// The whole area of a breath note: its shape, and the band around its row
// that answers the pointer even where a quiet breath is thin.
juce::Rectangle<float> breathArea(const BreathBody &body,
                                  const CoordinateMapper &mapper)
{
  const float reach = std::max(6.0f, rowHeight(mapper) * 0.5f);
  return juce::Rectangle<float>(body.left, body.centreY - reach,
                                std::max(1.0f, body.right - body.left),
                                reach * 2.0f)
      .getUnion(body.shape.getBounds().expanded(0.0f, 2.0f));
}

void drawLevelLabel(juce::Graphics &g, float centreX, float top, float db)
{
  const juce::String label =
      (db > 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
  const auto font =
      juce::Font(juce::FontOptions("Montserrat", "Regular", 11.0f))
          .withPointHeight(11.0f);
  const float width = std::max(
      60.0f,
      static_cast<float>(juce::GlyphArrangement::getStringWidthInt(font, label)) +
          16.0f);
  constexpr float height = 20.0f;
  const float x = centreX - width * 0.5f;
  const float y = top - height - 4.0f;
  g.setColour(juce::Colour(0xFF2E2E2Du));
  g.fillRoundedRectangle(x, y, width, height, 4.0f);
  g.setColour(juce::Colour(0xFFEFEFEFu));
  g.setFont(font);
  g.drawFittedText(label, static_cast<int>(x), static_cast<int>(y),
                   static_cast<int>(width), static_cast<int>(height),
                   juce::Justification::centred, 1);
}
} // namespace

void drawOutsideNotes(juce::Graphics &g, const Project &project,
                      const CoordinateMapper &mapper,
                      double visibleStartSeconds, double visibleEndSeconds,
                      const BreathView &view)
{
  const auto &audioData = project.getAudioData();
  const float pixelsPerSecond = mapper.getPixelsPerSecond();

  for (const auto &placement : breathnotes::place(project))
  {
    // A stray sibilant against a note is not drawn: it belongs to that note,
    // whose volume and sibilant balance it takes
    // (projectgain::appendStrayRegions).
    if (!placement.drawn || !placement.breath || placement.anchor == nullptr)
      continue;
    const double startSeconds = placement.left * secondsPerFrame();
    const double endSeconds = placement.right * secondsPerFrame();
    if (endSeconds < visibleStartSeconds || startSeconds > visibleEndSeconds)
      continue;
    const float left = static_cast<float>(startSeconds * pixelsPerSecond);
    const float right = static_cast<float>(endSeconds * pixelsPerSecond);

    // A breath note: its waveform, hatched in the colour of the note it
    // belongs to, at its level - the dragged one while it is dragged.
    const int key = placement.key();
    const bool hovered = view.hovered == key;
    const auto dragged = view.draggedDb ? view.draggedDb(key) : std::nullopt;
    const float db =
        dragged.value_or(breathnotes::gainDb(audioData, placement));
    const auto body = breathBody(audioData, placement, mapper, db);
    const auto &shape = body.shape;
    // Hovered or dragged, its whole area is highlighted, as a hovered note's.
    if (hovered)
    {
      g.setColour(juce::Colours::white.withAlpha(0.08f));
      g.fillRoundedRectangle(breathArea(body, mapper).expanded(2.0f), 3.0f);
    }
    drawNoise(g, shape,
              view.noteColour ? view.noteColour(*placement.anchor)
                              : capsuleColour(breathsibilance::Breath));
    if (dragged && view.labelled == key)
      drawLevelLabel(g, (left + right) * 0.5f, shape.getBounds().getY(), db);
  }
}

int breathAt(const Project &project, const CoordinateMapper &mapper,
             float worldX, float worldY)
{
  const float pixelsPerSecond = mapper.getPixelsPerSecond();
  // Anywhere in the area its highlight covers (breathArea): on its row - a
  // quiet breath is thin - or on its shape.
  const float reach = std::max(6.0f, rowHeight(mapper) * 0.5f);
  const auto &audioData = project.getAudioData();
  for (const auto &placement : breathnotes::place(project))
  {
    if (!placement.drawn || !placement.breath || placement.anchor == nullptr)
      continue;
    const float left =
        static_cast<float>(placement.left * secondsPerFrame() * pixelsPerSecond);
    const float right = static_cast<float>(placement.right * secondsPerFrame() *
                                           pixelsPerSecond);
    if (worldX < left || worldX > right)
      continue;
    if (std::abs(worldY - rowCentre(mapper, *placement.anchor)) <= reach)
      return placement.key();
    const auto body = breathBody(audioData, placement, mapper,
                                 breathnotes::gainDb(audioData, placement));
    if (breathArea(body, mapper).contains(worldX, worldY))
      return placement.key();
  }
  return -1;
}

} // namespace BreathSibilanceRenderer
