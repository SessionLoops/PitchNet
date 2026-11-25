#include "ChordLane.h"

#include "../../Utils/UI/Theme.h"
#include "../Components/AppFont.h"

#include <cmath>
#include <limits>

namespace
{
constexpr float kNameFontHeight = 12.0f;
constexpr float kBoundaryPixels = 4.0f; // either side of where two chords meet
const juce::Colour kNameColour(0xFFE2E2E2u);
const juce::Colour kChordFill(0xFF343434u);
} // namespace

void ChordLane::setProject(Project *proj)
{
  project = proj;
  detectedValid = false;
  segmentedValid = false;
  detected.clear();
  segmented.clear();
  take.reset();
  segmentedTake.reset();
  edits.clear();
  segmentedEdits.clear();
  hoverSplit.reset();
  hoverMerge.reset();
}

void ChordLane::setHostChords(std::vector<polyphonic::ChordSpan> timelineChords)
{
  if (timelineChords == hostChords)
    return;
  hostChords = std::move(timelineChords);
  hostValid = false;
}

bool ChordLane::isSegmentedByUser() const
{
  return hostChords.empty() && project != nullptr && project->isPolyphonic() &&
         project->getChordBoundaries().size() >= 2;
}

std::vector<polyphonic::ChordNote> ChordLane::notesOf(const PolyphonicTake &t) const
{
  const auto levels = noteLevels(t);
  return polyphonic::chordNotes(*t.analysis, &levels, &project->getPolyphonicEdits());
}

const std::vector<polyphonic::ChordSpan> &ChordLane::getChords(const Grid &grid)
{
  if (!hostChords.empty())
  {
    // The host's chords sit on its timeline; the canvas is in project time.
    const double offset = coordMapper != nullptr ? coordMapper->getTimelineDisplayOffset() : 0.0;
    if (!hostValid || offset != hostOffset)
    {
      hostProjectChords = hostChords;
      for (auto &span : hostProjectChords)
      {
        span.startSeconds -= offset; // stays infinite where it is
        span.endSeconds -= offset;
      }
      hostValid = true;
      hostOffset = offset;
    }
    return hostProjectChords;
  }

  if (!isSegmentedByUser())
    return getDetectedChords(grid);

  const auto currentTake = project->getPolyphonicTake();
  const auto &currentEdits = project->getPolyphonicEdits();
  const auto &boundaries = project->getChordBoundaries();
  if (segmentedValid && currentTake == segmentedTake && currentEdits == segmentedEdits &&
      boundaries == segmentedBoundaries)
    return segmented;

  segmented = polyphonic::chordsOver(notesOf(*currentTake), boundaries);
  segmentedValid = true;
  segmentedTake = currentTake;
  segmentedEdits = currentEdits;
  segmentedBoundaries = boundaries;
  getDetectedChords(grid); // keeps `flats` current
  return segmented;
}

const std::vector<polyphonic::ChordSpan> &ChordLane::getDetectedChords(const Grid &grid)
{
  const auto currentTake =
      project != nullptr && project->isPolyphonic() ? project->getPolyphonicTake() : nullptr;
  if (currentTake == nullptr)
  {
    detected.clear();
    detectedValid = false;
    take.reset();
    return detected;
  }

  const auto &currentEdits = project->getPolyphonicEdits();
  if (detectedValid && currentTake == take && grid == detectedGrid && currentEdits == edits)
    return detected;

  const auto notes = notesOf(*currentTake);
  polyphonic::ChordOptions options;
  options.beatSeconds = grid.beatSeconds;
  options.gridOriginSeconds = grid.originProjectSeconds;
  detected = polyphonic::detectChords(notes, options);
  flats = polyphonic::keyPrefersFlats(notes);

  detectedValid = true;
  take = currentTake;
  edits = currentEdits;
  detectedGrid = grid;
  return detected;
}

const polyphonic::ChordSpan *ChordLane::chordAt(double projectSeconds, const Grid &grid)
{
  for (const auto &span : getChords(grid))
    if (projectSeconds >= span.startSeconds && projectSeconds < span.endSeconds)
      return &span;
  return nullptr;
}

std::vector<double> ChordLane::getShownBoundaries(const Grid &grid)
{
  std::vector<double> out;
  for (const auto &span : getChords(grid))
  {
    if (!std::isfinite(span.startSeconds) || !std::isfinite(span.endSeconds))
      continue;
    if (out.empty() || span.startSeconds > out.back())
      out.push_back(span.startSeconds); // a gap before it becomes a span of its own
    out.push_back(span.endSeconds);
  }
  return out;
}

