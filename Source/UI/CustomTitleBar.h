#pragma once

#include "../JuceHeader.h"
#include "../Utils/Localization.h"

/**
 * Custom title bar component for frameless window.
 * Platform-specific styling: macOS uses traffic lights on left, Windows/Linux uses buttons on right.
 */
class CustomTitleBar : public juce::Component
{
public:
    CustomTitleBar();
    ~CustomTitleBar() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    void setTitle(const juce::String& title);

    /** Restores the default, translated window title after a language change. */
    void refreshLocalisedText();

    static constexpr int titleBarHeight = 32;

private:
    class WindowButton : public juce::Button
    {
    public:
        enum Type { Close, Minimize, Maximize };
        WindowButton(Type type);
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;
    private:
        Type buttonType;
    };

    void closeWindow();
    void minimizeWindow();
    void toggleMaximize();

    juce::String title;
    // True while the title is the app's own default rather than a document
    // name, which is what makes it safe to re-translate on a language change.
    bool titleIsDefault = true;
    juce::ComponentDragger dragger;

#if !JUCE_MAC
    std::unique_ptr<WindowButton> minimizeButton;
    std::unique_ptr<WindowButton> maximizeButton;
    std::unique_ptr<WindowButton> closeButton;
#endif

    bool isMaximized = false;
    juce::Rectangle<int> normalBounds;

    LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomTitleBar)
};
