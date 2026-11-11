#pragma once
#include "UndoableAction.h"
#include <functional>

// Bind a clip-local action to its owning project before applying it from a
// shared track history. Track-wide actions retain their own validity checks.
class ContextUndoAction final : public UndoableAction {
public:
  ContextUndoAction(std::unique_ptr<UndoableAction> action, juce::String context,
      juce::String label, std::function<bool()> prepare)
      : action(std::move(action)), context(std::move(context)), label(std::move(label)),
        prepare(std::move(prepare)) {}
  void undo() override { if (prepare()) action->performUndo(); }
  void redo() override { if (prepare()) action->performRedo(); }
  juce::String getName() const override { return action->getName() + " — " + label; }
  bool belongsToContext(const juce::String& key) const override { return key == context; }
  bool requiresAudioResynthesis() const override { return action->requiresAudioResynthesis(); }
private:
  std::unique_ptr<UndoableAction> action;
  juce::String context, label;
  std::function<bool()> prepare;
};
