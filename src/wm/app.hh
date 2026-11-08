#pragma once

/* The application: owns the document, undo history and window, and runs the main loop.
 * In background mode it only runs Python scripts and exits, like `blender --background`. */

#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>

#include "data/document.hh"
#include "editors/editors.hh"
#include "gpu/scene_renderer.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

struct SDL_Window;
struct SDL_GLContextState;

namespace oanim {

/* A workspace: a saved screen layout for one kind of work, chosen from the top bar's tabs,
 * like Blender's Layout, Animation and Scripting workspaces. Each keeps its own areas and docking layout; going to
 * Layout picks the Select Box tool and going to Animation the Motion Path tool. */
enum class WorkspaceKind { Layout, Scripting, Animation };

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
  void update_number_grab();
  void handle_event(const SDL_Event &event);
  void playback_update();
  /* The frame last shown, to play a frame's sound when scrubbing moves it. */
  int scrub_frame_ = -1;
  void draw_ui();
  /* A selection changed with no undo step of its own gets one ("Select"), once the mouse is up. */
  void record_selection();
  uint64_t selection_seen_changes_ = ~uint64_t(0);
  uint64_t selection_seen_ = 0;
  /* The newest revision when the selection was last looked at: none since, nothing could have changed. */
  uint64_t selection_seen_revision_ = 0;
  /* Revisions (see Revision): what was seen last frame, to tell whether this one may have changed anything
   * nobody said; and, with OANIM_CHECK_REVISIONS set, every layer as it was, to catch changes made
   * without a new revision. */
  int revision_frame_ = 0;
  uint64_t revision_python_runs_ = 0;
  uint64_t revision_unknown_changes_ = 0;
  /* Grows with every frame the UI may have edited something unsaid (a field dragged, a modal, a click): with Python
   * runs, undo steps and operators run, the stamp of edits animation_sync_playing keeps what it found by. */
  uint64_t ui_edits_ = 0;
  void revisions_check_frame(bool after_ui);
  void revisions_verify();
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
  /* The last render: Save As (a file dialog) and Copy Image (to the clipboard). */
  void render_save_as();
  void render_copy();
  void draw_unsaved_changes_prompt();
  void draw_asset_library_prompts();
  void draw_preferences();

  Document doc_;
  UndoStack undo_;
  WindowManager wm_;
  Context ctx_;

  std::vector<Workspace> workspaces_;
  /* The workspace drawn last frame: another one now was just gone to, and picks its tool. -1 at launch, which picks
   * none. */
  int workspace_shown_ = -1;
  int next_area_id_ = 1;
  bool show_about_ = false;
  /* A number field's scrub has the mouse grabbed (relative mouse mode). */
  bool number_grab_ = false;
  /* Keys pressed while a click is still on its way to the tools (ImGui hands a click's press and release over one
   * frame each), held until it has arrived: G right after clicking a point moves that point, as its selection is
   * there by then. */
  std::vector<SDL_Event> held_keys_;
  bool click_arriving() const;
  bool show_preferences_ = false;
  /* The asset library was set up while the app ran: offer Moho's brushes if it has none. */
  /* The last render (Render Frame), shown in the Render Result window. */
  std::unique_ptr<SceneRenderer> render_renderer_;
  /* oanim.wm.render_png's: a renderer of its own, so the Render Result window keeps its picture. */
  std::unique_ptr<SceneRenderer> file_renderer_;
  void render_to_file();
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
