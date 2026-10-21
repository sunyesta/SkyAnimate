#pragma once

/* Native Windows open and save dialogs (through SDL3). The dialog runs without blocking
 * the app; its callback runs later on the main thread, from file_dialog_process(). */

#include <functional>
#include <string>

namespace oanim {

using FileDialogDone = std::function<void(const std::string &path)>;

/* filter_name/pattern, e.g. "Python scripts" / "py". `done` runs only if a file was chosen.
 * `folder` is where the dialog starts (empty: wherever Windows remembers). */
void file_dialog_open(const char *filter_name, const char *pattern, FileDialogDone done,
                      const std::string &folder = {});
void file_dialog_save(const char *filter_name, const char *pattern, const std::string &default_path,
                      FileDialogDone done);

/* Picks a folder. */
void file_dialog_folder(FileDialogDone done);

/* Runs the callbacks of dialogs that have closed. Call once per frame. */
void file_dialog_process();

}  // namespace oanim
