#include "io/svg_export.hh"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <SDL3/SDL.h>

#include "io/vector_export.hh"

namespace oanim {

namespace {

std::string hex(const Color &c)
{
  const auto byte = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  char text[8];
  std::snprintf(text, sizeof(text), "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
  return text;
}

std::string base64(const uint8_t *data, size_t size)
{
  static const char *ABC = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t n = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0u) |
                       (i + 2 < size ? uint32_t(data[i + 2]) : 0u);
    out += ABC[n >> 18 & 63];
    out += ABC[n >> 12 & 63];
    out += i + 1 < size ? ABC[n >> 6 & 63] : '=';
    out += i + 2 < size ? ABC[n & 63] : '=';
  }
  return out;
}

/* A picture as a PNG, base64. */
std::string png_base64(const VectorPicture &picture)
{
  std::vector<uint8_t> rgba = picture.rgba;
  SDL_Surface *surface = SDL_CreateSurfaceFrom(picture.width, picture.height, SDL_PIXELFORMAT_ABGR8888, rgba.data(),
                                               picture.width * 4);
  SDL_IOStream *io = SDL_IOFromDynamicMem();
  std::string png;
  if (surface && io && SDL_SavePNG_IO(surface, io, false)) {
    const auto *data = static_cast<const uint8_t *>(
        SDL_GetPointerProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
    const Sint64 size = SDL_GetIOSize(io);
    if (data && size > 0) {
      png = base64(data, size_t(size));
    }
  }
  if (io) {
    SDL_CloseIO(io);
  }
  if (surface) {
    SDL_DestroySurface(surface);
  }
  return png;
}

std::string path_data(const VectorPath &path)
{
  const auto &num = vector_number;
  std::string d;
  size_t p = 0;
  VectorPath::Op last = VectorPath::Close;
  for (const VectorPath::Op op : path.ops) {
    switch (op) {
      case VectorPath::Move:
        d += "M" + num(path.points[p].x) + " " + num(path.points[p].y);
        p++;
        break;
      case VectorPath::Line:
        d += (last == VectorPath::Line || last == VectorPath::Move ? (last == VectorPath::Move ? "L" : " ") : "L") +
             num(path.points[p].x) + " " + num(path.points[p].y);
        p++;
        break;
      case VectorPath::Cubic:
        d += "C";
        for (int k = 0; k < 3; k++) {
          d += (k ? " " : "") + num(path.points[p].x) + " " + num(path.points[p].y);
          p++;
        }
        break;
      case VectorPath::Close:
        d += "Z";
        break;
    }
    last = op;
  }
  return d;
}

std::string mask_attr(int mask)
{
  return mask >= 0 ? " mask=\"url(#m" + std::to_string(mask) + ")\"" : "";
}

void write_nodes(const VectorScene &scene, const std::vector<VectorNode> &nodes, std::string &out);

void write_node(const VectorScene &scene, const VectorNode &n, std::string &out)
{
  const auto &num = vector_number;
  switch (n.kind) {
    case VectorNode::Path: {
      if (n.path.empty()) {
        return;
      }
      out += "<path d=\"" + path_data(n.path) + "\" fill=\"" + hex(n.color) + "\"";
      const float alpha = n.color.a * n.alpha;
      if (alpha < 0.9995f) {
        out += " fill-opacity=\"" + num(std::max(alpha, 0.0f)) + "\"";
      }
      if (n.even_odd) {
        out += " fill-rule=\"evenodd\"";
      }
      out += mask_attr(n.mask) + "/>\n";
      return;
    }
    case VectorNode::Picture: {
      const std::string png = png_base64(scene.pictures[size_t(n.picture)]);
      if (png.empty()) {
        return;
      }
      /* The mask in the picture's group: an element's own transform would move it. */
      out += "<g" + mask_attr(n.mask);
      if (n.alpha < 0.9995f) {
        out += " opacity=\"" + num(n.alpha) + "\"";
      }
      const Transform2D &t = n.place;
      out += "><image x=\"0\" y=\"0\" width=\"1\" height=\"1\" preserveAspectRatio=\"none\" transform=\"matrix(" +
             num(t.a) + " " + num(t.b) + " " + num(t.c) + " " + num(t.d) + " " + num(t.tx) + " " + num(t.ty) +
             ")\" xlink:href=\"data:image/png;base64," + png + "\"/></g>\n";
      return;
    }
    case VectorNode::Group: {
      /* Only what fades or masks needs a group of its own. */
      const bool own = n.mask >= 0 || n.opacity < 0.9995f;
      if (own) {
        out += "<g" + mask_attr(n.mask);
        if (n.opacity < 0.9995f) {
          out += " opacity=\"" + num(n.opacity) + "\"";
        }
        out += ">\n";
      }
      write_nodes(scene, n.children, out);
      if (own) {
        out += "</g>\n";
      }
      return;
    }
  }
}

void write_nodes(const VectorScene &scene, const std::vector<VectorNode> &nodes, std::string &out)
{
  for (const VectorNode &n : nodes) {
    write_node(scene, n, out);
  }
}

}  // namespace

std::string svg_frame(const Document &doc, const SvgExport &options, SvgExportReport *r_report)
{
  VectorSceneOptions scene_options;
  scene_options.width = options.width;
  scene_options.height = options.height;
  scene_options.transparent = options.transparent;
  const VectorScene scene = vector_scene(doc, scene_options);
  const std::string w = std::to_string(scene.width), h = std::to_string(scene.height);
  std::string defs;
  for (size_t i = 0; i < scene.masks.size(); i++) {
    defs += "<mask id=\"m" + std::to_string(i) + "\" maskUnits=\"userSpaceOnUse\" x=\"-" + w + "\" y=\"-" + h +
            "\" width=\"" + std::to_string(scene.width * 3) + "\" height=\"" + std::to_string(scene.height * 3) +
            "\">\n";
    write_nodes(scene, scene.masks[i].content, defs);
    defs += "</mask>\n";
  }
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" + w +
         "\" height=\"" + h + "\" viewBox=\"0 0 " + w + " " + h + "\">\n";
  if (!defs.empty()) {
    out += "<defs>\n" + defs + "</defs>\n";
  }
  write_nodes(scene, scene.nodes, out);
  out += "</svg>\n";
  if (r_report) {
    r_report->fills = scene.fills;
    r_report->lines = scene.lines;
    r_report->masks = int(scene.masks.size());
    r_report->brushed = scene.brushed;
    int pictures = int(scene.pictures.size());
    r_report->pictures = pictures - scene.brushed;
  }
  return out;
}

bool svg_export_frame(const Document &doc, const std::string &path, const SvgExport &options, std::string *r_error,
                      SvgExportReport *r_report)
{
  const std::string text = svg_frame(doc, options, r_report);
  std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())),
                    std::ios::binary | std::ios::trunc);
  out.write(text.data(), std::streamsize(text.size()));
  out.close();
  if (!out) {
    if (r_error) {
      *r_error = "Could not write " + path;
    }
    return false;
  }
  return true;
}

}  // namespace oanim
