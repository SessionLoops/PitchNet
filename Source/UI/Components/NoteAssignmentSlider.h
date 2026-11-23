#pragma once

#include "../../JuceHeader.h"

#include <functional>

/**
 * The note assignment slider (after Melodyne's): two handles on one track,
 * both probabilities of the note filter, fewest at the left. Drawn as two
 * triangles that point at the track, so they never cover each other: the
 * lower half of the slider belongs to the ball, the upper half to the bracket.
 *
 *  - The ball (filled triangle below the track, like a solid note): how likely a possible note must be to be a note. Every note at
 *    or above it is active, every other one a candidate. Dragging it previews
 *    (onBallDragging); letting go commits (onBallChanged), as one undo step.
 *  - The bracket (outlined triangle above the track, like a dashed candidate),
 *    at the ball or to its right: how likely a candidate must be
 *    to be shown (onBracketChanged). It cannot pass the ball; the ball pushes
 *    it along.
 *
 * The track is solid left of the ball (notes), dashed between the two
 * handles (candidates shown) and dim right of the bracket (hidden). A press
 * on the track moves the handle of that half there; double-click resets it (the ball to the
 * detection's own threshold, the bracket to its default). The far right
 * means "all" (probability 0).
 *
 * The scale is logistic, so the probabilities that matter (a few percent to
 * a few tens of percent) get most of the track.
 */
class NoteAssignmentSlider : public juce::Component, public juce::TooltipClient
{
public:
  NoteAssignmentSlider();

  void setBall(float probability);
  void setBracket(float probability);
  float getBall() const { return ballProbability; }
  float getBracket() const { return probabilityAt(bracketPos); }
  /** Where a double-click puts the ball (the detection's own threshold). */
  void setDefaultBall(float probability) { defaultBall = probability; }

  std::function<void(float)> onBallDragging;
  std::function<void(float)> onBallChanged;
  std::function<void(float)> onBracketChanged;

  /** Each half's tooltip: the threshold's (lower) and the candidates' (upper). */
  void setTooltips(juce::String lower, juce::String upper)
  {
    lowerTooltip = std::move(lower);
    upperTooltip = std::move(upper);
  }
  juce::String getTooltip() override;

  static float probabilityAt(float position);
  static float positionOf(float probability);

  void paint(juce::Graphics &g) override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void mouseMove(const juce::MouseEvent &e) override;
  void mouseExit(const juce::MouseEvent &e) override;

private:
  enum class Handle { none, ball, bracket };
  juce::Rectangle<float> track() const;
  float xOf(float position) const;
  float positionAtX(float x) const;
  /** The handle a press at y works on: the ball below the track, the bracket above. */
  Handle handleAt(float y) const;

  float ballPos = 0.5f;
  float ballProbability = 0.5f; // exact, so an untouched ball commits what was set
  float bracketPos = 0.75f;
  float defaultBall = 0.5f;
  Handle dragging = Handle::none;
  Handle hovered = Handle::none;
  juce::String lowerTooltip, upperTooltip;
  float grabOffset = 0.0f;
  float ballAtPress = 0.0f;
};