double ChordLane::toScreenX(double seconds, double laneLeft, double laneRight) const
{
  if (!std::isfinite(seconds))
    return seconds < 0.0 ? laneLeft - 10.0 : laneRight + 10.0;
  const double scrollX = static_cast<double>(static_cast<int>(coordMapper->getScrollX()));
  const double x = CoordinateMapper::pianoKeysWidth +
                   static_cast<double>(coordMapper->timeToX(seconds)) - scrollX;
  return juce::jlimit(laneLeft - 10.0, laneRight + 10.0, x);
}

std::vector<ChordLane::Layout> ChordLane::layout(int componentWidth, int scrollBarSize,
                                                 const Grid &grid)
{
  std::vector<Layout> out;
  if (coordMapper == nullptr)
    return out;
  const double laneLeft = CoordinateMapper::pianoKeysWidth;
  const double laneRight = componentWidth - scrollBarSize;
  const float top = static_cast<float>(CoordinateMapper::rulerHeight);
  const float height = static_cast<float>(CoordinateMapper::chordLaneHeight);
  const auto font = AppFont::getFont(kNameFontHeight);

  const auto &spans = getChords(grid);
  for (size_t i = 0; i < spans.size(); ++i)
  {
    const auto &span = spans[i];
    const double x0 = toScreenX(span.startSeconds, laneLeft, laneRight);
    const double x1 = toScreenX(span.endSeconds, laneLeft, laneRight);
    if (x1 <= laneLeft || x0 >= laneRight || x1 - x0 < 1.0)
      continue;
    Layout l;
    l.span = static_cast<int>(i);
    l.x0 = static_cast<float>(x0);
    l.x1 = static_cast<float>(x1);
    l.box = {l.x0 + 1.0f, top + 2.0f, l.x1 - l.x0 - 2.0f, height - 5.0f};
    // The name stays in view while its chord is scrolled past the left edge.
    const float textLeft = std::max(l.box.getX(), static_cast<float>(laneLeft)) + 4.0f;
    const float room = l.box.getRight() - textLeft - 3.0f;
    const float width = juce::GlyphArrangement::getStringWidth(
        font, juce::String::fromUTF8(span.name.c_str()));
    l.text = {textLeft, l.box.getY(), std::max(0.0f, std::min(room, width)), l.box.getHeight()};
    out.push_back(l);
  }
  return out;
}

ChordLane::Hit ChordLane::hitTest(int screenX, int componentWidth, int scrollBarSize,
                                  const Grid &grid)
{
  Hit hit;
  const auto boxes = layout(componentWidth, scrollBarSize, grid);
  const auto &spans = getChords(grid);
  const float x = static_cast<float>(screenX);

  // Where two chords meet comes first: the edges of both chords are there.
  for (size_t i = 0; i + 1 < boxes.size(); ++i)
  {
    const auto &a = spans[static_cast<size_t>(boxes[i].span)];
    const auto &b = spans[static_cast<size_t>(boxes[i + 1].span)];
    if (boxes[i + 1].span != boxes[i].span + 1 || a.endSeconds != b.startSeconds)
      continue;
    if (std::abs(x - boxes[i].x1) <= kBoundaryPixels)
    {
      hit.kind = Hit::Kind::Boundary;
      hit.span = boxes[i].span;
      hit.boundarySeconds = a.endSeconds;
      return hit;
    }
  }
  for (const auto &l : boxes)
  {
    if (x < l.x0 || x >= l.x1)
      continue;
    hit.span = l.span;
    const bool overName = l.text.getWidth() > 0.0f && x >= l.text.getX() - 3.0f &&
                          x <= l.text.getRight() + 3.0f;
    hit.kind = overName ? Hit::Kind::Name : Hit::Kind::Chord;
    return hit;
  }
  return hit;
}

std::optional<juce::Rectangle<float>> ChordLane::getChordBox(double projectSeconds,
                                                            int componentWidth,
                                                            int scrollBarSize, const Grid &grid)
{
  const auto &spans = getChords(grid);
  for (const auto &l : layout(componentWidth, scrollBarSize, grid))
  {
    const auto &span = spans[static_cast<size_t>(l.span)];
    if (projectSeconds >= span.startSeconds && projectSeconds < span.endSeconds)
      return l.box;
  }
  return std::nullopt;
}

juce::Colour ChordLane::getChordFill() const
{
  return showsHostChords() ? APP_COLOR_PRIMARY.withAlpha(0.28f) : kChordFill;
}

juce::Colour ChordLane::getNameColour() const { return kNameColour; }
float ChordLane::getNameFontHeight() { return kNameFontHeight; }

