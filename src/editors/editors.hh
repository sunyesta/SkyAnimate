#pragma once

#include <map>

/* Editor types and areas. An area is one dockable panel of the window showing one
 * editor, like Blender's ScrArea + SpaceType. Splitting and joining areas uses ImGui
 * docking: drag an area's tab onto another area. */

#include <cstdint>
#include <string>
#include <vector>

namespace oanim {

struct Context;

enum class EditorType { Viewport, Properties, Layers, Timeline, TextEditor, Console, Info };

inline constexpr EditorType EDITOR_TYPES[] = {EditorType::Viewport,   EditorType::Properties, EditorType::Layers,
                                              EditorType::Timeline,   EditorType::TextEditor, EditorType::Console,
                                              EditorType::Info};

const char *editor_type_label(EditorType type);
/* Name of the editor's keymap, tried before "Window" while the mouse is over it. */
const char *editor_keymap_name(EditorType type);

enum class AreaSplit { None, Vertical, Horizontal };

struct Area {
  int id = 0;
  EditorType type = EditorType::Viewport;
  bool open = true;
  /* Set from the area menu; the app splits the area at the start of the next frame. */
  AreaSplit split_request = AreaSplit::None;

  /* Timeline view: the frame range visible across the area. */
  float view_start = 0.0f;
  float view_end = 250.0f;
  bool view_initialized = false;
  /* Timeline: how far the channel rows are scrolled, in pixels (which folding rows are open is the
   * project's: Document::ui_open). */
  float channel_scroll = 0.0f;
  /* View > Group by Kind (the default), else by Owner. */
  bool timeline_by_kind = true;
  /* The names column's width, dragged by its edge (0: the default). */
  float timeline_names_w = 0.0f;

  /* Viewport view: the document point at the centre and pixels per document unit (the project
   * keeps a copy, Document::view, saved with it; `view_serial` is the project's view it took). */
  uint64_t view_serial = 0;
  float view_center_x = 0.0f;
  float view_center_y = 0.0f;
  float view_zoom = 0.0f;
  /* Until the user zooms or pans, the view keeps the camera framed as the area changes size
   * (the docking layout only settles a frame or two after startup). */
  bool view_navigated = false;
  float view_fit_width = 0.0f;
  float view_fit_height = 0.0f;

  /* Properties editor tab. */
  int properties_tab = 0;

  /* Layers editor: the layer being renamed, or 0. */
  uint64_t renaming_layer = 0;
  /* Layers editor: the last Show Active it took (WindowManager::layers_show_active_serial), and the
   * row it scrolled to, flashing since `show_active_time`. */
  uint64_t show_active_seen = 0;
  uint64_t show_active_layer = 0;
  double show_active_time = -10.0;
  /* Layers editor: list the drawing layers in this frame's draw order (top first), to reorder by
   * dragging, instead of the tree. */
  bool layers_draw_order_view = false;

  /* Text editor. */
  bool show_line_numbers = true;

  /* Console: the line being typed, the history entry shown, and the scrollback seen. */
  std::string console_input;
  int console_history_pos = -1;
  uint64_t console_seen_version = 0;

  /* ImGui window name: the label shows on the tab; "###area<id>" keeps the window's
   * identity when the editor type changes. */
  std::string window_name() const;
};

/* Draws the area as a dockable window. Returns true while the mouse is over it. */
bool area_draw(Area &area, Context &C);
/* The viewport's own operators (mesh.transform_selected). */
void viewport_operators_register();

/* Starts every area header: a grip to drag the area elsewhere (right-click for the area
 * menu), then the editor-type dropdown. */
void area_editor_type_menu(Area &area);

/* Viewport, in viewport.cc. */
void viewport_draw(Area &area, Context &C);
/* Properties editor, in properties.cc. */
void properties_draw(Area &area, Context &C);
/* Layers editor, in layers_editor.cc. */
void layers_draw(Area &area, Context &C);
/* Timeline editor, in timeline.cc. */
void timeline_draw(Area &area, Context &C);
/* The timeline's channel rows as a timeline with every row open lists them (for tests). */
struct TimelineRowInfo {
  int depth = 0;
  std::string name;
  /* In the Pinned or Selected sections. */
  bool red = false;
  /* A pinned property's row (with its pin). */
  bool pinned = false;
  /* A copy's (reference layer's) channel changed from its original (teal). */
  bool changed = false;
};
std::vector<TimelineRowInfo> timeline_rows_list(Context &C, bool by_kind);
/* Text editor, in text_editor.cc. */
void text_editor_draw(Area &area, Context &C);
/* Python console, in console.cc. */
void console_draw(Area &area, Context &C);

/* A header button that opens a popup menu of the same name, styled like Blender's
 * flat header menus. Returns true while the menu is open; then call ImGui::EndPopup(). */
bool header_menu(const char *label);

}  // namespace oanim
