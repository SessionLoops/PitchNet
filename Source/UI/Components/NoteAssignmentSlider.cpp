#include "NoteAssignmentSlider.h"

#include <cmath>

namespace
{
// The logistic scale: logit(probability) runs from kLogitLeft at the left end
// to kLogitRight at kAllFrom; from there on everything is shown (0).
constexpr float kLogitLeft = 5.3f;   // 0.995
constexpr float kLogitRight = -5.8f; // 0.003
constexpr float kAllFrom = 0.995f;
constexpr float kDefaultBracket = 0.05f;

// The triangles: their tips a little off the track, their bases outward.
constexpr float kHalfWidth = 5.0f;
constexpr float kTipGap = 2.0f;
constexpr float kHeight = 7.5f;
constexpr float kSidePad = kHalfWidth + 2.0f;
constexpr float kTrackThickness = 2.0f;

const juce::Colour kNotesTrack{0xffb4b4b4};      // left of the ball: notes
const juce::Colour kCandidatesTrack{0xff8c8c8c}; // between: candidates shown
const juce::Colour kHiddenTrack{0xff4c4c4c};     // right of the bracket: hidden
const juce::Colour kHandle{0xffc8c8c8};
const juce::Colour kHandleActive{0xffffffff};    // hovered or dragged

juce::Path triangle(float x, float cy, bool below)
{
  const float s = below ? 1.0f : -1.0f;
  juce::Path p;
  p.addTriangle(x, cy + s * kTipGap, x - kHalfWidth, cy + s * (kTipGap + kHeight),
                x + kHalfWidth, cy + s * (kTipGap + kHeight));
  return p;
}
} // namespace

NoteAssignmentSlider::NoteAssignmentSlider()
{
  ballPos = positionOf(ballProbability);
  bracketPos = positionOf(kDefaultBracket);
}

float NoteAssignmentSlider::probabilityAt(float position)
{
  if (position >= kAllFrom)
    return 0.0f;
  const float logit = kLogitLeft + (kLogitRight - kLogitLeft) * std::max(0.0f, position) / kAllFrom;
  return 1.0f / (1.0f + std::exp(-logit));
}

float NoteAssignmentSlider::positionOf(float probability)
{
  if (probability <= 0.0f)
    return 1.0f;
  const float p = juce::jlimit(1e-6f, 1.0f - 1e-6f, probability);
  const float logit = std::log(p / (1.0f - p));
  return juce::jlimit(0.0f, kAllFrom, kAllFrom * (logit - kLogitLeft) / (kLogitRight - kLogitLeft));
}

void NoteAssignmentSlider::setBall(float probability)
{
  ballProbability = probability;
  ballPos = positionOf(probability);
  bracketPos = std::max(bracketPos, ballPos);
  repaint();
}

void NoteAssignmentSlider::setBracket(float probability)
{
  bracketPos = std::max(ballPos, positionOf(probability));
  repaint();
}

juce::Rectangle<float> NoteAssignmentSlider::track() const
{
  return getLocalBounds().toFloat().reduced(kSidePad, 0.0f);
}

float NoteAssignmentSlider::xOf(float position) const
{
  const auto t = track();
  return t.getX() + position * t.getWidth();
}

float NoteAssignmentSlider::positionAtX(float x) const
{
  const auto t = track();
  return juce::jlimit(0.0f, 1.0f, (x - t.getX()) / std::max(1.0f, t.getWidth()));
}

juce::String NoteAssignmentSlider::getTooltip()
{
  // The half under the pointer: the handle a press there would move.
  const auto pointer = getMouseXYRelative();
  return handleAt(static_cast<float>(pointer.y)) == Handle::ball ? lowerTooltip : upperTooltip;
}

NoteAssignmentSlider::Handle NoteAssignmentSlider::handleAt(float y) const
{
  return y >= getHeight() * 0.5f ? Handle::ball : Handle::bracket;
}

