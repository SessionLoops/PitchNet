#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/Localization.h"
#include "../Buttons.h"

#include <functional>

class AnchorConfirmationPanel final : public juce::Component
{
public:
    AnchorConfirmationPanel();
    ~AnchorConfirmationPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    std::function<void()> onApply;
    std::function<void()> onCancel;

    /** Re-reads the prompt after a language change. */
    void refreshLocalisedText();

private:
    juce::Label prompt;
    Button cancelButton;
    Button okButton;

    LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnchorConfirmationPanel)
};
