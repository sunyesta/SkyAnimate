#include "wm/app.hh"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"

#include "assets/asset_library.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
#include "editors/brush_settings.hh"
#include "editors/ui.hh"
#include "ops/builtin_ops.hh"
#include "ops/edit_ops.hh"
#include "ops/file_ops.hh"
#include "ops/operator.hh"
#include "python/py_runtime.hh"
#include "ui/theme.hh"
#include "wm/event_system.hh"
#include "wm/file_dialog.hh"
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
  /* Tests run without the user's preferences (and never change them). */
  preferences_set_persistent(!options.background);
  preferences_load();
  ctx_.document = &doc_;
  ctx_.undo = &undo_;
  ctx_.wm = &wm_;
  /* A new document starts with one vector layer to draw on, as in Moho. */
  document_add_layer(doc_, LayerType::Vector, "Layer 1");
  undo_.reset(doc_);
  wm_.saved_change = wm_.autosaved_change = undo_.change_count();

  builtin_operators_register();
  layer_operators_register();
  vector_operators_register();
  asset_operators_register();
  file_operators_register();

  /* The file opens first, so scripts given with it work on it, as in Blender. */
  bool ok = options.open_file.empty() || document_open(ctx_, options.open_file);

  /* Background mode needs no video, but SDL_GetBasePath works either way. */
  ok = python::init(scripts_dir_find(), &ctx_) && ok;
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
  autosave_quit(ctx_);
  window_exit();
  python::shutdown();
  return 0;
}

bool App::window_init()
{
  /* The first click on the window while another app is focused still counts, as in Blender. */
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
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

  workspaces_.push_back(workspace_new("Layout", WorkspaceKind::Layout));
  workspaces_.push_back(workspace_new("Scripting", WorkspaceKind::Scripting));
  wm_.workspace_count = int(workspaces_.size());

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
    file_dialog_process();
    autosave_update(ctx_, SDL_GetTicksNS());
    update_window_title();

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
    ui_eyedropper_sample(height);
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
      /* Through the operator, which asks about unsaved changes first. */
      operator_call(ctx_, "wm.quit");
      break;
    case SDL_EVENT_DROP_FILE:
      if (event.drop.data) {
        operator_call(ctx_, "wm.open_mainfile", {{"filepath", event.drop.data}});
      }
      break;
    case SDL_EVENT_PEN_AXIS:
      if (event.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        wm_.pen_pressure = event.paxis.value;
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      /* A real mouse draws at full pressure; pen-made mouse events keep the pen's. */
      if (event.motion.which != SDL_PEN_MOUSEID) {
        wm_.pen_pressure = 1.0f;
      }
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (event.window.windowID == SDL_GetWindowID(window_)) {
        operator_call(ctx_, "wm.quit");
      }
      break;
    case SDL_EVENT_KEY_DOWN: {
      /* While typing in a text field, or with the mouse over the text editor or console, only
       * that editor's keymap applies (Alt P in the text editor), so arrows, Ctrl Z and the like
       * stay with the text, as in Blender. */
      const bool typing = ImGui::GetIO().WantTextInput || ctx_.area_type == "Text Editor" ||
                          ctx_.area_type == "Console";
      /* A running G/R/S takes the keys itself (X, Y, numbers, Enter, Esc), and a modal
       * prompt (unsaved changes) keeps shortcuts from editing behind it. */
      const SDL_Keymod mod = event.key.mod;
      const KeyEvent key{key_type_from_sdl(event.key.key), (mod & SDL_KMOD_CTRL) != 0,
                         (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_ALT) != 0};
      if (wm_.modal_running) {
        wm_.modal_keys.push_back({key.type, key.ctrl, key.shift, key.alt});
        break;
      }
      if (ImGui::GetTopMostPopupModal()) {
        break;
      }
      if (!key.type.empty()) {
        wm_handle_key_event(ctx_, key, typing);
        update_window_title();
      }
      break;
    }
    default:
      break;
  }
}

