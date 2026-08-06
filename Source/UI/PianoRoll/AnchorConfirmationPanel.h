#pragma once

#include "../../JuceHeader.h"
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

private:
    juce::Label prompt;
    Button cancelButton;
    Button okButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnchorConfirmationPanel)
};
