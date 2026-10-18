#include "io/sky_file.hh"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>

#include <miniz.h>
#include <nlohmann/json.hpp>

namespace sky {

using json = nlohmann::ordered_json;

static constexpr const char *DOCUMENT_ENTRY = "document.json";
static constexpr const char *FORMAT_NAME = "SkyAnimate";

static std::filesystem::path path_from_utf8(const std::string &path)
{
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

/* ---------------------------------------------------------------------------------
 * Writing
 */

/* Floats rounded to 7 significant digits, so 0.3f is written as 0.3 rather than
 * 0.30000001192092896 and files stay readable. */
static double num(float value)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%.7g", double(value));
  return std::strtod(text, nullptr);
}

static json color_to_json(const Color &c)
{
  return json::array({num(c.r), num(c.g), num(c.b), num(c.a)});
}

static json float2_to_json(const float2 &v)
{
  return json::array({num(v.x), num(v.y)});
}

/* Enums are saved by their RNA identifier ("ADD_INVISIBLE"), the same names Python uses,
 * so reordering an enum never changes what a file means. */
static std::string enum_identifier(const char *prop_id, int value)
{
  const rna::PropertyRNA *prop = Layer::rna().find(prop_id);
  return (value >= 0 && value < int(prop->items.size())) ? prop->items[value].identifier : prop->items[0].identifier;
}

static json style_to_json(const Style &style)
{
  return {{"fill", color_to_json(style.fill)}, {"line", color_to_json(style.line)}, {"line_width", num(style.line_width)}};
}

static json drawing_to_json(const VectorDrawing &drawing)
{
  json points = json::array();
  for (const MeshPoint &p : drawing.points) {
    json point = {{"pos", float2_to_json(p.pos)}};
    if (p.radius != 1.0f) {
      point["radius"] = num(p.radius);
    }
    if (p.corner == CornerType::Sharp) {
      point["corner"] = "SHARP";
    }
    points.push_back(std::move(point));
  }
  json curves = json::array();
  for (const Curve &curve : drawing.curves) {
    json curve_points = json::array();
    for (const CurvePoint &cp : curve.points) {
      json point = {{"point", cp.point},
                    {"curvature", num(cp.curvature)},
                    {"weight_in", num(cp.weight_in)},
                    {"weight_out", num(cp.weight_out)},
                    {"offset_in", num(cp.offset_in)},
                    {"offset_out", num(cp.offset_out)}};
      if (cp.edge_hidden) {
        point["edge_hidden"] = true;
      }
      curve_points.push_back(std::move(point));
    }
    curves.push_back({{"closed", curve.closed}, {"points", std::move(curve_points)}});
  }
  json shapes = json::array();
  for (const Shape &shape : drawing.shapes) {
    json edges = json::array();
    for (const ShapeEdge &edge : shape.edges) {
      edges.push_back(json::array({edge.curve, edge.segment}));
    }
    shapes.push_back({{"name", shape.name},
                      {"fill", shape.has_fill},
                      {"outline", shape.has_outline},
                      {"style", style_to_json(shape.style)},
                      {"edges", std::move(edges)}});
  }
  return {{"points", std::move(points)}, {"curves", std::move(curves)}, {"shapes", std::move(shapes)}};
}

static json layer_to_json(const Layer &layer)
{
  json j = {
      {"id", layer.id},
      {"name", layer.name},
      {"type", enum_identifier("type", int(layer.type))},
      {"visible", layer.visible},
      {"locked", layer.locked},
      {"expanded", layer.expanded},
      {"label_color", enum_identifier("label_color", int(layer.label_color))},
      {"position", float2_to_json(layer.position)},
      {"rotation", num(layer.rotation)},
      {"scale", float2_to_json(layer.scale)},
      {"opacity", num(layer.opacity)},
      {"mask_mode", enum_identifier("mask_mode", int(layer.mask_mode))},
      {"group_mask", enum_identifier("group_mask", int(layer.group_mask))},
  };
  if (layer.is_reference()) {
    /* A reference's drawing lives in its source; storing a copy would only go stale. */
    j["reference_source"] = layer.reference_source;
  }
  else if (!layer.is_group()) {
    j["drawing"] = drawing_to_json(layer.drawing);
  }
  if (layer.is_group()) {
    json children = json::array();
    for (const Layer &child : layer.children) {
      children.push_back(layer_to_json(child));
    }
    j["children"] = std::move(children);
  }
  return j;
}

std::string sky_document_to_json(const Document &doc)
{
  json texts = json::array();
  for (const Text &text : doc.texts) {
    texts.push_back({{"name", text.name}, {"body", text.body}, {"filepath", text.filepath}});
  }
  json layers = json::array();
  for (const Layer &layer : doc.layers) {
    layers.push_back(layer_to_json(layer));
  }
  const json root = {
      {"format", FORMAT_NAME},
      {"version", SKY_FILE_VERSION},
      {"document",
       {
           {"name", doc.name},
           {"frame_start", doc.frame_start},
           {"frame_end", doc.frame_end},
           {"frame_current", doc.frame_current},
           {"fps", num(doc.fps)},
           {"width", doc.width},
           {"height", doc.height},
           {"background", color_to_json(doc.background)},
           {"viewport_quality", doc.viewport_quality},
           {"render_quality", doc.render_quality},
           {"action", doc.action},
           {"active_layer", doc.active_layer},
           {"next_layer_id", doc.next_layer_id},
           {"layers", std::move(layers)},
           {"texts", std::move(texts)},
       }},
  };
  return root.dump(1, '\t');
}

static bool file_write_bytes(const std::filesystem::path &path, const void *data, size_t size)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(static_cast<const char *>(data), std::streamsize(size));
  out.close();
  return bool(out);
}

