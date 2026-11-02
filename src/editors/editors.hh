#pragma once

#include <map>

/* Editor types and areas. An area is one dockable panel of the window showing one
 * editor, like Blender's ScrArea + SpaceType. Splitting and joining areas uses ImGui
 * docking: drag an area's tab onto another area. */

#include <cstdint>
#include <string>
#include <vector>

#include "anim/sculpt.hh"
#include "editors/tool_shelf.hh"

struct ImDrawList;
struct ImVec2;

namespace oanim {

struct Context;
struct Document;

enum class EditorType { Viewport, Properties, Layers, Timeline, TextEditor, Console, Info, AssetShelf };

inline constexpr EditorType EDITOR_TYPES[] = {EditorType::Viewport,   EditorType::Properties, EditorType::Layers,
                                              EditorType::Timeline,   EditorType::AssetShelf, EditorType::TextEditor,
                                              EditorType::Console,    EditorType::Info};

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
  /* Frames it still scrolls to that row: groups Show Active opened only make the list taller once drawn, and a scroll
   * can't go past the list's height of the frame before, so it goes on till the list has its full height. */
  int show_active_frames = 0;
  /* That scroll only brings the row into sight (Auto Focus): none when it is already in sight. */
  bool show_active_only_if_hidden = false;
  /* Layers editor, Auto Focus: the picked layers it last folded down to, active first. */
  std::vector<uint64_t> auto_focus_picked;
  /* Layers editor: list the drawing layers in this frame's draw order (top first), to reorder by
   * dragging, instead of the tree. */
  bool layers_draw_order_view = false;

  /* Viewport: the Sculpt and Draw tools' shelves along the bottom (Blender's asset shelf). Sculpt's tabs are 0 All,
   * 1 Move, 2 Shape, 3 Mine. */
  ShelfState sculpt_shelf;
  ShelfState draw_shelf;
  /* Viewport: the Tool Settings strip's width last frame (0: the tool has no settings), and how far it is
   * scrolled when it doesn't fit, in pixels. */
  float tool_settings_width = 0.0f;
  float tool_settings_scroll = 0.0f;

  /* Asset Shelf (asset_shelf.cc): the tab (0 Context, 1 All Switches), pinned and its rows (switch ids),
   * the rows that fitted last (kept while nothing fits), the search, the tiles' size and whether hovering
   * previews. */
  int shelf_tab = 0;
  bool shelf_pinned = false;
  std::vector<uint64_t> shelf_pins;
  std::vector<uint64_t> shelf_last;
  std::string shelf_search;
  float shelf_tile = 72.0f;
  bool shelf_hover_preview = true;

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
/* The ViewportActionPanel checkboxes kept for the next moves until the frame or action changes. */
struct MoveOptions {
  bool keep_poses = false;
  bool only_bones = false;
  bool without_children = false;
};
MoveOptions viewport_move_options(const Document &doc);

/* Starts every area header: a grip to drag the area elsewhere (right-click for the area
 * menu), then the editor-type dropdown. */
void area_editor_type_menu(Area &area);

/* Viewport, in viewport.cc. */
void viewport_draw(Area &area, Context &C);
/* The Sculpt tool's brush shelf (sculpt_shelf.cc): how much of the viewport's height it takes (0 unless
 * Sculpt is the tool), drawn into the strip below the view. */
float sculpt_shelf_height(const Context &C, const Area &area);
void sculpt_shelf_draw(Context &C, Area &area, ImVec2 min, ImVec2 size);
/* The Draw tool's shelf (draw_shelf.cc): the same for Draw, and its pens in a popup ("##draw_presets", opened by the
 * caller); a line of varying width drawn with the stroke tessellator into an ImGui draw list. */
float draw_shelf_height(const Context &C, const Area &area);
void draw_shelf_draw(Context &C, Area &area, ImVec2 min, ImVec2 size);
void draw_presets_popup(Context &C);
void ink_line_draw(ImDrawList *dl, const std::vector<float2> &points, const std::vector<float> &half_widths, Color color);
/* A sculpt brush's picture: a line before and after it (the shelf's tiles, the tool bar). */
void sculpt_brush_icon(ImDrawList *dl, SculptBrush brush, ImVec2 center, float size, unsigned int col,
                       unsigned int before);
/* Whether something is being moved in a viewport (G, R, S, W, a Transform tool's box). */
bool viewport_drag_active();
/* Whether the edit going on (a point move: G, R, S, Extrude, a handle tweak) says which layers it changes,
 * each update (see Revision); any other edit with the mouse down may change anything. */
bool viewport_precise_edit();
/* Whether a viewport is being panned or zoomed with the middle button (the view only, not a camera looked
 * through): it changes nothing in the document. */
bool viewport_navigating();
/* The frame moved on while playing during that drag: what it began from takes the keys just recorded
 * and, where it isn't holding anything, the new frame (animation_follow_playback). `dragged` is the
 * document as the drag had it before the frame changed. */
void viewport_drag_follow_playback(Document &dragged, Document &now);
/* Called every frame no drag is going on: forgets what the last one held. */
void viewport_drag_follow_forget();
/* For tests: where Ctrl puts a turn of `degrees` of the active bone layer's picked bones (R when `rotate_key`, else
 * the Transform Bones tool's turn), and what it snapped to ("45Â°", "start" or "setup"). */
float viewport_bone_turn_snap(Document &doc, float degrees, bool rotate_key, std::string *r_what);
/* Properties editor, in properties.cc. */
void properties_draw(Area &area, Context &C);
/* Layers editor, in layers_editor.cc. */
void layers_draw(Area &area, Context &C);
/* Timeline editor, in timeline.cc. */
void timeline_draw(Area &area, Context &C);
/* Asset Shelf, in asset_shelf.cc. */
void asset_shelf_draw(Area &area, Context &C);
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
