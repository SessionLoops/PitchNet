#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Note.h"
#include "CoordinateMapper.h"
#include <vector>

/**
 * Visual handles for pitch tool operations on selected notes.
 */
class PitchToolHandles {
public:
  enum class HandleType {
    TiltLeft,
    TiltRight,
    Vibrato,
    SmoothLeft,
    SmoothRight,
    None
  };

  static constexpr float buttonWidth = 30.0f;
  static constexpr float buttonHeight = 10.0f;
  static constexpr float buttonGap = 5.0f;
  static constexpr float buttonGroupWidth =
      buttonWidth * 3.0f + buttonGap * 2.0f;

  struct Handle {
    HandleType type;
    juce::Rectangle<float> bounds;  // World coordinates (matches mouse event coordinates)
    Note* note;  // Associated note (for per-note handles)
    juce::Colour color;
  };

  PitchToolHandles();

  /**
   * Update handle positions based on current note selection.
   * Call this when selection changes or viewport moves.
   */
  void updateHandles(const std::vector<Note*>& selectedNotes,
                     const CoordinateMapper& mapper,
                     const juce::Rectangle<float>& hoverBounds = {});

  /**
   * Draw all handles to graphics context.
   */
  void draw(juce::Graphics& g) const;

  /**
   * Hit-test for mouse interaction.
   * @param worldX Mouse X in world coordinates
   * @param worldY Mouse Y in world coordinates
   * @param tolerance Hit-test radius in pixels (default 12.0)
   * @return Index of hit handle, or -1 if no hit
   */
  int hitTest(float worldX, float worldY, float tolerance = 12.0f) const;

  /** Whether a point lies inside the full three-control layout, including gaps. */
  bool containsLayoutPoint(float worldX, float worldY) const;

  /** Bounding box of the full three-control layout. */
  juce::Rectangle<float> getLayoutBounds() const;

  /**
   * Get handle at index.
   */
  const Handle& getHandle(int index) const { return handles[index]; }

  /**
   * Get all handles.
   */
  const std::vector<Handle>& getHandles() const { return handles; }

  /**
   * Clear all handles (call when selection is empty).
   */
  void clear() { handles.clear(); }

  /**
   * Check if any handles are visible.
   */
  bool isEmpty() const { return handles.empty(); }

  /**
   * Set the index of the currently hovered handle.
   * @param index Index of handle, or -1 for none.
   */
  void setHoveredHandleIndex(int index) { hoveredHandleIndex = index; }

private:
  std::vector<Handle> handles;
  int hoveredHandleIndex = -1;

  void addHandle(HandleType type, float worldX, float worldY, Note* note = nullptr);
  juce::Colour getColorForType(HandleType type) const;
};
