#pragma once

/* Editor types and areas. An area is one dockable panel of the window showing one
 * editor, like Blender's ScrArea + SpaceType. Splitting and joining areas uses ImGui
 * docking: drag an area's tab onto another area. */

#include <string>

namespace sky {

struct Context;

enum class EditorType { Viewport, Properties, Timeline, Info };

inline constexpr EditorType EDITOR_TYPES[] = {
    EditorType::Viewport, EditorType::Properties, EditorType::Timeline, EditorType::Info};

const char *editor_type_label(EditorType type);
/* Name of the editor's keymap, tried before "Window" while the mouse is over it. */
const char *editor_keymap_name(EditorType type);

struct Area {
  int id = 0;
  EditorType type = EditorType::Viewport;
  bool open = true;

  /* Timeline view: the frame range visible across the area. */
  float view_start = 0.0f;
  float view_end = 250.0f;
  bool view_initialized = false;

  /* ImGui window name: the label shows on the tab; "###area<id>" keeps the window's
   * identity when the editor type changes. */
  std::string window_name() const;
};

/* Draws the area as a dockable window. Returns true while the mouse is over it. */
bool area_draw(Area &area, Context &C);

/* The editor-type dropdown that starts every area header. */
void area_editor_type_menu(Area &area);

/* Timeline editor, in timeline.cc. */
void timeline_draw(Area &area, Context &C);

}  // namespace sky
