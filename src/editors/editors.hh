#pragma once

#include <map>

/* Editor types and areas. An area is one dockable panel of the window showing one
 * editor, like Blender's ScrArea + SpaceType. Splitting and joining areas uses ImGui
 * docking: drag an area's tab onto another area. */

#include <cstdint>
#include <string>

namespace sky {

struct Context;

enum class EditorType { Viewport, Properties, Layers, Timeline, TextEditor, Console, Info };

inline constexpr EditorType EDITOR_TYPES[] = {EditorType::Viewport,   EditorType::Properties, EditorType::Layers,
                                              EditorType::Timeline,   EditorType::TextEditor, EditorType::Console,
                                              EditorType::Info};

const char *editor_type_label(EditorType type);
/* Name of the editor's keymap, tried before "Window" while the mouse is over it. */
const char *editor_keymap_name(EditorType type);

enum class AreaSplit { None, Vertical, Horizontal };

/* What a viewport draws over the artwork, like Blender's viewport overlays. Each viewport has
 * its own; `show` turns them all off at once. */
/* Which curve handles the viewport shows, as Blender's Overlays > Handles. */
/* Shape: every handle of the shapes being worked on (any of whose points is selected), plus the
 * selected points'. */
enum class HandleDisplay { None, Selected, Shape, All };
/* Which bones the viewport shows: none, those of selected bone layers, or every bone layer's. */
enum class ArmatureDisplay { Off, Selected, On };

struct ViewportOverlays {
  bool show = true;
  bool grid = false;
  bool cursor = true;
  bool camera_frame = true;
  /* Camera, frame and layer in the top-left corner. */
  bool info = true;
  /* Curve wires and points of the active layer while editing it. */
  bool wireframe = true;
  bool points = true;
  HandleDisplay handles = HandleDisplay::Shape;
  ArmatureDisplay armature = ArmatureDisplay::On;
};

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
  /* Timeline: how far the channel rows are scrolled, in pixels, and which folding rows are open
   * (by key; a row missing here is at its default). */
  float channel_scroll = 0.0f;
  std::map<std::string, bool> timeline_open;

  ViewportOverlays overlays;

  /* Viewport view: the document point at the centre and pixels per document unit. */
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
/* Text editor, in text_editor.cc. */
void text_editor_draw(Area &area, Context &C);
/* Python console, in console.cc. */
void console_draw(Area &area, Context &C);

/* A header button that opens a popup menu of the same name, styled like Blender's
 * flat header menus. Returns true while the menu is open; then call ImGui::EndPopup(). */
bool header_menu(const char *label);

}  // namespace sky
