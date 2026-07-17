#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "Components/StyledWidgets.h"
#include <functional>
#include <optional>

class ScaleSelectionControl : public CompactSelectionButton
{
public:
    ScaleSelectionControl();

    void setProject(Project* projectToUse);
    void showPopup();
    Project* getProject() const { return project; }
    void setScaleRoot(int rootNote);
    void setScaleMode(ScaleMode mode);
    int getPreferredWidth() const;

    std::function<void(int)> onScaleRootChanged;
    std::function<void(ScaleMode)> onScaleModeChanged;
    std::function<void(std::optional<int>)> onScaleRootPreviewChanged;
    std::function<void(std::optional<ScaleMode>)> onScaleModePreviewChanged;
    std::function<void()> onPreferredWidthChanged;

    void previewScaleRoot(std::optional<int> rootNote);
    void previewScaleMode(std::optional<ScaleMode> mode);

private:
    void refreshLabel();

    Project* project = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScaleSelectionControl)
};
