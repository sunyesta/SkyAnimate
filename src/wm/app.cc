#include "wm/app.hh"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"

#include "core/log.hh"
#include "ops/builtin_ops.hh"
#include "ops/operator.hh"
#include "python/py_runtime.hh"
#include "ui/theme.hh"
#include "wm/event_system.hh"
#include "wm/keymap.hh"

namespace sky {

static constexpr const char *SKY_VERSION = "0.1.0";

/* Scripts live next to the executable in a packaged build, and in the source tree
 * while developing so edits apply without rebuilding. */
static std::filesystem::path scripts_dir_find()
{
  if (const char *base = SDL_GetBasePath()) {
    std::filesystem::path packaged = std::filesystem::path(reinterpret_cast<const char8_t *>(base)) / "scripts";
    if (std::filesystem::exists(packaged / "modules")) {
      return packaged;
    }
  }
  return std::filesystem::path(reinterpret_cast<const char8_t *>(SKY_SCRIPTS_DIR));
}

/* SDL key names to Blender's event type names ("Space" -> "SPACE", "A" -> "A"). */
static std::string key_type_from_sdl(SDL_Keycode key)
{
  static const std::pair<const char *, const char *> renames[] = {
      {"Space", "SPACE"},       {"Escape", "ESC"},          {"Return", "RET"},         {"Delete", "DEL"},
      {"Backspace", "BACK_SPACE"}, {"Tab", "TAB"},           {"PageUp", "PAGE_UP"},     {"PageDown", "PAGE_DOWN"},
      {"Left", "LEFT_ARROW"},   {"Right", "RIGHT_ARROW"},   {"Up", "UP_ARROW"},        {"Down", "DOWN_ARROW"},
  };
  std::string name = SDL_GetKeyName(key);
  for (const auto &[sdl_name, blender_name] : renames) {
    if (name == sdl_name) {
      return blender_name;
    }
  }
  for (char &c : name) {
    c = (c == ' ') ? '_' : char(std::toupper(static_cast<unsigned char>(c)));
  }
  return name;
}

int App::run(const LaunchOptions &options)
{
  wm_.background = options.background;
  ctx_.document = &doc_;
  ctx_.undo = &undo_;
  ctx_.wm = &wm_;
  undo_.reset(doc_);

  builtin_operators_register();

  /* Background mode needs no video, but SDL_GetBasePath works either way. */
  bool ok = python::init(scripts_dir_find(), &ctx_);
  for (const std::string &file : options.python_files) {
    ok = python::run_file(std::filesystem::path(reinterpret_cast<const char8_t *>(file.c_str()))) && ok;
  }
  for (const std::string &expr : options.python_exprs) {
    ok = python::run_string(expr) && ok;
  }

  if (options.background) {
    python::shutdown();
    return ok ? 0 : 1;
  }

  if (!window_init()) {
    python::shutdown();
    return 1;
  }
  main_loop();
  window_exit();
  python::shutdown();
  return 0;
}

bool App::window_init()
{
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    report(ReportType::Error, std::string("Could not start SDL: ") + SDL_GetError());
    return false;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  window_ = SDL_CreateWindow("SkyAnimate", 1600, 900,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                 SDL_WINDOW_HIDDEN);
  if (!window_) {
    report(ReportType::Error, std::string("Could not create the window: ") + SDL_GetError());
    return false;
  }
  gl_context_ = SDL_GL_CreateContext(window_);
  if (!gl_context_) {
    report(ReportType::Error, std::string("Could not create an OpenGL 3.3 context: ") + SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(window_, gl_context_);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  /* The default layout is rebuilt on every launch until workspaces are saved in files. */
  io.IniFilename = nullptr;

  /* Start at the display's scale (150% on many Windows laptops). */
  wm_.ui_scale = std::clamp(SDL_GetWindowDisplayScale(window_), 0.5f, 3.0f);
  wm_.ui_scale_changed = true;
  theme_load_fonts();

  ImGui_ImplSDL3_InitForOpenGL(window_, gl_context_);
  ImGui_ImplOpenGL3_Init("#version 330");

  areas_ = {{next_area_id_++, EditorType::Viewport},
            {next_area_id_++, EditorType::Properties},
            {next_area_id_++, EditorType::Timeline},
            {next_area_id_++, EditorType::Info}};

  update_window_title();
  SDL_MaximizeWindow(window_);
  SDL_ShowWindow(window_);
  report(ReportType::Info, std::string("SkyAnimate ") + SKY_VERSION + " started");
  return true;
}

void App::window_exit()
{
  if (ImGui::GetCurrentContext()) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (gl_context_) {
    SDL_GL_DestroyContext(gl_context_);
  }
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  SDL_Quit();
}

void App::main_loop()
{
  while (!wm_.quit_requested) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      handle_event(event);
    }
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(10);
      continue;
    }

    playback_update();

    if (wm_.ui_scale_changed) {
      theme_apply_blender(wm_.ui_scale);
      wm_.ui_scale_changed = false;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();

    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.086f, 0.086f, 0.086f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);
  }
}

