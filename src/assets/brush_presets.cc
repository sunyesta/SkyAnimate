#include "assets/brush_presets.hh"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "assets/asset_library.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "io/oanim_file.hh"

namespace oanim {

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr const char *PRESETS_DIR = "Brushes";
static constexpr const char *BRUSH_EXT = ".oabrush";

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

static std::vector<BrushPreset> &library()
{
  static std::vector<BrushPreset> presets;
  return presets;
}

std::string asset_library_presets_dir()
{
  const std::string shapes = asset_library_shapes_dir();
  if (shapes.empty()) {
    return {};
  }
  return path_to_utf8(path_from_utf8(shapes).parent_path() / PRESETS_DIR);
}

const std::vector<BrushPreset> &library_brush_presets()
{
  return library();
}

const BrushPreset *library_brush_preset_find(const std::string &name)
{
  for (const BrushPreset &p : library()) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

void library_brush_presets_reload()
{
  std::vector<BrushPreset> found;
  const std::string dir = asset_library_presets_dir();
  std::error_code ec;
  if (!dir.empty() && fs::is_directory(path_from_utf8(dir), ec)) {
    const fs::path root = path_from_utf8(dir);
    for (const fs::directory_entry &e : fs::recursive_directory_iterator(root, ec)) {
      if (!e.is_regular_file(ec) || e.path().extension() != BRUSH_EXT) {
        continue;
      }
      std::ifstream in(e.path(), std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const json j = json::parse(text, nullptr, false);
      if (!j.is_object()) {
        report(ReportType::Warning, "Can't read brush preset " + path_to_utf8(e.path()));
        continue;
      }
      BrushPreset p;
      p.name = j.value("name", path_to_utf8(e.path().stem()));
      p.group = path_to_utf8(e.path().parent_path().lexically_relative(root));
      if (p.group == ".") {
        p.group.clear();
      }
      if (auto it = j.find("brush"); it != j.end() && it->is_object()) {
        brush_settings_from_json(it->dump(), &p.settings);
      }
      found.push_back(std::move(p));
    }
  }
  std::sort(found.begin(), found.end(), [](const BrushPreset &a, const BrushPreset &b) {
    return a.group != b.group ? a.group < b.group : a.name < b.name;
  });
  /* Built in, first: Photoshop's General Brushes, on the Round tip the app draws. */
  std::vector<BrushPreset> all;
  for (const float hardness : {1.0f, 0.0f}) {
    BrushPreset p;
    p.name = hardness > 0.5f ? "Hard Round" : "Soft Round";
    p.group = "Basic";
    p.builtin = true;
    p.settings.name = BRUSH_ROUND_TIP;
    p.settings.hardness = hardness;
    p.settings.spacing = 0.1f;
    p.settings.random_order = false;
    all.push_back(std::move(p));
  }
  all.insert(all.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
  library() = std::move(all);
}

std::string brush_preset_unique_name(const std::string &wanted, const std::function<bool(const std::string &)> &taken)
{
  const std::string base_wanted = wanted.empty() ? std::string("Preset") : wanted;
  if (!taken(base_wanted)) {
    return base_wanted;
  }
  std::string base = base_wanted;
  const size_t dot = base.rfind('.');
  if (dot != std::string::npos && dot + 4 == base.size() &&
      base.find_first_not_of("0123456789", dot + 1) == std::string::npos)
  {
    base = base.substr(0, dot);
  }
  for (int n = 1; n < 100000; n++) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%03d", n);
    if (!taken(base + suffix)) {
      return base + suffix;
    }
  }
  return base;
}

BrushPreset *document_brush_preset_find(Document &doc, const std::string &name)
{
  for (BrushPreset &p : doc.brush_presets) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

BrushPreset &document_brush_preset_add(Document &doc, const std::string &name, const BrushSettings &settings)
{
  BrushPreset p;
  p.name = brush_preset_unique_name(name, [&](const std::string &n) { return document_brush_preset_find(doc, n) != nullptr; });
  p.settings = settings;
  /* A copy of the shape's images, so the file carries them. */
  if (!settings.name.empty()) {
    brush_shape_bytes(settings.name, p.shape_files, p.shape_data);
  }
  doc.brush_presets.push_back(std::move(p));
  return doc.brush_presets.back();
}

std::vector<std::string> library_brush_categories()
{
  std::vector<std::string> categories;
  for (const BrushPreset &p : library()) {
    if (!p.group.empty() && std::find(categories.begin(), categories.end(), p.group) == categories.end()) {
      categories.push_back(p.group);
    }
  }
  /* Folders with nothing in them yet count too. */
  const std::string dir = asset_library_presets_dir();
  std::error_code ec;
  if (!dir.empty()) {
    for (const fs::directory_entry &e : fs::directory_iterator(path_from_utf8(dir), ec)) {
      const std::string name = path_to_utf8(e.path().filename());
      if (e.is_directory(ec) && std::find(categories.begin(), categories.end(), name) == categories.end()) {
        categories.push_back(name);
      }
    }
  }
  std::sort(categories.begin(), categories.end());
  return categories;
}

std::string library_brush_preset_save(const BrushPreset &preset, const std::string &category_wanted,
                                      std::string *r_error, bool *r_shape_copied)
{
  const std::string dir_root = asset_library_presets_dir();
  const std::string brushes_dir = asset_library_shapes_dir();
  if (dir_root.empty() || brushes_dir.empty()) {
    *r_error = "Set up an asset library first (Edit > Preferences)";
    return {};
  }
  /* Library brushes always have a category: a folder of Brushes. */
  std::string category = category_wanted;
  while (!category.empty() && (category.back() == ' ' || category.back() == '.')) {
    category.pop_back();
  }
  category = category.empty() ? std::string(BRUSH_CATEGORY_DEFAULT) : file_safe(category);
  const std::string dir = path_to_utf8(path_from_utf8(dir_root) / path_from_utf8(category));
  if (r_shape_copied) {
    *r_shape_copied = false;
  }
  std::error_code ec;
  fs::create_directories(path_from_utf8(dir), ec);
  BrushPreset saved = preset;
  saved.group.clear();
  saved.settings.preset.clear();
  /* The shape: the library's own when it has the same images under that name; otherwise the copy
   * goes in, under a new name if that one is taken by other images. */
  if (!preset.settings.name.empty() && !preset.shape_data.empty()) {
    const Brush *existing = brush_find_in_library(preset.settings.name);
    bool same = false;
    if (existing) {
      std::vector<std::string> files, data;
      same = brush_shape_bytes(existing->name, files, data) && data == preset.shape_data;
    }
    if (!same) {
      const std::string shape = existing ? brush_preset_unique_name(preset.settings.name, [](const std::string &n) {
                                             return brush_find_in_library(n) != nullptr;
                                           })
                                         : preset.settings.name;
      const fs::path root = path_from_utf8(brushes_dir);
      if (preset.shape_data.size() == 1) {
        std::ofstream out(root / path_from_utf8(file_safe(shape) + ".png"), std::ios::binary | std::ios::trunc);
        out.write(preset.shape_data[0].data(), std::streamsize(preset.shape_data[0].size()));
      }
      else {
        const fs::path folder = root / path_from_utf8(file_safe(shape));
        fs::create_directories(folder, ec);
        for (size_t i = 0; i < preset.shape_data.size(); i++) {
          std::ofstream out(folder / path_from_utf8(preset.shape_files[i]), std::ios::binary | std::ios::trunc);
          out.write(preset.shape_data[i].data(), std::streamsize(preset.shape_data[i].size()));
        }
      }
      saved.settings.name = file_safe(shape);
      if (r_shape_copied) {
        *r_shape_copied = true;
      }
    }
  }
  saved.name = brush_preset_unique_name(preset.name, [](const std::string &n) { return library_brush_preset_find(n) != nullptr; });
  const json j = {{"version", 1},
                  {"name", saved.name},
                  {"brush", json::parse(brush_settings_to_json(saved.settings), nullptr, false)}};
  std::ofstream out(path_from_utf8(dir) / path_from_utf8(file_safe(saved.name) + BRUSH_EXT), std::ios::binary | std::ios::trunc);
  out << j.dump(1, '\t');
  if (!out) {
    *r_error = "Couldn't write the preset into " + dir;
    return {};
  }
  out.close();
  brushes_reload();
  return saved.name;
}

bool library_brush_preset_remove(const std::string &name, std::string *r_error)
{
  const std::string dir = asset_library_presets_dir();
  if (dir.empty() || !library_brush_preset_find(name)) {
    *r_error = "There's no library preset \"" + name + "\"";
    return false;
  }
  if (library_brush_preset_find(name)->builtin) {
    *r_error = "\"" + name + "\" is built in";
    return false;
  }
  std::error_code ec;
  /* The file named after it (in its group's folder), or the one whose name field says it. */
  const BrushPreset *found = library_brush_preset_find(name);
  bool removed = fs::remove(path_from_utf8(dir) / path_from_utf8(found->group) / path_from_utf8(file_safe(name) + BRUSH_EXT), ec);
  if (!removed) {
    for (const fs::directory_entry &e : fs::recursive_directory_iterator(path_from_utf8(dir), ec)) {
      if (!e.is_regular_file(ec)) {
        continue;
      }
      std::ifstream in(e.path(), std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      const json j = json::parse(text, nullptr, false);
      if (j.is_object() && j.value("name", std::string()) == name) {
        removed = fs::remove(e.path(), ec);
        break;
      }
    }
  }
  if (!removed) {
    *r_error = "Couldn't delete the preset's file";
    return false;
  }
  library_brush_presets_reload();
  return true;
}

void brush_presets_register_shapes(const Document &doc)
{
  /* Called every frame: only redone when the presets' shapes change (names and image sizes tell). */
  static std::string last;
  std::string signature;
  for (const BrushPreset &p : doc.brush_presets) {
    signature += p.settings.name;
    signature += '';
    for (const std::string &d : p.shape_data) {
      signature += std::to_string(d.size()) + ",";
    }
    signature += '';
  }
  if (signature == last) {
    return;
  }
  last = signature;
  std::vector<Brush> shapes;
  for (const BrushPreset &p : doc.brush_presets) {
    if (p.settings.name.empty() || p.shape_data.empty()) {
      continue;
    }
    const bool listed = std::any_of(shapes.begin(), shapes.end(), [&](const Brush &b) { return b.name == p.settings.name; });
    if (listed) {
      continue;
    }
    Brush b;
    b.name = p.settings.name;
    b.defaults = p.settings;
    b.data = p.shape_data;
    shapes.push_back(std::move(b));
  }
  brushes_set_packed(std::move(shapes));
}

}  // namespace oanim
