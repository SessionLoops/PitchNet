#include "ToolbarComponent.h"
#include "PianoRollComponent.h" // For EditMode enum
#include "StyledComponents.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/SvgUtils.h"
#include "../Utils/UI/TimecodeFont.h"
#include "BinaryData.h"

ToolbarComponent::ToolbarComponent()
{
    // Load SVG icons with white tint
    auto playIcon = SvgUtils::loadSvg(BinaryData::playline_svg, BinaryData::playline_svgSize, juce::Colours::white);
    auto pauseIcon = SvgUtils::loadSvg(BinaryData::pauseline_svg, BinaryData::pauseline_svgSize, juce::Colours::white);
    auto stopIcon = SvgUtils::loadSvg(BinaryData::stopline_svg, BinaryData::stopline_svgSize, juce::Colours::white);
    auto startIcon = SvgUtils::loadSvg(BinaryData::movestartline_svg, BinaryData::movestartline_svgSize, juce::Colours::white);
    auto endIcon = SvgUtils::loadSvg(BinaryData::moveendline_svg, BinaryData::moveendline_svgSize, juce::Colours::white);
    auto cursorIcon = SvgUtils::loadSvg(BinaryData::cursor_24_filled_svg, BinaryData::cursor_24_filled_svgSize, juce::Colours::white);
#if PITCHNET_ENABLE_STRETCH
    auto stretchIcon = SvgUtils::loadSvg(BinaryData::stretch_24_filled_svg, BinaryData::stretch_24_filled_svgSize, juce::Colours::white);
#endif
    auto pitchEditIcon = SvgUtils::loadSvg(BinaryData::pitch_edit_24_filled_svg, BinaryData::pitch_edit_24_filled_svgSize, juce::Colours::white);
    auto scissorsIcon = SvgUtils::loadSvg(BinaryData::scissors_24_filled_svg, BinaryData::scissors_24_filled_svgSize, juce::Colours::white);
    // HNSep mode icon: three horizontal bars representing voicing/breath/tension curves
    const juce::String hnsepIconSvg =
        R"(<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><path d="M3 6c0-.6.4-1 1-1h2c.6 0 1 .4 1 1s-.4 1-1 1H4c-.6 0-1-.4-1-1z" opacity="0.5"/><path d="M9 6c1.5-1.5 3-1.5 4.5 0s3 1.5 4.5 0" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round"/><path d="M3 12c0-.6.4-1 1-1h2c.6 0 1 .4 1 1s-.4 1-1 1H4c-.6 0-1-.4-1-1z" opacity="0.5"/><path d="M9 12c1 2 3 2 4.5.5s3-.5 4.5 1" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round"/><path d="M3 18c0-.6.4-1 1-1h2c.6 0 1 .4 1 1s-.4 1-1 1H4c-.6 0-1-.4-1-1z" opacity="0.5"/><path d="M9 18h9" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-dasharray="3 2"/></svg>)";
    auto hnsepIcon = SvgUtils::createDrawableFromSvg(hnsepIconSvg, juce::Colours::white);
    auto followIcon = SvgUtils::loadSvg(BinaryData::follow24filled_svg, BinaryData::follow24filled_svgSize, juce::Colours::white);
    auto loopIcon = SvgUtils::loadSvg(BinaryData::loop24filled_svg, BinaryData::loop24filled_svgSize, juce::Colours::white);
    const juce::String parametersIconSvg =
        R"(<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><rect x="3" y="2" width="2" height="20" rx="1"/><circle cx="4" cy="9" r="3"/><rect x="11" y="2" width="2" height="20" rx="1"/><circle cx="12" cy="15" r="3"/><rect x="19" y="2" width="2" height="20" rx="1"/><circle cx="20" cy="6" r="3"/></svg>)";
    auto parametersIcon = SvgUtils::createDrawableFromSvg(parametersIconSvg, juce::Colours::white);

#if PITCHNET_ENABLE_STRETCH
    // Absorb mode icon: two opposing arrows (←|→) representing zero-sum stretch
    const juce::String absorbIconSvg =
        R"(<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><path d="M3 12l5-4v3h3v2H8v3L3 12z"/><path d="M21 12l-5-4v3h-3v2h3v3l5-4z"/><rect x="11.25" y="5" width="1.5" height="14" rx="0.75" opacity="0.5"/></svg>)";
    auto absorbIcon = SvgUtils::createDrawableFromSvg(absorbIconSvg, juce::Colours::white);

    // Ripple mode icon: arrow pushing blocks rightward (→|→→)
    const juce::String rippleIconSvg =
        R"(<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><path d="M2 12l5-4v3h4v2H7v3L2 12z"/><rect x="12" y="6" width="3" height="12" rx="1"/><rect x="16.5" y="7" width="2.5" height="10" rx="0.75" opacity="0.6"/><rect x="20" y="8" width="2" height="8" rx="0.75" opacity="0.35"/></svg>)";
    auto rippleIcon = SvgUtils::createDrawableFromSvg(rippleIconSvg, juce::Colours::white);
#endif

    playButton.setImages(playIcon.get());
    stopButton.setImages(stopIcon.get());
    goToStartButton.setImages(startIcon.get());
    goToEndButton.setImages(endIcon.get());
    selectModeButton.setImages(cursorIcon.get());
#if PITCHNET_ENABLE_STRETCH
    stretchModeButton.setImages(stretchIcon.get());
#endif
    drawModeButton.setImages(pitchEditIcon.get());
    splitModeButton.setImages(scissorsIcon.get());
    hnsepModeButton.setImages(hnsepIcon.get());
    followButton.setImages(followIcon.get());
    loopButton.setImages(loopIcon.get());
    parametersButton.setImages(parametersIcon.get());
#if PITCHNET_ENABLE_STRETCH
    rippleToggleButton.setImages(absorbIcon.get()); // Default: Absorb mode
#endif

    // Set edge indent for icon padding (makes icons smaller within button bounds)
    goToStartButton.setEdgeIndent(4);
    playButton.setEdgeIndent(6);
    stopButton.setEdgeIndent(6);
    goToEndButton.setEdgeIndent(4);
    selectModeButton.setEdgeIndent(6);
#if PITCHNET_ENABLE_STRETCH
    stretchModeButton.setEdgeIndent(6);
#endif
    drawModeButton.setEdgeIndent(6);
    splitModeButton.setEdgeIndent(6);
    hnsepModeButton.setEdgeIndent(6);
    followButton.setEdgeIndent(6);
    loopButton.setEdgeIndent(6);
    parametersButton.setEdgeIndent(6);
#if PITCHNET_ENABLE_STRETCH
    rippleToggleButton.setEdgeIndent(6);
#endif

    // Store pause icon for later use
    pauseDrawable = std::move(pauseIcon);
    playDrawable = SvgUtils::loadSvg(BinaryData::playline_svg, BinaryData::playline_svgSize, juce::Colours::white);
#if PITCHNET_ENABLE_STRETCH
    absorbDrawable = std::move(absorbIcon);
    rippleDrawable = std::move(rippleIcon);
#endif

    // Configure buttons
    addAndMakeVisible(goToStartButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(goToEndButton);
    addAndMakeVisible(selectModeButton);
#if PITCHNET_ENABLE_STRETCH
    addAndMakeVisible(stretchModeButton);
#endif
    addAndMakeVisible(drawModeButton);
    addAndMakeVisible(splitModeButton);
    addAndMakeVisible(hnsepModeButton);
    addAndMakeVisible(followButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(parametersButton);
#if PITCHNET_ENABLE_STRETCH
    addChildComponent(rippleToggleButton); // Hidden by default, shown in Stretch mode
#endif

    // Plugin mode buttons (hidden by default)
    addChildComponent(reanalyzeButton);
    addChildComponent(araModeLabel);

    // ARA mode label style (background drawn in paint() for rounded corners)
    araModeLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    araModeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    araModeLabel.setJustificationType(juce::Justification::centred);
    araModeLabel.setFont(AppFont::getBoldFont(11.0f));

    goToStartButton.addListener(this);
    playButton.addListener(this);
    stopButton.addListener(this);
    goToEndButton.addListener(this);
    selectModeButton.addListener(this);
#if PITCHNET_ENABLE_STRETCH
    stretchModeButton.addListener(this);
#endif
    drawModeButton.addListener(this);
    splitModeButton.addListener(this);
    hnsepModeButton.addListener(this);
    followButton.addListener(this);
    loopButton.addListener(this);
    parametersButton.addListener(this);
    reanalyzeButton.addListener(this);
#if PITCHNET_ENABLE_STRETCH
    rippleToggleButton.addListener(this);
#endif

    // Set localized text (tooltips for icon buttons)
    selectModeButton.setTooltip(TR("toolbar.select"));
#if PITCHNET_ENABLE_STRETCH
    stretchModeButton.setTooltip(TR("toolbar.stretch"));
#endif
    drawModeButton.setTooltip(TR("toolbar.draw"));
    splitModeButton.setTooltip(TR("toolbar.split"));
    hnsepModeButton.setTooltip(TR("toolbar.hnsep"));
    followButton.setTooltip(TR("toolbar.follow"));
    loopButton.setTooltip(TR("toolbar.loop"));
    parametersButton.setTooltip(TR("panel.parameters"));
#if PITCHNET_ENABLE_STRETCH
    rippleToggleButton.setTooltip(TR("toolbar.stretch_absorb"));
#endif
    reanalyzeButton.setButtonText(TR("toolbar.reanalyze"));
    zoomLabel.setText(TR("toolbar.zoom"), juce::dontSendNotification);

    // Style reanalyze button — transparent background (custom painted in paint()), bold white text
    reanalyzeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    reanalyzeButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    reanalyzeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    reanalyzeButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);

    // Set default active states
    selectModeButton.setActive(true);
    followButton.setActive(true); // Follow is on by default
    loopButton.setActive(false);
    parametersButton.setActive(false);

    // Time label with app font (larger and bold for readability)
    addAndMakeVisible(timeLabel);
    timeLabel.setText("00:00.000 / 00:00.000", juce::dontSendNotification);
    timeLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    timeLabel.setJustificationType(juce::Justification::centred);
    timeLabel.setFont(TimecodeFont::getBoldFont(21.0f).withHorizontalScale(0.92f));

    // Zoom slider
    addAndMakeVisible(zoomLabel);
    addAndMakeVisible(zoomSlider);

    zoomLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);

    zoomSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 1.0);
    zoomSlider.setValue(100.0);
    zoomSlider.setSkewFactorFromMidPoint(200.0);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.addListener(this);

    zoomSlider.setColour(juce::Slider::backgroundColourId, APP_COLOR_SURFACE_ALT);
    zoomSlider.setColour(juce::Slider::trackColourId, APP_COLOR_PRIMARY.withAlpha(0.75f));
    zoomSlider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);

    // Progress bar (hidden by default)
    addChildComponent(progressBar);
    addChildComponent(progressLabel);

    progressLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    progressLabel.setJustificationType(juce::Justification::centredLeft);
    progressBar.setColour(juce::ProgressBar::foregroundColourId, APP_COLOR_PRIMARY);
    progressBar.setColour(juce::ProgressBar::backgroundColourId, APP_COLOR_SURFACE_ALT);
    progressBar.setLookAndFeel(&DarkLookAndFeel::getInstance());

    // Status label (hidden by default)
    addChildComponent(statusLabel);
    statusLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
}