/* "SkyAnimate - walk.sky * (D:/Anim/walk.sky)", with * while there are unsaved changes. */
void App::update_window_title()
{
  if (!window_) {
    return;
  }
  std::string title = "SkyAnimate - ";
  if (wm_.filepath.empty()) {
    title += doc_.name;
  }
  else {
    const std::u8string name = std::filesystem::path(std::u8string(wm_.filepath.begin(), wm_.filepath.end())).filename().u8string();
    title += std::string(name.begin(), name.end());
  }
  if (document_is_dirty(ctx_)) {
    title += " *";
  }
  if (!wm_.filepath.empty()) {
    title += " (" + wm_.filepath + ")";
  }
  if (title != window_title_) {
    window_title_ = title;
    SDL_SetWindowTitle(window_, title.c_str());
  }
}

void App::build_default_layout(Workspace &workspace, ImGuiID dockspace_id)
{
  ImGui::DockBuilderRemoveNode(dockspace_id);
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

  /* Docking node for each editor type; types without one go to the main node. */
  ImGuiID main_id = dockspace_id;
  ImGuiID node_for[std::size(EDITOR_TYPES)] = {};
  const auto node_of = [&](EditorType type) -> ImGuiID & { return node_for[int(type)]; };

  if (workspace.kind == WorkspaceKind::Layout) {
    /* Blender's Layout: viewport in the middle, properties on the right, timeline along the
     * bottom with the Info log beside it. */
    ImGuiID bottom = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Down, 0.22f, nullptr, &main_id);
    /* Layers above Properties on the right, like Blender's outliner above its properties. */
    ImGuiID right = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, 0.26f, nullptr, &main_id);
    node_of(EditorType::Layers) = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.4f, nullptr, &right);
    node_of(EditorType::Properties) = right;
    node_of(EditorType::Info) = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.35f, nullptr, &bottom);
    node_of(EditorType::Timeline) = bottom;
    node_of(EditorType::Viewport) = main_id;
  }
  else {
    /* Blender's Scripting: viewport, console and info down the left, the text editor in the
     * middle, properties on the right. */
    node_of(EditorType::Properties) = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, 0.2f, nullptr,
                                                                  &main_id);
    ImGuiID left = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Left, 0.42f, nullptr, &main_id);
    node_of(EditorType::Info) = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.2f, nullptr, &left);
    node_of(EditorType::Console) = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, &left);
    node_of(EditorType::Viewport) = left;
    node_of(EditorType::TextEditor) = main_id;
  }

  for (const Area &area : workspace.areas) {
    const ImGuiID node = node_of(area.type);
    ImGui::DockBuilderDockWindow(area.window_name().c_str(), node ? node : main_id);
  }
  ImGui::DockBuilderFinish(dockspace_id);
}

/* Docking IDs that don't depend on ImGui's window ID stack, so a workspace's dockspace can be
 * kept alive from inside another window while it isn't shown. */
static ImGuiID workspace_dockspace_id(const Workspace &workspace)
{
  return ImHashStr(("SkyAnimate.Dockspace." + workspace.name).c_str());
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

/* SkyAnimate's mark at the left of the top bar, where Blender shows its logo: a small cloud
 * on a sky-blue disc. */
static void app_icon_draw()
{
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(h, h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
  const float r = h * 0.4f;
  dl->AddCircleFilled(c, r, IM_COL32(0x47, 0x8f, 0xd6, 255));
  const ImU32 white = IM_COL32(0xff, 0xff, 0xff, 255);
  dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y + r * 0.12f), r * 0.28f, white);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.05f, c.y - r * 0.08f), r * 0.36f, white);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.38f, c.y + r * 0.16f), r * 0.24f, white);
  dl->AddRectFilled(ImVec2(c.x - r * 0.32f, c.y + r * 0.12f), ImVec2(c.x + r * 0.38f, c.y + r * 0.4f), white);
}

