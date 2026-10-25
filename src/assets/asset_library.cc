#include "assets/asset_library.hh"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>

#include "anim/channel.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
#include "ops/file_ops.hh"

namespace oanim {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

static constexpr int LIBRARY_VERSION = 2;
static constexpr const char *LIBRARY_FILE = "library.json";
static constexpr const char *SHAPES_DIR = "Brush Shapes";

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
  if (auto it = j.find("remote_control"); it != j.end() && it->is_boolean()) {
    preferences().remote_control = it->get<bool>();
  }
  if (auto it = j.find("remote_port"); it != j.end() && it->is_number_integer()) {
    preferences().remote_port = std::clamp(it->get<int>(), 1024, 65535);
  }
  for (const auto &[key, flag] : {std::pair{"status_statistics", &Preferences::status_statistics},
                                 std::pair{"status_frame", &Preferences::status_frame},
                                 std::pair{"status_version", &Preferences::status_version}})
  {
    if (auto it = j.find(key); it != j.end() && it->is_boolean()) {
      preferences().*flag = it->get<bool>();
    }
  }
  /* What new keys get (the timeline's Interpolation menu with no keys selected). */
  if (auto it = j.find("new_keys"); it != j.end() && it->is_object()) {
    NewKeySettings &keys = new_key_settings();
    KeySettings &s = keys.settings;
    const json &k = *it;
    if (auto v = k.find("interp"); v != k.end() && v->is_string()) {
      interp_from_identifier(v->get<std::string>(), &s.interp);
    }
    if (auto v = k.find("ease"); v != k.end() && v->is_string()) {
      ease_from_identifier(v->get<std::string>(), &s.ease);
    }
    const auto number = [&](const char *name, auto &r_value) {
      if (auto v = k.find(name); v != k.end() && v->is_number()) {
        r_value = v->get<std::decay_t<decltype(r_value)>>();
      }
    };
    number("hold", s.hold);
    number("interval", s.interval);
    number("overshoot", s.overshoot);
    number("elastic_amplitude", s.elastic_amplitude);
    number("elastic_period", s.elastic_period);
    number("noise_amplitude", s.noise_amplitude);
    number("noise_frequency", s.noise_frequency);
    number("cycle_back", s.cycle_back);
    s.hold = std::max(0, s.hold);
    s.interval = std::clamp(s.interval, 1, 3);
    s.elastic_period = std::max(0.02f, s.elastic_period);
    if (auto v = k.find("copy_previous"); v != k.end() && v->is_boolean()) {
      keys.copy_previous = v->get<bool>();
    }
  }
  brushes_reload();
}

void preferences_save()
{
  if (!g_persistent) {
    return;
  }
  const NewKeySettings &keys = new_key_settings();
  const KeySettings &s = keys.settings;
  const json new_keys = {{"interp", interp_identifier(s.interp)},
                         {"ease", ease_identifier(s.ease)},
                         {"hold", s.hold},
                         {"interval", s.interval},
                         {"overshoot", s.overshoot},
                         {"elastic_amplitude", s.elastic_amplitude},
                         {"elastic_period", s.elastic_period},
                         {"noise_amplitude", s.noise_amplitude},
                         {"noise_frequency", s.noise_frequency},
                         {"cycle_back", s.cycle_back},
                         {"copy_previous", keys.copy_previous}};
  const json j = {{"version", 1},
                  {"asset_library", preferences().asset_library},
                  {"remote_control", preferences().remote_control},
                  {"remote_port", preferences().remote_port},
                  {"status_statistics", preferences().status_statistics},
                  {"status_frame", preferences().status_frame},
                  {"status_version", preferences().status_version},
                  {"new_keys", new_keys}};
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
  fs::create_directories(root / SHAPES_DIR, ec);
  fs::create_directories(root / "Brushes", ec);
  if (ec) {
    report(ReportType::Error, "Can't create the asset library in " + folder + ": " + ec.message());
    return false;
  }
  if (!fs::is_regular_file(root / LIBRARY_FILE, ec)) {
    std::ofstream out(root / LIBRARY_FILE, std::ios::trunc);
    out << json{{"format", "Open Animate asset library"}, {"version", LIBRARY_VERSION}}.dump(2) << "\n";
    if (!out) {
      report(ReportType::Error, "Can't write to " + folder);
      return false;
    }
  }
  preferences().asset_library = path_to_utf8(fs::absolute(root, ec).lexically_normal().make_preferred());
  preferences_save();
  brushes_reload();
  report(ReportType::Info, "Asset library: " + preferences().asset_library, false);
  return true;
}

std::string asset_library_shapes_dir()
{
  if (preferences().asset_library.empty()) {
    return "";
  }
  return path_to_utf8(path_from_utf8(preferences().asset_library) / SHAPES_DIR);
}

int asset_library_version()
{
  if (preferences().asset_library.empty()) {
    return 0;
  }
  std::ifstream in(path_from_utf8(preferences().asset_library) / LIBRARY_FILE, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const json j = json::parse(text, nullptr, false);
  return j.is_object() ? j.value("version", 0) : 0;
}

void asset_library_mark_current()
{
  if (preferences().asset_library.empty()) {
    return;
  }
  std::ofstream out(path_from_utf8(preferences().asset_library) / LIBRARY_FILE, std::ios::trunc);
  out << json{{"format", "Open Animate asset library"}, {"version", LIBRARY_VERSION}}.dump(2) << "\n";
}

}  // namespace oanim
