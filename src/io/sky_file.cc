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

#include "anim/animation.hh"
#include "anim/deform.hh"

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

/* Brush settings use Moho's .mohobrush names, so the two read alike. Only a line with a brush
 * saves them. */
static json brush_to_json(const BrushSettings &b)
{
  return {{"name", b.name},
          {"align", b.align},
          {"jitter", num(b.jitter)},
          {"spacing", num(b.spacing)},
          {"angleDrift", num(b.angle_drift)},
          {"randomize", b.randomize},
          {"randomOrder", b.random_order},
          {"mergedAlpha", b.merged_alpha},
          {"sizeVariationAmp", num(b.size_variation_amp)},
          {"sizeVariationScale", num(b.size_variation_scale)},
          {"randomInterval", b.random_interval},
          {"tint", b.tint},
          {"hueDrift", num(b.hue_drift)},
          {"satDrift", num(b.sat_drift)},
          {"valDrift", num(b.val_drift)}};
}

static json style_to_json(const Style &style)
{
  json j = {{"fill", color_to_json(style.fill)}, {"line", color_to_json(style.line)}, {"line_width", num(style.line_width)}};
  if (!style.brush.name.empty()) {
    j["brush"] = brush_to_json(style.brush);
  }
  return j;
}

/* A channel's keys: [{"frame", "value", "interp"} plus any setting that isn't its default]. */
static json value_to_json(float v)
{
  return num(v);
}

static json value_to_json(const float2 &v)
{
  return float2_to_json(v);
}

template<typename T> static json keys_to_json(const Channel<T> &ch)
{
  const Key<T> defaults;
  json keys = json::array();
  for (const Key<T> &k : ch.keys) {
    json key = {{"frame", k.frame}, {"value", value_to_json(k.value)}, {"interp", interp_identifier(k.interp)}};
    if (k.hold != defaults.hold) {
      key["hold"] = k.hold;
    }
    if (k.interval != defaults.interval) {
      key["interval"] = k.interval;
    }
    if (k.overshoot != defaults.overshoot) {
      key["overshoot"] = num(k.overshoot);
    }
    if (k.noise_amplitude != defaults.noise_amplitude) {
      key["noise_amplitude"] = num(k.noise_amplitude);
    }
    if (k.noise_frequency != defaults.noise_frequency) {
      key["noise_frequency"] = num(k.noise_frequency);
    }
    if (k.cycle_back != defaults.cycle_back) {
      key["cycle_back"] = k.cycle_back;
    }
    if (k.cycle_absolute) {
      key["cycle_absolute"] = true;
      key["cycle_frame"] = k.cycle_frame;
    }
    if (k.color != 0) {
      key["color"] = k.color;
    }
    keys.push_back(std::move(key));
  }
  return keys;
}