ToolbarComponent::~ToolbarComponent()
{
    progressBar.setLookAndFeel(nullptr);
}

void ToolbarComponent::paint(juce::Graphics &g)
{
    auto bounds = getLocalBounds().toFloat();

    // Flat surface background
    g.setColour(APP_COLOR_SURFACE);
    g.fillRect(bounds);

    // Bottom separator line (1 px)
    g.setColour(APP_COLOR_BORDER_SUBTLE);
    g.fillRect(bounds.removeFromBottom(1.0f));

    // Transport capsule background (standalone) or ARA/reanalyze area (plugin)
    if (!transportCapsuleBounds.isEmpty())
    {
        auto capsule = transportCapsuleBounds.toFloat();
        g.setColour(APP_COLOR_BACKGROUND.withAlpha(0.7f));
        g.fillRoundedRectangle(capsule, 8.0f);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawRoundedRectangle(capsule.reduced(0.5f), 8.0f, 0.75f);
    }

    // Tool buttons container — subtle inset pill
    if (!toolContainerBounds.isEmpty())
    {
        auto toolBounds = toolContainerBounds.toFloat();
        g.setColour(APP_COLOR_BACKGROUND.withAlpha(0.7f));
        g.fillRoundedRectangle(toolBounds, 8.0f);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawRoundedRectangle(toolBounds.reduced(0.5f), 8.0f, 0.75f);

        // Draw a subtle divider between edit tools and playback tools (standalone only)
        if (!pluginMode)
        {
            // Divider after the last edit tool button (hnsep), before follow (or ripple toggle)
            int dividerAfterEditTools = hnsepModeButton.getRight() + 1;
#if PITCHNET_ENABLE_STRETCH
            if (rippleToggleButton.isVisible())
                dividerAfterEditTools = rippleToggleButton.getRight() + 1;
#endif
            if (followButton.isVisible() && dividerAfterEditTools > toolBounds.getX() && dividerAfterEditTools < toolBounds.getRight())
            {
                g.setColour(APP_COLOR_BORDER.withAlpha(0.35f));
                float divY = toolBounds.getY() + 6.0f;
                float divH = toolBounds.getHeight() - 12.0f;
                g.fillRect(juce::Rectangle<float>((float)dividerAfterEditTools, divY, 1.0f, divH));
            }
        }

#if PITCHNET_ENABLE_STRETCH
        // Draw divider between edit tools and ripple toggle (when visible)
        if (rippleToggleButton.isVisible())
        {
            int dividerX = hnsepModeButton.getRight() + 1;
            if (dividerX > toolBounds.getX() && dividerX < toolBounds.getRight())
            {
                g.setColour(APP_COLOR_BORDER.withAlpha(0.35f));
                float divY = toolBounds.getY() + 6.0f;
                float divH = toolBounds.getHeight() - 12.0f;
                g.fillRect(juce::Rectangle<float>((float)dividerX, divY, 1.0f, divH));
            }
        }
#endif
    }

    // Time display — centered inset card
    if (!timeCapsuleBounds.isEmpty() && timeLabel.isVisible())
    {
        auto timeBounds = timeCapsuleBounds.toFloat();
        g.setColour(APP_COLOR_BACKGROUND.withAlpha(0.7f));
        g.fillRoundedRectangle(timeBounds, 8.0f);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawRoundedRectangle(timeBounds.reduced(0.5f), 8.0f, 0.75f);
    }

    // ARA mode badge (plugin mode)
    if (pluginMode && araModeLabel.isVisible())
    {
        auto araBounds = araModeLabel.getBounds().toFloat();
        auto badgeColour = araMode ? APP_COLOR_PRIMARY : APP_COLOR_SURFACE_RAISED;
        g.setColour(badgeColour.withAlpha(0.85f));
        g.fillRoundedRectangle(araBounds, 6.0f);
        if (!araMode)
        {
            g.setColour(APP_COLOR_BORDER.withAlpha(0.5f));
            g.drawRoundedRectangle(araBounds.reduced(0.5f), 6.0f, 0.75f);
        }
    }

    // Reanalyze button custom background (plugin mode — draw as a prominent action button)
    if (pluginMode && reanalyzeButton.isVisible())
    {
        auto rBounds = reanalyzeButton.getBounds().toFloat();
        bool isHover = reanalyzeButton.isMouseOver();
        bool isDown = reanalyzeButton.isMouseButtonDown();
        auto baseColour = APP_COLOR_SECONDARY;
        if (isDown)
            baseColour = baseColour.darker(0.15f);
        else if (isHover)
            baseColour = baseColour.brighter(0.08f);
        g.setColour(baseColour.withAlpha(0.9f));
        g.fillRoundedRectangle(rBounds, 7.0f);
    }
}

void ToolbarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10, 0);
    const int contentH = bounds.getHeight() - 2; // leave 1px top/bottom margin + 1px for separator
    const int yOffset = 1;
    const int capsuleH = contentH - 10; // capsule inner height with vertical padding
    const int capsuleY = yOffset + (contentH - capsuleH) / 2;

    // =========================================================================
    // RIGHT SIDE — Parameters button + status/progress
    // =========================================================================
    const int rightButtonSize = 30;
    auto rightSection = bounds.removeFromRight(250);

    // Parameters button — rightmost
    auto paramBtnArea = rightSection.removeFromRight(rightButtonSize + 6);
    parametersButton.setBounds(
        paramBtnArea.getRight() - rightButtonSize,
        capsuleY + (capsuleH - rightButtonSize) / 2,
        rightButtonSize, rightButtonSize);

    // Status / Progress in remaining right area
    if (showingStatus && !showingProgress)
    {
        statusLabel.setBounds(rightSection.getX(), capsuleY, 140, capsuleH);
    }
    if (showingProgress)
    {
        auto progressArea = rightSection.withWidth(std::min(200, rightSection.getWidth()));
        int pH = capsuleH / 2;
        progressLabel.setBounds(progressArea.getX(), capsuleY, progressArea.getWidth(), capsuleH - pH);
        progressBar.setBounds(progressArea.getX(), capsuleY + capsuleH - pH, progressArea.getWidth(), pH);
    }

    // Hide zoom controls (moved to workspace overlays)
    zoomLabel.setVisible(false);
    zoomSlider.setVisible(false);

    // =========================================================================
    // LEFT SIDE — Transport capsule (standalone) or ARA+Reanalyze (plugin)
    // =========================================================================
    int leftSectionWidth = 0;

    if (pluginMode)
    {
        // ARA badge
        const int araW = 80;
        const int araH = 28;
        int araY = capsuleY + (capsuleH - araH) / 2;
        araModeLabel.setBounds(bounds.getX(), araY, araW, araH);

        // Reanalyze button — prominent action button
        const int reanalyzeW = 110;
        const int reanalyzeH = 32;
        int reanalyzeY = capsuleY + (capsuleH - reanalyzeH) / 2;
        reanalyzeButton.setBounds(bounds.getX() + araW + 8, reanalyzeY, reanalyzeW, reanalyzeH);

        transportCapsuleBounds = {}; // no transport capsule in plugin mode
        leftSectionWidth = araW + 8 + reanalyzeW + 16;
    }
    else
    {
        // Transport controls grouped in capsule
        const int transportBtnSize = 30;
        const int transportPad = 5;
        const int numTransport = 4;
        const int capsuleW = transportBtnSize * numTransport + transportPad * 2 + 6; // 6 = inner gaps
        int cx = bounds.getX();
        transportCapsuleBounds = juce::Rectangle<int>(cx, capsuleY, capsuleW, capsuleH);

        int btnY = capsuleY + (capsuleH - transportBtnSize) / 2;
        int btnX = cx + transportPad;
        goToStartButton.setBounds(btnX, btnY, transportBtnSize, transportBtnSize);
        btnX += transportBtnSize + 2;
        playButton.setBounds(btnX, btnY, transportBtnSize, transportBtnSize);
        btnX += transportBtnSize + 2;
        stopButton.setBounds(btnX, btnY, transportBtnSize, transportBtnSize);
        btnX += transportBtnSize + 2;
        goToEndButton.setBounds(btnX, btnY, transportBtnSize, transportBtnSize);

        leftSectionWidth = capsuleW + 12;
    }

    // =========================================================================
    // CENTER — Time display (centered in remaining space)
    // =========================================================================
    int centerStart = bounds.getX() + leftSectionWidth;
    int centerEnd = rightSection.getX();
    int centerAvail = centerEnd - centerStart;

    // Time display capsule
    const int timeWidth = 190;
    const int timeH = capsuleH - 2;

    // Tool container measurements (need these to center time between left section and tools)
    const int toolButtonSize = 32;
    const int toolContainerPadding = 5;
