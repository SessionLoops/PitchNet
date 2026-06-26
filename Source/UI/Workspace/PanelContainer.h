#pragma once

#include "../../JuceHeader.h"
#include "DraggablePanel.h"
#include <vector>
#include <memory>

/**
 * Container that manages multiple draggable panels.
 * Panels can be reordered by dragging their headers.
 */
class PanelContainer : public juce::Component
{
public:
    PanelContainer();
    ~PanelContainer() override = default;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

    void addPanel(std::unique_ptr<DraggablePanel> panel);
    void removePanel(const juce::String& panelId);
    void showPanel(const juce::String& panelId, bool show);
    bool isPanelVisible(const juce::String& panelId) const;

    DraggablePanel* getPanel(const juce::String& panelId);
    const std::vector<juce::String>& getPanelOrder() const { return panelOrder; }

    void updateLayout();
    void handlePanelDrag(DraggablePanel* panel, const juce::MouseEvent& e);
    void handlePanelDragEnd(DraggablePanel* panel);

    std::function<void(const std::vector<juce::String>&)> onPanelOrderChanged;

private:
    void reorderPanels();
    int findPanelIndexAt(int y) const;

    std::map<juce::String, std::unique_ptr<DraggablePanel>> panels;
    std::vector<juce::String> panelOrder;
    std::set<juce::String> visiblePanels;

    // Drag state
    DraggablePanel* draggedPanel = nullptr;
    int dragInsertIndex = -1;
    int dragStartY = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanelContainer)
};