/* Adds `name` to `j` when the channel is animated. */
template<typename T> static void put_keys(json &j, const char *name, const Channel<T> &ch)
{
  if (ch.animated()) {
    j[name] = keys_to_json(ch);
  }
  /* Other timelines' keys: actions by name, and "" for the mainline while an action is edited. */
  if (!ch.actions.empty()) {
    json actions = json::object();
    for (const auto &[action, keys] : ch.actions) {
      Channel<T> other;
      other.keys = keys;
      actions[action] = keys_to_json(other);
    }
    j[std::string(name) + "_actions"] = std::move(actions);
  }
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
    if (!p.weights.empty()) {
      json weights = json::array();
      for (const BoneWeight &w : p.weights) {
        weights.push_back(json::array({w.bone, num(w.weight)}));
      }
      point["weights"] = std::move(weights);
    }
    put_keys(point, "pos_keys", p.pos_keys);
    put_keys(point, "radius_keys", p.radius_keys);
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
      if (cp.aligned) {
        point["aligned"] = true;
      }
      if (cp.handles_set) {
        point["handle_in"] = float2_to_json(cp.handle_in);
        point["handle_out"] = float2_to_json(cp.handle_out);
      }
      put_keys(point, "curvature_keys", cp.curvature_keys);
      put_keys(point, "handle_in_keys", cp.handle_in_keys);
      put_keys(point, "handle_out_keys", cp.handle_out_keys);
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
      {"render_visible", layer.render_visible},
      {"locked", layer.locked},
      {"edit_with_bones", layer.edit_with_bones},
      {"expanded", layer.expanded},
      {"label_color", enum_identifier("label_color", int(layer.label_color))},
      {"position", float2_to_json(layer.position)},
      {"rotation", num(layer.rotation)},
      {"scale", float2_to_json(layer.scale)},
      {"opacity", num(layer.opacity)},
      {"mask_mode", enum_identifier("mask_mode", int(layer.mask_mode))},
      {"group_mask", enum_identifier("group_mask", int(layer.group_mask))},
      {"mask_expand", layer.mask_expand},
      {"mask_exclude_strokes", layer.mask_exclude_strokes},
  };
  if (layer.parent_bone != 0) {
    j["parent_bone"] = layer.parent_bone;
  }
  if (!layer.modifiers.empty()) {
    json modifiers = json::array();
    for (const Modifier &m : layer.modifiers) {
      json mj = {{"type", m.type == ModifierType::Armature ? "ARMATURE" : "MIRROR"},
                 {"name", m.name},
                 {"show_viewport", m.show_viewport},
                 {"show_render", m.show_render},
                 {"show_expanded", m.show_expanded}};
      if (m.type == ModifierType::Mirror) {
        mj["axis_x"] = m.axis_x;
        mj["axis_y"] = m.axis_y;
        mj["center"] = float2_to_json(m.center);
        mj["merge"] = m.merge;
        mj["merge_distance"] = num(m.merge_distance);
        mj["mirror_bones"] = m.mirror_bones;
      }
      modifiers.push_back(std::move(mj));
    }
    j["modifiers"] = std::move(modifiers);
  }
  if (layer.is_bone_layer()) {
    json bones = json::array();
    for (const Bone &b : layer.bones) {
      json bj = {{"id", b.id},
                 {"name", b.name},
                 {"parent", b.parent},
                 {"position", float2_to_json(b.position)},
                 {"angle", num(b.angle)},
                 {"length", num(b.length)},
                 {"scale", num(b.scale)},
                 {"strength", num(b.strength)},
                 {"label_color", enum_identifier("label_color", int(b.color))}};
      if (b.connected) {
        bj["connected"] = true;
      }
      put_keys(bj, "position_keys", b.position_keys);
      put_keys(bj, "angle_keys", b.angle_keys);
      put_keys(bj, "scale_keys", b.scale_keys);
      bones.push_back(std::move(bj));
    }
    j["bones"] = std::move(bones);
    if (!layer.actions.empty()) {
      j["actions"] = layer.actions;
    }
    j["next_bone_id"] = layer.next_bone_id;
    j["active_bone"] = layer.active_bone;
  }
  put_keys(j, "position_keys", layer.position_keys);
  put_keys(j, "rotation_keys", layer.rotation_keys);
  put_keys(j, "scale_keys", layer.scale_keys);
  put_keys(j, "opacity_keys", layer.opacity_keys);
  put_keys(j, "visible_keys", layer.visible_keys);
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
           {"auto_key", doc.auto_key},
           {"fps", num(doc.fps)},
           {"width", doc.width},
           {"height", doc.height},
           {"background", color_to_json(doc.background)},
           {"viewport_quality", doc.viewport_quality},
           {"viewport_samples", doc.viewport_samples},
           {"render_samples", doc.render_samples},
           {"render_quality", doc.render_quality},
           {"action", doc.action},
           {"action_layer", doc.action_layer},
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
  /* What smart actions add isn't data: it is worked out again on opening. */
  Document data = doc;
  animation_strip_smart(data);
  const std::string text = sky_document_to_json(data);

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

static BrushSettings brush_from_json(const json &j)
{
  BrushSettings b;
  b.name = get_string(j, "name");
  b.align = get_bool(j, "align", b.align);
  b.jitter = get_float(j, "jitter", b.jitter);
  b.spacing = std::max(0.01f, get_float(j, "spacing", b.spacing));
  b.angle_drift = get_float(j, "angleDrift", b.angle_drift);
  b.randomize = get_bool(j, "randomize", b.randomize);
  b.random_order = get_bool(j, "randomOrder", b.random_order);
  b.merged_alpha = get_bool(j, "mergedAlpha", b.merged_alpha);
  b.size_variation_amp = std::max(0.0f, get_float(j, "sizeVariationAmp", b.size_variation_amp));
  b.size_variation_scale = std::max(0.001f, get_float(j, "sizeVariationScale", b.size_variation_scale));
  b.random_interval = std::max(1, get_int(j, "randomInterval", b.random_interval));
  b.tint = get_bool(j, "tint", b.tint);
  b.hue_drift = get_float(j, "hueDrift", b.hue_drift);
  b.sat_drift = get_float(j, "satDrift", b.sat_drift);
  b.val_drift = get_float(j, "valDrift", b.val_drift);
  return b;
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
  if (auto it = j.find("brush"); it != j.end() && it->is_object()) {
    style.brush = brush_from_json(*it);
  }
  return style;
}

static bool value_from_json(const json &j, float *r)
{
  if (!j.is_number()) {
    return false;
  }
  *r = j.get<float>();
  return true;
}

static bool value_from_json(const json &j, float2 *r)
{
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) {
    return false;
  }
  *r = {j[0].get<float>(), j[1].get<float>()};
  return true;
}

