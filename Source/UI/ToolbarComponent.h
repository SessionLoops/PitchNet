#pragma once

#include "../JuceHeader.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/Theme.h"

// Forward declaration - EditMode is defined in PianoRollComponent.h
enum class EditMode;

// Tool button with hover and active states
class ToolButton : public juce::DrawableButton
{
public:
    ToolButton(const juce::String &name) : juce::DrawableButton(name, juce::DrawableButton::ImageFitted) {}

    void setActive(bool active)
    {
        isActive = active;
        repaint();
    }
    bool getActive() const { return isActive; }

    void paint(juce::Graphics &g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(2.0f);
        auto corner = 6.0f;

        if (isActive)
        {
            // Solid primary fill with subtle inner brightness
            g.setColour(APP_COLOR_PRIMARY.withAlpha(0.85f));
            g.fillRoundedRectangle(bounds, corner);

            // Soft inner highlight along top edge
            g.setColour(juce::Colour(0x18FFFFFFu));
            g.fillRoundedRectangle(bounds.removeFromTop(bounds.getHeight() * 0.45f), corner);
        }
        else if (isMouseOver())
        {
            g.setColour(APP_COLOR_SURFACE_RAISED.brighter(0.06f));
            g.fillRoundedRectangle(bounds, corner);
            g.setColour(APP_COLOR_BORDER.withAlpha(0.5f));
            g.drawRoundedRectangle(bounds, corner, 0.75f);
        }

        juce::DrawableButton::paint(g);
    }

private:
    bool isActive = false;
};

class ToolbarComponent : public juce::Component,
                         public juce::Button::Listener,
                         public juce::Slider::Listener
{
public:
    ToolbarComponent();
    ~ToolbarComponent() override;

    void paint(juce::Graphics &g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &e) override;
    void mouseDrag(const juce::MouseEvent &e) override;
    void mouseDoubleClick(const juce::MouseEvent &e) override;

    void buttonClicked(juce::Button *button) override;
    void sliderValueChanged(juce::Slider *slider) override;

    void setPlaying(bool playing);
    void setCurrentTime(double time);
    void setTotalTime(double time);
    void setEditMode(EditMode mode);
    void setZoom(float pixelsPerSecond); // Update zoom slider without triggering callback
    void setLoopEnabled(bool enabled);
    void setParametersVisible(bool visible);
#if PITCHNET_ENABLE_STRETCH
    void setRippleMode(bool ripple); // Update ripple toggle visual
#endif
    bool isFollowPlayback() const { return followPlayback; }
    bool isLoopEnabled() const { return loopEnabled; }

    // Plugin mode
    void setPluginMode(bool isPlugin);
    void setARAMode(bool isARA); // Set ARA mode indicator

    // Progress bar control
    void showProgress(const juce::String &message);
    void hideProgress();
    void setProgress(float progress); // 0.0 to 1.0, or -1 for indeterminate

    // Status display
    void setStatusMessage(const juce::String &message); // Show status (e.g., "ARA Mode" or "Non-ARA Mode")
    juce::String getStatusText() const { return statusLabel.getText(); }

    std::function<void()> onPlay;
    std::function<void()> onPause;
    std::function<void()> onStop;
    std::function<void()> onGoToStart;
    std::function<void()> onGoToEnd;
    std::function<void(float)> onZoomChanged;
    std::function<void(EditMode)> onEditModeChanged;
    std::function<void(bool)> onLoopToggled;
#if PITCHNET_ENABLE_STRETCH
    std::function<void(bool)> onRippleModeToggled; // Called with true = Ripple, false = Absorb
#endif

    // Plugin mode callbacks
    std::function<void()> onReanalyze;
    std::function<void(bool)> onToggleParameters; // Called with new visibility state
    // Note: Removed onRender - Melodyne-style: edits automatically trigger real-time processing

private:
    void updateTimeDisplay();
    juce::String formatTime(double seconds);

    juce::DrawableButton playButton{"Play", juce::DrawableButton::ImageFitted};
    juce::DrawableButton stopButton{"Stop", juce::DrawableButton::ImageFitted};
    juce::DrawableButton goToStartButton{"Start", juce::DrawableButton::ImageFitted};
    juce::DrawableButton goToEndButton{"End", juce::DrawableButton::ImageFitted};
    std::unique_ptr<juce::Drawable> playDrawable;
    std::unique_ptr<juce::Drawable> pauseDrawable;
#if PITCHNET_ENABLE_STRETCH
    std::unique_ptr<juce::Drawable> absorbDrawable;
    std::unique_ptr<juce::Drawable> rippleDrawable;
#endif

    // Plugin mode buttons
    juce::TextButton reanalyzeButton{"Re-analyze"};
    juce::Label araModeLabel; // ARA mode indicator tag
    bool pluginMode = false;
    bool araMode = false;
    // Note: Removed renderButton - Melodyne-style: automatic real-time processing

    // Edit mode buttons
    ToolButton selectModeButton{"Select"};
#if PITCHNET_ENABLE_STRETCH
    ToolButton stretchModeButton{"Stretch"};
#endif
    ToolButton drawModeButton{"Draw"};
    ToolButton splitModeButton{"Split"};
    ToolButton hnsepModeButton{"HNSep"}; // Harmonic-noise parameter editing mode
#if PITCHNET_ENABLE_STRETCH
    ToolButton rippleToggleButton{"RippleToggle"}; // Absorb/Ripple stretch sub-mode toggle
#endif
    ToolButton followButton{"Follow"};
    ToolButton loopButton{"Loop"};
    ToolButton parametersButton{"Parameters"};
    juce::Rectangle<int> toolContainerBounds;    // For drawing container background
    juce::Rectangle<int> transportCapsuleBounds; // For drawing transport capsule background
    juce::Rectangle<int> timeCapsuleBounds;      // For drawing centered time capsule

    juce::Label timeLabel;

    juce::Slider zoomSlider;
    juce::Label zoomLabel{{}, "Zoom:"};

    // Progress components
    double progressValue = 0.0; // Must be declared before progressBar
    juce::ProgressBar progressBar{progressValue};
    juce::Label progressLabel;
    bool showingProgress = false;

    // Status label (for mode indication)
    juce::Label statusLabel;
    bool showingStatus = false;

    bool parametersVisible = false;

    double currentTime = 0.0;
    double totalTime = 0.0;
    bool isPlaying = false;
    bool followPlayback = true;
    bool loopEnabled = false;
#if PITCHNET_ENABLE_STRETCH
    bool isRippleStretchMode = false;
#endif
    int currentEditModeInt = 0; // 0 = Select, 1 = Stretch, 2 = Draw, 3 = Split, 4 = Parameter

#if JUCE_MAC
    juce::ComponentDragger dragger;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToolbarComponent)
};