/* Advances the current frame in real time at the document's frame rate. When drawing falls
 * behind, frames are skipped so playback keeps the true speed. */
void App::playback_update()
{
  if (!wm_.playing) {
    wm_.play_last_ns = 0;
    return;
  }
  const uint64_t now = SDL_GetTicksNS();
  if (wm_.play_last_ns == 0) {
    wm_.play_last_ns = now;
    return;
  }
  wm_.play_frames_due += double(now - wm_.play_last_ns) * 1e-9 * double(doc_.fps);
  wm_.play_last_ns = now;

  const int frames = int(wm_.play_frames_due);
  if (frames == 0) {
    return;
  }
  wm_.play_frames_due -= frames;

  const int start = doc_.frame_start;
  const int end = std::max(doc_.frame_start, doc_.frame_end);
  const int length = end - start + 1;
  int frame = doc_.frame_current + (wm_.play_reverse ? -frames : frames);
  if (frame > end || frame < start) {
    if (wm_.play_loop) {
      frame = start + (((frame - start) % length) + length) % length;
    }
    else {
      frame = std::clamp(frame, start, end);
      wm_.playing = false;
    }
  }
  doc_.frame_current = frame;
}

void App::handle_event(const SDL_Event &event)
{
  switch (event.type) {
    case SDL_EVENT_QUIT:
      wm_.quit_requested = true;
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (event.window.windowID == SDL_GetWindowID(window_)) {
        wm_.quit_requested = true;
      }
      break;
    case SDL_EVENT_KEY_DOWN: {
      /* Shortcuts work everywhere except while typing in a text field, as in Blender. */
      if (ImGui::GetIO().WantTextInput) {
        break;
      }
      const SDL_Keymod mod = event.key.mod;
      const KeyEvent key{key_type_from_sdl(event.key.key), (mod & SDL_KMOD_CTRL) != 0,
                         (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_ALT) != 0};
      if (!key.type.empty()) {
        wm_handle_key_event(ctx_, key);
        update_window_title();
      }
      break;
    }
    default:
      break;
  }
}

void App::update_window_title()
{
  if (window_) {
    SDL_SetWindowTitle(window_, ("SkyAnimate - " + doc_.name).c_str());
  }
}

void App::build_default_layout(ImGuiID dockspace_id)
{
  /* Blender's Layout workspace: viewport in the middle, properties on the right,
   * timeline along the bottom (with the Info log beside it for now). */
  ImGui::DockBuilderRemoveNode(dockspace_id);
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

  ImGuiID main_id = dockspace_id;
  ImGuiID bottom_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Down, 0.22f, nullptr, &main_id);
  ImGuiID right_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, 0.26f, nullptr, &main_id);
  ImGuiID info_id = ImGui::DockBuilderSplitNode(bottom_id, ImGuiDir_Right, 0.35f, nullptr, &bottom_id);

  for (const Area &area : areas_) {
    ImGuiID target = main_id;
    switch (area.type) {
      case EditorType::Viewport:
        target = main_id;
        break;
      case EditorType::Properties:
        target = right_id;
        break;
      case EditorType::Timeline:
        target = bottom_id;
        break;
      case EditorType::Info:
        target = info_id;
        break;
    }
    ImGui::DockBuilderDockWindow(area.window_name().c_str(), target);
  }
  ImGui::DockBuilderFinish(dockspace_id);
}

