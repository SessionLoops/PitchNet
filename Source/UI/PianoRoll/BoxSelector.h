#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"
#include <vector>

/**
 * Handles box selection (marquee selection) for notes in the piano roll.
 */
class BoxSelector {
public:
    BoxSelector() = default;
    ~BoxSelector() = default;

    void startSelection(float x, float y);
    void updateSelection(float x, float y);
    void endSelection();
    void setLastSelectionFromBox(bool wasFromBox) { lastSelectionFromBox = wasFromBox; }
    bool wasLastSelectionFromBox() const { return lastSelectionFromBox; }

    bool isSelecting() const { return selecting; }
    juce::Rectangle<float> getSelectionRect() const;

    std::vector<Note*> getNotesInRect(Project* project, CoordinateMapper* mapper) const;

private:
    bool selecting = false;
    bool lastSelectionFromBox = false;
    juce::Point<float> startPoint;
    juce::Point<float> endPoint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BoxSelector)
};
