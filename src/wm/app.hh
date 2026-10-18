#pragma once

/* The application: owns the document, undo history and window, and runs the main loop.
 * In background mode it only runs Python scripts and exits, like `blender --background`. */

#include <string>
#include <vector>

#include "data/document.hh"
#include "editors/editors.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

struct SDL_Window;
struct SDL_GLContextState;
union SDL_Event;

namespace sky {

struct LaunchOptions {
  bool background = false;
  std::vector<std::string> python_files;
  std::vector<std::string> python_exprs;
};

class App {
 public:
  /* Returns the process exit code. */
  int run(const LaunchOptions &options);

 private:
  bool window_init();
  void window_exit();
  void main_loop();
  void handle_event(const SDL_Event &event);
  void draw_ui();
  void draw_top_bar();
  void build_default_layout(unsigned int dockspace_id);
  void update_window_title();

  Document doc_;
  UndoStack undo_;
  WindowManager wm_;
  Context ctx_;

  std::vector<Area> areas_;
  int next_area_id_ = 1;
  bool layout_built_ = false;
  bool show_about_ = false;

  SDL_Window *window_ = nullptr;
  SDL_GLContextState *gl_context_ = nullptr;
};

}  // namespace sky
