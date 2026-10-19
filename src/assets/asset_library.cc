#include "assets/asset_library.hh"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#include "assets/brushes.hh"
#include "core/log.hh"
#include "ops/file_ops.hh"

namespace sky {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

static constexpr int LIBRARY_VERSION = 1;
static constexpr const char *LIBRARY_FILE = "library.json";
static constexpr const char *BRUSHES_DIR = "Brushes";

static fs::path path_from_utf8(const std::string &path)
{
  return fs::path(std::u8string(path.begin(), path.end()));
}

static std::string path_to_utf8(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

static bool g_persistent = true;

Preferences &preferences()
{
  static Preferences prefs;
  return prefs;
}

static fs::path preferences_path()
{
  return path_from_utf8(user_data_dir()) / "preferences.json";
}

void preferences_set_persistent(bool persistent)
{
  g_persistent = persistent;
}

void preferences_load()
{
  if (!g_persistent) {
    return;
  }
  std::ifstream in(preferences_path());
  if (!in) {
    return;
  }
  const json j = json::parse(in, nullptr, false);
  if (!j.is_object()) {
    return;
  }
  if (auto it = j.find("asset_library"); it != j.end() && it->is_string()) {
    preferences().asset_library = it->get<std::string>();
  }
  brushes_reload();
}

void preferences_save()
{
  if (!g_persistent) {
    return;
  }
  const json j = {{"version", 1}, {"asset_library", preferences().asset_library}};
  std::ofstream out(preferences_path(), std::ios::trunc);
  out << j.dump(2) << "\n";
}

bool asset_library_ready()
{
  const std::string &folder = preferences().asset_library;
  std::error_code ec;
  return !folder.empty() && fs::is_regular_file(path_from_utf8(folder) / LIBRARY_FILE, ec);
}

bool asset_library_set(const std::string &folder)
{
  const fs::path root = path_from_utf8(folder);
  std::error_code ec;
  fs::create_directories(root / BRUSHES_DIR, ec);
  if (ec) {
    report(ReportType::Error, "Can't create the asset library in " + folder + ": " + ec.message());
    return false;
  }
  if (!fs::is_regular_file(root / LIBRARY_FILE, ec)) {
    std::ofstream out(root / LIBRARY_FILE, std::ios::trunc);
    out << json{{"format", "SkyAnimate asset library"}, {"version", LIBRARY_VERSION}}.dump(2) << "\n";
    if (!out) {
      report(ReportType::Error, "Can't write to " + folder);
      return false;
    }
  }
  preferences().asset_library = path_to_utf8(fs::absolute(root, ec).lexically_normal().make_preferred());
  preferences_save();
  brushes_reload();
  report(ReportType::Info, "Asset library: " + preferences().asset_library);
  return true;
}

std::string asset_library_brushes_dir()
{
  if (preferences().asset_library.empty()) {
    return "";
  }
  return path_to_utf8(path_from_utf8(preferences().asset_library) / BRUSHES_DIR);
}

std::vector<std::string> moho_brush_dirs()
{
  std::vector<std::string> dirs;
  std::error_code ec;
  const auto add = [&](const fs::path &p) {
    if (fs::is_directory(p, ec)) {
      dirs.push_back(path_to_utf8(p));
    }
  };
  /* Installed brushes: Program Files\Moho <version>\Resources\Support\{Common,Pro,Debut}\Brushes. */
  for (const fs::path program_files : {fs::path(L"C:\\Program Files"), fs::path(L"C:\\Program Files (x86)")}) {
    for (const fs::directory_entry &e : fs::directory_iterator(program_files, ec)) {
      const std::wstring name = e.path().filename().wstring();
      if (!e.is_directory(ec) || (name.rfind(L"Moho", 0) != 0 && name.rfind(L"Anime Studio", 0) != 0)) {
        continue;
      }
      for (const wchar_t *variant : {L"Common", L"Pro", L"Debut"}) {
        add(e.path() / L"Resources" / L"Support" / variant / L"Brushes");
      }
    }
  }
  /* The user's custom content: Documents\MohoCustomContent\<edition>\Brushes. */
  wchar_t *profile = nullptr;
  size_t length = 0;
  if (_wdupenv_s(&profile, &length, L"USERPROFILE") == 0 && profile) {
    const fs::path custom = fs::path(profile) / L"Documents" / L"MohoCustomContent";
    for (const fs::directory_entry &e : fs::directory_iterator(custom, ec)) {
      if (e.is_directory(ec)) {
        add(e.path() / L"Brushes");
      }
    }
  }
  std::free(profile);
  return dirs;
}

/* Copies a file or folder unless the destination already exists. */
static bool copy_new(const fs::path &from, const fs::path &to)
{
  std::error_code ec;
  if (fs::exists(to, ec)) {
    return false;
  }
  fs::copy(from, to, fs::copy_options::recursive, ec);
  if (ec) {
    report(ReportType::Warning, "Couldn't copy " + path_to_utf8(from) + ": " + ec.message());
    return false;
  }
  return true;
}

int asset_library_import_moho_brushes(const std::vector<std::string> &dirs)
{
  const std::string brushes_dir = asset_library_brushes_dir();
  if (brushes_dir.empty()) {
    report(ReportType::Warning, "Set up an asset library first (Edit > Preferences)");
    return -1;
  }
  const fs::path dest = path_from_utf8(brushes_dir);
  std::error_code ec;
  fs::create_directories(dest, ec);

  int added = 0;
  for (const std::string &dir : dirs) {
    /* Read the folder the way the library does, then copy each brush's settings and images. */
    std::vector<Brush> found;
    brushes_scan(dir, found);
    const fs::path source = path_from_utf8(dir);
    for (const Brush &brush : found) {
      const fs::path settings = source / path_from_utf8(brush.name + ".mohobrush");
      bool copied = false;
      if (fs::is_regular_file(settings, ec)) {
        copied = copy_new(settings, dest / settings.filename());
      }
      /* Images: a PNG beside the brush, or a multi-brush folder. */
      std::set<fs::path> parts;
      for (const std::string &file : brush.files) {
        fs::path rel = path_from_utf8(file).lexically_relative(source);
        if (rel.empty() || *rel.begin() == "..") {
          continue;
        }
        parts.insert(*rel.begin());
      }
      for (const fs::path &part : parts) {
        copied = copy_new(source / part, dest / part) || copied;
      }
      added += copied ? 1 : 0;
    }
  }
  brushes_reload();
  report(ReportType::Info, "Imported " + std::to_string(added) + " Moho brushes");
  return added;
}

}  // namespace sky