#if PITCHNET_ENABLE_STRETCH
    const int numEditTools = 5; // select, stretch, draw, split, hnsep
    const bool showRippleToggle = rippleToggleButton.isVisible();
#else
    const int numEditTools = 4; // select, draw, split, hnsep
    const bool showRippleToggle = false;
#endif
    const int numRippleToggle = showRippleToggle ? 1 : 0;
    const int numPlaybackTools = pluginMode ? 0 : 2;         // follow + loop
    const int rippleDividerWidth = showRippleToggle ? 8 : 0; // divider before ripple toggle
    const int dividerWidth = numPlaybackTools > 0 ? 8 : 0;   // space for divider between groups
    const int numAllTools = numEditTools + numRippleToggle + numPlaybackTools;
    const int toolContainerWidth = toolButtonSize * numAllTools + toolContainerPadding * 2 + rippleDividerWidth + dividerWidth;

    // Layout: [leftSection] [time] [gap] [tools] [rightSection]
    // Center the time+tools group together in the available center space
    const int timeToolGap = 16;
    const int totalCenterContent = timeWidth + timeToolGap + toolContainerWidth;
    int contentStart = centerStart + std::max(0, (centerAvail - totalCenterContent) / 2);

    // Time display
    int timeY = capsuleY + (capsuleH - timeH) / 2;
    timeCapsuleBounds = juce::Rectangle<int>(contentStart, capsuleY, timeWidth, capsuleH);
    timeLabel.setBounds(contentStart + 4, timeY, timeWidth - 8, timeH);

    // =========================================================================
    // TOOL BUTTONS — Right of time display
    // =========================================================================
    int toolStartX = contentStart + timeWidth + timeToolGap;
    toolContainerBounds = juce::Rectangle<int>(toolStartX, capsuleY, toolContainerWidth, capsuleH);
    auto toolArea = toolContainerBounds.reduced(toolContainerPadding, toolContainerPadding);
    int toolBtnH = toolArea.getHeight();
    int toolBtnY = toolArea.getY();
    int toolX = toolArea.getX();

    // Edit tools group: select, stretch, draw, split, hnsep
    selectModeButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
    toolX += toolButtonSize;
