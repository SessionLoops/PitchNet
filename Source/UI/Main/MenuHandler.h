#pragma once

#include "../../JuceHeader.h"
#include "../../Undo/UndoActions.h"
#include "../../Utils/Localization.h"
#include "../Commands.h"
#include <functional>

/**
 * Handles menu bar creation and menu item selection.
 */
class MenuHandler : public juce::MenuBarModel {
public:
    static constexpr int kRecentFileMenuBaseId = 0x3000;
    static constexpr int kMaxRecentMenuItems = 10;

    MenuHandler();
    ~MenuHandler() override = default;

    void setPluginMode(bool isPlugin) { pluginMode = isPlugin; }
    void setUndoManager(PitchUndoManager* mgr) { undoManager = mgr; }
    void setCommandManager(juce::ApplicationCommandManager* mgr) { commandManager = mgr; }
    void setRecentFiles(const juce::StringArray& files) { recentFiles = files; }
    void setOnRecentFileSelected(std::function<void(const juce::File&)> cb) { onRecentFileSelected = std::move(cb); }

    // MenuBarModel interface
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int menuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

#if JUCE_MAC
    juce::PopupMenu getMacExtraAppleMenu() const;
#endif

private:
    bool pluginMode = false;
    PitchUndoManager* undoManager = nullptr;
    juce::ApplicationCommandManager* commandManager = nullptr;
    juce::StringArray recentFiles;
    std::function<void(const juce::File&)> onRecentFileSelected;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MenuHandler)
};
