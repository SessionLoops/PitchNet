#include "WorkspaceComponent.h"

WorkspaceComponent::WorkspaceComponent()
{
    setOpaque(true);

    mainCard.setPadding(0);
    mainCard.setCornerRadius(0.0f);
    mainCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
    addAndMakeVisible(mainCard);
    addAndMakeVisible(panelContainer);

    // Initially hide panel container (no panels visible)
    panelContainer.setVisible(false);
}

void WorkspaceComponent::paint(juce::Graphics& g)
{
    // Clean flat background
    g.fillAll(APP_COLOR_BACKGROUND);
}

void WorkspaceComponent::resized()
{
    auto bounds = getLocalBounds();
    const int margin = 0;
    const int topMargin = 0;
    const int panelGap = 8; // Gap between piano roll and panel

    // Apply top margin first so sidebar aligns with content
    bounds.removeFromTop(topMargin);
    bounds.removeFromRight(margin); // Outer right padding

    // Apply left/bottom margins
    bounds.removeFromLeft(margin);
    bounds.removeFromBottom(margin);

    // No sidebar; main content starts after outer margin

    const float progress = juce::jlimit(0.0f, 1.0f, panelAnimationProgress);
    if (progress > 0.001f)
    {
        const int animatedPanelWidth = static_cast<int>(
            std::round(static_cast<float>(panelContainerWidth) * progress));
        const int animatedGap = static_cast<int>(
            std::round(static_cast<float>(panelGap) * progress));
        const auto fullBounds = bounds;

        bounds.removeFromRight(animatedPanelWidth + animatedGap);
        panelContainer.setBounds(fullBounds.getRight() - animatedPanelWidth,
                                 fullBounds.getY(),
                                 panelContainerWidth,
                                 fullBounds.getHeight());
    }
    else
    {
        panelContainer.setBounds({});
    }

    // Main content card
    mainCard.setBounds(bounds);
}

void WorkspaceComponent::setMainContent(juce::Component* content)
{
    mainContent = content;
    mainCard.setContentComponent(content);
}

void WorkspaceComponent::addPanel(const juce::String& id, const juce::String& title,
                                   juce::Component* content,
                                   bool initiallyVisible)
{
    // Set content size before adding to panel
    if (content != nullptr)
        content->setSize(panelContainerWidth - 40, 520);

    // Create draggable panel wrapper
    auto panel = std::make_unique<DraggablePanel>(id, title);
    panel->setContentComponent(content);

    // Add to panel container
    panelContainer.addPanel(std::move(panel));
    requestedPanelVisibility[id] = initiallyVisible;

    // Set initial visibility
    if (initiallyVisible)
    {
        panelContainer.showPanel(id, true);
        panelAnimationProgress = 1.0f;
        panelAnimationTargetVisible = true;
        updatePanelContainerVisibility();

        if (onPanelVisibilityChanged)
            onPanelVisibilityChanged(id, true);
    }
}

void WorkspaceComponent::showPanel(const juce::String& id, bool show)
{
    requestedPanelVisibility[id] = show;

    bool hasRequestedPanels = false;
    for (const auto& [panelId, requestedVisible] : requestedPanelVisibility)
    {
        juce::ignoreUnused(panelId);
        hasRequestedPanels = hasRequestedPanels || requestedVisible;
    }

    if (show || hasRequestedPanels)
        panelContainer.showPanel(id, show);

    startPanelAnimation(hasRequestedPanels);

    if (onPanelVisibilityChanged)
        onPanelVisibilityChanged(id, show);
}

bool WorkspaceComponent::isPanelVisible(const juce::String& id) const
{
    const auto it = requestedPanelVisibility.find(id);
    return it != requestedPanelVisibility.end() && it->second;
}

void WorkspaceComponent::updatePanelContainerVisibility()
{
    bool hasPanels = false;
    for (const auto& id : panelContainer.getPanelOrder())
    {
        if (panelContainer.isPanelVisible(id))
        {
            hasPanels = true;
            break;
        }
    }

    panelContainer.setVisible(hasPanels);
    resized();
}

void WorkspaceComponent::startPanelAnimation(bool visible)
{
    panelAnimationStartProgress = panelAnimationProgress;
    panelAnimationStartMs = juce::Time::getMillisecondCounter();
    panelAnimationTargetVisible = visible;
    panelAnimationActive = true;

    if (visible)
        panelContainer.setVisible(true);

    startTimerHz(30);
    resized();
    repaint();

    if (onLayoutAnimationUpdated)
        onLayoutAnimationUpdated();
}

void WorkspaceComponent::timerCallback()
{
    if (!panelAnimationActive)
        return;

    const float target = panelAnimationTargetVisible ? 1.0f : 0.0f;
    const auto now = juce::Time::getMillisecondCounter();
    const float elapsed = static_cast<float>(now - panelAnimationStartMs) /
                          static_cast<float>(panelAnimationMs);
    const float t = juce::jlimit(0.0f, 1.0f, elapsed);
    const float eased = 1.0f - std::pow(1.0f - t, 3.0f);

    panelAnimationProgress = panelAnimationStartProgress +
                             (target - panelAnimationStartProgress) * eased;

    if (t >= 1.0f)
    {
        panelAnimationProgress = target;
        panelAnimationActive = false;
        stopTimer();

        if (!panelAnimationTargetVisible)
        {
            for (const auto& [id, requestedVisible] : requestedPanelVisibility)
                if (!requestedVisible)
                    panelContainer.showPanel(id, false);

            panelContainer.setVisible(false);
        }
    }

    resized();
    repaint();

    if (onLayoutAnimationUpdated)
        onLayoutAnimationUpdated();
}
