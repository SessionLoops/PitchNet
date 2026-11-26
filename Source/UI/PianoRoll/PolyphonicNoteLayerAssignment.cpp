// Note assignment mode of PolyphonicNoteLayer (see the class comment): the
// detection itself is edited, Melodyne-style. Notes are drawn as analysed
// (editOf() hides the edits), candidates as dashed outlines, like deleted notes.

#include "PolyphonicNoteLayer.h"

#include "NoteColours.h"
#include "../../Models/PolyphonicAssignment.h"
#include "../../Utils/Constants.h"

#include <algorithm>
#include <cmath>

using namespace notecolours;

namespace
{
// The canvas tint that says the mode is on (Melodyne changes the editor's
// background colour the same way).
const juce::Colour kAssignmentTint = juce::Colour(0xff2f6fb0).withAlpha(0.10f);
// Candidates look like deleted (muted) notes in edit mode: a dashed white
// outline (PolyphonicNoteLayer.cpp), at the selected strength when they belong
// to the focused note or are under the pointer.
constexpr float kCandidateAlpha = 0.45f;        // as kMutedOutlineAlpha
constexpr float kRelatedCandidateAlpha = 0.85f; // as kSelectedOutlineAlpha
constexpr float kCandidateWidth = 1.2f;         // as kMutedOutlineWidth
constexpr float kHoveredCandidateWidth = 1.6f;
// A note being dragged to another pitch: its ghost, and the target it snaps to.
const juce::Colour kTargetColour = juce::Colour(0xff7fd4ff);
// Harmonic guides of the focused note: its partials 2..8 above it and the
// pitches it would be a partial of (/2, /3) below it. Dotted, so they do not
// read as (dashed) candidates.
constexpr int kMaxGuideHarmonic = 8;
constexpr float kGuideAlpha = 0.3f;
// A candidate belongs to a note when it sits within this far of one of the
// note's harmonics or subharmonics up to kMaxRelatedHarmonic.
constexpr int kMaxRelatedHarmonic = 8;
constexpr float kRelatedCents = 50.0f;
// Candidates at the same pitch as an overlapping note or more probable
// candidate (fragments of one sound) are shown once.
constexpr float kDuplicateSemitones = 0.5f;
// Where a dragged note snaps: a candidate within this far of the pointer's
// pitch, else one of its own (sub)harmonics within kHarmonicSnapSemitones.
constexpr float kCandidateSnapSemitones = 0.6f;
constexpr float kHarmonicSnapSemitones = 0.5f;
constexpr float kDecidePixels = 6.0f;
// Two notes overlap when they share at least this share of the shorter one.
constexpr double kOverlapShare = 0.5;
constexpr float kRatios[] = {2.0f, 3.0f, 4.0f, 1.0f / 2.0f, 1.0f / 3.0f, 1.0f / 4.0f};

float semitonesOf(float ratio) { return 12.0f * std::log2(ratio); }

void strokeDashed(juce::Graphics &g, const juce::Path &path, juce::Colour colour, float width)
{
  juce::Path dashed;
  const float dashes[] = {4.0f, 3.0f};
  juce::PathStrokeType(width).createDashedStroke(dashed, path, dashes, 2);
  g.setColour(colour);
  g.fillPath(dashed);
}
} // namespace

void PolyphonicNoteLayer::setAssignmentMode(bool enabled)
{
  if (assignmentMode == enabled)
    return;
  assignmentMode = enabled;
  // Whatever gesture was under way belongs to the other mode.
  drag = Drag::none;
  working.clear();
  band = {};
  pressedNote = -1;
  stretchNote = -1;
  stretchTargets.clear();
  pendingSingle = -1;
  hoverNote = -1;
  assignTarget = {};
  // Pasted notes (copies) are not part of the detection: they leave the
  // selection there.
  dropMissingCopies();
}

void PolyphonicNoteLayer::setCandidateThreshold(float probability)
{
  probability = juce::jlimit(0.0f, 1.0f, probability);
  if (probability == candidateThreshold)
    return;
  candidateThreshold = probability;
  updateVisibleCandidates();
  hoverNote = -1;
}