template<typename T> static void get_key_list(const json &j, const char *name, Channel<T> &r_ch)
{
  for (const json &k : get_array(j, name)) {
    if (!k.is_object()) {
      continue;
    }
    Key<T> key;
    auto value = k.find("value");
    if (value == k.end() || !value_from_json(*value, &key.value)) {
      continue;
    }
    key.frame = std::max(0, get_int(k, "frame", 0));
    if (!interp_from_identifier(get_string(k, "interp", "SMOOTH"), &key.interp)) {
      key.interp = Interp::Smooth;
    }
    key.hold = std::max(0, get_int(k, "hold", key.hold));
    key.interval = std::max(1, get_int(k, "interval", key.interval));
    key.overshoot = get_float(k, "overshoot", key.overshoot);
    key.noise_amplitude = get_float(k, "noise_amplitude", key.noise_amplitude);
    key.noise_frequency = get_float(k, "noise_frequency", key.noise_frequency);
    key.cycle_back = std::max(0, get_int(k, "cycle_back", key.cycle_back));
    key.cycle_absolute = get_bool(k, "cycle_absolute", false);
    key.cycle_frame = std::max(0, get_int(k, "cycle_frame", 0));
    key.color = get_int(k, "color", 0);
    if (!r_ch.key_at(key.frame)) {
      r_ch.keys.push_back(key);
    }
  }
  std::sort(r_ch.keys.begin(), r_ch.keys.end(), [](const Key<T> &a, const Key<T> &b) { return a.frame < b.frame; });
}

template<typename T> static void get_keys(const json &j, const char *name, Channel<T> &r_ch)
{
  get_key_list(j, name, r_ch);
  auto it = j.find(std::string(name) + "_actions");
  if (it == j.end() || !it->is_object()) {
    return;
  }
  for (const auto &[action, list] : it->items()) {
    Channel<T> other;
    const json wrapper = {{"keys", list}};
    get_key_list(wrapper, "keys", other);
    if (!other.keys.empty()) {
      r_ch.actions[action] = std::move(other.keys);
    }
  }
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
      for (const json &w : get_array(p, "weights")) {
        if (w.is_array() && w.size() == 2 && w[0].is_number_unsigned() && w[1].is_number()) {
          drawing.points.back().weights.push_back({w[0].get<uint64_t>(), std::clamp(w[1].get<float>(), 0.0f, 1.0f)});
        }
      }
      /* Older files: a point bound to one bone (and not painted) follows it alone. */
      if (const uint64_t bone = get_id(p, "bone"); bone != 0 && drawing.points.back().weights.empty()) {
        drawing.points.back().weights.push_back({bone, 1.0f});
      }
      get_keys(p, "pos_keys", drawing.points.back().pos_keys);
      get_keys(p, "radius_keys", drawing.points.back().radius_keys);
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
      /* Handles are stored when the file has them; older files' Automatic and Vector handles are
       * worked out once from their neighbours (drawing_init_handles), Vector ones straight. */
      const std::string legacy = get_string(cp, "handle_type", "AUTO");
      point.handles_set = cp.contains("handle_in");
      point.aligned = get_bool(cp, "aligned", legacy == "ALIGNED");
      if (legacy == "VECTOR" && !point.handles_set) {
        point.curvature = CURVATURE_PEAKED;
      }
      point.handle_in = get_float2(cp, "handle_in", {});
      point.handle_out = get_float2(cp, "handle_out", {});
      get_keys(cp, "curvature_keys", point.curvature_keys);
      get_keys(cp, "handle_in_keys", point.handle_in_keys);
      get_keys(cp, "handle_out_keys", point.handle_out_keys);
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
  /* Older files' Automatic and Vector handles become stored ones now, from the setup pose. */
  drawing_init_handles(drawing);
  drawing_repair_stale_handle_keys(drawing);
  return drawing;
}

