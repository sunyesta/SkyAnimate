#pragma once

/* The asset library: one folder, chosen once in Preferences, holding the app's brushes (and
 * later swatches, templates and the like) so every document shares them.
 *
 *   <library>/library.json                    marks it as an asset library (format version 2)
 *   <library>/Brushes/<Category>/<Name>.oabrush   the brushes: one file each, a category a folder
 *                                                 (assets/brush_presets.hh)
 *   <library>/Brush Shapes/                   the images they paint with: PNGs, or folders of
 *                                             them (assets/brushes.hh)
 *
 * A version 1 library (Moho's brushes in Brushes/) is upgraded when it's read: see
 * brushes_reload. *
 * When no library is set (or its folder is gone), the app asks for a folder at startup and
 * initializes it there. */

#include <cstdint>
#include <string>
#include <vector>

namespace oanim {

struct Preferences {
  /* The asset library folder (UTF-8). Empty until one is chosen. */
  std::string asset_library;
  /* Remote control (wm/remote.hh): programs on this computer, such as Claude through the MCP
   * bridge in tools/mcp, may drive the app. Off until turned on. */
  bool remote_control = false;
  int remote_port = 7853;
  /* Which API pens come through (TabletApi): 0 Automatic, 1 Windows Ink, 2 WinTab. */
  int tablet_api = 0;
  /* What the status bar shows at its right (its right-click menu): the statistics, the frame, the version. */
  bool status_statistics = true;
  bool status_frame = false;
  bool status_version = true;
  /* The viewport's Tool Settings bar (its View menu). */
  bool viewport_tool_settings = true;
  /* Layers editor settings: Auto Focus keeps the list folded down to the selected layers. */
  bool layers_auto_focus = false;
  /* Overlay colours (0xRRGGBB, editors/overlay_contrast.hh): vector wires and points, and motion paths' lines, each
   * with the lighter colour a piece takes over something darker it is hard to see on, and the darker one over
   * something lighter. */
  uint32_t wire_color = 0x919191;
  uint32_t wire_lighter = 0xAEAEAE;
  uint32_t wire_darker = 0x6E6E6E;
  uint32_t path_color = 0x4D9191;
  uint32_t path_lighter = 0x5CA8AE;
  uint32_t path_darker = 0x3A7070;
  /* Vector handles: the line and tip dot, and the line where it is picked. */
  uint32_t handle_color = 0xFF9D9D;
  uint32_t handle_selected = 0xFF3737;
};

/* The current preferences. Loaded at startup from %LOCALAPPDATA%\OpenAnimate\preferences.json;
 * in background mode (tests) nothing is read or written, so runs never touch the user's. */
Preferences &preferences();
void preferences_load();
void preferences_save();
/* Whether preferences_save() writes the file (off in background mode). */
void preferences_set_persistent(bool persistent);

/* True when the preferences name a folder that holds an initialized asset library. */
bool asset_library_ready();
/* Creates library.json and the asset folders in `folder` (keeping anything already there), makes
 * it the asset library, saves the preferences and reloads the brushes. False (and a report) if
 * the folder can't be written. */
bool asset_library_set(const std::string &folder);
/* <library>/Brush Shapes, or "" when no library is set. */
std::string asset_library_shapes_dir();
/* The library's format version (library.json), 0 without one. */
int asset_library_version();
/* Marks the library as being in the current format (after an upgrade). */
void asset_library_mark_current();

}  // namespace oanim
