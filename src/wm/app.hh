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

/* A workspace: a saved screen layout for one kind of work, chosen from the top bar's tabs,
 * like Blender's Layout and Scripting workspaces. Each keeps its own areas and docking layout. */
enum class WorkspaceKind { Layout, Scripting };

struct Workspace {
  std::string name;
  WorkspaceKind kind;
  std::vector<Area> areas;
  bool layout_built = false;
};

struct LaunchOptions {
  bool background = false;
  std::vector<std::string> python_files;
  std::vector<std::string> python_exprs;
  /* A .sky file to open before running scripts. */
  std::string open_file;
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
  void playback_update();
  void draw_ui();
  void draw_top_bar();
  void build_default_layout(Workspace &workspace, unsigned int dockspace_id);
  Workspace workspace_new(const std::string &name, WorkspaceKind kind);
  std::string workspace_unique_name(const std::string &base) const;
  void draw_workspace_tabs();
  void areas_split_requested(Workspace &workspace, unsigned int dockspace_id);
  void update_window_title();
  void draw_file_menu();
  void draw_unsaved_changes_prompt();

  Document doc_;
  UndoStack undo_;
  WindowManager wm_;
  Context ctx_;

  std::vector<Workspace> workspaces_;
  int next_area_id_ = 1;
  bool show_about_ = false;
  std::string window_title_;

  SDL_Window *window_ = nullptr;
  SDL_GLContextState *gl_context_ = nullptr;
};

}  // namespace sky
