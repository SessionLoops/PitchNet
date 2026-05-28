#include "ScrollZoomController.h"
#include "../../Utils/UI/Theme.h"

ScrollZoomController::ScrollZoomController() {
    horizontalScrollBar.addListener(this);
    verticalScrollBar.addListener(this);

    auto thumbColor = juce::Colour(0xFF565656u);
    auto trackColor = juce::Colour(0xFF0D0B0Bu);

    horizontalScrollBar.setColour(juce::ScrollBar::thumbColourId, thumbColor);
    horizontalScrollBar.setColour(juce::ScrollBar::trackColourId, trackColor);
    horizontalScrollBar.setColour(juce::ScrollBar::backgroundColourId, trackColor);
    verticalScrollBar.setColour(juce::ScrollBar::thumbColourId, thumbColor);
    verticalScrollBar.setColour(juce::ScrollBar::trackColourId, trackColor);
    verticalScrollBar.setColour(juce::ScrollBar::backgroundColourId, trackColor);

    verticalScrollBar.setRangeLimits(0, (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * DEFAULT_PIXELS_PER_SEMITONE);
    verticalScrollBar.setCurrentRange(0, 500);
}

ScrollZoomController::~ScrollZoomController() {
    horizontalScrollBar.removeListener(this);
    verticalScrollBar.removeListener(this);
}

void ScrollZoomController::scrollBarMoved(juce::ScrollBar* scrollBar, double newRangeStart) {
    if (!coordMapper)
        return;

    if (scrollBar == &horizontalScrollBar) {
        coordMapper->setScrollX(newRangeStart);
        if (onScrollChanged)
            onScrollChanged(newRangeStart);
    } else if (scrollBar == &verticalScrollBar) {
        coordMapper->setScrollY(newRangeStart);
    }

    if (onRepaintNeeded)
        onRepaintNeeded();
}

void ScrollZoomController::handleMouseWheel(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel,
                                            int pianoKeysWidth, int headerHeight,
                                            int componentWidth, int componentHeight) {
    if (!coordMapper)
        return;

    float scrollMultiplier = wheel.isSmooth ? 200.0f : 80.0f;

    bool isOverPianoKeys = e.x < pianoKeysWidth;
    bool isOverTimeline = e.y < headerHeight;

    if (!e.mods.isCommandDown() && !e.mods.isCtrlDown()) {
        // Hover-based zoom
        if (isOverPianoKeys) {
            // Vertical zoom
            float mouseY = e.y - headerHeight;

            float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
            float newPps = coordMapper->getPixelsPerSemitone() * zoomFactor;
            const int visibleHeight = componentHeight - headerHeight - 14;
            const float minPpsForFill =
                visibleHeight > 0
                    ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
                    : MIN_PIXELS_PER_SEMITONE;
            const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
            if (zoomFactor < 1.0f)
            {
                const float range = minPps * 0.35f;
                const float currentPps = coordMapper->getPixelsPerSemitone();
                const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (currentPps - minPps) / range) : 0.0f;
                newPps = currentPps * (1.0f + (zoomFactor - 1.0f) * t);
            }
            newPps = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, newPps);
            setPixelsPerSemitone(newPps, mouseY,
                                 componentHeight - headerHeight - 14);

            updateScrollBars(componentWidth - pianoKeysWidth - 14, componentHeight - headerHeight - 14);
            if (onRepaintNeeded) onRepaintNeeded();
            return;
        }

        if (isOverTimeline) {
            // Horizontal zoom
            float mouseX = e.x - pianoKeysWidth;
            double timeAtMouse = (mouseX + coordMapper->getScrollX()) / coordMapper->getPixelsPerSecond();

            float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
            float newPps = coordMapper->getPixelsPerSecond() * zoomFactor;
            newPps = juce::jlimit(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, newPps);
            coordMapper->setPixelsPerSecond(newPps);

            double newScrollX = timeAtMouse * newPps - mouseX;
            coordMapper->setScrollX(std::max(0.0, newScrollX));

            updateScrollBars(componentWidth - pianoKeysWidth - 14, componentHeight - headerHeight - 14);
            if (onRepaintNeeded) onRepaintNeeded();
            if (onZoomChanged) onZoomChanged(newPps);
            return;
        }

        // Normal scrolling
        float deltaX = wheel.deltaX;
        float deltaY = wheel.deltaY;

        if (e.mods.isShiftDown() && std::abs(deltaX) < 0.001f) {
            deltaX = deltaY;
            deltaY = 0.0f;
        }

        if (std::abs(deltaX) > 0.001f) {
            double newScrollX = coordMapper->getScrollX() - deltaX * scrollMultiplier;
            newScrollX = std::max(0.0, newScrollX);
            horizontalScrollBar.setCurrentRangeStart(newScrollX);
        }

        if (std::abs(deltaY) > 0.001f) {
            double newScrollY = coordMapper->getScrollY() - deltaY * scrollMultiplier;
            verticalScrollBar.setCurrentRangeStart(newScrollY);
        }
        return;
    }

    // Key-based zoom: XY simultaneous (like other DAWs)
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
        float zoomFactor = 1.0f + wheel.deltaY * 0.3f;

        // Calculate mouse position relative to content area
        float mouseX = static_cast<float>(e.x - pianoKeysWidth);
        float mouseY = static_cast<float>(e.y - headerHeight);

        // Get current position under mouse
        double timeAtMouse = coordMapper->xToTime(mouseX + static_cast<float>(coordMapper->getScrollX()));
        float midiAtMouse = (mouseY + static_cast<float>(coordMapper->getScrollY())) / coordMapper->getPixelsPerSemitone();

        // Apply horizontal zoom
        float newPpsX = coordMapper->getPixelsPerSecond() * zoomFactor;
        newPpsX = juce::jlimit(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, newPpsX);
        coordMapper->setPixelsPerSecond(newPpsX);

        // Apply vertical zoom
        float newPpsY = coordMapper->getPixelsPerSemitone() * zoomFactor;
        {
            const int visibleHeight = componentHeight - headerHeight - 14;
            const float minPpsForFill =
                visibleHeight > 0
                    ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
                    : MIN_PIXELS_PER_SEMITONE;
            const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
            if (zoomFactor < 1.0f)
            {
                const float range = minPps * 0.35f;
                const float currentPps = coordMapper->getPixelsPerSemitone();
                const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (currentPps - minPps) / range) : 0.0f;
                newPpsY = currentPps * (1.0f + (zoomFactor - 1.0f) * t);
            }
            newPpsY = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, newPpsY);
        }
        setPixelsPerSemitone(newPpsY, mouseY,
                             componentHeight - headerHeight - 14);

        // Adjust scroll to keep mouse position stable
        float newMouseX = static_cast<float>(timeAtMouse * newPpsX);
        coordMapper->setScrollX(std::max(0.0, static_cast<double>(newMouseX - mouseX)));

        updateScrollBars(componentWidth - pianoKeysWidth - 14, componentHeight - headerHeight - 14);
        if (onRepaintNeeded) onRepaintNeeded();
        if (onZoomChanged) onZoomChanged(newPpsX);
    }
}

