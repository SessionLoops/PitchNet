#pragma once

#include "../../JuceHeader.h"

// UI theme colors (defined in Theme.cpp)
extern const juce::Colour APP_COLOR_BACKGROUND;
extern const juce::Colour APP_COLOR_SURFACE;
extern const juce::Colour APP_COLOR_SURFACE_ALT;
extern const juce::Colour APP_COLOR_SURFACE_RAISED;
extern const juce::Colour APP_COLOR_BORDER;
extern const juce::Colour APP_COLOR_BORDER_SUBTLE;
extern const juce::Colour APP_COLOR_BORDER_HIGHLIGHT;
extern const juce::Colour APP_COLOR_GRID;
extern const juce::Colour APP_COLOR_GRID_BAR;
extern const juce::Colour APP_COLOR_TIMELINE;
extern const juce::Colour APP_COLOR_PIANO_WHITE;
extern const juce::Colour APP_COLOR_PIANO_BLACK;
extern const juce::Colour APP_COLOR_PIANO_TEXT;
extern const juce::Colour APP_COLOR_PIANO_TEXT_DIM;
extern const juce::Colour APP_COLOR_TEXT_PRIMARY;
extern const juce::Colour APP_COLOR_TEXT_MUTED;
extern const juce::Colour APP_COLOR_PITCH_CURVE;
extern const juce::Colour APP_COLOR_NOTE_NORMAL;
extern const juce::Colour APP_COLOR_NOTE_SELECTED;
extern const juce::Colour APP_COLOR_NOTE_HOVER;
extern const juce::Colour APP_COLOR_PRIMARY;
extern const juce::Colour APP_COLOR_PRIMARY_GLOW;
extern const juce::Colour APP_COLOR_SECONDARY;
extern const juce::Colour APP_COLOR_WAVEFORM;
extern const juce::Colour APP_COLOR_KNOB_SHADOW;
extern const juce::Colour APP_COLOR_ALERT_WARNING;
extern const juce::Colour APP_COLOR_ALERT_ERROR;
extern const juce::Colour APP_COLOR_OVERLAY_DIM;
extern const juce::Colour APP_COLOR_OVERLAY_SHADOW;
extern const juce::Colour APP_COLOR_SELECTION_OVERLAY;
extern const juce::Colour APP_COLOR_SELECTION_HIGHLIGHT;
extern const juce::Colour APP_COLOR_SELECTION_HIGHLIGHT_STRONG;
extern const juce::Colour APP_COLOR_TITLEBAR_CLOSE_MAC;
extern const juce::Colour APP_COLOR_TITLEBAR_MINIMIZE_MAC;
extern const juce::Colour APP_COLOR_TITLEBAR_MAXIMIZE_MAC;
extern const juce::Colour APP_COLOR_TITLEBAR_CLOSE_HOVER;

// ── Scrollbars ────────────────────────────────────────────────────
// One look for every scrollbar in the app: a dim grey rounded thumb over a
// recessed gutter. The piano-roll canvas set the look; styleCanvasScrollBar()
// is how everything else adopts it, so the side panel and the canvas cannot
// drift apart. The default JUCE renderer draws the thumb only - whoever hosts
// the bar fills APP_COLOR_SCROLLBAR_TRACK behind it.
extern const juce::Colour APP_COLOR_SCROLLBAR_THUMB;
extern const juce::Colour APP_COLOR_SCROLLBAR_TRACK;
constexpr int APP_SCROLLBAR_THICKNESS = 8;

void styleCanvasScrollBar(juce::ScrollBar& scrollBar);
