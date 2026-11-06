#include "io/oanim_file.hh"

#include <numeric>
#include <execution>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <cstdlib>
#include <atomic>
#include <map>
#include <memory>

#include "assets/brush_presets.hh"
#include <set>
#include <thread>
#include <unordered_map>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "anim/controller.hh"
#include "anim/animation.hh"
#include "data/material.hh"
#include "core/log.hh"
#include "anim/deform.hh"
#include "anim/vector_deform.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/detail.hh"
#include "data/media_layer.hh"

namespace oanim {

using json = nlohmann::ordered_json;

static constexpr const char *DOCUMENT_ENTRY = "document.json";
static constexpr const char *MEDIA_ENTRY_PREFIX = "media/";
static constexpr const char *FORMAT_NAME = "OpenAnimate";
/* What files said before Open Animate was renamed from SkyAnimate: they open as they are. */
static constexpr const char *LEGACY_FORMAT_NAME = "SkyAnimate";

static std::filesystem::path path_from_utf8(const std::string &path)
{
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

/* ---------------------------------------------------------------------------------
 * Writing
 */

/* Floats in the fewest digits that read back as the very same float, so 0.3f is written as 0.3
 * rather than 0.30000001192092896 and files stay readable. (Always 7 lost the last bit of most
 * places: each save and open moved points by a hair.) */
static double num(float value)
{
  char text[32];
  for (int digits = 6; digits < 9; digits++) {
    std::snprintf(text, sizeof(text), "%.*g", digits, double(value));
    if (std::strtof(text, nullptr) == value) {
      return std::strtod(text, nullptr);
    }
  }
  std::snprintf(text, sizeof(text), "%.9g", double(value));
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
          {"valDrift", num(b.val_drift)},
          {"preset", b.preset},
          {"tipAngle", num(b.tip_angle)},
          {"roundness", num(b.roundness)},
          {"flipX", b.flip_x},
          {"flipY", b.flip_y},
          {"hardness", num(b.hardness)},
          {"popIn", b.pop_in},
          {"shapeDynamics", b.shape_dynamics},
          {"scattering", b.scattering},
          {"colorDynamics", b.color_dynamics},
          {"transfer", b.transfer},
          {"sizeJitter", num(b.size_jitter)},
          {"sizeMin", num(b.size_min)},
          {"sizeFadeIn", b.size_fade_in},
          {"sizeFadeOut", b.size_fade_out},
          {"roundnessJitter", num(b.roundness_jitter)},
          {"scatter", num(b.scatter)},
          {"scatterBoth", b.scatter_both},
          {"count", b.count},
          {"countJitter", num(b.count_jitter)},
          {"opacityJitter", num(b.opacity_jitter)},
          {"opacityFadeIn", b.opacity_fade_in},
          {"opacityFadeOut", b.opacity_fade_out}};
}

static json materials_to_json(const Document &doc);
static json brush_presets_to_json(const Document &doc);
static json drivers_to_json(const Document &doc);
static json annotations_to_json(const Document &doc);
static json guides_to_json(const Document &doc);
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
    if (k.type != KeyType::Key) {
      key["type"] = key_type_identifier(k.type);
    }
    if (k.hold != defaults.hold) {
      key["hold"] = k.hold;
    }
    if (k.interval != defaults.interval) {
      key["interval"] = k.interval;
    }
    if (k.ease != defaults.ease) {
      key["ease"] = ease_identifier(k.ease);
    }
    if (k.overshoot != defaults.overshoot) {
      key["overshoot"] = num(k.overshoot);
    }
    if (k.elastic_amplitude != defaults.elastic_amplitude) {
      key["elastic_amplitude"] = num(k.elastic_amplitude);
    }
    if (k.elastic_period != defaults.elastic_period) {
      key["elastic_period"] = num(k.elastic_period);
    }
    if (k.bezier != defaults.bezier) {
      key["bezier"] = json::array({num(k.bezier[0]), num(k.bezier[1]), num(k.bezier[2]), num(k.bezier[3])});
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
    if (!k.speeds.empty()) {
      json speeds = json::array();
      for (const float s : k.speeds) {
        speeds.push_back(num(s));
      }
      key["speeds"] = std::move(speeds);
    }
    if constexpr (std::is_same_v<T, float2>) {
      /* Path handles (the Motion Path tool's), once dragged. */
      if (k.path_handles != PathHandles::Auto) {
        key["path_handles"] = path_handles_identifier(k.path_handles);
        key["path_in"] = float2_to_json(k.path_in);
        key["path_out"] = float2_to_json(k.path_out);
      }
      if (!k.path_controls.empty()) {
        json controls = json::array();
        for (const float2 c : k.path_controls) {
          controls.push_back(float2_to_json(c));
        }
        key["path_controls"] = std::move(controls);
      }
      if (std::any_of(k.path_control_handles.begin(), k.path_control_handles.end(),
                      [](const PathControlHandles &h) { return h.set || !h.aligned; }))
      {
        /* Their handles, once moved or unaligned (as stored, relative to the keys). */
        json handles = json::array();
        for (size_t j = 0; j < k.path_controls.size(); j++) {
          const PathControlHandles h = path_control_handles_of(k, j);
          json one = {{"set", h.set}, {"aligned", h.aligned}};
          if (h.set) {
            one["in"] = float2_to_json(h.in);
            one["out"] = float2_to_json(h.out);
          }
          handles.push_back(std::move(one));
        }
        key["path_control_handles"] = std::move(handles);
      }
    }
    if (k.selected) {
      key["selected"] = true;
    }
    if (k.filler) {
      /* Its value is rounded as saved: what it was is taken again once opened (Key::filler_self 0). */
      key["filler"] = true;
      if (k.filler_inside) {
        key["filler_inside"] = true;
      }
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
  /* Pinned to the timeline: kept even without keys. */
  if (ch.pinned) {
    j[std::string(name) + "_pinned"] = true;
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
  /* Frame 0's key in each action not being edited, for its path handles. */
  if (!ch.action_setup.empty()) {
    json setup = json::object();
    for (const auto &[action, key] : ch.action_setup) {
      Channel<T> other;
      other.keys = {key};
      setup[action] = keys_to_json(other);
    }
    j[std::string(name) + "_action_setup"] = std::move(setup);
  }
}

static json drawing_to_json(const VectorDrawing &drawing);

/* Drawings written ahead of their layers (oanim_document_to_json, in parallel): each layer takes its own. */
static std::unordered_map<const VectorDrawing *, json> *g_drawings_ahead = nullptr;

static json layer_drawing_json(const VectorDrawing &drawing)
{
  if (g_drawings_ahead) {
    if (auto it = g_drawings_ahead->find(&drawing); it != g_drawings_ahead->end()) {
      return std::move(it->second);
    }
  }
  return drawing_to_json(drawing);
}

/* `count` items written by `item` into a JSON array, on all the cores when there are many (a large drawing's points
 * and curves). */
template<typename F> static json json_array_of(size_t count, F &&item)
{
  std::vector<json> items(count);
  if (count >= 2048) {
    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), size_t(0));
    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t i) { items[i] = item(i); });
  }
  else {
    for (size_t i = 0; i < count; i++) {
      items[i] = item(i);
    }
  }
  json out = json::array();
  out.get_ref<json::array_t &>().reserve(count);
  for (json &j : items) {
    out.push_back(std::move(j));
  }
  return out;
}

static json drawing_to_json(const VectorDrawing &drawing)
{
  json points = json_array_of(drawing.points.size(), [&](size_t index) {
    const MeshPoint &p = drawing.points[index];
    json point = {{"id", p.id}, {"pos", float2_to_json(p.pos)}};
    if (p.radius != 1.0f) {
      point["radius"] = num(p.radius);
    }
    if (p.selected) {
      point["selected"] = true;
    }
    if (!p.weights.empty()) {
      json weights = json::array();
      for (const BoneWeight &w : p.weights) {
        weights.push_back(json::array({w.bone, num(w.weight)}));
      }
      point["weights"] = std::move(weights);
    }
    if (p.pos_keys.animated() || !(point_setup_pos(p) == p.pos)) {
      point["setup_pos"] = float2_to_json(point_setup_pos(p));
    }
    put_keys(point, "pos_keys", p.pos_keys);
    put_keys(point, "radius_keys", p.radius_keys);
    /* A detail point: its level, place and offsets (data/detail.hh). */
    if (p.detail.level > 0) {
      const PointDetail &dt = p.detail;
      point["detail"] = {{"level", dt.level},
                         {"t", num(dt.t)},
                         {"pos", float2_to_json(dt.pos)},
                         {"in", float2_to_json(dt.in)},
                         {"out", float2_to_json(dt.out)},
                         {"setup_pos", float2_to_json(dt.setup_pos)},
                         {"setup_in", float2_to_json(dt.setup_in)},
                         {"setup_out", float2_to_json(dt.setup_out)}};
    }
    return point;
  });
  json curves = json_array_of(drawing.curves.size(), [&](size_t index) {
    const Curve &curve = drawing.curves[index];
    json curve_points = json_array_of(curve.points.size(), [&](size_t k) {
      const CurvePoint &cp = curve.points[k];
      /* Only what isn't the reader's default: most points have none of these, and a large file has many. */
      json point = {{"point", cp.point}};
      if (cp.curvature != CURVATURE_SMOOTH) {
        point["curvature"] = num(cp.curvature);
      }
      if (cp.weight_in != 1.0f) {
        point["weight_in"] = num(cp.weight_in);
      }
      if (cp.weight_out != 1.0f) {
        point["weight_out"] = num(cp.weight_out);
      }
      if (cp.offset_in != 0.0f) {
        point["offset_in"] = num(cp.offset_in);
      }
      if (cp.offset_out != 0.0f) {
        point["offset_out"] = num(cp.offset_out);
      }
      if (cp.select_in) {
        point["select_in"] = true;
      }
      if (cp.select_out) {
        point["select_out"] = true;
      }
      if (cp.edge_hidden) {
        point["edge_hidden"] = true;
      }
      if (cp.aligned) {
        point["aligned"] = true;
      }
      if (cp.handles_set) {
        point["handle_in"] = float2_to_json(cp.handle_in);
        point["handle_out"] = float2_to_json(cp.handle_out);
        for (const bool in : {true, false}) {
          const float2 setup = handle_setup(cp, in);
          if ((in ? cp.handle_in_keys : cp.handle_out_keys).animated() || !(setup == (in ? cp.handle_in : cp.handle_out))) {
            point[in ? "setup_in" : "setup_out"] = float2_to_json(setup);
          }
        }
      }
      put_keys(point, "curvature_keys", cp.curvature_keys);
      put_keys(point, "handle_in_keys", cp.handle_in_keys);
      put_keys(point, "handle_out_keys", cp.handle_out_keys);
      return point;
    });
    return json{{"closed", curve.closed}, {"points", std::move(curve_points)}};
  });
  json shapes = json::array();
  for (const Shape &shape : drawing.shapes) {
    json edges = json::array();
    for (const ShapeEdge &edge : shape.edges) {
      edges.push_back(json::array({edge.curve, edge.segment}));
    }
    shapes.push_back({{"id", shape.id},
                      {"name", shape.name},
                      {"fill", shape.has_fill},
                      {"outline", shape.has_outline},
                      {"style", style_to_json(shape.style)},
                      {"edges", std::move(edges)}});
    if (shape.material != 0) {
      shapes.back()["material"] = shape.material;
    }
    if (!shape.material_last.empty()) {
      shapes.back()["material_last"] = shape.material_last;
    }
    if (shape.detail_level > 0) {
      shapes.back()["detail_level"] = shape.detail_level;
    }
    /* Left out for Loops: each loop filled on its own, as every shape was before holes. */
    if (shape.fill_rule != FillRule::Loops) {
      shapes.back()["fill_rule"] = shape.fill_rule == FillRule::EvenOdd ? "EVEN_ODD" : "NONZERO";
    }
  }
  json out = {{"points", std::move(points)},
              {"curves", std::move(curves)},
              {"shapes", std::move(shapes)},
              {"next_id", drawing.next_id}};
  if (!drawing.groups.empty()) {
    /* Point groups: each member as [point id, weight]. */
    json groups = json::array();
    for (const PointGroup &g : drawing.groups) {
      json members = json::array();
      for (const auto &[id, w] : g.weights) {
        members.push_back({id, num(w)});
      }
      groups.push_back({{"name", g.name}, {"points", std::move(members)}, {"locked", g.locked}});
    }
    out["groups"] = std::move(groups);
    out["active_group"] = drawing.active_group;
  }
  if (!drawing.grids.empty()) {
    /* Grids: their cells and their nodes' point ids, row by row from the bottom. */
    json grids = json::array();
    for (const DrawingGrid &g : drawing.grids) {
      grids.push_back({{"name", g.name}, {"columns", g.columns}, {"rows", g.rows}, {"nodes", g.nodes}});
    }
    out["grids"] = std::move(grids);
  }
  return out;
}

/* A reference's own changes on top of its original. */
static json reference_to_json(const ReferenceSettings &rs)
{
  json points = json::array();
  for (const auto &[id, o] : rs.points) {
    json p = {{"id", id}, {"offset", float2_to_json(o.offset)}};
    put_keys(p, "offset_keys", o.offset_keys);
    points.push_back(std::move(p));
  }
  json looks = json::array();
  for (const auto &[id, look] : rs.looks) {
    json l = {{"id", id}};
    if (look.fill_set) {
      l["fill"] = color_to_json(look.fill);
    }
    if (look.line_set) {
      l["line"] = color_to_json(look.line);
    }
    if (look.width_set) {
      l["line_width"] = num(look.line_width);
    }
    if (look.brush_set) {
      l["brush"] = brush_to_json(look.brush);
    }
    if (look.has_fill_set) {
      l["has_fill"] = look.has_fill;
    }
    if (look.has_outline_set) {
      l["has_outline"] = look.has_outline;
    }
    if (look.hidden) {
      l["hidden"] = true;
    }
    looks.push_back(std::move(l));
  }
  json swaps = json::array();
  for (const ReferenceSwap &s : rs.swaps) {
    swaps.push_back({{"from", color_to_json(s.from)}, {"to", color_to_json(s.to)}, {"fill", s.fill}, {"line", s.line}});
  }
  json weights = json::array();
  for (const auto &[id, list] : rs.weights) {
    json ws = json::array();
    for (const BoneWeight &w : list) {
      ws.push_back(json::array({w.bone, num(w.weight)}));
    }
    weights.push_back({{"id", id}, {"weights", std::move(ws)}});
  }
  json handles = json::array();
  for (const auto &[key, o] : rs.handles) {
    json h = {{"point", key.point}, {"toward", key.toward}, {"side", key.in ? "IN" : "OUT"},
              {"offset", float2_to_json(o.offset)}};
    put_keys(h, "offset_keys", o.offset_keys);
    handles.push_back(std::move(h));
  }
  json edges = json::array();
  for (const auto &[key, hidden] : rs.edges) {
    edges.push_back({{"from", key.first}, {"to", key.second}, {"hidden", hidden}});
  }
  return {{"edit_points", rs.edit_points},
          {"follow_animation", rs.follow_animation},
          {"match_original", rs.match_original},
          {"own_channels", rs.own_channels},
          {"handles", std::move(handles)},
          {"edges", std::move(edges)},
          {"points", std::move(points)},
          {"looks", std::move(looks)},
          {"swaps", std::move(swaps)},
          {"weights", std::move(weights)}};
}

/* The folder of the .oanim being written or read: media files are saved relative to it as well as
 * absolute, and found from either (the project folder moved, or the drive letter changed). */
/* Per thread: an autosave writes on its own thread while the main one may save or open. */
static thread_local std::filesystem::path g_file_folder;

static const char *const MEDIA_FIT_IDS[] = {"FIT", "FILL", "STRETCH", "ORIGINAL"};
static const char *const MEDIA_TIMING_IDS[] = {"REAL_TIME", "FRAME_BY_FRAME"};
static const char *const MEDIA_END_IDS[] = {"HOLD", "LOOP", "HIDE"};

static json text_to_json(const TextSettings &t)
{
  static const char *const ALIGN[] = {"LEFT", "CENTER", "RIGHT", "JUSTIFY"};
  json runs = json::array();
  for (const TextRun &run : t.runs) {
    json r = {{"text", run.text},
              {"font", run.font},
              {"weight", run.weight},
              {"italic", run.italic},
              {"size", run.size},
              {"fill", run.has_fill},
              {"outline", run.has_outline},
              {"style", style_to_json(run.style)}};
    if (run.material != 0) {
      r["material"] = run.material;
    }
    if (!run.material_last.empty()) {
      r["material_last"] = run.material_last;
    }
    runs.push_back(std::move(r));
  }
  return {{"runs", std::move(runs)},
          {"align", ALIGN[std::clamp(int(t.align), 0, 3)]},
          {"line_spacing", t.line_spacing},
          {"letter_spacing", t.letter_spacing},
          {"word_spacing", t.word_spacing},
          {"shear", t.shear},
          {"box_width", t.box_width},
          {"dont_auto_key", t.no_auto_key}};
}

