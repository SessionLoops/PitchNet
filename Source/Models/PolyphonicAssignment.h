#pragma once

#include "Project.h"
#include "PolyphonicTake.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

/**
 * Note assignment for a polyphonic take (Melodyne's Note Assignment Mode):
 * which of the analysis's notes are active (played notes) and which are only
 * candidates, and at what pitch an active note sits. Unlike an edit, a change
 * of assignment changes the analysis itself, so each one builds a new take.
 *
 * The notes keep their order (edits stay keyed by index), so the new take is
 * the current one with other `active` flags (or a note moved by a harmonic
 * ratio). Its masks are rebuilt from the take's channels by
 * polyphonic::restore, exactly as for an opened project: until then the take
 * is a restoring take that is drawn and edited from its notes and levels and
 * renders nothing (see makeRestoringTake).
 */
namespace polyphonicassignment
{

/** True when `project`'s take can be reassigned now: it has its notes and
 *  its own channels to rebuild the masks from (an opened project's take gets
 *  them once its first restore lands). */
inline bool canReassign(const Project &project)
{
  const auto take = project.getPolyphonicTake();
  return take != nullptr && take->hasAnalysis() && take->source.getNumSamples() > 0 &&
         take->sourceRate > 0.0;
}

/** The probability a possible note needs to be a note, for the detector the
 *  analysis used (the note assignment slider's ball). It travels with the
 *  take's params, so undo and saving keep it. */
inline float thresholdOf(const polyphonic::Params &params)
{
  return params.activationThresholdFor(params.detectionMode);
}
inline void setThreshold(polyphonic::Params &params, float threshold)
{
  (params.detectionMode == polyphonic::DetectionMode::NeuralGuided ? params.guidedActivationThreshold
                                                                   : params.activationThreshold) =
      threshold;
}
inline float thresholdOf(const Project &project)
{
  const auto take = project.getPolyphonicTake();
  return take != nullptr && take->analysis != nullptr ? thresholdOf(take->analysis->params)
                                                      : polyphonic::Params{}.activationThreshold;
}

/** `notes` with every note at or above `threshold` active and every other one
 *  a candidate (the ball overrides any assignment made by hand). At the
 *  analysis's own threshold this is the detection as it came: merged notes
 *  carry their pieces' highest probability. A note that becomes active leaves
 *  its parent. */
inline std::vector<polyphonic::Note> notesAtThreshold(std::vector<polyphonic::Note> notes,
                                                      float threshold)
{
  for (auto &n : notes)
  {
    const bool active = !n.f0.empty() && n.probability >= threshold;
    if (active && !n.active)
      n.parent = -1;
    n.active = active;
  }
  return notes;
}

/** The project's current assignment (its take's notes). */
inline std::vector<polyphonic::Note> notesOf(const Project &project)
{
  const auto take = project.getPolyphonicTake();
  return take != nullptr && take->analysis != nullptr ? take->analysis->notes
                                                      : std::vector<polyphonic::Note>{};
}

/** Two assignments of the same note differ (it became active or inactive, or
 *  its pitch changed): its masks, level and edit no longer apply. */
inline bool noteChanged(const polyphonic::Note &a, const polyphonic::Note &b)
{
  return a.active != b.active || a.f0 != b.f0 || a.startFrame != b.startFrame ||
         a.endFrame != b.endFrame;
}

/**
 * Puts `notes` (the take's notes, reassigned) and `edits` on the project: a
 * restoring take with those notes and a pending restore that rebuilds its
 * masks. Every note keeps its level until the masks land: a deactivated note
 * its masks', an activated candidate its estimated one, and a note moved to
 * another octave or harmonic the one it had (the same frames; only its f0 is
 * scaled), so it keeps its shape. Only a note whose frames changed has none. Returns the pending restore (to
 * hand to whatever rebuilds it), or null if the take cannot be reassigned.
 */
inline std::shared_ptr<const PolyphonicRestore>
apply(Project &project, std::vector<polyphonic::Note> notes, Project::PolyphonicEdits edits,
      std::optional<float> threshold = std::nullopt,
      std::optional<std::vector<std::vector<float>>> givenLevels = std::nullopt)
{
  if (!canReassign(project))
    return nullptr;
  const auto take = project.getPolyphonicTake();
  const auto &old = take->analysis->notes;
  std::vector<std::vector<float>> levels;
  if (givenLevels)
  {
    levels = std::move(*givenLevels);
  }
  else
  {
    levels = noteLevels(*take);
    for (size_t i = 0; i < notes.size() && i < levels.size(); ++i)
      if (i >= old.size() || notes[i].startFrame != old[i].startFrame ||
          notes[i].endFrame != old[i].endFrame)
        levels[i].clear();
  }
  levels.resize(notes.size());

  auto pending = std::make_shared<PolyphonicRestore>();
  pending->params = take->analysis->params;
  if (threshold)
    setThreshold(pending->params, *threshold);
  pending->notes = std::move(notes);
  pending->levels = std::move(levels);
  auto restoring = makeRestoringTake(*pending, take->analysis->numSamples);
  restoring->source.makeCopyOf(take->source);
  restoring->sourceRate = take->sourceRate;

  project.setPolyphonicTake(std::move(restoring));
  project.setPendingPolyphonicRestore(pending);
  project.setPolyphonicEdits(std::move(edits));
  return pending;
}

/** `edits` without the edits of notes whose assignment changed from `before`
 *  to `after` (a deactivated note, or one moved to another pitch, loses its
 *  edit, as a re-detected note does in Melodyne), and without the copies
 *  (pasted notes) of those notes. */
inline Project::PolyphonicEdits editsFor(const std::vector<polyphonic::Note> &before,
                                         const std::vector<polyphonic::Note> &after,
                                         Project::PolyphonicEdits edits)
{
  for (auto it = edits.begin(); it != edits.end();)
  {
    const int source = polyphonic::sourceNote(it->first, it->second);
    const auto i = static_cast<size_t>(source);
    const bool keep = source >= 0 && i < after.size() && after[i].active &&
                      (i >= before.size() || !noteChanged(before[i], after[i]));
    it = keep ? std::next(it) : edits.erase(it);
  }
  return edits;
}

// ---- note separation (split and merge, as Melodyne's Note Separation Tool) ----

/** A new assignment with its edits and the levels to draw it with until its
 *  masks are rebuilt (for apply). */
struct Separation
{
  std::vector<polyphonic::Note> notes;
  Project::PolyphonicEdits edits;
  std::vector<std::vector<float>> levels;
  std::vector<int> select; // the notes it leaves selected
};

/** Frames a part of a split must keep at least: the engine's shortest note. */
inline int minimumNoteFrames(const polyphonic::Params &params)
{
  return std::max(1, static_cast<int>(std::ceil(params.minNoteSeconds * params.sampleRate / params.hop)));
}

/** `note` can be split before `frame` (frame starts the second part). */
inline bool canSplit(const polyphonic::Analysis &analysis, int note, int frame)
{
  if (note < 0 || note >= static_cast<int>(analysis.notes.size()))
    return false;
  const auto &n = analysis.notes[static_cast<size_t>(note)];
  const int least = minimumNoteFrames(analysis.params);
  return n.active && !n.f0.empty() && frame - n.startFrame >= least && n.endFrame + 1 - frame >= least;
}

/**
 * `note` split before `frame`: it keeps the part before, and the part from
 * `frame` on becomes a new note at the end of the list (so every other note,
 * and its edit, keeps its index). Both parts lose the note's edit and keep
 * its probability; each part's level is its share of the note's.
 */
inline Separation split(const PolyphonicTake &take, const Project::PolyphonicEdits &edits,
                        int note, int frame)
{
  const auto &analysis = *take.analysis;
  Separation out;
  out.notes = analysis.notes;
  out.levels = noteLevels(take);
  out.levels.resize(out.notes.size());
  auto first = out.notes[static_cast<size_t>(note)];
  const int cut = frame - first.startFrame;
  auto second = first;
  int nextId = 0;
  for (const auto &n : out.notes)
    nextId = std::max(nextId, n.id + 1);
  second.id = nextId;
  second.startFrame = frame;
  second.f0.assign(first.f0.begin() + cut, first.f0.end());
  first.endFrame = frame - 1;
  first.f0.resize(static_cast<size_t>(cut));
  const double share = static_cast<double>(cut) / std::max(1, cut + static_cast<int>(second.f0.size()));
  second.energy = first.energy * (1.0 - share);
  first.energy *= share;

  auto level = out.levels[static_cast<size_t>(note)];
  std::vector<float> firstLevel, secondLevel;
  if (!level.empty())
  {
    level.resize(first.f0.size() + second.f0.size(), level.back());
    firstLevel.assign(level.begin(), level.begin() + cut);
    secondLevel.assign(level.begin() + cut, level.end());
  }
  out.notes[static_cast<size_t>(note)] = std::move(first);
  out.levels[static_cast<size_t>(note)] = std::move(firstLevel);
  out.notes.push_back(std::move(second));
  out.levels.push_back(std::move(secondLevel));
  out.edits = edits;
  out.edits.erase(note);
  // its copies (pasted notes) go with its edit
  polyphonic::remapCopies(out.edits, [note](int source) { return source == note ? -1 : source; });
  out.select = {note, static_cast<int>(out.notes.size()) - 1};
  return out;
}

/** The note `second` can be merged into, or -1. Only consecutive notes
 *  merge, as the parts of a split are: an active note that ends on the frame
 *  before `second` starts, whose pitch runs on into it (within half a
 *  semitone at the joint, so a split glide merges back but a note of another
 *  voice that happens to end there does not). */
inline int mergePartner(const polyphonic::Analysis &analysis, int second)
{
  if (second < 0 || second >= static_cast<int>(analysis.notes.size()))
    return -1;
  const auto &b = analysis.notes[static_cast<size_t>(second)];
  if (!b.active || b.f0.empty() || b.f0.front() <= 0.0f)
    return -1;
  int best = -1;
  float bestJump = 0.5f;
  for (size_t i = 0; i < analysis.notes.size(); ++i)
  {
    const auto &a = analysis.notes[i];
    if (static_cast<int>(i) == second || !a.active || a.f0.empty() || a.f0.back() <= 0.0f ||
        a.endFrame + 1 != b.startFrame)
      continue;
    const float jump = std::abs(12.0f * std::log2(b.f0.front() / a.f0.back()));
    if (jump < bestJump)
    {
      best = static_cast<int>(i);
      bestJump = jump;
    }
  }
  return best;
}

/**
 * `second` merged into `first` (its merge partner: consecutive notes): one
 * note over both, its pitch curve and level theirs end to end, the higher
 * probability, the energy of both. `second` leaves the list, so
 * the notes after it, their edits and the candidates' parents move down one;
 * the merged note loses both edits.
 */
inline Separation merge(const PolyphonicTake &take, const Project::PolyphonicEdits &edits,
                        int first, int second)
{
  const auto &analysis = *take.analysis;
  Separation out;
  out.notes = analysis.notes;
  out.levels = noteLevels(take);
  out.levels.resize(out.notes.size());
  auto &a = out.notes[static_cast<size_t>(first)];
  const auto &b = analysis.notes[static_cast<size_t>(second)];
  auto level = out.levels[static_cast<size_t>(first)];
  const auto &levelB = out.levels[static_cast<size_t>(second)];
  if (!level.empty() || !levelB.empty())
  {
    level.resize(a.f0.size(), level.empty() ? 0.0f : level.back());
    std::vector<float> tail = levelB;
    tail.resize(b.f0.size(), tail.empty() ? 0.0f : tail.back());
    level.insert(level.end(), tail.begin(), tail.end());
  }
  a.f0.insert(a.f0.end(), b.f0.begin(), b.f0.end());
  const double weightA = a.energy, weightB = b.energy;
  if (weightA + weightB > 0.0)
    a.inharmonicity = static_cast<float>((a.inharmonicity * weightA + b.inharmonicity * weightB) /
                                         (weightA + weightB));
  a.energy = weightA + weightB;
  a.probability = std::max(a.probability, b.probability);
  a.endFrame = b.endFrame;
  out.levels[static_cast<size_t>(first)] = std::move(level);

  out.notes.erase(out.notes.begin() + second);
  out.levels.erase(out.levels.begin() + second);
  for (auto &n : out.notes)
    if (n.parent == second)
      n.parent = first < second ? first : first - 1;
    else if (n.parent > second)
      --n.parent;
  for (const auto &[index, edit] : edits)
    if (index != first && index != second)
      out.edits[index > second ? index - 1 : index] = edit;
  // copies (pasted notes, keyed below zero) follow their notes; those of the
  // merged notes go with their edits
  polyphonic::remapCopies(out.edits, [first, second](int source) {
    return source == first || source == second ? -1 : (source > second ? source - 1 : source);
  });
  out.select = {first < second ? first : first - 1};
  return out;
}

} // namespace polyphonicassignment
