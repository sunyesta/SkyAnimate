#pragma once

/* The asset library: one folder, chosen once in Preferences, holding the app's brushes (and
 * later swatches, templates and the like) so every document shares them.
 *
 *   <library>/library.json   marks the folder as a SkyAnimate asset library
 *   <library>/Brushes/       brushes in Moho's own format (see assets/brushes.hh)
 *
 * When no library is set (or its folder is gone), the app asks for a folder at startup and
 * initializes it there. */

#include <string>
#include <vector>

namespace sky {

struct Preferences {
  /* The asset library folder (UTF-8). Empty until one is chosen. */
  std::string asset_library;
};

/* The current preferences. Loaded at startup from %LOCALAPPDATA%\SkyAnimate\preferences.json;
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
/* <library>/Brushes, or "" when no library is set. */
std::string asset_library_brushes_dir();

/* Moho's brush folders found on this computer: the installed brushes of each Moho version and
 * the user's Moho custom content brushes. */
std::vector<std::string> moho_brush_dirs();
/* Copies Moho brushes (.mohobrush files with their images, and plain brush PNGs) from `dirs`
 * into the library's Brushes folder, leaving brushes already there alone, then reloads the
 * brushes. Returns how many brushes were added, or -1 if there is no library. */
int asset_library_import_moho_brushes(const std::vector<std::string> &dirs);

}  // namespace sky