void PolyphonicNoteLayer::setThresholdPreview(std::optional<float> probability)
{
  thresholdPreview = probability;
  hoverNote = -1;
}

bool PolyphonicNoteLayer::shownActive(int note) const
{
  const auto &n = builtFor->notes[static_cast<size_t>(note)];
  return thresholdPreview ? !n.f0.empty() && n.probability >= *thresholdPreview : n.active;
}

void PolyphonicNoteLayer::commitThreshold(float probability)
{
  thresholdPreview.reset();
  if (!assignmentMode || !builtFor || drag != Drag::none)
    return;
  auto notes = polyphonicassignment::notesAtThreshold(builtFor->notes, probability);
  commitAssignment(std::move(notes), "Note Assignment Threshold", probability);
}

void PolyphonicNoteLayer::updateVisibleCandidates()
{
  candidateVisible.assign(candidateShapes.size(), false);
  if (!builtFor)
    return;
  const float threshold = candidateThreshold;
  const auto &notes = builtFor->notes;
  const auto sameSound = [&](const NoteShape &a, const NoteShape &b) {
    return std::abs(a.medianMidi - b.medianMidi) < kDuplicateSemitones &&
           overlapsInTime(a.note, b.note);
  };
  for (size_t i = 0; i < candidateShapes.size(); ++i)
  {
    const auto &c = candidateShapes[i];
    const float p = notes[static_cast<size_t>(c.note)].probability;
    if (p < threshold)
      continue;
    bool duplicate = false;
    for (const auto &active : shapes)
      if ((duplicate = sameSound(c, active)))
        break;
    for (size_t j = 0; !duplicate && j < candidateShapes.size(); ++j)
    {
      if (j == i)
        continue;
      const auto &d = candidateShapes[j];
      const float q = notes[static_cast<size_t>(d.note)].probability;
      duplicate = q >= threshold && (q > p || (q == p && j < i)) && sameSound(c, d);
    }
    candidateVisible[i] = !duplicate;
  }
}

void PolyphonicNoteLayer::buildCandidateShapes(const std::vector<std::vector<float>> &levels,
                                               float scale, float fallbackLevel)
{
  candidateShapes.clear();
  candidateIndex.assign(builtFor ? builtFor->notes.size() : 0, -1);
  if (!builtFor)
    return;
  for (size_t i = 0; i < builtFor->notes.size(); ++i)
  {
    const auto &note = builtFor->notes[i];
    if (note.active || note.f0.empty())
      continue;
    // Shaped like a deleted note: by its level per frame, here the one
    // estimated from the spectral peaks at its partials (it has no masks), on
    // the take's scale. Without one (an older saved project): an even outline.
    NoteShape shape;
    shape.note = static_cast<int>(i);
    shape.medianMidi = note.medianMidi();
    shape.firstFrame = note.startFrame;
    shape.level.assign(static_cast<size_t>(std::max(1, note.numFrames())), 0.0f);
    if (i < levels.size())
      for (size_t k = 0; k < shape.level.size() && k < levels[i].size(); ++k)
        shape.level[k] = std::min(1.0f, levels[i][k] * scale);
    for (float v : shape.level)
      shape.peakLevel = std::max(shape.peakLevel, v);
    if (shape.peakLevel <= 0.0f)
    {
      std::fill(shape.level.begin(), shape.level.end(), fallbackLevel);
      shape.peakLevel = fallbackLevel;
    }
    candidateIndex[i] = static_cast<int>(candidateShapes.size());
    candidateShapes.push_back(std::move(shape));
  }
  updateVisibleCandidates();
}

bool PolyphonicNoteLayer::overlapsInTime(int a, int b) const
{
  const auto &na = builtFor->notes[static_cast<size_t>(a)];
  const auto &nb = builtFor->notes[static_cast<size_t>(b)];
  const int shared = std::min(na.endFrame, nb.endFrame) - std::max(na.startFrame, nb.startFrame) + 1;
  const int shorter = std::min(na.numFrames(), nb.numFrames());
  return shared > 0 && shared >= kOverlapShare * shorter;
}

