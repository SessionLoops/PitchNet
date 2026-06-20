#pragma once

#include "../../JuceHeader.h"
#include "RoundedCard.h"
#include "PanelContainer.h"
#include "DraggablePanel.h"

/**
 * Main workspace component that manages the layout of:
 * - Piano roll (main content area with rounded card)
 * - Panel container (right side panels)
 */
class WorkspaceComponent : public juce::Component,
                           private juce::Timer
{
public:
    WorkspaceComponent();
    ~WorkspaceComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setMainContent(juce::Component* content);
    void addPanel(const juce::String& id, const juce::String& title,
                  juce::Component* content,
                  bool initiallyVisible = false);

    void showPanel(const juce::String& id, bool show);
    bool isPanelVisible(const juce::String& id) const;

    PanelContainer& getPanelContainer() { return panelContainer; }
    RoundedCard& getMainCard() { return mainCard; }
    int getMainViewRight() const { return mainCard.getRight(); }

    std::function<void(const juce::String&, bool)> onPanelVisibilityChanged;
    std::function<void()> onLayoutAnimationUpdated;

private:
    void updatePanelContainerVisibility();
    void startPanelAnimation(bool visible);
    void timerCallback() override;

    RoundedCard mainCard;
    PanelContainer panelContainer;

    juce::Component* mainContent = nullptr;
    int panelContainerWidth = 250;
    std::map<juce::String, bool> requestedPanelVisibility;
    float panelAnimationProgress = 0.0f;
    float panelAnimationStartProgress = 0.0f;
    juce::uint32 panelAnimationStartMs = 0;
    bool panelAnimationActive = false;
    bool panelAnimationTargetVisible = false;

    static constexpr int panelAnimationMs = 220;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WorkspaceComponent)
};
