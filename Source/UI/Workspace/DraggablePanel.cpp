#include "DraggablePanel.h"
#include "PanelContainer.h"

DraggablePanel::DraggablePanel(const juce::String& id, const juce::String& panelTitle)
    : panelId(id), title(panelTitle)
{
    setOpaque(false);

    // Vertical only, and only when the content does not fit. A horizontal bar
    // would fight the content, which is laid out to whatever width it is given.
    contentViewport.setScrollBarsShown(true, false);
    contentViewport.setScrollBarThickness(APP_SCROLLBAR_THICKNESS);
    contentViewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);

    // Same bar as the piano-roll canvas: same width, same colours, and no look
    // and feel of its own so the default renderer draws both. Installing one
    // here would also leak into every control inside the panel.
    styleCanvasScrollBar(contentViewport.getVerticalScrollBar());

    addAndMakeVisible(contentViewport);
}

void DraggablePanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const float corner = 8.0f;

    // Clip to rounded rectangle for entire panel
    juce::Path clipPath;
    clipPath.addRoundedRectangle(bounds, corner);
    g.reduceClipRegion(clipPath);

    // Side-panel background
    g.setColour(juce::Colour(0xFF232323u));
    g.fillRect(bounds);

    // Gutter behind the scrollbar, as the canvas paints behind its own: the
    // look and feel renders the thumb and leaves the track to the host.
    const auto& verticalBar = contentViewport.getVerticalScrollBar();
    if (verticalBar.isVisible())
    {
        g.setColour(APP_COLOR_SCROLLBAR_TRACK);
        g.fillRect(verticalBar.getBounds().translated(contentViewport.getX(),
                                                      contentViewport.getY()));
    }
}

void DraggablePanel::paintOverChildren(juce::Graphics& g)
{
    // Clean rounded border
    auto bounds = getLocalBounds().toFloat();
    g.setColour(APP_COLOR_BORDER.withAlpha(0.5f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 0.75f);
}

void DraggablePanel::resized()
{
    const auto contentBounds =
        getLocalBounds().withTrimmedTop(headerHeight).reduced(contentMargin);

    // The viewport reaches past the content into the right margin, which is
    // where the scrollbar lands. The content itself is only ever as wide as
    // contentBounds, so a bar appearing never reflows the cards - it just
    // fills margin that was empty anyway.
    const auto viewportBounds = contentBounds.withRight(
        juce::jmax(contentBounds.getX(), getWidth() - scrollBarEdgeInset));
    contentViewport.setBounds(viewportBounds);

    if (contentComponent == nullptr)
        return;

    // Give the content its natural height whenever the panel is too short for
    // it - the viewport then shows a scrollbar - and let it fill the panel
    // otherwise, so a tall window looks exactly as it did before.
    contentComponent->setSize(contentBounds.getWidth(),
                              juce::jmax(getPreferredContentHeight(),
                                         viewportBounds.getHeight()));

    // The gutter is painted here, so a bar that just appeared or went away
    // needs the background redrawn under it.
    repaint();
}

void DraggablePanel::mouseDown(const juce::MouseEvent& e)
{
    auto headerBounds = getLocalBounds().removeFromTop(headerHeight);

    if (headerBounds.contains(e.getPosition()))
    {
        // Start drag for reordering
        isDragging = true;
        dragStartPos = e.getPosition();
    }
}

void DraggablePanel::mouseDrag(const juce::MouseEvent& e)
{
    if (isDragging && panelContainer != nullptr)
    {
        auto delta = e.getPosition() - dragStartPos;
        if (std::abs(delta.y) > 10)
        {
            panelContainer->handlePanelDrag(this, e.getEventRelativeTo(panelContainer));
        }
    }
}

void DraggablePanel::mouseUp(const juce::MouseEvent&)
{
    if (isDragging && panelContainer != nullptr)
    {
        panelContainer->handlePanelDragEnd(this);
    }
    isDragging = false;
}

void DraggablePanel::setContentComponent(juce::Component* content)
{
    contentComponent = content;

    // Cache the natural height at attach time. Reading it back from the
    // component later would be circular, since resized() is what sets its
    // height.
    contentPreferredHeight = 400;
    if (auto* panelContent = dynamic_cast<PanelContent*>(content))
        contentPreferredHeight = juce::jmax(1, panelContent->getPreferredHeight());
    else if (content != nullptr && content->getHeight() > 0)
        contentPreferredHeight = content->getHeight();

    contentViewport.setViewedComponent(content, false);

    if (contentComponent != nullptr)
    {
        contentComponent->setVisible(!collapsed);
        contentViewport.setVisible(!collapsed);
        resized();
    }
}

void DraggablePanel::setCollapsed(bool newCollapsed)
{
    if (collapsed != newCollapsed)
    {
        collapsed = newCollapsed;
        if (contentComponent != nullptr)
        {
            contentComponent->setVisible(!collapsed);
            contentViewport.setVisible(!collapsed);
        }

        if (panelContainer != nullptr)
            panelContainer->updateLayout();

        repaint();
    }
}

void DraggablePanel::refreshContentPreferredHeight()
{
    if (auto* panelContent = dynamic_cast<PanelContent*>(contentComponent))
        contentPreferredHeight = juce::jmax(1, panelContent->getPreferredHeight());

    resized();

    if (panelContainer != nullptr)
        panelContainer->updateLayout();
}

int DraggablePanel::getPreferredContentHeight() const
{
    return contentPreferredHeight;
}

int DraggablePanel::getPreferredHeight() const
{
    if (collapsed)
        return headerHeight;

    return headerHeight + getPreferredContentHeight() + contentMargin * 2;
}

void DraggablePanel::paintContent(juce::Graphics&, juce::Rectangle<int>)
{
    // Override in subclasses for custom content painting
}