#if PITCHNET_ENABLE_STRETCH
    stretchModeButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
    toolX += toolButtonSize;
#endif
    drawModeButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
    toolX += toolButtonSize;
    splitModeButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
    toolX += toolButtonSize;
    hnsepModeButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
    toolX += toolButtonSize;

#if PITCHNET_ENABLE_STRETCH
    // Ripple mode toggle (visible only in Stretch mode)
    if (showRippleToggle)
    {
        toolX += rippleDividerWidth;
        rippleToggleButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
        toolX += toolButtonSize;
    }
#endif

    // Playback tools group (standalone only): follow, loop — with divider gap
    if (!pluginMode)
    {
        toolX += dividerWidth; // gap for visual divider
        followButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
        toolX += toolButtonSize;
        loopButton.setBounds(toolX, toolBtnY, toolButtonSize, toolBtnH);
    }
}

void ToolbarComponent::buttonClicked(juce::Button *button)
{
    if (button == &goToStartButton && onGoToStart)
        onGoToStart();
    else if (button == &goToEndButton && onGoToEnd)
        onGoToEnd();
    else if (button == &playButton)
    {
        if (isPlaying)
        {
            if (onPause)
                onPause();
        }
        else
        {
            if (onPlay)
                onPlay();
        }
    }
    else if (button == &stopButton && onStop)
        onStop();
    else if (button == &reanalyzeButton && onReanalyze)
        onReanalyze();
    else if (button == &selectModeButton)
    {
        setEditMode(EditMode::Select);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Select);
    }
