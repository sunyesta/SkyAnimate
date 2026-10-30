#include "assets/draw_presets.hh"

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

static constexpr const char *DRAW_DIR = "Draw Presets";
static constexpr const char *DRAW_EXT = ".oadraw";

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

static std::vector<DrawPreset> builtin_presets()
{
  const auto make = [](const char *name, float line_width, auto &&set) {
    DrawPreset p;
    p.name = name;
    p.line_width = line_width;
    p.category = DRAW_CATEGORY_PENS;
    p.builtin = true;
    set(p.ink);
    return p;
  };
  DrawPreset bezier;
  bezier.name = "Bezier Pen";
  bezier.mode = DrawMode::Pen;
  bezier.category = DRAW_CATEGORY_PENS;
  bezier.builtin = true;
  const auto area = [&](const char *name, DrawMode mode, float size, const char *category, auto &&set) {
    DrawPreset p = make(name, size, set);
    p.mode = mode;
    p.category = category;
    return p;
  };
  return {
      make("Ink", 0.008f, [](InkSettings &) {}),
      make("Pencil", 0.005f,
           [](InkSettings &ink) {
             ink.smoothing = 0.3f;
             ink.fidelity = 0.7f;
             ink.min_size = 0.4f;
             ink.tilt_width = 0.5f;
           }),
      make("Marker", 0.02f,
           [](InkSettings &ink) {
             ink.use_pressure = false;
             ink.smoothing = 0.6f;
           }),
      make("Tapered", 0.012f,
           [](InkSettings &ink) {
             ink.min_size = 0.1f;
             ink.taper_start = 40.0f;
             ink.taper_end = 80.0f;
           }),
      /* No pressure to go on: width comes from speed, as a brush lifting off when flicked. */
      make("Mouse", 0.012f,
           [](InkSettings &ink) {
             ink.use_pressure = false;
             ink.smoothing = 0.7f;
             ink.speed_width = 0.8f;
             ink.taper_start = 20.0f;
             ink.taper_end = 40.0f;
           }),
      area("Fill Pen", DrawMode::FillPen, 0.03f, DRAW_CATEGORY_PENS, [](InkSettings &ink) { ink.min_size = 0.3f; }),
      area("Fill Marker", DrawMode::FillPen, 0.06f, DRAW_CATEGORY_PENS,
           [](InkSettings &ink) {
             ink.use_pressure = false;
             ink.smoothing = 0.6f;
           }),
      bezier,
      area("Fill Eraser", DrawMode::FillEraser, 0.04f, DRAW_CATEGORY_ERASERS, [](InkSettings &ink) { ink.use_pressure = false; }),
      area("Pressure Fill Eraser", DrawMode::FillEraser, 0.06f, DRAW_CATEGORY_ERASERS, [](InkSettings &ink) { ink.min_size = 0.2f; }),
  };
}

static std::vector<DrawPreset> &all()
{
  static std::vector<DrawPreset> presets = builtin_presets();
  return presets;
}

static std::string library_dir()
{
  const std::string shapes = asset_library_shapes_dir();
  if (shapes.empty()) {
    return {};
  }
  return path_to_utf8(path_from_utf8(shapes).parent_path() / DRAW_DIR);
}

static const char *stabilizer_id(InkStabilizer s)
{
  return s == InkStabilizer::Off ? "OFF" : s == InkStabilizer::Smart ? "SMART" : "PULLED_STRING";
}

static json preset_json(const DrawPreset &p)
{
  const InkSettings &ink = p.ink;
  return {{"version", 1},
          {"name", p.name},
          {"mode", p.mode == DrawMode::FillPen ? "FILL_PEN" : p.mode == DrawMode::FillEraser ? "FILL_ERASER" : "STROKE_PEN"},
          {"lineWidth", p.line_width},
          {"stabilizer", stabilizer_id(ink.stabilizer)},
          {"smoothing", ink.smoothing},
          {"stringLength", ink.string_length},
          {"catchUp", ink.catch_up},
          {"fidelity", ink.fidelity},
          {"usePressure", ink.use_pressure},
          {"pressureSmoothing", ink.pressure_smoothing},
          {"pressureCurve", {ink.pressure_curve_a.x, ink.pressure_curve_a.y, ink.pressure_curve_b.x, ink.pressure_curve_b.y}},
          {"minSize", ink.min_size},
          {"speedWidth", ink.speed_width},
          {"tiltWidth", ink.tilt_width},
          {"taperStart", ink.taper_start},
          {"taperEnd", ink.taper_end}};
}

