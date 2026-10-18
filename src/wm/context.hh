#pragma once

/* Context: what an operator or panel is working on right now, like Blender's bContext. */

#include <cstdint>
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

  /* Animation playback, advanced by the main loop in real time. */
  bool playing = false;
  bool play_reverse = false;
  bool play_loop = true;
  /* Frame to return to when playback is cancelled with Esc. */
  int play_start_frame = 1;
  uint64_t play_last_ns = 0;
  double play_frames_due = 0.0;

  /* Auto-keying record button; takes effect once keyframes exist (milestone 3). */
  bool auto_key = true;
};

struct Context {
  Document *document = nullptr;
  UndoStack *undo = nullptr;
  WindowManager *wm = nullptr;
  /* Keymap name of the editor under the mouse, e.g. "Viewport". */
  std::string area_type = "Window";
};

}  // namespace sky
