#include "DarkLookAndFeel.h"

DarkLookAndFeel::DarkLookAndFeel()
    : oldTooltip([]
      {
          const juce::PluginHostType hostType;
          return hostType.isLogic() || hostType.isGarageBand();
      }())
{
    // ── PopupMenu ─────────────────────────────────────────────────
    setColour(juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::PopupMenu::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, APP_COLOR_PRIMARY.withAlpha(0.15f));
    setColour(juce::PopupMenu::highlightedTextColourId, APP_COLOR_TEXT_PRIMARY);

    // ── ComboBox ──────────────────────────────────────────────────
    setColour(juce::ComboBox::backgroundColourId, APP_COLOR_SURFACE);
    setColour(juce::ComboBox::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::ComboBox::outlineColourId, APP_COLOR_BORDER);
    setColour(juce::ComboBox::arrowColourId, APP_COLOR_TEXT_MUTED);
    setColour(juce::ComboBox::focusedOutlineColourId, APP_COLOR_PRIMARY.withAlpha(0.6f));

    // ── Label ─────────────────────────────────────────────────────
    setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    // ── TextButton ────────────────────────────────────────────────
    setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF5B5B5Bu));
    setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF3E3E3Eu));
    setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFEFEFEFu));
    setColour(juce::TextButton::textColourOnId, juce::Colour(0xFFEFEFEFu));

    // ── ListBox ───────────────────────────────────────────────────
    setColour(juce::ListBox::backgroundColourId, APP_COLOR_SURFACE);
    setColour(juce::ListBox::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::ListBox::outlineColourId, APP_COLOR_BORDER_SUBTLE);

    // ── ScrollBar ─────────────────────────────────────────────────
    setColour(juce::ScrollBar::thumbColourId, APP_COLOR_TEXT_MUTED.withAlpha(0.28f));
    setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    // ── TextEditor ────────────────────────────────────────────────
    setColour(juce::TextEditor::backgroundColourId, APP_COLOR_SURFACE);
    setColour(juce::TextEditor::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::TextEditor::outlineColourId, APP_COLOR_BORDER);
    setColour(juce::TextEditor::focusedOutlineColourId, APP_COLOR_PRIMARY.withAlpha(0.6f));
    setColour(juce::CaretComponent::caretColourId, APP_COLOR_PRIMARY);

    // ── AlertWindow / DialogWindow ────────────────────────────────
    setColour(juce::AlertWindow::backgroundColourId, APP_COLOR_SURFACE_RAISED);
    setColour(juce::AlertWindow::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::AlertWindow::outlineColourId, APP_COLOR_BORDER);

    // ── ProgressBar ───────────────────────────────────────────────
    setColour(juce::ProgressBar::backgroundColourId, APP_COLOR_SURFACE);
    setColour(juce::ProgressBar::foregroundColourId, APP_COLOR_PRIMARY);

    // ── TooltipWindow ────────────────────────────────────────────
    setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xFF232323u));
    setColour(juce::TooltipWindow::outlineColourId, juce::Colour(0xFF3C3C3Cu));
    setColour(juce::TooltipWindow::textColourId, juce::Colour(0xFF9B9B9Bu));

    // Keep JUCE's standard tooltip renderer, but give its default text layout
    // the same Montserrat face used by the rest of the application.
    setDefaultSansSerifTypeface(
        juce::Typeface::createSystemTypefaceFor(AppFont::getFont(14.0f)));
}

juce::Rectangle<int> DarkLookAndFeel::getTooltipBounds(const juce::String& tipText,
                                                        juce::Point<int> screenPos,
                                                        juce::Rectangle<int> parentArea)
{
    auto bounds = juce::LookAndFeel_V2::getTooltipBounds(tipText, screenPos, parentArea);
    return bounds.withPosition(screenPos.x - bounds.getWidth() / 2,
                               screenPos.y - bounds.getHeight() - 16);
}

void DarkLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text,
                                  int width, int height)
{
    if (oldTooltip)
        juce::LookAndFeel_V2::drawTooltip(g, text, width, height);
    else
        juce::LookAndFeel_V4::drawTooltip(g, text, width, height);
}

// =====================================================================
// Popup menu — rounded card with subtle shadow, pill-shaped highlights
// =====================================================================

void DarkLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    auto corner = 8.0f;

    // Outer shadow (two layers for soft spread)
    g.setColour(juce::Colour(0x30000000u));
    g.fillRoundedRectangle(bounds.expanded(1.0f), corner + 1.0f);

    // Background fill
    g.setColour(APP_COLOR_SURFACE_RAISED);
    g.fillRoundedRectangle(bounds, corner);

    // Subtle 1px inner highlight along the top edge
    g.setColour(juce::Colour(0x0CFFFFFF));
    g.drawHorizontalLine(1, bounds.getX() + corner, bounds.getRight() - corner);

    // Border
    g.setColour(APP_COLOR_BORDER);
    g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);
}

void DarkLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                        bool isSeparator, bool isActive, bool isHighlighted,
                                        bool isTicked, bool hasSubMenu,
                                        const juce::String& text, const juce::String& shortcutKeyText,
                                        const juce::Drawable* icon, const juce::Colour* textColour)
{
    juce::ignoreUnused(icon, textColour);

    if (isSeparator)
    {
        auto r = area.reduced(12, 0).withHeight(1).withY(area.getCentreY());
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.fillRect(r);
        return;
    }

    auto margin = 6;
    auto rowBounds = area.reduced(margin, 1).toFloat();
    auto corner = 5.0f;

    // Highlight — soft pill-shaped tinted background
    if (isHighlighted && isActive)
    {
        g.setColour(APP_COLOR_PRIMARY.withAlpha(0.14f));
        g.fillRoundedRectangle(rowBounds, corner);
        g.setColour(APP_COLOR_TEXT_PRIMARY);
    }
    else
    {
        g.setColour(isActive ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_TEXT_MUTED.withAlpha(0.5f));
    }

    auto textArea = area.reduced(16, 0);

    g.setFont(getPopupMenuFont());
    g.drawFittedText(text, textArea, juce::Justification::centredLeft, 1);

    // Shortcut key text (right-aligned, muted)
    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour(APP_COLOR_TEXT_MUTED.withAlpha(0.6f));
        g.drawFittedText(shortcutKeyText, textArea, juce::Justification::centredRight, 1);
    }

    // Submenu arrow
    if (hasSubMenu)
    {
        auto arrowH = 7.0f;
        auto arrowX = (float)(area.getRight() - 16);
        auto arrowY = (float)area.getCentreY();

        juce::Path arrow;
        arrow.addTriangle(arrowX, arrowY - arrowH * 0.5f,
                          arrowX, arrowY + arrowH * 0.5f,
                          arrowX + arrowH * 0.5f, arrowY);
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.fillPath(arrow);
    }

    // Tick mark
    if (isTicked)
    {
        auto tickBounds = juce::Rectangle<float>(
            (float)(area.getRight() - area.getHeight()), (float)area.getY(),
            (float)area.getHeight(), (float)area.getHeight()).reduced(7.0f);

        juce::Path tick;
        tick.startNewSubPath(tickBounds.getX(), tickBounds.getCentreY());
        tick.lineTo(tickBounds.getX() + tickBounds.getWidth() * 0.35f, tickBounds.getBottom());
        tick.lineTo(tickBounds.getRight(), tickBounds.getY());

        g.setColour(APP_COLOR_PRIMARY);
        g.strokePath(tick, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }
}

// =====================================================================
// Tick box — smooth toggle with animated-feel states
// =====================================================================