static json media_to_json(const MediaSettings &m)
{
  json clips = json::array();
  for (const MediaClip &c : m.clips) {
    json cj = {{"id", c.id},
               {"filepath", c.filepath},
               {"start", c.start},
               {"length", c.length},
               {"in", c.in},
               {"timing", MEDIA_TIMING_IDS[int(c.timing)]},
               {"end", MEDIA_END_IDS[int(c.end)]}};
    if (!g_file_folder.empty() && !c.filepath.empty()) {
      std::error_code ec;
      const std::filesystem::path rel = std::filesystem::relative(path_from_utf8(c.filepath), g_file_folder, ec);
      if (!ec && !rel.empty()) {
        const std::u8string text = rel.generic_u8string();
        cj["relative"] = std::string(text.begin(), text.end());
      }
    }
    if (c.sequence) {
      cj["sequence"] = true;
    }
    if (!c.packed.empty()) {
      cj["packed"] = c.packed;
    }
    clips.push_back(std::move(cj));
  }
  return {{"fit", MEDIA_FIT_IDS[int(m.fit)]},
          {"greyscale", m.greyscale},
          {"tint", color_to_json(m.tint)},
          {"tint_amount", num(m.tint_amount)},
          {"in_front", m.in_front},
          {"dont_auto_key", m.no_auto_key},
          {"volume", num(m.volume)},
          {"muted", m.muted},
          {"next_clip_id", m.next_clip_id},
          {"clips", std::move(clips)}};
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
      {"ignore_group_deforms", layer.ignore_group_deforms},
      {"key_whole_drawing", layer.key_whole_drawing},
      {"key_together", layer.key_together},
      {"edit_descendants", layer.edit_descendants},
      {"sub_bone_group", layer.sub_bone_group},
      {"link_same_named", layer.link_same_named},
      {"expanded", layer.expanded},
      {"selected", layer.selected},
      {"label_color", enum_identifier("label_color", int(layer.label_color))},
      {"position", float2_to_json(layer.position)},
      {"rotation", num(layer.rotation)},
      {"scale", float2_to_json(layer.scale)},
      {"opacity", num(layer.opacity)},
  };
  if (layer.shear.x != 0.0f || layer.shear.y != 0.0f) {
    j["shear"] = float2_to_json(layer.shear);
  }
  if (layer.parent_bone != 0) {
    j["parent_bone"] = layer.parent_bone;
  }
  /* Always written: Points is the default now, and a file without it was saved when Layer was. */
  j["parent_bone_deform"] = layer.parent_bone_points ? "POINTS" : "LAYER";
  if (layer.clip_to_below) {
    j["clip_to_below"] = true;
  }
  if (layer.clip_exclude_strokes) {
    j["clip_exclude_strokes"] = true;
  }
  if (!layer.masks.empty()) {
    json masks = json::array();
    for (const LayerMask &card : layer.masks) {
      json steps = json::array();
      const auto step_json = [](auto &&self, const MaskStep &m) -> json {
        json s = {{"mode", mask_mode_identifier(m.mode)},
                  {"invert", m.invert},
                  {"layers", m.layers},
                  {"combine", mask_combine_identifier(m.combine)},
                  {"grow", num(m.grow)},
                  {"fills_only", m.fills_only},
                  {"strokes_only", m.strokes_only},
                  {"inside_stroke", m.inside_stroke},
                  {"filled", m.filled},
                  {"extend", mask_extend_identifier(m.extend)},
                  {"show_expanded", m.show_expanded}};
        if (m.group) {
          json children = json::array();
          for (const MaskStep &child : m.children) {
            children.push_back(self(self, child));
          }
          s["group"] = true;
          s["children"] = std::move(children);
        }
        if (m.whole) {
          s["whole"] = true;
        }
        if (!m.name.empty()) {
          s["name"] = m.name;
        }
        if (m.global != 0) {
          s["global"] = m.global;
        }
        return s;
      };
      for (const MaskStep &m : card.steps) {
        steps.push_back(step_json(step_json, m));
      }
      json mj = {{"name", card.name},
                 {"start", card.start_shown ? "EVERYTHING" : "NOTHING"},
                 {"steps", std::move(steps)},
                 {"enabled", card.enabled},
                 {"render", card.render},
                 {"cuts", mask_cuts_identifier(card.cuts)},
                 {"end_lines", card.end_lines},
                 {"end_lines_flat", card.end_lines_flat},
                 {"line_reach", num(card.line_reach)},
                 {"show_expanded", card.show_expanded}};
      put_keys(mj, "line_reach_keys", card.line_reach_keys);
      masks.push_back(std::move(mj));
    }
    j["masks"] = std::move(masks);
  }
  if (!layer.modifiers.empty()) {
    json modifiers = json::array();
    for (const Modifier &m : layer.modifiers) {
      json mj = {{"type", m.type == ModifierType::Armature    ? "ARMATURE"
                          : m.type == ModifierType::Boolean   ? "BOOLEAN"
                          : m.type == ModifierType::Bevel     ? "BEVEL"
                          : m.type == ModifierType::Resample ? "RESAMPLE"
                          : modifier_is_deform(m.type) ? deform_type_id(m.type)
                                                              : "MIRROR"},
                 {"name", m.name},
                 {"show_viewport", m.show_viewport},
                 {"show_render", m.show_render},
                 {"show_points", m.show_points},
                 {"show_expanded", m.show_expanded}};
      if (modifier_bends(m.type)) {
        mj["show_mesh"] = m.show_mesh;
      }
      if (m.type == ModifierType::Mirror) {
        mj["axis_x"] = m.axis_x;
        mj["axis_y"] = m.axis_y;
        mj["center"] = float2_to_json(m.center);
        mj["merge"] = m.merge;
        mj["merge_distance"] = num(m.merge_distance);
        mj["mirror_bones"] = m.mirror_bones;
      }
      if (m.type == ModifierType::Boolean) {
        mj["shapes"] = m.merge_shapes;
        json targets = json::array();
        for (const MergeTarget &t : m.merge_targets) {
          targets.push_back({{"layer", t.layer}, {"shapes", t.shapes}});
        }
        mj["targets"] = std::move(targets);
        mj["operation"] = m.merge_operation == MergeOperation::Union      ? "UNION"
                          : m.merge_operation == MergeOperation::Subtract ? "SUBTRACT"
                                                                          : "INTERSECT";
        mj["smoothing"] = num(m.merge_smoothing);
        mj["corners_only"] = m.merge_corners_only;
        put_keys(mj, "smoothing_keys", m.merge_smoothing_keys);
      }
      if (m.type == ModifierType::Bevel) {
        mj["limit"] = m.bevel_limit == BevelLimit::Group ? "GROUP" : m.bevel_limit == BevelLimit::None ? "NONE" : "ANGLE";
        mj["width_type"] = m.bevel_width == BevelWidth::Width   ? "WIDTH"
                           : m.bevel_width == BevelWidth::Depth   ? "DEPTH"
                           : m.bevel_width == BevelWidth::Percent ? "PERCENT"
                                                                  : "OFFSET";
        mj["angle"] = num(m.bevel_angle);
        mj["group"] = m.bevel_group;
        mj["amount"] = num(m.bevel_amount);
        put_keys(mj, "amount_keys", m.bevel_amount_keys);
        mj["profile"] = num(m.bevel_profile);
        mj["clamp"] = m.bevel_clamp;
      }
      if (m.type == ModifierType::Resample) {
        mj["split"] = m.subdiv_by_length ? "LENGTH" : "COUNT";
        mj["spacing"] = num(m.subdiv_spacing);
        mj["render_spacing"] = num(m.subdiv_render_spacing);
        mj["pieces"] = m.subdiv_pieces;
        mj["max_pieces"] = m.subdiv_max_pieces;
        mj["weights"] = m.subdiv_from_bones ? "BONES" : "BLEND";
        mj["preserve_volume"] = m.subdiv_preserve_volume;
        mj["group"] = m.subdiv_group;
      }
      if (modifier_is_deform(m.type)) {
        mj["layer"] = m.deform_layer;
        if (m.id != 0) {
          mj["id"] = m.id;
        }
        mj["shape"] = m.deform_shape;
        mj["weights"] = m.deform_custom ? "CUSTOM" : "AUTOMATIC";
        mj["group"] = m.deform_group;
        mj["strength"] = num(m.deform_strength);
        put_keys(mj, "strength_keys", m.deform_strength_keys);
        mj["tolerance"] = num(m.deform_precision);
        mj["stretch"] = m.deform_stretch;
        if (m.deform_part_from != 0) {
          mj["part_from"] = m.deform_part_from;
          mj["part_to"] = m.deform_part_to;
        }
        mj["resample_self"] = m.deform_resample_self;
        if (m.follow_limit != FollowLimit::None) {
          mj["limit"] = m.follow_limit == FollowLimit::Shape ? "SHAPE" : "GROUP";
          mj["limit_shape"] = m.follow_limit_shape;
          mj["limit_group"] = m.follow_limit_group;
          mj["limit_invert"] = m.follow_limit_invert;
        }
        mj["split"] = m.subdiv_by_length ? "LENGTH" : "COUNT";
        mj["spacing"] = num(m.subdiv_spacing);
        mj["pieces"] = m.subdiv_pieces;
        if (m.type == ModifierType::CageDeform || m.type == ModifierType::FollowDeform) {
          mj["cage_method"] = cage_method_id(m.deform_cage_method);
          mj["blend"] = num(m.deform_blend);
          mj["range"] = num(m.deform_range);
        }
        if (m.type == ModifierType::CurveDeform || m.type == ModifierType::CageDeform) {
          mj["envelope_method"] = envelope_method_id(m.deform_envelope_method);
          mj["pre_blend"] = num(m.deform_pre_blend);
          mj["post_blend"] = num(m.deform_post_blend);
        }
        if (m.type == ModifierType::CurveDeform) {
          mj["curve_method"] = curve_method_id(m.deform_curve_method);
        }
        if (m.type == ModifierType::CurveDeform || m.type == ModifierType::CageDeform) {
          mj["influence"] = influence_type_id(m.deform_influence);
          mj["influence_symmetric"] = m.deform_influence_symmetric;
          mj["influence_left"] = num(m.deform_influence_left);
          mj["influence_right"] = num(m.deform_influence_right);
          mj["influence_begin"] = num(m.deform_influence_begin);
          mj["influence_end"] = num(m.deform_influence_end);
          mj["influence_fade"] = num(m.deform_influence_fade);
        }
        if (m.type == ModifierType::FollowDeform) {
          mj["point"] = {num(m.deform_point.x), num(m.deform_point.y)};
          mj["point2"] = {num(m.deform_point2.x), num(m.deform_point2.y)};
          mj["show_point"] = m.deform_show_point;
          mj["two"] = m.deform_two;
          mj["rotation"] = m.deform_rotation;
          mj["scale"] = m.deform_scale;
          mj["shift"] = {num(m.deform_shift.x), num(m.deform_shift.y)};
          mj["shift2"] = {num(m.deform_shift2.x), num(m.deform_shift2.y)};
          put_keys(mj, "shift_keys", m.deform_shift_keys);
          put_keys(mj, "shift2_keys", m.deform_shift2_keys);
          if (m.deform_point_id != 0) {
            mj["point_id"] = m.deform_point_id;
          }
          if (m.deform_point2_id != 0) {
            mj["point2_id"] = m.deform_point2_id;
          }
        }
        if (m.type == ModifierType::MeshDeform || m.type == ModifierType::FollowDeform) {
          mj["mesh_method"] = mesh_method_id(m.deform_mesh_method);
          mj["smooth"] = m.deform_smooth;
          mj["blend"] = num(m.deform_blend);
          mj["falloff"] = num(m.deform_falloff);
        }
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
                 {"setup_position", float2_to_json(b.setup_position)},
                 {"setup_angle", num(b.setup_angle)},
                 {"length", num(b.length)},
                 {"scale", float2_to_json(b.scale)},
                 {"strength", num(b.strength)}};
      if (b.color_set != 0) {
        bj["color_set"] = bone_color_set_identifier(b.color_set);
      }
      if (b.color_set == BONE_COLOR_CUSTOM) {
        json shades = json::array();
        for (const Color &c : b.custom_colors) {
          shades.push_back({num(c.r), num(c.g), num(c.b)});
        }
        bj["custom_colors"] = shades;
      }
      if (b.connected) {
        bj["connected"] = true;
      }
      if (b.follow_knob) {
        bj["follow_knob"] = true;
      }
      if (!b.inherit_rotation) {
        bj["inherit_rotation"] = false;
      }
      if (b.swap_layer != 0) {
        bj["swap"] = {b.swap_layer, b.swap_bone};
      }
      if (b.outer_bone != 0) {
        bj["outer_parent"] = {b.outer_layer, b.outer_bone};
      }
      if (b.inherit_scale != InheritScale::Full) {
        bj["inherit_scale"] = b.inherit_scale == InheritScale::Uniform ? "UNIFORM" : "NONE";
      }
      if (!b.local_location) {
        bj["local_location"] = false;
      }
      if (b.selected || b.select_head || b.select_tail) {
        bj["selected"] = b.selected;
        bj["select_head"] = b.select_head;
        bj["select_tail"] = b.select_tail;
      }
      put_keys(bj, "position_keys", b.position_keys);
      put_keys(bj, "angle_keys", b.angle_keys);
      put_keys(bj, "scale_keys", b.scale_keys);
      if (b.depth != 1.0f || b.depth_keys.animated()) {
        bj["depth"] = num(b.depth);
        put_keys(bj, "depth_keys", b.depth_keys);
      }
      if (b.ik_ignore) {
        bj["ik_ignore"] = true;
      }
      if (b.arc_ik) {
        bj["arc_ik"] = true;
      }
      if (b.shape != BoneShape::None) {
        bj["shape"] = "CUSTOM";
      }
      if (b.type == BoneType::Pin) {
        bj["type"] = "PIN";
      }
      if (b.pin_size != 1.0f) {
        bj["pin_size"] = num(b.pin_size);
      }
      if (b.shape_layer != 0) {
        bj["shape_layer"] = b.shape_layer;
      }
      if (!b.collections.empty()) {
        bj["collections"] = b.collections;
      }
      if (b.type == BoneType::Controller2D) {
        bj["type"] = "CONTROLLER_2D";
        json actions = json::object();
        for (int side = 0; side < CONTROLLER_SIDES; side++) {
          if (!b.controller_actions[side].empty()) {
            actions[controller_side_id(side)] = b.controller_actions[side];
          }
        }
        bj["controller_actions"] = std::move(actions);
        bj["controller_frame_size"] = num(b.controller_frame_size);
        if (b.controller_box_angle != 0.0f) {
          bj["controller_box_angle"] = num(b.controller_box_angle);
        }
        if (b.controller_knob_independent) {
          bj["controller_knob_independent"] = true;
        }
        if (b.controller_knob_parent != 0) {
          bj["controller_knob_parent"] = b.controller_knob_parent;
        }
        if (!b.controller_size_auto) {
          bj["controller_size_auto"] = false;
        }
        if (!b.controller_spots.empty()) {
          json spots = json::array();
          for (const ControllerSpot &spot : b.controller_spots) {
            spots.push_back({{"name", spot.name}, {"at", {num(spot.at.x), num(spot.at.y)}}});
          }
          bj["controller_spots"] = std::move(spots);
        }
      }
      if (!b.constraints.empty()) {
        json constraints = json::array();
        for (const BoneConstraint &c : b.constraints) {
          const auto spring = [](const BoneSpring &s) {
            json sj{{"enabled", s.enabled}, {"settle_speed", num(s.settle_speed)},
                    {"bounce", num(s.bounce)},        {"inertia", num(s.inertia)},
                    {"stretch", num(s.stretch)},
                    {"use_limit", s.use_limit},       {"limit", num(s.limit)},
                    {"use_squash_limit", s.use_squash_limit}, {"squash_limit", num(s.squash_limit)}};
            put_keys(sj, "settle_speed_keys", s.settle_speed_keys);
            put_keys(sj, "bounce_keys", s.bounce_keys);
            put_keys(sj, "inertia_keys", s.inertia_keys);
            put_keys(sj, "stretch_keys", s.stretch_keys);
            put_keys(sj, "limit_keys", s.limit_keys);
            put_keys(sj, "squash_limit_keys", s.squash_limit_keys);
            return sj;
          };
          json cj = {{"type", bone_constraint_identifier(c.type)},
                     {"name", c.name},
                     {"enabled", c.enabled},
                     {"expanded", c.expanded},
                     {"influence", num(c.influence)},
                     {"min_turn", num(c.min_angle)},
                     {"max_turn", num(c.max_angle)},
                     {"amount", num(c.amount)},
                     {"target", c.target},
                     {"factor", num(c.factor)},
                     {"delay", c.delay},
                     {"follow_move", c.follow_move},
                     {"chain", c.chain},
                     {"max_stretch", num(c.max_stretch)},
                     {"shape_layer", c.shape_layer},
                     {"shape", c.shape_id},
                     {"follow_layer", c.follow_layer},
                     {"follow_modifier", c.follow_modifier},
                     {"follow_point", {num(c.follow_point.x), num(c.follow_point.y)}},
                     {"follow_rest", {num(c.follow_rest.x), num(c.follow_rest.y)}},
                     {"follow_show", c.follow_show},
                     {"follow_two", c.follow_two},
                     {"follow_point2", {num(c.follow_point2.x), num(c.follow_point2.y)}},
                     {"follow_rest2", {num(c.follow_rest2.x), num(c.follow_rest2.y)}},
                     {"follow_rotation", c.follow_rotation},
                     {"follow_scale", c.follow_scale},
                     {"follow_shift", {num(c.follow_shift.x), num(c.follow_shift.y)}},
                     {"follow_shift2", {num(c.follow_shift2.x), num(c.follow_shift2.y)}},
                     {"follow_point_id", c.follow_point_id},
                     {"follow_point2_id", c.follow_point2_id},
                     {"follow_three", c.follow_three},
                     {"follow_point3", {num(c.follow_point3.x), num(c.follow_point3.y)}},
                     {"follow_rest3", {num(c.follow_rest3.x), num(c.follow_rest3.y)}},
                     {"follow_shift3", {num(c.follow_shift3.x), num(c.follow_shift3.y)}},
                     {"follow_point3_id", c.follow_point3_id},
                     {"follow_scale_mode", c.follow_scale_mode == FollowScaleMode::AlongLine ? "ALONG_LINE" : "UNIFORM"},
                     {"follow_volume", num(c.follow_volume)},
                     {"fit", c.shape_fit == ShapeFit::Whole ? "WHOLE" : "CENTER"},
                     {"fit_layer", c.fit_layer},
                     {"overlap", num(c.overlap)},
                     {"softness", num(c.softness)},
                     {"affect_transform", c.affect_transform},
                     {"child_position", c.child_position},
                     {"child_rotation", c.child_rotation},
                     {"child_scale", c.child_scale},
                     {"weight", num(c.weight)},
                     {"wind_catch", num(c.wind_catch)},
                     {"own_gravity", c.own_gravity},
                     {"own_wind", c.own_wind},
                     {"own_gravity_set", c.own_gravity_set},
                     {"own_wind_set", c.own_wind_set},
                     {"gravity", num(c.gravity)},
                     {"wind", num(c.wind)},
                     {"wind_direction", num(c.wind_direction)},
                     {"wind_gusts", num(c.wind_gusts)},
                     {"wind_gust_speed", num(c.wind_gust_speed)},
                     {"wind_turbulence", num(c.wind_turbulence)},
                     {"angle_spring", spring(c.angle_spring)},
                     {"position_spring", spring(c.position_spring)},
                     {"scale_spring", spring(c.scale_spring)}};
          put_keys(cj, "influence_keys", c.influence_keys);
          put_keys(cj, "follow_shift_keys", c.follow_shift_keys);
          put_keys(cj, "follow_shift2_keys", c.follow_shift2_keys);
          put_keys(cj, "follow_shift3_keys", c.follow_shift3_keys);
          put_keys(cj, "weight_keys", c.weight_keys);
          put_keys(cj, "wind_catch_keys", c.wind_catch_keys);
          put_keys(cj, "gravity_keys", c.gravity_keys);
          put_keys(cj, "wind_keys", c.wind_keys);
          put_keys(cj, "wind_direction_keys", c.wind_direction_keys);
          put_keys(cj, "wind_gusts_keys", c.wind_gusts_keys);
          put_keys(cj, "wind_gust_speed_keys", c.wind_gust_speed_keys);
          put_keys(cj, "wind_turbulence_keys", c.wind_turbulence_keys);
          constraints.push_back(std::move(cj));
        }
        bj["constraints"] = std::move(constraints);
      }
      bones.push_back(std::move(bj));
    }
    j["bones"] = std::move(bones);
    if (!layer.actions.empty()) {
      j["actions"] = layer.actions;
    }
    if (!layer.fix_actions.empty()) {
      json fixes = json::array();
      for (const FixAction &f : layer.fix_actions) {
        json poses = json::array();
        for (const FixPose &pose : f.poses) {
          json at = json::array();
          for (const float v : pose.at) {
            at.push_back(num(v));
          }
          poses.push_back({{"frame", pose.frame}, {"at", at}, {"keyed", pose.keyed}});
        }
        fixes.push_back({{"name", f.name}, {"drivers", f.drivers}, {"poses", poses}});
      }
      j["fix_actions"] = std::move(fixes);
    }
    if (!layer.action_plays.empty()) {
      json plays = json::array();
      for (const ActionPlay &a : layer.action_plays) {
        json pj = {{"name", a.name}, {"frame", num(a.frame)}, {"mute", a.mute},
                   {"loop", a.loop == ActionLoop::On ? "ON" : a.loop == ActionLoop::Off ? "OFF" : "AUTO"}};
        put_keys(pj, "frame_keys", a.frame_keys);
        plays.push_back(std::move(pj));
      }
      j["action_plays"] = std::move(plays);
    }
    j["next_bone_id"] = layer.next_bone_id;
    j["active_bone"] = layer.active_bone;
    if (!layer.bone_collections.empty()) {
      json collections = json::array();
      for (const BoneCollection &c : layer.bone_collections) {
        collections.push_back({{"id", c.id}, {"name", c.name}, {"visible", c.visible}, {"solo", c.solo}});
      }
      j["bone_collections"] = std::move(collections);
      j["active_collection"] = layer.active_collection;
    }
    j["next_collection_id"] = layer.next_collection_id;
    if (!layer.all_bones_visible || layer.all_bones_solo) {
      j["all_bones"] = {{"visible", layer.all_bones_visible}, {"solo", layer.all_bones_solo}};
    }
  }
  if (!layer.collections.empty()) {
    j["collections"] = layer.collections;
  }
  put_keys(j, "position_keys", layer.position_keys);
  put_keys(j, "rotation_keys", layer.rotation_keys);
  put_keys(j, "scale_keys", layer.scale_keys);
  put_keys(j, "shear_keys", layer.shear_keys);
  put_keys(j, "opacity_keys", layer.opacity_keys);
  put_keys(j, "visible_keys", layer.visible_keys);
  if (layer.draw_order != 0.0f) {
    j["draw_order"] = std::llround(layer.draw_order);
  }
  put_keys(j, "draw_order_keys", layer.draw_order_keys);
  if (!layer.switch_aliases.empty()) {
    j["aliases"] = layer.switch_aliases;
  }
  if (layer.is_switch()) {
    /* The child as a layer id (Rig is -1), as Draw Order names its target. */
    j["switch"] = std::llround(layer.switch_value);
    if (layer.switch_link) {
      j["switch_link"] = layer.switch_link;
    }
    if (layer.switch_link_off) {
      j["switch_link_off"] = true;
    }
    put_keys(j, "switch_keys", layer.switch_keys);
  }
  if (layer.is_reference()) {
    /* A reference's drawing lives in its source; storing a copy would only go stale. */
    j["reference_source"] = layer.reference_source;
    j["reference"] = reference_to_json(layer.reference);
  }
  else if (layer.is_media()) {
    j["media"] = media_to_json(layer.media);
  }
  else if (layer.is_text()) {
    /* The text, and its letters as they were made (so it shows where the font isn't installed). */
    j["text"] = text_to_json(layer.text);
    j["drawing"] = layer_drawing_json(layer.drawing);
  }
  else if (!layer.is_group()) {
    j["drawing"] = layer_drawing_json(layer.drawing);
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

static json overlays_to_json(const ViewportOverlays &ov);

std::string oanim_document_to_json(const Document &doc)
{
  json texts = json::array();
  for (const Text &text : doc.texts) {
    texts.push_back({{"name", text.name}, {"body", text.body}, {"filepath", text.filepath}});
  }
  /* Every drawing written first, on all the cores at once (they are most of a large file's work, and each
   * stands alone), then the layers in order, each taking its own. */
  std::vector<const VectorDrawing *> drawings;
  const auto gather = [&](auto &&self, const std::vector<Layer> &list) -> void {
    for (const Layer &layer : list) {
      if (layer.is_group()) {
        self(self, layer.children);
      }
      else if (!layer.is_media()) {
        drawings.push_back(&layer.drawing);
      }
    }
  };
  gather(gather, doc.layers);
  std::vector<json> written(drawings.size());
  Layer::rna();
  std::vector<size_t> order(drawings.size());
  std::iota(order.begin(), order.end(), size_t(0));
  std::for_each(std::execution::par, order.begin(), order.end(),
                [&](size_t i) { written[i] = drawing_to_json(*drawings[i]); });
  std::unordered_map<const VectorDrawing *, json> ahead;
  ahead.reserve(drawings.size());
  for (size_t i = 0; i < drawings.size(); i++) {
    ahead.emplace(drawings[i], std::move(written[i]));
  }
  g_drawings_ahead = &ahead;
  json layers = json::array();
  for (const Layer &layer : doc.layers) {
    layers.push_back(layer_to_json(layer));
  }
  g_drawings_ahead = nullptr;
  /* Keys of the animated gravity and wind, by setting. */
  json simulation_keys = json::object();
  for (const CustomChannel &c : doc.simulation.custom_channels) {
    put_keys(simulation_keys, c.path.c_str(), c.keys);
  }
  /* The camera: where it is now, and its keys (the zoom's hold log2 of the zoom, see data/camera.hh). */
  json camera_keys = json::object();
  put_keys(camera_keys, "position", doc.camera.position_keys);
  put_keys(camera_keys, "rotation", doc.camera.rotation_keys);
  for (const CustomChannel &c : doc.camera.custom_channels) {
    put_keys(camera_keys, c.path.c_str(), c.keys);
  }
  json camera = {{"position", float2_to_json(doc.camera.position)},
                 {"roll", num(doc.camera.rotation)},
                 {"zoom", num(camera_zoom(doc))},
                 {"selected", doc.camera.selected},
                 {"passepartout", num(doc.camera_passepartout)},
                 {"keys", std::move(camera_keys)}};
  const json root = {
      {"format", FORMAT_NAME},
      {"version", OANIM_FILE_VERSION},
      {"document",
       {
           {"name", doc.name},
           {"frame_start", doc.frame_start},
           {"frame_end", doc.frame_end},
           {"frame_current", doc.frame_current},
           {"auto_key", doc.auto_key},
           {"auto_key_mode", Document::rna().find("auto_key_mode")->items[size_t(doc.auto_key_mode)].identifier},
           {"auto_freeze_keys", doc.auto_freeze_keys},
           {"time_together", doc.time_together},
           {"timeline_only_selected", doc.timeline_only_selected},
           {"timeline_lock", doc.timeline_lock},
           {"timeline_locked", doc.timeline_locked},
           {"timeline_show", doc.timeline_show},
           {"timeline_kinds", doc.timeline_kinds},
           {"timeline_search", doc.timeline_search},
           {"onion_skin", doc.onion_skin},
           {"onion_mode", Document::rna().find("onion_mode")->items[size_t(doc.onion_mode)].identifier},
           {"onion_before", doc.onion_before},
           {"onion_after", doc.onion_after},
           {"onion_step", doc.onion_step},
           {"onion_fade", doc.onion_fade},
           {"onion_all_layers", doc.onion_all_layers},
           {"onion_color_before", color_to_json(doc.onion_color_before)},
           {"onion_color_after", color_to_json(doc.onion_color_after)},
           {"motion_paths", doc.motion_paths},
           {"path_before", doc.path_before},
           {"path_after", doc.path_after},
           {"path_all", doc.path_all},
           {"path_step", doc.path_step},
           {"path_lines", doc.path_lines},
           {"path_own_motion", doc.path_own_motion},
           {"path_dot_colors", Document::rna().find("path_dot_colors")->items[size_t(doc.path_dot_colors)].identifier},
           {"path_relative", Document::rna().find("path_relative")->items[size_t(doc.path_relative)].identifier},
           {"path_relative_layer", doc.path_relative_layer},
           {"path_relative_bone", doc.path_relative_bone},
           {"path_targets", [&] {
              json targets = json::array();
              for (const MotionPathTarget &t : doc.path_targets) {
                json tj = {{"layer", t.layer}, {"bone", t.bone}, {"point", t.point}, {"head", t.head},
                           {"pinned", t.pinned}};
                if (t.has_color) {
                  tj["color"] = color_to_json(t.color);
                }
                targets.push_back(std::move(tj));
              }
              return targets;
            }()},
           {"sync_mode", Document::rna().find("sync_mode")->items[size_t(doc.sync)].identifier},
           {"play_audio", doc.play_audio},
           {"use_audio_scrub", doc.audio_scrub},
           {"fps", num(doc.fps)},
           {"width", doc.width},
           {"height", doc.height},
           {"resolution_linked", doc.resolution_linked},
           {"resolution_aspect", num(doc.resolution_aspect)},
           {"background", color_to_json(doc.background)},
           {"viewport_quality", doc.viewport_quality},
           {"viewport_samples", doc.viewport_samples},
           {"render_samples", doc.render_samples},
           {"viewport_overlays", overlays_to_json(doc.overlays)},
           {"viewport_shading",
            {{"layer_caching", doc.shading.layer_caching},
             {"zoom_preview", doc.shading.zoom_preview},
             {"renderer", doc.shading.gpu_geometry ? "GPU_GEOMETRY" : "CLASSIC"}}},
           {"mirror_editing", {{"x", doc.mirror_x}, {"y", doc.mirror_y}}},
           {"ui_open", doc.ui_open},
           {"ui_panel_order", doc.ui_panel_order},
           {"viewport_view", {{"center", {num(doc.view.center_x), num(doc.view.center_y)}}, {"zoom", num(doc.view.zoom)}}},
           {"render_quality", doc.render_quality},
           {"action", doc.action},
           {"gravity", num(doc.gravity)},
           {"wind", num(doc.wind)},
           {"wind_direction", num(doc.wind_direction)},
           {"wind_gusts", num(doc.wind_gusts)},
           {"wind_gust_speed", num(doc.wind_gust_speed)},
           {"wind_turbulence", num(doc.wind_turbulence)},
           {"simulation_start", doc.simulation_start},
           {"simulation_keys", std::move(simulation_keys)},
           {"camera", std::move(camera)},
           {"action_layer", doc.action_layer},
           {"active_layer", doc.active_layer},
           {"next_layer_id", doc.next_layer_id},
           {"layers", std::move(layers)},
           {"texts", std::move(texts)},
           {"materials", materials_to_json(doc)},
           {"brush_presets", brush_presets_to_json(doc)},
           {"drivers", drivers_to_json(doc)},
           {"switch_links", [&] {
              json links = json::array();
              for (const SwitchLink &l : doc.switch_links) {
                json names = json::array();
                for (const SwitchLinkName &n : l.names) {
                  names.push_back({n.id, n.name});
                }
                links.push_back({{"id", l.id}, {"lead", l.lead}, {"names", names}, {"next_name", l.next_name}});
              }
              return links;
            }()},
           {"next_switch_link", doc.next_switch_link},
           {"annotations", annotations_to_json(doc)},
           {"active_annotation", doc.active_annotation},
           {"guides", guides_to_json(doc)},
           {"active_guide", doc.active_guide},
           {"guide_settings",
            {{"show", doc.guide_settings.show},
             {"hue", doc.guide_settings.hue},
             {"opacity", doc.guide_settings.opacity}}},
           /* Mask rows take their layers' shapes before those layers' own masks (before this, a mask
            * layer's own masks cut its shape first, one level deep). */
           {"mask_sources", "shapes"},
       }},
  };
  /* Compact: a large file's indentation and line breaks were two thirds of its text. */
  return root.dump();
}

static bool file_write_bytes(const std::filesystem::path &path, const void *data, size_t size)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(static_cast<const char *>(data), std::streamsize(size));
  out.close();
  return bool(out);
}

