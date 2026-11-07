#include "ops/file_ops.hh"

#include "anim/animation.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <process.h>

#include "core/log.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "io/oanim_file.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "python/py_runtime.hh"
#include "wm/context.hh"
#include "wm/file_dialog.hh"

namespace oanim {

namespace fs = std::filesystem;

static constexpr const char *FILTER_NAME = "Open Animate files";
static constexpr const char *FILTER_PATTERN = "oanim";
/* Opening also takes files saved before Open Animate was renamed from SkyAnimate (.sky). */
static constexpr const char *OPEN_PATTERN = "oanim;sky;fla;xfl";
/* Blender's default autosave interval. */
static constexpr uint64_t AUTOSAVE_INTERVAL_NS = 2ull * 60 * 1000000000;
static constexpr size_t RECENT_FILES_MAX = 10;

/* ---------------------------------------------------------------------------------
 * Paths
 */

static fs::path path_from_utf8(const std::string &path)
{
  return fs::path(std::u8string(path.begin(), path.end()));
}

static std::string path_to_utf8(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

static std::string file_name(const std::string &path)
{
  return path_to_utf8(path_from_utf8(path).filename());
}

/* An absolute path with backslashes, so one file is always named the same way (recent files,
 * the window title) however it was given. */
static std::string path_normalize(const std::string &path)
{
  std::error_code ec;
  fs::path p = fs::absolute(path_from_utf8(path), ec);
  if (ec) {
    p = path_from_utf8(path);
  }
  return path_to_utf8(p.lexically_normal().make_preferred());
}

/* Adds .oanim unless the name already ends in it (or in .sky, the extension from before Open Animate
 * was renamed). */
static std::string path_with_extension(const std::string &path)
{
  fs::path p = path_from_utf8(path);
  std::string ext = path_to_utf8(p.extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext == ".oanim" || ext == ".sky" ? path : path + ".oanim";
}

std::string user_data_dir()
{
  fs::path base = fs::temp_directory_path();
  wchar_t *local = nullptr;
  size_t length = 0;
  if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") == 0 && local) {
    base = fs::path(local);
  }
  std::free(local);
  const fs::path dir = base / "OpenAnimate";
  std::error_code ec;
  /* The first run after the rename from SkyAnimate brings its preferences, autosaves and crash
   * reports along (a copy: a SkyAnimate still open keeps its own). */
  if (const fs::path old = base / "SkyAnimate"; !fs::exists(dir, ec) && fs::is_directory(old, ec)) {
    fs::copy(old, dir, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    fs::remove(dir / "remote.json", ec);
  }
  fs::create_directories(dir, ec);
  return path_to_utf8(dir);
}

std::string crash_dir()
{
  return path_to_utf8(path_from_utf8(user_data_dir()) / "crashes");
}

std::string autosave_dir()
{
  const fs::path dir = path_from_utf8(user_data_dir()) / "autosave";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return path_to_utf8(dir);
}

std::string quit_file_path()
{
  return path_to_utf8(path_from_utf8(user_data_dir()) / "quit.oanim");
}

/* The file open when Open Animate last quit, for reopening it next time. */
static fs::path session_file_path()
{
  return path_from_utf8(user_data_dir()) / "last_session.txt";
}

static std::string time_label(const fs::path &path)
{
  std::error_code ec;
  const fs::file_time_type written = fs::last_write_time(path, ec);
  if (ec) {
    return "";
  }
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(written));
  std::tm local{};
  localtime_s(&local, &t);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
  return text;
}

/* ---------------------------------------------------------------------------------
 * Recent files
 */

static fs::path recent_files_path()
{
  return path_from_utf8(user_data_dir()) / "recent-files.txt";
}

std::vector<std::string> recent_files()
{
  std::vector<std::string> files;
  std::ifstream in(recent_files_path());
  std::string line;
  while (std::getline(in, line) && files.size() < RECENT_FILES_MAX) {
    if (!line.empty()) {
      files.push_back(line);
    }
  }
  return files;
}

static void recent_files_add(const Context &C, const std::string &path)
{
  if (C.wm->background) {
    return;
  }
  std::vector<std::string> files = recent_files();
  std::erase(files, path);
  files.insert(files.begin(), path);
  files.resize(std::min(files.size(), RECENT_FILES_MAX));
  std::ofstream out(recent_files_path(), std::ios::trunc);
  for (const std::string &file : files) {
    out << file << '\n';
  }
}

/* ---------------------------------------------------------------------------------
 * Loading and saving
 */

bool document_is_dirty(const Context &C)
{
  return C.undo->change_count() != C.wm->saved_change;
}

/* Replaces the document and starts a fresh undo history. */
/* Mirror editing goes with the file: the header's X and Y as they are, into what is written. */
static void mirror_settings_to_document(Context &C)
{
  C.document->mirror_x = C.wm->tool_settings.mirror_x;
  C.document->mirror_y = C.wm->tool_settings.mirror_y;
}

static void document_replace(Context &C, Document &&doc, const std::string &filepath, bool saved)
{
  *C.document = std::move(doc);
  C.wm->tool_settings.mirror_x = C.document->mirror_x;
  C.wm->tool_settings.mirror_y = C.document->mirror_y;
  C.wm->filepath = filepath;
  C.wm->playing = false;
  /* What was picked on the old document's paths goes with it: the new one's layers may have the same ids, and Own
   * Motion would take a stale pick for one of theirs. */
  C.wm->path_dots.clear();
  C.wm->path_handle_picks.clear();
  motion_path_picks_sync(C);
  C.undo->reset(*C.document);
  /* A recovered document isn't saved anywhere yet, so it starts out with unsaved changes. */
  C.wm->saved_change = saved ? C.undo->change_count() : std::numeric_limits<uint64_t>::max();
  C.wm->autosaved_change = C.undo->change_count();
}

static Document document_new()
{
  Document doc;
  /* A new document starts with one vector layer to draw on, as in Moho. */
  document_add_layer(doc, LayerType::Vector, "Layer 1");
  return doc;
}

static bool document_read(const std::string &path, Document &r_doc)
{
  std::string error;
  if (!oanim_file_read(path, r_doc, &error)) {
    report(ReportType::Error, error);
    return false;
  }
  return true;
}

/* A Flash .fla (Flash 5 to CS6's binary format, or Flash CS5 on and Animate's XFL: a zip of XML, or an unzipped
 * .xfl project): converted by scripts/modules/oanim/io_fla.py (io_xfl.py for XFL) into an .oanim
 * in the temp folder and opened from there, untitled and unsaved (named after the .fla), so saving asks
 * where to keep it and the .fla is never written. What didn't come across is listed in the Info editor. */
static bool document_import_fla(Context &C, const std::string &path)
{
  const fs::path temp = fs::temp_directory_path() / ("oanim_fla_" + std::to_string(_getpid()) + ".oanim");
  std::error_code ec;
  fs::remove(temp, ec);
  /* Paths as Python string literals: forward slashes, quotes escaped. */
  const auto literal = [](std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    std::string out = "'";
    for (const char ch : text) {
      out += ch == '\'' ? std::string("\\'") : std::string(1, ch);
    }
    return out + "'";
  };
  const std::string code = "import oanim, oanim.io_fla\n"
                           "_r = oanim.io_fla.convert(" + literal(path) + ", " + literal(path_to_utf8(temp)) +
                           ", compress=False)\n"
                           "for _n in _r['notes']:\n"
                           "    oanim.wm.report('From the .fla: ' + _n, 'WARNING')\n";
  if (!python::run_string(code) || !fs::exists(temp, ec)) {
    report(ReportType::Error, "Could not open \"" + file_name(path) + "\" (see the Info editor)");
    return false;
  }
  Document doc;
  const bool read = document_read(path_to_utf8(temp), doc);
  fs::remove(temp, ec);
  if (!read) {
    return false;
  }
  doc.name = path_to_utf8(path_from_utf8(path).stem());
  document_replace(C, std::move(doc), "", false);
  report(ReportType::Info, "Opened \"" + file_name(path) + "\" from Flash: save it as an .oanim to keep it");
  return true;
}

bool document_open(Context &C, const std::string &given_path)
{
  const std::string path = path_normalize(given_path);
  {
    std::string ext = path_to_utf8(path_from_utf8(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (ext == ".fla" || ext == ".xfl") {
      return document_import_fla(C, path);
    }
  }
  Document doc;
  if (!document_read(path, doc)) {
    return false;
  }
  document_replace(C, std::move(doc), path, true);
  recent_files_add(C, path);
  report(ReportType::Info, "Opened \"" + file_name(path) + "\"");
  /* Missing media say so once, and where to fix it (no dialog in the way). */
  if (const int missing = media_missing_count(*C.document); missing > 0) {
    report_banner(ReportType::Warning,
                  std::to_string(missing) + (missing == 1 ? " media file is" : " media files are") +
                      " missing (moved or renamed)",
                  "Select its Media layer (marked red in the Layers editor) and click Locate in its Media tab",
                  "Missing media");
  }
  return true;
}

/* Writes the document. `remember` makes the path the document's file (Save, Save As);
 * Save Copy leaves it alone. */
static bool document_write(Context &C, const std::string &given_path, bool remember)
{
  const std::string path = path_normalize(given_path);
  std::string error;
  animation_record(*C.document);
  mirror_settings_to_document(C);
  if (!oanim_file_write(*C.document, path, &error)) {
    report(ReportType::Error, error);
    return false;
  }
  if (remember) {
    C.wm->filepath = path;
    C.wm->saved_change = C.undo->change_count();
  }
  recent_files_add(C, path);
  report(ReportType::Info, std::string(remember ? "Saved" : "Saved copy") + " \"" + file_name(path) + "\"");
  return true;
}

static std::string default_save_path(const Context &C)
{
  return C.wm->filepath.empty() ? C.document->name + ".oanim" : C.wm->filepath;
}

void document_save_then(Context &C, std::function<void()> then)
{
  if (!C.wm->filepath.empty()) {
    if (document_write(C, C.wm->filepath, true)) {
      then();
    }
    return;
  }
  Context *ctx = &C;
  file_dialog_save(FILTER_NAME, FILTER_PATTERN, default_save_path(C), [ctx, then](const std::string &path) {
    if (document_write(*ctx, path_with_extension(path), true)) {
      then();
    }
  });
}

/* New, Open and Quit would lose unsaved changes: in the window they first ask, by leaving
 * the call for the window's prompt, which runs it again with confirmed=1. */
static bool confirm_unsaved(Context &C, const char *idname)
{
  if (C.wm->background || !document_is_dirty(C) || op_arg_bool(C, "confirmed")) {
    return false;
  }
  C.wm->confirm_operator = idname;
  C.wm->confirm_args = C.op_args ? *C.op_args : OpArgs{};
  return true;
}

/* Runs `idname` again with the path a dialog returns. Without a window there's no dialog. */
static OpStatus ask_for_path(Context &C, const char *idname, bool save, const std::string &folder = {})
{
  if (C.wm->background) {
    report(ReportType::Error, std::string(idname) + " needs a filepath without a window");
    return OpStatus::Cancelled;
  }
  Context *ctx = &C;
  OpArgs args = C.op_args ? *C.op_args : OpArgs{};
  auto done = [ctx, idname, args, save](const std::string &path) mutable {
    args["filepath"] = save ? path_with_extension(path) : path;
    operator_call(*ctx, idname, args);
  };
  if (save) {
    file_dialog_save(FILTER_NAME, FILTER_PATTERN, default_save_path(C), std::move(done));
  }
  else {
    file_dialog_open(FILTER_NAME, OPEN_PATTERN, std::move(done), folder);
  }
  /* Nothing happened yet; the dialog's answer runs the operator for real. */
  return OpStatus::Cancelled;
}

/* scene.oanim -> scene_001.oanim, scene_001.oanim -> scene_002.oanim, skipping files that exist. */
static std::string incremented_path(const std::string &path)
{
  const fs::path p = path_from_utf8(path);
  std::string stem = path_to_utf8(p.stem());
  size_t digits_at = stem.size();
  while (digits_at > 0 && std::isdigit(static_cast<unsigned char>(stem[digits_at - 1]))) {
    digits_at--;
  }
  long long number = 0;
  size_t width = 3;
  if (digits_at < stem.size()) {
    number = std::atoll(stem.c_str() + digits_at);
    width = std::max<size_t>(width, stem.size() - digits_at);
    stem.resize(digits_at);
  }
  else {
    stem += "_";
  }
  for (;;) {
    std::string digits = std::to_string(++number);
    if (digits.size() < width) {
      digits.insert(0, width - digits.size(), '0');
    }
    const fs::path candidate = p.parent_path() / path_from_utf8(stem + digits + ".oanim");
    if (!fs::exists(candidate)) {
      return path_to_utf8(candidate);
    }
  }
}

/* ---------------------------------------------------------------------------------
 * Operators
 */

static void file_operator_add(const char *idname, const char *name, const char *description,
                              std::function<OpStatus(Context &C)> exec, std::function<bool(Context &C)> poll = {})
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = OPTYPE_REGISTER;
  ot.exec = std::move(exec);
  ot.poll = std::move(poll);
  operators().add(std::move(ot));
}

static bool has_filepath(Context &C)
{
  return !C.wm->filepath.empty();
}

void file_operators_register()
{
  file_operator_add("wm.read_homefile", "New", "Start a new document", [](Context &C) {
    if (confirm_unsaved(C, "wm.read_homefile")) {
      return OpStatus::Cancelled;
    }
    document_replace(C, document_new(), "", true);
    return OpStatus::Finished;
  });

  file_operator_add("wm.open_mainfile", "Open", "Open a .oanim file, or a Flash or Animate .fla (or .xfl) as a new one", [](Context &C) {
    const std::string path = op_arg(C, "filepath");
    if (path.empty()) {
      return ask_for_path(C, "wm.open_mainfile", false);
    }
    if (confirm_unsaved(C, "wm.open_mainfile")) {
      return OpStatus::Cancelled;
    }
    return document_open(C, path) ? OpStatus::Finished : OpStatus::Cancelled;
  });

  file_operator_add("wm.save_mainfile", "Save", "Save the document, asking for a file the first time",
                    [](Context &C) {
                      const std::string path = op_arg(C, "filepath", C.wm->filepath);
                      if (path.empty()) {
                        return ask_for_path(C, "wm.save_mainfile", true);
                      }
                      return document_write(C, path_with_extension(path), true) ? OpStatus::Finished
                                                                                : OpStatus::Cancelled;
                    });

  file_operator_add("wm.save_as_mainfile", "Save As", "Save the document to a new file", [](Context &C) {
    const std::string path = op_arg(C, "filepath");
    if (path.empty()) {
      return ask_for_path(C, "wm.save_as_mainfile", true);
    }
    return document_write(C, path_with_extension(path), true) ? OpStatus::Finished : OpStatus::Cancelled;
  });

  file_operator_add("wm.save_copy", "Save Copy", "Save a copy, and keep working on the current file",
                    [](Context &C) {
                      const std::string path = op_arg(C, "filepath");
                      if (path.empty()) {
                        return ask_for_path(C, "wm.save_copy", true);
                      }
                      return document_write(C, path_with_extension(path), false) ? OpStatus::Finished
                                                                                 : OpStatus::Cancelled;
                    });

  file_operator_add(
      "wm.save_incremental", "Save Incremental",
      "Save to the next numbered file (scene_001.oanim, scene_002.oanim, ...) and keep working there",
      [](Context &C) {
        return document_write(C, incremented_path(C.wm->filepath), true) ? OpStatus::Finished : OpStatus::Cancelled;
      },
      has_filepath);

  file_operator_add(
      "wm.revert_mainfile", "Revert", "Reload the saved file, losing unsaved changes",
      [](Context &C) {
        if (confirm_unsaved(C, "wm.revert_mainfile")) {
          return OpStatus::Cancelled;
        }
        return document_open(C, C.wm->filepath) ? OpStatus::Finished : OpStatus::Cancelled;
      },
      has_filepath);

  file_operator_add(
      "wm.recover_last_session", "Recover Last Session", "Open the document as it was when Open Animate last quit",
      [](Context &C) {
        if (confirm_unsaved(C, "wm.recover_last_session")) {
          return OpStatus::Cancelled;
        }
        Document doc;
        if (!document_read(quit_file_path(), doc)) {
          return OpStatus::Cancelled;
        }
        document_replace(C, std::move(doc), "", false);
        report(ReportType::Info, "Recovered the last session");
        return OpStatus::Finished;
      },
      [](Context &) { return fs::exists(path_from_utf8(quit_file_path())); });

  file_operator_add("wm.recover_auto_save", "Recover Auto Save", "Open an automatically saved document",
                    [](Context &C) {
                      const std::string path = op_arg(C, "filepath");
                      if (path.empty()) {
                        /* As Blender's File > Recover > Auto Save...: the file browser, in the
                         * autosave folder. */
                        return ask_for_path(C, "wm.recover_auto_save", false, autosave_dir() + "\\");
                      }
                      if (confirm_unsaved(C, "wm.recover_auto_save")) {
                        return OpStatus::Cancelled;
                      }
                      Document doc;
                      if (!document_read(path, doc)) {
                        return OpStatus::Cancelled;
                      }
                      document_replace(C, std::move(doc), "", false);
                      report(ReportType::Info, "Recovered \"" + file_name(path) + "\"");
                      return OpStatus::Finished;
                    });

  /* Replaces the plain quit from builtin_ops, now that there's something to lose. */
  OperatorType quit;
  quit.idname = "wm.quit";
  quit.name = "Quit";
  quit.description = "Quit Open Animate";
  quit.exec = [](Context &C) {
    if (confirm_unsaved(C, "wm.quit")) {
      return OpStatus::Cancelled;
    }
    C.wm->quit_requested = true;
    return OpStatus::Finished;
  };
  operators().add(std::move(quit));
}

/* ---------------------------------------------------------------------------------
 * Autosave
 */

static fs::path g_autosave_path;
static uint64_t g_autosave_last_ns = 0;

/* An autosave being written on its own thread: making the file of a big document takes seconds, which
 * froze the window every few minutes. Only the copy it writes from is made on the main thread. */
struct AutosaveJob {
  std::future<bool> done;
  std::string error;
  fs::path path;
  uint64_t change = 0;
};
static AutosaveJob g_autosave_job;

/* Takes a finished autosave's result (or waits for it with `wait`). False while one is still writing. */
static bool autosave_job_finish(Context &C, bool wait)
{
  AutosaveJob &job = g_autosave_job;
  if (!job.done.valid()) {
    return true;
  }
  if (!wait && job.done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return false;
  }
  if (!job.done.get()) {
    report(ReportType::Warning, "Autosave failed: " + job.error);
    return true;
  }
  /* Saving under a new name leaves one autosave per session, not one per name. */
  std::error_code ec;
  if (!g_autosave_path.empty() && g_autosave_path != job.path) {
    fs::remove(g_autosave_path, ec);
  }
  g_autosave_path = job.path;
  C.wm->autosaved_change = job.change;
  return true;
}

/* walk_12345_autosave.oanim: the process id keeps two running copies apart. */
static fs::path autosave_path_for(const Context &C)
{
  const std::string stem = C.wm->filepath.empty() ? "untitled" : path_to_utf8(path_from_utf8(C.wm->filepath).stem());
  return path_from_utf8(autosave_dir()) / path_from_utf8(stem + "_" + std::to_string(_getpid()) + "_autosave.oanim");
}

void autosave_update(Context &C, uint64_t now_ns)
{
  if (!autosave_job_finish(C, false)) {
    return;
  }
  if (g_autosave_last_ns == 0) {
    g_autosave_last_ns = now_ns;
  }
  if (now_ns - g_autosave_last_ns < AUTOSAVE_INTERVAL_NS) {
    return;
  }
  g_autosave_last_ns = now_ns;
  if (C.undo->change_count() == C.wm->autosaved_change || !document_is_dirty(C)) {
    return;
  }
  AutosaveJob &job = g_autosave_job;
  job.path = autosave_path_for(C);
  job.change = C.undo->change_count();
  job.error.clear();
  mirror_settings_to_document(C);
  std::function<bool(std::string *)> write = oanim_file_writer(*C.document, path_to_utf8(job.path));
  job.done = std::async(std::launch::async, [write = std::move(write)]() { return write(&g_autosave_job.error); });
}

void autosave_quit(Context &C)
{
  autosave_job_finish(C, true);
  {
    /* The next start reopens this file, as last saved (unsaved changes at quitting were turned
     * down; File > Recover > Last Session still has them). */
    std::ofstream out(session_file_path(), std::ios::binary | std::ios::trunc);
    out << C.wm->filepath;
  }
  std::string error;
  mirror_settings_to_document(C);
  /* Saved as it is: the snapshot is that file, copied (writing a large document again took seconds). */
  std::error_code copy_ec;
  const bool copied = !C.wm->filepath.empty() && !document_is_dirty(C) &&
                      fs::copy_file(path_from_utf8(C.wm->filepath), path_from_utf8(quit_file_path()),
                                    fs::copy_options::overwrite_existing, copy_ec);
  if (!copied && !oanim_file_write(*C.document, quit_file_path(), &error)) {
    report(ReportType::Warning, "Could not write quit.oanim: " + error);
  }
  std::error_code ec;
  if (!g_autosave_path.empty()) {
    fs::remove(g_autosave_path, ec);
  }
}

std::vector<RecoverableFile> autosave_files()
{
  std::vector<std::pair<fs::file_time_type, RecoverableFile>> found;
  std::error_code ec;
  for (const fs::directory_entry &entry : fs::directory_iterator(path_from_utf8(autosave_dir()), ec)) {
    if (entry.path().extension() != ".oanim" && entry.path().extension() != ".sky") {
      continue;
    }
    found.push_back({entry.last_write_time(ec),
                     {path_to_utf8(entry.path()), path_to_utf8(entry.path().filename()) + "  -  " + time_label(entry.path())}});
  }
  std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  std::vector<RecoverableFile> files;
  for (auto &[time, file] : found) {
    files.push_back(std::move(file));
  }
  return files;
}

bool session_reopen(Context &C)
{
  std::ifstream in(session_file_path(), std::ios::binary);
  if (!in) {
    return false;
  }
  std::string path;
  std::getline(in, path);
  while (!path.empty() && (path.back() == '\r' || path.back() == '\n')) {
    path.pop_back();
  }
  std::error_code ec;
  if (path.empty() || !fs::exists(path_from_utf8(path), ec)) {
    return false;
  }
  return document_open(C, path);
}

}  // namespace oanim