void DarkLookAndFeel::drawTickBox(juce::Graphics& g, juce::Component& component,
                                    float x, float y, float w, float h,
                                    bool ticked, bool isEnabled,
                                    bool shouldDrawButtonAsHighlighted,
                                    bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(component);

    // Modern switch-style toggle
    const float trackH = std::floor(std::min(h, 16.0f));
    const float trackW = trackH * 1.75f;
    const float trackX = x;
    const float trackY = y + (h - trackH) * 0.5f;
    const float trackRadius = trackH * 0.5f;
    juce::ignoreUnused(shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    const float alpha = isEnabled ? 1.0f : 0.4f;

    juce::Rectangle<float> track(trackX, trackY, trackW, trackH);

    if (ticked)
    {
        g.setColour(juce::Colour(0xFF575757u).withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(track, trackRadius);
    }
    else
    {
        g.setColour(juce::Colour(0xFF30302Eu).withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(track, trackRadius);
    }

    // Thumb circle
    const float thumbPad = 2.0f;
    const float thumbD = trackH - thumbPad * 2.0f;
    const float thumbX = ticked ? (trackX + trackW - thumbD - thumbPad) : (trackX + thumbPad);
    const float thumbY = trackY + thumbPad;

    g.setColour((ticked ? juce::Colours::white : juce::Colour(0xFF575757u))
                    .withMultipliedAlpha(alpha));
    g.fillEllipse(thumbX, thumbY, thumbD, thumbD);
}

// =====================================================================
// Toggle button — accounts for wider switch-style indicator
// =====================================================================

void DarkLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                        bool shouldDrawButtonAsHighlighted,
                                        bool shouldDrawButtonAsDown)
{
    const float h = static_cast<float>(button.getHeight());
    const float switchH = std::floor(std::min(h * 0.75f, 16.0f));
    const float switchW = switchH * 1.75f;
    const float switchX = 4.0f;
    const float switchY = (h - switchH) * 0.5f;

    drawTickBox(g, button, switchX, switchY, switchH, switchH,
                button.getToggleState(), button.isEnabled(),
                shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

    // Text after the switch, with proper gap
    const float textX = switchX + switchW + 6.0f;
    g.setColour(button.findColour(juce::ToggleButton::textColourId)
                    .withAlpha(button.isEnabled() ? 1.0f : 0.4f));
    g.setFont(AppFont::getFont(14.0f));
    g.drawText(button.getButtonText(),
               juce::Rectangle<float>(textX, 0.0f,
                   static_cast<float>(button.getWidth()) - textX, h),
               juce::Justification::centredLeft, true);
}

// =====================================================================
// Progress bar — rounded pill with gradient fill and soft glow
// =====================================================================

void DarkLookAndFeel::drawProgressBar(juce::Graphics& g, juce::ProgressBar& bar,
                                       int width, int height, double progress,
                                       const juce::String& textToShow)
{
    juce::ignoreUnused(textToShow);

    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    auto corner = (float)height * 0.5f; // fully rounded pill

    // Track background
    g.setColour(bar.findColour(juce::ProgressBar::backgroundColourId));
    g.fillRoundedRectangle(bounds, corner);

    // Fill
    if (progress >= 0.0 && progress <= 1.0)
    {
        auto fillW = juce::jmax(bounds.getHeight(), bounds.getWidth() * (float)progress);
        auto fillBounds = bounds.withWidth(fillW);

        g.setColour(bar.findColour(juce::ProgressBar::foregroundColourId));
        g.fillRoundedRectangle(fillBounds, corner);
    }
    else
    {
        // Indeterminate animation
        auto time = juce::Time::getMillisecondCounter();
        auto pos = (float)(time % 1200) / 1200.0f;
        auto barW = bounds.getWidth() * 0.3f;
        auto x0 = bounds.getX() + (bounds.getWidth() + barW) * pos - barW;

        g.setColour(bar.findColour(juce::ProgressBar::foregroundColourId));
        g.saveState();
        g.reduceClipRegion(bounds.toNearestInt());
        g.fillRoundedRectangle(x0, bounds.getY(), barW, bounds.getHeight(), corner);
        g.restoreState();
    }
}

// =====================================================================
// TextButton — VocalNet-style rounded grey buttons
// =====================================================================

void DarkLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                            const juce::Colour& backgroundColour,
                                            bool shouldDrawButtonAsHighlighted,
                                            bool shouldDrawButtonAsDown)
{
    auto cornerSize = button.getHeight() < 20 ? 6.0f : 13.0f;
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f, 0.5f);

    auto baseColour = backgroundColour
                          .withMultipliedSaturation(button.hasKeyboardFocus(true) ? 1.3f : 0.9f)
                          .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f);

    if (baseColour.getAlpha() > 0 && (shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted))
        baseColour = baseColour.contrasting(shouldDrawButtonAsDown ? 0.2f : 0.05f);

    g.setColour(shouldDrawButtonAsDown
                    ? findColour(juce::TextButton::buttonOnColourId)
                    : baseColour);

    const auto flatOnLeft = button.isConnectedOnLeft();
    const auto flatOnRight = button.isConnectedOnRight();
    const auto flatOnTop = button.isConnectedOnTop();
    const auto flatOnBottom = button.isConnectedOnBottom();

    if (flatOnLeft || flatOnRight || flatOnTop || flatOnBottom)
    {
        juce::Path path;
        path.addRoundedRectangle(bounds.getX(), bounds.getY(),
                                 bounds.getWidth(), bounds.getHeight(),
                                 cornerSize, cornerSize,
                                 !(flatOnLeft || flatOnTop),
                                 !(flatOnRight || flatOnTop),
                                 !(flatOnLeft || flatOnBottom),
                                 !(flatOnRight || flatOnBottom));

        g.fillPath(path);

        g.setColour(button.findColour(juce::ComboBox::outlineColourId));
        g.strokePath(path, juce::PathStrokeType(1.0f));
    }
    else
    {
        g.fillRoundedRectangle(bounds, cornerSize);
    }
}

void DarkLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                      bool shouldDrawButtonAsHighlighted,
                                      bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(shouldDrawButtonAsDown);

    auto font = getTextButtonFont(button, button.getHeight());
    g.setFont(font);
    g.setColour(button.findColour(button.getToggleState() || shouldDrawButtonAsHighlighted
                                      ? juce::TextButton::textColourOnId
                                      : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));

    const int yIndent = juce::jmin(4, button.proportionOfHeight(0.3f));
    const int cornerSize = juce::jmin(button.getHeight(), button.getWidth()) / 2;
    const int fontHeight = juce::roundToInt(font.getHeight() * 0.6f);
    const int leftIndent =
        juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent =
        juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
    const int textWidth = button.getWidth() - leftIndent - rightIndent;

    if (textWidth > 0)
        g.drawFittedText(button.getButtonText(),
                         leftIndent, yIndent,
                         textWidth, button.getHeight() - yIndent * 2,
                         juce::Justification::centred, 2);
}

// =====================================================================
// TextEditor — rounded background and outline
// =====================================================================

void DarkLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                                juce::TextEditor& editor)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    auto corner = 6.0f;

    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(bounds, corner);
}

void DarkLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                             juce::TextEditor& editor)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    auto corner = 6.0f;

    if (editor.isEnabled())
    {
        auto outlineColour = (editor.hasKeyboardFocus(true) && !editor.isReadOnly())
                                 ? editor.findColour(juce::TextEditor::focusedOutlineColourId)
                                 : editor.findColour(juce::TextEditor::outlineColourId);
        g.setColour(outlineColour);
        g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);
    }
}

// =====================================================================
// ComboBox — rounded with subtle arrow indicator
// =====================================================================

void DarkLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                    int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                                    juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    auto corner = 6.0f;

    auto bg = box.findColour(juce::ComboBox::backgroundColourId);
    if (isButtonDown)
        bg = bg.brighter(0.06f);

    g.setColour(bg);
    g.fillRoundedRectangle(bounds, corner);

    auto outlineColour = box.hasKeyboardFocus(true)
                            ? box.findColour(juce::ComboBox::focusedOutlineColourId)
                            : box.findColour(juce::ComboBox::outlineColourId);
    g.setColour(outlineColour);
    g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);

    // Chevron arrow
    auto arrowZone = juce::Rectangle<float>((float)width - 22.0f, 0.0f, 16.0f, (float)height);
    juce::Path arrow;
    auto cy = arrowZone.getCentreY();
    auto cx = arrowZone.getCentreX();
    arrow.startNewSubPath(cx - 4.0f, cy - 2.0f);
    arrow.lineTo(cx, cy + 2.0f);
    arrow.lineTo(cx + 4.0f, cy - 2.0f);

    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.strokePath(arrow, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
}

// =====================================================================
// ScrollBar — thin rounded thumb over a filled track
// =====================================================================

void DarkLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                                     int x, int y, int width, int height,
                                     bool isScrollbarVertical, int thumbStartPosition,
                                     int thumbSize, bool isMouseOver, bool isMouseDown)
{
    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    g.setColour(scrollbar.findColour(juce::ScrollBar::backgroundColourId));
    g.fillRect(bounds);

    auto alpha = (isMouseDown || isMouseOver) ? 1.0f : 0.86f;
    g.setColour(scrollbar.findColour(juce::ScrollBar::thumbColourId).withAlpha(alpha));

    juce::Rectangle<float> thumbBounds;

    if (isScrollbarVertical)
    {
        auto thumbW = static_cast<float>(width);
        thumbBounds = { static_cast<float>(x), static_cast<float>(thumbStartPosition),
                        thumbW, (float)juce::jmax(thumbSize, 16) };
    }
    else
    {
        auto thumbH = static_cast<float>(height);
        thumbBounds = { (float)thumbStartPosition, static_cast<float>(y),
                        (float)juce::jmax(thumbSize, 16), thumbH };
    }

    g.fillRect(thumbBounds);
}