#if PITCHNET_ENABLE_STRETCH
    else if (button == &stretchModeButton)
    {
        setEditMode(EditMode::Stretch);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Stretch);
    }
#endif
    else if (button == &drawModeButton)
    {
        setEditMode(EditMode::Draw);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Draw);
    }
    else if (button == &splitModeButton)
    {
        setEditMode(EditMode::Split);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Split);
    }
    else if (button == &hnsepModeButton)
    {
        setEditMode(EditMode::Parameter);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Parameter);
    }
    else if (button == &followButton)
    {
        followPlayback = !followPlayback;
        followButton.setActive(followPlayback);
    }
    else if (button == &loopButton)
    {
        loopEnabled = !loopEnabled;
        loopButton.setActive(loopEnabled);
        if (onLoopToggled)
            onLoopToggled(loopEnabled);
    }
    else if (button == &parametersButton)
    {
        parametersVisible = !parametersVisible;
        parametersButton.setActive(parametersVisible);
        if (onToggleParameters)
            onToggleParameters(parametersVisible);
    }
#if PITCHNET_ENABLE_STRETCH
    else if (button == &rippleToggleButton)
    {
        isRippleStretchMode = !isRippleStretchMode;
        rippleToggleButton.setImages(isRippleStretchMode ? rippleDrawable.get() : absorbDrawable.get());
        rippleToggleButton.setTooltip(isRippleStretchMode ? TR("toolbar.stretch_ripple") : TR("toolbar.stretch_absorb"));
        if (onRippleModeToggled)
            onRippleModeToggled(isRippleStretchMode);
    }
#endif
}

void ToolbarComponent::sliderValueChanged(juce::Slider *slider)
{
    if (slider == &zoomSlider && onZoomChanged)
        onZoomChanged(static_cast<float>(slider->getValue()));
}

void ToolbarComponent::setPlaying(bool playing)
{
    isPlaying = playing;
    playButton.setImages(playing ? pauseDrawable.get() : playDrawable.get());
}

