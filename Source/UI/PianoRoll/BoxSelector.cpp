#include "BoxSelector.h"

void BoxSelector::startSelection(float x, float y) {
    selecting = true;
    lastSelectionFromBox = false;
    startPoint = {x, y};
    endPoint = {x, y};
}

void BoxSelector::updateSelection(float x, float y) {
    if (selecting) {
        endPoint = {x, y};
    }
}

void BoxSelector::endSelection() {
    selecting = false;
}

juce::Rectangle<float> BoxSelector::getSelectionRect() const {
    float x1 = std::min(startPoint.x, endPoint.x);
    float y1 = std::min(startPoint.y, endPoint.y);
    float x2 = std::max(startPoint.x, endPoint.x);
    float y2 = std::max(startPoint.y, endPoint.y);
    return {x1, y1, x2 - x1, y2 - y1};
}

std::vector<Note*> BoxSelector::getNotesInRect(Project* project, CoordinateMapper* mapper) const {
    std::vector<Note*> result;
    if (!project || !mapper)
        return result;

    auto rect = getSelectionRect();

    for (auto& note : project->getNotes()) {
        if (note.isRest())
            continue;

        float noteX = framesToSeconds(note.getStartFrame()) * mapper->getPixelsPerSecond();
        float noteW = framesToSeconds(note.getDurationFrames()) * mapper->getPixelsPerSecond();
        float noteY = mapper->midiToY(note.getAdjustedMidiNote());
        float noteH = mapper->getPixelsPerSemitone();

        juce::Rectangle<float> noteRect(noteX, noteY, noteW, noteH);

        if (rect.intersects(noteRect)) {
            result.push_back(&note);
        }
    }

    return result;
}