bool sky_file_write(const Document &doc, const std::string &path, std::string *r_error)
{
  const std::string text = sky_document_to_json(doc);

  /* Build the archive in memory: miniz's file functions can't open non-ASCII Windows paths. */
  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
    *r_error = "Could not start the archive";
    return false;
  }
  void *buffer = nullptr;
  size_t size = 0;
  const bool built = mz_zip_writer_add_mem(&zip, DOCUMENT_ENTRY, text.data(), text.size(), MZ_DEFAULT_LEVEL) &&
                     mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
  mz_zip_writer_end(&zip);
  if (!built) {
    mz_free(buffer);
    *r_error = "Could not compress the document";
    return false;
  }

  const std::filesystem::path target = path_from_utf8(path);
  std::filesystem::path temp = target;
  temp += u8"@";
  const bool written = file_write_bytes(temp, buffer, size);
  mz_free(buffer);
  std::error_code ec;
  if (!written) {
    std::filesystem::remove(temp, ec);
    *r_error = "Could not write " + path;
    return false;
  }
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    *r_error = "Could not replace " + path + ": " + ec.message();
    return false;
  }
  return true;
}

/* ---------------------------------------------------------------------------------
 * Reading
 *
 * Every value has a default, so files written before a property existed still open.
 */

static float get_float(const json &j, const char *key, float fallback)
{
  auto it = j.find(key);
  return (it != j.end() && it->is_number()) ? it->get<float>() : fallback;
}