static DrawPreset preset_from_json(const json &j, const std::string &fallback_name)
{
  const InkSettings none;
  DrawPreset p;
  InkSettings &ink = p.ink;
  p.name = j.value("name", fallback_name);
  const std::string mode = j.value("mode", "STROKE_PEN");
  p.mode = mode == "FILL_PEN" ? DrawMode::FillPen : mode == "FILL_ERASER" ? DrawMode::FillEraser : DrawMode::Freehand;
  p.line_width = std::max(j.value("lineWidth", p.line_width), 0.0f);
  const std::string stabilizer = j.value("stabilizer", "SMART");
  ink.stabilizer = stabilizer == "OFF"             ? InkStabilizer::Off
                   : stabilizer == "PULLED_STRING" ? InkStabilizer::PulledString
                                                   : InkStabilizer::Smart;
  const auto amount = [&](const char *key, float fallback) { return std::clamp(j.value(key, fallback), 0.0f, 1.0f); };
  ink.smoothing = amount("smoothing", none.smoothing);
  ink.string_length = std::max(j.value("stringLength", none.string_length), 0.0f);
  ink.catch_up = j.value("catchUp", none.catch_up);
  ink.fidelity = amount("fidelity", none.fidelity);
  ink.use_pressure = j.value("usePressure", none.use_pressure);
  ink.pressure_smoothing = amount("pressureSmoothing", none.pressure_smoothing);
  if (const auto it = j.find("pressureCurve"); it != j.end() && it->is_array() && it->size() == 4) {
    const auto at = [&](size_t i) { return std::clamp(it->at(i).is_number() ? it->at(i).get<float>() : 0.0f, 0.0f, 1.0f); };
    ink.pressure_curve_a = {at(0), at(1)};
    ink.pressure_curve_b = {at(2), at(3)};
  }
  ink.min_size = amount("minSize", none.min_size);
  ink.speed_width = amount("speedWidth", none.speed_width);
  ink.tilt_width = amount("tiltWidth", none.tilt_width);
  ink.taper_start = std::max(j.value("taperStart", none.taper_start), 0.0f);
  ink.taper_end = std::max(j.value("taperEnd", none.taper_end), 0.0f);
  p.category = DRAW_CATEGORY_MINE;
  return p;
}

void draw_presets_reload()
{
  std::vector<DrawPreset> presets = builtin_presets();
  std::vector<DrawPreset> saved;
  const std::string dir = library_dir();
  std::error_code ec;
  if (!dir.empty() && fs::is_directory(path_from_utf8(dir), ec)) {
    for (const fs::directory_entry &entry : fs::directory_iterator(path_from_utf8(dir), ec)) {
      if (entry.path().extension() != DRAW_EXT) {
        continue;
      }
      std::ifstream in(entry.path(), std::ios::binary);
      const json j = json::parse(std::string(std::istreambuf_iterator<char>(in), {}), nullptr, false);
      if (!j.is_discarded() && j.is_object()) {
        saved.push_back(preset_from_json(j, path_to_utf8(entry.path().stem())));
      }
    }
  }
  std::sort(saved.begin(), saved.end(), [](const DrawPreset &a, const DrawPreset &b) { return a.name < b.name; });
  presets.insert(presets.end(), saved.begin(), saved.end());
  all() = std::move(presets);
}

const std::vector<DrawPreset> &draw_presets()
{
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    draw_presets_reload();
  }
  return all();
}

const DrawPreset *draw_preset_find(const std::string &name)
{
  for (const DrawPreset &p : draw_presets()) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

std::string draw_preset_save(const DrawPreset &preset, std::string *r_error)
{
  const std::string dir = library_dir();
  if (dir.empty()) {
    *r_error = "No asset library: choose one in Preferences to save pens";
    return {};
  }
  DrawPreset p = preset;
  p.builtin = false;
  p.category = DRAW_CATEGORY_MINE;
  p.name = brush_preset_unique_name(p.name.empty() ? std::string("Pen") : p.name,
                                    [](const std::string &n) { return draw_preset_find(n) != nullptr; });
  std::error_code ec;
  fs::create_directories(path_from_utf8(dir), ec);
  std::ofstream out(path_from_utf8(dir) / path_from_utf8(file_safe(p.name) + DRAW_EXT), std::ios::binary | std::ios::trunc);
  out << preset_json(p).dump(2);
  if (!out) {
    *r_error = "Couldn't write the pen to " + dir;
    return {};
  }
  out.close();
  draw_presets_reload();
  return p.name;
}

bool draw_preset_remove(const std::string &name, std::string *r_error)
{
  const DrawPreset *p = draw_preset_find(name);
  if (!p) {
    *r_error = "No pen named \"" + name + "\"";
    return false;
  }
  if (p->builtin) {
    *r_error = "\"" + name + "\" comes with the app and can't be deleted";
    return false;
  }
  std::error_code ec;
  fs::remove(path_from_utf8(library_dir()) / path_from_utf8(file_safe(name) + DRAW_EXT), ec);
  if (ec) {
    *r_error = "Couldn't delete \"" + name + "\": " + ec.message();
    return false;
  }
  draw_presets_reload();
  return true;
}

}  // namespace oanim
