#include "assets/brushes.hh"
#include "assets/draw_presets.hh"
#include "assets/sculpt_presets.hh"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <regex>
#include <set>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "assets/asset_library.hh"
#include "assets/brush_presets.hh"
#include "core/log.hh"
#include "io/oanim_file.hh"

namespace oanim {

namespace fs = std::filesystem;
using json = nlohmann::json;

/* Images are scaled down to this size: plenty for dabs a line's width across, and it keeps a
 * library of large brushes small in memory. */
static constexpr int IMAGE_MAX_SIZE = 256;

static fs::path path_from_utf8(const std::string &path)
{
  return fs::path(std::u8string(path.begin(), path.end()));
}

static std::string path_to_utf8(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

static std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

static bool is_png(const fs::path &p)
{
  return lower(path_to_utf8(p.extension())) == ".png";
}

/* The PNGs of a multi-brush folder, in name order. */
static std::vector<std::string> folder_pngs(const fs::path &folder)
{
  std::vector<std::string> files;
  std::error_code ec;
  for (const fs::directory_entry &e : fs::directory_iterator(folder, ec)) {
    if (e.is_regular_file(ec) && is_png(e.path())) {
      files.push_back(path_to_utf8(e.path()));
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

/* --- Brush Shapes ------------------------------------------------------------------------- */

void brushes_scan(const std::string &folder, std::vector<Brush> &r_brushes)
{
  const fs::path root = path_from_utf8(folder);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    return;
  }
  std::vector<fs::path> entries;
  for (const fs::directory_entry &e : fs::directory_iterator(root, ec)) {
    entries.push_back(e.path());
  }
  std::sort(entries.begin(), entries.end());
  for (const fs::path &p : entries) {
    Brush brush;
    if (fs::is_regular_file(p, ec) && is_png(p)) {
      brush.name = path_to_utf8(p.stem());
      brush.files.push_back(path_to_utf8(p));
    }
    else if (fs::is_directory(p, ec)) {
      brush.name = path_to_utf8(p.filename());
      brush.files = folder_pngs(p);
    }
    if (brush.files.empty()) {
      continue;
    }
    brush.defaults.name = brush.name;
    r_brushes.push_back(std::move(brush));
  }
}

/* --- Upgrading a version 1 library ----------------------------------------------------------
 * It kept Moho's brushes in Brushes/: .mohobrush files (their settings and images) and plain PNGs
 * or folders (settings in the name, as Name_<align>_<jitter>_<spacing>). Each becomes a shape in
 * Brush Shapes, under its own name (so documents using it find it), and a brush file with its
 * settings in the new Brushes/, in a category by what it looks like (Moho's and Chad Kerns'
 * brushes; others go to Other). Once all of it is copied, the old folder goes. */

/* brush.json from a .mohobrush zip. Read into memory: miniz can't open non-ASCII paths. */
static json read_mohobrush(const fs::path &path)
{
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  mz_zip_archive zip{};
  if (bytes.empty() || !mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
    return json();
  }
  size_t size = 0;
  void *data = mz_zip_reader_extract_file_to_heap(&zip, "brush.json", &size, 0);
  mz_zip_reader_end(&zip);
  if (!data) {
    return json();
  }
  json j = json::parse(static_cast<const char *>(data), static_cast<const char *>(data) + size, nullptr, false);
  mz_free(data);
  return j;
}

static float get_float(const json &j, const char *key, float fallback)
{
  auto it = j.find(key);
  return (it != j.end() && it->is_number()) ? it->get<float>() : fallback;
}

static bool get_bool(const json &j, const char *key, bool fallback)
{
  auto it = j.find(key);
  return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

static BrushSettings settings_from_mohobrush(const json &j)
{
  BrushSettings b;
  b.align = get_bool(j, "align", b.align);
  b.jitter = get_float(j, "jitter", b.jitter);
  b.spacing = std::max(0.01f, get_float(j, "spacing", b.spacing));
  b.angle_drift = get_float(j, "angleDrift", b.angle_drift);
  b.randomize = get_bool(j, "randomize", b.randomize);
  b.random_order = get_bool(j, "randomOrder", b.random_order);
  b.merged_alpha = get_bool(j, "mergedAlpha", b.merged_alpha);
  b.size_variation_amp = std::max(0.0f, get_float(j, "sizeVariationAmp", b.size_variation_amp));
  b.size_variation_scale = std::max(0.001f, get_float(j, "sizeVariationScale", b.size_variation_scale));
  b.random_interval = std::max(1, int(get_float(j, "randomInterval", float(b.random_interval))));
  b.hue_drift = get_float(j, "hueDrift", b.hue_drift);
  b.sat_drift = get_float(j, "satDrift", b.sat_drift);
  b.val_drift = get_float(j, "valDrift", b.val_drift);
  return b;
}

/* An older brush named Name_<align>_<jitter degrees>_<spacing percent>: the settings it
 * carries in its name. Other names get the defaults. */
static BrushSettings settings_from_name(const std::string &name)
{
  BrushSettings b;
  static const std::regex pattern(R"(^.*_([01])_(\d+)_(\d+)$)");
  std::smatch m;
  if (std::regex_match(name, m, pattern)) {
    b.align = m[1] == "1";
    b.jitter = float(std::stoi(m[2])) * 3.14159265f / 180.0f;
    b.spacing = std::max(0.01f, float(std::stoi(m[3])) / 100.0f);
  }
  return b;
}

static void legacy_scan(const fs::path &root, std::vector<Brush> &r_brushes)
{
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    return;
  }
  std::vector<fs::path> entries;
  for (const fs::directory_entry &e : fs::directory_iterator(root, ec)) {
    entries.push_back(e.path());
  }
  std::sort(entries.begin(), entries.end());

  /* .mohobrush files first: they claim their images and folders. */
  std::set<fs::path> claimed;
  for (const fs::path &p : entries) {
    if (lower(path_to_utf8(p.extension())) != ".mohobrush") {
      continue;
    }
    const json j = read_mohobrush(p);
    if (!j.is_object()) {
      report(ReportType::Warning, "Can't read brush " + path_to_utf8(p));
      continue;
    }
    Brush brush;
    brush.name = path_to_utf8(p.stem());
    brush.defaults = settings_from_mohobrush(j);
    if (auto files = j.find("brushFiles"); files != j.end() && files->is_array()) {
      for (const json &f : *files) {
        const json ref = f.value("brushFileRef", json::object());
        const std::string rel = ref.value("path", std::string());
        if (rel.empty()) {
          continue;
        }
        const fs::path target = root / path_from_utf8(rel);
        claimed.insert(target);
        if (fs::is_directory(target, ec)) {
          const std::vector<std::string> pngs = folder_pngs(target);
          brush.files.insert(brush.files.end(), pngs.begin(), pngs.end());
        }
        else if (fs::is_regular_file(target, ec)) {
          brush.files.push_back(path_to_utf8(target));
        }
      }
    }
    if (!brush.files.empty()) {
      r_brushes.push_back(std::move(brush));
    }
  }
  /* Then older brushes: a lone PNG, or a folder of PNGs (a multi-brush). */
  for (const fs::path &p : entries) {
    if (claimed.count(p)) {
      continue;
    }
    Brush brush;
    brush.name = path_to_utf8(p.stem());
    if (fs::is_regular_file(p, ec) && is_png(p)) {
      brush.files.push_back(path_to_utf8(p));
    }
    else if (fs::is_directory(p, ec)) {
      brush.name = path_to_utf8(p.filename());
      brush.files = folder_pngs(p);
    }
    if (brush.files.empty()) {
      continue;
    }
    brush.defaults = settings_from_name(brush.name);
    r_brushes.push_back(std::move(brush));
  }
}


/* `name`, or "name.001"... while `taken` says it's used. */
static std::string free_name(const std::string &name, const std::function<bool(const std::string &)> &taken)
{
  if (!taken(name)) {
    return name;
  }
  for (int n = 1; n < 100000; n++) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%03d", n);
    if (!taken(name + suffix)) {
      return name + suffix;
    }
  }
  return name;
}

/* The preset group a converted brush goes in. */
static std::string upgrade_group(const std::string &name)
{
  const auto starts = [&](const char *prefix) { return name.rfind(prefix, 0) == 0; };
  if (starts("CK Hair") || name == "CK Hairy Toon") {
    return "Hair & Fur";
  }
  if (starts("CK Ink") || name == "CK Inky" || starts("MC-") || starts("CK Low Pressure") || starts("Water Edge")) {
    return "Ink";
  }
  if (name == "CK Grass") {
    return "Nature";
  }
  if (name == "CK Sketch Sponge m" || name == "Spray") {
    return "Texture";
  }
  if (name == "SimpleRound") {
    return "Basic";
  }
  if (name == "Dots" || name == "Rainbow Lens" || starts("sz_fluff") || starts("CK NotBoroque")) {
    return "Fun";
  }
  /* Moho's numbered brushes, by their number. */
  if (starts("Brush") && name.size() >= 8 && std::isdigit((unsigned char)name[5])) {
    const int n = std::atoi(name.c_str() + 5);
    const auto in = [n](std::initializer_list<int> list) { return std::find(list.begin(), list.end(), n) != list.end(); };
    if (in({508, 509, 548})) {
      return "Basic";
    }
    if (in({5, 564})) {
      return "Hair & Fur";
    }
    if (in({2, 505, 506, 515, 517, 519, 520, 521})) {
      return "Nature";
    }
    if ((n >= 511 && n <= 514) || n == 516 || (n >= 522 && n <= 547 && n != 525)) {
      return "Fun";
    }
    if (in({1, 4, 501, 502, 503, 504, 507, 510, 518, 525}) || (n >= 549 && n <= 573)) {
      return "Texture";
    }
  }
  return "Other";
}

/* A brush's name without the settings an older one carried in it ("Brush522_1_0_200": "Brush522"). */
static std::string upgrade_preset_name(const std::string &name)
{
  static const std::regex pattern(R"(^(.*)_[01]_\d+_\d+$)");
  std::smatch m;
  return std::regex_match(name, m, pattern) ? std::string(m[1]) : name;
}

static void library_upgrade()
{
  const std::string library_dir = preferences().asset_library;
  if (library_dir.empty()) {
    return;
  }
  if (asset_library_version() >= 2) {
    return;
  }
  const fs::path root = path_from_utf8(library_dir);
  std::error_code ec;
  if (!fs::is_directory(root / "Brushes", ec)) {
    asset_library_mark_current();
    return;
  }
  /* The old Brushes folder is set aside: the new one (brush files by category) takes its name. */
  fs::path old = root / "Brushes (old)";
  for (int n = 2; fs::exists(old, ec); n++) {
    old = root / path_from_utf8("Brushes (old " + std::to_string(n) + ")");
  }
  fs::rename(root / "Brushes", old, ec);
  if (ec) {
    report(ReportType::Warning, "Couldn't upgrade the brush library: " + ec.message());
    return;
  }
  std::vector<Brush> found;
  legacy_scan(old, found);
  const fs::path shapes = path_from_utf8(asset_library_shapes_dir());
  const fs::path presets_root = root / "Brushes";
  fs::create_directories(shapes, ec);
  int failed = 0, made_shapes = 0, made_presets = 0;
  for (const Brush &b : found) {
    const bool single = b.files.size() == 1;
    const std::string name = free_name(b.name, [&](const std::string &n) {
      return fs::exists(shapes / path_from_utf8(n + ".png"), ec) || fs::exists(shapes / path_from_utf8(n), ec);
    });
    bool ok = true;
    if (single) {
      ok = fs::copy_file(path_from_utf8(b.files[0]), shapes / path_from_utf8(name + ".png"), ec) && !ec;
    }
    else {
      const fs::path folder = shapes / path_from_utf8(name);
      fs::create_directories(folder, ec);
      for (const std::string &file : b.files) {
        const fs::path from = path_from_utf8(file);
        ok = ok && fs::copy_file(from, folder / from.filename(), ec) && !ec;
      }
    }
    if (!ok) {
      failed++;
      report(ReportType::Warning, "Couldn't copy brush " + b.name + " into Brush Shapes");
      continue;
    }
    made_shapes++;
    /* Its settings as a preset, in its group. */
    BrushSettings settings = b.defaults;
    settings.name = name;
    const fs::path presets = presets_root / path_from_utf8(upgrade_group(b.name));
    fs::create_directories(presets, ec);
    const std::string preset = free_name(upgrade_preset_name(b.name), [&](const std::string &n) {
      return fs::exists(presets / path_from_utf8(n + ".oabrush"), ec);
    });
    const json j = {{"version", 1},
                    {"name", preset},
                    {"brush", json::parse(brush_settings_to_json(settings), nullptr, false)}};
    std::ofstream out(presets / path_from_utf8(preset + ".oabrush"), std::ios::binary | std::ios::trunc);
    out << j.dump(1, '\t');
    if (out) {
      made_presets++;
    }
    else {
      failed++;
    }
  }
  asset_library_mark_current();
  if (failed == 0) {
    fs::remove_all(old, ec);
    report(ReportType::Info, "Brush library upgraded: " + std::to_string(made_shapes) + " shapes and " +
                                 std::to_string(made_presets) + " brushes, sorted into categories; the old Brushes folder was removed");
  }
  else {
    report(ReportType::Warning, "Brush library upgrade: " + std::to_string(failed) + " brushes couldn't be copied, so the old "
                                    "brushes were kept in " + path_to_utf8(old.filename()));
  }
}

static std::vector<Brush> &library()
{
  static std::vector<Brush> brushes;
  return brushes;
}

const std::vector<Brush> &brushes()
{
  return library();
}

static std::vector<Brush> &packed()
{
  static std::vector<Brush> brushes;
  return brushes;
}

const Brush *brush_find_in_library(const std::string &name)
{
  if (name.empty()) {
    return nullptr;
  }
  for (const Brush &b : library()) {
    if (b.name == name) {
      return &b;
    }
  }
  return nullptr;
}

static const Brush &round_tip()
{
  static const Brush b = [] {
    Brush r;
    r.name = BRUSH_ROUND_TIP;
    r.round_tip = true;
    r.loaded = true;
    /* Photoshop's Hard Round: tight spacing, so the line reads as a line. */
    r.defaults.name = BRUSH_ROUND_TIP;
    r.defaults.spacing = 0.1f;
    r.defaults.random_order = false;
    return r;
  }();
  return b;
}

std::string brush_display_name(const std::string &name)
{
  return name == BRUSH_ROUND_TIP ? std::string("Round") : name;
}

const std::vector<BrushImage> &brush_tip_images(const Brush &brush, const BrushSettings &settings)
{
  if (!brush.round_tip) {
    return brush_images(brush);
  }
  /* One image per 5% of hardness, drawn when first asked for. */
  static std::vector<BrushImage> levels[21];
  static uint64_t next_id = uint64_t(1) << 40;
  const int level = std::clamp(int(std::lround(settings.hardness * 20.0f)), 0, 20);
  std::vector<BrushImage> &images = levels[level];
  if (images.empty()) {
    BrushImage image;
    const int size = 128;
    image.width = image.height = size;
    image.rgba.resize(size_t(size) * size * 4);
    const float hard = float(level) / 20.0f;
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        const float dx = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f, dy = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
        const float r = std::sqrt(dx * dx + dy * dy);
        /* Solid inside `hard` of the radius, then easing out to the edge; the edge a pixel soft. */
        float a = r <= hard ? 1.0f : 1.0f - std::clamp((r - hard) / std::max(1.0f - hard, 1e-3f), 0.0f, 1.0f);
        a = a * a * (3.0f - 2.0f * a);
        a *= std::clamp((1.0f - r) * float(size) * 0.5f, 0.0f, 1.0f);
        const uint8_t v = uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        uint8_t *out = &image.rgba[(size_t(y) * size + x) * 4];
        out[0] = out[1] = out[2] = out[3] = v;
      }
    }
    image.id = next_id++;
    images.push_back(std::move(image));
  }
  return images;
}

static uint64_t g_brushes_generation = 1;

uint64_t brushes_generation()
{
  return g_brushes_generation;
}

const Brush *brush_find(const std::string &name)
{
  if (name == BRUSH_ROUND_TIP) {
    return &round_tip();
  }
  if (const Brush *b = brush_find_in_library(name)) {
    return b;
  }
  for (const Brush &b : packed()) {
    if (b.name == name && !name.empty()) {
      return &b;
    }
  }
  return nullptr;
}

void brushes_set_packed(std::vector<Brush> brushes)
{
  g_brushes_generation++;
  /* Keep the ones already loaded (their images and GPU textures), by name and bytes. */
  for (Brush &b : brushes) {
    for (Brush &old : packed()) {
      if (old.name == b.name && old.data == b.data) {
        b = std::move(old);
        break;
      }
    }
  }
  packed() = std::move(brushes);
}

bool brush_shape_bytes(const std::string &name, std::vector<std::string> &r_files, std::vector<std::string> &r_data)
{
  r_files.clear();
  r_data.clear();
  const Brush *brush = brush_find(name);
  if (!brush) {
    return false;
  }
  if (!brush->data.empty()) {
    for (size_t i = 0; i < brush->data.size(); i++) {
      r_files.push_back(std::to_string(i + 1) + ".png");
      r_data.push_back(brush->data[i]);
    }
    return true;
  }
  for (const std::string &file : brush->files) {
    std::ifstream in(path_from_utf8(file), std::ios::binary);
    if (!in) {
      return false;
    }
    r_files.push_back(path_to_utf8(path_from_utf8(file).filename()));
    r_data.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  return true;
}

void brushes_reload()
{
  g_brushes_generation++;
  library_upgrade();
  std::vector<Brush> found;
  brushes_scan(asset_library_shapes_dir(), found);
  std::sort(found.begin(), found.end(), [](const Brush &a, const Brush &b) { return lower(a.name) < lower(b.name); });
  /* Two shapes with one name (a PNG and a folder of the same name): the first wins. */
  found.erase(std::unique(found.begin(), found.end(), [](const Brush &a, const Brush &b) { return a.name == b.name; }),
              found.end());
  library() = std::move(found);
  library_brush_presets_reload();
  sculpt_presets_reload();
  draw_presets_reload();
}

/* Reads one image: ink brushes become white with coverage as alpha (dark ink on white, times
 * the image's own alpha); colour images keep their colours, premultiplied. */
static bool image_load(const std::string &file, const std::string *bytes, BrushImage &r_image, bool *r_color)
{
  SDL_Surface *loaded = bytes ? SDL_LoadPNG_IO(SDL_IOFromConstMem(bytes->data(), bytes->size()), true)
                              : SDL_LoadPNG(file.c_str());
  if (!loaded) {
    return false;
  }
  SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(loaded);
  if (!rgba) {
    return false;
  }
  const int longest = std::max(rgba->w, rgba->h);
  if (longest > IMAGE_MAX_SIZE) {
    const float f = float(IMAGE_MAX_SIZE) / float(longest);
    SDL_Surface *scaled = SDL_ScaleSurface(rgba, std::max(1, int(std::lround(rgba->w * f))),
                                           std::max(1, int(std::lround(rgba->h * f))), SDL_SCALEMODE_LINEAR);
    if (scaled) {
      SDL_DestroySurface(rgba);
      rgba = scaled;
    }
  }
  r_image.width = rgba->w;
  r_image.height = rgba->h;
  r_image.rgba.resize(size_t(rgba->w) * rgba->h * 4);

  /* Colour if a fair share of the visible pixels aren't grey. */
  int coloured = 0, visible = 0;
  for (int y = 0; y < rgba->h; y++) {
    const uint8_t *row = static_cast<const uint8_t *>(rgba->pixels) + size_t(y) * rgba->pitch;
    for (int x = 0; x < rgba->w; x++) {
      const uint8_t *px = row + x * 4;
      if (px[3] < 16) {
        continue;
      }
      visible++;
      const int spread = std::max({px[0], px[1], px[2]}) - std::min({px[0], px[1], px[2]});
      coloured += spread > 24 ? 1 : 0;
    }
  }
  const bool color = visible > 0 && coloured * 100 > visible;
  for (int y = 0; y < rgba->h; y++) {
    const uint8_t *row = static_cast<const uint8_t *>(rgba->pixels) + size_t(y) * rgba->pitch;
    for (int x = 0; x < rgba->w; x++) {
      const uint8_t *px = row + x * 4;
      uint8_t *out = &r_image.rgba[(size_t(y) * rgba->w + x) * 4];
      const float a = px[3] / 255.0f;
      if (color) {
        for (int c = 0; c < 3; c++) {
          out[c] = uint8_t(std::lround(px[c] * a));
        }
        out[3] = px[3];
      }
      else {
        const float lum = (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) / 255.0f;
        const uint8_t cov = uint8_t(std::lround(std::clamp((1.0f - lum) * a, 0.0f, 1.0f) * 255.0f));
        out[0] = out[1] = out[2] = out[3] = cov;
      }
    }
  }
  SDL_DestroySurface(rgba);
  *r_color = color;
  return true;
}

const std::vector<BrushImage> &brush_images(const Brush &brush)
{
  if (brush.round_tip) {
    return brush_tip_images(brush, brush.defaults);
  }
  if (!brush.loaded) {
    static uint64_t next_id = 1;
    brush.loaded = true;
    const size_t count = brush.data.empty() ? brush.files.size() : brush.data.size();
    for (size_t i = 0; i < count; i++) {
      BrushImage image;
      bool color = false;
      const std::string file = brush.data.empty() ? brush.files[i] : brush.name;
      if (image_load(file, brush.data.empty() ? nullptr : &brush.data[i], image, &color)) {
        image.id = next_id++;
        brush.is_color = brush.is_color || color;
        brush.images.push_back(std::move(image));
      }
      else {
        report(ReportType::Warning, "Can't read brush image " + file);
      }
    }
  }
  return brush.images;
}

}  // namespace oanim
