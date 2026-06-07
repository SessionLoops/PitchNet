#include "ToolbarComponent.h"
#include "PianoRollComponent.h" // For EditMode enum
#include "StyledComponents.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/SvgUtils.h"
#include "../Utils/UI/TimecodeFont.h"
#include "BinaryData.h"

ToolbarComponent::ToolbarComponent()
{
    auto loadImage = [](const void *data, int size)
    {
        return juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size));
    };

    playButton.setImage(loadImage(BinaryData::play_png, BinaryData::play_pngSize));
    stopButton.setImage(loadImage(BinaryData::stop_png, BinaryData::stop_pngSize));
    goToStartButton.setImage(loadImage(BinaryData::backward_png, BinaryData::backward_pngSize));
    goToEndButton.setImage(loadImage(BinaryData::forward_png, BinaryData::forward_pngSize));
    loopButton.setImage(loadImage(BinaryData::cycle_png, BinaryData::cycle_pngSize));
    logoImage = loadImage(BinaryData::logo_png, BinaryData::logo_pngSize);

    // Load remaining SVG icons with white tint
    auto cursorIcon = SvgUtils::loadSvg(BinaryData::cursor_24_filled_svg, BinaryData::cursor_24_filled_svgSize, juce::Colours::white);
    auto scissorsIcon = SvgUtils::loadSvg(BinaryData::scissors_24_filled_svg, BinaryData::scissors_24_filled_svgSize, juce::Colours::white);
    auto followIcon = SvgUtils::loadSvg(BinaryData::follow24filled_svg, BinaryData::follow24filled_svgSize, juce::Colours::white);
    const juce::String parametersIconSvg =
        R"(<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><rect x="3" y="2" width="2" height="20" rx="1"/><circle cx="4" cy="9" r="3"/><rect x="11" y="2" width="2" height="20" rx="1"/><circle cx="12" cy="15" r="3"/><rect x="19" y="2" width="2" height="20" rx="1"/><circle cx="20" cy="6" r="3"/></svg>)";
    auto parametersIcon = SvgUtils::createDrawableFromSvg(parametersIconSvg, juce::Colours::white);

    selectModeButton.setImages(cursorIcon.get());
    splitModeButton.setImages(scissorsIcon.get());
    followButton.setImages(followIcon.get());
    parametersButton.setImages(parametersIcon.get());

    // Set edge indent for icon padding (makes icons smaller within button bounds)
    selectModeButton.setEdgeIndent(6);
    splitModeButton.setEdgeIndent(6);
    followButton.setEdgeIndent(6);
    parametersButton.setEdgeIndent(6);

    // Configure buttons
    addAndMakeVisible(goToStartButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(goToEndButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(selectModeButton);
    addAndMakeVisible(splitModeButton);
    addAndMakeVisible(followButton);
    addAndMakeVisible(parametersButton);

    goToStartButton.addListener(this);
    playButton.addListener(this);
    stopButton.addListener(this);
    goToEndButton.addListener(this);
    loopButton.addListener(this);
    selectModeButton.addListener(this);
    splitModeButton.addListener(this);
    followButton.addListener(this);
    parametersButton.addListener(this);

    // Set localized text (tooltips for icon buttons)
    selectModeButton.setTooltip(TR("toolbar.select"));
    splitModeButton.setTooltip(TR("toolbar.split"));
    followButton.setTooltip(TR("toolbar.follow"));
    loopButton.setTooltip(TR("toolbar.loop"));
    parametersButton.setTooltip(TR("panel.parameters"));
    zoomLabel.setText(TR("toolbar.zoom"), juce::dontSendNotification);

    // Set default active states
    selectModeButton.setActive(true);
    followButton.setActive(true); // Follow is on by default
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

    progressLabel.setVisible(false);
    progressBar.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(0xFFFF5600u));
    progressBar.setColour(juce::ProgressBar::backgroundColourId, juce::Colours::transparentBlack);
    progressBar.setLookAndFeel(&DarkLookAndFeel::getInstance());

}

ToolbarComponent::~ToolbarComponent()
{
    progressBar.setLookAndFeel(nullptr);
}

void ToolbarComponent::paint(juce::Graphics &g)
{
    auto bounds = getLocalBounds().toFloat();

    // Flat surface background
    g.setColour(juce::Colour(0xFF0D0B0Bu));
    g.fillRect(bounds);

    if (logoImage.isValid())
    {
        const int logoW = (logoImage.getWidth() + 1) / 2;
        const int logoH = (logoImage.getHeight() + 1) / 2;
        const int logoX = 16;
        const int logoY = (getHeight() - logoH) / 2;
        g.drawImage(logoImage, logoX, logoY, logoW, logoH,
                    0, 0, logoImage.getWidth(), logoImage.getHeight());
    }

    // Transport capsule background
    if (!transportCapsuleBounds.isEmpty())
    {
        auto capsule = transportCapsuleBounds.toFloat();
        g.setColour(juce::Colour(0xFF191818u));
        g.fillRoundedRectangle(capsule, 8.0f);
    }
}

void ToolbarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10, 0);
    const auto fullToolbarBounds = bounds;
    const int contentH = bounds.getHeight() - 2; // leave 1px top/bottom margin + 1px for separator
    const int yOffset = 1;
    const int capsuleH = contentH - 10; // capsule inner height with vertical padding
    const int capsuleY = yOffset + (contentH - capsuleH) / 2;

    // =========================================================================
    // RIGHT SIDE — Parameters button
    // =========================================================================
    const int rightButtonSize = 30;
    auto rightSection = bounds.removeFromRight(250);

    // Parameters button — rightmost
    auto paramBtnArea = rightSection.removeFromRight(rightButtonSize + 6);
    parametersButton.setBounds(
        paramBtnArea.getRight() - rightButtonSize,
        capsuleY + (capsuleH - rightButtonSize) / 2,
        rightButtonSize, rightButtonSize);

    // Hide center controls (time/tools removed from toolbar) and zoom controls
    timeLabel.setVisible(false);
    selectModeButton.setVisible(false);
    splitModeButton.setVisible(false);
    followButton.setVisible(false);
    zoomLabel.setVisible(false);
    zoomSlider.setVisible(false);
    toolContainerBounds = {};
    timeCapsuleBounds = {};

    goToStartButton.setVisible(true);
    playButton.setVisible(true);
    stopButton.setVisible(true);
    goToEndButton.setVisible(true);
    loopButton.setVisible(true);

    // Transport controls grouped in capsule. In plugin mode these buttons
    // request host transport changes through MainComponent.
    const int transportSlotSize = 30;
    const int transportPad = 5;
    const int numCenteredTransport = 4;
    const int numTransport = 5;
    const int centeredTransportW = transportSlotSize * numCenteredTransport + (numCenteredTransport - 1) * 2 + transportPad * 2;
    const int capsuleW = transportSlotSize * numTransport + (numTransport - 1) * 2 + transportPad * 2;
    int cx = fullToolbarBounds.getCentreX() - centeredTransportW / 2;
    const int transportCapsuleH = 38;
    const int transportCapsuleY = yOffset + (contentH - transportCapsuleH) / 2;
    transportCapsuleBounds = juce::Rectangle<int>(cx, transportCapsuleY, capsuleW, transportCapsuleH);
    if (showingProgress)
        progressBar.setBounds(transportCapsuleBounds.getX(),
                              transportCapsuleBounds.getBottom() - 2,
                              transportCapsuleBounds.getWidth(), 2);

    auto setButtonInSlot = [&](juce::Button &button, int slotX)
    {
        const int buttonW = button.getWidth() > 0 ? button.getWidth() : transportSlotSize;
        const int buttonH = button.getHeight() > 0 ? button.getHeight() : transportSlotSize;
        button.setBounds(slotX + (transportSlotSize - buttonW) / 2,
                         transportCapsuleY + (transportCapsuleH - buttonH) / 2,
                         buttonW, buttonH);
    };

    int slotX = cx + transportPad;
    setButtonInSlot(stopButton, slotX);
    slotX += transportSlotSize + 2;
    setButtonInSlot(playButton, slotX);
    slotX += transportSlotSize + 2;
    setButtonInSlot(goToStartButton, slotX);
    slotX += transportSlotSize + 2;
    setButtonInSlot(goToEndButton, slotX);
    slotX += transportSlotSize + 2;
    setButtonInSlot(loopButton, slotX);
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
    else if (button == &loopButton)
    {
        loopEnabled = !loopEnabled;
        loopButton.setToggleState(loopEnabled, juce::dontSendNotification);
        if (onToggleLoop)
            onToggleLoop(loopEnabled);
    }
    else if (button == &selectModeButton)
    {
        setEditMode(EditMode::Select);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Select);
    }
    else if (button == &splitModeButton)
    {
        setEditMode(EditMode::Split);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Split);
    }
    else if (button == &followButton)
    {
        followPlayback = !followPlayback;
        followButton.setActive(followPlayback);
    }
    else if (button == &parametersButton)
    {
        parametersVisible = !parametersVisible;
        parametersButton.setActive(parametersVisible);
        if (onToggleParameters)
            onToggleParameters(parametersVisible);
    }
}

void ToolbarComponent::sliderValueChanged(juce::Slider *slider)
{
    if (slider == &zoomSlider && onZoomChanged)
        onZoomChanged(static_cast<float>(slider->getValue()));
}

void ToolbarComponent::setPlaying(bool playing)
{
    isPlaying = playing;
    playButton.setToggleState(playing, juce::dontSendNotification);
}

void ToolbarComponent::setTransportEnabled(bool enabled)
{
    stopButton.setEnabled(enabled);
    playButton.setEnabled(enabled);
    goToStartButton.setEnabled(enabled);
    goToEndButton.setEnabled(enabled);
    loopButton.setEnabled(enabled);
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
    splitModeButton.setActive(mode == EditMode::Split);
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
    loopButton.setToggleState(loopEnabled, juce::dontSendNotification);
}

void ToolbarComponent::setParametersVisible(bool visible)
{
    parametersVisible = visible;
    parametersButton.setActive(parametersVisible);
}

void ToolbarComponent::showProgress(const juce::String &message)
{
    juce::ignoreUnused(message);
    showingProgress = true;
    progressLabel.setVisible(false);
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
    juce::ignoreUnused(message);
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

    goToStartButton.setVisible(true);
    playButton.setVisible(true);
    stopButton.setVisible(true);
    goToEndButton.setVisible(true);
    loopButton.setVisible(true);

    // In plugin mode, hide follow button (host controls playback)
    followButton.setVisible(!isPlugin);

    resized();
}
