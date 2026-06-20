#include "PanelContainer.h"

PanelContainer::PanelContainer()
{
    setOpaque(true);
}

void PanelContainer::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF232323u));
}

void PanelContainer::resized()
{
    updateLayout();
}

void PanelContainer::addPanel(std::unique_ptr<DraggablePanel> panel)
{
    auto id = panel->getPanelId();
    panel->setPanelContainer(this);
    addChildComponent(panel.get());

    panels[id] = std::move(panel);
    panelOrder.push_back(id);
}

void PanelContainer::removePanel(const juce::String& panelId)
{
    auto it = panels.find(panelId);
    if (it != panels.end())
    {
        removeChildComponent(it->second.get());
        panels.erase(it);

        panelOrder.erase(std::remove(panelOrder.begin(), panelOrder.end(), panelId), panelOrder.end());
        visiblePanels.erase(panelId);

        updateLayout();
    }
}

void PanelContainer::showPanel(const juce::String& panelId, bool show)
{
    auto it = panels.find(panelId);
    if (it == panels.end())
        return;

    if (show)
    {
        visiblePanels.insert(panelId);
        it->second->setVisible(true);
    }
    else
    {
        visiblePanels.erase(panelId);
        it->second->setVisible(false);
    }

    updateLayout();
}

bool PanelContainer::isPanelVisible(const juce::String& panelId) const
{
    return visiblePanels.find(panelId) != visiblePanels.end();
}

DraggablePanel* PanelContainer::getPanel(const juce::String& panelId)
{
    auto it = panels.find(panelId);
    return it != panels.end() ? it->second.get() : nullptr;
}

void PanelContainer::updateLayout()
{
    int width = getWidth();
    int height = getHeight();

    // Single panel takes full size
    for (const auto& id : panelOrder)
    {
        if (visiblePanels.find(id) == visiblePanels.end())
            continue;

        auto* panel = panels[id].get();
        if (panel == nullptr)
            continue;

        panel->setBounds(0, 0, width, height);
        break;
    }
}

void PanelContainer::handlePanelDrag(DraggablePanel* panel, const juce::MouseEvent& e)
{
    if (draggedPanel == nullptr)
    {
        draggedPanel = panel;
        dragStartY = panel->getY();
    }

    // Find insert position
    int newIndex = findPanelIndexAt(e.y);
    if (newIndex != dragInsertIndex)
    {
        dragInsertIndex = newIndex;
        repaint();
    }
}

void PanelContainer::handlePanelDragEnd(DraggablePanel* panel)
{
    if (draggedPanel != panel || dragInsertIndex < 0)
    {
        draggedPanel = nullptr;
        dragInsertIndex = -1;
        return;
    }

    // Find current index
    auto it = std::find(panelOrder.begin(), panelOrder.end(), panel->getPanelId());
    if (it != panelOrder.end())
    {
        int currentIndex = static_cast<int>(std::distance(panelOrder.begin(), it));

        if (currentIndex != dragInsertIndex)
        {
            // Remove from current position
            panelOrder.erase(it);

            // Adjust insert index if needed
            if (dragInsertIndex > currentIndex)
                dragInsertIndex--;

            // Insert at new position
            panelOrder.insert(panelOrder.begin() + dragInsertIndex, panel->getPanelId());

            if (onPanelOrderChanged)
                onPanelOrderChanged(panelOrder);
        }
    }

    draggedPanel = nullptr;
    dragInsertIndex = -1;
    updateLayout();
}

int PanelContainer::findPanelIndexAt(int y) const
{
    int index = 0;
    int currentY = 8;

    for (const auto& id : panelOrder)
    {
        if (visiblePanels.find(id) == visiblePanels.end())
            continue;

        auto it = panels.find(id);
        if (it == panels.end())
            continue;

        int height = it->second->getPreferredHeight();
        int midY = currentY + height / 2;

        if (y < midY)
            return index;

        currentY += height + 8;
        index++;
    }

    return index;
}

void PanelContainer::reorderPanels()
{
    updateLayout();
}
