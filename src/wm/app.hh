#pragma once

/* The application: owns the document, undo history and window, and runs the main loop.
 * In background mode it only runs Python scripts and exits, like `blender --background`. */

#include <memory>
#include <string>
#include <vector>

#include "data/document.hh"
#include "editors/editors.hh"
#include "gpu/scene_renderer.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

struct SDL_Window;
struct SDL_GLContextState;
union SDL_Event;

namespace oanim {

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
  /* A .oanim file to open before running scripts. */
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
  /* The frame last shown, to play a frame's sound when scrubbing moves it. */
  int scrub_frame_ = -1;
  void draw_ui();
  /* A selection changed with no undo step of its own gets one ("Select"), once the mouse is up. */
  void record_selection();
  uint64_t selection_seen_changes_ = ~uint64_t(0);
  uint64_t selection_seen_ = 0;
  void draw_top_bar();
  void build_default_layout(Workspace &workspace, unsigned int dockspace_id);
  Workspace workspace_new(const std::string &name, WorkspaceKind kind);
  std::string workspace_unique_name(const std::string &base) const;
  void draw_workspace_tabs();
  void areas_split_requested(Workspace &workspace, unsigned int dockspace_id);
  void update_window_title();
  void draw_file_menu();
  void render_frame();
  void draw_render_result();
  void draw_unsaved_changes_prompt();
  void draw_asset_library_prompts();
  void draw_preferences();

  Document doc_;
  UndoStack undo_;
  WindowManager wm_;
  Context ctx_;

  std::vector<Workspace> workspaces_;
  int next_area_id_ = 1;
  bool show_about_ = false;
  bool show_preferences_ = false;
  /* The asset library was set up while the app ran: offer Moho's brushes if it has none. */
  bool library_was_ready_ = false;
  bool offer_moho_import_ = false;
  /* The last render (Render Frame), shown in the Render Result window. */
  std::unique_ptr<SceneRenderer> render_renderer_;
  unsigned int render_texture_ = 0;
  bool show_render_ = false;
  int render_frame_ = 0;
  int render_width_ = 0, render_height_ = 0;
  double render_seconds_ = 0.0;
  std::string window_title_;

  SDL_Window *window_ = nullptr;
  SDL_GLContextState *gl_context_ = nullptr;
};

}  // namespace oanim
