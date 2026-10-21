#include "assets/brushes.hh"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "assets/asset_library.hh"
#include "core/log.hh"

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

static std::vector<Brush> &library()
{
  static std::vector<Brush> brushes;
  return brushes;
}

const std::vector<Brush> &brushes()
{
  return library();
}

const Brush *brush_find(const std::string &name)
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

void brushes_reload()
{
  std::vector<Brush> found;
  brushes_scan(asset_library_brushes_dir(), found);
  std::sort(found.begin(), found.end(), [](const Brush &a, const Brush &b) { return lower(a.name) < lower(b.name); });
  /* Two brushes with one name (a .mohobrush and a PNG of the same name): the first wins. */
  found.erase(std::unique(found.begin(), found.end(), [](const Brush &a, const Brush &b) { return a.name == b.name; }),
              found.end());
  library() = std::move(found);
}

/* Reads one image: ink brushes become white with coverage as alpha (dark ink on white, times
 * the image's own alpha); colour images keep their colours, premultiplied. */
static bool image_load(const std::string &file, BrushImage &r_image, bool *r_color)
{
  SDL_Surface *loaded = SDL_LoadPNG(file.c_str());
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
  if (!brush.loaded) {
    static uint64_t next_id = 1;
    brush.loaded = true;
    for (const std::string &file : brush.files) {
      BrushImage image;
      bool color = false;
      if (image_load(file, image, &color)) {
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

bool brush_save(const std::string &name, const BrushSettings &settings, std::string *r_error)
{
  const std::string dir = asset_library_brushes_dir();
  const Brush *source = brush_find(settings.name);
  if (dir.empty()) {
    *r_error = "Set up an asset library first (Edit > Preferences)";
    return false;
  }
  if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos) {
    *r_error = "Brush names can't be empty or use \\ / : * ? \" < > |";
    return false;
  }
  if (!source) {
    *r_error = "Pick a brush to save its settings as a new one";
    return false;
  }
  if (brush_find(name)) {
    *r_error = "There is already a brush called " + name;
    return false;
  }
  const fs::path root = path_from_utf8(dir);
  std::error_code ec;
  /* One image beside the brush, or a folder of them for a multi-brush. */
  std::string ref;
  if (source->files.size() == 1) {
    ref = name + ".png";
    fs::copy_file(path_from_utf8(source->files[0]), root / path_from_utf8(ref), ec);
  }
  else {
    ref = name;
    fs::create_directories(root / path_from_utf8(ref), ec);
    for (size_t i = 0; i < source->files.size() && !ec; i++) {
      fs::copy_file(path_from_utf8(source->files[i]), root / path_from_utf8(ref) / path_from_utf8(source->files[i]).filename(), ec);
    }
  }
  if (ec) {
    *r_error = "Couldn't copy the brush images: " + ec.message();
    return false;
  }
  const json j = {{"version", 1},
                  {"align", settings.align},
                  {"jitter", settings.jitter},
                  {"spacing", settings.spacing},
                  {"angleDrift", settings.angle_drift},
                  {"randomize", settings.randomize},
                  {"randomOrder", settings.random_order},
                  {"mergedAlpha", settings.merged_alpha},
                  {"sizeVariationAmp", settings.size_variation_amp},
                  {"sizeVariationScale", settings.size_variation_scale},
                  {"randomInterval", settings.random_interval},
                  {"hueDrift", settings.hue_drift},
                  {"satDrift", settings.sat_drift},
                  {"valDrift", settings.val_drift},
                  {"brushFiles", json::array({{{"brushFileRef", {{"relativeTo", "Project"}, {"path", ref}}}}})}};
  const std::string text = j.dump();
  mz_zip_archive zip{};
  void *buffer = nullptr;
  size_t size = 0;
  const bool built = mz_zip_writer_init_heap(&zip, 0, 0) &&
                     mz_zip_writer_add_mem(&zip, "brush.json", text.data(), text.size(), MZ_DEFAULT_LEVEL) &&
                     mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
  mz_zip_writer_end(&zip);
  if (built) {
    std::ofstream out(root / path_from_utf8(name + ".mohobrush"), std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char *>(buffer), std::streamsize(size));
    if (!out) {
      *r_error = "Couldn't write " + name + ".mohobrush";
      mz_free(buffer);
      return false;
    }
  }
  mz_free(buffer);
  if (!built) {
    *r_error = "Couldn't build " + name + ".mohobrush";
    return false;
  }
  brushes_reload();
  return true;
}

}  // namespace oanim