static Layer layer_from_json(const json &j, int depth)
{
  Layer layer;
  layer.id = get_id(j, "id");
  layer.name = get_string(j, "name", "Layer");
  layer.type = get_enum(j, "type", LayerType::Vector);
  layer.visible = get_bool(j, "visible", true);
  layer.render_visible = get_bool(j, "render_visible", true);
  layer.locked = get_bool(j, "locked", false);
  layer.edit_with_bones = get_bool(j, "edit_with_bones", false);
  layer.expanded = get_bool(j, "expanded", true);
  layer.label_color = get_enum(j, "label_color", LabelColor::None);
  layer.position = get_float2(j, "position", {});
  layer.rotation = get_float(j, "rotation", 0.0f);
  layer.scale = get_float2(j, "scale", {1.0f, 1.0f});
  layer.opacity = std::clamp(get_float(j, "opacity", 1.0f), 0.0f, 1.0f);
  layer.parent_bone = get_id(j, "parent_bone");
  for (const json &a : get_array(j, "actions")) {
    if (a.is_string() && !a.get<std::string>().empty()) {
      layer.actions.push_back(a.get<std::string>());
    }
  }
  for (const json &mj : get_array(j, "modifiers")) {
    if (!mj.is_object()) {
      continue;
    }
    Modifier m;
    m.type = get_string(mj, "type", "MIRROR") == "ARMATURE" ? ModifierType::Armature : ModifierType::Mirror;
    m.name = get_string(mj, "name", m.type == ModifierType::Armature ? "Armature" : "Mirror");
    m.show_viewport = get_bool(mj, "show_viewport", true);
    m.show_render = get_bool(mj, "show_render", true);
    m.show_expanded = get_bool(mj, "show_expanded", true);
    m.axis_x = get_bool(mj, "axis_x", true);
    m.axis_y = get_bool(mj, "axis_y", false);
    m.center = get_float2(mj, "center", {});
    m.merge = get_bool(mj, "merge", true);
    m.merge_distance = std::max(0.0f, get_float(mj, "merge_distance", m.merge_distance));
    m.mirror_bones = get_bool(mj, "mirror_bones", true);
    layer.modifiers.push_back(std::move(m));
  }
  for (const json &bj : get_array(j, "bones")) {
    if (!bj.is_object()) {
      continue;
    }
    Bone b;
    b.id = get_id(bj, "id");
    if (b.id == 0) {
      continue;
    }
    b.name = get_string(bj, "name", "Bone");
    b.parent = get_id(bj, "parent");
    b.position = get_float2(bj, "position", {});
    b.angle = get_float(bj, "angle", 0.0f);
    b.length = std::max(0.0f, get_float(bj, "length", 0.5f));
    b.scale = get_float(bj, "scale", 1.0f);
    b.strength = std::max(0.0f, get_float(bj, "strength", 1.0f));
    b.color = get_enum(bj, "label_color", LabelColor::None);
    b.connected = bj.contains("connected") && bj["connected"].is_boolean() && bj["connected"].get<bool>();
    get_keys(bj, "position_keys", b.position_keys);
    get_keys(bj, "angle_keys", b.angle_keys);
    get_keys(bj, "scale_keys", b.scale_keys);
    layer.next_bone_id = std::max(layer.next_bone_id, b.id + 1);
    layer.bones.push_back(std::move(b));
  }
  layer.next_bone_id = std::max<uint64_t>(layer.next_bone_id, get_id(j, "next_bone_id"));
  layer.active_bone = get_id(j, "active_bone");
  get_keys(j, "position_keys", layer.position_keys);
  get_keys(j, "rotation_keys", layer.rotation_keys);
  get_keys(j, "scale_keys", layer.scale_keys);
  get_keys(j, "opacity_keys", layer.opacity_keys);
  get_keys(j, "visible_keys", layer.visible_keys);
  layer.mask_mode = get_enum(j, "mask_mode", MaskMode::Masked);
  layer.group_mask = get_enum(j, "group_mask", GroupMask::None);
  layer.mask_expand = get_bool(j, "mask_expand", false);
  layer.mask_exclude_strokes = get_bool(j, "mask_exclude_strokes", false);
  layer.reference_source = get_id(j, "reference_source");
  if (auto it = j.find("drawing"); it != j.end() && !layer.is_group()) {
    layer.drawing = drawing_from_json(*it);
  }
  /* Older files without flexi-binding: points that weren't bound stayed put, and still do. */
  if (!get_bool(j, "flexi_binding", true)) {
    for (MeshPoint &p : layer.drawing.points) {
      if (p.weights.empty()) {
        p.weights.push_back({0, 0.0f});
      }
    }
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
  doc.auto_key = get_bool(j, "auto_key", true);
  doc.fps = std::clamp(get_float(j, "fps", doc.fps), 1.0f, 1000.0f);
  doc.width = std::clamp(get_int(j, "width", doc.width), 1, 65536);
  doc.height = std::clamp(get_int(j, "height", doc.height), 1, 65536);
  doc.background = get_color(j, "background", doc.background);
  doc.viewport_quality = std::clamp(get_int(j, "viewport_quality", doc.viewport_quality), 1, 64);
  doc.render_quality = std::clamp(get_int(j, "render_quality", doc.render_quality), 1, 64);
  doc.viewport_samples = std::clamp(get_int(j, "viewport_samples", doc.viewport_samples), 1, 16);
  doc.render_samples = std::clamp(get_int(j, "render_samples", doc.render_samples), 1, 16);
  doc.action = get_string(j, "action");
  doc.action_layer = get_id(j, "action_layer");
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
  /* Older files' flexi-binding becomes the same weights, kept. */
  document_weight_new_points(doc, true);

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