bool PolyphonicNoteLayer::harmonicallyRelated(int a, int b) const
{
  if (!overlapsInTime(a, b))
    return false;
  const float cents = 100.0f * (builtFor->notes[static_cast<size_t>(b)].medianMidi() -
                                builtFor->notes[static_cast<size_t>(a)].medianMidi());
  for (int h = 1; h <= kMaxRelatedHarmonic; ++h)
  {
    const float step = 1200.0f * std::log2(static_cast<float>(h));
    if (std::abs(std::abs(cents) - step) < kRelatedCents)
      return true;
  }
  return false;
}

int PolyphonicNoteLayer::assignmentHitAt(float worldX, float worldY) const
{
  if (!builtFor || coordMapper == nullptr)
    return -1;
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  const float height = waveHeight();
  const double frame = worldX / coordMapper->getPixelsPerSecond() / secondsPerFrame();

  int best = -1;
  float bestDistance = 0.0f;
  const auto consider = [&](const NoteShape &shape) {
    const auto &note = builtFor->notes[static_cast<size_t>(shape.note)];
    if (frame < note.startFrame || frame > note.endFrame + 1.0)
      return;
    const int f = juce::jlimit(note.startFrame, note.endFrame, static_cast<int>(std::floor(frame)));
    const float reach = std::max(halfHeightAt(shape, f, height, 1.0f), pixelsPerSemitone * 0.5f);
    const float distance = std::abs(worldY - rowCentre(shape.medianMidi));
    if (distance <= reach && (best < 0 || distance < bestDistance))
    {
      best = shape.note;
      bestDistance = distance;
    }
  };
  for (const auto &shape : shapes)
    consider(shape);
  for (size_t i = 0; i < candidateShapes.size(); ++i)
    if (candidateVisible[i])
      consider(candidateShapes[i]);
  return best;
}

int PolyphonicNoteLayer::assignmentFocus() const
{
  if (drag == Drag::assign && pressedNote >= 0)
    return pressedNote;
  if (hoverNote >= 0)
  {
    // A candidate under the pointer shows the note it belongs to, when it
    // sits on one of that note's harmonics (the engine's `parent` can be any
    // harmonic, even the 30th).
    const auto &note = builtFor->notes[static_cast<size_t>(hoverNote)];
    if (note.active)
      return hoverNote;
    if (note.parent >= 0 && note.parent < static_cast<int>(builtFor->notes.size()) &&
        builtFor->notes[static_cast<size_t>(note.parent)].active &&
        harmonicallyRelated(note.parent, hoverNote))
      return note.parent;
    return -1;
  }
  return selection.size() == 1 ? *selection.begin() : -1;
}

bool PolyphonicNoteLayer::setHoverPoint(std::optional<juce::Point<float>> worldPoint)
{
  if (!assignmentMode)
    return false;
  rebuildIfNeeded();
  const auto oldSplit = splitHover;
  const auto oldMerge = mergeHover;
  if (splitTool && worldPoint)
    updateSeparationHover(worldPoint->x, worldPoint->y);
  else
  {
    splitHover = {};
    mergeHover = {};
  }
  const bool separationChanged = oldSplit.note != splitHover.note ||
                                 oldSplit.frame != splitHover.frame ||
                                 oldMerge.second != mergeHover.second;
  const int hit = worldPoint ? assignmentHitAt(worldPoint->x, worldPoint->y) : -1;
  if (hit == hoverNote)
    return separationChanged;
  hoverNote = hit;
  return true;
}

void PolyphonicNoteLayer::setSplitTool(bool enabled)
{
  splitTool = enabled;
  splitHover = {};
  mergeHover = {};
}

