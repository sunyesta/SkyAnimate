#include "assets/sculpt_presets.hh"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "assets/asset_library.hh"
#include "assets/brush_presets.hh"

namespace oanim {

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr const char *SCULPT_DIR = "Sculpt Brushes";
static constexpr const char *SCULPT_EXT = ".oasculpt";

static fs::path path_from_utf8(const std::string &path)
{
  return fs::path(std::u8string(path.begin(), path.end()));
}

static std::string path_to_utf8(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

/* A name safe as a file name: characters Windows refuses become "_". */
static std::string file_safe(std::string name)
{
  for (char &c : name) {
    if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) {
      c = '_';
    }
  }
  return name.empty() ? std::string("Preset") : name;
}

static std::vector<SculptPreset> builtin_presets()
{
  const auto make = [](const char *name, SculptBrush brush, float radius, float strength, SculptFalloff falloff,
                       const char *category) {
    SculptPreset p;
    p.name = name;
    p.brush = brush;
    p.radius = radius;
    p.strength = strength;
    p.falloff = falloff;
    p.category = category;
    p.builtin = true;
    return p;
  };
  using B = SculptBrush;
  using F = SculptFalloff;
  return {
      make("Grab", B::Grab, 60.0f, 1.0f, F::Smooth, SCULPT_CATEGORY_MOVE),
      make("Grab Soft", B::Grab, 90.0f, 0.5f, F::Smooth, SCULPT_CATEGORY_MOVE),
      make("Grab Big", B::Grab, 180.0f, 1.0f, F::Sphere, SCULPT_CATEGORY_MOVE),
      make("Push", B::Push, 50.0f, 0.6f, F::Smooth, SCULPT_CATEGORY_MOVE),
      make("Pinch", B::Pinch, 60.0f, 0.5f, F::Smooth, SCULPT_CATEGORY_MOVE),
      make("Twist", B::Twist, 70.0f, 0.5f, F::Smooth, SCULPT_CATEGORY_MOVE),
      make("Smooth", B::Smooth, 60.0f, 0.5f, F::Smooth, SCULPT_CATEGORY_SHAPE),
      make("Smooth Strong", B::Smooth, 80.0f, 1.0f, F::Sphere, SCULPT_CATEGORY_SHAPE),
      make("Thickness", B::Thickness, 40.0f, 0.5f, F::Smooth, SCULPT_CATEGORY_SHAPE),
  };
}

static std::vector<SculptPreset> &all()
{
  static std::vector<SculptPreset> presets = builtin_presets();
  return presets;
}

static std::string library_dir()
{
  const std::string shapes = asset_library_shapes_dir();
  if (shapes.empty()) {
    return {};
  }
  return path_to_utf8(path_from_utf8(shapes).parent_path() / SCULPT_DIR);
}

static json preset_json(const SculptPreset &p)
{
  return {{"version", 1},
          {"name", p.name},
          {"brush", sculpt_brush_id(p.brush)},
          {"radius", p.radius},
          {"strength", p.strength},
          {"falloff", sculpt_falloff_id(p.falloff)}};
}

void sculpt_presets_reload()
{
  std::vector<SculptPreset> presets = builtin_presets();
  std::vector<SculptPreset> saved;
  const std::string dir = library_dir();
  std::error_code ec;
  if (!dir.empty() && fs::is_directory(path_from_utf8(dir), ec)) {
    for (const fs::directory_entry &entry : fs::directory_iterator(path_from_utf8(dir), ec)) {
      if (entry.path().extension() != SCULPT_EXT) {
        continue;
      }
      std::ifstream in(entry.path(), std::ios::binary);
      const json j = json::parse(std::string(std::istreambuf_iterator<char>(in), {}), nullptr, false);
      if (j.is_discarded() || !j.is_object()) {
        continue;
      }
      SculptPreset p;
      p.name = j.value("name", path_to_utf8(entry.path().stem()));
      if (!sculpt_brush_from_id(j.value("brush", ""), &p.brush)) {
        continue;
      }
      sculpt_falloff_from_id(j.value("falloff", "SMOOTH"), &p.falloff);
      p.radius = std::clamp(j.value("radius", 50.0f), 2.0f, 1000.0f);
      p.strength = std::clamp(j.value("strength", 0.5f), 0.0f, 1.0f);
      p.category = SCULPT_CATEGORY_MINE;
      saved.push_back(p);
    }
  }
  std::sort(saved.begin(), saved.end(), [](const SculptPreset &a, const SculptPreset &b) { return a.name < b.name; });
  presets.insert(presets.end(), saved.begin(), saved.end());
  all() = std::move(presets);
}

const std::vector<SculptPreset> &sculpt_presets()
{
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    sculpt_presets_reload();
  }
  return all();
}

const SculptPreset *sculpt_preset_find(const std::string &name)
{
  for (const SculptPreset &p : sculpt_presets()) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

const SculptPreset &sculpt_preset_default(SculptBrush brush)
{
  for (const SculptPreset &p : sculpt_presets()) {
    if (p.builtin && p.brush == brush) {
      return p;
    }
  }
  return sculpt_presets().front();
}

std::string sculpt_preset_save(const SculptPreset &preset, std::string *r_error)
{
  const std::string dir = library_dir();
  if (dir.empty()) {
    *r_error = "No asset library: choose one in Preferences to save brushes";
    return {};
  }
  SculptPreset p = preset;
  p.builtin = false;
  p.category = SCULPT_CATEGORY_MINE;
  p.name = brush_preset_unique_name(p.name.empty() ? std::string(sculpt_brush_name(p.brush)) : p.name,
                                    [](const std::string &n) { return sculpt_preset_find(n) != nullptr; });
  std::error_code ec;
  fs::create_directories(path_from_utf8(dir), ec);
  std::ofstream out(path_from_utf8(dir) / path_from_utf8(file_safe(p.name) + SCULPT_EXT), std::ios::binary | std::ios::trunc);
  out << preset_json(p).dump(2);
  if (!out) {
    *r_error = "Couldn't write the brush to " + dir;
    return {};
  }
  out.close();
  sculpt_presets_reload();
  return p.name;
}

bool sculpt_preset_remove(const std::string &name, std::string *r_error)
{
  const SculptPreset *p = sculpt_preset_find(name);
  if (!p) {
    *r_error = "No brush named \"" + name + "\"";
    return false;
  }
  if (p->builtin) {
    *r_error = "\"" + name + "\" comes with the app and can't be deleted";
    return false;
  }
  std::error_code ec;
  fs::remove(path_from_utf8(library_dir()) / path_from_utf8(file_safe(name) + SCULPT_EXT), ec);
  if (ec) {
    *r_error = "Couldn't delete \"" + name + "\": " + ec.message();
    return false;
  }
  sculpt_presets_reload();
  return true;
}

}  // namespace oanim
