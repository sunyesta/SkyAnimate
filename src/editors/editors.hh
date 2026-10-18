#pragma once

/* Editor types and areas. An area is one dockable panel of the window showing one
 * editor, like Blender's ScrArea + SpaceType. Splitting and joining areas uses ImGui
 * docking: drag an area's tab onto another area. */

#include <cstdint>
#include <string>

namespace sky {

struct Context;

enum class EditorType { Viewport, Properties, Timeline, TextEditor, Console, Info };

inline constexpr EditorType EDITOR_TYPES[] = {EditorType::Viewport, EditorType::Properties,
                                              EditorType::Timeline, EditorType::TextEditor,
                                              EditorType::Console,  EditorType::Info};

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

/* The editor-type dropdown that starts every area header. */
void area_editor_type_menu(Area &area);

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