static int get_int(const json &j, const char *key, int fallback)
{
  auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

static uint64_t get_id(const json &j, const char *key)
{
  auto it = j.find(key);
  return (it != j.end() && it->is_number_unsigned()) ? it->get<uint64_t>() : 0;
}

static bool get_bool(const json &j, const char *key, bool fallback)
{
  auto it = j.find(key);
  return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

static std::string get_string(const json &j, const char *key, const std::string &fallback = "")
{
  auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

static float2 get_float2(const json &j, const char *key, float2 fallback)
{
  auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) {
    return fallback;
  }
  return {(*it)[0].get<float>(), (*it)[1].get<float>()};
}

static Color get_color(const json &j, const char *key, Color fallback)
{
  auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 4) {
    return fallback;
  }
  for (const json &v : *it) {
    if (!v.is_number()) {
      return fallback;
    }
  }
  return {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>(), (*it)[3].get<float>()};
}

template<typename E> static E get_enum(const json &j, const char *key, E fallback)
{
  const int index = Layer::rna().find(key)->enum_index(get_string(j, key));
  return index >= 0 ? E(index) : fallback;
}

static const json &get_array(const json &j, const char *key)
{
  static const json empty = json::array();
  auto it = j.find(key);
  return (it != j.end() && it->is_array()) ? *it : empty;
}

static Style style_from_json(const json &j)
{
  Style style;
  if (!j.is_object()) {
    return style;
  }
  style.fill = get_color(j, "fill", style.fill);
  style.line = get_color(j, "line", style.line);
  style.line_width = std::max(0.0f, get_float(j, "line_width", style.line_width));
  return style;
}

static VectorDrawing drawing_from_json(const json &j)
{
  VectorDrawing drawing;
  if (!j.is_object()) {
    return drawing;
  }
  for (const json &p : get_array(j, "points")) {
    if (p.is_object()) {
      drawing.points.push_back({get_float2(p, "pos", {}), std::clamp(get_float(p, "radius", 1.0f), 0.0f, 1.0f),
                                false, get_string(p, "corner") == "SHARP" ? CornerType::Sharp : CornerType::Round});
    }
  }
  const int point_count = int(drawing.points.size());
  for (const json &c : get_array(j, "curves")) {
    if (!c.is_object()) {
      continue;
    }
    Curve curve;
    curve.closed = get_bool(c, "closed", false);
    for (const json &cp : get_array(c, "points")) {
      if (!cp.is_object()) {
        continue;
      }
      CurvePoint point;
      point.point = get_int(cp, "point", -1);
      if (point.point < 0 || point.point >= point_count) {
        continue;
      }
      point.curvature = get_float(cp, "curvature", CURVATURE_SMOOTH);
      point.weight_in = get_float(cp, "weight_in", 1.0f);
      point.weight_out = get_float(cp, "weight_out", 1.0f);
      point.offset_in = get_float(cp, "offset_in", 0.0f);
      point.offset_out = get_float(cp, "offset_out", 0.0f);
      point.edge_hidden = get_bool(cp, "edge_hidden", false);
      curve.points.push_back(point);
    }
    /* Keep curve indices stable for the shapes even if a curve came out too short; it then
     * just has no segments for a shape to use. */
    drawing.curves.push_back(std::move(curve));
  }
  for (const json &s : get_array(j, "shapes")) {
    if (!s.is_object()) {
      continue;
    }
    Shape shape;
    shape.name = get_string(s, "name");
    shape.has_fill = get_bool(s, "fill", true);
    shape.has_outline = get_bool(s, "outline", true);
    if (auto it = s.find("style"); it != s.end()) {
      shape.style = style_from_json(*it);
    }
    for (const json &e : get_array(s, "edges")) {
      if (!e.is_array() || e.size() != 2 || !e[0].is_number_integer() || !e[1].is_number_integer()) {
        continue;
      }
      const ShapeEdge edge{e[0].get<int>(), e[1].get<int>()};
      if (edge.curve >= 0 && edge.curve < int(drawing.curves.size()) && edge.segment >= 0 &&
          edge.segment < drawing.curves[edge.curve].segment_count()) {
        shape.edges.push_back(edge);
      }
    }
    if (!shape.edges.empty()) {
      drawing.shapes.push_back(std::move(shape));
    }
  }
  return drawing;
}

static Layer layer_from_json(const json &j, int depth)
{
  Layer layer;
  layer.id = get_id(j, "id");
  layer.name = get_string(j, "name", "Layer");
  layer.type = get_enum(j, "type", LayerType::Vector);
  layer.visible = get_bool(j, "visible", true);
  layer.locked = get_bool(j, "locked", false);
  layer.expanded = get_bool(j, "expanded", true);
  layer.label_color = get_enum(j, "label_color", LabelColor::None);
  layer.position = get_float2(j, "position", {});
  layer.rotation = get_float(j, "rotation", 0.0f);
  layer.scale = get_float2(j, "scale", {1.0f, 1.0f});
  layer.opacity = std::clamp(get_float(j, "opacity", 1.0f), 0.0f, 1.0f);
  layer.mask_mode = get_enum(j, "mask_mode", MaskMode::Masked);
  layer.group_mask = get_enum(j, "group_mask", GroupMask::None);
  layer.reference_source = get_id(j, "reference_source");
  if (auto it = j.find("drawing"); it != j.end() && !layer.is_group()) {
    layer.drawing = drawing_from_json(*it);
  }
  /* Deeper than any real file: stops a crafted file from overflowing the stack. */
  if (layer.is_group() && depth < 256) {
    for (const json &child : get_array(j, "children")) {
      if (child.is_object()) {
        layer.children.push_back(layer_from_json(child, depth + 1));
      }
    }
  }
  return layer;
}

static void layers_visit(std::vector<Layer> &layers, const std::function<void(Layer &)> &fn)
{
  for (Layer &layer : layers) {
    fn(layer);
    layers_visit(layer.children, fn);
  }
}

/* Gives every layer a unique id, and drops references that point nowhere. */
static void layers_validate(Document &doc)
{
  std::set<uint64_t> seen;
  uint64_t max_id = 0;
  layers_visit(doc.layers, [&](Layer &layer) { max_id = std::max(max_id, layer.id); });
  layers_visit(doc.layers, [&](Layer &layer) {
    if (layer.id == 0 || !seen.insert(layer.id).second) {
      layer.id = ++max_id;
      seen.insert(layer.id);
    }
  });
  layers_visit(doc.layers, [&](Layer &layer) {
    if (layer.reference_source == layer.id || (layer.reference_source && !seen.count(layer.reference_source))) {
      layer.reference_source = 0;
    }
  });
  /* A reference must lead to a real drawing; break chains and loops of references. */
  layers_visit(doc.layers, [&](Layer &layer) {
    if (const Layer *source = layer_find(doc, layer.reference_source); source && source->is_reference()) {
      layer.reference_source = 0;
    }
  });
  doc.next_layer_id = std::max(doc.next_layer_id, max_id + 1);
  if (!layer_find(doc, doc.active_layer)) {
    doc.active_layer = doc.layers.empty() ? 0 : doc.layers.front().id;
  }
}

bool sky_document_from_json(const std::string &text, Document &r_doc, std::string *r_error)
{
  const json root = json::parse(text, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    *r_error = "The document is damaged (not valid JSON)";
    return false;
  }
  if (get_string(root, "format") != FORMAT_NAME) {
    *r_error = "Not a SkyAnimate file";
    return false;
  }
  const int version = get_int(root, "version", 0);
  if (version < 1 || version > SKY_FILE_VERSION) {
    *r_error = "The file was saved by a newer SkyAnimate (format " + std::to_string(version) +
               "); this version reads up to format " + std::to_string(SKY_FILE_VERSION);
    return false;
  }
  auto doc_it = root.find("document");
  if (doc_it == root.end() || !doc_it->is_object()) {
    *r_error = "The file has no document";
    return false;
  }
  const json &j = *doc_it;

  Document doc;
  doc.name = get_string(j, "name", doc.name);
  doc.frame_start = std::max(0, get_int(j, "frame_start", doc.frame_start));
  doc.frame_end = std::max(doc.frame_start, get_int(j, "frame_end", doc.frame_end));
  doc.frame_current = std::max(0, get_int(j, "frame_current", doc.frame_current));
  doc.fps = std::clamp(get_float(j, "fps", doc.fps), 1.0f, 1000.0f);
  doc.width = std::clamp(get_int(j, "width", doc.width), 1, 65536);
  doc.height = std::clamp(get_int(j, "height", doc.height), 1, 65536);
  doc.background = get_color(j, "background", doc.background);
  doc.viewport_quality = std::clamp(get_int(j, "viewport_quality", doc.viewport_quality), 1, 64);
  doc.render_quality = std::clamp(get_int(j, "render_quality", doc.render_quality), 1, 64);
  doc.action = get_string(j, "action");
  doc.active_layer = get_id(j, "active_layer");
  doc.next_layer_id = std::max<uint64_t>(1, get_id(j, "next_layer_id"));
  for (const json &layer : get_array(j, "layers")) {
    if (layer.is_object()) {
      doc.layers.push_back(layer_from_json(layer, 0));
    }
  }
  for (const json &t : get_array(j, "texts")) {
    if (t.is_object()) {
      doc.texts.push_back({get_string(t, "name", "Text"), get_string(t, "body"), get_string(t, "filepath")});
    }
  }
  layers_validate(doc);

  r_doc = std::move(doc);
  return true;
}

bool sky_file_read(const std::string &path, Document &r_doc, std::string *r_error)
{
  std::ifstream in(path_from_utf8(path), std::ios::binary);
  if (!in) {
    *r_error = "Could not open " + path;
    return false;
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
    *r_error = "Not a SkyAnimate file (not a zip archive): " + path;
    return false;
  }
  size_t size = 0;
  void *data = mz_zip_reader_extract_file_to_heap(&zip, DOCUMENT_ENTRY, &size, 0);
  mz_zip_reader_end(&zip);
  if (!data) {
    *r_error = std::string("The file has no ") + DOCUMENT_ENTRY + ": " + path;
    return false;
  }
  const std::string text(static_cast<const char *>(data), size);
  mz_free(data);
  return sky_document_from_json(text, r_doc, r_error);
}

}  // namespace sky