/* A workspace tab like Blender's: plain text, the active one on a lighter rounded block.
 * Returns true when clicked. */
static bool workspace_tab(const char *label, bool active, const char *tooltip = nullptr)
{
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImVec2 text_size = ImGui::CalcTextSize(label);
  const ImVec2 size(text_size.x + style.FramePadding.x * 2.0f, ImGui::GetFrameHeight());
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(label, size);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (active || hovered) {
    const ImU32 back = active ? IM_COL32(0x30, 0x30, 0x30, 255) : IM_COL32(0x26, 0x26, 0x26, 255);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), back, style.FrameRounding);
  }
  const ImU32 text = (active || hovered) ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xb3, 0xb3, 0xb3, 255);
  dl->AddText(ImVec2(p.x + style.FramePadding.x, p.y + (size.y - text_size.y) * 0.5f), text, label);
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return clicked;
}

Workspace App::workspace_new(const std::string &name, WorkspaceKind kind)
{
  const auto area = [this](EditorType type) { return Area{next_area_id_++, type}; };
  if (kind == WorkspaceKind::Scripting) {
    return {name, kind,
            {area(EditorType::Viewport), area(EditorType::Console), area(EditorType::Info),
             area(EditorType::TextEditor), area(EditorType::Properties)}};
  }
  return {name, kind,
          {area(EditorType::Viewport), area(EditorType::Layers), area(EditorType::Properties),
           area(EditorType::Timeline), area(EditorType::Info)}};
}

std::string App::workspace_unique_name(const std::string &base) const
{
  const auto taken = [&](const std::string &name) {
    return std::any_of(workspaces_.begin(), workspaces_.end(),
                       [&](const Workspace &ws) { return ws.name == name; });
  };
  if (!taken(base)) {
    return base;
  }
  for (int i = 1;; i++) {
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), ".%03d", i);
    if (!taken(base + suffix)) {
      return base + suffix;
    }
  }
}