std::function<bool(std::string *r_error)> oanim_file_writer(const Document &doc, const std::string &path)
{
  /* What action bone actions add isn't data: it is worked out again on opening. */
  auto data = std::make_shared<Document>(doc);
  animation_strip_action_bones(*data);
  /* Packed media beside it, as media/<id>/<name>: the bytes are shared, not copied. */
  std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> media;
  for (const std::string &id : media_packed_used(*data)) {
    if (std::shared_ptr<const std::string> bytes = media_packed_shared(id)) {
      media.push_back({std::string(MEDIA_ENTRY_PREFIX) + id + "/" + media_packed_name(id), std::move(bytes)});
    }
  }
  return [data, media = std::move(media), path](std::string *r_error) {
    g_file_folder = path_from_utf8(path).parent_path();
    const std::string text = oanim_document_to_json(*data);
    g_file_folder.clear();

    /* Build the archive in memory: miniz's file functions can't open non-ASCII Windows paths. */
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
      *r_error = "Could not start the archive";
      return false;
    }
    void *buffer = nullptr;
    size_t size = 0;
    /* Fast compression: on a large file the default level took several times as long for a few per cent. */
    bool built = mz_zip_writer_add_mem(&zip, DOCUMENT_ENTRY, text.data(), text.size(), MZ_BEST_SPEED);
    /* Video and sound are compressed already: stored. */
    for (const auto &[entry, bytes] : media) {
      if (built) {
        built = mz_zip_writer_add_mem(&zip, entry.c_str(), bytes->data(), bytes->size(), MZ_NO_COMPRESSION);
      }
    }
    /* The shape images file presets carry, as brush_shapes/<preset>/<file> (PNGs: stored). */
    for (size_t p = 0; p < data->brush_presets.size() && built; p++) {
      const BrushPreset &preset = data->brush_presets[p];
      for (size_t f = 0; f < preset.shape_files.size() && f < preset.shape_data.size() && built; f++) {
        const std::string entry = "brush_shapes/" + std::to_string(p) + "/" + preset.shape_files[f];
        built = mz_zip_writer_add_mem(&zip, entry.c_str(), preset.shape_data[f].data(), preset.shape_data[f].size(),
                                      MZ_NO_COMPRESSION);
      }
    }
    built = built && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
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
  };
}

bool oanim_file_write(const Document &doc, const std::string &path, std::string *r_error)
{
  return oanim_file_writer(doc, path)(r_error);
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
  b.preset = get_string(j, "preset");
  b.tip_angle = get_float(j, "tipAngle", b.tip_angle);
  b.roundness = std::clamp(get_float(j, "roundness", b.roundness), 0.01f, 1.0f);
  b.flip_x = get_bool(j, "flipX", b.flip_x);
  b.flip_y = get_bool(j, "flipY", b.flip_y);
  b.hardness = std::clamp(get_float(j, "hardness", b.hardness), 0.0f, 1.0f);
  b.pop_in = get_bool(j, "popIn", b.pop_in);
  b.shape_dynamics = get_bool(j, "shapeDynamics", b.shape_dynamics);
  b.scattering = get_bool(j, "scattering", b.scattering);
  b.color_dynamics = get_bool(j, "colorDynamics", b.color_dynamics);
  b.transfer = get_bool(j, "transfer", b.transfer);
  b.size_jitter = std::clamp(get_float(j, "sizeJitter", b.size_jitter), 0.0f, 1.0f);
  b.size_min = std::clamp(get_float(j, "sizeMin", b.size_min), 0.0f, 1.0f);
  b.size_fade_in = std::max(0, get_int(j, "sizeFadeIn", b.size_fade_in));
  b.size_fade_out = std::max(0, get_int(j, "sizeFadeOut", b.size_fade_out));
  b.roundness_jitter = std::clamp(get_float(j, "roundnessJitter", b.roundness_jitter), 0.0f, 1.0f);
  b.scatter = std::max(0.0f, get_float(j, "scatter", b.scatter));
  b.scatter_both = get_bool(j, "scatterBoth", b.scatter_both);
  b.count = std::clamp(get_int(j, "count", b.count), 1, 16);
  b.count_jitter = std::clamp(get_float(j, "countJitter", b.count_jitter), 0.0f, 1.0f);
  b.opacity_jitter = std::clamp(get_float(j, "opacityJitter", b.opacity_jitter), 0.0f, 1.0f);
  b.opacity_fade_in = std::max(0, get_int(j, "opacityFadeIn", b.opacity_fade_in));
  b.opacity_fade_out = std::max(0, get_int(j, "opacityFadeOut", b.opacity_fade_out));
  return b;
}

std::string brush_settings_to_json(const BrushSettings &settings)
{
  return brush_to_json(settings).dump();
}

bool brush_settings_from_json(const std::string &text, BrushSettings *r_settings)
{
  const json j = json::parse(text, nullptr, false);
  if (!j.is_object()) {
    return false;
  }
  *r_settings = brush_from_json(j);
  return true;
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


/* Drivers: [{"target": {"layer", "path", "component"}, "type", "expression", "vars": [{"name",
 * "source"}], "remap": {...} when on}]. */
/* A file from before mask rows took their layers' shapes as drawn: says which layers mask with a layer
 * that has masks of its own (those masks used to cut the shape first; now they don't), so a changed
 * look isn't a mystery. */
static void report_mask_sources_changed(const Document &doc)
{
  std::vector<std::string> pairs;
  for (const Layer *layer : document_layers_flat(doc)) {
    for (const LayerMask &m : layer->masks) {
      for (const uint64_t id : mask_card_layers(m)) {
        const Layer *source = layer_find(const_cast<Document &>(doc), id);
        if (source && source->id != layer->id && !source->masks.empty()) {
          pairs.push_back(layer->name + " (with " + source->name + ")");
        }
      }
    }
  }
  if (pairs.empty()) {
    return;
  }
  std::string list;
  for (size_t i = 0; i < pairs.size() && i < 6; i++) {
    list += (i ? ", " : "") + pairs[i];
  }
  if (pairs.size() > 6) {
    list += " and " + std::to_string(pairs.size() - 6) + " more";
  }
  report(ReportType::Warning, "Masks now use their layers' shapes before those layers' own masks, so these may look "
                              "different: " + list + ". Add the rows to the masked layer to get the old look");
}

/* A polyline as [x, y, x, y, ...]. */
static json points_to_json(const std::vector<float2> &points)
{
  json flat = json::array();
  for (const float2 &p : points) {
    flat.push_back(num(p.x));
    flat.push_back(num(p.y));
  }
  return flat;
}

static std::vector<float2> points_from_json(const json &j, const char *key)
{
  std::vector<float2> points;
  const json flat = j.value(key, json::array());
  if (!flat.is_array()) {
    return points;
  }
  for (size_t i = 0; i + 1 < flat.size(); i += 2) {
    if (flat[i].is_number() && flat[i + 1].is_number()) {
      points.push_back({flat[i].get<float>(), flat[i + 1].get<float>()});
    }
  }
  return points;
}

static json annotations_to_json(const Document &doc)
{
  json list = json::array();
  for (const AnnotationLayer &l : doc.annotations) {
    json strokes = json::array();
    for (const AnnotationStroke &s : l.strokes) {
      strokes.push_back({{"points", points_to_json(s.points)},
                         {"start", annotation_end_identifier(s.start)},
                         {"end", annotation_end_identifier(s.end)}});
    }
    list.push_back({{"name", l.name},
                    {"color", color_to_json(l.color)},
                    {"thickness", num(l.thickness)},
                    {"hidden", l.hidden},
                    {"locked", l.locked},
                    {"strokes", std::move(strokes)}});
  }
  return list;
}

static json guides_to_json(const Document &doc)
{
  json list = json::array();
  for (const DrawingGuide &g : doc.guides) {
    list.push_back({{"type", guide_type_identifier(g.type)},
                    {"points", points_to_json(g.points)},
                    {"lines", g.lines},
                    {"hidden", g.hidden},
                    {"locked", g.locked}});
  }
  return list;
}

static json drivers_to_json(const Document &doc)
{
  const auto ref_json = [](const DriverRef &r) {
    return json{{"layer", r.layer}, {"path", r.path}, {"component", r.component}};
  };
  json list = json::array();
  for (const Driver &d : doc.drivers) {
    static const char *const TYPES[] = {"EXPRESSION", "AVERAGE", "SUM", "MIN", "MAX"};
    json vars = json::array();
    for (const DriverVar &v : d.vars) {
      vars.push_back({{"name", v.name}, {"source", ref_json(v.source)}});
    }
    json dj = {{"target", ref_json(d.target)}, {"type", TYPES[int(d.type)]}, {"expression", d.expression}, {"vars", vars}};
    if (d.remap) {
      dj["remap"] = {{"from_min", num(d.from_min)}, {"from_max", num(d.from_max)}, {"to_min", num(d.to_min)},
                     {"to_max", num(d.to_max)}, {"clamp", d.clamp}};
    }
    if (d.rest_captured && !d.two_way) {
      dj["rest"] = num(d.target_rest);
    }
    if (d.two_way) {
      dj["two_way"] = {{"factor", num(d.factor)}, {"flip", d.flip}, {"source_rest", num(d.source_rest)},
                       {"target_rest", num(d.target_rest)}};
    }
    list.push_back(std::move(dj));
  }
  return list;
}

static json brush_presets_to_json(const Document &doc)
{
  json list = json::array();
  for (const BrushPreset &p : doc.brush_presets) {
    list.push_back({{"name", p.name}, {"brush", brush_to_json(p.settings)}, {"shape_files", p.shape_files}});
  }
  return list;
}

static json materials_to_json(const Document &doc)
{
  json list = json::array();
  for (const Material &m : doc.materials) {
    list.push_back({{"id", m.id}, {"name", m.name}, {"style", style_to_json(m.style)}});
  }
  return list;
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
  /* One number for both (bone scale before X and Y). */
  if (j.is_number()) {
    *r = {j.get<float>(), j.get<float>()};
    return true;
  }
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) {
    return false;
  }
  *r = {j[0].get<float>(), j[1].get<float>()};
  return true;
}

