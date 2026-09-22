#include "ToolbarComponent.h"
#include "PianoRollComponent.h" // For EditMode enum
#include "StyledComponents.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/TimecodeFont.h"
#include "BinaryData.h"

ToolbarComponent::ToolbarComponent()
{
    auto loadImage = [](const void *data, int size)
    {
        return juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size));
    };

    recordButton.setImage(loadImage(BinaryData::record_png, BinaryData::record_pngSize));
    playButton.setImage(loadImage(BinaryData::play_png, BinaryData::play_pngSize));
    stopButton.setImage(loadImage(BinaryData::stop_png, BinaryData::stop_pngSize));
    loopButton.setImage(loadImage(BinaryData::cycle_png, BinaryData::cycle_pngSize));
    quantizeButton.setImage(loadImage(BinaryData::quantize_png, BinaryData::quantize_pngSize));
    auditionButton.setImage(loadImage(BinaryData::audition_png, BinaryData::audition_pngSize));
    undoButton.setImage(loadImage(BinaryData::undo_png, BinaryData::undo_pngSize));
    redoButton.setImage(loadImage(BinaryData::redo_png, BinaryData::redo_pngSize));
    selectModeButton.setImage(loadImage(BinaryData::select_png, BinaryData::select_pngSize));
    splitModeButton.setImage(loadImage(BinaryData::split_png, BinaryData::split_pngSize));
    pitchDrawingModeButton.setImage(loadImage(BinaryData::drawing_png, BinaryData::drawing_pngSize));
    timingModeButton.setImage(loadImage(BinaryData::timing_png, BinaryData::timing_pngSize));
    logoImage = loadImage(BinaryData::logo_png, BinaryData::logo_pngSize);

    followButton.setImage(loadImage(BinaryData::scroll_png, BinaryData::scroll_pngSize));
    parametersButton.setImage(loadImage(BinaryData::side_png, BinaryData::side_pngSize));

    // Configure buttons
    addChildComponent(recordButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(selectModeButton);
    addAndMakeVisible(splitModeButton);
    addAndMakeVisible(pitchDrawingModeButton);
    addAndMakeVisible(timingModeButton);
    addAndMakeVisible(followButton);
    addAndMakeVisible(scaleSelectionButton);
    addAndMakeVisible(quantizeButton);
    addAndMakeVisible(auditionButton);
    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    addAndMakeVisible(parametersButton);

    recordButton.addListener(this);
    playButton.addListener(this);
    stopButton.addListener(this);
    loopButton.addListener(this);
    selectModeButton.addListener(this);
    splitModeButton.addListener(this);
    pitchDrawingModeButton.addListener(this);
    timingModeButton.addListener(this);
    followButton.addListener(this);
    quantizeButton.addListener(this);
    auditionButton.addListener(this);
    undoButton.addListener(this);
    redoButton.addListener(this);
    parametersButton.addListener(this);

    scaleSelectionButton.onScaleRootChanged = [this](int rootNote)
    {
        if (onScaleRootChanged)
            onScaleRootChanged(rootNote);
    };
    scaleSelectionButton.onScaleModeChanged = [this](ScaleMode mode)
    {
        if (onScaleModeChanged)
            onScaleModeChanged(mode);
    };
    scaleSelectionButton.onScaleRootPreviewChanged =
        [this](std::optional<int> rootNote)
    {
        if (onScaleRootPreviewChanged)
            onScaleRootPreviewChanged(rootNote);
    };
    scaleSelectionButton.onScaleModePreviewChanged =
        [this](std::optional<ScaleMode> mode)
    {
        if (onScaleModePreviewChanged)
            onScaleModePreviewChanged(mode);
    };
    scaleSelectionButton.onPreferredWidthChanged = [this]
    {
        resized();
    };

    // Tooltips and the zoom caption all come from the string table; they are
    // set here and re-set whenever the language changes.
    refreshLocalisedText();

    // Set default active states
    selectModeButton.setToggleState(true, juce::dontSendNotification);
    followButton.setToggleState(followPlayback, juce::dontSendNotification);
    auditionButton.setToggleState(false, juce::dontSendNotification);
    undoButton.setEnabled(false);
    redoButton.setEnabled(false);
    parametersButton.setToggleState(false, juce::dontSendNotification);

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
        const int logoX = 17;
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

    // Edit tools form their own capsule, matching the transport group.
    if (!toolContainerBounds.isEmpty())
    {
        g.setColour(juce::Colour(0xFF191818u));
        g.fillRoundedRectangle(toolContainerBounds.toFloat(), 8.0f);
    }
}

void ToolbarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(12, 0);
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

    auto redoBtnArea = rightSection.removeFromRight(rightButtonSize + 2);
    redoButton.setBounds(redoBtnArea.getRight() - rightButtonSize,
                         capsuleY + (capsuleH - rightButtonSize) / 2,
                         rightButtonSize, rightButtonSize);

    auto undoBtnArea = rightSection.removeFromRight(rightButtonSize + 2);
    undoButton.setBounds(undoBtnArea.getRight() - rightButtonSize,
                         capsuleY + (capsuleH - rightButtonSize) / 2,
                         rightButtonSize, rightButtonSize);

    auto auditionBtnArea = rightSection.removeFromRight(rightButtonSize + 2);
    auditionButton.setBounds(auditionBtnArea.getRight() - rightButtonSize - 4,
                             capsuleY + (capsuleH - rightButtonSize) / 2,
                             rightButtonSize, rightButtonSize);

    auto quantizeBtnArea = rightSection.removeFromRight(rightButtonSize + 2);
    quantizeButton.setBounds(quantizeBtnArea.getRight() - rightButtonSize - 8,
                             capsuleY + (capsuleH - rightButtonSize) / 2,
                             rightButtonSize, rightButtonSize);

    const int scaleButtonWidth = scaleSelectionButton.getPreferredWidth();
    constexpr int scaleQuantizeGap = 11;
    scaleSelectionButton.setBounds(
        quantizeButton.getX() - scaleQuantizeGap - scaleButtonWidth,
        capsuleY + (capsuleH - 26) / 2,
        scaleButtonWidth, 26);

    // Keep the edit tools centered in the toolbar.
    const int editToolGap = 6;
    const int editToolPad = 15;
    const int editToolSlotSize = 22;
    const int editToolGroupHeight = 38;
    const int logoRight = 17 + (logoImage.isValid()
                                    ? (logoImage.getWidth() + 1) / 2
                                    : 0);
    const int editToolGroupWidth = editToolSlotSize * 4 + editToolGap * 3
                                   + editToolPad * 2;
    const int editToolGroupX = fullToolbarBounds.getCentreX()
                               - editToolGroupWidth / 2;
    const int editToolGroupY = yOffset + (contentH - editToolGroupHeight) / 2;
    toolContainerBounds = {editToolGroupX, editToolGroupY,
                           editToolGroupWidth, editToolGroupHeight};
    int editToolX = toolContainerBounds.getX() + editToolPad;

    selectModeButton.setVisible(true);
    selectModeButton.setBounds(editToolX,
                               toolContainerBounds.getCentreY() - editToolSlotSize / 2,
                               editToolSlotSize, editToolSlotSize);
    editToolX += editToolSlotSize + editToolGap;
    splitModeButton.setVisible(true);
    splitModeButton.setBounds(editToolX,
                              toolContainerBounds.getCentreY() - editToolSlotSize / 2,
                              editToolSlotSize, editToolSlotSize);
    editToolX += editToolSlotSize + editToolGap;
    pitchDrawingModeButton.setVisible(true);
    pitchDrawingModeButton.setBounds(editToolX,
                               toolContainerBounds.getCentreY() - editToolSlotSize / 2,
                               editToolSlotSize, editToolSlotSize);
    editToolX += editToolSlotSize + editToolGap;
    timingModeButton.setVisible(true);
    timingModeButton.setBounds(editToolX,
                               toolContainerBounds.getCentreY() - editToolSlotSize / 2,
                               editToolSlotSize, editToolSlotSize);

    // Hide the remaining controls that have been removed from the toolbar.
    timeLabel.setVisible(false);
    zoomLabel.setVisible(false);
    zoomSlider.setVisible(false);
    timeCapsuleBounds = {};

    playButton.setVisible(true);
    stopButton.setVisible(true);
    loopButton.setVisible(true);
    followButton.setVisible(true);

    // Keep transport beside the logo. In plugin mode these buttons request
    // host transport changes through MainComponent.
    const int transportSlotSize = 30;
    const int transportPad = 9;
    const int transportSlotStride = 28;
    const int numTransport = recordButton.isVisible() ? 5 : 4;
    const int capsuleW = transportSlotSize
                         + (numTransport - 1) * transportSlotStride
                         + transportPad * 2 + 6;
    const int capsuleX = logoRight + 24;
    const int transportCapsuleH = 38;
    const int transportCapsuleY = yOffset + (contentH - transportCapsuleH) / 2;
    transportCapsuleBounds = juce::Rectangle<int>(capsuleX, transportCapsuleY, capsuleW, transportCapsuleH);
    if (showingProgress)
        progressBar.setBounds(toolContainerBounds.getX(),
                              toolContainerBounds.getBottom() - 2,
                              toolContainerBounds.getWidth(), 2);

    auto setButtonInSlot = [&](juce::Button &button, int slotX)
    {
        const int buttonW = button.getWidth() > 0 ? button.getWidth() : transportSlotSize;
        const int buttonH = button.getHeight() > 0 ? button.getHeight() : transportSlotSize;
        button.setBounds(slotX + (transportSlotSize - buttonW) / 2,
                         transportCapsuleY + (transportCapsuleH - buttonH) / 2,
                         buttonW, buttonH);
    };

    int slotX = capsuleX + transportPad;
    if (recordButton.isVisible()) {
        setButtonInSlot(recordButton, slotX);
        slotX += transportSlotStride;
    }
    setButtonInSlot(stopButton, slotX);
    slotX += transportSlotStride;
    setButtonInSlot(playButton, slotX);
    slotX += transportSlotStride;
    setButtonInSlot(loopButton, slotX);
    slotX += transportSlotStride;
    setButtonInSlot(followButton, slotX);
    followButton.setBounds(followButton.getBounds().translated(5, 0));
}

