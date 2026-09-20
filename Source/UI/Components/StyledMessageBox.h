#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/Localization.h"
#include "../../Utils/UI/Theme.h"
#include "AppFont.h"
#include <functional>
#include <memory>

/**
 * Custom styled message box component matching the app's dark theme.
 */
class StyledMessageBox : public juce::Component
{
public:
    enum IconType
    {
        NoIcon,
        InfoIcon,
        WarningIcon,
        ErrorIcon
    };

    StyledMessageBox(const juce::String& title, const juce::String& message, IconType iconTypeValue = NoIcon)
        : titleText(title), messageText(message), iconType(iconTypeValue)
    {
        setOpaque(false);

        // Add OK button
        okButton = std::make_unique<juce::TextButton>(TR("dialog.ok"));
        okButton->setSize(90, 34);
        okButton->onClick = [this] {
            if (onClose != nullptr)
                onClose();
        };
        addAndMakeVisible(okButton.get());

        // Style the button
        okButton->setColour(juce::TextButton::buttonColourId, APP_COLOR_PRIMARY);
        okButton->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        okButton->setColour(juce::TextButton::buttonOnColourId, APP_COLOR_PRIMARY.brighter(0.15f));

        setSize(400, 200);
    }

    void resized() override
    {
        if (okButton != nullptr)
        {
            okButton->setCentrePosition(getWidth() / 2, getHeight() - 32);
        }
    }

    std::function<void()> onClose;

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        constexpr float radius = 10.0f;

        // Rounded card background
        g.setColour(APP_COLOR_SURFACE_ALT);
        g.fillRoundedRectangle(bounds, radius);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.55f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 0.75f);

        // Title
        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getBoldFont(18.0f));
        g.drawText(titleText, 20, 18, getWidth() - 40, 28, juce::Justification::left);

        // Icon (if any)
        int iconX = 20;
        int iconY = 58;
        int iconSize = 30;

        if (iconType != NoIcon)
        {
            juce::Colour iconColour;
            if (iconType == InfoIcon)
                iconColour = APP_COLOR_PRIMARY;
            else if (iconType == WarningIcon)
                iconColour = APP_COLOR_ALERT_WARNING;
            else if (iconType == ErrorIcon)
                iconColour = APP_COLOR_ALERT_ERROR;

            // Softer icon circle
            g.setColour(iconColour.withAlpha(0.15f));
            g.fillEllipse(static_cast<float>(iconX), static_cast<float>(iconY),
                          static_cast<float>(iconSize), static_cast<float>(iconSize));
            g.setColour(iconColour);
            g.drawEllipse(static_cast<float>(iconX) + 0.5f, static_cast<float>(iconY) + 0.5f,
                          static_cast<float>(iconSize) - 1.0f, static_cast<float>(iconSize) - 1.0f, 1.2f);

            g.setFont(AppFont::getBoldFont(static_cast<float>(iconSize) * 0.52f));

            juce::String iconChar;
            if (iconType == InfoIcon)
                iconChar = "i";
            else if (iconType == WarningIcon)
                iconChar = "!";
            else if (iconType == ErrorIcon)
                iconChar = "X";

            g.drawText(iconChar, iconX, iconY, iconSize, iconSize, juce::Justification::centred);
            iconX += iconSize + 14;
        }

        // Message text
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.setFont(AppFont::getFont(14.0f));
        g.drawMultiLineText(messageText, iconX, iconY + 5, getWidth() - iconX - 20, juce::Justification::topLeft);
    }

    static void show(juce::Component* parent, const juce::String& title, const juce::String& message, IconType iconType = NoIcon)
    {
        auto* dialog = new StyledMessageDialog(parent, title, message, iconType);
        dialog->enterModalState(true, nullptr, true);
    }

private:
    juce::String titleText;
    juce::String messageText;
    IconType iconType;
    std::unique_ptr<juce::TextButton> okButton;

    class StyledMessageDialog : public juce::DialogWindow
    {
    public:
        StyledMessageDialog(juce::Component* parent, const juce::String& title, const juce::String& message, StyledMessageBox::IconType iconTypeValue)
            : juce::DialogWindow(title, APP_COLOR_BACKGROUND, true)
        {
            setOpaque(true);
            setUsingNativeTitleBar(false);
            setResizable(false, false);

            // Remove close button from title bar
            setTitleBarButtonsRequired(0, false);

            messageBox = std::make_unique<StyledMessageBox>(title, message, iconTypeValue);
            messageBox->onClose = [this] { closeDialog(); };
            setContentOwned(messageBox.get(), false);

            int dialogWidth = 420;
            int dialogHeight = 220;
            setSize(dialogWidth, dialogHeight);

            if (parent != nullptr)
                centreAroundComponent(parent, dialogWidth, dialogHeight);
            else
                centreWithSize(dialogWidth, dialogHeight);
        }

        void closeButtonPressed() override
        {
            closeDialog();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(APP_COLOR_BACKGROUND);
        }

    private:
        void closeDialog()
        {
            exitModalState(0);
        }

        std::unique_ptr<StyledMessageBox> messageBox;
    };
};