bool ChordLane::setHover(std::optional<double> splitSeconds, std::optional<double> mergeBoundary)
{
  if (splitSeconds == hoverSplit && mergeBoundary == hoverMerge)
    return false;
  hoverSplit = splitSeconds;
  hoverMerge = mergeBoundary;
  return true;
}

void ChordLane::draw(juce::Graphics &g, int componentWidth, int scrollBarSize, const Grid &grid)
{
  if (coordMapper == nullptr)
    return;

  const int keysWidth = CoordinateMapper::pianoKeysWidth;
  const int top = CoordinateMapper::rulerHeight;
  const int height = CoordinateMapper::chordLaneHeight;
  const auto lane = juce::Rectangle<int>(keysWidth, top,
                                         componentWidth - keysWidth - scrollBarSize, height);

  juce::Graphics::ScopedSaveState savedState(g);
  g.reduceClipRegion(juce::Rectangle<int>(0, top, componentWidth - scrollBarSize, height));

  g.setColour(juce::Colour(0xFF1D1D1Du));
  g.fillRect(lane);

  // The keys column: three stacked note heads, a chord.
  g.setColour(juce::Colour(0xFF232323u));
  g.fillRect(0, top, keysWidth, height);
  g.setColour(juce::Colour(0xFF6A6A6Au));
  for (int i = 0; i < 3; ++i)
    g.fillEllipse(static_cast<float>(keysWidth) * 0.5f - 3.5f,
                  static_cast<float>(top + height - 6 - 4 * i), 7.0f, 4.0f);

  g.setColour(juce::Colour(0xFF0D0B0Bu));
  g.drawHorizontalLine(top + height - 1, 0.0f, static_cast<float>(componentWidth - scrollBarSize));

  const auto boxes = layout(componentWidth, scrollBarSize, grid);
  const auto &spans = getChords(grid);
  const bool host = showsHostChords();
  const auto fill = host ? APP_COLOR_PRIMARY.withAlpha(0.28f) : kChordFill;
  const auto edge = host ? APP_COLOR_PRIMARY.withAlpha(0.6f) : juce::Colour(0xFF4A4A4Au);
  g.setFont(AppFont::getFont(kNameFontHeight));

  // Outlines first, then the merge preview over them, then the names on top.
  for (const auto &l : boxes)
  {
    const auto &span = spans[static_cast<size_t>(l.span)];
    if (span.chord.isNone())
    {
      // A span of the user's segmentation where the notes play no chord.
      g.setColour(edge.withMultipliedAlpha(0.5f));
      g.drawRoundedRectangle(l.box, 3.0f, 1.0f);
      continue;
    }
    g.setColour(fill);
    g.fillRoundedRectangle(l.box, 3.0f);
    g.setColour(edge);
    g.drawRoundedRectangle(l.box, 3.0f, 1.0f);
  }

  // A click on this boundary merges the two chords: the area they would cover.
  if (hoverMerge)
  {
    for (size_t i = 0; i + 1 < boxes.size(); ++i)
    {
      const auto &a = spans[static_cast<size_t>(boxes[i].span)];
      const auto &b = spans[static_cast<size_t>(boxes[i + 1].span)];
      if (a.endSeconds != *hoverMerge || b.startSeconds != *hoverMerge)
        continue;
      const auto merged = boxes[i].box.getUnion(boxes[i + 1].box);
      g.setColour(kNameColour.withAlpha(0.15f));
      g.fillRoundedRectangle(merged, 3.0f);
      g.setColour(kNameColour.withAlpha(0.8f));
      g.drawRoundedRectangle(merged, 3.0f, 1.0f);
      break;
    }
  }

  for (const auto &l : boxes)
  {
    const auto &span = spans[static_cast<size_t>(l.span)];
    if (span.chord.isNone() || l.text.getWidth() < 6.0f)
      continue;
    g.setColour(kNameColour);
    g.drawText(juce::String::fromUTF8(span.name.c_str()),
               l.text.withWidth(l.box.getRight() - l.text.getX() - 3.0f),
               juce::Justification::centredLeft, true);
  }

  // Where a click splits.
  const double laneLeft = lane.getX(), laneRight = lane.getRight();
  if (hoverSplit)
  {
    const float x = static_cast<float>(toScreenX(*hoverSplit, laneLeft, laneRight));
    g.setColour(APP_COLOR_TEXT_PRIMARY.withAlpha(0.8f));
    const float dashes[] = {2.0f, 2.0f};
    g.drawDashedLine(juce::Line<float>(x, static_cast<float>(top) + 2.0f, x,
                                       static_cast<float>(top + height) - 3.0f),
                     dashes, 2, 1.0f);
  }
}
