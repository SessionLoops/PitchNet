#pragma once

#include "../../../JuceHeader.h"
#include "../PianoRollInteractionContext.h"
#include "../../PianoRollComponent.h"

/**
 * Base class for PianoRoll interaction handlers.
 * Each handler encapsulates a specific interaction mode (select, draw, split, etc.).
 * Handlers access editor state through PianoRollInteractionContext.
 */
class InteractionHandler {
public:
  explicit InteractionHandler(PianoRollComponent &owner)
      : owner_(owner.getInteractionContext()) {}
  virtual ~InteractionHandler() = default;

  /// Called on mouse down. Returns true if the event was consumed.
  virtual bool mouseDown(const juce::MouseEvent &e, float worldX,
                         float worldY) {
    juce::ignoreUnused(e, worldX, worldY);
    return false;
  }

  /// Called on mouse drag. Returns true if the event was consumed.
  virtual bool mouseDrag(const juce::MouseEvent &e, float worldX,
                         float worldY) {
    juce::ignoreUnused(e, worldX, worldY);
    return false;
  }

  /// Called on mouse up. Returns true if the event was consumed.
  virtual bool mouseUp(const juce::MouseEvent &e, float worldX,
                       float worldY) {
    juce::ignoreUnused(e, worldX, worldY);
    return false;
  }

  /// Called on mouse move (no buttons pressed).
  virtual void mouseMove(const juce::MouseEvent &e, float worldX,
                         float worldY) {
    juce::ignoreUnused(e, worldX, worldY);
  }

  /// Called on mouse double-click.
  virtual void mouseDoubleClick(const juce::MouseEvent &e, float worldX,
                                float worldY) {
    juce::ignoreUnused(e, worldX, worldY);
  }

  /// Draw handler-specific overlays (guides, selection rects, etc.)
  virtual void draw(juce::Graphics &g) { juce::ignoreUnused(g); }

  /// Returns true if the handler is in an active drag/interaction state.
  virtual bool isActive() const { return false; }

  /// Cancel the current interaction (e.g., when undo is triggered mid-drag).
  virtual void cancel() {}

protected:
  PianoRollInteractionContext &owner_;

  InteractionHandler(const InteractionHandler &) = delete;
  InteractionHandler &operator=(const InteractionHandler &) = delete;
};
