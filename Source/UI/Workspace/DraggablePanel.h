#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"
#include "PanelContent.h"

class PanelContainer;

/**
 * Base wrapper for content hosted in the side-panel container.
 */
class DraggablePanel : public juce::Component
{
public:
    DraggablePanel(const juce::String& panelId, const juce::String& title);
    ~DraggablePanel() override = default;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    const juce::String& getPanelId() const { return panelId; }
    const juce::String& getTitle() const { return title; }

    void setContentComponent(juce::Component* content);
    juce::Component* getContentComponent() const { return contentComponent; }

    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return collapsed; }

    void setPanelContainer(PanelContainer* container) { panelContainer = container; }

    int getPreferredHeight() const;
    /**
     * Re-read the content's natural height. The height is cached at attach
     * time (reading it back later would be circular), so content whose layout
     * grows or shrinks - a card appearing - has to say so.
     */
    void refreshContentPreferredHeight();
    static constexpr int headerHeight = 0;
    static constexpr int contentMargin = 10;

    // The scrollbar sits in the panel's right margin instead of taking width
    // from the content, so the cards keep their full width whether or not it
    // is showing. Whatever the bar does not use stays as an edge inset.
    static_assert(contentMargin >= APP_SCROLLBAR_THICKNESS,
                  "the right margin must be wide enough to hold the scrollbar");
    static constexpr int scrollBarEdgeInset = contentMargin - APP_SCROLLBAR_THICKNESS;

protected:
    virtual void paintContent(juce::Graphics& g, juce::Rectangle<int> contentArea);

private:
    int getPreferredContentHeight() const;

    juce::String panelId;
    juce::String title;

    // The content lives inside the viewport, so a panel shorter than the
    // content scrolls instead of clipping it.
    juce::Viewport contentViewport;
    juce::Component* contentComponent = nullptr;
    int contentPreferredHeight = 400;
    PanelContainer* panelContainer = nullptr;
    bool collapsed = false;
    bool isDragging = false;
    juce::Point<int> dragStartPos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DraggablePanel)
};
