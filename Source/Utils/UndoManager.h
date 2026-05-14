#pragma once

/**
 * DEPRECATED: This header is kept for backward compatibility.
 * New code should include specific headers from Source/Undo/ instead:
 *   - Undo/UndoableAction.h    (base class)
 *   - Undo/F0FrameEdit.h       (shared struct)
 *   - Undo/NoteActions.h       (note-related actions)
 *   - Undo/F0Actions.h         (pitch curve actions)
 *   - Undo/DragActions.h       (drag actions)
 *   - Undo/PitchToolAction.h   (pitch tool action)
 *   - Undo/PitchUndoManager.h  (undo manager class)
 *   - Undo/UndoActions.h       (convenience include-all)
 */
#include "../Undo/UndoActions.h"