float PolyphonicNoteLayer::separationX(int second) const
{
  return static_cast<float>(builtFor->frameToSeconds(
                                builtFor->notes[static_cast<size_t>(second)].startFrame) *
                            coordMapper->getPixelsPerSecond());
}

void PolyphonicNoteLayer::updateSeparationHover(float worldX, float worldY)
{
  splitHover = {};
  mergeHover = {};
  if (!builtFor || coordMapper == nullptr)
    return;
  // A separation first (as the vocal Split tool): within a few pixels of
  // where a note starts that merges with the one before it, between their rows.
  constexpr float kBoundaryPixels = 5.0f;
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  for (const auto &shape : shapes)
  {
    const int first = polyphonicassignment::mergePartner(*builtFor, shape.note);
    if (first < 0)
      continue;
    const float x = separationX(shape.note);
    const auto *firstShape = shapeFor(first);
    if (firstShape == nullptr || std::abs(worldX - x) > kBoundaryPixels)
      continue;
    const float top = std::min(rowCentre(firstShape->medianMidi), rowCentre(shape.medianMidi)) -
                      pixelsPerSemitone * 0.5f;
    const float bottom = std::max(rowCentre(firstShape->medianMidi), rowCentre(shape.medianMidi)) +
                         pixelsPerSemitone * 0.5f;
    if (worldY >= top && worldY <= bottom)
    {
      mergeHover = {first, shape.note};
      return;
    }
  }
  // Else a split of the active note under the pointer, before the frame there.
  const int hit = assignmentHitAt(worldX, worldY);
  if (hit < 0 || !builtFor->notes[static_cast<size_t>(hit)].active)
    return;
  const int frame = static_cast<int>(std::lround(worldX / coordMapper->getPixelsPerSecond() /
                                                 secondsPerFrame()));
  if (polyphonicassignment::canSplit(*builtFor, hit, frame))
    splitHover = {hit, frame};
}

void PolyphonicNoteLayer::drawSeparations(juce::Graphics &g, float visibleStart, float visibleEnd)
{
  const juce::Colour lineColour{0xFF9A9A9Au}; // as the vocal Split tool's
  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  // Every separation between consecutive notes, so it can be found and merged.
  for (const auto &shape : shapes)
  {
    const int first = polyphonicassignment::mergePartner(*builtFor, shape.note);
    const auto *firstShape = shapeFor(first);
    if (firstShape == nullptr)
      continue;
    const float x = separationX(shape.note);
    if (x < visibleStart || x > visibleEnd)
      continue;
    const float top = std::min(rowCentre(firstShape->medianMidi), rowCentre(shape.medianMidi)) -
                      pixelsPerSemitone * 0.5f;
    const float bottom = std::max(rowCentre(firstShape->medianMidi), rowCentre(shape.medianMidi)) +
                         pixelsPerSemitone * 0.5f;
    if (mergeHover.second == shape.note)
    {
      // The two notes a click merges, highlighted; the line between them is
      // gone, as it will be once they are one note.
      g.setColour(juce::Colours::white.withAlpha(0.18f));
      g.fillPath(notePath(*firstShape, {}));
      g.fillPath(notePath(shape, {}));
      continue;
    }
    g.setColour(lineColour);
    g.drawLine(x, top, x, bottom, 1.5f);
  }
  // Where a click splits the note under the pointer.
  if (const auto *shape = shapeFor(splitHover.note))
  {
    const float x = static_cast<float>(builtFor->frameToSeconds(splitHover.frame) *
                                       coordMapper->getPixelsPerSecond());
    const float half = std::max(halfHeightAt(*shape, splitHover.frame, waveHeight(), 1.0f),
                                pixelsPerSemitone * 0.5f);
    const float cy = rowCentre(shape->medianMidi);
    g.setColour(lineColour);
    g.drawLine(x, cy - half, x, cy + half, 1.5f);
  }
}