/* Menu item that runs an operator, showing its shortcut and greyed out when its poll fails. */
static void menu_operator(Context &C, const char *label, const char *idname)
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return;
  }
  const std::string shortcut = keymaps().shortcut_for(idname);
  if (ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, operator_poll(C, *ot))) {
    operator_call(C, idname);
  }
  if (!ot->description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s\n\nPython: sky.ops.%s()", ot->description.c_str(), idname);
  }
}

static void menu_not_yet(const char *label, const char *shortcut, const char *milestone)
{
  ImGui::MenuItem(label, shortcut, false, false);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Arrives in %s", milestone);
  }
}

void App::draw_top_bar()
{
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  if (ImGui::BeginMenu("File")) {
    menu_not_yet("New", "Ctrl N", "milestone 2");
    menu_not_yet("Open...", "Ctrl O", "milestone 2");
    menu_not_yet("Save", "Ctrl S", "milestone 2");
    ImGui::Separator();
    menu_operator(ctx_, "Quit", "wm.quit");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    menu_operator(ctx_, "Undo", "ed.undo");
    menu_operator(ctx_, "Redo", "ed.redo");
    if (ImGui::BeginMenu("Undo History")) {
      for (size_t i = 0; i < undo_.size(); i++) {
        ImGui::MenuItem(undo_.step_name(i).c_str(), nullptr, i == undo_.active(), false);
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    menu_not_yet("Operator Search...", "F3", "milestone 5");
    menu_not_yet("Preferences...", nullptr, "milestone 5");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Render")) {
    menu_not_yet("Render Frame", "F12", "milestone 3");
    menu_not_yet("Render Animation", "Ctrl F12", "milestone 3");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Window")) {
    if (ImGui::BeginMenu("New Area")) {
      for (EditorType type : EDITOR_TYPES) {
        if (ImGui::MenuItem(editor_type_label(type))) {
          areas_.push_back({next_area_id_++, type});
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset Layout")) {
      layout_built_ = false;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Interface Scale: %d%%", int(wm_.ui_scale * 100.0f + 0.5f));
    menu_operator(ctx_, "Increase Interface Scale", "wm.ui_scale_increase");
    menu_operator(ctx_, "Decrease Interface Scale", "wm.ui_scale_decrease");
    menu_operator(ctx_, "Reset Interface Scale", "wm.ui_scale_reset");
    ImGui::Separator();
    const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (ImGui::MenuItem("Toggle Fullscreen", nullptr, fullscreen)) {
      SDL_SetWindowFullscreen(window_, !fullscreen);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    menu_not_yet("Manual", nullptr, "a later milestone");
    menu_not_yet("Python API Reference", nullptr, "milestone 5");
    ImGui::Separator();
    if (ImGui::MenuItem("About SkyAnimate")) {
      show_about_ = true;
    }
    ImGui::EndMenu();
  }

  /* Workspace tabs. Only Layout exists until milestone 5. */
  ImGui::Dummy(ImVec2(ImGui::GetFontSize(), 0));
  ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
  ImGui::Selectable("Layout", true, ImGuiSelectableFlags_None, ImVec2(ImGui::CalcTextSize("Layout").x, 0));
  ImGui::PopStyleColor();

  ImGui::EndMainMenuBar();
}

void App::draw_ui()
{
  draw_top_bar();

  const ImGuiID dockspace_id = ImGui::GetID("SkyAnimateDockspace");
  ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport());
  if (!layout_built_) {
    build_default_layout(dockspace_id);
    layout_built_ = true;
  }

  std::string hovered_area = "Window";
  for (Area &area : areas_) {
    if (area_draw(area, ctx_)) {
      hovered_area = editor_keymap_name(area.type);
    }
  }
  std::erase_if(areas_, [](const Area &area) { return !area.open; });
  ctx_.area_type = hovered_area;

  if (show_about_) {
    ImGui::OpenPopup("About SkyAnimate");
    show_about_ = false;
  }
  if (ImGui::BeginPopupModal("About SkyAnimate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("SkyAnimate %s", SKY_VERSION);
    ImGui::TextDisabled("2D animation inspired by Moho and Blender");
    ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace sky
