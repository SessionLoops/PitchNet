#include "PolyphonicNoteLayer.h"

#include "NoteColours.h"
#include "../../Utils/Constants.h"
#include "../../Utils/ScaleUtils.h"
#include "../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace notecolours;

namespace
{
// The selected note's outline.
constexpr float kSelectedOutlineWidth = 1.0f;
constexpr float kSelectedOutlineAlpha = 0.85f;
// A muted note: a dashed outline, brighter when selected.
constexpr float kMutedOutlineAlpha = 0.45f;
constexpr float kMutedOutlineWidth = 1.2f;

// A note drag locks to the direction of its first few pixels: mostly vertical
// changes only the pitch, mostly horizontal only the time. Moving clearly the
// other way too (beyond kCrossPixels) adds that axis for the rest of the drag.
constexpr float kDecidePixels = 6.0f;
constexpr float kCrossPixels = 40.0f;
// A note end grabs the mouse this far outside the note, and up to this far
// inside it (less for short notes, which keep their middle for moving).
constexpr float kEdgeOutsidePixels = 5.0f;
constexpr float kEdgeInsidePixels = 5.0f;
constexpr float kMaxSemitones = 36.0f;
constexpr double kMaxStretch = 4.0;
// A moved or stretched note stays this far inside the end of the take.
constexpr double kEndMarginSeconds = 0.05;

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

// How much louder a note is drawn for its gain edit (capped, so a boosted
// note does not swallow its neighbours).
float displayGain(float gainDb)
{
  return std::min(4.0f, std::pow(10.0f, gainDb / 20.0f));
}

void strokeDashed(juce::Graphics &g, const juce::Path &path, juce::Colour colour,
                  float width)
{
  juce::Path dashed;
  const float dashes[] = {4.0f, 3.0f};
  juce::PathStrokeType(width).createDashedStroke(dashed, path, dashes, 2);
  g.setColour(colour);
  g.fillPath(dashed);
}
/** What Cut or Copy took (see the header): the notes, each as its source
 *  note and the edit it sounded with, and the take they came from. */
struct NoteClipboard
{
  struct Entry
  {
    int source = -1;           // analysis note
    int startFrame = 0;        // the source's frames, to tell it is still the same note
    int endFrame = 0;
    polyphonic::NoteEdit edit; // as it sounded (copyOf cleared)
    double playedStart = 0.0;  // seconds, where it started
  };
  const Project *project = nullptr;
  std::vector<Entry> entries;
};

NoteClipboard &noteClipboard()
{
  static NoteClipboard clipboard;
  return clipboard;
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

void PolyphonicNoteLayer::setDragSnap(bool enabled,
                                      std::function<float(float, float)> snap)
{
  dragSnapEnabled = enabled;
  snapOffset = std::move(snap);
}

void PolyphonicNoteLayer::setTimeSnap(
    std::function<double(double, const juce::ModifierKeys &)> snap)
{
  snapTime = std::move(snap);
}

double PolyphonicNoteLayer::snappedDelta(double seconds, double dt,
                                         const juce::ModifierKeys &mods) const
{
  return snapTime ? snapTime(seconds + dt, mods) - seconds : dt;
}

void PolyphonicNoteLayer::setDoubleClickTarget(
    std::function<float(float, double, double)> target)
{
  doubleClickTarget = std::move(target);
}

void PolyphonicNoteLayer::rebuildIfNeeded()
{
  const auto take = project != nullptr ? project->getPolyphonicTake() : nullptr;
  const auto analysis = take != nullptr ? take->analysis : nullptr;
  if (analysis == builtFor)
    return;

  // A restored take replaces its restoring one with the same notes: keep the
  // selection (and a gesture in progress). Any other new take starts afresh.
  const bool sameNotes = builtFor && analysis && builtFor->notes.size() == analysis->notes.size();
  builtFor = analysis;
  shapes.clear();
  shapeIndex.clear();
  candidateShapes.clear();
  candidateIndex.clear();
  hoverNote = -1;
  splitHover = {};
  mergeHover = {};
  if (!sameNotes)
  {
    selection.clear();
    drag = Drag::none;
    working.clear();
    before.clear();
  }
  if (selectAfterRebuild && analysis)
  {
    // A split or merge: its notes are selected (their indices are the new
    // take's).
    selection.clear();
    for (int note : *selectAfterRebuild)
      if (note >= 0 && note < static_cast<int>(analysis->notes.size()))
        selection.insert(note);
    selectAfterRebuild.reset();
    drag = Drag::none;
  }
  if (sameNotes)
  {
    // A new assignment of the same notes: notes that are no longer active
    // leave the selection (only active notes are selected and edited).
    // Copies (keys below -1) that are gone or whose source is no longer
    // active go too.
    for (auto it = selection.begin(); it != selection.end();)
    {
      const int source = polyphonic::isCopyKey(*it) && project != nullptr
                             ? [&] {
                                 const auto &e = project->getPolyphonicEdits();
                                 const auto f = e.find(*it);
                                 return f != e.end() ? f->second.copyOf : -1;
                               }()
                             : *it;
      const bool keep = analysis && source >= 0 &&
                        source < static_cast<int>(analysis->notes.size()) &&
                        analysis->notes[static_cast<size_t>(source)].active;
      it = keep ? std::next(it) : selection.erase(it);
    }
    if (drag == Drag::assign)
      drag = Drag::none;
  }
  if (!analysis)
    return;

  const int numFrames = analysis->numFrames;
  std::vector<double> total(static_cast<size_t>(std::max(0, numFrames)), 0.0);
  shapeIndex.assign(analysis->notes.size(), -1);

  // Each active note's own level per frame (noteLevels: from its masks, or
  // the saved levels while an opened take is being restored).
  const auto levels = noteLevels(*take);
  for (size_t i = 0; i < analysis->notes.size(); ++i)
  {
    const auto &note = analysis->notes[i];
    if (!note.active)
      continue;
    NoteShape shape;
    shape.note = static_cast<int>(i);
    shape.medianMidi = note.medianMidi();
    shape.firstFrame = note.startFrame;
    shape.level.assign(static_cast<size_t>(std::max(1, note.numFrames())), 0.0f);
    if (i < levels.size())
      std::copy_n(levels[i].begin(), std::min(levels[i].size(), shape.level.size()),
                  shape.level.begin());
    for (size_t k = 0; k < shape.level.size(); ++k)
    {
      const int f = shape.firstFrame + static_cast<int>(k);
      if (f >= 0 && f < numFrames)
        total[static_cast<size_t>(f)] +=
            static_cast<double>(shape.level[k]) * shape.level[k];
    }
    shapeIndex[i] = static_cast<int>(shapes.size());
    shapes.push_back(std::move(shape));
  }

  // One scale for the take: the loudest moment of all notes together matches
  // the loudest moment of the mix's display envelope, so blob sizes compare
  // with vocal blobs and with each other. Measured on the original mix: the
  // waveform shows the edited audio once a render lands.
  const auto &audioData = project->getAudioData();
  const auto &mixSource = audioData.originalWaveform.getNumSamples() > 0
                              ? audioData.originalWaveform
                              : audioData.waveform;
  const auto mix = mixEnvelope(mixSource, audioData.sampleRate,
                               analysis->params.hop, numFrames);
  float mixPeak = 0.0f;
  double modelPeak = 0.0;
  for (int f = 0; f < numFrames; ++f)
  {
    mixPeak = std::max(mixPeak, mix[static_cast<size_t>(f)]);
    modelPeak = std::max(modelPeak, std::sqrt(total[static_cast<size_t>(f)]));
  }
  const float scale = modelPeak > 0.0 ? mixPeak / static_cast<float>(modelPeak) : 0.0f;
  std::vector<float> peaks;
  for (auto &shape : shapes)
  {
    for (auto &v : shape.level)
    {
      v = std::min(1.0f, v * scale);
      shape.peakLevel = std::max(shape.peakLevel, v);
    }
    if (shape.peakLevel > 0.0f)
      peaks.push_back(shape.peakLevel);
  }

  // A note without a level (none saved), or a candidate without an
  // estimated one (an older saved project), is drawn at a typical level: a little below the
  // median note's peak.
  float fallback = 0.3f;
  if (!peaks.empty())
  {
    std::nth_element(peaks.begin(), peaks.begin() + static_cast<long>(peaks.size() / 2),
                     peaks.end());
    fallback = std::max(0.08f, 0.6f * peaks[peaks.size() / 2]);
  }
  for (auto &shape : shapes)
    if (shape.peakLevel <= 0.0f)
    {
      std::fill(shape.level.begin(), shape.level.end(), fallback);
      shape.peakLevel = fallback;
    }
  buildCandidateShapes(levels, scale, fallback);
}

int PolyphonicNoteLayer::sourceOf(int key) const
{
  if (!polyphonic::isCopyKey(key))
    return key;
  const auto &edits = currentEdits();
  const auto it = edits.find(key);
  return it != edits.end() ? it->second.copyOf : -1;
}

std::vector<std::pair<int, const PolyphonicNoteLayer::NoteShape *>>
PolyphonicNoteLayer::drawnNotes() const
{
  std::vector<std::pair<int, const NoteShape *>> out;
  out.reserve(shapes.size());
  for (const auto &shape : shapes)
    out.emplace_back(shape.note, &shape);
  // The copies (pasted notes) after the notes: drawn on top, picked first.
  const auto &edits = currentEdits();
  for (auto it = edits.begin(); it != edits.end() && polyphonic::isCopyKey(it->first); ++it)
    if (const auto *shape = shapeFor(it->first))
      out.emplace_back(it->first, shape);
  return out;
}

void PolyphonicNoteLayer::dropMissingCopies()
{
  bool dropped = false;
  for (auto it = selection.begin(); it != selection.end() && polyphonic::isCopyKey(*it);)
  {
    if (assignmentMode || shapeFor(*it) == nullptr)
    {
      it = selection.erase(it);
      dropped = true;
    }
    else
      ++it;
  }
  if (dropped)
    selectionChanged();
}

const PolyphonicNoteLayer::NoteShape *PolyphonicNoteLayer::shapeFor(int key) const
{
  // A copy is drawn with its source note's shape.
  const int note = sourceOf(key);
  if (note < 0 || note >= static_cast<int>(shapeIndex.size()))
    return nullptr;
  const int index = shapeIndex[static_cast<size_t>(note)];
  return index >= 0 ? &shapes[static_cast<size_t>(index)] : nullptr;
}

const PolyphonicNoteLayer::Edits &PolyphonicNoteLayer::currentEdits() const
{
  static const Edits none;
  // Assignment mode shows (and plays) the take as analysed.
  if (assignmentMode)
    return none;
  if (drag == Drag::notes || drag == Drag::stretch || drag == Drag::preview)
    return working;
  return project != nullptr ? project->getPolyphonicEdits() : none;
}

polyphonic::NoteEdit PolyphonicNoteLayer::editOf(int note) const
{
  // Assignment mode shows the take as analysed: its edits come back when the
  // mode is left.
  if (assignmentMode)
    return {};
  const auto &edits = currentEdits();
  const auto it = edits.find(note);
  return it != edits.end() ? it->second : polyphonic::NoteEdit{};
}

float PolyphonicNoteLayer::waveHeight() const
{
  return coordMapper->getPixelsPerSemitone() * 3.0f * coordMapper->getBlobHeightScale();
}

float PolyphonicNoteLayer::rowCentre(float midi) const
{
  return coordMapper->midiToY(midi) + coordMapper->getPixelsPerSemitone() * 0.5f;
}

double PolyphonicNoteLayer::secondsPerFrame() const
{
  return builtFor ? builtFor->frameToSeconds(1.0) : 0.0;
}

double PolyphonicNoteLayer::takeSeconds() const
{
  return project != nullptr ? project->getAudioData().getDuration() : 0.0;
}

double PolyphonicNoteLayer::playedAt(int note, const polyphonic::NoteEdit &edit,
                                     double frame) const
{
  return builtFor->frameToSeconds(
             polyphonic::stretchMap(*builtFor, sourceOf(note), edit).outputFrame(frame)) +
         edit.timeOffsetSeconds;
}

float PolyphonicNoteLayer::halfHeightAt(const NoteShape &shape, int frame,
                                        float height, float gain) const
{
  const int k = juce::jlimit(0, static_cast<int>(shape.level.size()) - 1,
                             frame - shape.firstFrame);
  // At least half a pixel each side, so a near-silent stretch stays visible.
  return std::max(0.5f, std::min(1.0f, shape.level[static_cast<size_t>(k)] * gain) *
                            height * 0.5f);
}

juce::Rectangle<float> PolyphonicNoteLayer::noteBounds(int key, const NoteShape &shape) const
{
  const auto edit = editOf(key);
  const auto &note = builtFor->notes[static_cast<size_t>(shape.note)];
  const auto map = polyphonic::stretchMap(*builtFor, shape.note, edit);
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const double lastOut = map.cuts() ? map.end + 1.0 : map.outputFrame(note.endFrame + 1.0);
  const float x0 = static_cast<float>(
      (builtFor->frameToSeconds(note.startFrame) + edit.timeOffsetSeconds) * pixelsPerSecond);
  const float x1 = static_cast<float>(
      (builtFor->frameToSeconds(lastOut) + edit.timeOffsetSeconds) * pixelsPerSecond);
  const float half = std::max(
      0.5f, std::min(1.0f, shape.peakLevel * displayGain(edit.gainDb)) * waveHeight() * 0.5f);
  const float cy = rowCentre(shape.medianMidi + edit.semitones);
  return {x0, cy - half, std::max(1.0f, x1 - x0), 2.0f * half};
}

juce::Path PolyphonicNoteLayer::notePath(const NoteShape &shape,
                                         const polyphonic::NoteEdit &edit) const
{
  const auto &note = builtFor->notes[static_cast<size_t>(shape.note)];
  const auto map = polyphonic::stretchMap(*builtFor, shape.note, edit);
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float height = waveHeight();
  const float gain = displayGain(edit.gainDb);
  const float cy = rowCentre(shape.medianMidi + edit.semitones);
  const double frameWidth = secondsPerFrame() * pixelsPerSecond;

  // A shortened note is drawn up to where it is cut.
  const int last = map.cuts() ? juce::jlimit(note.startFrame, note.endFrame,
                                             static_cast<int>(std::floor(map.end)))
                              : note.endFrame;
  const auto xAt = [&](double frame) {
    return static_cast<float>(
        (builtFor->frameToSeconds(map.outputFrame(frame)) + edit.timeOffsetSeconds) *
        pixelsPerSecond);
  };
  // About a point per pixel when zoomed out.
  const int stride = std::max(1, static_cast<int>(std::floor(1.0 / std::max(1e-9, frameWidth))));

  std::vector<std::pair<float, float>> points; // x, half height
  for (int f = note.startFrame;; f = std::min(last, f + stride))
  {
    points.emplace_back(xAt(f), halfHeightAt(shape, f, height, gain));
    if (f == last)
      break;
  }
  // The closing edge, one (stretched) frame past the last one drawn.
  const float endX = map.cuts() ? xAt(last) + static_cast<float>(frameWidth) : xAt(last + 1.0);
  points.emplace_back(std::max(endX, points.back().first + 1.0f), points.back().second);

  // Top edge left to right, bottom edge back: symmetric about the pitch row.
  juce::Path body;
  body.startNewSubPath(points.front().first, cy - points.front().second);
  for (size_t j = 1; j < points.size(); ++j)
    body.lineTo(points[j].first, cy - points[j].second);
  for (size_t j = points.size(); j-- > 0;)
    body.lineTo(points[j].first, cy + points[j].second);
  body.closeSubPath();
  return body.createPathWithRoundedCorners(2.0f);
}

void PolyphonicNoteLayer::draw(juce::Graphics &g, int componentWidth)
{
  rebuildIfNeeded();
  if (!builtFor || coordMapper == nullptr)
    return;

  const float visibleStart = static_cast<float>(coordMapper->getScrollX());
  const float visibleEnd = visibleStart + static_cast<float>(componentWidth);
  if (assignmentMode && !outlineColour)
  {
    drawAssignment(g, visibleStart, visibleEnd);
    return;
  }
  if (shapes.empty())
    return;
  const int pitchReferenceHz = project->getPitchReferenceHz();

  for (const auto &[key, shapePtr] : drawnNotes())
  {
    const auto &shape = *shapePtr;
    const auto bounds = noteBounds(key, shape);
    if (bounds.getRight() < visibleStart || bounds.getX() > visibleEnd)
      continue;

    const auto edit = editOf(key);
    const auto body = notePath(shape, edit);
    const bool isSelected = selection.count(key) > 0;

    if (outlineColour)
    {
      if (edit.mute)
      {
        strokeDashed(g, body, *outlineColour, kMutedOutlineWidth);
      }
      else
      {
        g.setColour(*outlineColour);
        g.strokePath(body, juce::PathStrokeType(kSelectedOutlineWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
      }
      continue;
    }

    if (edit.mute)
    {
      strokeDashed(g, body,
                   juce::Colours::white.withAlpha(isSelected ? kSelectedOutlineAlpha
                                                             : kMutedOutlineAlpha),
                   kMutedOutlineWidth);
      continue;
    }

    const float midi = shape.medianMidi + edit.semitones;
    const auto colours = getNoteGradientColours(midi, pitchReferenceHz);
    setNoteGradientFill(g, body.getBounds(), rowCentre(midi), colours);
    g.fillPath(body);

    // Selection: an outline that follows the note's own shape, so the tuning
    // colours stay untouched and overlapping notes in a chord stay apart.
    if (isSelected)
    {
      g.setColour(juce::Colours::white.withAlpha(kSelectedOutlineAlpha));
      g.strokePath(body, juce::PathStrokeType(kSelectedOutlineWidth,
                                              juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    }
  }

  // Band selection, drawn like the vocal editor's box selection.
  if (drag == Drag::band && !band.isEmpty())
  {
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRect(band);
    g.setColour(juce::Colours::white.withAlpha(0.25f));
    g.drawRect(band, 1.0f);
  }
}

int PolyphonicNoteLayer::noteAt(float worldX, float worldY)
{
  rebuildIfNeeded();
  if (!builtFor || coordMapper == nullptr)
    return -1;
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const float height = waveHeight();
  const double seconds = worldX / pixelsPerSecond;
  const double framesPerSecond = 1.0 / secondsPerFrame();

  int best = -1;
  float bestDistance = 0.0f;
  for (const auto &[key, shapePtr] : drawnNotes())
  {
    const auto &shape = *shapePtr;
    const auto edit = editOf(key);
    const auto &note = builtFor->notes[static_cast<size_t>(shape.note)];
    const auto map = polyphonic::stretchMap(*builtFor, shape.note, edit);
    const double out = (seconds - edit.timeOffsetSeconds) * framesPerSecond;
    const double lastOut = map.cuts() ? map.end + 1.0 : map.outputFrame(note.endFrame + 1.0);
    if (out < note.startFrame || out > lastOut)
      continue;
    const int frame = juce::jlimit(note.startFrame, note.endFrame,
                                   static_cast<int>(std::floor(map.sourceFrame(out))));
    const float cy = rowCentre(shape.medianMidi + edit.semitones);
    // The blob, but never less than the note's own semitone row: thin notes
    // stay easy to pick, and overlapping notes go to the nearest pitch.
    const float reach = std::max(
        halfHeightAt(shape, frame, height, displayGain(edit.gainDb)),
        pixelsPerSemitone * 0.5f);
    const float distance = std::abs(worldY - cy);
    // A copy over its note (pasted in place) is the one picked: it is drawn on top.
    if (distance <= reach &&
        (best == -1 || distance < bestDistance ||
         (polyphonic::isCopyKey(key) && distance <= bestDistance)))
    {
      best = key;
      bestDistance = distance;
    }
  }
  return best;
}

bool PolyphonicNoteLayer::canStretch(int key, const polyphonic::NoteEdit &edit) const
{
  const int note = sourceOf(key);
  if (note < 0 || note >= static_cast<int>(builtFor->notes.size()))
    return false;
  const auto &n = builtFor->notes[static_cast<size_t>(note)];
  // Nothing after the attack to stretch, or nothing to hear.
  return !edit.mute && !n.f0.empty() &&
         n.endFrame - polyphonic::stretchMap(*builtFor, note, edit).anchor >= 2.0;
}

PolyphonicNoteLayer::Edge PolyphonicNoteLayer::edgeAt(float worldX, float worldY)
{
  Edge best;
  rebuildIfNeeded();
  if (!builtFor || coordMapper == nullptr)
    return best;
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  float bestDistance = std::numeric_limits<float>::max();

  const auto consider = [&](int key, const NoteShape &shape) {
    const auto edit = editOf(key);
    if (!canStretch(key, edit))
      return;
    const auto bounds = noteBounds(key, shape);
    const float x0 = bounds.getX();
    const float x1 = bounds.getRight();
    const float inside = std::min(kEdgeInsidePixels, 0.25f * (x1 - x0));
    const float reach = std::max(bounds.getHeight() * 0.5f, pixelsPerSemitone * 0.5f) + 2.0f;
    if (std::abs(worldY - bounds.getCentreY()) > reach)
      return;
    const auto check = [&](float x, bool start) {
      const float inward = start ? worldX - x : x - worldX;
      if (inward > inside || inward < -kEdgeOutsidePixels)
        return;
      if (std::abs(inward) < bestDistance)
      {
        bestDistance = std::abs(inward);
        best = {key, start};
      }
    };
    check(x0, true);
    check(x1, false);
  };

  // A selected note's ends first, so the note just picked is the one stretched.
  for (int note : selection)
    if (const auto *shape = shapeFor(note))
      consider(note, *shape);
  if (best.note != -1)
    return best;
  for (const auto &[key, shape] : drawnNotes())
    consider(key, *shape);
  return best;
}

bool PolyphonicNoteLayer::isOverStretchEdge(float worldX, float worldY)
{
  if (assignmentMode)
    return false; // nothing is stretched there
  return edgeAt(worldX, worldY).note != -1;
}

double PolyphonicNoteLayer::clampOffset(int key, double offset) const
{
  const int note = sourceOf(key);
  if (note < 0 || note >= static_cast<int>(builtFor->notes.size()))
    return offset;
  const double start =
      builtFor->frameToSeconds(builtFor->notes[static_cast<size_t>(note)].startFrame);
  const double latest = std::max(-start, takeSeconds() - start - kEndMarginSeconds);
  return juce::jlimit(-start, latest, offset);
}

void PolyphonicNoteLayer::mouseDown(float worldX, float worldY,
                                    const juce::ModifierKeys &mods)
{
  rebuildIfNeeded();
  if (!builtFor || coordMapper == nullptr || project == nullptr)
    return;
  if (assignmentMode)
  {
    assignmentMouseDown(worldX, worldY, mods);
    return;
  }

  const bool additive = mods.isShiftDown() || mods.isCommandDown();
  pendingSingle = -1;
  dragStart = {worldX, worldY};
  dragMoved = false;
  timeEngaged = false;
  pitchEngaged = false;
  before = project->getPolyphonicEdits();
  working = before;

  if (!additive)
  {
    if (const auto edge = edgeAt(worldX, worldY); edge.note != -1)
    {
      // The end of a selected note stretches the whole selection (every
      // note that can be stretched; deleted ones stay as they are); the end
      // of any other note selects it and stretches it alone.
      if (selection.count(edge.note) == 0)
      {
        selection = {edge.note};
        selectionChanged();
      }
      stretchTargets.clear();
      for (int note : selection)
        if (canStretch(note, editOf(note)))
          stretchTargets.push_back(note);
      drag = Drag::stretch;
      stretchNote = edge.note;
      stretchFromStart = edge.start;
      return;
    }
  }

  const int hit = noteAt(worldX, worldY);
  if (hit != -1)
  {
    if (additive)
    {
      if (selection.erase(hit) == 0)
        selection.insert(hit);
      selectionChanged();
      if (selection.count(hit) == 0)
        return; // deselected: nothing to drag
    }
    else if (selection.count(hit) == 0)
    {
      selection = {hit};
      selectionChanged();
    }
    else
    {
      pendingSingle = hit;
    }
    // A muted (deleted) note can be selected, to unmute or restore it, but
    // not edited: pressing it starts no drag.
    if (editOf(hit).mute)
    {
      if (!additive && pendingSingle != -1 && selection != std::set<int>{hit})
      {
        selection = {hit};
        selectionChanged();
      }
      pendingSingle = -1;
      return;
    }
    drag = Drag::notes;
    pressedNote = hit;
    return;
  }

  if (!additive && !selection.empty())
  {
    selection.clear();
    selectionChanged();
  }
  drag = Drag::band;
  band = {};
  bandBase = selection;
}

void PolyphonicNoteLayer::mouseDrag(float worldX, float worldY,
                                    const juce::ModifierKeys &mods)
{
  if (!builtFor || coordMapper == nullptr)
    return;
  switch (drag)
  {
    case Drag::notes:
      dragNotes(worldX, worldY, mods);
      break;
    case Drag::stretch:
      dragStretch(worldX, mods);
      break;
    case Drag::assign:
      assignmentMouseDrag(worldX, worldY);
      break;
    case Drag::listen:
      break;
    case Drag::band:
    {
      band = juce::Rectangle<float>(dragStart, {worldX, worldY});
      auto next = bandBase;
      for (const auto &[key, shape] : drawnNotes())
        if (noteBounds(key, *shape).intersects(band))
          next.insert(key);
      if (next != selection)
      {
        selection = std::move(next);
        selectionChanged();
      }
      break;
    }
    case Drag::none:
    case Drag::preview:
      break;
  }
}

void PolyphonicNoteLayer::dragNotes(float worldX, float worldY,
                                    const juce::ModifierKeys &mods)
{
  const float dx = worldX - dragStart.x;
  const float dy = worldY - dragStart.y;
  const float raw = -dy / coordMapper->getPixelsPerSemitone();

  // Snapped: whole-semitone (or in-scale) offsets from where the note was, so
  // a sharp note stays sharp. Unsnapped: cents. Alt inverts it for one drag.
  const auto pressedIt = before.find(pressedNote);
  const float pressedSemitones = pressedIt != before.end() ? pressedIt->second.semitones : 0.0f;
  const auto *pressedShape = shapeFor(pressedNote);
  const float anchor = (pressedShape ? pressedShape->medianMidi : 0.0f) + pressedSemitones;
  // Direction lock (see kDecidePixels): until the drag has a direction
  // nothing changes; then only its axis follows the mouse, until the other
  // axis is clearly moved as well.
  const float adx = std::abs(dx), ady = std::abs(dy);
  if (!pitchEngaged && !timeEngaged && std::max(adx, ady) > kDecidePixels)
  {
    pitchEngaged = ady >= adx;
    timeEngaged = adx > ady;
  }
  pitchEngaged = pitchEngaged || (timeEngaged && ady > kCrossPixels);
  timeEngaged = timeEngaged || (pitchEngaged && adx > kCrossPixels);

  float semis = 0.0f;
  if (pitchEngaged)
  {
    if (dragSnapEnabled != mods.isAltDown())
      semis = snapOffset ? snapOffset(raw, anchor) : std::round(raw);
    else
      semis = std::round(raw * 100.0f) / 100.0f;
  }
  double dt = timeEngaged ? dx / coordMapper->getPixelsPerSecond() : 0.0;
  // Drag Snap puts the pressed note's start on the grid; the rest of the
  // selection moves by the same time.
  if (timeEngaged && pressedNote != -1)
  {
    const auto pressedBase =
        pressedIt != before.end() ? pressedIt->second : polyphonic::NoteEdit{};
    const auto &n = builtFor->notes[static_cast<size_t>(sourceOf(pressedNote))];
    dt = snappedDelta(playedAt(pressedNote, pressedBase, n.startFrame), dt, mods);
  }

  bool changed = false;
  for (int note : selection)
  {
    const auto baseIt = before.find(note);
    const auto base = baseIt != before.end() ? baseIt->second : polyphonic::NoteEdit{};
    if (base.mute)
      continue; // muted notes are not edited
    auto &edit = working[note];
    const float s = juce::jlimit(-kMaxSemitones, kMaxSemitones, base.semitones + semis);
    const double o = clampOffset(note, base.timeOffsetSeconds + dt);
    changed = changed || s != edit.semitones || o != edit.timeOffsetSeconds;
    edit.semitones = s;
    edit.timeOffsetSeconds = o;
  }
  if (changed)
  {
    dragMoved = true;
    pendingSingle = -1;
    if (onEditsChanging)
      onEditsChanging();
  }
}

void PolyphonicNoteLayer::dragStretch(float worldX, const juce::ModifierKeys &mods)
{
  // Every note being stretched has the end that was grabbed moved by the same
  // time (the start, for the left end), each within its own limits. Drag Snap
  // puts the grabbed end of the grabbed note on the grid.
  double dt = (worldX - dragStart.x) / coordMapper->getPixelsPerSecond();
  if (stretchNote != -1)
  {
    const auto baseIt = before.find(stretchNote);
    const auto base = baseIt != before.end() ? baseIt->second : polyphonic::NoteEdit{};
    const auto &n = builtFor->notes[static_cast<size_t>(sourceOf(stretchNote))];
    dt = snappedDelta(playedAt(stretchNote, base,
                               stretchFromStart ? n.startFrame : n.endFrame),
                      dt, mods);
  }
  bool changed = false;
  for (int note : stretchTargets)
    changed = stretchNoteBy(note, dt) || changed;
  if (changed)
  {
    dragMoved = true;
    if (onEditsChanging)
      onEditsChanging();
  }
}

bool PolyphonicNoteLayer::stretchNoteBy(int i, double dt)
{
  // Made longer, only the part after the attack stretches (the attack keeps
  // its speed); made shorter, the note keeps that share of its length and is
  // cut, down to a single frame. Ported from the Polyphonic Lab.
  const int source = sourceOf(i); // a copy: the note it plays
  if (source < 0 || source >= static_cast<int>(builtFor->notes.size()))
    return false;
  const auto &n = builtFor->notes[static_cast<size_t>(source)];
  const auto baseIt = before.find(i);
  const auto base = baseIt != before.end() ? baseIt->second : polyphonic::NoteEdit{};
  const auto map = polyphonic::stretchMap(*builtFor, source, base);
  const double framesPerSecond = 1.0 / secondsPerFrame();
  const double span = n.endFrame - map.anchor;
  const double whole = n.endFrame - n.startFrame;
  if (span < 2.0 || whole <= 0.0)
    return false;
  const double shortest = std::ceil(100.0 / whole) / 100.0; // one frame
  // The end moves as far as the mouse (dt).
  const double end = playedAt(i, base, n.endFrame);
  double ratio = 1.0;
  double offset = base.timeOffsetSeconds;
  if (stretchFromStart)
  {
    // The left end: the note still ends where it did and its start, attack
    // included, follows the mouse. framesAt(r) is how long the note lasts at
    // stretch r, ratioFor(t) the stretch that makes it start at t.
    const auto framesAt = [&](double r) {
      return r < 1.0 ? r * whole : (map.anchor - n.startFrame) + r * span;
    };
    const auto ratioFor = [&](double start) {
      const double frames = (end - start) * framesPerSecond;
      return frames < whole ? frames / whole : (frames - (map.anchor - n.startFrame)) / span;
    };
    ratio = std::round(juce::jlimit(shortest, kMaxStretch,
                                    ratioFor(playedAt(i, base, n.startFrame) + dt)) *
                       100.0) /
            100.0;
    // Stay inside the take.
    ratio = std::max(shortest, std::min(ratio, std::floor(ratioFor(0.0) * 100.0) / 100.0));
    // Whatever the stretch adds to the length comes off the start (exactly the
    // old offset when the stretch is unchanged, so pressing the end without
    // dragging it edits nothing).
    const double stored = static_cast<float>(ratio); // the edit keeps it as a float
    offset = clampOffset(i, base.timeOffsetSeconds -
                                (framesAt(stored) - framesAt(map.ratio)) / framesPerSecond);
  }
  else
  {
    // The right end: the start stays put. `target` is the output frame the
    // end should land on. It may pass the end of the recording: the render,
    // and with it the take, then grows to fit (renderChannels in
    // EditorController).
    const double target = (end + dt - base.timeOffsetSeconds) * framesPerSecond;
    ratio = target < n.endFrame ? (target - n.startFrame) / whole
                                : (target - map.anchor) / span;
    ratio = std::round(juce::jlimit(shortest, kMaxStretch, ratio) * 100.0) / 100.0;
  }

  auto &edit = working[i];
  if (static_cast<float>(ratio) == edit.stretch && offset == edit.timeOffsetSeconds)
    return false;
  edit.stretch = static_cast<float>(ratio);
  edit.timeOffsetSeconds = offset;
  return true;
}

void PolyphonicNoteLayer::mouseUp()
{
  if (drag == Drag::assign)
  {
    assignmentMouseUp();
    return;
  }
  if (drag == Drag::listen)
  {
    drag = Drag::none;
    pressedNote = -1;
    return;
  }
  const auto finished = drag;
  const bool moved = dragMoved;
  const int single = pendingSingle;
  const bool several = stretchTargets.size() > 1;
  // The gesture is over before the edits go to the project, so drawing and
  // hit tests read the project's edits again.
  drag = Drag::none;
  band = {};
  pressedNote = -1;
  stretchNote = -1;
  stretchTargets.clear();
  pendingSingle = -1;

  if (finished == Drag::notes || finished == Drag::stretch)
  {
    if (moved)
      commit(finished == Drag::stretch ? (several ? "Stretch Notes" : "Stretch Note")
                                       : "Move Notes");
    else if (finished == Drag::notes && single != -1 && selection != std::set<int>{single})
    {
      selection = {single};
      selectionChanged();
    }
  }
  working.clear();
}

bool PolyphonicNoteLayer::mouseDoubleClick(float worldX, float worldY)
{
  if (assignmentMode)
    return assignmentDoubleClick(worldX, worldY);
  const int hit = noteAt(worldX, worldY);
  if (hit == -1 || !doubleClickTarget || project == nullptr)
    return false;
  const auto *shape = shapeFor(hit);
  if (shape == nullptr)
    return false;

  const auto edit = editOf(hit);
  if (edit.mute)
  {
    // A deleted note comes back, as Delete does: with the rest of the
    // selection when every selected note is deleted, else on its own.
    bool allMuted = selection.count(hit) > 0;
    for (int note : selection)
      allMuted = allMuted && editOf(note).mute;
    if (!allMuted)
    {
      selection = {hit};
      selectionChanged();
    }
    toggleMuteSelection();
    return true;
  }
  const auto bounds = noteBounds(hit, *shape);
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();
  const float pitch = shape->medianMidi + edit.semitones;
  const float target = doubleClickTarget(pitch, bounds.getX() / pixelsPerSecond,
                                         bounds.getRight() / pixelsPerSecond);

  before = project->getPolyphonicEdits();
  working = before;
  working[hit].semitones =
      juce::jlimit(-kMaxSemitones, kMaxSemitones, edit.semitones + (target - pitch));
  if (selection != std::set<int>{hit})
  {
    selection = {hit};
    selectionChanged();
  }
  commit("Snap Note Pitch");
  working.clear();
  return true;
}

std::vector<int> PolyphonicNoteLayer::getDraggedNotes() const
{
  // Assignment mode: a held note's own sound, while it is not about to snap
  // anywhere and its masks exist (getAssignmentAudition).
  if (assignmentMode)
  {
    const auto audition = getAssignmentAudition();
    return audition.separated ? std::vector<int>{audition.note} : std::vector<int>{};
  }
  if (drag == Drag::notes)
  {
    std::vector<int> notes;
    for (int note : selection)
      if (!editOf(note).mute)
        notes.push_back(note);
    return notes;
  }
  if (drag == Drag::stretch)
    return stretchTargets;
  return {};
}

void PolyphonicNoteLayer::toggleMuteSelection()
{
  if (assignmentMode)
  {
    deactivateSelection(); // Delete removes notes from the detection there
    return;
  }
  deleteSelection(false, {});
}

void PolyphonicNoteLayer::restoreSelection()
{
  if (assignmentMode || project == nullptr || selection.empty() || drag != Drag::none)
    return;
  before = project->getPolyphonicEdits();
  working = before;
  // Pasted notes (copies) go too: the original has none.
  for (int note : selection)
    working.erase(note);
  for (auto it = selection.begin(); it != selection.end();)
    it = polyphonic::isCopyKey(*it) ? selection.erase(it) : std::next(it);
  commit("Restore Original");
  working.clear();
  selectionChanged();
}

std::vector<int> PolyphonicNoteLayer::selectionInTimeOrder() const
{
  // By where each starts playing (a copy where it was pasted).
  std::vector<std::pair<double, int>> order;
  if (builtFor == nullptr)
    return {};
  for (int key : selection)
  {
    const int source = sourceOf(key);
    if (source < 0 || source >= static_cast<int>(builtFor->notes.size()))
      continue;
    order.emplace_back(playedAt(key, editOf(key), builtFor->notes[static_cast<size_t>(source)].startFrame),
                       key);
  }
  std::stable_sort(order.begin(), order.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<int> notes;
  for (const auto &entry : order)
    notes.push_back(entry.second);
  return notes;
}

bool PolyphonicNoteLayer::canCopySelection() const
{
  if (assignmentMode || project == nullptr || builtFor == nullptr || drag != Drag::none)
    return false;
  // Deleted (muted) notes are not copied: something that sounds is needed.
  for (int key : selection)
    if (!editOf(key).mute && shapeFor(key) != nullptr)
      return true;
  return false;
}

bool PolyphonicNoteLayer::canPaste() const
{
  if (assignmentMode || project == nullptr || builtFor == nullptr || drag != Drag::none)
    return false;
  // Only into the take the notes were copied from, while they are the same notes.
  const auto &clipboard = noteClipboard();
  if (clipboard.project != project)
    return false;
  for (const auto &entry : clipboard.entries)
  {
    if (entry.source < 0 || entry.source >= static_cast<int>(builtFor->notes.size()))
      continue;
    const auto &n = builtFor->notes[static_cast<size_t>(entry.source)];
    if (n.active && n.startFrame == entry.startFrame && n.endFrame == entry.endFrame)
      return true;
  }
  return false;
}

void PolyphonicNoteLayer::copySelection()
{
  if (!canCopySelection())
    return;
  NoteClipboard copied;
  copied.project = project;
  for (int key : selectionInTimeOrder())
  {
    auto edit = editOf(key);
    const int source = sourceOf(key);
    if (edit.mute || source < 0)
      continue;
    const auto &n = builtFor->notes[static_cast<size_t>(source)];
    NoteClipboard::Entry entry;
    entry.source = source;
    entry.startFrame = n.startFrame;
    entry.endFrame = n.endFrame;
    entry.playedStart = playedAt(key, edit, n.startFrame);
    edit.copyOf = -1;
    entry.edit = edit;
    copied.entries.push_back(std::move(entry));
  }
  if (!copied.entries.empty())
    noteClipboard() = std::move(copied);
}

void PolyphonicNoteLayer::cutSelection()
{
  if (!canCopySelection())
    return;
  copySelection();
  deleteSelection(true, "Cut Notes");
}

void PolyphonicNoteLayer::deleteSelection(bool muteAll, const juce::String &name)
{
  if (assignmentMode || project == nullptr || selection.empty() || drag != Drag::none)
    return;
  // Copies (pasted notes) go; the notes themselves are muted (Delete), or
  // unmuted when every one of them already is.
  bool allMuted = !muteAll;
  bool anyNote = false;
  for (int key : selection)
    if (!polyphonic::isCopyKey(key))
    {
      anyNote = true;
      allMuted = allMuted && editOf(key).mute;
    }
  before = project->getPolyphonicEdits();
  working = before;
  bool removedCopies = false;
  for (int key : selection)
  {
    if (polyphonic::isCopyKey(key))
    {
      removedCopies = working.erase(key) > 0 || removedCopies;
      continue;
    }
    working[key].mute = !allMuted;
  }
  for (auto it = selection.begin(); it != selection.end();)
    it = polyphonic::isCopyKey(*it) ? selection.erase(it) : std::next(it);
  const juce::String undoName =
      name.isNotEmpty() ? name
                        : (!anyNote ? juce::String("Delete Notes")
                                    : (allMuted ? "Unmute Notes" : "Mute Notes"));
  commit(undoName);
  working.clear();
  if (removedCopies)
    selectionChanged();
}

void PolyphonicNoteLayer::pasteAt(double seconds)
{
  if (!canPaste())
    return;
  const auto &clipboard = noteClipboard();
  // The earliest note starts at `seconds`; the others keep their places
  // from it, their pitches, levels and lengths.
  std::vector<const NoteClipboard::Entry *> entries;
  double first = std::numeric_limits<double>::max();
  for (const auto &entry : clipboard.entries)
  {
    if (entry.source < 0 || entry.source >= static_cast<int>(builtFor->notes.size()))
      continue;
    const auto &n = builtFor->notes[static_cast<size_t>(entry.source)];
    if (!n.active || n.startFrame != entry.startFrame || n.endFrame != entry.endFrame)
      continue;
    entries.push_back(&entry);
    first = std::min(first, entry.playedStart);
  }
  if (entries.empty())
    return;
  const double shift = std::max(0.0, seconds) - first;

  before = project->getPolyphonicEdits();
  working = before;
  std::set<int> pasted;
  for (const auto *entry : entries)
  {
    const int key = polyphonic::nextCopyKey(working);
    auto edit = entry->edit;
    edit.copyOf = entry->source;
    edit.mute = false;
    // kept inside the take, as a dragged note is
    edit.timeOffsetSeconds = clampOffset(entry->source, edit.timeOffsetSeconds + shift);
    working[key] = edit;
    pasted.insert(key);
  }
  commit(entries.size() > 1 ? "Paste Notes" : "Paste Note");
  working.clear();
  // The pasted notes are selected, to be moved on from there.
  selection = std::move(pasted);
  selectionChanged();
}

namespace
{
bool hasAspect(int key, const polyphonic::NoteEdit &edit,
               PolyphonicNoteLayer::RestoreAspect aspect)
{
  using Aspect = PolyphonicNoteLayer::RestoreAspect;
  switch (aspect)
  {
    case Aspect::pitch: return edit.semitones != 0.0f || !edit.pitchCurve.empty();
    case Aspect::amplitude: return edit.gainDb != 0.0f;
    case Aspect::timing:
      return !polyphonic::isCopyKey(key) &&
             (edit.timeOffsetSeconds != 0.0 || edit.stretch != 1.0f);
    case Aspect::all: return polyphonic::isCopyKey(key) || !edit.isIdentity();
  }
  return false;
}
} // namespace

bool PolyphonicNoteLayer::selectionHasEdits(RestoreAspect aspect) const
{
  if (assignmentMode || project == nullptr)
    return false;
  const auto &edits = project->getPolyphonicEdits();
  for (int key : selection)
    if (const auto it = edits.find(key); it != edits.end() && hasAspect(key, it->second, aspect))
      return true;
  return false;
}

void PolyphonicNoteLayer::restoreSelection(RestoreAspect aspect)
{
  if (aspect == RestoreAspect::all)
  {
    restoreSelection();
    return;
  }
  if (assignmentMode || project == nullptr || selection.empty() || drag != Drag::none)
    return;
  before = project->getPolyphonicEdits();
  working = before;
  for (int key : selection)
  {
    const auto it = working.find(key);
    if (it == working.end() || !hasAspect(key, it->second, aspect))
      continue;
    auto &edit = it->second;
    if (aspect == RestoreAspect::pitch)
    {
      edit.semitones = 0.0f;
      edit.pitchCurve.clear();
    }
    else if (aspect == RestoreAspect::amplitude)
    {
      edit.gainDb = 0.0f;
    }
    else
    {
      edit.timeOffsetSeconds = 0.0;
      edit.stretch = 1.0f;
    }
  }
  // An edit left with nothing goes (setPolyphonicEdits drops identity edits).
  commit(aspect == RestoreAspect::pitch       ? "Restore Pitch"
         : aspect == RestoreAspect::amplitude ? "Restore Amplitude"
                                              : "Restore Timing");
  working.clear();
}

void PolyphonicNoteLayer::setSelectionGainDb(float gainDb)
{
  if (assignmentMode || project == nullptr || selection.empty() || drag != Drag::none)
    return;
  before = project->getPolyphonicEdits();
  working = before;
  for (int note : selection)
    if (!working[note].mute)
      working[note].gainDb = gainDb;
  commit("Note Level");
  working.clear();
}

void PolyphonicNoteLayer::shiftSelectionSemitones(float semitones,
                                                  const juce::String &undoName)
{
  if (assignmentMode || project == nullptr || selection.empty() || drag != Drag::none || semitones == 0.0f)
    return;
  before = project->getPolyphonicEdits();
  working = before;
  for (int note : selection)
  {
    auto &edit = working[note];
    if (!edit.mute)
      edit.semitones = juce::jlimit(-kMaxSemitones, kMaxSemitones, edit.semitones + semitones);
  }
  commit(undoName);
  working.clear();
}

void PolyphonicNoteLayer::previewSelectionShift(float semitones)
{
  if (assignmentMode || project == nullptr || selection.empty() || (drag != Drag::none && drag != Drag::preview))
    return;
  if (drag == Drag::none)
    before = project->getPolyphonicEdits();
  drag = Drag::preview;
  working = before;
  for (int note : selection)
  {
    auto &edit = working[note];
    if (!edit.mute)
      edit.semitones = juce::jlimit(-kMaxSemitones, kMaxSemitones, edit.semitones + semitones);
  }
}

void PolyphonicNoteLayer::previewSelectionGain(float gainDb)
{
  if (assignmentMode || project == nullptr || selection.empty() || (drag != Drag::none && drag != Drag::preview))
    return;
  if (drag == Drag::none)
    before = project->getPolyphonicEdits();
  drag = Drag::preview;
  working = before;
  for (int note : selection)
    if (!working[note].mute)
      working[note].gainDb = gainDb;
}

void PolyphonicNoteLayer::endPreview()
{
  if (drag != Drag::preview)
    return;
  drag = Drag::none;
  working.clear();
}

std::optional<PolyphonicNoteLayer::PitchReadout>
PolyphonicNoteLayer::getSelectionPitch(int referenceHz) const
{
  const float offset = ScaleUtils::getReferenceOffsetSemitones(referenceHz);
  std::optional<PitchReadout> readout;
  int firstNearest = 0;
  int firstCents = 0;
  for (int note : selection)
  {
    const auto edit = editOf(note);
    const auto *shape = shapeFor(note);
    if (edit.mute || shape == nullptr)
      continue;
    const float midi = shape->medianMidi + edit.semitones;
    // Nearest semitone on the reference grid and the cents from it (-50...+49).
    const int totalCents = static_cast<int>(std::lround((midi - offset) * 100.0f));
    const int nearest = static_cast<int>(std::floor((totalCents + 50) / 100.0));
    const int cents = totalCents - nearest * 100;
    if (!readout)
    {
      readout = PitchReadout{midi, false, false};
      firstNearest = nearest;
      firstCents = cents;
      continue;
    }
    readout->notesDiffer = readout->notesDiffer || nearest != firstNearest;
    readout->centsDiffer = readout->centsDiffer || cents != firstCents;
  }
  return readout;
}

std::optional<float> PolyphonicNoteLayer::getSelectionGainDb() const
{
  // Muted notes are not edited, so only the others count.
  std::optional<float> common;
  for (int note : selection)
  {
    const auto edit = editOf(note);
    if (edit.mute)
      continue;
    if (!common)
      common = edit.gainDb;
    else if (*common != edit.gainDb)
      return std::numeric_limits<float>::quiet_NaN();
  }
  return common;
}

void PolyphonicNoteLayer::commit(const juce::String &name)
{
  if (project == nullptr)
    return;
  const Edits old = before;
  project->setPolyphonicEdits(working);
  const auto &now = project->getPolyphonicEdits();
  if (now != old && onEditsCommitted)
    onEditsCommitted(old, now, name);
}

void PolyphonicNoteLayer::select(int note, bool additive)
{
  if (!additive)
    selection.clear();
  if (note != -1) // -1: none (a copy's key is below it)
  {
    if (additive && selection.count(note))
      selection.erase(note);
    else
      selection.insert(note);
  }
  selectionChanged();
}

void PolyphonicNoteLayer::selectAll()
{
  rebuildIfNeeded();
  selection.clear();
  for (const auto &[key, shape] : drawnNotes())
    if (!assignmentMode || !polyphonic::isCopyKey(key))
      selection.insert(key);
  selectionChanged();
}

void PolyphonicNoteLayer::clearSelection()
{
  if (selection.empty())
    return;
  selection.clear();
  selectionChanged();
}

void PolyphonicNoteLayer::selectionChanged()
{
  if (onSelectionChanged)
    onSelectionChanged();
}
