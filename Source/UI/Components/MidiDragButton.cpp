#include "MidiDragButton.h"
#include "AppFont.h"

namespace
{
// How far the mouse has to travel before a press becomes a drag.
constexpr int kDragThresholdPx = 4;

void paintMidiIcon(juce::Graphics& g, juce::Rectangle<float> area, float alpha)
{
    // A tiny piano-roll: three staggered note bars.
    g.setColour(juce::Colour(0xFF9B9B9Bu).withMultipliedAlpha(alpha));
    const float barH = 2.5f;
    const float rowStep = (area.getHeight() - barH) / 2.0f;
    const float w = area.getWidth();

    g.fillRoundedRectangle(area.getX(), area.getY(), w * 0.45f, barH, 1.0f);
    g.fillRoundedRectangle(area.getX() + w * 0.30f, area.getY() + rowStep,
                           w * 0.50f, barH, 1.0f);
    g.fillRoundedRectangle(area.getX() + w * 0.55f, area.getY() + rowStep * 2.0f,
                           w * 0.45f, barH, 1.0f);
}

void paintMidiButton(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& text,
                     bool enabled, bool highlighted, bool down, bool showIcon)
{
    // Same look as the panel's other buttons (ComboSelectionButton): a flat
    // fill that brightens on hover and press, no border.
    auto colour = juce::Colour(0xFF30302Eu);
    if (enabled && down)
        colour = colour.brighter(0.10f);
    else if (enabled && highlighted)
        colour = colour.brighter(0.05f);

    g.setColour(colour);
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);

    const float alpha = enabled ? 1.0f : 0.5f;
    auto content = bounds.reduced(9, 0);
    if (showIcon)
    {
        const auto icon = content.removeFromLeft(16).toFloat().withSizeKeepingCentre(16.0f, 11.0f);
        paintMidiIcon(g, icon, alpha);
        content.removeFromLeft(8);
    }

    g.setColour(juce::Colour(0xFFE6E6E6u).withMultipliedAlpha(alpha));
    g.setFont(AppFont::getFont(13.0f));
    g.drawFittedText(text, content, juce::Justification::centredLeft, 1);
}
}

MidiFileButton::MidiFileButton()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void MidiFileButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    paintMidiButton(g, getLocalBounds(), getButtonText(), isEnabled(),
                    highlighted || hasKeyboardFocus(true), down, false);
}

MidiDragButton::MidiDragButton()
{
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    setRepaintsOnMouseActivity(false);
}

void MidiDragButton::setButtonText(const juce::String& text)
{
    if (buttonText == text)
        return;

    buttonText = text;
    repaint();
}

void MidiDragButton::paint(juce::Graphics& g)
{
    paintMidiButton(g, getLocalBounds(), buttonText, isEnabled(), hovered, pressed, true);
}

void MidiDragButton::enablementChanged()
{
    setMouseCursor(isEnabled() ? juce::MouseCursor::DraggingHandCursor
                               : juce::MouseCursor::NormalCursor);
    hovered = false;
    pressed = false;
    dragStarted = false;
    repaint();
}

void MidiDragButton::mouseEnter(const juce::MouseEvent&)
{
    hovered = true;
    repaint();
}

void MidiDragButton::mouseExit(const juce::MouseEvent&)
{
    hovered = false;
    repaint();
}

void MidiDragButton::mouseDown(const juce::MouseEvent&)
{
    if (!isEnabled())
        return;

    pressed = true;
    dragStarted = false;
    repaint();
}

void MidiDragButton::mouseDrag(const juce::MouseEvent& e)
{
    if (!isEnabled() || !pressed || dragStarted || e.getDistanceFromDragStart() < kDragThresholdPx)
        return;

    dragStarted = true;

    if (!onCreateDragFile)
        return;

    const auto file = onCreateDragFile();
    if (!file.existsAsFile())
        return;

    // The receiving app copies or imports the file, so it must stay where it
    // is (canMoveFiles = false). It lives in the temp folder and is simply
    // overwritten by the next drag.
    juce::Component::SafePointer<MidiDragButton> safeThis(this);
    juce::DragAndDropContainer::performExternalDragDropOfFiles(
        { file.getFullPathName() }, false, this,
        [safeThis]
        {
            if (safeThis == nullptr)
                return;
            safeThis->pressed = false;
            safeThis->repaint();
        });
}

void MidiDragButton::mouseUp(const juce::MouseEvent& e)
{
    const bool wasClick = isEnabled() && pressed && !dragStarted &&
                          getLocalBounds().contains(e.getPosition());
    pressed = false;
    dragStarted = false;
    repaint();

    if (wasClick && onClick)
        onClick();
}