void App::draw_workspace_tabs()
{
  /* Changes wait until after the loop so the list isn't modified while drawing it. */
  int duplicate_index = -1, delete_index = -1;
  std::optional<WorkspaceKind> add_kind;

  for (int i = 0; i < int(workspaces_.size()); i++) {
    ImGui::PushID(i);
    if (workspace_tab(workspaces_[i].name.c_str(), i == wm_.workspace_index)) {
      wm_.workspace_index = i;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      ImGui::OpenPopup("##workspace_menu");
    }
    if (ImGui::BeginPopup("##workspace_menu")) {
      ImGui::TextDisabled("%s", workspaces_[i].name.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem("Duplicate")) {
        duplicate_index = i;
      }
      if (ImGui::MenuItem("Delete", nullptr, false, workspaces_.size() > 1)) {
        delete_index = i;
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SameLine(0.0f, 2.0f);
  }

  if (workspace_tab("+", false, "Add Workspace")) {
    ImGui::OpenPopup("##add_workspace");
  }
  if (ImGui::BeginPopup("##add_workspace")) {
    ImGui::TextDisabled("General");
    ImGui::Separator();
    if (ImGui::MenuItem("Layout")) {
      add_kind = WorkspaceKind::Layout;
    }
    if (ImGui::MenuItem("Scripting")) {
      add_kind = WorkspaceKind::Scripting;
    }
    ImGui::EndPopup();
  }

  if (add_kind) {
    const char *base = *add_kind == WorkspaceKind::Scripting ? "Scripting" : "Layout";
    workspaces_.push_back(workspace_new(workspace_unique_name(base), *add_kind));
    wm_.workspace_index = int(workspaces_.size()) - 1;
  }
  if (duplicate_index >= 0) {
    const Workspace &source = workspaces_[duplicate_index];
    Workspace copy = workspace_new(workspace_unique_name(source.name), source.kind);
    workspaces_.insert(workspaces_.begin() + duplicate_index + 1, std::move(copy));
    wm_.workspace_index = duplicate_index + 1;
  }
  if (delete_index >= 0 && workspaces_.size() > 1) {
    workspaces_.erase(workspaces_.begin() + delete_index);
    if (wm_.workspace_index >= delete_index) {
      wm_.workspace_index = std::max(0, wm_.workspace_index - 1);
    }
  }
  wm_.workspace_count = int(workspaces_.size());
}

void App::areas_split_requested(Workspace &workspace, ImGuiID dockspace_id)
{
  /* Indexing, not references: adding an area may move the others in memory. */
  for (size_t i = 0; i < workspace.areas.size(); i++) {
    if (workspace.areas[i].split_request == AreaSplit::None) {
      continue;
    }
    const AreaSplit split = workspace.areas[i].split_request;
    workspace.areas[i].split_request = AreaSplit::None;
    /* As in Blender, the new area starts as a copy of the one that was split. */
    const Area added{next_area_id_++, workspace.areas[i].type};
    const ImGuiWindow *window = ImGui::FindWindowByName(workspace.areas[i].window_name().c_str());
    if (window && window->DockNode) {
      ImGuiID new_node = 0, old_node = 0;
      ImGui::DockBuilderSplitNode(window->DockNode->ID, split == AreaSplit::Vertical ? ImGuiDir_Right : ImGuiDir_Down,
                                  0.5f, &new_node, &old_node);
      ImGui::DockBuilderDockWindow(added.window_name().c_str(), new_node);
      ImGui::DockBuilderFinish(dockspace_id);
    }
    workspace.areas.push_back(added);
  }
}

void App::draw_top_bar()
{
  /* Blender's top bar is darker and a little taller than an area header. */
  ImGui::PushStyleColor(ImGuiCol_MenuBarBg, IM_COL32(0x18, 0x18, 0x18, 255));
  const ImGuiStyle &style = ImGui::GetStyle();
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 1.4f, style.FramePadding.y * 1.6f));
  const bool open = ImGui::BeginMainMenuBar();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (!open) {
    return;
  }

  app_icon_draw();
  ImGui::SameLine();

  draw_file_menu();

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
    if (ImGui::MenuItem("Preferences...")) {
      show_preferences_ = true;
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Render")) {
    menu_operator(ctx_, "Render Frame", "render.render");
    if (ImGui::MenuItem("View Render", nullptr, show_render_, render_texture_ != 0)) {
      show_render_ = !show_render_;
    }
    ImGui::Separator();
    menu_not_yet("Render Animation", "Ctrl F12", "milestone 3");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Window")) {
    Workspace &workspace = workspaces_[wm_.workspace_index];
    if (ImGui::BeginMenu("New Area")) {
      for (EditorType type : EDITOR_TYPES) {
        if (ImGui::MenuItem(editor_type_label(type))) {
          workspace.areas.push_back({next_area_id_++, type});
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset Layout")) {
      workspace.layout_built = false;
    }
    ImGui::Separator();
    menu_operator(ctx_, "Next Workspace", "screen.workspace_cycle_next");
    menu_operator(ctx_, "Previous Workspace", "screen.workspace_cycle_prev");
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

  /* A divider, then the workspace tabs, as in Blender. */
  {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + h * 0.35f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + h * 0.2f), ImVec2(x, p.y + h * 0.8f),
                                        IM_COL32(0x3d, 0x3d, 0x3d, 255));
    ImGui::Dummy(ImVec2(h * 0.7f, h));
    ImGui::SameLine();
  }
  draw_workspace_tabs();

  /* Hidden workspaces keep their docking layout alive while another one is shown. */
  for (int i = 0; i < int(workspaces_.size()); i++) {
    if (i != wm_.workspace_index && workspaces_[i].layout_built) {
      ImGui::DockSpace(workspace_dockspace_id(workspaces_[i]), ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
    }
  }

  ImGui::EndMainMenuBar();
}

/* The file name, with the folder in a tooltip; the ## part keeps same-named files apart. */
static bool menu_file_item(const std::string &path, const std::string &label)
{
  const bool clicked = ImGui::MenuItem((label + "##" + path).c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", path.c_str());
  }
  return clicked;
}

void App::draw_file_menu()
{
  if (!ImGui::BeginMenu("File")) {
    return;
  }
  menu_operator(ctx_, "New", "wm.read_homefile");
  menu_operator(ctx_, "Open...", "wm.open_mainfile");
  if (ImGui::BeginMenu("Open Recent")) {
    const std::vector<std::string> files = recent_files();
    if (files.empty()) {
      ImGui::MenuItem("No recent files", nullptr, false, false);
    }
    for (const std::string &file : files) {
      const std::u8string name = std::filesystem::path(std::u8string(file.begin(), file.end())).filename().u8string();
      if (menu_file_item(file, std::string(name.begin(), name.end()))) {
        operator_call(ctx_, "wm.open_mainfile", {{"filepath", file}});
      }
    }
    ImGui::EndMenu();
  }
  menu_operator(ctx_, "Revert", "wm.revert_mainfile");
  if (ImGui::BeginMenu("Recover")) {
    menu_operator(ctx_, "Last Session", "wm.recover_last_session");
    if (ImGui::BeginMenu("Auto Save")) {
      const std::vector<RecoverableFile> files = autosave_files();
      if (files.empty()) {
        ImGui::MenuItem("No autosaves", nullptr, false, false);
      }
      for (const RecoverableFile &file : files) {
        if (menu_file_item(file.path, file.label)) {
          operator_call(ctx_, "wm.recover_auto_save", {{"filepath", file.path}});
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  menu_operator(ctx_, "Save", "wm.save_mainfile");
  menu_operator(ctx_, "Save As...", "wm.save_as_mainfile");
  menu_operator(ctx_, "Save Copy...", "wm.save_copy");
  menu_operator(ctx_, "Save Incremental", "wm.save_incremental");
  ImGui::Separator();
  menu_operator(ctx_, "Quit", "wm.quit");
  ImGui::EndMenu();
}

/* Asks before New, Open, Recover or Quit throw away unsaved changes, as Blender does. */
void App::draw_unsaved_changes_prompt()
{
  const char *popup = "Unsaved Changes";
  if (!wm_.confirm_operator.empty() && !ImGui::IsPopupOpen(popup)) {
    ImGui::OpenPopup(popup);
  }
  if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  const std::string idname = wm_.confirm_operator;
  OpArgs args = wm_.confirm_args;
  args["confirmed"] = "1";
  const bool revert = idname == "wm.revert_mainfile";
  const char *when = idname == "wm.quit"            ? "quitting"
                     : idname == "wm.read_homefile" ? "starting a new document"
                     : idname == "wm.open_mainfile" ? "opening another file"
                                                    : "recovering";
  if (revert) {
    ImGui::TextUnformatted("Revert to the saved file? Unsaved changes will be lost.");
  }
  else {
    ImGui::Text("Save changes before %s?", when);
  }
  ImGui::Spacing();

  enum { None, Save, Discard, Cancel } choice = None;
  if (!revert && ImGui::Button("Save")) {
    choice = Save;
  }
  if (!revert) {
    ImGui::SameLine();
  }
  if (ImGui::Button(revert ? "Revert" : "Don't Save")) {
    choice = Discard;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    choice = Cancel;
  }
  if (choice != None) {
    wm_.confirm_operator.clear();
    wm_.confirm_args.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();

  if (choice == Save) {
    document_save_then(ctx_, [this, idname, args] { operator_call(ctx_, idname, args); });
  }
  else if (choice == Discard) {
    operator_call(ctx_, idname, args);
  }
}

/* Without an asset library the app asks for a folder to set one up in; once there is one with no
 * brushes, it offers to import Moho's. */
void App::draw_asset_library_prompts()
{
  const bool ready = asset_library_ready();
  if (ready && !library_was_ready_ && brushes().empty() && !moho_brush_dirs().empty()) {
    offer_moho_import_ = true;
  }
  library_was_ready_ = ready;

  const char *setup = "Set Up Asset Library";
  if (!ready && !ImGui::IsPopupOpen(setup)) {
    ImGui::OpenPopup(setup);
  }
  if (ImGui::BeginPopupModal(setup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextUnformatted("SkyAnimate keeps its brushes in an asset library folder, shared by all your documents.");
    if (preferences().asset_library.empty()) {
      ImGui::TextUnformatted("Pick a folder where SkyAnimate can set one up.");
    }
    else {
      ImGui::Text("The asset library at %s can't be found.", preferences().asset_library.c_str());
      ImGui::TextUnformatted("Pick a folder for a new one, or find the old one.");
    }
    ImGui::Spacing();
    if (ImGui::Button("Choose Folder...")) {
      operator_call(ctx_, "preferences.asset_library_set");
    }
    if (ready) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  const char *import = "Import Moho Brushes";
  if (offer_moho_import_ && !ImGui::IsPopupOpen(import) && !ImGui::IsPopupOpen(setup)) {
    ImGui::OpenPopup(import);
    offer_moho_import_ = false;
  }
  if (ImGui::BeginPopupModal(import, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextUnformatted("Moho is installed on this computer. Copy its brushes into the asset library?");
    for (const std::string &dir : moho_brush_dirs()) {
      ImGui::BulletText("%s", dir.c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Import")) {
      operator_call(ctx_, "preferences.import_moho_brushes");
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not Now")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

/* Edit > Preferences: where the asset library is, and bringing in Moho's brushes. */
void App::draw_preferences()
{
  if (!show_preferences_) {
    return;
  }
  const ImGuiViewport *main = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(ImVec2(560, 260), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Preferences", &show_preferences_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
    ImGui::End();
    return;
  }
  ImGui::SeparatorText("File Paths");
  ImGui::TextUnformatted("Asset Library");
  ImGui::SameLine(130);
  const std::string &library = preferences().asset_library;
  ImGui::TextDisabled("%s", library.empty() ? "(not set)" : library.c_str());
  ImGui::SetCursorPosX(130);
  if (ImGui::Button("Change...")) {
    operator_call(ctx_, "preferences.asset_library_set");
  }

  ImGui::SeparatorText("Brushes");
  ImGui::Text("%d brushes in the asset library", int(brushes().size()));
  ImGui::BeginDisabled(!asset_library_ready());
  if (ImGui::Button("Import Moho Brushes")) {
    operator_call(ctx_, "preferences.import_moho_brushes");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Copy the brushes of Moho's install and your Moho custom content into the asset library");
  }
  ImGui::SameLine();
  if (ImGui::Button("Import From Folder...")) {
    Context *ctx = &ctx_;
    file_dialog_folder([ctx](const std::string &path) {
      operator_call(*ctx, "preferences.import_moho_brushes", {{"directory", path}});
    });
  }
  ImGui::SameLine();
  if (ImGui::Button("Reload")) {
    operator_call(ctx_, "preferences.brushes_reload");
  }
  ImGui::EndDisabled();
  ImGui::End();
}

/* Renders the current frame through the camera: the document's size, background, render
 * quality and render samples. */
void App::render_frame()
{
  wm_.render_requested = false;
  if (!render_renderer_) {
    render_renderer_ = std::make_unique<SceneRenderer>();
  }
  const int width = std::max(doc_.width, 1), height = std::max(doc_.height, 1);
  /* The camera frame is 2 document units tall, centred on the origin. */
  const float scale = float(height) * 0.5f;
  const Transform2D document_to_pixels{scale, 0, 0, scale, float(width) * 0.5f, float(height) * 0.5f};
  const uint64_t start = SDL_GetTicksNS();
  render_texture_ = render_renderer_->render(doc_, width, height, document_to_pixels, doc_.render_quality,
                                             doc_.render_samples, &doc_.background);
  glFinish();
  render_seconds_ = double(SDL_GetTicksNS() - start) * 1e-9;
  render_frame_ = doc_.frame_current;
  render_width_ = width;
  render_height_ = height;
  show_render_ = render_texture_ != 0;
  char text[128];
  std::snprintf(text, sizeof(text), "Rendered frame %d (%d x %d, %d samples) in %.2f s", render_frame_, width, height,
                render_renderer_->samples(), render_seconds_);
  report(ReportType::Info, text);
}

/* Saves the last render as a PNG. */
static void render_save_png(SceneRenderer &renderer, const std::string &path)
{
  std::vector<uint8_t> pixels;
  int width = 0, height = 0;
  if (!renderer.read_pixels(pixels, &width, &height)) {
    return;
  }
  SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels.data(), width * 4);
  const bool saved = surface && SDL_SavePNG(surface, path.c_str());
  SDL_DestroySurface(surface);
  report(saved ? ReportType::Info : ReportType::Error,
         saved ? "Saved render to " + path : "Could not save " + path + ": " + SDL_GetError());
}

/* Blender's render window: the last render, fitted to the window, with Save As. */
void App::draw_render_result()
{
  if (!show_render_ || !render_texture_) {
    return;
  }
  const ImGuiViewport *main = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(ImVec2(main->Size.x * 0.6f, main->Size.y * 0.6f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Render Result", &show_render_, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  if (ImGui::Button("Save As...")) {
    char name[64];
    std::snprintf(name, sizeof(name), "_%04d.png", render_frame_);
    SceneRenderer *renderer = render_renderer_.get();
    file_dialog_save("PNG images", "png", doc_.name + name, [renderer](const std::string &path) {
      std::string file = path;
      if (file.size() < 4 || file.compare(file.size() - 4, 4, ".png") != 0) {
        file += ".png";
      }
      render_save_png(*renderer, file);
    });
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Frame %d  |  %d x %d  |  %d samples  |  %.2f s", render_frame_, render_width_, render_height_,
                      render_renderer_->samples(), render_seconds_);
  /* Fit the image to the window, keeping its shape. */
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float scale = std::min(avail.x / float(render_width_), avail.y / float(render_height_));
  const ImVec2 size(float(render_width_) * scale, float(render_height_) * scale);
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const ImVec2 at(cursor.x + (avail.x - size.x) * 0.5f, cursor.y + (avail.y - size.y) * 0.5f);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddImage(ImTextureRef(ImTextureID(intptr_t(render_texture_))), at, ImVec2(at.x + size.x, at.y + size.y),
               ImVec2(0, 1), ImVec2(1, 0));
  ImGui::Dummy(avail);
  ImGui::End();
}

void App::draw_ui()
{
  if (wm_.render_requested) {
    render_frame();
  }
  draw_top_bar();

  wm_.workspace_index = std::clamp(wm_.workspace_index, 0, int(workspaces_.size()) - 1);
  Workspace &workspace = workspaces_[wm_.workspace_index];
  const ImGuiID dockspace_id = workspace_dockspace_id(workspace);
  ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport());
  if (!workspace.layout_built) {
    build_default_layout(workspace, dockspace_id);
    workspace.layout_built = true;
  }
  areas_split_requested(workspace, dockspace_id);

  std::string hovered_area = "Window";
  wm_.mouse_in_viewport = false;
  for (Area &area : workspace.areas) {
    if (area_draw(area, ctx_)) {
      hovered_area = editor_keymap_name(area.type);
    }
  }
  std::erase_if(workspace.areas, [](const Area &area) { return !area.open; });
  ctx_.area_type = hovered_area;
  /* A G/R/S/E request no viewport picked up (the mouse wasn't over one) expires. */
  wm_.transform_request = 0;

  draw_unsaved_changes_prompt();
  draw_render_result();
  brush_settings_draw(ctx_);
  draw_preferences();
  draw_asset_library_prompts();
  ui_eyedropper_overlay();

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
