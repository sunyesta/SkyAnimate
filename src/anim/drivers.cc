#include "anim/drivers.hh"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <tuple>
#include <map>

#include "anim/animation.hh"
#include "anim/expression.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/material.hh"
#include "rna/rna.hh"

namespace oanim {

/* --- The numbers drivers set and read ---------------------------------------------------------- */

namespace {

/* Where a referenced number lives: the value, and for a channel what it last recorded and what smart
 * actions add, so a driven value never reads as an edit. */
struct Slot {
  float *value = nullptr;
  float *last = nullptr;
  bool *has_last = nullptr;
  float *smart = nullptr;
};

/* "shapes[12].fill": the shape's id and what of it (".fill", ".line" or ".position"). */
bool parse_shape_path(const std::string &path, uint64_t *r_id, std::string *r_what)
{
  if (path.rfind("shapes[", 0) != 0) {
    return false;
  }
  const size_t close = path.find(']');
  if (close == std::string::npos) {
    return false;
  }
  const std::string rest = path.substr(close + 1);
  if (rest != ".fill" && rest != ".line" && rest != ".position") {
    return false;
  }
  *r_id = std::strtoull(path.c_str() + 7, nullptr, 10);
  *r_what = rest;
  return true;
}

/* The shape a "shapes[<id>]..." path names, in its layer's drawing. */
Shape *shape_find(Document &doc, uint64_t layer_id, const std::string &path, std::string *r_what, Layer **r_layer = nullptr)
{
  uint64_t id = 0;
  if (!parse_shape_path(path, &id, r_what)) {
    return nullptr;
  }
  Layer *layer = layer_find(doc, layer_id);
  if (!layer || !layer->has_drawing()) {
    return nullptr;
  }
  const int s = drawing_shape_index(layer->drawing, id);
  if (s < 0) {
    return nullptr;
  }
  if (r_layer) {
    *r_layer = layer;
  }
  return &layer->drawing.shapes[size_t(s)];
}

Color *shape_color(Document &doc, uint64_t layer_id, const std::string &path)
{
  std::string what;
  Shape *shape = shape_find(doc, layer_id, path, &what);
  if (!shape || what == ".position") {
    return nullptr;
  }
  return what == ".fill" ? &shape->style.fill : &shape->style.line;
}

/* A shape's Position / Transform (the Shape tab's row, see shape_position_value): its layer and
 * points, or false when `path` isn't one. */
bool shape_position(Document &doc, uint64_t layer_id, const std::string &path, Layer **r_layer,
                    std::vector<int> *r_points)
{
  std::string what;
  Shape *shape = shape_find(doc, layer_id, path, &what, r_layer);
  if (!shape || what != ".position") {
    return false;
  }
  *r_points = shape_points((*r_layer)->drawing, *shape);
  return !r_points->empty();
}

/* What the shape's row shows, read from where the points are now (a drag moves them before their
 * keys' values are worked out): on frame 0 (and on a reference) their Position, the middle of them;
 * on any other frame their Transform, how far that has moved from the setup pose. */
float2 shape_position_value(const Document &doc, const Layer &layer, const std::vector<int> &points)
{
  float2 mean;
  drawing_points_mean(layer.drawing, points, &mean);
  if (doc.frame_current == 0 || layer.is_reference()) {
    return mean;
  }
  float2 setup;
  for (const int i : points) {
    setup += point_setup_pos(layer.drawing.points[size_t(i)]);
  }
  return mean - setup / float(points.size());
}

/* Moves a shape's points together so the row's `component` shows `value`, keeping their own motion
 * around it; their keys record it as evaluated, so it is never an edit. */
void shape_position_set(const Document &doc, Layer &layer, const std::vector<int> &points, int component, float value)
{
  VectorDrawing &drawing = layer.drawing;
  const float2 now = shape_position_value(doc, layer, points);
  const float2 delta = component == 0 ? float2(value - now.x, 0.0f) : float2(0.0f, value - now.y);
  if (delta.x == 0.0f && delta.y == 0.0f) {
    return;
  }
  drawing_points_move(drawing, points, delta);
  drawing_changes_from_values(drawing);
  for (const int i : points) {
    MeshPoint &p = drawing.points[size_t(i)];
    p.pos_keys.last = p.transform;
    p.pos_keys.has_last = true;
    p.pos_keys.smart = float2();
  }
}

/* --- Settings: values that aren't keyed but, as in Blender, can still be driven -----------------
 * Blender drives any number, checkbox, dropdown or colour of the scene's data (not text, links to
 * other data, or tool and editor settings). The channels above are the keyable ones; these are the
 * rest of a layer's data (and the document's background). Checkboxes read 0 or 1 and are on from
 * 0.5; whole numbers and dropdowns (by item, from 0) round. */

struct Setting {
  /* How many numbers it holds, and what it is called after its owner ("Mirror: Merge Distance"). */
  int size = 1;
  std::string name;
  std::function<float(int)> get;
  std::function<void(int, float)> set;
};

Setting float_setting(std::string name, float *v, float lo = -FLT_MAX, float hi = FLT_MAX)
{
  return {1, std::move(name), [v](int) { return *v; }, [v, lo, hi](int, float x) { *v = std::clamp(x, lo, hi); }};
}

Setting bool_setting(std::string name, bool *v)
{
  return {1, std::move(name), [v](int) { return *v ? 1.0f : 0.0f; }, [v](int, float x) { *v = x >= 0.5f; }};
}

Setting int_setting(std::string name, int *v, int lo, int hi)
{
  return {1, std::move(name), [v](int) { return float(*v); },
          [v, lo, hi](int, float x) { *v = std::clamp(int(std::lround(x)), lo, hi); }};
}

template<typename E> Setting enum_setting(std::string name, E *v, int count)
{
  return {1, std::move(name), [v](int) { return float(int(*v)); },
          [v, count](int, float x) { *v = E(std::clamp(int(std::lround(x)), 0, count - 1)); }};
}

Setting float2_setting(std::string name, float2 *v)
{
  return {2, std::move(name), [v](int c) { return c == 0 ? v->x : v->y; },
          [v](int c, float x) { (c == 0 ? v->x : v->y) = x; }};
}

Setting color_setting(std::string name, Color *v)
{
  return {4, std::move(name), [v](int c) { return (&v->r)[c]; },
          [v](int c, float x) { (&v->r)[c] = std::clamp(x, 0.0f, 1.0f); }};
}

/* A layer's own setting by its Python name ("props.media_volume"), through its RNA description. */
Setting rna_setting(Layer &layer, const std::string &identifier)
{
  /* Its look and sound (not its name, label colour or what it's editable with: Blender drives
   * none of those either). */
  static const char *const DRIVABLE[] = {"render_visible", "locked",       "media_guide",    "media_fit",
                                         "media_greyscale", "media_tint_amount", "media_in_front",
                                         "media_volume",   "media_muted"};
  if (std::find_if(std::begin(DRIVABLE), std::end(DRIVABLE), [&](const char *d) { return identifier == d; }) ==
      std::end(DRIVABLE))
  {
    return {};
  }
  const rna::PropertyRNA *prop = Layer::rna().find(identifier);
  if (!prop || !prop->set || (identifier.rfind("media_", 0) == 0 && layer.type != LayerType::Media)) {
    return {};
  }
  Layer *l = &layer;
  return {1, prop->name,
          [l, prop](int) {
            const rna::Value v = prop->get(l);
            switch (prop->type) {
              case rna::PropType::Bool:
                return std::get<bool>(v) ? 1.0f : 0.0f;
              case rna::PropType::Float:
                return std::get<float>(v);
              default:
                return float(std::get<int>(v));
            }
          },
          [l, prop](int, float x) {
            rna::Value v;
            switch (prop->type) {
              case rna::PropType::Bool:
                v = x >= 0.5f;
                break;
              case rna::PropType::Float:
                v = x;
                break;
              case rna::PropType::Enum:
                v = std::clamp(int(std::lround(x)), 0, int(prop->items.size()) - 1);
                break;
              default:
                v = int(std::lround(x));
                break;
            }
            prop->set(l, rna::clamp(*prop, v));
          }};
}

/* "masks[2]", "modifiers[0]", "constraints[1]": the number in brackets after `prefix`, and the rest
 * of the path after the closing bracket ("invert" from "masks[2].invert"). */
bool parse_index(const std::string &path, const char *prefix, size_t *r_index, std::string *r_rest)
{
  const size_t n = std::strlen(prefix);
  if (path.compare(0, n, prefix) != 0) {
    return false;
  }
  const size_t close = path.find("].", n);
  if (close == std::string::npos) {
    return false;
  }
  *r_index = size_t(std::strtoull(path.c_str() + n, nullptr, 10));
  *r_rest = path.substr(close + 2);
  return true;
}

Setting constraint_setting(BoneConstraint &c, const std::string &rest)
{
  const std::string name = c.name.empty() ? std::string(bone_constraint_name(c.type)) : c.name;
  const auto named = [&](const char *what) { return name + ": " + what; };
  if (rest == "enabled") return bool_setting(named("Enabled"), &c.enabled);
  if (rest == "min_angle") return float_setting(named("Min Angle"), &c.min_angle);
  if (rest == "max_angle") return float_setting(named("Max Angle"), &c.max_angle);
  if (rest == "amount") return float_setting(named("Amount"), &c.amount, 0.0f, 4.0f);
  if (rest == "factor") return float_setting(named("Factor"), &c.factor);
  if (rest == "delay") return int_setting(named("Delay"), &c.delay, 0, 1000);
  if (rest == "chain") return int_setting(named("Chain"), &c.chain, 0, 64);
  if (rest == "max_stretch") return float_setting(named("Max Stretch"), &c.max_stretch, 1.0f, 10.0f);
  if (rest == "own_gravity") return bool_setting(named("Own Gravity"), &c.own_gravity);
  if (rest == "own_wind") return bool_setting(named("Own Wind"), &c.own_wind);
  for (const auto &[prefix, label, s] : {std::tuple<const char *, const char *, BoneSpring *>{"angle_spring.", "Angle", &c.angle_spring},
                                         {"position_spring.", "Position", &c.position_spring},
                                         {"scale_spring.", "Squash & Stretch", &c.scale_spring}})
  {
    if (rest == std::string(prefix) + "enabled") return bool_setting(named(label), &s->enabled);
    if (rest == std::string(prefix) + "use_limit") return bool_setting(named(label) + " Limit", &s->use_limit);
  }
  return {};
}

/* A shared material's look ("materials[<id>].fill", ".line", ".line_width"): the material and every
 * shape linked to it at once, so they stay in step (document_sync_materials sees no edit). */
Setting material_setting(Document &doc, uint64_t id, const std::string &rest)
{
  Material *m = material_find(doc, id);
  if (!m || (rest != "fill" && rest != "line" && rest != "line_width")) {
    return {};
  }
  Document *d = &doc;
  const auto field = [rest](Style &style, int c) -> float & {
    return rest == "line_width" ? style.line_width : (&(rest == "fill" ? style.fill : style.line).r)[c];
  };
  const float px = float(doc.height) * 0.5f;
  const bool width = rest == "line_width";
  return {width ? 1 : 4, m->name + ": " + (width ? "Line Width" : rest == "fill" ? "Fill Color" : "Line Color"),
          [m, field, width, px](int c) { return field(m->style, c) * (width ? px : 1.0f); },
          [d, m, field, width, px](int c, float x) {
            /* Line Width in pixels, as shown; colours 0-1. */
            const float v = width ? std::clamp(x, 0.0f, 500.0f) / px : std::clamp(x, 0.0f, 1.0f);
            field(m->style, c) = v;
            const uint64_t mid = m->id;
            document_for_each_shape(*d, [&](Layer &, VectorDrawing &, Shape &shape) {
              if (shape.material == mid) {
                field(shape.style, c) = v;
              }
            });
          }};
}

/* The setting `path` names on `layer_id` (0: the document), or one of size 0. */
Setting setting_find(Document &doc, uint64_t layer_id, const std::string &path)
{
  if (layer_id == 0) {
    size_t id = 0;
    std::string rest;
    if (parse_index(path, "materials[", &id, &rest)) {
      return material_setting(doc, uint64_t(id), rest);
    }
    return path == "background" ? color_setting("Background Color", &doc.background) : Setting{};
  }
  Layer *layer = layer_find(doc, layer_id);
  if (!layer) {
    return {};
  }
  if (path.rfind("props.", 0) == 0) {
    return rna_setting(*layer, path.substr(6));
  }
  if (path == "media.tint" && layer->type == LayerType::Media) {
    return color_setting("Tint Color", &layer->media.tint);
  }
  if (path == "clip_to_below") return bool_setting("Clip to Below", &layer->clip_to_below);
  if (path == "clip_exclude_strokes") return bool_setting("Exclude Strokes", &layer->clip_exclude_strokes);
  size_t i = 0;
  std::string rest;
  if (parse_index(path, "masks[", &i, &rest) && i < layer->masks.size()) {
    LayerMask &card = layer->masks[i];
    const std::string row = "Mask " + std::to_string(i + 1) + ": ";
    if (rest == "start") return bool_setting(row + "Start With Everything", &card.start_shown);
    if (rest == "end_lines") return bool_setting(row + "End Lines at Edge", &card.end_lines);
    if (rest == "ends") return bool_setting(row + "Ends Flat", &card.end_lines_flat);
    if (rest == "cuts") return enum_setting(row + "Affects", &card.cuts, 3);
    /* A step's: "steps[1].invert"; on the card itself, its first step's (as before cards). */
    size_t k = 0;
    std::string step_rest;
    if (parse_index(rest, "steps[", &k, &step_rest)) {
      rest = step_rest;
    }
    if (k >= card.steps.size()) {
      return {};
    }
    MaskStep &m = card.steps[k];
    const std::string step = row + "Step " + std::to_string(k + 1) + " ";
    if (rest == "invert") return bool_setting(step + "Invert", &m.invert);
    if (rest == "mode") return enum_setting(step + "Mode", &m.mode, 2);
    if (rest == "grow") return float_setting(step + "Grow", &m.grow, -64.0f, 64.0f);
    if (rest == "extend") return enum_setting(step + "Extend", &m.extend, 5);
    if (rest == "filled") return bool_setting(step + "Fill Every Shape", &m.filled);
    if (rest == "inside_stroke") return bool_setting(step + "Inside the Stroke", &m.inside_stroke);
    if (rest == "shape") {
      /* As Drawn (0), Fills Only (1), Strokes Only (2). */
      MaskStep *row_mask = &m;
      return {1, step + "Shape", [row_mask](int) { return row_mask->fills_only ? 1.0f : row_mask->strokes_only ? 2.0f : 0.0f; },
              [row_mask](int, float x) {
                const int k = std::clamp(int(std::lround(x)), 0, 2);
                row_mask->fills_only = k == 1;
                row_mask->strokes_only = k == 2;
              }};
    }
    return {};
  }
  if (parse_index(path, "modifiers[", &i, &rest) && i < layer->modifiers.size()) {
    Modifier &m = layer->modifiers[i];
    const std::string mod = m.name + ": ";
    if (rest == "show_viewport") return bool_setting(mod + "Viewport", &m.show_viewport);
    if (rest == "show_render") return bool_setting(mod + "Render", &m.show_render);
    if (m.type == ModifierType::Boolean) {
      if (rest == "smoothing") return float_setting(mod + "Smoothing", &m.merge_smoothing, 0.0f, 100.0f);
      return {};
    }
    if (m.type != ModifierType::Mirror) return {};
    if (rest == "axis_x") return bool_setting(mod + "Axis X", &m.axis_x);
    if (rest == "axis_y") return bool_setting(mod + "Axis Y", &m.axis_y);
    if (rest == "center") return float2_setting(mod + "Center", &m.center);
    if (rest == "merge") return bool_setting(mod + "Merge", &m.merge);
    if (rest == "merge_distance") return float_setting(mod + "Merge Distance", &m.merge_distance, 0.0f, 1.0f);
    if (rest == "mirror_bones") return bool_setting(mod + "Mirror Bones", &m.mirror_bones);
    return {};
  }
  if (path.rfind("bones[\"", 0) == 0) {
    const size_t end = path.find("\"].");
    if (end == std::string::npos) {
      return {};
    }
    Bone *bone = const_cast<Bone *>(bone_find_by_name(*layer, path.substr(7, end - 7)));
    if (!bone) {
      return {};
    }
    rest = path.substr(end + 3);
    if (rest == "strength") return float_setting(bone->name + ": Strength", &bone->strength, 0.0f, 100.0f);
    if (rest == "pin_size") return float_setting(bone->name + ": Pin Size", &bone->pin_size, 0.1f, 20.0f);
    if (rest == "ik_ignore") return bool_setting(bone->name + ": Ignored by IK", &bone->ik_ignore);
    if (rest == "arc_ik") return bool_setting(bone->name + ": Arc Solver", &bone->arc_ik);
    if (parse_index(rest, "constraints[", &i, &rest) && i < bone->constraints.size()) {
      Setting s = constraint_setting(bone->constraints[i], rest);
      s.name = bone->name + " › " + s.name;
      return s.get ? s : Setting{};
    }
    return {};
  }
  std::string what;
  Shape *shape = nullptr;
  if (path.rfind("shapes[", 0) == 0 && layer->has_drawing()) {
    const size_t close = path.find("].");
    const int s = close == std::string::npos
                      ? -1
                      : drawing_shape_index(layer->drawing, std::strtoull(path.c_str() + 7, nullptr, 10));
    if (s >= 0) {
      shape = &layer->drawing.shapes[size_t(s)];
      rest = path.substr(close + 2);
      const std::string sn = (shape->name.empty() ? std::string("a shape") : shape->name) + ": ";
      if (rest == "has_fill") return bool_setting(sn + "Fill", &shape->has_fill);
      if (rest == "has_outline") return bool_setting(sn + "Outline", &shape->has_outline);
      if (rest == "line_width") {
        /* In pixels, as its field shows it (stored in document units; the frame is 2 tall). */
        float *width = &shape->style.line_width;
        const float px = float(doc.height) * 0.5f;
        return {1, sn + "Line Width", [width, px](int) { return *width * px; },
                [width, px](int, float x) { *width = std::clamp(x, 0.0f, 500.0f) / px; }};
      }
    }
  }
  return {};
}

bool slot_find(Document &doc, const DriverRef &ref, Slot *r_slot)
{
  if (Color *c = shape_color(doc, ref.layer, ref.path)) {
    if (ref.component < 0 || ref.component > 3) {
      return false;
    }
    float *parts[4] = {&c->r, &c->g, &c->b, &c->a};
    r_slot->value = parts[ref.component];
    return true;
  }
  Layer *layer = layer_find(doc, ref.layer);
  ChannelRef ch;
  if (!layer || !layer_channel(*layer, ref.path, &ch)) {
    return false;
  }
  if (ch.scalar) {
    if (ref.component > 0) {
      return false;
    }
    *r_slot = {ch.scalar_value, &ch.scalar->last, &ch.scalar->has_last, &ch.scalar->smart};
    return true;
  }
  if (ch.vector && (ref.component == 0 || ref.component == 1)) {
    const bool x = ref.component == 0;
    *r_slot = {x ? &ch.vector_value->x : &ch.vector_value->y, x ? &ch.vector->last.x : &ch.vector->last.y,
               &ch.vector->has_last, x ? &ch.vector->smart.x : &ch.vector->smart.y};
    return true;
  }
  return false;
}

}  // namespace

int driver_ref_size(Document &doc, uint64_t layer_id, const std::string &path)
{
  if (shape_color(doc, layer_id, path)) {
    return 4;
  }
  Layer *shape_layer = nullptr;
  std::vector<int> points;
  if (shape_position(doc, layer_id, path, &shape_layer, &points)) {
    return 2;
  }
  Layer *layer = layer_find(doc, layer_id);
  ChannelRef ch;
  if (!layer || !layer_channel(*layer, path, &ch)) {
    const Setting setting = setting_find(doc, layer_id, path);
    return setting.get ? setting.size : 0;
  }
  return ch.vector ? 2 : 1;
}

bool driver_ref_read(Document &doc, const DriverRef &ref, float *r_value)
{
  Layer *shape_layer = nullptr;
  std::vector<int> points;
  if (shape_position(doc, ref.layer, ref.path, &shape_layer, &points)) {
    if (ref.component != 0 && ref.component != 1) {
      return false;
    }
    const float2 mean = shape_position_value(doc, *shape_layer, points);
    *r_value = ref.component == 0 ? mean.x : mean.y;
    return true;
  }
  Slot slot;
  if (!slot_find(doc, ref, &slot)) {
    const Setting setting = setting_find(doc, ref.layer, ref.path);
    if (!setting.get || ref.component >= setting.size || (setting.size > 1 && ref.component < 0)) {
      return false;
    }
    *r_value = setting.get(std::max(ref.component, 0));
    return true;
  }
  *r_value = *slot.value;
  return true;
}

std::string driver_ref_label(Document &doc, const DriverRef &ref)
{
  if (ref.layer == 0) {
    const Setting setting = setting_find(doc, 0, ref.path);
    return std::string(ref.path.rfind("materials[", 0) == 0 ? "Material " : "Document: ") +
           (setting.get ? setting.name : ref.path) +
           (setting.size == 4 && ref.component >= 0 && ref.component < 4 ? std::string(" ") + "RGBA"[ref.component]
                                                                        : std::string());
  }
  Layer *layer = layer_find(doc, ref.layer);
  if (!layer) {
    return "(a layer that's gone)";
  }
  ChannelRef ch;
  if (!layer_channel(*layer, ref.path, &ch)) {
    if (const Setting setting = setting_find(doc, ref.layer, ref.path); setting.get) {
      std::string part;
      if (setting.size == 4 && ref.component >= 0 && ref.component < 4) {
        part = std::string(" ") + "RGBA"[ref.component];
      }
      else if (setting.size == 2 && ref.component >= 0) {
        part = ref.component == 0 ? " X" : " Y";
      }
      return layer->name + ": " + setting.name + part;
    }
  }
  uint64_t id = 0;
  std::string what;
  if (parse_shape_path(ref.path, &id, &what)) {
    const int s = layer->has_drawing() ? drawing_shape_index(layer->drawing, id) : -1;
    const std::string shape = s >= 0 && !layer->drawing.shapes[size_t(s)].name.empty() ? layer->drawing.shapes[size_t(s)].name
                                                                                      : std::string("a shape");
    if (what == ".position") {
      return layer->name + " › " + shape + ": Position" +
             (ref.component == 0 ? " X" : ref.component == 1 ? " Y" : "");
    }
    const char *parts = "RGBA";
    return layer->name + " › " + shape + ": " + (what == ".fill" ? "Fill Color" : "Line Color") +
           (ref.component >= 0 && ref.component < 4 ? std::string(" ") + parts[ref.component] : std::string());
  }
  std::string label = channel_label(*layer, ref.path, true);
  if (driver_ref_size(doc, ref.layer, ref.path) == 2 && ref.component >= 0) {
    label += ref.component == 0 ? " X" : " Y";
  }
  return label;
}

Driver *driver_find(Document &doc, const DriverRef &target)
{
  for (Driver &d : doc.drivers) {
    if (d.target == target) {
      return &d;
    }
  }
  return nullptr;
}

std::vector<DriverRef> driver_field_refs(Document &doc, uint64_t layer, const std::string &path, int component)
{
  const int size = driver_ref_size(doc, layer, path);
  std::vector<DriverRef> refs;
  /* Points and curves are read, not driven (as Blender doesn't drive a mesh's vertices); a part past
   * the field's end names nothing. */
  if (size == 0 || path.rfind("points[", 0) == 0 || path.rfind("curves[", 0) == 0 || component >= size) {
    return refs;
  }
  if (size == 1) {
    refs.push_back({layer, path, -1});
  }
  else if (component >= 0 && component < size) {
    refs.push_back({layer, path, component});
  }
  else {
    for (int c = 0; c < size; c++) {
      refs.push_back({layer, path, c});
    }
  }
  return refs;
}

std::string driver_shape_color_path(uint64_t shape_id, bool fill)
{
  return "shapes[" + std::to_string(shape_id) + "]" + (fill ? ".fill" : ".line");
}

std::string driver_shape_position_path(uint64_t shape_id)
{
  return "shapes[" + std::to_string(shape_id) + "].position";
}

void drivers_copy_for_layers(Document &doc, const std::map<uint64_t, uint64_t> &remap)
{
  const auto copied = [&](DriverRef &ref) {
    const auto it = remap.find(ref.layer);
    if (it != remap.end()) {
      ref.layer = it->second;
    }
    /* Worked out again on the copy. */
    ref.bone = 0;
    ref.item = 0;
  };
  const size_t n = doc.drivers.size();
  for (size_t i = 0; i < n; i++) {
    if (!remap.contains(doc.drivers[i].target.layer)) {
      continue;
    }
    Driver d = doc.drivers[i];
    copied(d.target);
    for (DriverVar &v : d.vars) {
      copied(v.source);
    }
    d.error.clear();
    doc.drivers.push_back(std::move(d));
  }
}

std::vector<Driver> &driver_clipboard()
{
  static std::vector<Driver> clipboard;
  return clipboard;
}

Driver driver_copying(const DriverRef &source)
{
  Driver d;
  d.type = DriverType::Expression;
  d.expression = "var";
  d.vars.push_back({"var", source});
  return d;
}

/* --- Working them out --------------------------------------------------------------------------- */

/* A channel's limits, as its field has them (Blender clamps a driven value to its property's): by
 * the path's last name, the same for a constraint's own Gravity as the document's. */
static void channel_range(const std::string &path, float *r_lo, float *r_hi)
{
  const size_t dot = path.rfind('.');
  const std::string name = dot == std::string::npos ? path : path.substr(dot + 1);
  struct Range {
    const char *name;
    float lo, hi;
  };
  static const Range RANGES[] = {
      {"opacity", 0.0f, 1.0f},        {"visible", 0.0f, 1.0f},      {"influence", 0.0f, 1.0f},
      {"wind_catch", 0.0f, 1.0f},     {"bounce", 0.0f, 0.9f},       {"settle_speed", 0.05f, 20.0f},
      {"inertia", 0.0f, 2.0f},        {"stretch", 0.0f, 2.0f},      {"limit", 0.0f, FLT_MAX},
      {"weight", 0.0f, FLT_MAX},      {"gravity", -100.0f, 100.0f}, {"wind", -100.0f, 100.0f},
      {"wind_gusts", 0.0f, 2.0f},     {"wind_gust_speed", 0.05f, 10.0f}, {"wind_turbulence", 0.0f, 2.0f},
      {"reach", -2.0f, 2.0f},
  };
  *r_lo = -FLT_MAX;
  *r_hi = FLT_MAX;
  for (const Range &r : RANGES) {
    if (name == r.name) {
      *r_lo = r.lo;
      *r_hi = r.hi;
    }
  }
}

/* A bone's pose channel: 1 its position, 2 its angle, 3 its scale; 0 anything else. */
static int bone_pose_part(const std::string &path)
{
  if (path.rfind("bones[\"", 0) != 0) {
    return 0;
  }
  const size_t end = path.find("\"].");
  const std::string rest = end == std::string::npos ? std::string() : path.substr(end + 3);
  return rest == "position" ? 1 : rest == "angle" ? 2 : rest == "scale" ? 3 : 0;
}

/* A shape's points at their setup pose (no change from it), as evaluated: never an edit. */
static void shape_points_rest(Layer &layer, const std::vector<int> &points)
{
  VectorDrawing &drawing = layer.drawing;
  drawing_changes_from_values(drawing);
  for (const int i : points) {
    MeshPoint &p = drawing.points[size_t(i)];
    p.transform = float2();
    p.pos_keys.last = p.transform;
    p.pos_keys.has_last = true;
    p.pos_keys.smart = float2();
  }
  drawing_values_from_changes(drawing);
}

/* Some channels hold a stand-in for the value itself: Visibility a number for the `visible` flag, a
 * bone's position and angle its change from the setup pose. A driven stand-in goes into the value at
 * once (the next channel lookup works the stand-in out from the value again). */
static void channel_value_applied(Document &doc, const DriverRef &target)
{
  Layer *layer = layer_find(doc, target.layer);
  if (!layer) {
    return;
  }
  if (target.path == "visible") {
    layer->visible = layer->visible_value > 0.5f;
    return;
  }
  if (target.path.rfind("bones[\"", 0) != 0) {
    return;
  }
  const size_t end = target.path.find("\"].");
  if (end == std::string::npos) {
    return;
  }
  const std::string rest = target.path.substr(end + 3);
  if (rest == "position" || rest == "angle") {
    if (Bone *bone = const_cast<Bone *>(bone_find_by_name(*layer, target.path.substr(7, end - 7)))) {
      bone_values_from_changes(*bone);
    }
  }
}

/* Compiled expressions, by their text (a file has a handful; they're kept for good). */
static const Expression *compiled(const std::string &text, std::string *r_error)
{
  struct Entry {
    Expression expression;
    std::string error;
  };
  static std::map<std::string, Entry> cache;
  auto it = cache.find(text);
  if (it == cache.end()) {
    Entry e;
    expression_compile(text, &e.expression, &e.error);
    it = cache.emplace(text, std::move(e)).first;
  }
  *r_error = it->second.error;
  return it->second.error.empty() ? &it->second.expression : nullptr;
}

static bool driver_value(Document &doc, Driver &d, double *r_value)
{
  std::vector<double> values;
  values.reserve(d.vars.size());
  for (const DriverVar &v : d.vars) {
    float f = 0.0f;
    if (!driver_ref_read(doc, v.source, &f)) {
      d.error = "Variable '" + v.name + "' reads something that's gone";
      return false;
    }
    values.push_back(double(f));
  }
  double result = 0.0;
  if (d.type == DriverType::Expression) {
    std::string error;
    const Expression *e = compiled(d.expression, &error);
    if (!e) {
      d.error = error;
      return false;
    }
    std::vector<double> named;
    named.reserve(e->names.size());
    /* A name is a variable first (the first of that name), then frame and the constants. */
    for (const std::string &name : e->names) {
      const auto it = std::find_if(d.vars.begin(), d.vars.end(), [&](const DriverVar &v) { return v.name == name; });
      if (it != d.vars.end()) {
        named.push_back(values[size_t(it - d.vars.begin())]);
      }
      else if (name == "frame") {
        named.push_back(double(doc.frame_current));
      }
      else if (name == "pi") {
        named.push_back(3.14159265358979323846);
      }
      else if (name == "e") {
        named.push_back(2.71828182845904523536);
      }
      else if (name == "True" || name == "False") {
        named.push_back(name == "True" ? 1.0 : 0.0);
      }
      else {
        d.error = "No variable called '" + name + "'";
        return false;
      }
    }
    result = expression_run(*e, named);
  }
  else {
    if (values.empty()) {
      d.error = "No variables to combine";
      return false;
    }
    double sum = 0.0, lo = values[0], hi = values[0];
    for (const double v : values) {
      sum += v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    result = d.type == DriverType::Sum ? sum : d.type == DriverType::Average ? sum / double(values.size())
                                          : d.type == DriverType::Min      ? lo
                                                                            : hi;
  }
  if (d.remap) {
    const double span = double(d.from_max) - double(d.from_min);
    double t = span != 0.0 ? (result - double(d.from_min)) / span : 0.0;
    if (d.clamp) {
      t = std::clamp(t, 0.0, 1.0);
    }
    result = double(d.to_min) + (double(d.to_max) - double(d.to_min)) * t;
  }
  if (!std::isfinite(result)) {
    d.error = "The result isn't a number (a division by zero?)";
    return false;
  }
  if (std::abs(result) > double(FLT_MAX)) {
    d.error = "The result is too big";
    return false;
  }
  *r_value = result;
  return true;
}

/* --- Following what a path means ------------------------------------------------------------------
 * Paths name bones by name and constraints, modifiers and mask rows by their place, which renaming
 * a bone or removing, adding or moving one of those changes (Blender fixes driver paths on renames
 * the same way). Each reference keeps the bone's id and the item's uid it last meant, and before
 * the drivers run, a path whose bone was renamed or whose item moved is written to match; one whose
 * bone or item is gone is reported (`false`). */

static uint64_t next_uid()
{
  static uint64_t uid = 0;
  return ++uid;
}

/* `list[index]` in "<prefix>[index].rest" at `at`: follows it to where it is now. */
template<typename T>
static bool follow_item(std::vector<T> &list, DriverRef &ref, size_t at, const char *prefix)
{
  const size_t open = at + std::strlen(prefix);
  const size_t close = ref.path.find(']', open);
  if (close == std::string::npos) {
    return true;
  }
  const size_t index = size_t(std::strtoull(ref.path.c_str() + open, nullptr, 10));
  if (ref.item == 0) {
    /* First time: what the index names now. */
    if (index < list.size()) {
      if (list[index].uid == 0) {
        list[index].uid = next_uid();
      }
      ref.item = list[index].uid;
    }
    return true;
  }
  for (size_t i = 0; i < list.size(); i++) {
    if (list[i].uid == ref.item) {
      if (i != index) {
        ref.path = ref.path.substr(0, open) + std::to_string(i) + ref.path.substr(close);
      }
      return true;
    }
  }
  return false;
}

/* Brings `ref`'s path up to date (see above). False when what it named is gone. */
static bool ref_follow(Document &doc, DriverRef &ref)
{
  if (ref.layer == 0) {
    return true;
  }
  Layer *layer = layer_find(doc, ref.layer);
  if (!layer) {
    return false;
  }
  std::string &path = ref.path;
  if (path.rfind("bones[\"", 0) == 0) {
    const size_t end = path.find("\"]");
    if (end == std::string::npos) {
      return true;
    }
    Bone *bone = nullptr;
    if (ref.bone != 0) {
      bone = bone_find(*layer, ref.bone);
      if (!bone) {
        return false;
      }
      if (bone->name != path.substr(7, end - 7)) {
        path = "bones[\"" + bone->name + path.substr(end);
      }
    }
    else {
      bone = const_cast<Bone *>(bone_find_by_name(*layer, path.substr(7, end - 7)));
      if (!bone) {
        return true;
      }
      ref.bone = bone->id;
    }
    const size_t at = path.find("\"].constraints[");
    return at == std::string::npos || follow_item(bone->constraints, ref, at + 3, "constraints[");
  }
  if (path.rfind("modifiers[", 0) == 0) {
    return follow_item(layer->modifiers, ref, 0, "modifiers[");
  }
  if (path.rfind("masks[", 0) == 0) {
    return follow_item(layer->masks, ref, 0, "masks[");
  }
  return true;
}

/* Every driver's paths followed; drivers whose target is gone (its layer, bone or item deleted) go
 * with it, as Blender's go with their object; a variable whose source is gone is marked so it
 * reports (rather than reading whatever took its place). */
static void drivers_follow(Document &doc)
{
  std::erase_if(doc.drivers, [&](Driver &d) { return !ref_follow(doc, d.target); });
  for (Driver &d : doc.drivers) {
    for (DriverVar &v : d.vars) {
      if (!ref_follow(doc, v.source) && v.source.path.rfind("(gone) ", 0) != 0) {
        v.source.path = "(gone) " + v.source.path;
      }
    }
  }
}

/* A driver on a shape's look whose shape shares a material drives the material instead (the look is
 * the material's, so every shape sharing it shows it driven), unless the material has that driver
 * already. */
static void drivers_onto_materials(Document &doc)
{
  for (Driver &d : doc.drivers) {
    const std::string &path = d.target.path;
    const size_t close = path.find("].");
    if (d.target.layer == 0 || path.rfind("shapes[", 0) != 0 || close == std::string::npos) {
      continue;
    }
    const std::string what = path.substr(close + 2);
    if (what != "fill" && what != "line" && what != "line_width") {
      continue;
    }
    Layer *layer = layer_find(doc, d.target.layer);
    const int s = layer && layer->has_drawing()
                      ? drawing_shape_index(layer->drawing, std::strtoull(path.c_str() + 7, nullptr, 10))
                      : -1;
    const uint64_t material = s >= 0 ? layer->drawing.shapes[size_t(s)].material : 0;
    if (material == 0 || !material_find(doc, material)) {
      continue;
    }
    const DriverRef onto{0, "materials[" + std::to_string(material) + "]." + what, d.target.component};
    if (!driver_find(doc, onto)) {
      d.target = onto;
    }
  }
}

bool drivers_evaluate(Document &doc)
{
  std::vector<Driver> &drivers = doc.drivers;
  if (drivers.empty()) {
    return false;
  }
  drivers_follow(doc);
  if (drivers.empty()) {
    return false;
  }
  drivers_onto_materials(doc);
  /* Each after the drivers whose numbers it reads; a loop stops the drivers in it. */
  const size_t n = drivers.size();
  std::vector<int> state(n, 0);
  std::vector<size_t> order;
  order.reserve(n);
  std::vector<bool> looped(n, false);
  const auto visit = [&](auto &&self, size_t i) -> bool {
    if (state[i] == 2) {
      return true;
    }
    if (state[i] == 1) {
      looped[i] = true;
      return false;
    }
    state[i] = 1;
    bool ok = true;
    for (const DriverVar &v : drivers[i].vars) {
      for (size_t j = 0; j < n; j++) {
        if (drivers[j].target == v.source && !self(self, j)) {
          looped[i] = true;
          ok = false;
        }
      }
    }
    state[i] = 2;
    order.push_back(i);
    return ok;
  };
  for (size_t i = 0; i < n; i++) {
    visit(visit, i);
  }
  bool simulation = false;
  bool posed = false;
  for (const size_t i : order) {
    Driver &d = drivers[i];
    d.error.clear();
    if (looped[i]) {
      d.error = "Loop: it depends on its own value";
      continue;
    }
    /* The setup frame (0) is the rig at rest, as constraints leave it: drivers don't pose bones or move
     * shapes there (it would become the setup pose, and every visit add to it again). */
    const bool setup = doc.frame_current == 0;
    Layer *shape_layer = nullptr;
    std::vector<int> points;
    if (shape_position(doc, d.target.layer, d.target.path, &shape_layer, &points)) {
      double value = 0.0;
      if (setup) {
        shape_points_rest(*shape_layer, points);
      }
      else if ((d.target.component == 0 || d.target.component == 1) && driver_value(doc, d, &value)) {
        shape_position_set(doc, *shape_layer, points, d.target.component, float(value));
      }
      continue;
    }
    Slot slot;
    if (!slot_find(doc, d.target, &slot)) {
      const Setting setting = setting_find(doc, d.target.layer, d.target.path);
      const int part = std::max(d.target.component, 0);
      if (!setting.get || part >= setting.size) {
        d.error = "What it drives is gone";
        continue;
      }
      double value = 0.0;
      if (driver_value(doc, d, &value)) {
        setting.set(part, float(value));
      }
      continue;
    }
    const int pose = bone_pose_part(d.target.path);
    double value = 0.0;
    if (setup && pose != 0) {
      /* At rest: no change from the setup pose, a scale of 1. */
      value = pose == 3 ? 1.0 : 0.0;
    }
    else if (!driver_value(doc, d, &value)) {
      continue;
    }
    posed = posed || pose != 0;
    float v = float(value);
    if (shape_color(doc, d.target.layer, d.target.path)) {
      v = std::clamp(v, 0.0f, 1.0f);
    }
    else {
      float lo = 0.0f, hi = 0.0f;
      channel_range(d.target.path, &lo, &hi);
      v = std::clamp(v, lo, hi);
    }
    *slot.value = v;
    if (slot.last) {
      /* Recorded as evaluated, with nothing of smart actions in it: never an edit, never keyed. */
      *slot.smart = 0.0f;
      *slot.last = v;
      *slot.has_last = true;
    }
    channel_value_applied(doc, d.target);
    simulation = simulation || d.target.layer == SIMULATION_LAYER_ID;
  }
  if (simulation) {
    simulation_values_out(doc);
  }
  return posed;
}

}  // namespace oanim