void ScrollZoomController::handleMagnify(const juce::MouseEvent& e, float scaleFactor, int pianoKeysWidth) {
    if (!coordMapper)
        return;

    float mouseX = static_cast<float>(e.x - pianoKeysWidth);
    double timeAtMouse = coordMapper->xToTime(mouseX + static_cast<float>(coordMapper->getScrollX()));

    float newPps = coordMapper->getPixelsPerSecond() * scaleFactor;
    newPps = juce::jlimit(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, newPps);

    float newMouseX = static_cast<float>(timeAtMouse * newPps);
    coordMapper->setScrollX(std::max(0.0, static_cast<double>(newMouseX - mouseX)));
    coordMapper->setPixelsPerSecond(newPps);

    if (onRepaintNeeded) onRepaintNeeded();
    if (onZoomChanged) onZoomChanged(newPps);
}

void ScrollZoomController::setScrollX(double x) {
    if (!coordMapper)
        return;

    if (std::abs(coordMapper->getScrollX() - x) < 0.01)
        return;

    coordMapper->setScrollX(x);
    horizontalScrollBar.setCurrentRangeStart(x);

    if (onRepaintNeeded) onRepaintNeeded();
}

void ScrollZoomController::setPixelsPerSecond(float pps, bool centerOnCursor, double cursorTime) {
    if (!coordMapper)
        return;

    float oldPps = coordMapper->getPixelsPerSecond();
    float newPps = juce::jlimit(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, pps);

    if (std::abs(oldPps - newPps) < 0.01f)
        return;

    if (centerOnCursor) {
        float cursorX = static_cast<float>(cursorTime * oldPps);
        float cursorRelativeX = cursorX - static_cast<float>(coordMapper->getScrollX());
        float newCursorX = static_cast<float>(cursorTime * newPps);
        coordMapper->setScrollX(std::max(0.0, static_cast<double>(newCursorX - cursorRelativeX)));
    }

    coordMapper->setPixelsPerSecond(newPps);

    if (onRepaintNeeded) onRepaintNeeded();
}