PolyphonicNoteLayer::AssignTarget PolyphonicNoteLayer::assignTargetFor(int note, float midi) const
{
  AssignTarget target;
  const auto *shape = shapeFor(note);
  if (shape == nullptr)
    return target;

  // A candidate sounding at the same time, nearest to the pointer's pitch.
  float bestDistance = kCandidateSnapSemitones;
  for (size_t i = 0; i < candidateShapes.size(); ++i)
  {
    if (!candidateVisible[i])
      continue;
    const auto &candidate = candidateShapes[i];
    const float distance = std::abs(candidate.medianMidi - midi);
    if (distance <= bestDistance && overlapsInTime(note, candidate.note))
    {
      bestDistance = distance;
      target.candidate = candidate.note;
      target.midi = candidate.medianMidi;
    }
  }
  if (target.valid())
    return target;

  // Else the note's own harmonic or subharmonic: the same partials, read as
  // another fundamental (an octave ghost moved down onto the real note's
  // pitch, a missed fundamental under a detected overtone).
  bestDistance = kHarmonicSnapSemitones;
  for (float ratio : kRatios)
  {
    const float at = shape->medianMidi + semitonesOf(ratio);
    if (at < static_cast<float>(MIN_MIDI_NOTE) || at > static_cast<float>(MAX_MIDI_NOTE))
      continue;
    const float distance = std::abs(at - midi);
    if (distance <= bestDistance)
    {
      bestDistance = distance;
      target.ratio = ratio;
      target.midi = at;
    }
  }
  return target;
}