void ToolbarComponent::refreshLocalisedText()
{
    selectModeButton.setTooltip(TR("tooltip.select_tool"));
    splitModeButton.setTooltip(TR("tooltip.split_tool"));
    pitchDrawingModeButton.setTooltip(TR("tooltip.pitch_drawing_tool"));
    timingModeButton.setTooltip(TR("tooltip.timing_tool"));
    followButton.setTooltip(TR("tooltip.auto_scroll"));
    quantizeButton.setTooltip(TR("tooltip.quantize_macro"));
    auditionButton.setTooltip(TR("tooltip.live_audition"));
    recordButton.setTooltip(TR("tooltip.capture"));
    playButton.setTooltip(TR("toolbar.play"));
    stopButton.setTooltip(TR("toolbar.stop"));
    loopButton.setTooltip(TR("toolbar.loop"));
    parametersButton.setTooltip(TR("panel.parameters"));
#if JUCE_MAC
    undoButton.setTooltip(TR("command.undo") + " (\u2318Z)");
    redoButton.setTooltip(TR("command.redo") + " (\u21e7\u2318Z)");
#else
    undoButton.setTooltip(TR("command.undo") + " (Ctrl+Z)");
    redoButton.setTooltip(TR("command.redo") + " (Ctrl+Y)");
#endif

    zoomLabel.setText(TR("toolbar.zoom"), juce::dontSendNotification);
}