void NoteAssignmentSlider::paint(juce::Graphics &g)
{
  const float cy = getHeight() * 0.5f;
  const float alpha = isEnabled() ? 1.0f : 0.4f;
  const float x0 = xOf(0.0f), xBall = xOf(ballPos), xBracket = xOf(bracketPos), x1 = xOf(1.0f);
  const float top = cy - kTrackThickness * 0.5f;

  // The track in three parts: notes (solid), candidates shown (dashed, like
  // them), hidden (dim).
  g.setColour(kHiddenTrack.withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(x0, top, x1 - x0, kTrackThickness, 1.0f);
  g.setColour(kNotesTrack.withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(x0, top, std::max(0.0f, xBall - x0), kTrackThickness, 1.0f);
  if (xBracket > xBall)
  {
    juce::Path line, dashed;
    line.startNewSubPath(xBall, cy);
    line.lineTo(xBracket, cy);
    const float dashes[] = {3.0f, 2.0f};
    juce::PathStrokeType(kTrackThickness).createDashedStroke(dashed, line, dashes, 2);
    g.setColour(kCandidatesTrack.withMultipliedAlpha(alpha));
    g.fillPath(dashed);
  }

  const auto colourFor = [&](Handle h) {
    return (dragging == h || (dragging == Handle::none && hovered == h) ? kHandleActive : kHandle)
        .withMultipliedAlpha(alpha);
  };
  // The bracket: outlined, above the track, pointing down.
  g.setColour(colourFor(Handle::bracket));
  g.strokePath(triangle(xBracket, cy, false),
               juce::PathStrokeType(1.3f, juce::PathStrokeType::mitered));
  // The ball: filled, below the track, pointing up.
  g.setColour(colourFor(Handle::ball));
  g.fillPath(triangle(xBall, cy, true));
}

void NoteAssignmentSlider::mouseMove(const juce::MouseEvent &e)
{
  const auto h = handleAt(e.position.y);
  if (h != hovered)
  {
    hovered = h;
    repaint();
  }
}

void NoteAssignmentSlider::mouseExit(const juce::MouseEvent &)
{
  if (hovered != Handle::none)
  {
    hovered = Handle::none;
    repaint();
  }
}

void NoteAssignmentSlider::mouseDown(const juce::MouseEvent &e)
{
  dragging = handleAt(e.position.y);
  ballAtPress = ballPos;
  const float handleX = xOf(dragging == Handle::ball ? ballPos : bracketPos);
  // Pressed on the handle: it keeps its offset from the pointer. Pressed on
  // the track: the handle jumps there.
  const bool onHandle = std::abs(e.position.x - handleX) <= kHalfWidth + 2.0f;
  grabOffset = onHandle ? e.position.x - handleX : 0.0f;
  repaint();
  if (!onHandle)
    mouseDrag(e);
}

void NoteAssignmentSlider::mouseDrag(const juce::MouseEvent &e)
{
  const float pos = positionAtX(e.position.x - grabOffset);
  if (dragging == Handle::ball)
  {
    if (pos == ballPos)
      return;
    ballPos = pos;
    ballProbability = probabilityAt(pos);
    if (bracketPos < ballPos)
    {
      bracketPos = ballPos;
      if (onBracketChanged)
        onBracketChanged(getBracket());
    }
    repaint();
    if (onBallDragging)
      onBallDragging(getBall());
  }
  else if (dragging == Handle::bracket)
  {
    const float next = std::max(ballPos, pos);
    if (next == bracketPos)
      return;
    bracketPos = next;
    repaint();
    if (onBracketChanged)
      onBracketChanged(getBracket());
  }
}

void NoteAssignmentSlider::mouseUp(const juce::MouseEvent &)
{
  const auto released = dragging;
  dragging = Handle::none;
  repaint();
  if (released == Handle::ball && onBallChanged)
    onBallChanged(getBall()); // also when unchanged: ends the preview
}

void NoteAssignmentSlider::mouseDoubleClick(const juce::MouseEvent &e)
{
  if (handleAt(e.position.y) == Handle::bracket)
  {
    bracketPos = std::max(ballPos, positionOf(kDefaultBracket));
    repaint();
    if (onBracketChanged)
      onBracketChanged(getBracket());
    return;
  }
  ballProbability = defaultBall;
  ballPos = positionOf(defaultBall);
  bracketPos = std::max(bracketPos, ballPos);
  repaint();
  if (onBracketChanged)
    onBracketChanged(getBracket());
  if (onBallChanged)
    onBallChanged(getBall());
}
