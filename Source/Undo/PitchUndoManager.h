#pragma once

#include "UndoableAction.h"
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>

/**
 * Simple undo manager for the pitch editor.
 */
class PitchUndoManager
{
public:
    PitchUndoManager(size_t maxHistory = 100) : maxHistory(maxHistory) {}
    
    void addAction(std::unique_ptr<UndoableAction> action)
    {
        if (action && noteStorage)
            action->bindNoteStorage(noteStorage);
        if (decorateAction) action = decorateAction(std::move(action));
        if (!action) return;
        // Clear redo stack when new action is added
        redoStack.clear();
        redoStack.shrink_to_fit();

        undoStack.push_back(std::move(action));

        // Limit history size
        while (undoStack.size() > maxHistory)
        {
            undoStack.erase(undoStack.begin());
        }

        if (onHistoryChanged)
            onHistoryChanged();
    }
    
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    
    bool undo()
    {
        if (undoStack.empty()) return false;
        
        auto action = std::move(undoStack.back());
        undoStack.pop_back();
        
        const bool requiresResynthesis = action->requiresAudioResynthesis();
        action->performUndo();
        redoStack.push_back(std::move(action));
        
        if (onHistoryChanged)
            onHistoryChanged();

        return requiresResynthesis;
    }
    
    bool redo()
    {
        if (redoStack.empty()) return false;
        
        auto action = std::move(redoStack.back());
        redoStack.pop_back();
        
        const bool requiresResynthesis = action->requiresAudioResynthesis();
        action->performRedo();
        undoStack.push_back(std::move(action));
        
        if (onHistoryChanged)
            onHistoryChanged();

        return requiresResynthesis;
    }
    
    void clear()
    {
        undoStack.clear();
        undoStack.shrink_to_fit();
        redoStack.clear();
        redoStack.shrink_to_fit();

        if (onHistoryChanged)
            onHistoryChanged();
    }
    
    juce::String getUndoName() const
    {
        return undoStack.empty() ? "" : undoStack.back()->getName();
    }
    
    juce::String getRedoName() const
    {
        return redoStack.empty() ? "" : redoStack.back()->getName();
    }
    
    void removeContext(const juce::String& key)
    {
        const auto remove = [&key](auto& stack) {
            stack.erase(std::remove_if(stack.begin(), stack.end(), [&key](const auto& action)
                { return action->belongsToContext(key); }), stack.end());
        };
        remove(undoStack);
        remove(redoStack);
        if (onHistoryChanged) onHistoryChanged();
    }

    // The current project's notes; actions store note indices against it.
    std::function<std::vector<Note> *()> noteStorage;
    std::function<std::unique_ptr<UndoableAction>(std::unique_ptr<UndoableAction>)> decorateAction;
    std::function<void()> onHistoryChanged;
    
private:
    std::vector<std::unique_ptr<UndoableAction>> undoStack;
    std::vector<std::unique_ptr<UndoableAction>> redoStack;
    size_t maxHistory;
};
