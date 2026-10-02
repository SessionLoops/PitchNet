#pragma once

#include "../../JuceHeader.h"
#include "RoundedCard.h"
#include "PanelContainer.h"
#include "DraggablePanel.h"

/**
 * Main workspace component that manages the layout of:
 * - Piano roll (main content area with rounded card)
 * - Panel container (right side panels)
 * - Left panel container (slides in from the left, same width as the right)
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
    /** Re-measure one panel's content after its natural height changed. */
    void refreshPanelContentHeight(const juce::String& id);
    bool isPanelVisible(const juce::String& id) const;

    // Left side panel: a single panel that slides in from the left edge and
    // pushes the main view, mirroring the right-hand panel container.
    void setLeftPanelContent(const juce::String& id, const juce::String& title,
                             juce::Component* content);
    void showLeftPanel(bool show);
    /** Re-measure the left panel's content after its natural height changed. */
    void refreshLeftPanelContentHeight();
    bool isLeftPanelVisible() const { return leftPanelSlide.targetVisible; }

    PanelContainer& getPanelContainer() { return panelContainer; }
    RoundedCard& getMainCard() { return mainCard; }
    int getMainViewRight() const { return mainCard.getRight(); }
    int getMainViewX() const { return mainCard.getX(); }

    std::function<void(const juce::String&, bool)> onPanelVisibilityChanged;
    std::function<void(bool)> onLeftPanelVisibilityChanged;
    std::function<void()> onLayoutAnimationUpdated;

private:
    // Eased 0..1 open/close progress for one side panel.
    struct SlideAnimation
    {
        float progress = 0.0f;
        float startProgress = 0.0f;
        juce::uint32 startMs = 0;
        bool active = false;
        bool targetVisible = false;

        void start(bool visible);
        /** Advances the animation; returns true when it just finished. */
        bool advance(juce::uint32 nowMs, int durationMs);
    };

    void updatePanelContainerVisibility();
    void startPanelAnimation(bool visible);
    void startAnimationTimerIfNeeded();
    void timerCallback() override;

    RoundedCard mainCard;
    PanelContainer panelContainer;
    PanelContainer leftPanelContainer;
    juce::String leftPanelId;

    juce::Component* mainContent = nullptr;
    int panelContainerWidth = 250;
    // Left panel: two thirds of the right one, plus 15px.
    int leftPanelWidth = panelContainerWidth * 2 / 3 + 15;
    std::map<juce::String, bool> requestedPanelVisibility;
    SlideAnimation rightPanelSlide;
    SlideAnimation leftPanelSlide;

    static constexpr int panelAnimationMs = 220;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WorkspaceComponent)
};
