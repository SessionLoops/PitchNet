#include "WorkspaceComponent.h"

WorkspaceComponent::WorkspaceComponent()
{
    setOpaque(true);

    mainCard.setPadding(0);
    mainCard.setCornerRadius(0.0f);
    mainCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
    addAndMakeVisible(mainCard);
    addAndMakeVisible(panelContainer);
    addChildComponent(leftPanelContainer);

    // Initially hide panel container (no panels visible)
    panelContainer.setVisible(false);
}

void WorkspaceComponent::SlideAnimation::start(bool visible)
{
    startProgress = progress;
    startMs = juce::Time::getMillisecondCounter();
    targetVisible = visible;
    active = true;
}

bool WorkspaceComponent::SlideAnimation::advance(juce::uint32 nowMs, int durationMs)
{
    if (!active)
        return false;

    const float target = targetVisible ? 1.0f : 0.0f;
    const float elapsed = static_cast<float>(nowMs - startMs) /
                          static_cast<float>(durationMs);
    const float t = juce::jlimit(0.0f, 1.0f, elapsed);
    const float eased = 1.0f - std::pow(1.0f - t, 3.0f);

    progress = startProgress + (target - startProgress) * eased;

    if (t < 1.0f)
        return false;

    progress = target;
    active = false;
    return true;
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
    const int panelGap = 0; // Side panel sits flush against the main view

    // Apply top margin first so sidebar aligns with content
    bounds.removeFromTop(topMargin);
    bounds.removeFromRight(margin); // Outer right padding

    // Apply left/bottom margins
    bounds.removeFromLeft(margin);
    bounds.removeFromBottom(margin);

    // Left panel: slides in from the left edge and pushes the main view. The
    // container keeps its full width and is positioned partly off-screen while
    // animating, so its content never reflows mid-slide.
    const float leftProgress = juce::jlimit(0.0f, 1.0f, leftPanelSlide.progress);
    if (leftProgress > 0.001f)
    {
        const int animatedLeftWidth = static_cast<int>(
            std::round(static_cast<float>(leftPanelWidth) * leftProgress));
        const auto fullBounds = bounds;

        bounds.removeFromLeft(animatedLeftWidth);
        leftPanelContainer.setBounds(fullBounds.getX() + animatedLeftWidth - leftPanelWidth,
                                     fullBounds.getY(),
                                     leftPanelWidth,
                                     fullBounds.getHeight());
    }
    else
    {
        leftPanelContainer.setBounds({});
    }

    const float progress = juce::jlimit(0.0f, 1.0f, rightPanelSlide.progress);
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
        rightPanelSlide.progress = 1.0f;
        rightPanelSlide.targetVisible = true;
        updatePanelContainerVisibility();

        if (onPanelVisibilityChanged)
            onPanelVisibilityChanged(id, true);
    }
}

void WorkspaceComponent::refreshPanelContentHeight(const juce::String& id)
{
    if (auto* panel = panelContainer.getPanel(id))
        panel->refreshContentPreferredHeight();
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

void WorkspaceComponent::setLeftPanelContent(const juce::String& id,
                                             const juce::String& title,
                                             juce::Component* content)
{
    if (leftPanelId.isNotEmpty())
        leftPanelContainer.removePanel(leftPanelId);

    if (content != nullptr)
        content->setSize(leftPanelWidth - 40, 520);

    auto panel = std::make_unique<DraggablePanel>(id, title);
    panel->setContentComponent(content);
    panel->setExtraTopPadding(5); // left panel sits 5px lower than the right
    leftPanelContainer.addPanel(std::move(panel));
    leftPanelContainer.showPanel(id, true);
    leftPanelId = id;
}

void WorkspaceComponent::refreshLeftPanelContentHeight()
{
    if (auto* panel = leftPanelContainer.getPanel(leftPanelId))
        panel->refreshContentPreferredHeight();
}

void WorkspaceComponent::showLeftPanel(bool show)
{
    if (show == leftPanelSlide.targetVisible && !leftPanelSlide.active)
        return;

    leftPanelSlide.start(show);
    if (show)
        leftPanelContainer.setVisible(true);

    startAnimationTimerIfNeeded();
    resized();
    repaint();

    if (onLayoutAnimationUpdated)
        onLayoutAnimationUpdated();

    if (onLeftPanelVisibilityChanged)
        onLeftPanelVisibilityChanged(show);
}

void WorkspaceComponent::startPanelAnimation(bool visible)
{
    rightPanelSlide.start(visible);

    if (visible)
        panelContainer.setVisible(true);

    startAnimationTimerIfNeeded();
    resized();
    repaint();

    if (onLayoutAnimationUpdated)
        onLayoutAnimationUpdated();
}

void WorkspaceComponent::startAnimationTimerIfNeeded()
{
    if (!isTimerRunning())
        startTimerHz(30);
}

void WorkspaceComponent::timerCallback()
{
    if (!rightPanelSlide.active && !leftPanelSlide.active)
    {
        stopTimer();
        return;
    }

    const auto now = juce::Time::getMillisecondCounter();

    if (rightPanelSlide.advance(now, panelAnimationMs) && !rightPanelSlide.targetVisible)
    {
        for (const auto& [id, requestedVisible] : requestedPanelVisibility)
            if (!requestedVisible)
                panelContainer.showPanel(id, false);

        panelContainer.setVisible(false);
    }

    if (leftPanelSlide.advance(now, panelAnimationMs) && !leftPanelSlide.targetVisible)
        leftPanelContainer.setVisible(false);

    if (!rightPanelSlide.active && !leftPanelSlide.active)
        stopTimer();

    resized();
    repaint();

    if (onLayoutAnimationUpdated)
        onLayoutAnimationUpdated();
}