/* The file being read: its format version. */
static int g_loading_version = OANIM_FILE_VERSION;

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
    if (!key_type_from_identifier(get_string(k, "type", "KEYFRAME"), &key.type) || key.frame == 0) {
      key.type = KeyType::Key;
    }
    key.hold = std::max(0, get_int(k, "hold", key.hold));
    key.interval = std::max(1, get_int(k, "interval", key.interval));
    if (!ease_from_identifier(get_string(k, "ease", "QUAD"), &key.ease)) {
      key.ease = Ease::Quad;
    }
    key.overshoot = get_float(k, "overshoot", key.overshoot);
    key.elastic_amplitude = get_float(k, "elastic_amplitude", key.elastic_amplitude);
    key.elastic_period = std::max(0.02f, get_float(k, "elastic_period", key.elastic_period));
    if (auto v = k.find("bezier"); v != k.end() && v->is_array() && v->size() == 4) {
      for (size_t i = 0; i < 4; i++) {
        if ((*v)[i].is_number()) {
          key.bezier[i] = (*v)[i].get<float>();
        }
      }
      key.bezier = bezier_clamped(key.bezier);
    }
    key.noise_amplitude = get_float(k, "noise_amplitude", key.noise_amplitude);
    key.noise_frequency = get_float(k, "noise_frequency", key.noise_frequency);
    key.cycle_back = std::max(0, get_int(k, "cycle_back", key.cycle_back));
    key.cycle_absolute = get_bool(k, "cycle_absolute", false);
    key.cycle_frame = std::max(0, get_int(k, "cycle_frame", 0));
    key.color = get_int(k, "color", 0);
    for (const json &s : get_array(k, "speeds")) {
      if (s.is_number()) {
        key.speeds.push_back(s.get<float>());
      }
    }
    if constexpr (std::is_same_v<T, float2>) {
      const auto in = k.find("path_in"), out = k.find("path_out");
      if (path_handles_from_identifier(get_string(k, "path_handles", "AUTO"), &key.path_handles) &&
          key.path_handles != PathHandles::Auto &&
          !(in != k.end() && out != k.end() && value_from_json(*in, &key.path_in) &&
            value_from_json(*out, &key.path_out)))
      {
        key.path_handles = PathHandles::Auto;
      }
      for (const json &c : get_array(k, "path_controls")) {
        float2 v;
        if (value_from_json(c, &v)) {
          key.path_controls.push_back(v);
        }
      }
      for (const json &hj : get_array(k, "path_control_handles")) {
        if (key.path_control_handles.size() >= key.path_controls.size()) {
          break;
        }
        PathControlHandles h;
        h.aligned = get_bool(hj, "aligned", true);
        const auto hin = hj.find("in"), hout = hj.find("out");
        h.set = get_bool(hj, "set", false) && hin != hj.end() && hout != hj.end() && value_from_json(*hin, &h.in) &&
                value_from_json(*hout, &h.out);
        if (!h.set) {
          h.in = h.out = float2{0.0f, 0.0f};
        }
        key.path_control_handles.push_back(h);
      }
    }
    key.selected = get_bool(k, "selected", false);
    key.filler = get_bool(k, "filler", false);
    key.filler_inside = key.filler && get_bool(k, "filler_inside", false);
    if (!r_ch.key_at(key.frame)) {
      r_ch.keys.push_back(key);
    }
  }
  std::sort(r_ch.keys.begin(), r_ch.keys.end(), [](const Key<T> &a, const Key<T> &b) { return a.frame < b.frame; });
  /* Before format 4 a key's curve was for the stretch after it: now it's the one arriving at it. */
  if (g_loading_version < 4) {
    keys_curves_to_arriving(r_ch.keys);
  }
}

template<typename T> static void get_keys(const json &j, const char *name, Channel<T> &r_ch)
{
  get_key_list(j, name, r_ch);
  if (auto it = j.find(std::string(name) + "_pinned"); it != j.end() && it->is_boolean()) {
    r_ch.pinned = it->get<bool>();
  }
  if (auto s = j.find(std::string(name) + "_action_setup"); s != j.end() && s->is_object()) {
    for (const auto &[action, list] : s->items()) {
      Channel<T> other;
      const json wrapper = {{"keys", list}};
      get_key_list(wrapper, "keys", other);
      if (other.keys.size() == 1) {
        r_ch.action_setup[action] = std::move(other.keys.front());
      }
    }
  }
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

/* The viewports' overlays, by name (older files have none: the defaults). */
static constexpr std::pair<const char *, bool ViewportOverlays::*> OVERLAY_SWITCHES[] = {
    {"show", &ViewportOverlays::show},
    {"grid", &ViewportOverlays::grid},
    {"cursor", &ViewportOverlays::cursor},
    {"camera_frame", &ViewportOverlays::camera_frame},
    {"info", &ViewportOverlays::info},
    {"wireframe", &ViewportOverlays::wireframe},
    {"points", &ViewportOverlays::points},
    {"mask_outlines", &ViewportOverlays::mask_outlines},
    {"relationship_lines", &ViewportOverlays::relationship_lines},
    {"physics_ghosts", &ViewportOverlays::physics_ghosts},
    {"live_physics", &ViewportOverlays::live_physics},
    {"bone_shapes", &ViewportOverlays::bone_shapes},
    {"guides", &ViewportOverlays::guides},
    {"annotations", &ViewportOverlays::annotations},
};
static constexpr const char *HANDLE_DISPLAYS[] = {"NONE", "SELECTED", "SHAPE", "ALL"};
static constexpr const char *ARMATURE_DISPLAYS[] = {"OFF", "SELECTED", "ON"};

static json overlays_to_json(const ViewportOverlays &ov)
{
  json j = json::object();
  for (const auto &[name, field] : OVERLAY_SWITCHES) {
    j[name] = ov.*field;
  }
  j["handles"] = HANDLE_DISPLAYS[int(ov.handles)];
  j["armature"] = ARMATURE_DISPLAYS[int(ov.armature)];
  j["point_size"] = ov.point_size;
  j["handle_size"] = ov.handle_size;
  return j;
}

static void overlays_from_json(const json &j, ViewportOverlays &r_ov)
{
  if (!j.is_object()) {
    return;
  }
  for (const auto &[name, field] : OVERLAY_SWITCHES) {
    r_ov.*field = get_bool(j, name, r_ov.*field);
  }
  r_ov.point_size = std::clamp(get_float(j, "point_size", r_ov.point_size), OVERLAY_SIZE_MIN, OVERLAY_SIZE_MAX);
  r_ov.handle_size = std::clamp(get_float(j, "handle_size", r_ov.handle_size), OVERLAY_SIZE_MIN, OVERLAY_SIZE_MAX);
  const std::string handles = get_string(j, "handles", HANDLE_DISPLAYS[int(r_ov.handles)]);
  for (int i = 0; i < 4; i++) {
    if (handles == HANDLE_DISPLAYS[i]) {
      r_ov.handles = HandleDisplay(i);
    }
  }
  const std::string armature = get_string(j, "armature", ARMATURE_DISPLAYS[int(r_ov.armature)]);
  for (int i = 0; i < 3; i++) {
    if (armature == ARMATURE_DISPLAYS[i]) {
      r_ov.armature = ArmatureDisplay(i);
    }
  }
}

/* The action that was being edited in the file being read. */
static std::string g_loading_action;

template<typename T> static const std::vector<Key<T>> *mainline_keys(const Channel<T> &ch)
{
  if (g_loading_action.empty()) {
    return &ch.keys;
  }
  const auto it = ch.actions.find("");
  return it != ch.actions.end() ? &it->second : nullptr;
}

/* Format 2 and before: a point's or handle's setup is its mainline's frame-0 key, else its value. */
template<typename T> static T old_setup(const Channel<T> &ch, const T &value)
{
  const std::vector<Key<T>> *mainline = mainline_keys(ch);
  if (!mainline || mainline->empty()) {
    return value;
  }
  Channel<T> c;
  c.keys = *mainline;
  return c.evaluate(0);
}

static void migrate_drawing_setups(VectorDrawing &drawing)
{
  for (MeshPoint &p : drawing.points) {
    p.setup_pos = old_setup(p.pos_keys, p.pos);
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.setup_in = old_setup(cp.handle_in_keys, cp.handle_in);
      cp.setup_out = old_setup(cp.handle_out_keys, cp.handle_out);
    }
  }
}