void PolyphonicNoteLayer::drawAssignment(juce::Graphics &g, float visibleStart, float visibleEnd)
{
  g.setColour(kAssignmentTint);
  g.fillRect(g.getClipBounds());

  const int pitchReferenceHz = project != nullptr ? project->getPitchReferenceHz() : 440;
  const int focus = assignmentFocus();
  const auto visible = [&](const NoteShape &shape) {
    const auto bounds = noteBounds(shape.note, shape);
    return bounds.getRight() >= visibleStart && bounds.getX() <= visibleEnd;
  };

  // The focused note's harmonic series: where its partials lie, so the
  // notes and candidates sitting on them can be told apart.
  if (const auto *shape = shapeFor(focus))
  {
    const auto bounds = noteBounds(shape->note, *shape);
    const auto guide = [&](float midi, bool sub) {
      if (midi < static_cast<float>(MIN_MIDI_NOTE) || midi > static_cast<float>(MAX_MIDI_NOTE) + 1.0f)
        return;
      const float y = rowCentre(midi);
      juce::Path line;
      line.startNewSubPath(bounds.getX(), y);
      line.lineTo(bounds.getRight(), y);
      juce::Path dotted;
      const float dots[] = {1.0f, sub ? 6.0f : 3.0f};
      juce::PathStrokeType(1.0f).createDashedStroke(dotted, line, dots, 2);
      g.setColour(juce::Colours::white.withAlpha(kGuideAlpha));
      g.fillPath(dotted);
    };
    for (int h = 2; h <= kMaxGuideHarmonic; ++h)
      guide(shape->medianMidi + semitonesOf(static_cast<float>(h)), false);
    guide(shape->medianMidi - 12.0f, true);
    guide(shape->medianMidi - semitonesOf(3.0f), true);
  }

  // Candidates: dashed outlines (like deleted notes) where the analysis
  // considered a note.
  // While the ball is dragged, notes it would deactivate are drawn as
  // candidates (when the bracket shows them) and candidates it would activate
  // as notes.
  if (thresholdPreview)
    for (const auto &shape : shapes)
      if (!shownActive(shape.note) && visible(shape) &&
          builtFor->notes[static_cast<size_t>(shape.note)].probability >= candidateThreshold)
        strokeDashed(g, notePath(shape, {}), juce::Colours::white.withAlpha(kCandidateAlpha),
                     kCandidateWidth);
  for (size_t i = 0; i < candidateShapes.size(); ++i)
  {
    const auto &shape = candidateShapes[i];
    if (thresholdPreview && shownActive(shape.note) && visible(shape))
    {
      const auto body = notePath(shape, {});
      setNoteGradientFill(g, body.getBounds(), rowCentre(shape.medianMidi),
                          getNoteGradientColours(shape.medianMidi, pitchReferenceHz));
      g.fillPath(body);
      continue;
    }
    if (!candidateVisible[i] || !visible(shape))
      continue;
    const bool hovered = shape.note == hoverNote;
    // Its candidates: the ones on its harmonics or subharmonics while it sounds.
    const bool related = focus >= 0 && harmonicallyRelated(focus, shape.note);
    const bool targeted = drag == Drag::assign && assignTarget.candidate == shape.note;
    const auto body = notePath(shape, {});
    if (targeted)
    {
      g.setColour(kTargetColour.withAlpha(0.25f));
      g.fillPath(body);
    }
    strokeDashed(g, body,
                 targeted ? kTargetColour
                          : juce::Colours::white.withAlpha(hovered || related ? kRelatedCandidateAlpha
                                                                              : kCandidateAlpha),
                 hovered || targeted ? kHoveredCandidateWidth : kCandidateWidth);
  }

  // The active notes as analysed, in the tuning colours.
  for (const auto &shape : shapes)
  {
    if (!visible(shape) || !shownActive(shape.note))
      continue;
    const auto body = notePath(shape, {});
    const bool dragged = drag == Drag::assign && shape.note == pressedNote;
    const auto colours = getNoteGradientColours(shape.medianMidi, pitchReferenceHz);
    g.saveState();
    if (dragged)
      g.setOpacity(0.35f);
    setNoteGradientFill(g, body.getBounds(), rowCentre(shape.medianMidi), colours);
    g.fillPath(body);
    g.restoreState();
    if (selection.count(shape.note) > 0 || shape.note == hoverNote)
    {
      g.setColour(juce::Colours::white.withAlpha(selection.count(shape.note) > 0 ? 0.85f : 0.45f));
      g.strokePath(body, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    }
  }

  if (splitTool)
    drawSeparations(g, visibleStart, visibleEnd);

  // A note being dragged: its outline at the pointer's pitch, and at the
  // harmonic it would snap to.
  if (drag == Drag::assign)
    if (const auto *shape = shapeFor(pressedNote))
    {
      polyphonic::NoteEdit at;
      at.semitones = assignDragMidi - shape->medianMidi;
      g.setColour(juce::Colours::white.withAlpha(0.6f));
      g.strokePath(notePath(*shape, at), juce::PathStrokeType(1.0f));
      if (assignTarget.ratio > 0.0f)
      {
        polyphonic::NoteEdit snapped;
        snapped.semitones = assignTarget.midi - shape->medianMidi;
        const auto body = notePath(*shape, snapped);
        g.setColour(kTargetColour.withAlpha(0.25f));
        g.fillPath(body);
        strokeDashed(g, body, kTargetColour, kHoveredCandidateWidth);
      }
    }

  if (drag == Drag::band && !band.isEmpty())
  {
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRect(band);
    g.setColour(juce::Colours::white.withAlpha(0.25f));
    g.drawRect(band, 1.0f);
  }
}

void PolyphonicNoteLayer::assignmentMouseDown(float worldX, float worldY,
                                              const juce::ModifierKeys &mods)
{
  const bool additive = mods.isShiftDown() || mods.isCommandDown();
  dragStart = {worldX, worldY};
  dragMoved = false;
  pendingSingle = -1;
  assignTarget = {};

  if (splitTool)
  {
    // The Split tool: a click merges at a separation, else splits the note
    // under the pointer there; nothing is dragged.
    updateSeparationHover(worldX, worldY);
    const auto take = project->getPolyphonicTake();
    if (take == nullptr || !take->analysis)
      return;
    if (mergeHover.second >= 0)
      commitSeparation(polyphonicassignment::merge(*take, project->getPolyphonicEdits(),
                                                   mergeHover.first, mergeHover.second),
                       "Merge Notes");
    else if (splitHover.note >= 0)
      commitSeparation(polyphonicassignment::split(*take, project->getPolyphonicEdits(),
                                                   splitHover.note, splitHover.frame),
                       "Split Note");
    return;
  }

  const int hit = assignmentHitAt(worldX, worldY);
  if (hit >= 0 && builtFor->notes[static_cast<size_t>(hit)].active)
  {
    if (additive)
    {
      if (selection.erase(hit) == 0)
        selection.insert(hit);
      selectionChanged();
      if (selection.count(hit) == 0)
        return;
    }
    else if (selection.count(hit) == 0)
    {
      selection = {hit};
      selectionChanged();
    }
    // One note at a time is moved to another pitch: it is the detection of
    // that note that is being corrected.
    drag = Drag::assign;
    pressedNote = hit;
    assignDragMidi = shapeFor(hit)->medianMidi;
    if (onAssignmentAuditionChanged)
      onAssignmentAuditionChanged();
    return;
  }
  if (hit >= 0)
  {
    // A candidate is not selected; a double-click activates it. Held down,
    // it is heard (as a tone).
    if (!additive && !selection.empty())
    {
      selection.clear();
      selectionChanged();
    }
    drag = Drag::listen;
    pressedNote = hit;
    if (onAssignmentAuditionChanged)
      onAssignmentAuditionChanged();
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

void PolyphonicNoteLayer::assignmentMouseDrag(float /*worldX*/, float worldY)
{
  const auto *shape = shapeFor(pressedNote);
  if (shape == nullptr)
    return;
  const float dy = worldY - dragStart.y;
  if (!dragMoved && std::abs(dy) <= kDecidePixels)
    return;
  dragMoved = true;
  assignDragMidi = shape->medianMidi - dy / coordMapper->getPixelsPerSemitone();
  const auto previous = assignTarget;
  assignTarget = assignTargetFor(pressedNote, assignDragMidi);
  // A new pitch to snap to (or none any more) is heard at once.
  if ((previous.candidate != assignTarget.candidate || previous.ratio != assignTarget.ratio) &&
      onAssignmentAuditionChanged)
    onAssignmentAuditionChanged();
}

PolyphonicNoteLayer::AssignmentAudition PolyphonicNoteLayer::getAssignmentAudition() const
{
  AssignmentAudition out;
  if (!assignmentMode || !builtFor || (drag != Drag::assign && drag != Drag::listen) ||
      pressedNote < 0 || pressedNote >= static_cast<int>(builtFor->notes.size()))
    return out;
  const auto toneOf = [&](int note, float ratio) {
    const auto &n = builtFor->notes[static_cast<size_t>(note)];
    out.startFrame = n.startFrame;
    out.f0 = n.f0;
    for (auto &f : out.f0)
      f *= ratio;
    const auto *shape = n.active ? shapeFor(note)
                        : candidateIndex[static_cast<size_t>(note)] >= 0
                            ? &candidateShapes[static_cast<size_t>(candidateIndex[static_cast<size_t>(note)])]
                            : nullptr;
    if (shape != nullptr)
      out.level = shape->level;
  };
  out.note = pressedNote;
  if (drag == Drag::listen)
  {
    out.candidate = pressedNote;
    toneOf(pressedNote, 1.0f);
    return out;
  }
  if (assignTarget.candidate >= 0)
  {
    out.candidate = assignTarget.candidate;
    toneOf(assignTarget.candidate, 1.0f);
    return out;
  }
  if (assignTarget.ratio > 0.0f)
  {
    out.ratio = assignTarget.ratio;
    toneOf(pressedNote, assignTarget.ratio);
    return out;
  }
  // The note as it is: its own sound once its masks exist.
  const bool masks = pressedNote < static_cast<int>(builtFor->masks.size()) &&
                     !builtFor->masks[static_cast<size_t>(pressedNote)].partials.empty();
  out.separated = masks;
  if (!masks)
    toneOf(pressedNote, 1.0f);
  return out;
}

void PolyphonicNoteLayer::assignmentMouseUp()
{
  const int note = pressedNote;
  const auto target = assignTarget;
  const bool moved = dragMoved;
  drag = Drag::none;
  pressedNote = -1;
  assignTarget = {};
  if (!moved || !target.valid() || !builtFor)
    return; // dropped where nothing plausible is: it springs back

  auto notes = builtFor->notes;
  auto &n = notes[static_cast<size_t>(note)];
  if (target.candidate >= 0)
  {
    // The candidate becomes the note, and the note a candidate of it.
    auto &c = notes[static_cast<size_t>(target.candidate)];
    c.active = true;
    c.parent = -1;
    n.active = false;
    n.parent = target.candidate;
    selection = {target.candidate};
    selectionChanged();
    commitAssignment(std::move(notes), "Assign Note to Candidate");
    return;
  }
  for (auto &f : n.f0)
    f *= target.ratio;
  commitAssignment(std::move(notes), target.ratio > 1.0f ? "Move Note to Harmonic"
                                                         : "Move Note to Subharmonic");
}

bool PolyphonicNoteLayer::assignmentDoubleClick(float worldX, float worldY)
{
  rebuildIfNeeded();
  if (splitTool)
    return true; // each click of it split or merged already
  const int hit = assignmentHitAt(worldX, worldY);
  if (hit < 0 || !builtFor)
    return false;
  auto notes = builtFor->notes;
  auto &n = notes[static_cast<size_t>(hit)];
  n.active = !n.active;
  if (n.active)
  {
    n.parent = -1;
    selection = {hit};
  }
  else
  {
    selection.erase(hit);
  }
  selectionChanged();
  commitAssignment(std::move(notes), n.active ? "Activate Note" : "Deactivate Note");
  return true;
}

void PolyphonicNoteLayer::deactivateSelection()
{
  if (!assignmentMode || !builtFor || selection.empty() || drag != Drag::none)
    return;
  auto notes = builtFor->notes;
  bool changed = false;
  for (int note : selection)
  {
    auto &n = notes[static_cast<size_t>(note)];
    changed = changed || n.active;
    n.active = false;
  }
  if (!changed)
    return;
  const bool several = selection.size() > 1;
  selection.clear();
  selectionChanged();
  commitAssignment(std::move(notes), several ? "Deactivate Notes" : "Deactivate Note");
}

void PolyphonicNoteLayer::commitAssignment(std::vector<polyphonic::Note> notes,
                                           const juce::String &name,
                                           std::optional<float> threshold)
{
  if (project == nullptr || !builtFor || !onAssignmentCommitted)
    return;
  const auto &notesBefore = builtFor->notes;
  const auto editsBefore = project->getPolyphonicEdits();
  const auto editsAfter = polyphonicassignment::editsFor(notesBefore, notes, editsBefore);
  const float thresholdAfter =
      threshold.value_or(polyphonicassignment::thresholdOf(builtFor->params));
  hoverNote = -1;
  // Nothing changed (the ball let go where it was): no step.
  const bool sameNotes = std::equal(notes.begin(), notes.end(), notesBefore.begin(), notesBefore.end(),
                                    [](const polyphonic::Note &x, const polyphonic::Note &y) {
                                      return !polyphonicassignment::noteChanged(x, y) &&
                                             x.parent == y.parent;
                                    });
  if (sameNotes && thresholdAfter == polyphonicassignment::thresholdOf(builtFor->params))
    return;
  onAssignmentCommitted({std::move(notes), editsAfter, thresholdAfter, std::nullopt, name});
}

void PolyphonicNoteLayer::commitSeparation(polyphonicassignment::Separation separation,
                                           const juce::String &name)
{
  if (project == nullptr || !builtFor || !onAssignmentCommitted)
    return;
  hoverNote = -1;
  splitHover = {};
  mergeHover = {};
  selectAfterRebuild = separation.select;
  onAssignmentCommitted({std::move(separation.notes), std::move(separation.edits),
                         polyphonicassignment::thresholdOf(builtFor->params),
                         std::move(separation.levels), name});
}
