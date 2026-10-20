#pragma once

/* File operators, named after Blender's: wm.read_homefile (New), wm.open_mainfile,
 * wm.save_mainfile, wm.save_as_mainfile, wm.save_copy, wm.save_incremental, wm.revert_mainfile,
 * wm.recover_last_session and wm.recover_auto_save. Each takes an optional `filepath`
 * argument; without one, the window shows a file dialog.
 *
 * Autosave and recovery follow Blender: while there are unsaved changes the document is
 * written every few minutes to the autosave folder, a clean quit writes quit.sky, and
 * File > Recover opens either. Autosaves of a session that crashed stay behind to recover. */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sky {

struct Context;

void file_operators_register();

/* The document changed since it was last saved or opened. */
bool document_is_dirty(const Context &C);

/* Opens a .sky file (no unsaved-changes check). Reports and returns false on failure. */
bool document_open(Context &C, const std::string &path);
/* Saves to the current file, asking for a path first if there is none, then runs `then`
 * (used by the unsaved-changes prompt: Save, then quit). */
void document_save_then(Context &C, std::function<void()> then);

/* %LOCALAPPDATA%\SkyAnimate, created on first use. */
std::string user_data_dir();
/* Where crash reports go: <user_data_dir>/crashes (see core/crash.hh). */
std::string crash_dir();
std::string autosave_dir();
std::string quit_file_path();

/* Writes an autosave when the interval has passed and there are changes since the last one. */
void autosave_update(Context &C, uint64_t now_ns);
/* A clean quit: writes quit.sky and removes this session's autosave. */
void autosave_quit(Context &C);
/* Opens the file that was open when SkyAnimate last quit, if it is still there. */
bool session_reopen(Context &C);

struct RecoverableFile {
  std::string path;
  /* "walk.sky  -  2026-10-18 14:05" */
  std::string label;
};
/* Autosaves, newest first. */
std::vector<RecoverableFile> autosave_files();

/* Most recently opened or saved files, newest first. Not recorded in background mode. */
std::vector<std::string> recent_files();

}  // namespace sky
