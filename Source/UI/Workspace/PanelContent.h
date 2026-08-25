#pragma once

/**
 * Implemented by components hosted in the side panel that know the height
 * their layout needs.
 *
 * DraggablePanel asks for this height instead of stretching the content to
 * whatever the panel happens to be: when the plugin window is too short for
 * the full stack the content keeps its natural height and the panel scrolls,
 * rather than the bottom cards being squeezed out of the visible area.
 *
 * Content that does not implement this scales with the panel as before.
 */
class PanelContent
{
public:
    virtual ~PanelContent() = default;

    /** Height the content needs to lay out every control at full size. */
    virtual int getPreferredHeight() const = 0;
};
