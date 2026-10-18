#pragma once

/* Context: what an operator or panel is working on right now, like Blender's bContext. */

#include <string>

namespace sky {

struct Document;
class UndoStack;

/* Window-manager state that operators may change. */
struct WindowManager {
  bool background = false;
  bool quit_requested = false;
  /* Interface scale, 1.0 = 100%. */
  float ui_scale = 1.0f;
  bool ui_scale_changed = true;
};

struct Context {
  Document *document = nullptr;
  UndoStack *undo = nullptr;
  WindowManager *wm = nullptr;
  /* Keymap name of the editor under the mouse, e.g. "Viewport". */
  std::string area_type = "Window";
};

}  // namespace sky
