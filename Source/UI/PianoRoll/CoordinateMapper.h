#pragma once

#include "../../Utils/Constants.h"

/**
 * Handles coordinate transformations between screen space and musical space.
 * Manages zoom levels and scroll offsets for the piano roll view.
 */
class CoordinateMapper {
public:
    // Layout constants
    static constexpr int pianoKeysWidth = 30;
    static constexpr int timelineHeight = 18;
    static constexpr int loopTimelineHeight = 16;
    static constexpr int headerHeight = timelineHeight + loopTimelineHeight;

    CoordinateMapper() = default;

    // Zoom settings
    void setPixelsPerSecond(float pps) { pixelsPerSecond = juce::jlimit(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, pps); }
    void setPixelsPerSemitone(float pps) { pixelsPerSemitone = juce::jlimit(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, pps); }
    float getPixelsPerSecond() const { return pixelsPerSecond; }
    float getPixelsPerSemitone() const { return pixelsPerSemitone; }

    // Scroll settings
    void setScrollX(double x) { scrollX = std::max(0.0, x); }
    void setScrollY(double y) { scrollY = std::max(0.0, y); }
    double getScrollX() const { return scrollX; }
    double getScrollY() const { return scrollY; }

    // MIDI <-> Y coordinate conversion (in world space, before scroll)
    float midiToY(float midiNote) const {
        return (MAX_MIDI_NOTE - midiNote) * pixelsPerSemitone;
    }

    float yToMidi(float y) const {
        return MAX_MIDI_NOTE - y / pixelsPerSemitone;
    }

    // Time <-> X coordinate conversion (in world space, before scroll)
    float timeToX(double time) const {
        return static_cast<float>(time * pixelsPerSecond);
    }

    double xToTime(float x) const {
        return x / pixelsPerSecond;
    }

    // Frame <-> coordinate helpers
    int secondsToFrames(float seconds) const {
        return static_cast<int>(seconds * SAMPLE_RATE / HOP_SIZE);
    }

    float framesToSeconds(int frame) const {
        return static_cast<float>(frame) * HOP_SIZE / SAMPLE_RATE;
    }

    // Screen <-> World coordinate conversion
    float screenToWorldX(float screenX) const {
        return screenX - pianoKeysWidth + static_cast<float>(scrollX);
    }

    float screenToWorldY(float screenY) const {
        return screenY - headerHeight + static_cast<float>(scrollY);
    }

    float worldToScreenX(float worldX) const {
        return worldX + pianoKeysWidth - static_cast<float>(scrollX);
    }

    float worldToScreenY(float worldY) const {
        return worldY + headerHeight - static_cast<float>(scrollY);
    }

    // Get total content dimensions
    float getTotalWidth(float duration) const {
        return std::max(duration * pixelsPerSecond, 800.0f);
    }

    float getTotalHeight() const {
        return (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
    }

private:
    float pixelsPerSecond = DEFAULT_PIXELS_PER_SECOND;
    float pixelsPerSemitone = DEFAULT_PIXELS_PER_SEMITONE;
    double scrollX = 0.0;
    double scrollY = 0.0;
};
