#include "MenuHandler.h"

MenuHandler::MenuHandler() = default;

juce::StringArray MenuHandler::getMenuBarNames() {
    if (pluginMode)
        return {TR("menu.edit"), TR("menu.settings")};
    return {TR("menu.file"), TR("menu.edit"), TR("menu.settings")};
}

juce::PopupMenu MenuHandler::getMenuForIndex(int menuIndex, const juce::String& /*menuName*/) {
    juce::PopupMenu menu;

    if (pluginMode) {
        if (menuIndex == 0) {
            // Edit menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::undo);
                menu.addCommandItem(commandManager, CommandIDs::redo);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::selectAll);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::fourierFilter);
            }
        } else if (menuIndex == 1) {
            // Settings menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::showSettings);
            }
        }
    } else {
        if (menuIndex == 0) {
            // File menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::openFile);
                juce::PopupMenu recentMenu;
                if (recentFiles.isEmpty()) {
                    recentMenu.addItem(1, "No Recent Files", false, false);
                } else {
                    const int count = juce::jmin(kMaxRecentMenuItems, recentFiles.size());
                    for (int i = 0; i < count; ++i) {
                        juce::File file(recentFiles[i]);
                        const juce::String label =
                            juce::String(i + 1) + "  " + file.getFullPathName();
                        recentMenu.addItem(kRecentFileMenuBaseId + i, label);
                    }
                }
                menu.addSubMenu("Recent Files", recentMenu);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::exportAudio);
                menu.addCommandItem(commandManager, CommandIDs::exportMidi);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::quit);
            }
        } else if (menuIndex == 1) {
            // Edit menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::undo);
                menu.addCommandItem(commandManager, CommandIDs::redo);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::selectAll);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::fourierFilter);
            }
        } else if (menuIndex == 2) {
            // Settings menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::showSettings);
            }
        }
    }

    return menu;
}

void MenuHandler::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/) {
    // Command items are handled automatically by ApplicationCommandManager
    if (menuItemID >= kRecentFileMenuBaseId &&
        menuItemID < kRecentFileMenuBaseId + kMaxRecentMenuItems) {
        const int idx = menuItemID - kRecentFileMenuBaseId;
        if (idx >= 0 && idx < recentFiles.size() && onRecentFileSelected) {
            onRecentFileSelected(juce::File(recentFiles[idx]));
        }
        return;
    }
}