void ScrollZoomController::setPixelsPerSemitone(float pps, float anchorContentY,
                                                int visibleHeight) {
    if (!coordMapper)
        return;

    const float oldPps = coordMapper->getPixelsPerSemitone();
    const int effectiveVisibleHeight = visibleHeight > 0 ? visibleHeight
                                                         : static_cast<int>(verticalScrollBar.getCurrentRangeSize());
    const float minPpsForFill =
        effectiveVisibleHeight > 0
            ? static_cast<float>(effectiveVisibleHeight) /
                  (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
            : MIN_PIXELS_PER_SEMITONE;
    const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
    const float newPps = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, pps);
    if (std::abs(oldPps - newPps) < 0.01f)
        return;

    float effectiveAnchorY = anchorContentY;
    if (effectiveAnchorY < 0.0f)
        effectiveAnchorY = static_cast<float>(effectiveVisibleHeight) * 0.5f;
    effectiveAnchorY = juce::jlimit(0.0f, static_cast<float>(effectiveVisibleHeight),
                                    effectiveAnchorY);

    const float midiAtAnchor =
        MAX_MIDI_NOTE -
        (effectiveAnchorY + static_cast<float>(coordMapper->getScrollY())) / oldPps;

    coordMapper->setPixelsPerSemitone(newPps);

    const double totalHeight =
        (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * coordMapper->getPixelsPerSemitone();
    const double maxScrollY =
        std::max(0.0, totalHeight - static_cast<double>(effectiveVisibleHeight));
    const double anchoredScrollY =
        (MAX_MIDI_NOTE - midiAtAnchor) * coordMapper->getPixelsPerSemitone() -
        effectiveAnchorY;
    coordMapper->setScrollY(juce::jlimit(0.0, maxScrollY, anchoredScrollY));

    if (onRepaintNeeded) onRepaintNeeded();
}

void ScrollZoomController::centerOnPitchRange(float minMidi, float maxMidi, int visibleHeight) {
    if (!coordMapper)
        return;

    float centerMidi = (minMidi + maxMidi) / 2.0f;
    float centerY = coordMapper->midiToY(centerMidi);

    double newScrollY = centerY - visibleHeight / 2.0;
    double totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * coordMapper->getPixelsPerSemitone();
    newScrollY = juce::jlimit(0.0, std::max(0.0, totalHeight - visibleHeight), newScrollY);

    coordMapper->setScrollY(newScrollY);
    verticalScrollBar.setCurrentRangeStart(newScrollY);

    if (onRepaintNeeded) onRepaintNeeded();
}

void ScrollZoomController::updateScrollBars(int visibleWidth, int visibleHeight) {
    if (!coordMapper)
        return;

    float duration = project ? project->getAudioData().getDuration() : 60.0f;
    float totalWidth = duration * coordMapper->getPixelsPerSecond();
    float totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * coordMapper->getPixelsPerSemitone();

    horizontalScrollBar.setRangeLimits(0, totalWidth);
    horizontalScrollBar.setCurrentRange(coordMapper->getScrollX(), visibleWidth);

    verticalScrollBar.setRangeLimits(0, totalHeight);
    verticalScrollBar.setCurrentRange(coordMapper->getScrollY(), visibleHeight);
}