void ToolbarComponent::setCurrentTime(double time)
{
    currentTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setTotalTime(double time)
{
    totalTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setEditMode(EditMode mode)
{
    currentEditModeInt = static_cast<int>(mode);
    selectModeButton.setActive(mode == EditMode::Select);
#if PITCHNET_ENABLE_STRETCH
    stretchModeButton.setActive(mode == EditMode::Stretch);
#endif
    drawModeButton.setActive(mode == EditMode::Draw);
    splitModeButton.setActive(mode == EditMode::Split);
    hnsepModeButton.setActive(mode == EditMode::Parameter);

#if PITCHNET_ENABLE_STRETCH
    // Show ripple toggle only in Stretch mode
    rippleToggleButton.setVisible(mode == EditMode::Stretch);
#endif
    resized();
}

void ToolbarComponent::setZoom(float pixelsPerSecond)
{
    // Update slider without triggering callback
    zoomSlider.setValue(pixelsPerSecond, juce::dontSendNotification);
}

void ToolbarComponent::setLoopEnabled(bool enabled)
{
    loopEnabled = enabled;
    loopButton.setActive(loopEnabled);
}

void ToolbarComponent::setParametersVisible(bool visible)
{
    parametersVisible = visible;
    parametersButton.setActive(parametersVisible);
}

#if PITCHNET_ENABLE_STRETCH
void ToolbarComponent::setRippleMode(bool ripple)
{
    isRippleStretchMode = ripple;
    rippleToggleButton.setImages(ripple ? rippleDrawable.get() : absorbDrawable.get());
    rippleToggleButton.setTooltip(ripple ? TR("toolbar.stretch_ripple") : TR("toolbar.stretch_absorb"));
}
#endif

void ToolbarComponent::showProgress(const juce::String &message)
{
    showingProgress = true;
    progressLabel.setText(message, juce::dontSendNotification);
    progressLabel.setVisible(true);
    progressBar.setVisible(true);
    progressValue = -1.0; // Indeterminate
    resized();
    repaint();
}

void ToolbarComponent::hideProgress()
{
    showingProgress = false;
    progressLabel.setVisible(false);
    progressBar.setVisible(false);
    resized();
    repaint();
}

void ToolbarComponent::setProgress(float progress)
{
    if (progress < 0)
        progressValue = -1.0; // Indeterminate
    else
        progressValue = static_cast<double>(juce::jlimit(0.0f, 1.0f, progress));
}

void ToolbarComponent::setStatusMessage(const juce::String &message)
{
    if (message.isEmpty())
    {
        showingStatus = false;
        statusLabel.setVisible(false);
    }
    else
    {
        showingStatus = true;
        statusLabel.setText(message, juce::dontSendNotification);
        statusLabel.setVisible(true);
    }
    resized();
    repaint();
}

void ToolbarComponent::updateTimeDisplay()
{
    timeLabel.setText(formatTime(currentTime) + " / " + formatTime(totalTime),
                      juce::dontSendNotification);
}

juce::String ToolbarComponent::formatTime(double seconds)
{
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int ms = static_cast<int>((seconds - std::floor(seconds)) * 1000);

    return juce::String::formatted("%02d:%02d.%03d", mins, secs, ms);
}

void ToolbarComponent::mouseDown(const juce::MouseEvent &e)
{
#if JUCE_MAC
    if (auto *window = getTopLevelComponent())
        dragger.startDraggingComponent(window, e.getEventRelativeTo(window));
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDrag(const juce::MouseEvent &e)
{
#if JUCE_MAC
    if (auto *window = getTopLevelComponent())
        dragger.dragComponent(window, e.getEventRelativeTo(window), nullptr);
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDoubleClick(const juce::MouseEvent &e)
{
    juce::ignoreUnused(e);
}

void ToolbarComponent::setPluginMode(bool isPlugin)
{
    pluginMode = isPlugin;

    goToStartButton.setVisible(!isPlugin);
    playButton.setVisible(!isPlugin);
    stopButton.setVisible(!isPlugin);
    goToEndButton.setVisible(!isPlugin);
    reanalyzeButton.setVisible(isPlugin);
    araModeLabel.setVisible(isPlugin);

    // In plugin mode, hide follow button (host controls playback)
    followButton.setVisible(!isPlugin);
    loopButton.setVisible(!isPlugin);

    resized();
}

void ToolbarComponent::setARAMode(bool isARA)
{
    araMode = isARA;
    araModeLabel.setText(isARA ? TR("toolbar.ara_mode") : TR("toolbar.non_ara"), juce::dontSendNotification);
}