void ToolbarComponent::buttonClicked(juce::Button *button)
{
    if (button == &recordButton)
    {
        if (onToggleRecord)
            onToggleRecord(recordButton.getToggleState());
    }
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
    else if (button == &pitchDrawingModeButton)
    {
        setEditMode(EditMode::PitchDrawing);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::PitchDrawing);
    }
    else if (button == &timingModeButton)
    {
        setEditMode(EditMode::Timing);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Timing);
    }
    else if (button == &followButton)
    {
        followPlayback = followButton.getToggleState();
    }
    else if (button == &auditionButton)
    {
        if (onToggleAudition)
            onToggleAudition(auditionButton.getToggleState());
    }
    else if (button == &quantizeButton)
    {
        if (onCorrectPitchCenter)
            onCorrectPitchCenter();
    }
    else if (button == &undoButton && onUndo)
        onUndo();
    else if (button == &redoButton && onRedo)
        onRedo();
    else if (button == &parametersButton)
    {
        parametersVisible = parametersButton.getToggleState();
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
    transportEnabled = enabled;

    // Capture is driven by the plugin itself, so it only follows `enabled`.
    recordButton.setEnabled(enabled);

    // Play / stop / cycle command the host transport. Without a host link
    // (non-ARA plugin mode) they cannot do anything, so keep them disabled.
    const bool hostControls = enabled && hostTransportAvailable;
    stopButton.setEnabled(hostControls);
    playButton.setEnabled(hostControls);
    loopButton.setEnabled(hostControls);
}

void ToolbarComponent::setHostTransportAvailable(bool available)
{
    if (hostTransportAvailable == available)
        return;

    hostTransportAvailable = available;

    if (!available)
    {
        isPlaying = false;
        playButton.setToggleState(false, juce::dontSendNotification);
    }

    setTransportEnabled(transportEnabled);
    repaint();
}

void ToolbarComponent::setToolGroupEnabled(bool enabled)
{
    selectModeButton.setEnabled(enabled);
    splitModeButton.setEnabled(enabled);
    pitchDrawingModeButton.setEnabled(enabled);
    timingModeButton.setEnabled(enabled);
}

void ToolbarComponent::setProject(Project* project)
{
    scaleSelectionButton.setProject(project);
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
    selectModeButton.setToggleState(mode == EditMode::Select,
                                    juce::dontSendNotification);
    splitModeButton.setToggleState(mode == EditMode::Split,
                                   juce::dontSendNotification);
    pitchDrawingModeButton.setToggleState(mode == EditMode::PitchDrawing,
                                    juce::dontSendNotification);
    timingModeButton.setToggleState(mode == EditMode::Timing,
                                    juce::dontSendNotification);
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

void ToolbarComponent::setAuditionEnabled(bool enabled)
{
    auditionButton.setToggleState(enabled, juce::dontSendNotification);
}

void ToolbarComponent::setParametersVisible(bool visible)
{
    parametersVisible = visible;
    parametersButton.setToggleState(parametersVisible, juce::dontSendNotification);
}

void ToolbarComponent::setUndoRedoEnabled(bool undoEnabled, bool redoEnabled)
{
    undoButton.setEnabled(undoEnabled);
    redoButton.setEnabled(redoEnabled);
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
    // Standalone only: the toolbar doubles as the window's drag handle.
    // In plugin mode getTopLevelComponent() is the AudioProcessorEditor itself,
    // which lives inside the host's view - dragging it would move the editor
    // component within that view and leave the UI clipped/offset.
    if (!pluginMode)
    {
        if (auto *window = getTopLevelComponent())
            dragger.startDraggingComponent(window, e.getEventRelativeTo(window));
    }
    else
    {
        juce::ignoreUnused(e);
    }
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDrag(const juce::MouseEvent &e)
{
#if JUCE_MAC
    // See mouseDown(): never drag the top-level component in plugin mode.
    if (!pluginMode)
    {
        if (auto *window = getTopLevelComponent())
            dragger.dragComponent(window, e.getEventRelativeTo(window), nullptr);
    }
    else
    {
        juce::ignoreUnused(e);
    }
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

    playButton.setVisible(true);
    stopButton.setVisible(true);
    loopButton.setVisible(true);

    resized();
}

void ToolbarComponent::setRecordControlVisible(bool visible)
{
    recordButton.setVisible(visible);
    if (!visible)
        recordButton.setToggleState(false, juce::dontSendNotification);
    resized();
    repaint();
}

void ToolbarComponent::setRecordArmed(bool armed)
{
    recordButton.setToggleState(armed, juce::dontSendNotification);
}
