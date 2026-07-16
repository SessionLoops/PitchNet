#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "Components/StyledWidgets.h"
#include <functional>

class ScaleSelectionControl : public CompactSelectionButton
{
public:
    ScaleSelectionControl();

    void setProject(Project* projectToUse);
    void showPopup();
    Project* getProject() const { return project; }
    void setScaleRoot(int rootNote);
    void setScaleMode(ScaleMode mode);
    void setDetectedScale(int rootNote, ScaleMode mode);

    std::function<void(int)> onScaleRootChanged;
    std::function<void(ScaleMode)> onScaleModeChanged;

private:
    void refreshLabel();

    Project* project = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScaleSelectionControl)
};