static void migrate_drawing_keys(VectorDrawing &drawing)
{
  const auto every = [](auto &ch, auto &&fn) {
    fn(ch.keys);
    for (auto &[name, keys] : ch.actions) {
      fn(keys);
    }
  };
  for (MeshPoint &p : drawing.points) {
    every(p.pos_keys, [&](std::vector<Key<float2>> &keys) {
      for (Key<float2> &k : keys) {
        k.value -= p.setup_pos;
      }
    });
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      for (const bool in : {true, false}) {
        const float2 setup = in ? cp.setup_in : cp.setup_out;
        every(in ? cp.handle_in_keys : cp.handle_out_keys, [&](std::vector<Key<float2>> &keys) {
          /* In frame order, so whole turns carry on from key to key. */
          float turn = 0.0f;
          for (Key<float2> &k : keys) {
            k.value = handle_change(setup, k.value, turn);
            turn = k.value.x;
          }
        });
      }
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
      drawing.points.push_back({get_float2(p, "pos", {}), std::clamp(get_float(p, "radius", 1.0f), 0.0f, POINT_RADIUS_MAX),
                                get_bool(p, "selected", false)});
      drawing.points.back().id = get_id(p, "id");
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
      drawing.points.back().setup_pos = get_float2(p, "setup_pos", drawing.points.back().pos);
      get_keys(p, "radius_keys", drawing.points.back().radius_keys);
      if (auto it = p.find("detail"); it != p.end() && it->is_object()) {
        PointDetail &dt = drawing.points.back().detail;
        dt.level = std::clamp(get_int(*it, "level", 0), 0, DETAIL_LEVEL_MAX);
        dt.t = std::clamp(get_float(*it, "t", 0.5f), 0.0f, 1.0f);
        dt.pos = get_float2(*it, "pos", {});
        dt.in = get_float2(*it, "in", {});
        dt.out = get_float2(*it, "out", {});
        dt.setup_pos = get_float2(*it, "setup_pos", dt.pos);
        dt.setup_in = get_float2(*it, "setup_in", dt.in);
        dt.setup_out = get_float2(*it, "setup_out", dt.out);
      }
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
      point.select_in = get_bool(cp, "select_in", false);
      point.select_out = get_bool(cp, "select_out", false);
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
      point.setup_in = get_float2(cp, "setup_in", point.handle_in);
      point.setup_out = get_float2(cp, "setup_out", point.handle_out);
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
    shape.id = get_id(s, "id");
    shape.name = get_string(s, "name");
    shape.has_fill = get_bool(s, "fill", true);
    shape.has_outline = get_bool(s, "outline", true);
    if (auto it = s.find("style"); it != s.end()) {
      shape.style = style_from_json(*it);
    }
    shape.material = get_id(s, "material");
    shape.material_last = get_string(s, "material_last");
    shape.detail_level = std::clamp(get_int(s, "detail_level", 0), 0, DETAIL_LEVEL_MAX);
    if (const std::string rule = get_string(s, "fill_rule"); rule == "EVEN_ODD") {
      shape.fill_rule = FillRule::EvenOdd;
    }
    else if (rule == "NONZERO") {
      shape.fill_rule = FillRule::NonZero;
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
  /* Older files' points and shapes get their ids now. */
  drawing.next_id = std::max<uint64_t>(1, get_id(j, "next_id"));
  for (const json &gj : get_array(j, "groups")) {
    if (!gj.is_object()) {
      continue;
    }
    PointGroup g;
    g.name = get_string(gj, "name", "Group");
    g.locked = get_bool(gj, "locked", false);
    for (const json &m : get_array(gj, "points")) {
      if (m.is_array() && m.size() == 2 && m[0].is_number() && m[1].is_number()) {
        g.weights[m[0].get<uint64_t>()] = std::clamp(m[1].get<float>(), 0.0f, 1.0f);
      }
    }
    drawing.groups.push_back(std::move(g));
  }
  drawing.active_group = std::clamp(get_int(j, "active_group", 0), 0, std::max(0, int(drawing.groups.size()) - 1));
  for (const json &gj : get_array(j, "grids")) {
    if (!gj.is_object()) {
      continue;
    }
    DrawingGrid g;
    g.name = get_string(gj, "name", "Grid");
    g.columns = get_int(gj, "columns", 0);
    g.rows = get_int(gj, "rows", 0);
    for (const json &n : get_array(gj, "nodes")) {
      if (n.is_number_unsigned() || n.is_number_integer()) {
        g.nodes.push_back(n.get<uint64_t>());
      }
    }
    if (grid_valid(drawing, g)) {
      drawing.grids.push_back(std::move(g));
    }
  }
  drawing_ensure_ids(drawing);
  if (g_loading_version < 3) {
    /* Format 2 and before kept points' and handles' own values in their keys, as Moho does: the
     * setup is frame 0's key (the mainline's while an action was being edited); the stale handle
     * keys an old bug left are mended first, then every key becomes the change from the setup. */
    migrate_drawing_setups(drawing);
    drawing_repair_stale_handle_keys(drawing);
    migrate_drawing_setups(drawing);
    migrate_drawing_keys(drawing);
  }
  /* Older files' Automatic and Vector handles become stored ones now, from the setup pose. */
  drawing_init_handles(drawing);
  return drawing;
}

static ReferenceSettings reference_from_json(const json &j)
{
  ReferenceSettings rs;
  if (!j.is_object()) {
    return rs;
  }
  rs.edit_points = get_bool(j, "edit_points", false);
  rs.follow_animation = get_bool(j, "follow_animation", true);
  rs.match_original = get_bool(j, "match_original", true);
  for (const json &p : get_array(j, "points")) {
    if (const uint64_t id = get_id(p, "id"); id != 0) {
      ReferencePoint &o = rs.points[id];
      o.offset = get_float2(p, "offset", {});
      get_keys(p, "offset_keys", o.offset_keys);
    }
  }
  for (const json &h : get_array(j, "handles")) {
    const ReferenceHandleKey key{get_id(h, "point"), get_id(h, "toward"), get_string(h, "side") == "IN"};
    if (key.point != 0) {
      ReferencePoint &o = rs.handles[key];
      o.offset = get_float2(h, "offset", {});
      get_keys(h, "offset_keys", o.offset_keys);
    }
  }
  for (const json &e : get_array(j, "edges")) {
    const uint64_t from = get_id(e, "from"), to = get_id(e, "to");
    if (from != 0 && to != 0) {
      rs.edges[{from, to}] = get_bool(e, "hidden", true);
    }
  }
  for (const json &c : get_array(j, "own_channels")) {
    if (c.is_string()) {
      rs.own_channels.insert(c.get<std::string>());
    }
  }
  for (const json &l : get_array(j, "looks")) {
    const uint64_t id = get_id(l, "id");
    if (id == 0) {
      continue;
    }
    ReferenceLook look;
    look.fill_set = l.contains("fill");
    look.fill = get_color(l, "fill", {});
    look.line_set = l.contains("line");
    look.line = get_color(l, "line", {});
    look.width_set = l.contains("line_width");
    look.line_width = get_float(l, "line_width", 0.0f);
    look.brush_set = l.contains("brush");
    if (look.brush_set) {
      look.brush = brush_from_json(l["brush"]);
    }
    look.has_fill_set = l.contains("has_fill");
    look.has_fill = get_bool(l, "has_fill", true);
    look.has_outline_set = l.contains("has_outline");
    look.has_outline = get_bool(l, "has_outline", true);
    look.hidden = get_bool(l, "hidden", false);
    if (!look.empty()) {
      rs.looks[id] = look;
    }
  }
  for (const json &s : get_array(j, "swaps")) {
    rs.swaps.push_back({get_color(s, "from", {}), get_color(s, "to", {}), get_bool(s, "fill", true),
                        get_bool(s, "line", true)});
  }
  for (const json &w : get_array(j, "weights")) {
    const uint64_t id = get_id(w, "id");
    std::vector<BoneWeight> list;
    for (const json &e : get_array(w, "weights")) {
      if (e.is_array() && e.size() == 2 && e[0].is_number_unsigned() && e[1].is_number()) {
        list.push_back({e[0].get<uint64_t>(), std::clamp(e[1].get<float>(), 0.0f, 1.0f)});
      }
    }
    if (id != 0) {
      rs.weights[id] = std::move(list);
    }
  }
  return rs;
}

/* Moho-style masking read from an older file, by layer id: (mask_mode, group_mask). */
static std::map<uint64_t, std::pair<std::string, std::string>> g_old_masks;
/* A mask layer's Expand by a Pixel and Fills Only from an older file, by layer id. */
static std::map<uint64_t, std::pair<bool, bool>> g_old_mask_options;
/* Rows from files before mask cards (by layer id): each a step with the settings that are a card's
 * now, and the layer's Start With (`start`: saved, or from the first row). Old files' fixes below
 * work on these; then they become cards (mask_rows_to_cards). */
struct OldMaskRow {
  MaskStep step;
  bool enabled = true, render = true;
  MaskCuts cuts = MaskCuts::All;
  bool end_lines = false, end_lines_flat = false;
  float line_reach = 0.0f;
  Channel<float> line_reach_keys = Channel<float>::by_hand();
  bool show_expanded = true;
};
struct OldMaskRows {
  std::vector<OldMaskRow> rows;
  bool start_saved = false;
  bool start = false;
};
static std::map<uint64_t, OldMaskRows> g_old_rows;

/* Rows from before cards, as cards that show the same: one for the fills (the rows that masked them)
 * and one for the lines (the rows that masked them, ending lines at the card's edge if any row
 * ended them), or one for both when the two are the same rows; a row that was off, a card of its
 * own, off. */
static void mask_rows_to_cards(std::vector<Layer> &layers)
{
  for (Layer &layer : layers) {
    mask_rows_to_cards(layer.children);
    const auto it = g_old_rows.find(layer.id);
    if (it == g_old_rows.end() || it->second.rows.empty()) {
      continue;
    }
    const OldMaskRows &old = it->second;
    const bool start = old.start_saved ? old.start : old.rows.front().step.mode == MaskMode::Subtract;
    const auto card_of = [&](MaskCuts cuts, const std::vector<const OldMaskRow *> &rows, bool ends) {
      LayerMask card;
      card.start_shown = start;
      card.cuts = cuts;
      card.show_expanded = rows.front()->show_expanded;
      for (const OldMaskRow *r : rows) {
        card.steps.push_back(r->step);
        if (ends && r->end_lines && !card.end_lines) {
          card.end_lines = true;
          card.end_lines_flat = r->end_lines_flat;
          card.line_reach = r->line_reach;
          card.line_reach_keys = r->line_reach_keys;
        }
      }
      return card;
    };
    std::vector<const OldMaskRow *> fills, lines;
    for (const OldMaskRow &r : old.rows) {
      if (!r.enabled || !r.render) {
        LayerMask off = card_of(r.cuts, {&r}, true);
        mask_card_start_from_steps(off);
        off.enabled = r.enabled;
        off.render = r.render;
        layer.masks.push_back(std::move(off));
        continue;
      }
      if (r.cuts != MaskCuts::Lines) {
        fills.push_back(&r);
      }
      if (r.cuts != MaskCuts::Fills) {
        lines.push_back(&r);
      }
    }
    std::vector<LayerMask> cards;
    if (!fills.empty() && fills == lines) {
      cards.push_back(card_of(MaskCuts::All, fills, true));
    }
    else {
      if (!fills.empty()) {
        cards.push_back(card_of(MaskCuts::Fills, fills, false));
      }
      if (!lines.empty()) {
        cards.push_back(card_of(MaskCuts::Lines, lines, true));
      }
    }
    layer.masks.insert(layer.masks.begin(), cards.begin(), cards.end());
  }
}

/* The masks naming a layer that had the two options take them. */
static void migrate_old_mask_options(std::vector<Layer> &layers)
{
  for (Layer &layer : layers) {
    for (OldMaskRow &row : g_old_rows[layer.id].rows) {
      MaskStep &m = row.step;
      for (const uint64_t id : m.layers) {
        if (const auto it = g_old_mask_options.find(id); it != g_old_mask_options.end()) {
          m.grow = it->second.first ? std::max(m.grow, 1.0f) : m.grow;
          m.fills_only |= it->second.second;
        }
      }
    }
    migrate_old_mask_options(layer.children);
  }
}

/* Turns a masking group's Moho modes into masks that name layers. Bottom up, as Moho builds the
 * mask: Clear starts it over, Add adds a layer's shape, Subtract cuts one out; each Masked layer
 * then shows inside what was added (if anything was, or the group hides all) and outside what was
 * cut. The masking-only (invisible) layers become hidden layers, which still mask. A Subtract
 * followed by an Add of the same area is the one order this can't say exactly. */
static void migrate_old_masks(std::vector<Layer> &layers)
{
  for (Layer &group : layers) {
    migrate_old_masks(group.children);
    const auto g = g_old_masks.find(group.id);
    const std::string group_mask = g != g_old_masks.end() ? g->second.second : "NONE";
    if (!group.is_group() || group_mask == "NONE") {
      continue;
    }
    bool full = group_mask == "SHOW_ALL";
    std::vector<uint64_t> added, cut;
    for (auto it = group.children.rbegin(); it != group.children.rend(); ++it) {
      Layer &child = *it;
      const auto c = g_old_masks.find(child.id);
      const std::string mode = c != g_old_masks.end() ? c->second.first : "MASKED";
      if (mode == "CLEAR_ADD" || mode == "CLEAR_ADD_INVISIBLE") {
        added.clear();
        cut.clear();
        full = false;
      }
      if (mode == "ADD" || mode == "ADD_INVISIBLE" || mode == "CLEAR_ADD" || mode == "CLEAR_ADD_INVISIBLE") {
        added.push_back(child.id);
      }
      if (mode == "SUBTRACT_INVISIBLE") {
        cut.push_back(child.id);
      }
      if (mode == "ADD_INVISIBLE" || mode == "SUBTRACT_INVISIBLE" || mode == "CLEAR_ADD_INVISIBLE") {
        child.visible = false;
        continue;
      }
      if (mode != "MASKED") {
        continue;
      }
      std::vector<OldMaskRow> masks;
      if (!full && !added.empty()) {
        OldMaskRow add;
        add.step.layers = added;
        masks.push_back(add);
      }
      if (!cut.empty()) {
        OldMaskRow subtract;
        subtract.step.mode = MaskMode::Subtract;
        subtract.step.layers = cut;
        masks.push_back(subtract);
      }
      std::vector<OldMaskRow> &rows = g_old_rows[child.id].rows;
      rows.insert(rows.begin(), masks.begin(), masks.end());
    }
  }
}

template<size_t N> static int id_index(const char *const (&ids)[N], const std::string &id, int fallback)
{
  for (size_t i = 0; i < N; i++) {
    if (id == ids[i]) {
      return int(i);
    }
  }
  return fallback;
}

static TextSettings text_from_json(const json &j)
{
  TextSettings t;
  for (const json &r : get_array(j, "runs")) {
    if (!r.is_object()) {
      continue;
    }
    TextRun run;
    run.text = get_string(r, "text");
    run.font = get_string(r, "font", run.font);
    run.weight = std::clamp(get_int(r, "weight", 400), 1, 999);
    run.italic = get_bool(r, "italic", false);
    run.size = std::max(get_float(r, "size", run.size), 1e-4f);
    run.has_fill = get_bool(r, "fill", true);
    run.has_outline = get_bool(r, "outline", false);
    if (auto it = r.find("style"); it != r.end()) {
      run.style = style_from_json(*it);
    }
    run.material = get_id(r, "material");
    run.material_last = get_string(r, "material_last");
    t.runs.push_back(std::move(run));
  }
  if (t.runs.empty()) {
    t.runs.emplace_back();
  }
  const std::string align = get_string(j, "align", "LEFT");
  t.align = align == "CENTER" ? TextAlign::Center
            : align == "RIGHT" ? TextAlign::Right
            : align == "JUSTIFY" ? TextAlign::Justify
                                 : TextAlign::Left;
  t.line_spacing = get_float(j, "line_spacing", 1.0f);
  t.letter_spacing = get_float(j, "letter_spacing", 0.0f);
  t.word_spacing = get_float(j, "word_spacing", 0.0f);
  t.shear = get_float(j, "shear", 0.0f);
  t.box_width = std::max(get_float(j, "box_width", 0.0f), 0.0f);
  t.no_auto_key = get_bool(j, "dont_auto_key", true);
  return t;
}

static MediaSettings media_from_json(const json &j)
{
  MediaSettings m;
  m.fit = MediaFit(id_index(MEDIA_FIT_IDS, get_string(j, "fit", ""), 0));
  m.greyscale = get_bool(j, "greyscale", false);
  m.tint = get_color(j, "tint", m.tint);
  m.tint_amount = std::clamp(get_float(j, "tint_amount", 0.0f), 0.0f, 1.0f);
  m.in_front = get_bool(j, "in_front", false);
  m.no_auto_key = get_bool(j, "dont_auto_key", true);
  m.volume = std::clamp(get_float(j, "volume", 1.0f), 0.0f, 4.0f);
  m.muted = get_bool(j, "muted", false);
  m.next_clip_id = std::max<uint64_t>(get_id(j, "next_clip_id"), 1);
  std::error_code ec;
  for (const json &cj : get_array(j, "clips")) {
    if (!cj.is_object()) {
      continue;
    }
    MediaClip c;
    c.id = get_id(cj, "id");
    c.filepath = get_string(cj, "filepath", "");
    /* Found where it was, or else beside the file as it was relative to it. */
    const std::string rel = get_string(cj, "relative", "");
    if (!rel.empty() && !g_file_folder.empty() && !std::filesystem::is_regular_file(path_from_utf8(c.filepath), ec)) {
      const std::filesystem::path beside = (g_file_folder / path_from_utf8(rel)).lexically_normal();
      if (std::filesystem::is_regular_file(beside, ec)) {
        const std::u8string text = beside.u8string();
        c.filepath = std::string(text.begin(), text.end());
      }
    }
    c.sequence = get_bool(cj, "sequence", false);
    c.packed = get_string(cj, "packed", "");
    c.start = std::max(get_int(cj, "start", 1), 1);
    c.length = std::max(get_int(cj, "length", 1), 1);
    if (auto it = cj.find("in"); it != cj.end() && it->is_number()) {
      c.in = std::max(it->get<double>(), 0.0);
    }
    c.timing = MediaTiming(id_index(MEDIA_TIMING_IDS, get_string(cj, "timing", ""), 0));
    c.end = MediaEnd(id_index(MEDIA_END_IDS, get_string(cj, "end", ""), 0));
    if (c.id == 0 || std::any_of(m.clips.begin(), m.clips.end(), [&](const MediaClip &o) { return o.id == c.id; })) {
      c.id = m.next_clip_id++;
    }
    m.next_clip_id = std::max(m.next_clip_id, c.id + 1);
    m.clips.push_back(std::move(c));
  }
  std::stable_sort(m.clips.begin(), m.clips.end(), [](const MediaClip &a, const MediaClip &b) { return a.start < b.start; });
  return m;
}

/* Drawings made ahead, on every core, from their JSON (see oanim_document_from_json): layer_from_json takes
 * its drawing from here when it's there. */
static std::unordered_map<const json *, VectorDrawing> g_ready_drawings;

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
  layer.ignore_group_deforms = get_bool(j, "ignore_group_deforms", false);
  /* Files from before it: each point keyed on its own, as they were made. */
  layer.key_whole_drawing = get_bool(j, "key_whole_drawing", false);
  /* Saved on: its keys were made to agree when it was turned on; matching their timing again on opening would undo
   * timing edits made to one point since. */
  layer.key_whole_settled = layer.key_whole_drawing;
  layer.key_together = get_bool(j, "key_together", false);
  /* Saved on: its keys already agree, and turning it off lets its fillers go. */
  layer.key_together_settled = layer.key_together;
  layer.edit_descendants = get_bool(j, "edit_descendants", layer.type != LayerType::Bone);
  layer.sub_bone_group = get_bool(j, "sub_bone_group", true);
  layer.link_same_named = get_bool(j, "link_same_named", true);
  for (const json &a : get_array(j, "aliases")) {
    if (a.is_string()) {
      layer.switch_aliases.push_back(a.get<std::string>());
    }
  }
  layer.expanded = get_bool(j, "expanded", true);
  layer.selected = get_bool(j, "selected", false);
  layer.label_color = get_enum(j, "label_color", LabelColor::None);
  layer.position = get_float2(j, "position", {});
  layer.rotation = get_float(j, "rotation", 0.0f);
  layer.scale = get_float2(j, "scale", {1.0f, 1.0f});
  layer.shear = get_float2(j, "shear", {});
  layer.opacity = std::clamp(get_float(j, "opacity", 1.0f), 0.0f, 1.0f);
  layer.parent_bone = get_id(j, "parent_bone");
  /* Older files left it out when it was Layer (then the default): a layer bound to a bone there stays
   * Layer, and one that wasn't takes today's default (Points). */
  layer.parent_bone_points = j.contains("parent_bone_deform") ? get_string(j, "parent_bone_deform", "") == "POINTS"
                                                              : !j.contains("parent_bone");
  for (const json &a : get_array(j, "actions")) {
    if (a.is_string() && !a.get<std::string>().empty()) {
      layer.actions.push_back(a.get<std::string>());
    }
  }
  for (const json &fj : get_array(j, "fix_actions")) {
    if (!fj.is_object()) {
      continue;
    }
    FixAction f;
    f.name = get_string(fj, "name");
    for (const json &d : get_array(fj, "drivers")) {
      if (d.is_string()) {
        f.drivers.push_back(d.get<std::string>());
      }
    }
    const auto read_at = [&](const json &arr) {
      std::vector<float> at;
      for (const json &v : arr) {
        if (v.is_number()) {
          at.push_back(v.get<float>());
        }
      }
      at.resize(f.drivers.size(), 0.0f);
      return at;
    };
    for (const json &pj : get_array(fj, "poses")) {
      if (pj.is_object()) {
        FixPose pose;
        pose.frame = std::max(1, get_int(pj, "frame", 10));
        pose.keyed = get_bool(pj, "keyed", false);
        pose.at = read_at(get_array(pj, "at"));
        f.poses.push_back(std::move(pose));
      }
    }
    /* Saved before fixes had poses: one, where it was made ("full"), on its last frame ("length"). */
    if (!fj.contains("poses")) {
      FixPose pose;
      pose.frame = std::max(1, get_int(fj, "length", 24));
      pose.at = read_at(get_array(fj, "full"));
      f.poses.push_back(std::move(pose));
    }
    if (!f.name.empty() && std::find(layer.actions.begin(), layer.actions.end(), f.name) != layer.actions.end()) {
      layer.fix_actions.push_back(std::move(f));
    }
  }
  for (const json &pj : get_array(j, "action_plays")) {
    const std::string name = pj.is_object() ? get_string(pj, "name") : std::string();
    if (name.empty() || std::find(layer.actions.begin(), layer.actions.end(), name) == layer.actions.end()) {
      continue;
    }
    ActionPlay a;
    a.name = name;
    a.frame = std::max(0.0f, get_float(pj, "frame", 0.0f));
    a.mute = get_bool(pj, "mute", false);
    const std::string loop = get_string(pj, "loop", "AUTO");
    a.loop = loop == "ON" ? ActionLoop::On : loop == "OFF" ? ActionLoop::Off : ActionLoop::Auto;
    get_keys(pj, "frame_keys", a.frame_keys);
    layer.action_plays.push_back(std::move(a));
  }
  for (const json &mj : get_array(j, "modifiers")) {
    if (!mj.is_object()) {
      continue;
    }
    Modifier m;
    const std::string type = get_string(mj, "type", "MIRROR");
    /* The Boolean modifier was called Merge at first: files saying MERGE open as Boolean. */
    m.type = type == "ARMATURE"                       ? ModifierType::Armature
             : type == "BOOLEAN" || type == "MERGE" ? ModifierType::Boolean
             : type == "BEVEL"                      ? ModifierType::Bevel
             : type == "RESAMPLE" || type == "SUBDIVIDE" ? ModifierType::Resample
             : type == "CURVE_DEFORM"               ? ModifierType::CurveDeform
             : type == "CAGE_DEFORM"                ? ModifierType::CageDeform
             : type == "ENVELOPE_DEFORM"            ? ModifierType::CurveDeform
             : type == "FOLLOW_DEFORM"              ? ModifierType::FollowDeform
             : type == "MESH_DEFORM" || type == "VECTOR_DEFORM" ? ModifierType::MeshDeform
                                                    : ModifierType::Mirror;
    m.name = get_string(mj, "name", m.type == ModifierType::Armature ? "Armature"
                                    : m.type == ModifierType::Boolean  ? "Boolean"
                                    : m.type == ModifierType::Bevel    ? "Bevel"
                                    : m.type == ModifierType::Resample ? "Resample"
                                    : modifier_is_deform(m.type) ? deform_type_name(m.type)
                                                                     : "Mirror");
    /* Resample was called Subdivide: a name it was given by default ("Subdivide", "Subdivide.001") follows. */
    if (m.type == ModifierType::Resample && m.name.rfind("Subdivide", 0) == 0 &&
        (m.name.size() == 9 || (m.name[9] == '.' && m.name.find_first_not_of("0123456789", 10) == std::string::npos)))
    {
      m.name = "Resample" + m.name.substr(9);
    }
    if (modifier_is_deform(m.type)) {
      /* The single Vector Deform of before: Curve, Cage or Mesh Deform by its shape, once all is read. */
      m.deform_legacy = type == "VECTOR_DEFORM";
      m.deform_layer = get_id(mj, "layer");
      m.id = get_id(mj, "id");
      if (m.type == ModifierType::FollowDeform) {
        const auto pair = [&](const char *key, float2 &value) {
          if (const auto it = mj.find(key); it != mj.end() && it->is_array() && it->size() == 2) {
            value = {(*it)[0].get<float>(), (*it)[1].get<float>()};
          }
        };
        pair("point", m.deform_point);
        pair("point2", m.deform_point2);
        pair("shift", m.deform_shift);
        pair("shift2", m.deform_shift2);
        m.deform_show_point = get_bool(mj, "show_point", false);
        m.deform_two = get_bool(mj, "two", false);
        m.deform_rotation = get_bool(mj, "rotation", true);
        m.deform_scale = get_bool(mj, "scale", false);
        get_keys(mj, "shift_keys", m.deform_shift_keys);
        get_keys(mj, "shift2_keys", m.deform_shift2_keys);
        m.deform_point_id = get_id(mj, "point_id");
        m.deform_point2_id = get_id(mj, "point2_id");
      }
      m.deform_shape = get_id(mj, "shape");
      m.deform_custom = get_string(mj, "weights", "AUTOMATIC") == "CUSTOM";
      m.deform_group = get_string(mj, "group", "");
      m.deform_strength = std::clamp(get_float(mj, "strength", 1.0f), 0.0f, 1.0f);
      get_keys(mj, "strength_keys", m.deform_strength_keys);
      /* Precision is how far a bent line may stray ("tolerance"); files from when it was a spacing
       * ("precision") take the default. */
      m.deform_precision = std::max(0.0f, get_float(mj, "tolerance", m.deform_precision));
      m.deform_stretch = get_bool(mj, "stretch", true);
      m.deform_part_from = get_id(mj, "part_from");
      m.deform_part_to = get_id(mj, "part_to");
      m.deform_resample_self = get_bool(mj, "resample_self", false);
      const std::string limit = get_string(mj, "limit", "NONE");
      m.follow_limit = limit == "SHAPE" ? FollowLimit::Shape : limit == "GROUP" ? FollowLimit::Group : FollowLimit::None;
      m.follow_limit_shape = get_id(mj, "limit_shape");
      m.follow_limit_group = get_string(mj, "limit_group", "");
      m.follow_limit_invert = get_bool(mj, "limit_invert", false);
      m.subdiv_by_length = get_string(mj, "split", "COUNT") != "COUNT";
      m.subdiv_spacing = std::max(1e-4f, get_float(mj, "spacing", m.subdiv_spacing));
      m.subdiv_pieces = std::clamp(get_int(mj, "pieces", m.subdiv_pieces), 1, 256);
      /* Before format 5 Cage Deform had no Envelope method: "ENVELOPE" was Outline's first name. */
      const std::string cage_method = get_string(mj, "cage_method", "MEAN_VALUE");
      if (g_loading_version < 5 && cage_method == "ENVELOPE") {
        m.deform_cage_method = CageMethod::Outline;
      }
      else if (!cage_method_from_id(cage_method, m.deform_cage_method)) {
        m.deform_cage_method = CageMethod::MeanValue;
      }
      if (!mesh_method_from_id(get_string(mj, "mesh_method", "CELLS"), m.deform_mesh_method)) {
        m.deform_mesh_method = MeshMethod::Cells;
      }
      m.deform_smooth = get_bool(mj, "smooth", false);
      m.deform_blend = std::clamp(get_float(mj, "blend", 0.5f), 0.0f, 1.0f);
      m.deform_range = std::clamp(get_float(mj, "range", 1.0f), 0.0f, 1.0f);
      m.deform_falloff = std::clamp(get_float(mj, "falloff", 0.25f), 0.0f, 1.0f);
      /* Blended was called Weighted until 2026-11-03. */
      const std::string envelope_method = get_string(mj, "envelope_method", "STANDARD");
      if (envelope_method == "WEIGHTED") {
        m.deform_envelope_method = EnvelopeMethod::Blended;
      }
      else if (!envelope_method_from_id(envelope_method, m.deform_envelope_method)) {
        m.deform_envelope_method = EnvelopeMethod::Standard;
      }
      m.deform_pre_blend = std::max(get_float(mj, "pre_blend", 0.0f), 0.0f);
      m.deform_post_blend = get_float(mj, "post_blend", 0.0f);
      if (!curve_method_from_id(get_string(mj, "curve_method", "SMOOTH"), m.deform_curve_method)) {
        m.deform_curve_method = CurveMethod::Smooth;
      }
      if (!influence_type_from_id(get_string(mj, "influence", "EVERYWHERE"), m.deform_influence)) {
        m.deform_influence = InfluenceType::Everywhere;
      }
      m.deform_influence_symmetric = get_bool(mj, "influence_symmetric", true);
      const Modifier fresh;
      m.deform_influence_left = std::max(get_float(mj, "influence_left", fresh.deform_influence_left), 0.0f);
      m.deform_influence_right = std::max(get_float(mj, "influence_right", fresh.deform_influence_right), 0.0f);
      m.deform_influence_begin = std::max(get_float(mj, "influence_begin", fresh.deform_influence_begin), 0.0f);
      m.deform_influence_end = std::max(get_float(mj, "influence_end", fresh.deform_influence_end), 0.0f);
      m.deform_influence_fade = std::max(get_float(mj, "influence_fade", fresh.deform_influence_fade), 0.0f);
      /* Envelope Deform, a modifier of its own until 2026-11-03: Curve Deform on an open line, Cage Deform on a
       * closed shape, either with the Envelope method and every setting it had (vector_deform_resolve_legacy picks
       * which once all is read). */
      if (type == "ENVELOPE_DEFORM") {
        m.deform_curve_method = CurveMethod::Envelope;
        m.deform_cage_method = CageMethod::Envelope;
        m.deform_legacy = true;
      }
    }
    if (m.type == ModifierType::Resample) {
      m.subdiv_by_length = get_string(mj, "split", "LENGTH") != "COUNT";
      m.subdiv_spacing = std::max(1e-4f, get_float(mj, "spacing", m.subdiv_spacing));
      m.subdiv_render_spacing = std::max(0.0f, get_float(mj, "render_spacing", 0.0f));
      m.subdiv_pieces = std::clamp(get_int(mj, "pieces", m.subdiv_pieces), 1, 256);
      m.subdiv_max_pieces = std::clamp(get_int(mj, "max_pieces", m.subdiv_max_pieces), 1, 256);
      m.subdiv_from_bones = get_string(mj, "weights", "BLEND") == "BONES";
      m.subdiv_preserve_volume = get_bool(mj, "preserve_volume", true);
      m.subdiv_group = get_string(mj, "group", "");
    }
    if (m.type == ModifierType::Bevel) {
      const std::string limit = get_string(mj, "limit", "ANGLE");
      m.bevel_limit = limit == "GROUP" ? BevelLimit::Group : limit == "NONE" ? BevelLimit::None : BevelLimit::Angle;
      const std::string width = get_string(mj, "width_type", "OFFSET");
      m.bevel_width = width == "WIDTH"     ? BevelWidth::Width
                      : width == "DEPTH"   ? BevelWidth::Depth
                      : width == "PERCENT" ? BevelWidth::Percent
                                           : BevelWidth::Offset;
      m.bevel_angle = std::clamp(get_float(mj, "angle", m.bevel_angle), 0.0f, 3.1415927f);
      m.bevel_group = get_string(mj, "group", "");
      m.bevel_amount = std::max(0.0f, get_float(mj, "amount", m.bevel_amount));
      get_keys(mj, "amount_keys", m.bevel_amount_keys);
      m.bevel_profile = std::clamp(get_float(mj, "profile", m.bevel_profile), 0.0f, 1.0f);
      m.bevel_clamp = get_bool(mj, "clamp", true);
    }
    /* Its old default name ("Merge", "Merge.001") goes with it. */
    if (type == "MERGE" && m.name.rfind("Merge", 0) == 0 &&
        (m.name.size() == 5 || (m.name.size() == 9 && m.name[5] == '.')))
    {
      m.name = "Boolean" + m.name.substr(5);
    }
    m.show_viewport = get_bool(mj, "show_viewport", true);
    m.show_render = get_bool(mj, "show_render", true);
    m.show_points = get_bool(mj, "show_points", false);
    m.show_mesh = get_bool(mj, "show_mesh", false);
    m.show_expanded = get_bool(mj, "show_expanded", true);
    m.axis_x = get_bool(mj, "axis_x", true);
    m.axis_y = get_bool(mj, "axis_y", false);
    m.center = get_float2(mj, "center", {});
    m.merge = get_bool(mj, "merge", true);
    m.merge_distance = std::max(0.0f, get_float(mj, "merge_distance", m.merge_distance));
    m.mirror_bones = get_bool(mj, "mirror_bones", true);
    if (m.type == ModifierType::Boolean) {
      const auto ids = [](const json &arr) {
        std::vector<uint64_t> out;
        if (arr.is_array()) {
          for (const json &v : arr) {
            if (v.is_number_unsigned() || v.is_number_integer()) {
              out.push_back(v.get<uint64_t>());
            }
          }
        }
        return out;
      };
      m.merge_shapes = ids(mj.value("shapes", json::array()));
      for (const json &tj : get_array(mj, "targets")) {
        if (tj.is_object() && tj.contains("layer") && tj["layer"].is_number()) {
          m.merge_targets.push_back({tj["layer"].get<uint64_t>(), ids(tj.value("shapes", json::array()))});
        }
      }
      const std::string op = get_string(mj, "operation", "UNION");
      m.merge_operation = op == "SUBTRACT"    ? MergeOperation::Subtract
                          : op == "INTERSECT" ? MergeOperation::Intersect
                                              : MergeOperation::Union;
      m.merge_smoothing = std::max(0.0f, get_float(mj, "smoothing", 0.0f));
      m.merge_corners_only = get_bool(mj, "corners_only", false);
      get_keys(mj, "smoothing_keys", m.merge_smoothing_keys);
    }
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
    b.setup_position = get_float2(bj, "setup_position", b.position);
    b.setup_angle = get_float(bj, "setup_angle", b.angle);
    b.length = std::max(0.0f, get_float(bj, "length", 0.5f));
    if (auto it = bj.find("scale"); it == bj.end() || !value_from_json(*it, &b.scale)) {
      b.scale = {1.0f, 1.0f};
    }
    b.strength = std::max(0.0f, get_float(bj, "strength", 1.0f));
    b.color_set = std::max(0, bone_color_set_from_identifier(bj.value("color_set", std::string("DEFAULT"))));
    if (const json shades = bj.value("custom_colors", json()); shades.is_array() && shades.size() == 3) {
      for (size_t k = 0; k < 3; k++) {
        if (shades[k].is_array() && shades[k].size() >= 3) {
          b.custom_colors[k] = {shades[k][0].get<float>(), shades[k][1].get<float>(), shades[k][2].get<float>(), 1.0f};
        }
      }
    }
    b.connected = bj.contains("connected") && bj["connected"].is_boolean() && bj["connected"].get<bool>();
    b.inherit_rotation = get_bool(bj, "inherit_rotation", true);
    /* Files from before ride a controller's box, which carried nothing. */
    b.follow_knob = get_bool(bj, "follow_knob", false);
    if (const auto it = bj.find("swap"); it != bj.end() && it->is_array() && it->size() == 2 &&
        (*it)[0].is_number_unsigned() && (*it)[1].is_number_unsigned())
    {
      b.swap_layer = (*it)[0].get<uint64_t>();
      b.swap_bone = (*it)[1].get<uint64_t>();
    }
    if (const auto it = bj.find("outer_parent"); it != bj.end() && it->is_array() && it->size() == 2 &&
        (*it)[0].is_number_unsigned() && (*it)[1].is_number_unsigned())
    {
      b.outer_layer = (*it)[0].get<uint64_t>();
      b.outer_bone = (*it)[1].get<uint64_t>();
    }
    b.local_location = get_bool(bj, "local_location", true);
    if (const auto it = bj.find("inherit_scale"); it != bj.end() && it->is_string()) {
      const std::string s = it->get<std::string>();
      b.inherit_scale = s == "UNIFORM" ? InheritScale::Uniform : s == "NONE" ? InheritScale::None : InheritScale::Full;
    }
    b.selected = get_bool(bj, "selected", false);
    b.select_head = get_bool(bj, "select_head", false);
    b.select_tail = get_bool(bj, "select_tail", false);
    get_keys(bj, "position_keys", b.position_keys);
    get_keys(bj, "angle_keys", b.angle_keys);
    get_keys(bj, "scale_keys", b.scale_keys);
    b.depth = std::max(1e-4f, get_float(bj, "depth", 1.0f));
    get_keys(bj, "depth_keys", b.depth_keys);
    b.ik_ignore = get_bool(bj, "ik_ignore", false);
    b.arc_ik = get_bool(bj, "arc_ik", false);
    const std::string shape = bj.value("shape", std::string());
    b.shape = shape == "CUSTOM" ? BoneShape::Custom : BoneShape::None;
    /* Files from before keep pins as a shape: they are the Pin type now, drawn the same. */
    if (shape == "PIN" || bj.value("type", std::string()) == "PIN") {
      b.type = BoneType::Pin;
    }
    b.pin_size = std::clamp(get_float(bj, "pin_size", 1.0f), 0.1f, 20.0f);
    b.shape_layer = bj.value("shape_layer", uint64_t(0));
    if (bj.value("type", std::string()) == "CONTROLLER_2D") {
      b.type = BoneType::Controller2D;
      if (const json actions = bj.value("controller_actions", json()); actions.is_object()) {
        for (int side = 0; side < CONTROLLER_SIDES; side++) {
          b.controller_actions[side] = actions.value(controller_side_id(side), std::string());
        }
      }
      b.controller_frame_size = std::max(get_float(bj, "controller_frame_size", b.controller_frame_size), 1e-5f);
      b.controller_size_auto = get_bool(bj, "controller_size_auto", true);
      b.controller_box_angle = get_float(bj, "controller_box_angle", 0.0f);
      /* Knob Stays in Place was saved under its first name. */
      b.controller_knob_independent = get_bool(bj, "controller_knob_independent", get_bool(bj, "controller_knob_stays", false));
      b.controller_knob_parent = bj.value("controller_knob_parent", uint64_t(0));
      for (const json &sj : get_array(bj, "controller_spots")) {
        const json at = sj.value("at", json());
        if (sj.is_object() && at.is_array() && at.size() == 2 && at[0].is_number() && at[1].is_number()) {
          b.controller_spots.push_back({sj.value("name", std::string("Spot")), {at[0].get<float>(), at[1].get<float>()}});
        }
      }
    }
    for (const json &c : get_array(bj, "collections")) {
      if (c.is_number_unsigned()) {
        b.collections.push_back(c.get<uint64_t>());
      }
    }
    for (const json &cj : get_array(bj, "constraints")) {
      if (!cj.is_object()) {
        continue;
      }
      BoneConstraint c;
      if (!bone_constraint_from_identifier(get_string(cj, "type"), &c.type)) {
        continue;
      }
      /* Saved before the bone's one Weight, each channel had its own (Position's 0 by default). */
      struct OldWeight {
        float weight = 1.0f;
        Channel<float> keys = Channel<float>::by_hand();
      };
      std::map<const BoneSpring *, OldWeight> old_weight;
      const auto spring = [&](const char *name, BoneSpring &s) {
        auto it = cj.find(name);
        if (it == cj.end() || !it->is_object()) {
          return;
        }
        OldWeight &ow = old_weight[&s];
        s.enabled = get_bool(*it, "enabled", s.enabled);
        get_keys(*it, "settle_speed_keys", s.settle_speed_keys);
        get_keys(*it, "bounce_keys", s.bounce_keys);
        get_keys(*it, "weight_keys", ow.keys);
        get_keys(*it, "inertia_keys", s.inertia_keys);
        get_keys(*it, "stretch_keys", s.stretch_keys);
        get_keys(*it, "limit_keys", s.limit_keys);
        if (it->contains("torque")) {
          /* Saved before Weight: Torque bent a bone half as far. */
          ow.weight = get_float(*it, "torque", 2.0f) / 2.0f;
          if (it->contains("spring") && !it->contains("settle_speed")) {
            /* Saved with Moho's Spring, Damping and Weight (scaled by 89 and 7.5 a second): the
             * same spring as Settle Speed and Bounce, and the same bend from gravity. */
            const float mass = std::max(0.01f, get_float(*it, "weight", 2.0f));
            const float omega2 = std::max(1e-3f, 89.0f * get_float(*it, "spring", 2.0f) / mass);
            const float decay = 7.5f * get_float(*it, "damping", 1.0f) / mass / 2.0f;
            const float zeta = decay / std::sqrt(omega2);
            s.settle_speed = decay / 8.0f;
            s.bounce = zeta >= 1.0f ? 0.0f : std::exp(-zeta * PI / std::sqrt(1.0f - zeta * zeta));
            ow.weight *= 5.0f / mass / omega2 / 0.028f;
          }
        }
        else {
          ow.weight = std::max(0.0f, get_float(*it, "weight", ow.weight));
        }
        s.settle_speed = std::clamp(get_float(*it, "settle_speed", s.settle_speed), 0.05f, 20.0f);
        s.bounce = std::clamp(get_float(*it, "bounce", s.bounce), 0.0f, 0.9f);
        s.inertia = std::max(0.0f, get_float(*it, "inertia", s.inertia));
        s.stretch = std::max(0.0f, get_float(*it, "stretch", s.stretch));
        /* Saved before Use Limit: a limit of 0 was none. */
        const float limit = get_float(*it, "limit", s.limit);
        s.use_limit = get_bool(*it, "use_limit", limit > 0.0f);
        if (limit > 0.0f) {
          s.limit = limit;
        }
        if (it->contains("use_squash_limit")) {
          s.use_squash_limit = get_bool(*it, "use_squash_limit", false);
          s.squash_limit = std::clamp(get_float(*it, "squash_limit", s.squash_limit), 1e-4f, 0.9f);
          get_keys(*it, "squash_limit_keys", s.squash_limit_keys);
        }
        else {
          /* Saved before Max Squash: the one limit held both ways. */
          s.use_squash_limit = s.use_limit;
          s.squash_limit = std::min(s.limit, 0.9f);
          s.squash_limit_keys = s.limit_keys;
        }
      };
      c.name = get_string(cj, "name", bone_constraint_name(c.type));
      c.enabled = get_bool(cj, "enabled", true);
      c.expanded = get_bool(cj, "expanded", true);
      c.influence = std::clamp(get_float(cj, "influence", 1.0f), 0.0f, 1.0f);
      if (cj.contains("min_turn") || cj.contains("max_turn")) {
        c.min_angle = get_float(cj, "min_turn", c.min_angle);
        c.max_angle = get_float(cj, "max_turn", c.max_angle);
      }
      else if (cj.contains("min_angle") || cj.contains("max_angle")) {
        /* Saved as angles in the parent's space: now turns from the setup angle. */
        c.min_angle = get_float(cj, "min_angle", c.min_angle + b.setup_angle) - b.setup_angle;
        c.max_angle = get_float(cj, "max_angle", c.max_angle + b.setup_angle) - b.setup_angle;
      }
      c.amount = get_float(cj, "amount", c.amount);
      c.target = get_id(cj, "target");
      c.factor = get_float(cj, "factor", c.factor);
      /* Saved before Copy Position could go to its target: it copied the target's move. */
      c.follow_move = get_bool(cj, "follow_move", true);
      c.delay = std::max(0, get_int(cj, "delay", 0));
      c.chain = std::max(0, get_int(cj, "chain", 0));
      c.max_stretch = std::max(1.0f, get_float(cj, "max_stretch", 1.0f));
      c.shape_layer = get_id(cj, "shape_layer");
      c.shape_id = get_id(cj, "shape");
      c.follow_layer = get_id(cj, "follow_layer");
      c.follow_modifier = get_id(cj, "follow_modifier");
      if (const auto it = cj.find("follow_point"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_point = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      if (const auto it = cj.find("follow_rest"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_rest = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      c.follow_show = get_bool(cj, "follow_show", false);
      c.follow_two = get_bool(cj, "follow_two", false);
      if (const auto it = cj.find("follow_point2"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_point2 = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      if (const auto it = cj.find("follow_rest2"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_rest2 = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      c.follow_rotation = get_bool(cj, "follow_rotation", true);
      /* Before one point could turn, Rotation was saved on with nothing to do: such a bone stays still. */
      if (!c.follow_two && !cj.contains("follow_scale_mode")) {
        c.follow_rotation = false;
      }
      c.follow_scale = get_bool(cj, "follow_scale", false);
      if (const auto it = cj.find("follow_shift"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_shift = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      if (const auto it = cj.find("follow_shift2"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_shift2 = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      get_keys(cj, "follow_shift_keys", c.follow_shift_keys);
      get_keys(cj, "follow_shift2_keys", c.follow_shift2_keys);
      c.follow_point_id = get_id(cj, "follow_point_id");
      c.follow_point2_id = get_id(cj, "follow_point2_id");
      c.follow_three = c.follow_two && get_bool(cj, "follow_three", false);
      if (const auto it = cj.find("follow_point3"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_point3 = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      if (const auto it = cj.find("follow_rest3"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_rest3 = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      if (const auto it = cj.find("follow_shift3"); it != cj.end() && it->is_array() && it->size() == 2) {
        c.follow_shift3 = {(*it)[0].get<float>(), (*it)[1].get<float>()};
      }
      get_keys(cj, "follow_shift3_keys", c.follow_shift3_keys);
      c.follow_point3_id = get_id(cj, "follow_point3_id");
      c.follow_scale_mode =
          get_string(cj, "follow_scale_mode", "UNIFORM") == "ALONG_LINE" ? FollowScaleMode::AlongLine : FollowScaleMode::Uniform;
      c.follow_volume = get_float(cj, "follow_volume", 1.0f);
      c.shape_fit = get_string(cj, "fit", "WHOLE") == "CENTER" ? ShapeFit::Center : ShapeFit::Whole;
      c.fit_layer = get_id(cj, "fit_layer");
      c.overlap = std::clamp(get_float(cj, "overlap", 0.0f), 0.0f, 1.0f);
      c.softness = std::max(0.0f, get_float(cj, "softness", 0.0f));
      c.affect_transform = get_bool(cj, "affect_transform", true);
      c.child_position = get_bool(cj, "child_position", true);
      c.child_rotation = get_bool(cj, "child_rotation", true);
      c.child_scale = get_bool(cj, "child_scale", true);
      /* Saved with one Own Gravity & Wind for both. */
      const bool own_forces = get_bool(cj, "own_forces", false);
      c.own_gravity = get_bool(cj, "own_gravity", own_forces);
      c.own_wind = get_bool(cj, "own_wind", own_forces);
      c.wind_catch = std::clamp(get_float(cj, "wind_catch", 1.0f), 0.0f, 10.0f);
      get_keys(cj, "wind_catch_keys", c.wind_catch_keys);
      c.gravity = get_float(cj, "gravity", 0.0f);
      c.wind = get_float(cj, "wind", 0.0f);
      c.wind_direction = get_float(cj, "wind_direction", 0.0f);
      c.wind_gusts = std::clamp(get_float(cj, "wind_gusts", 0.0f), 0.0f, 2.0f);
      c.wind_gust_speed = std::clamp(get_float(cj, "wind_gust_speed", 1.0f), 0.05f, 10.0f);
      c.wind_turbulence = std::clamp(get_float(cj, "wind_turbulence", 0.0f), 0.0f, 2.0f);
      /* Older files: it had its own if they were ticked. */
      const bool own_set = get_bool(cj, "own_forces_set", own_forces);
      c.own_gravity_set = get_bool(cj, "own_gravity_set", own_set);
      c.own_wind_set = get_bool(cj, "own_wind_set", own_set);
      spring("angle_spring", c.angle_spring);
      spring("position_spring", c.position_spring);
      spring("scale_spring", c.scale_spring);
      if (cj.contains("weight")) {
        c.weight = std::max(0.0f, get_float(cj, "weight", 1.0f));
        get_keys(cj, "weight_keys", c.weight_keys);
      }
      else if (c.type == BoneConstraintType::Dynamics) {
        /* The Weight of the channel that mattered most: Angle's, else the first one on. */
        const BoneSpring *from = &c.angle_spring;
        for (const BoneSpring *s : {&c.angle_spring, &c.scale_spring, &c.position_spring}) {
          if (s->enabled) {
            from = s;
            break;
          }
        }
        if (const auto it = old_weight.find(from); it != old_weight.end()) {
          c.weight = it->second.weight;
          c.weight_keys = it->second.keys;
        }
      }
      get_keys(cj, "influence_keys", c.influence_keys);
      get_keys(cj, "gravity_keys", c.gravity_keys);
      get_keys(cj, "wind_keys", c.wind_keys);
      get_keys(cj, "wind_direction_keys", c.wind_direction_keys);
      get_keys(cj, "wind_gusts_keys", c.wind_gusts_keys);
      get_keys(cj, "wind_gust_speed_keys", c.wind_gust_speed_keys);
      get_keys(cj, "wind_turbulence_keys", c.wind_turbulence_keys);
      b.constraints.push_back(std::move(c));
    }
    layer.next_bone_id = std::max(layer.next_bone_id, b.id + 1);
    layer.bones.push_back(std::move(b));
  }
  layer.next_bone_id = std::max<uint64_t>(layer.next_bone_id, get_id(j, "next_bone_id"));
  layer.active_bone = get_id(j, "active_bone");
  for (const json &cj : get_array(j, "bone_collections")) {
    if (!cj.is_object()) {
      continue;
    }
    BoneCollection c;
    c.id = get_id(cj, "id");
    c.name = get_string(cj, "name", "Collection");
    c.visible = get_bool(cj, "visible", true);
    c.solo = get_bool(cj, "solo", false);
    layer.next_collection_id = std::max(layer.next_collection_id, c.id + 1);
    layer.bone_collections.push_back(std::move(c));
  }
  layer.next_collection_id = std::max<uint64_t>(layer.next_collection_id, get_id(j, "next_collection_id"));
  layer.active_collection = get_id(j, "active_collection");
  for (const json &c : get_array(j, "collections")) {
    if (c.is_number_unsigned()) {
      layer.collections.push_back(c.get<uint64_t>());
    }
  }
  if (const json all = j.value("all_bones", json()); all.is_object()) {
    layer.all_bones_visible = get_bool(all, "visible", true);
    layer.all_bones_solo = get_bool(all, "solo", false);
  }
  /* Files saved while collections replaced Editable in Bone Group: a drawing in one is editable. */
  if (!j.contains("edit_with_bones") && !layer.collections.empty()) {
    layer.edit_with_bones = true;
  }
  get_keys(j, "position_keys", layer.position_keys);
  get_keys(j, "rotation_keys", layer.rotation_keys);
  get_keys(j, "scale_keys", layer.scale_keys);
  get_keys(j, "shear_keys", layer.shear_keys);
  get_keys(j, "opacity_keys", layer.opacity_keys);
  get_keys(j, "visible_keys", layer.visible_keys);
  layer.draw_order = float(get_int(j, "draw_order", 0));
  layer.clip_to_below = get_bool(j, "clip_to_below", false);
  layer.clip_exclude_strokes = get_bool(j, "clip_exclude_strokes", false);
  get_keys(j, "draw_order_keys", layer.draw_order_keys);
  if (layer.type == LayerType::Switch) {
    layer.switch_value = float(get_int(j, "switch", 0));
    if (const auto it = j.find("switch_link"); it != j.end() && it->is_number_unsigned()) {
      layer.switch_link = it->get<uint64_t>();
    }
    layer.switch_link_off = get_bool(j, "switch_link_off", false);
    get_keys(j, "switch_keys", layer.switch_keys);
  }
  const auto read_step = [](auto &&self, const json &sj, MaskStep &m) -> void {
    m.name = get_string(sj, "name");
    m.global = get_id(sj, "global");
    m.group = get_bool(sj, "group", false);
    m.whole = get_bool(sj, "whole", false);
    for (const json &cj : get_array(sj, "children")) {
      if (cj.is_object()) {
        MaskStep child;
        if (!mask_mode_from_identifier(get_string(cj, "mode", "ADD"), &child.mode)) {
          child.mode = MaskMode::Add;
        }
        child.invert = get_bool(cj, "invert", false);
        self(self, cj, child);
        m.children.push_back(std::move(child));
      }
    }
    m.grow = std::clamp(get_float(sj, "grow", get_bool(sj, "expand", false) ? 1.0f : 0.0f), -64.0f, 64.0f);
    m.fills_only = get_bool(sj, "fills_only", false);
    m.strokes_only = !m.fills_only && get_bool(sj, "strokes_only", false);
    m.inside_stroke = get_bool(sj, "inside_stroke", false);
    m.filled = get_bool(sj, "filled", false);
    if (!mask_extend_from_identifier(get_string(sj, "extend", "NONE"), &m.extend)) {
      m.extend = MaskExtend::None;
    }
    /* Files from before it: Union, which every step was. */
    if (!mask_combine_from_identifier(get_string(sj, "combine", "UNION"), &m.combine)) {
      m.combine = MaskCombine::Union;
    }
    for (const json &id : get_array(sj, "layers")) {
      if (id.is_number_unsigned() || id.is_number_integer()) {
        m.layers.push_back(id.get<uint64_t>());
      }
    }
  };
  for (const json &mj : get_array(j, "masks")) {
    if (!mj.is_object()) {
      continue;
    }
    MaskCuts cuts = MaskCuts::All;
    /* Lines Only came before Cuts. */
    if (!mask_cuts_from_identifier(get_string(mj, "cuts", get_bool(mj, "lines_only", false) ? "LINES" : "ALL"), &cuts)) {
      cuts = MaskCuts::All;
    }
    if (mj.contains("steps")) {
      LayerMask card;
      card.name = get_string(mj, "name");
      card.start_shown = get_string(mj, "start", "EVERYTHING") == "EVERYTHING";
      card.enabled = get_bool(mj, "enabled", true);
      card.render = get_bool(mj, "render", true);
      card.cuts = cuts;
      card.end_lines = get_bool(mj, "end_lines", false);
      card.end_lines_flat = get_bool(mj, "end_lines_flat", false);
      card.line_reach = std::clamp(get_float(mj, "line_reach", 0.0f), -2.0f, 2.0f);
      get_keys(mj, "line_reach_keys", card.line_reach_keys);
      card.show_expanded = get_bool(mj, "show_expanded", true);
      for (const json &sj : get_array(mj, "steps")) {
        if (!sj.is_object()) {
          continue;
        }
        MaskStep step;
        if (!mask_mode_from_identifier(get_string(sj, "mode", "ADD"), &step.mode)) {
          step.mode = MaskMode::Add;
        }
        step.invert = get_bool(sj, "invert", false);
        step.show_expanded = get_bool(sj, "show_expanded", false);
        read_step(read_step, sj, step);
        card.steps.push_back(std::move(step));
      }
      layer.masks.push_back(std::move(card));
      continue;
    }
    /* A row from before cards: its step and card settings, made a card once every row is in. */
    OldMaskRows &old = g_old_rows[layer.id];
    OldMaskRow row;
    row.step.invert = get_bool(mj, "invert", false);
    /* Older modes (Intersect, Difference) and files from before mask rows (Inside/Outside masks that
     * all had to hold), as rows that say so. */
    if (!mask_step_from_old_mode(get_string(mj, "mode", ""), get_bool(mj, "outside", false), old.rows.empty(),
                                 &row.step)) {
      row.step.mode = MaskMode::Add;
    }
    read_step(read_step, mj, row.step);
    row.enabled = get_bool(mj, "enabled", true);
    row.render = get_bool(mj, "render", true);
    row.cuts = cuts;
    row.end_lines = get_bool(mj, "end_lines", false);
    row.end_lines_flat = get_bool(mj, "end_lines_flat", false);
    row.line_reach = std::clamp(get_float(mj, "line_reach", 0.0f), -2.0f, 2.0f);
    get_keys(mj, "line_reach_keys", row.line_reach_keys);
    row.show_expanded = get_bool(mj, "show_expanded", true);
    old.rows.push_back(std::move(row));
  }
  if (const std::string start = get_string(j, "mask_start"); !start.empty()) {
    OldMaskRows &old = g_old_rows[layer.id];
    old.start_saved = true;
    old.start = start == "EVERYTHING";
  }
  /* Files from before masks named layers: Moho's masking group modes, turned into masks below. */
  if (j.contains("mask_mode") || j.contains("group_mask")) {
    g_old_masks[layer.id] = {get_string(j, "mask_mode", "MASKED"), get_string(j, "group_mask", "NONE")};
  }
  /* Files from before the two options were each mask's: the mask layer's, given to the masks
   * naming it (below). */
  if (get_bool(j, "mask_expand", false) || get_bool(j, "mask_exclude_strokes", false)) {
    g_old_mask_options[layer.id] = {get_bool(j, "mask_expand", false), get_bool(j, "mask_exclude_strokes", false)};
  }
  layer.reference_source = get_id(j, "reference_source");
  if (auto it = j.find("reference"); it != j.end() && layer.reference_source != 0) {
    layer.reference = reference_from_json(*it);
  }
  if (auto it = j.find("drawing"); it != j.end() && layer.has_shapes()) {
    if (auto ready = g_ready_drawings.find(&*it); ready != g_ready_drawings.end()) {
      layer.drawing = std::move(ready->second);
    }
    else {
      layer.drawing = drawing_from_json(*it);
    }
  }
  if (auto it = j.find("text"); it != j.end() && it->is_object() && layer.is_text()) {
    layer.text = text_from_json(*it);
    /* Its letters as saved stand (they show without the font); none saved (a converter's file), they
     * are made when it opens. */
    if (!layer.drawing.shapes.empty()) {
      layer.text_built = layer.text;
    }
  }
  if (auto it = j.find("media"); it != j.end() && it->is_object() && layer.is_media()) {
    layer.media = media_from_json(*it);
    /* Files from before a Media layer's Guide was its Show in Renders. */
    if (get_bool(*it, "guide", false)) {
      layer.render_visible = false;
    }
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
  /* A reference must lead to a real drawing: a reference of a reference follows the chain to the
   * original (references never chain), and a loop of references breaks. */
  layers_visit(doc.layers, [&](Layer &layer) {
    std::set<uint64_t> visited{layer.id};
    uint64_t original = layer.reference_source;
    for (const Layer *source = layer_find(doc, original); source && source->is_reference();
         source = layer_find(doc, original))
    {
      if (!visited.insert(source->id).second) {
        original = 0;
        break;
      }
      original = source->reference_source;
    }
    if (original == layer.id) {
      original = 0;
    }
    layer.reference_source = original;
  });
  doc.next_layer_id = std::max(doc.next_layer_id, max_id + 1);
  if (!layer_find(doc, doc.active_layer)) {
    doc.active_layer = doc.layers.empty() ? 0 : doc.layers.front().id;
  }
}

/* Format 1 kept bones' own position and angle in their keys, as Moho does (the Moho converter
 * still writes those): the setup pose is frame 0's key (the mainline's, while an action was being
 * edited), and every key, in every timeline, becomes the change from it. */
template<typename T> static void keys_to_changes(Channel<T> &ch, const std::string &action, T *r_setup)
{
  const std::vector<Key<T>> *mainline = &ch.keys;
  if (!action.empty()) {
    const auto it = ch.actions.find("");
    mainline = it != ch.actions.end() ? &it->second : nullptr;
  }
  if (mainline && !mainline->empty()) {
    Channel<T> c;
    c.keys = *mainline;
    *r_setup = c.evaluate(0);
  }
  for (Key<T> &k : ch.keys) {
    k.value -= *r_setup;
  }
  for (auto &[name, keys] : ch.actions) {
    for (Key<T> &k : keys) {
      k.value -= *r_setup;
    }
  }
}

static void migrate_bone_keys(std::vector<Layer> &layers, const std::string &action)
{
  for (Layer &layer : layers) {
    for (Bone &b : layer.bones) {
      b.setup_position = b.position;
      b.setup_angle = b.angle;
      keys_to_changes(b.angle_keys, action, &b.setup_angle);
      keys_to_changes(b.position_keys, action, &b.setup_position);
      /* A Transform is measured along the bone's rest axes. */
      const auto along_rest = [&](std::vector<Key<float2>> &keys) {
        for (Key<float2> &k : keys) {
          k.value = rotate(k.value, -b.setup_angle);
        }
      };
      along_rest(b.position_keys.keys);
      for (auto &[name, keys] : b.position_keys.actions) {
        along_rest(keys);
      }
    }
    migrate_bone_keys(layer.children, action);
  }
}

/* Parsing a big document: its drawings are most of it, so they are found by one quick scan (keeping to what
 * is inside strings and what isn't), parsed on every core, and put back where they were in the rest, parsed
 * with each of them a null. A big drawing's points, curves and shapes go in pieces of a few hundred
 * kilobytes, so one huge drawing doesn't keep the others waiting. Small documents, or text the scan can't
 * follow, are parsed whole. */
static json parse_document(const std::string &text)
{
  /* A drawing's list (points, curves, shapes): where it is and where each of its items starts. */
  struct List {
    std::string key;
    size_t start = 0, end = 0;
    std::vector<size_t> commas;
  };
  struct Found {
    size_t start = 0, end = 0;
    std::string pointer;
    std::vector<List> lists;
  };
  std::vector<Found> drawings;
  if (text.size() > (size_t(4) << 20)) {
    struct Level {
      bool object = false;
      std::string key;
      int index = 0;
      /* A drawing being found (its place in `drawings`), or one of its lists (`list`), -1 otherwise. */
      int drawing = -1;
      int list = -1;
    };
    std::vector<Level> stack;
    std::string last_string;
    bool ok = true;
    bool inside = false;
    for (size_t i = 0; i < text.size() && ok; i++) {
      const char c = text[i];
      if (c == '"') {
        const size_t from = i + 1;
        for (i = from; i < text.size() && text[i] != '"'; i++) {
          i += text[i] == '\\' ? 1 : 0;
        }
        last_string.assign(text, from, i - from);
        continue;
      }
      if (c == '{' || c == '[') {
        Level level;
        level.object = c == '{';
        if (level.object && !inside && !stack.empty() && stack.back().object && stack.back().key == "drawing") {
          std::string pointer;
          for (const Level &l : stack) {
            pointer += "/" + (l.object ? l.key : std::to_string(l.index));
          }
          level.drawing = int(drawings.size());
          drawings.push_back({i, 0, pointer, {}});
          inside = true;
        }
        else if (!level.object && !stack.empty() && stack.back().drawing >= 0) {
          const std::string &key = stack.back().key;
          if (key == "points" || key == "curves" || key == "shapes") {
            Found &d = drawings[size_t(stack.back().drawing)];
            level.list = int(d.lists.size());
            level.drawing = -1;
            d.lists.push_back({key, i, 0, {}});
          }
        }
        stack.push_back(std::move(level));
      }
      else if (c == '}' || c == ']') {
        if (stack.empty()) {
          ok = false;
          break;
        }
        const Level &top = stack.back();
        if (top.drawing >= 0) {
          drawings[size_t(top.drawing)].end = i + 1;
          inside = false;
        }
        if (top.list >= 0) {
          drawings.back().lists[size_t(top.list)].end = i + 1;
        }
        stack.pop_back();
      }
      else if (c == ':' && !stack.empty() && stack.back().object) {
        stack.back().key = last_string;
      }
      else if (c == ',' && !stack.empty()) {
        Level &top = stack.back();
        if (top.object) {
          top.key.clear();
        }
        else {
          top.index++;
          if (top.list >= 0) {
            drawings.back().lists[size_t(top.list)].commas.push_back(i);
          }
        }
      }
    }
    if (!ok || !stack.empty() || drawings.size() < 8 ||
        std::any_of(drawings.begin(), drawings.end(), [](const Found &d) { return d.end == 0; }))
    {
      drawings.clear();
    }
  }
  if (drawings.empty()) {
    return json::parse(text, nullptr, false);
  }

  /* The jobs: a small drawing whole; a big one's rest (its lists empty) and its lists in pieces. */
  constexpr size_t BIG = size_t(1) << 20, PIECE = size_t(256) << 10;
  struct Job {
    std::string text;
    size_t start = 0, end = 0;
  };
  struct Piece {
    size_t drawing = 0, list = 0, job = 0;
  };
  std::vector<Job> jobs;
  std::vector<size_t> drawing_job(drawings.size());
  std::vector<Piece> pieces;
  for (size_t k = 0; k < drawings.size(); k++) {
    const Found &d = drawings[k];
    if (d.end - d.start < BIG || d.lists.empty()) {
      drawing_job[k] = jobs.size();
      jobs.push_back({{}, d.start, d.end});
      continue;
    }
    std::string rest;
    size_t at = d.start;
    for (const List &l : d.lists) {
      rest.append(text, at, l.start - at);
      rest += "[]";
      at = l.end;
    }
    rest.append(text, at, d.end - at);
    drawing_job[k] = jobs.size();
    jobs.push_back({std::move(rest), 0, 0});
    for (size_t li = 0; li < d.lists.size(); li++) {
      const List &l = d.lists[li];
      /* Items run from just after '[' (or a comma) to the next comma kept (or ']'). */
      size_t from = l.start + 1;
      size_t ci = 0;
      while (from < l.end - 1) {
        size_t to = l.end - 1;
        while (ci < l.commas.size() && l.commas[ci] < from) {
          ci++;
        }
        size_t cj = ci;
        while (cj < l.commas.size() && l.commas[cj] - from < PIECE) {
          cj++;
        }
        if (cj < l.commas.size()) {
          to = l.commas[cj];
        }
        pieces.push_back({k, li, jobs.size()});
        jobs.push_back({"[" + text.substr(from, to - from) + "]", 0, 0});
        from = to + 1;
        ci = cj + 1;
      }
    }
  }
  /* The rest of the document, each drawing a null. */
  std::string rest;
  rest.reserve(text.size() / 8);
  size_t at = 0;
  for (const Found &d : drawings) {
    rest.append(text, at, d.start - at);
    rest += "null";
    at = d.end;
  }
  rest.append(text, at, std::string::npos);

  std::vector<json> parsed(jobs.size());
  std::atomic<size_t> next = 0;
  const auto work = [&]() {
    for (size_t k = next++; k < jobs.size(); k = next++) {
      const Job &job = jobs[k];
      parsed[k] = job.text.empty() ? json::parse(text.begin() + std::ptrdiff_t(job.start),
                                                 text.begin() + std::ptrdiff_t(job.end), nullptr, false)
                                   : json::parse(job.text, nullptr, false);
    }
  };
  std::vector<std::thread> threads;
  const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
  for (unsigned t = 1; t < count; t++) {
    threads.emplace_back(work);
  }
  json root = json::parse(rest, nullptr, false);
  work();
  for (std::thread &t : threads) {
    t.join();
  }
  if (root.is_discarded() ||
      std::any_of(parsed.begin(), parsed.end(), [](const json &j) { return j.is_discarded(); }))
  {
    return json::parse(text, nullptr, false);
  }
  /* The pieces back into their lists, in order. */
  for (const Piece &piece : pieces) {
    json &drawing = parsed[drawing_job[piece.drawing]];
    json &list = drawing[drawings[piece.drawing].lists[piece.list].key];
    for (json &item : parsed[piece.job]) {
      list.push_back(std::move(item));
    }
  }
  for (size_t k = 0; k < drawings.size(); k++) {
    const json::json_pointer pointer(drawings[k].pointer);
    if (!root.contains(pointer) || !root[pointer].is_null()) {
      return json::parse(text, nullptr, false);
    }
    root[pointer] = std::move(parsed[drawing_job[k]]);
  }
  return root;
}

bool oanim_document_from_json(const std::string &text, Document &r_doc, std::string *r_error)
{
  const json root = parse_document(text);
  if (root.is_discarded() || !root.is_object()) {
    *r_error = "The document is damaged (not valid JSON)";
    return false;
  }
  if (const std::string format = get_string(root, "format"); format != FORMAT_NAME && format != LEGACY_FORMAT_NAME) {
    *r_error = "Not an Open Animate file";
    return false;
  }
  const int version = get_int(root, "version", 0);
  g_loading_version = version;
  /* Keys read after this file (pasted, another file) go by their own format, however it ends. */
  struct VersionReset {
    ~VersionReset() { g_loading_version = OANIM_FILE_VERSION; }
  } version_reset;
  if (version < 1 || version > OANIM_FILE_VERSION) {
    *r_error = "The file was saved by a newer Open Animate (format " + std::to_string(version) +
               "); this version reads up to format " + std::to_string(OANIM_FILE_VERSION);
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
  if (const int mode = Document::rna().find("auto_key_mode")->enum_index(get_string(j, "auto_key_mode", "")); mode >= 0) {
    doc.auto_key_mode = AutoKeyMode(mode);
  }
  doc.auto_freeze_keys = get_bool(j, "auto_freeze_keys", false);
  doc.time_together = get_bool(j, "time_together", false);
  doc.timeline_only_selected = get_bool(j, "timeline_only_selected", true);
  doc.timeline_lock = get_bool(j, "timeline_lock", false);
  doc.timeline_show = std::clamp(get_int(j, "timeline_show", 0), 0, 2);
  doc.timeline_kinds = uint32_t(get_int(j, "timeline_kinds", int(TIMELINE_KIND_ALL))) & TIMELINE_KIND_ALL;
  doc.timeline_search = get_string(j, "timeline_search", "");
  doc.timeline_locked.clear();
  if (const auto it = j.find("timeline_locked"); it != j.end() && it->is_array()) {
    for (const auto &id : *it) {
      if (id.is_number_unsigned()) {
        doc.timeline_locked.push_back(id.get<uint64_t>());
      }
    }
  }
  doc.onion_skin = get_bool(j, "onion_skin", false);
  if (const int mode = Document::rna().find("onion_mode")->enum_index(get_string(j, "onion_mode", "")); mode >= 0) {
    doc.onion_mode = OnionMode(mode);
  }
  doc.onion_before = std::clamp(get_int(j, "onion_before", 2), 0, 20);
  doc.onion_after = std::clamp(get_int(j, "onion_after", 0), 0, 20);
  doc.onion_step = std::clamp(get_int(j, "onion_step", 1), 1, 100);
  doc.onion_fade = get_bool(j, "onion_fade", true);
  doc.onion_all_layers = get_bool(j, "onion_all_layers", false);
  doc.onion_color_before = get_color(j, "onion_color_before", doc.onion_color_before);
  doc.onion_color_after = get_color(j, "onion_color_after", doc.onion_color_after);
  doc.motion_paths = get_bool(j, "motion_paths", false);
  doc.path_before = std::clamp(get_int(j, "path_before", 1), 0, 20);
  doc.path_after = std::clamp(get_int(j, "path_after", 1), 0, 20);
  doc.path_all = get_bool(j, "path_all", false);
  doc.path_step = std::clamp(get_int(j, "path_step", 1), 1, 100);
  doc.path_lines = get_bool(j, "path_lines", true);
  doc.path_own_motion = get_bool(j, "path_own_motion", true);
  /* Files from before Dot Colour had a Speed Colours switch: off was the path's colour. */
  if (const int dots = Document::rna().find("path_dot_colors")->enum_index(get_string(j, "path_dot_colors", ""));
      dots >= 0)
  {
    doc.path_dot_colors = PathDotColors(dots);
  }
  else {
    doc.path_dot_colors = get_bool(j, "path_speed_colors", true) ? PathDotColors::TimingCurrent : PathDotColors::Path;
  }
  if (const int rel = Document::rna().find("path_relative")->enum_index(get_string(j, "path_relative", "")); rel >= 0) {
    doc.path_relative = PathRelative(rel);
  }
  doc.path_relative_layer = get_id(j, "path_relative_layer");
  doc.path_relative_bone = get_id(j, "path_relative_bone");
  doc.path_targets.clear();
  for (const json &tj : get_array(j, "path_targets")) {
    MotionPathTarget t;
    t.layer = get_id(tj, "layer");
    t.bone = get_id(tj, "bone");
    t.point = get_id(tj, "point");
    t.head = get_bool(tj, "head", false);
    t.pinned = get_bool(tj, "pinned", false);
    t.has_color = tj.contains("color");
    t.color = get_color(tj, "color", t.color);
    doc.path_targets.push_back(t);
  }
  if (const int sync = Document::rna().find("sync_mode")->enum_index(get_string(j, "sync_mode", "")); sync >= 0) {
    doc.sync = PlaybackSync(sync);
  }
  doc.play_audio = get_bool(j, "play_audio", doc.play_audio);
  doc.audio_scrub = get_bool(j, "use_audio_scrub", doc.audio_scrub);
  doc.fps =std::clamp(get_float(j, "fps", doc.fps), 1.0f, 1000.0f);
  doc.width = std::clamp(get_int(j, "width", doc.width), 1, 65536);
  doc.height = std::clamp(get_int(j, "height", doc.height), 1, 65536);
  doc.resolution_linked = get_bool(j, "resolution_linked", false);
  doc.resolution_aspect = get_float(j, "resolution_aspect", float(doc.width) / float(std::max(doc.height, 1)));
  if (!(doc.resolution_aspect > 0.0f)) {
    doc.resolution_aspect = float(doc.width) / float(std::max(doc.height, 1));
  }
  doc.background = get_color(j, "background", doc.background);
  doc.viewport_quality = std::clamp(get_int(j, "viewport_quality", doc.viewport_quality), 1, 64);
  doc.render_quality = std::clamp(get_int(j, "render_quality", doc.render_quality), 1, 64);
  doc.viewport_samples = std::clamp(get_int(j, "viewport_samples", doc.viewport_samples), 1, 16);
  doc.render_samples = std::clamp(get_int(j, "render_samples", doc.render_samples), 1, 16);
  overlays_from_json(j.value("viewport_overlays", json::object()), doc.overlays);
  if (const auto it = j.find("viewport_shading"); it != j.end() && it->is_object()) {
    doc.shading.layer_caching = get_bool(*it, "layer_caching", true);
    doc.shading.zoom_preview = get_bool(*it, "zoom_preview", false);
    /* GPU Geometry unless the file says Classic. (Files saved while Classic was the default have "gpu_geometry":
     * false, the default then rather than a choice; it's left unread.) */
    doc.shading.gpu_geometry = get_string(*it, "renderer", "GPU_GEOMETRY") != "CLASSIC";
  }
  if (const auto it = j.find("mirror_editing"); it != j.end() && it->is_object()) {
    doc.mirror_x = get_bool(*it, "x", false);
    doc.mirror_y = get_bool(*it, "y", false);
  }
  if (const auto it = j.find("viewport_view"); it != j.end() && it->is_object()) {
    const float2 center = get_float2(*it, "center", {});
    doc.view.center_x = center.x;
    doc.view.center_y = center.y;
    doc.view.zoom = std::max(0.0f, get_float(*it, "zoom", 0.0f));
  }
  /* The viewports take the file's view (or frame the camera, without one). */
  static uint64_t views_loaded = 0;
  doc.view_serial = ++views_loaded;
  if (const auto it = j.find("ui_open"); it != j.end() && it->is_object()) {
    for (const auto &[key, open] : it->items()) {
      if (open.is_boolean()) {
        doc.ui_open[key] = open.get<bool>();
      }
    }
  }
  if (const auto it = j.find("ui_panel_order"); it != j.end() && it->is_object()) {
    for (const auto &[key, order] : it->items()) {
      if (!order.is_array()) {
        continue;
      }
      std::vector<std::string> &names = doc.ui_panel_order[key];
      for (const auto &name : order) {
        if (name.is_string()) {
          names.push_back(name.get<std::string>());
        }
      }
    }
  }
  doc.action = get_string(j, "action");
  doc.gravity = get_float(j, "gravity", 0.0f);
  doc.wind = get_float(j, "wind", 0.0f);
  doc.wind_direction = get_float(j, "wind_direction", 0.0f);
  doc.wind_gusts = std::clamp(get_float(j, "wind_gusts", 0.0f), 0.0f, 2.0f);
  doc.wind_gust_speed = std::clamp(get_float(j, "wind_gust_speed", 1.0f), 0.05f, 10.0f);
  doc.wind_turbulence = std::clamp(get_float(j, "wind_turbulence", 0.0f), 0.0f, 2.0f);
  doc.simulation_start = std::max(1, get_int(j, "simulation_start", 1));
  if (auto it = j.find("simulation_keys"); it != j.end() && it->is_object()) {
    for (CustomChannel &c : document_simulation(doc).custom_channels) {
      get_keys(*it, c.path.c_str(), c.keys);
    }
  }
  /* Files from before the camera get one where the page was: at the origin, zoom 1. */
  Layer &camera = document_camera(doc);
  if (auto it = j.find("camera"); it != j.end() && it->is_object()) {
    const json &cj = *it;
    camera.position = get_float2(cj, "position", {});
    camera.rotation = get_float(cj, "roll", 0.0f);
    camera_set_zoom(doc, get_float(cj, "zoom", 1.0f));
    camera.selected = get_bool(cj, "selected", false);
    doc.camera_passepartout = std::clamp(get_float(cj, "passepartout", 0.6f), 0.0f, 1.0f);
    if (auto keys = cj.find("keys"); keys != cj.end() && keys->is_object()) {
      get_keys(*keys, "position", camera.position_keys);
      get_keys(*keys, "rotation", camera.rotation_keys);
      for (CustomChannel &c : camera.custom_channels) {
        get_keys(*keys, c.path.c_str(), c.keys);
      }
    }
  }
  doc.action_layer = get_id(j, "action_layer");
  doc.active_layer = get_id(j, "active_layer");
  g_loading_action = doc.action;
  doc.next_layer_id = std::max<uint64_t>(1, get_id(j, "next_layer_id"));
  g_old_masks.clear();
  g_old_mask_options.clear();
  g_old_rows.clear();
  /* Drawings are most of the work: made on every core first. */
  {
    std::vector<const json *> found;
    const auto collect = [&](auto &&self, const json &layers) -> void {
      if (!layers.is_array()) {
        return;
      }
      for (const json &l : layers) {
        if (!l.is_object()) {
          continue;
        }
        if (auto it = l.find("drawing"); it != l.end() && it->is_object()) {
          found.push_back(&*it);
        }
        if (auto it = l.find("children"); it != l.end()) {
          self(self, *it);
        }
      }
    };
    if (auto it = j.find("layers"); it != j.end()) {
      collect(collect, *it);
    }
    if (found.size() >= 8) {
      std::vector<VectorDrawing> made(found.size());
      std::atomic<size_t> next = 0;
      const auto work = [&]() {
        for (size_t k = next++; k < found.size(); k = next++) {
          made[k] = drawing_from_json(*found[k]);
        }
      };
      std::vector<std::thread> threads;
      const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
      for (unsigned t = 1; t < count; t++) {
        threads.emplace_back(work);
      }
      work();
      for (std::thread &t : threads) {
        t.join();
      }
      for (size_t k = 0; k < found.size(); k++) {
        g_ready_drawings.emplace(found[k], std::move(made[k]));
      }
    }
  }
  for (const json &layer : get_array(j, "layers")) {
    if (layer.is_object()) {
      doc.layers.push_back(layer_from_json(layer, 0));
    }
  }
  migrate_old_masks(doc.layers);
  migrate_old_mask_options(doc.layers);
  mask_rows_to_cards(doc.layers);
  g_old_rows.clear();
  if (version < 2) {
    migrate_bone_keys(doc.layers, doc.action);
  }
  g_old_masks.clear();
  if (get_string(j, "mask_sources") != "shapes") {
    report_mask_sources_changed(doc);
  }
  g_ready_drawings.clear();
  for (const json &t : get_array(j, "texts")) {
    if (t.is_object()) {
      doc.texts.push_back({get_string(t, "name", "Text"), get_string(t, "body"), get_string(t, "filepath")});
    }
  }
  for (const json &lj : get_array(j, "switch_links")) {
    if (!lj.is_object()) {
      continue;
    }
    SwitchLink l;
    l.id = lj.value("id", uint64_t(0));
    l.lead = lj.value("lead", uint64_t(0));
    l.next_name = lj.value("next_name", uint64_t(1));
    for (const json &nj : get_array(lj, "names")) {
      if (nj.is_array() && nj.size() == 2 && nj[0].is_number_unsigned() && nj[1].is_string()) {
        l.names.push_back({nj[0].get<uint64_t>(), nj[1].get<std::string>()});
      }
    }
    if (l.id) {
      doc.switch_links.push_back(std::move(l));
    }
  }
  doc.next_switch_link = std::max<uint64_t>(j.value("next_switch_link", uint64_t(1)), 1);
  for (const SwitchLink &l : doc.switch_links) {
    doc.next_switch_link = std::max(doc.next_switch_link, l.id + 1);
  }
  for (const json &dj : get_array(j, "drivers")) {
    if (!dj.is_object()) {
      continue;
    }
    const auto ref_from = [](const json &r) {
      DriverRef ref;
      if (r.is_object()) {
        ref.layer = get_id(r, "layer");
        ref.path = get_string(r, "path");
        ref.component = get_int(r, "component", -1);
      }
      return ref;
    };
    Driver d;
    d.target = ref_from(dj.value("target", json::object()));
    const std::string type = get_string(dj, "type", "EXPRESSION");
    d.type = type == "AVERAGE" ? DriverType::Average : type == "SUM" ? DriverType::Sum
             : type == "MIN"   ? DriverType::Min   : type == "MAX" ? DriverType::Max
                                                                     : DriverType::Expression;
    d.expression = get_string(dj, "expression", "var");
    for (const json &vj : get_array(dj, "vars")) {
      if (vj.is_object()) {
        d.vars.push_back({get_string(vj, "name", "var"), ref_from(vj.value("source", json::object()))});
      }
    }
    if (auto it = dj.find("remap"); it != dj.end() && it->is_object()) {
      d.remap = true;
      d.from_min = get_float(*it, "from_min", 0.0f);
      d.from_max = get_float(*it, "from_max", 1.0f);
      d.to_min = get_float(*it, "to_min", 0.0f);
      d.to_max = get_float(*it, "to_max", 1.0f);
      d.clamp = get_bool(*it, "clamp", true);
    }
    if (auto it = dj.find("rest"); it != dj.end() && it->is_number()) {
      d.target_rest = it->get<float>();
      d.rest_captured = true;
    }
    if (auto it = dj.find("two_way"); it != dj.end() && it->is_object()) {
      d.two_way = true;
      d.factor = get_float(*it, "factor", 1.0f);
      d.flip = get_bool(*it, "flip", false);
      d.source_rest = get_float(*it, "source_rest", 0.0f);
      d.target_rest = get_float(*it, "target_rest", 0.0f);
    }
    if (!d.target.path.empty()) {
      doc.drivers.push_back(std::move(d));
    }
  }
  for (const json &pj : get_array(j, "brush_presets")) {
    if (!pj.is_object()) {
      continue;
    }
    BrushPreset p;
    p.name = get_string(pj, "name", "Preset");
    if (auto it = pj.find("brush"); it != pj.end() && it->is_object()) {
      p.settings = brush_from_json(*it);
    }
    for (const json &f : get_array(pj, "shape_files")) {
      if (f.is_string()) {
        p.shape_files.push_back(f.get<std::string>());
      }
    }
    doc.brush_presets.push_back(std::move(p));
  }
  for (const json &mj : get_array(j, "materials")) {
    if (!mj.is_object()) {
      continue;
    }
    Material m;
    m.id = get_id(mj, "id");
    m.name = get_string(mj, "name", "Material");
    if (auto it = mj.find("style"); it != mj.end()) {
      m.style = style_from_json(*it);
    }
    if (m.id != 0 && !material_find(doc, m.id)) {
      doc.next_material_id = std::max(doc.next_material_id, m.id + 1);
      doc.materials.push_back(std::move(m));
    }
  }
  for (const json &aj : get_array(j, "annotations")) {
    if (!aj.is_object()) {
      continue;
    }
    AnnotationLayer l;
    l.name = get_string(aj, "name", "Note");
    l.color = get_color(aj, "color", l.color);
    l.thickness = std::clamp(get_float(aj, "thickness", 3.0f), ANNOTATION_THICKNESS_MIN, ANNOTATION_THICKNESS_MAX);
    l.hidden = get_bool(aj, "hidden", false);
    l.locked = get_bool(aj, "locked", false);
    for (const json &sj : get_array(aj, "strokes")) {
      if (!sj.is_object()) {
        continue;
      }
      AnnotationStroke s;
      s.points = points_from_json(sj, "points");
      if (!annotation_end_from_identifier(get_string(sj, "start", "NONE"), &s.start)) {
        s.start = AnnotationEnd::None;
      }
      if (!annotation_end_from_identifier(get_string(sj, "end", "NONE"), &s.end)) {
        s.end = AnnotationEnd::None;
      }
      if (s.points.size() >= 2) {
        l.strokes.push_back(std::move(s));
      }
    }
    doc.annotations.push_back(std::move(l));
  }
  doc.active_annotation = std::clamp(get_int(j, "active_annotation", 0), 0, std::max(0, int(doc.annotations.size()) - 1));
  for (const json &gj : get_array(j, "guides")) {
    if (!gj.is_object()) {
      continue;
    }
    DrawingGuide g;
    if (!guide_type_from_identifier(get_string(gj, "type", "LINE"), &g.type)) {
      continue;
    }
    g.points = points_from_json(gj, "points");
    g.lines = get_int(gj, "lines", 24);
    g.hidden = get_bool(gj, "hidden", false);
    g.locked = get_bool(gj, "locked", false);
    guide_validate(g);
    doc.guides.push_back(std::move(g));
  }
  doc.active_guide = std::clamp(get_int(j, "active_guide", -1), -1, int(doc.guides.size()) - 1);
  if (const auto it = j.find("guide_settings"); it != j.end() && it->is_object()) {
    doc.guide_settings.show = get_bool(*it, "show", true);
    doc.guide_settings.hue = std::clamp(get_int(*it, "hue", 0), 0, 360);
    doc.guide_settings.opacity = std::clamp(get_int(*it, "opacity", 100), 0, 100);
  }
  layers_validate(doc);
  /* Older files' flexi-binding becomes the same weights, kept. */
  document_weight_new_points(doc, true);
  /* Older files' Vector Deforms become the deform their shape fits. */
  vector_deform_resolve_legacy(doc);

  r_doc = std::move(doc);
  return true;
}

bool oanim_file_read(const std::string &path, Document &r_doc, std::string *r_error)
{
  std::ifstream in(path_from_utf8(path), std::ios::binary);
  if (!in) {
    *r_error = "Could not open " + path;
    return false;
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
    *r_error = "Not an Open Animate file (not a zip archive): " + path;
    return false;
  }
  size_t size = 0;
  void *data = mz_zip_reader_extract_file_to_heap(&zip, DOCUMENT_ENTRY, &size, 0);
  /* Packed media: media/<id>/<name>. */
  const std::string prefix = MEDIA_ENTRY_PREFIX;
  for (mz_uint i = 0; data && i < mz_zip_reader_get_num_files(&zip); i++) {
    char name[1024];
    mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
    const std::string entry = name;
    const size_t slash = entry.find('/', prefix.size());
    if (entry.compare(0, prefix.size(), prefix) != 0 || slash == std::string::npos || slash + 1 >= entry.size()) {
      continue;
    }
    const std::string id = entry.substr(prefix.size(), slash - prefix.size());
    if (media_packed_has(id)) {
      continue;
    }
    size_t bytes_size = 0;
    if (void *packed = mz_zip_reader_extract_to_heap(&zip, i, &bytes_size, 0)) {
      media_packed_add(id, entry.substr(slash + 1), std::string(static_cast<const char *>(packed), bytes_size));
      mz_free(packed);
    }
  }
  /* File presets' shape images: brush_shapes/<preset>/<file>. */
  std::map<std::pair<size_t, std::string>, std::string> shape_bytes;
  for (mz_uint i = 0; data && i < mz_zip_reader_get_num_files(&zip); i++) {
    char name[1024];
    mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
    const std::string entry = name;
    static const std::string shapes_prefix = "brush_shapes/";
    const size_t slash = entry.find('/', shapes_prefix.size());
    if (entry.compare(0, shapes_prefix.size(), shapes_prefix) != 0 || slash == std::string::npos) {
      continue;
    }
    size_t bytes_size = 0;
    if (void *packed = mz_zip_reader_extract_to_heap(&zip, i, &bytes_size, 0)) {
      const size_t index = size_t(std::strtoul(entry.substr(shapes_prefix.size(), slash - shapes_prefix.size()).c_str(), nullptr, 10));
      shape_bytes[{index, entry.substr(slash + 1)}] = std::string(static_cast<const char *>(packed), bytes_size);
      mz_free(packed);
    }
  }
  mz_zip_reader_end(&zip);
  if (!data) {
    *r_error = std::string("The file has no ") + DOCUMENT_ENTRY + ": " + path;
    return false;
  }
  const std::string text(static_cast<const char *>(data), size);
  mz_free(data);
  g_file_folder = path_from_utf8(path).parent_path();
  const bool read = oanim_document_from_json(text, r_doc, r_error);
  g_file_folder.clear();
  if (read) {
    for (size_t p = 0; p < r_doc.brush_presets.size(); p++) {
      BrushPreset &preset = r_doc.brush_presets[p];
      std::vector<std::string> files;
      for (const std::string &f : preset.shape_files) {
        if (auto it = shape_bytes.find({p, f}); it != shape_bytes.end()) {
          files.push_back(f);
          preset.shape_data.push_back(it->second);
        }
      }
      preset.shape_files = files;
    }
    brush_presets_register_shapes(r_doc);
  }
  return read;
}

}  // namespace oanim
