#include "io/oanim_file.hh"

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
    if (k.selected) {
      key["selected"] = true;
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
}

static json drawing_to_json(const VectorDrawing &drawing)
{
  json points = json::array();
  for (const MeshPoint &p : drawing.points) {
    json point = {{"id", p.id}, {"pos", float2_to_json(p.pos)}};
    if (p.radius != 1.0f) {
      point["radius"] = num(p.radius);
    }
    if (p.corner == CornerType::Sharp) {
      point["corner"] = "SHARP";
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
    shapes.push_back({{"id", shape.id},
                      {"name", shape.name},
                      {"fill", shape.has_fill},
                      {"outline", shape.has_outline},
                      {"style", style_to_json(shape.style)},
                      {"edges", std::move(edges)}});
    if (shape.detail_level > 0) {
      shapes.back()["detail_level"] = shape.detail_level;
    }
  }
  return {{"points", std::move(points)},
          {"curves", std::move(curves)},
          {"shapes", std::move(shapes)},
          {"next_id", drawing.next_id}};
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
static std::filesystem::path g_file_folder;

static const char *const MEDIA_FIT_IDS[] = {"FIT", "FILL", "STRETCH", "ORIGINAL"};
static const char *const MEDIA_TIMING_IDS[] = {"REAL_TIME", "FRAME_BY_FRAME"};
static const char *const MEDIA_END_IDS[] = {"HOLD", "LOOP", "HIDE"};

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
  return {{"guide", m.guide},
          {"fit", MEDIA_FIT_IDS[int(m.fit)]},
          {"greyscale", m.greyscale},
          {"tint", color_to_json(m.tint)},
          {"tint_amount", num(m.tint_amount)},
          {"in_front", m.in_front},
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
      {"expanded", layer.expanded},
      {"selected", layer.selected},
      {"label_color", enum_identifier("label_color", int(layer.label_color))},
      {"position", float2_to_json(layer.position)},
      {"rotation", num(layer.rotation)},
      {"scale", float2_to_json(layer.scale)},
      {"opacity", num(layer.opacity)},
  };
  if (layer.parent_bone != 0) {
    j["parent_bone"] = layer.parent_bone;
  }
  if (!layer.masks.empty()) {
    json masks = json::array();
    for (const LayerMask &m : layer.masks) {
      masks.push_back({{"outside", m.outside},
                       {"layers", m.layers},
                       {"enabled", m.enabled},
                       {"render", m.render},
                       {"expand", m.expand},
                       {"fills_only", m.fills_only},
                       {"filled", m.filled},
                       {"extend", mask_extend_identifier(m.extend)},
                       {"show_expanded", m.show_expanded}});
    }
    j["masks"] = std::move(masks);
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
      if (b.selected || b.select_head || b.select_tail) {
        bj["selected"] = b.selected;
        bj["select_head"] = b.select_head;
        bj["select_tail"] = b.select_tail;
      }
      put_keys(bj, "position_keys", b.position_keys);
      put_keys(bj, "angle_keys", b.angle_keys);
      put_keys(bj, "scale_keys", b.scale_keys);
      if (b.ik_ignore) {
        bj["ik_ignore"] = true;
      }
      if (b.arc_ik) {
        bj["arc_ik"] = true;
      }
      if (b.shape != BoneShape::None) {
        bj["shape"] = b.shape == BoneShape::Pin ? "PIN" : "CUSTOM";
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
      if (!b.constraints.empty()) {
        json constraints = json::array();
        for (const BoneConstraint &c : b.constraints) {
          const auto spring = [](const BoneSpring &s) {
            json sj{{"enabled", s.enabled}, {"settle_speed", num(s.settle_speed)},
                    {"bounce", num(s.bounce)},        {"inertia", num(s.inertia)},
                    {"stretch", num(s.stretch)},
                    {"use_limit", s.use_limit},       {"limit", num(s.limit)}};
            put_keys(sj, "settle_speed_keys", s.settle_speed_keys);
            put_keys(sj, "bounce_keys", s.bounce_keys);
            put_keys(sj, "inertia_keys", s.inertia_keys);
            put_keys(sj, "stretch_keys", s.stretch_keys);
            put_keys(sj, "limit_keys", s.limit_keys);
            return sj;
          };
          json cj = {{"type", bone_constraint_identifier(c.type)},
                     {"name", c.name},
                     {"enabled", c.enabled},
                     {"expanded", c.expanded},
                     {"influence", num(c.influence)},
                     {"min_angle", num(c.min_angle)},
                     {"max_angle", num(c.max_angle)},
                     {"amount", num(c.amount)},
                     {"target", c.target},
                     {"factor", num(c.factor)},
                     {"delay", c.delay},
                     {"chain", c.chain},
                     {"max_stretch", num(c.max_stretch)},
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
  put_keys(j, "opacity_keys", layer.opacity_keys);
  put_keys(j, "visible_keys", layer.visible_keys);
  if (layer.draw_order != 0.0f) {
    j["draw_order"] = std::llround(layer.draw_order);
  }
  put_keys(j, "draw_order_keys", layer.draw_order_keys);
  if (layer.is_reference()) {
    /* A reference's drawing lives in its source; storing a copy would only go stale. */
    j["reference_source"] = layer.reference_source;
    j["reference"] = reference_to_json(layer.reference);
  }
  else if (layer.is_media()) {
    j["media"] = media_to_json(layer.media);
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

static json overlays_to_json(const ViewportOverlays &ov);

std::string oanim_document_to_json(const Document &doc)
{
  json texts = json::array();
  for (const Text &text : doc.texts) {
    texts.push_back({{"name", text.name}, {"body", text.body}, {"filepath", text.filepath}});
  }
  json layers = json::array();
  for (const Layer &layer : doc.layers) {
    layers.push_back(layer_to_json(layer));
  }
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
           {"sync_mode", Document::rna().find("sync_mode")->items[size_t(doc.sync)].identifier},
           {"play_audio", doc.play_audio},
           {"use_audio_scrub", doc.audio_scrub},
           {"fps", num(doc.fps)},
           {"width", doc.width},
           {"height", doc.height},
           {"background", color_to_json(doc.background)},
           {"viewport_quality", doc.viewport_quality},
           {"viewport_samples", doc.viewport_samples},
           {"render_samples", doc.render_samples},
           {"viewport_overlays", overlays_to_json(doc.overlays)},
           {"ui_open", doc.ui_open},
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

bool oanim_file_write(const Document &doc, const std::string &path, std::string *r_error)
{
  /* What smart actions add isn't data: it is worked out again on opening. */
  Document data = doc;
  animation_strip_smart(data);
  g_file_folder = path_from_utf8(path).parent_path();
  const std::string text = oanim_document_to_json(data);
  g_file_folder.clear();

  /* Build the archive in memory: miniz's file functions can't open non-ASCII Windows paths. */
  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
    *r_error = "Could not start the archive";
    return false;
  }
  void *buffer = nullptr;
  size_t size = 0;
  bool built = mz_zip_writer_add_mem(&zip, DOCUMENT_ENTRY, text.data(), text.size(), MZ_DEFAULT_LEVEL);
  /* Packed media beside it, as media/<id>/<name> (video and sound are compressed already: stored). */
  for (const std::string &id : media_packed_used(data)) {
    if (const std::string *bytes = media_packed_bytes(id); bytes && built) {
      const std::string entry = std::string(MEDIA_ENTRY_PREFIX) + id + "/" + media_packed_name(id);
      built = mz_zip_writer_add_mem(&zip, entry.c_str(), bytes->data(), bytes->size(), MZ_NO_COMPRESSION);
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
    if (!ease_from_identifier(get_string(k, "ease", "QUAD"), &key.ease)) {
      key.ease = Ease::Quad;
    }
    key.overshoot = get_float(k, "overshoot", key.overshoot);
    key.elastic_amplitude = get_float(k, "elastic_amplitude", key.elastic_amplitude);
    key.elastic_period = std::max(0.02f, get_float(k, "elastic_period", key.elastic_period));
    key.noise_amplitude = get_float(k, "noise_amplitude", key.noise_amplitude);
    key.noise_frequency = get_float(k, "noise_frequency", key.noise_frequency);
    key.cycle_back = std::max(0, get_int(k, "cycle_back", key.cycle_back));
    key.cycle_absolute = get_bool(k, "cycle_absolute", false);
    key.cycle_frame = std::max(0, get_int(k, "cycle_frame", 0));
    key.color = get_int(k, "color", 0);
    key.selected = get_bool(k, "selected", false);
    if (!r_ch.key_at(key.frame)) {
      r_ch.keys.push_back(key);
    }
  }
  std::sort(r_ch.keys.begin(), r_ch.keys.end(), [](const Key<T> &a, const Key<T> &b) { return a.frame < b.frame; });
}

template<typename T> static void get_keys(const json &j, const char *name, Channel<T> &r_ch)
{
  get_key_list(j, name, r_ch);
  if (auto it = j.find(std::string(name) + "_pinned"); it != j.end() && it->is_boolean()) {
    r_ch.pinned = it->get<bool>();
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

/* The file being read: its format version and the action that was being edited. */
static int g_loading_version = OANIM_FILE_VERSION;
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
      drawing.points.push_back({get_float2(p, "pos", {}), std::clamp(get_float(p, "radius", 1.0f), 0.0f, 1.0f),
                                get_bool(p, "selected", false),
                                get_string(p, "corner") == "SHARP" ? CornerType::Sharp : CornerType::Round});
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
    shape.detail_level = std::clamp(get_int(s, "detail_level", 0), 0, DETAIL_LEVEL_MAX);
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

/* The masks naming a layer that had the two options take them. */
static void migrate_old_mask_options(std::vector<Layer> &layers)
{
  for (Layer &layer : layers) {
    for (LayerMask &m : layer.masks) {
      for (const uint64_t id : m.layers) {
        if (const auto it = g_old_mask_options.find(id); it != g_old_mask_options.end()) {
          m.expand |= it->second.first;
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
      std::vector<LayerMask> masks;
      if (!full && !added.empty()) {
        masks.push_back({false, added, true});
      }
      if (!cut.empty()) {
        masks.push_back({true, cut, true});
      }
      child.masks.insert(child.masks.begin(), masks.begin(), masks.end());
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

static MediaSettings media_from_json(const json &j)
{
  MediaSettings m;
  m.guide = get_bool(j, "guide", m.guide);
  m.fit = MediaFit(id_index(MEDIA_FIT_IDS, get_string(j, "fit", ""), 0));
  m.greyscale = get_bool(j, "greyscale", false);
  m.tint = get_color(j, "tint", m.tint);
  m.tint_amount = std::clamp(get_float(j, "tint_amount", 0.0f), 0.0f, 1.0f);
  m.in_front = get_bool(j, "in_front", false);
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
  layer.selected = get_bool(j, "selected", false);
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
    b.selected = get_bool(bj, "selected", false);
    b.select_head = get_bool(bj, "select_head", false);
    b.select_tail = get_bool(bj, "select_tail", false);
    get_keys(bj, "position_keys", b.position_keys);
    get_keys(bj, "angle_keys", b.angle_keys);
    get_keys(bj, "scale_keys", b.scale_keys);
    b.ik_ignore = get_bool(bj, "ik_ignore", false);
    b.arc_ik = get_bool(bj, "arc_ik", false);
    const std::string shape = bj.value("shape", std::string());
    b.shape = shape == "PIN" ? BoneShape::Pin : shape == "CUSTOM" ? BoneShape::Custom : BoneShape::None;
    b.pin_size = std::clamp(get_float(bj, "pin_size", 1.0f), 0.1f, 20.0f);
    b.shape_layer = bj.value("shape_layer", uint64_t(0));
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
      };
      c.name = get_string(cj, "name", bone_constraint_name(c.type));
      c.enabled = get_bool(cj, "enabled", true);
      c.expanded = get_bool(cj, "expanded", true);
      c.influence = std::clamp(get_float(cj, "influence", 1.0f), 0.0f, 1.0f);
      c.min_angle = get_float(cj, "min_angle", c.min_angle);
      c.max_angle = get_float(cj, "max_angle", c.max_angle);
      c.amount = get_float(cj, "amount", c.amount);
      c.target = get_id(cj, "target");
      c.factor = get_float(cj, "factor", c.factor);
      c.delay = std::max(0, get_int(cj, "delay", 0));
      c.chain = std::max(0, get_int(cj, "chain", 0));
      c.max_stretch = std::max(1.0f, get_float(cj, "max_stretch", 1.0f));
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
  get_keys(j, "opacity_keys", layer.opacity_keys);
  get_keys(j, "visible_keys", layer.visible_keys);
  layer.draw_order = float(get_int(j, "draw_order", 0));
  get_keys(j, "draw_order_keys", layer.draw_order_keys);
  for (const json &mj : get_array(j, "masks")) {
    if (!mj.is_object()) {
      continue;
    }
    LayerMask m;
    m.outside = get_bool(mj, "outside", false);
    m.enabled = get_bool(mj, "enabled", true);
    m.render = get_bool(mj, "render", true);
    m.expand = get_bool(mj, "expand", false);
    m.fills_only = get_bool(mj, "fills_only", false);
    m.filled = get_bool(mj, "filled", false);
    if (!mask_extend_from_identifier(get_string(mj, "extend", "NONE"), &m.extend)) {
      m.extend = MaskExtend::None;
    }
    m.show_expanded = get_bool(mj, "show_expanded", true);
    for (const json &id : get_array(mj, "layers")) {
      if (id.is_number_unsigned() || id.is_number_integer()) {
        m.layers.push_back(id.get<uint64_t>());
      }
    }
    layer.masks.push_back(std::move(m));
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
  if (auto it = j.find("drawing"); it != j.end() && layer.has_drawing()) {
    layer.drawing = drawing_from_json(*it);
  }
  if (auto it = j.find("media"); it != j.end() && it->is_object() && layer.is_media()) {
    layer.media = media_from_json(*it);
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

bool oanim_document_from_json(const std::string &text, Document &r_doc, std::string *r_error)
{
  const json root = json::parse(text, nullptr, false);
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
  if (const int sync = Document::rna().find("sync_mode")->enum_index(get_string(j, "sync_mode", "")); sync >= 0) {
    doc.sync = PlaybackSync(sync);
  }
  doc.play_audio = get_bool(j, "play_audio", doc.play_audio);
  doc.audio_scrub = get_bool(j, "use_audio_scrub", doc.audio_scrub);
  doc.fps =std::clamp(get_float(j, "fps", doc.fps), 1.0f, 1000.0f);
  doc.width = std::clamp(get_int(j, "width", doc.width), 1, 65536);
  doc.height = std::clamp(get_int(j, "height", doc.height), 1, 65536);
  doc.background = get_color(j, "background", doc.background);
  doc.viewport_quality = std::clamp(get_int(j, "viewport_quality", doc.viewport_quality), 1, 64);
  doc.render_quality = std::clamp(get_int(j, "render_quality", doc.render_quality), 1, 64);
  doc.viewport_samples = std::clamp(get_int(j, "viewport_samples", doc.viewport_samples), 1, 16);
  doc.render_samples = std::clamp(get_int(j, "render_samples", doc.render_samples), 1, 16);
  overlays_from_json(j.value("viewport_overlays", json::object()), doc.overlays);
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
  for (const json &layer : get_array(j, "layers")) {
    if (layer.is_object()) {
      doc.layers.push_back(layer_from_json(layer, 0));
    }
  }
  migrate_old_masks(doc.layers);
  migrate_old_mask_options(doc.layers);
  if (version < 2) {
    migrate_bone_keys(doc.layers, doc.action);
  }
  g_old_masks.clear();
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
  return read;
}

}  // namespace oanim
