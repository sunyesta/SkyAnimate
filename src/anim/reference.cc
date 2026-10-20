#include "anim/reference.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/layer.hh"

namespace sky {

static bool same_color(Color a, Color b)
{
  return std::fabs(a.r - b.r) < 1e-4f && std::fabs(a.g - b.g) < 1e-4f && std::fabs(a.b - b.b) < 1e-4f &&
         std::fabs(a.a - b.a) < 1e-4f;
}

/* Every layer, parents before their children. */
template<typename F> static void visit_layers(std::vector<Layer> &layers, F &&f)
{
  for (Layer &layer : layers) {
    f(layer);
    visit_layers(layer.children, f);
  }
}

/* The path from the top of the tree down to a layer (its parents, then it). */
static bool layer_path_to(const std::vector<Layer> &layers, uint64_t id, std::vector<const Layer *> &r_path)
{
  for (const Layer &layer : layers) {
    r_path.push_back(&layer);
    if (layer.id == id || layer_path_to(layer.children, id, r_path)) {
      return true;
    }
    r_path.pop_back();
  }
  return false;
}

Style reference_swapped_style(const Layer &ref, const Style &style)
{
  Style s = style;
  for (const ReferenceSwap &sw : ref.reference.swaps) {
    if (sw.fill && same_color(style.fill, sw.from)) {
      s.fill = sw.to;
    }
    if (sw.line && same_color(style.line, sw.from)) {
      s.line = sw.to;
    }
  }
  return s;
}

static void apply_look(Shape &shape, const ReferenceLook &look)
{
  if (look.fill_set) {
    shape.style.fill = look.fill;
  }
  if (look.line_set) {
    shape.style.line = look.line;
  }
  if (look.width_set) {
    shape.style.line_width = look.line_width;
  }
  if (look.brush_set) {
    shape.style.brush = look.brush;
  }
  if (look.has_fill_set) {
    shape.has_fill = look.has_fill;
  }
  if (look.has_outline_set) {
    shape.has_outline = look.has_outline;
  }
  shape.hidden = look.hidden;
}

/* The keys of a curve point's two handles: by its point and the neighbours they point toward. */
static ReferenceHandleKey handle_key(const VectorDrawing &d, const Curve &curve, int i, bool in)
{
  const int n = int(curve.points.size());
  const int j = in ? (i > 0 ? i - 1 : (curve.closed ? n - 1 : -1)) : (i + 1 < n ? i + 1 : (curve.closed ? 0 : -1));
  const uint64_t toward = j >= 0 && j != i ? d.points[curve.points[j].point].id : 0;
  return {d.points[curve.points[i].point].id, toward, in};
}

/* An edge's key: the ids of the points at either end, in curve order. */
static std::pair<uint64_t, uint64_t> edge_key(const VectorDrawing &d, const Curve &curve, int segment)
{
  int a, b;
  curve_segment_points(curve, segment, &a, &b);
  return {d.points[a].id, d.points[b].id};
}

/* An offset's value: as on the setup frame, or as now. */
static float2 offset_value(const ReferencePoint &o, bool setup)
{
  return setup && o.offset_keys.animated() ? o.offset_keys.evaluate(0) : o.offset;
}

VectorDrawing reference_build(const Document &doc, const Layer &ref, bool setup)
{
  const Layer *source = layer_find(doc, ref.reference_source);
  if (!source || source->is_group()) {
    return ref.drawing;
  }
  const ReferenceSettings &rs = ref.reference;
  VectorDrawing d = setup || !rs.follow_animation ? drawing_setup_pose(source->drawing) : source->drawing;
  for (MeshPoint &p : d.points) {
    p.selected = false;
    if (const auto it = rs.points.find(p.id); it != rs.points.end()) {
      p.pos += offset_value(it->second, setup);
    }
    if (!rs.match_original) {
      const auto w = rs.weights.find(p.id);
      p.weights = w != rs.weights.end() ? w->second : std::vector<BoneWeight>{};
    }
  }
  for (Curve &curve : d.curves) {
    if (!rs.edges.empty()) {
      for (int s = 0; s < curve.segment_count(); s++) {
        if (const auto it = rs.edges.find(edge_key(d, curve, s)); it != rs.edges.end()) {
          curve.points[s].edge_hidden = it->second;
        }
      }
    }
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      cp.select_in = cp.select_out = false;
      if (!rs.handles.empty()) {
        for (const bool in : {true, false}) {
          if (const auto it = rs.handles.find(handle_key(d, curve, i, in)); it != rs.handles.end()) {
            (in ? cp.handle_in : cp.handle_out) += offset_value(it->second, setup);
          }
        }
      }
    }
  }
  for (Shape &shape : d.shapes) {
    shape.style = reference_swapped_style(ref, shape.style);
    if (const auto it = rs.looks.find(shape.id); it != rs.looks.end()) {
      apply_look(shape, it->second);
    }
  }
  return d;
}

static bool same_curves(const std::vector<Curve> &a, const std::vector<Curve> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t c = 0; c < a.size(); c++) {
    if (a[c].closed != b[c].closed || a[c].points.size() != b[c].points.size()) {
      return false;
    }
    for (size_t i = 0; i < a[c].points.size(); i++) {
      if (a[c].points[i].point != b[c].points[i].point) {
        return false;
      }
    }
  }
  return true;
}

/* Calls f(name, the reference's channel, its value, the original's value, built value) for each
 * layer channel a reference can follow. */
template<typename F> static void for_each_layer_channel(Layer &ref, const Layer &source, F &&f)
{
  ReferenceSettings &rs = ref.reference;
  f("position", ref.position_keys, ref.position, source.position, rs.built_position);
  f("rotation", ref.rotation_keys, ref.rotation, source.rotation, rs.built_rotation);
  f("scale", ref.scale_keys, ref.scale, source.scale, rs.built_scale);
  f("opacity", ref.opacity_keys, ref.opacity, source.opacity, rs.built_opacity);
}

/* The layer channels it hasn't taken over show the original's (its animation too); keyed on the
 * reference, a channel is its own. */
static void follow_channels(Layer &ref, const Layer &source)
{
  ReferenceSettings &rs = ref.reference;
  for_each_layer_channel(ref, source, [&](const char *name, auto &ch, auto &value, const auto &original, auto &built) {
    if (ch.animated()) {
      rs.own_channels.insert(name);
    }
    if (!rs.own_channels.count(name)) {
      value = original;
      /* Not an edit of its own. */
      ch.last = value;
      ch.has_last = true;
    }
    built = value;
  });
}

static void build_one(const Document &doc, Layer &ref)
{
  ReferenceSettings &rs = ref.reference;
  /* What was picked stays picked: points by id, handles by curve point. */
  std::set<uint64_t> picked;
  for (const MeshPoint &p : ref.drawing.points) {
    if (p.selected) {
      picked.insert(p.id);
    }
  }
  VectorDrawing d = reference_build(doc, ref, false);
  for (MeshPoint &p : d.points) {
    p.selected = picked.count(p.id) > 0;
  }
  if (same_curves(d.curves, ref.drawing.curves)) {
    for (size_t c = 0; c < d.curves.size(); c++) {
      for (size_t i = 0; i < d.curves[c].points.size(); i++) {
        d.curves[c].points[i].select_in = ref.drawing.curves[c].points[i].select_in;
        d.curves[c].points[i].select_out = ref.drawing.curves[c].points[i].select_out;
      }
    }
  }
  /* Copied over the drawing it had rather than swapped for it: the points, curves and shapes stay
   * where they are in memory (their lists are refilled in place), so an editor drawing a row of
   * them when an undo step rebuilds the copy isn't left holding freed ones. */
  ref.drawing = d;
  rs.built = true;
  rs.built_ids.clear();
  rs.built_pos.clear();
  rs.built_radius.clear();
  rs.built_weights.clear();
  for (const MeshPoint &p : ref.drawing.points) {
    rs.built_ids.push_back(p.id);
    rs.built_pos.push_back(p.pos);
    rs.built_radius.push_back(p.radius);
    rs.built_weights.push_back(p.weights);
  }
  rs.built_shapes = ref.drawing.shapes;
  rs.built_curves = ref.drawing.curves;
}

void references_build(Document &doc)
{
  visit_layers(doc.layers, [&](Layer &ref) {
    if (!ref.is_reference()) {
      return;
    }
    Layer *source = layer_find(doc, ref.reference_source);
    if (!source) {
      /* Its original is gone: it keeps what it showed, as an ordinary layer. */
      ref.reference_source = 0;
      ref.reference = ReferenceSettings();
      return;
    }
    follow_channels(ref, *source);
    if (ref.is_group() || source->is_group()) {
      return;
    }
    drawing_ensure_ids(source->drawing);
    build_one(doc, ref);
  });
}

/* Whether a point's weights only lost bones that are gone (deleting bones takes their weights off
 * every point, leaving at most an empty entry): not an edit to the copy, which rebuilds from its
 * original. */
static bool only_lost_gone_bones(const std::vector<BoneWeight> &now, const std::vector<BoneWeight> &was,
                                 const Layer *armature)
{
  const auto living = [&](const std::vector<BoneWeight> &weights) {
    std::vector<BoneWeight> kept;
    for (const BoneWeight &w : weights) {
      if (w.weight > 0.0f && armature && bone_find(*armature, w.bone)) {
        kept.push_back(w);
      }
    }
    return kept;
  };
  const std::vector<BoneWeight> kept = living(was);
  return kept.size() != was.size() && living(now) == kept;
}

/* Edits to one reference's drawing since it was built, into its overrides. */
static void capture_one(const Document &doc, Layer &ref)
{
  const Layer *source = layer_find(doc, ref.reference_source);
  ReferenceSettings &rs = ref.reference;
  if (!source || !rs.built) {
    return;
  }
  const VectorDrawing &d = ref.drawing;
  std::string refused;
  bool refused_locked = false;
  const auto refuse = [&](const char *why) {
    if (refused.empty()) {
      refused = why;
      refused_locked = std::string(why).rfind("its points are locked", 0) == 0;
    }
  };

  bool same = d.points.size() == rs.built_ids.size() && d.shapes.size() == rs.built_shapes.size() &&
              same_curves(d.curves, rs.built_curves);
  for (size_t i = 0; same && i < d.points.size(); i++) {
    same = d.points[i].id == rs.built_ids[i];
  }
  for (size_t i = 0; same && i < d.shapes.size(); i++) {
    same = d.shapes[i].id == rs.built_shapes[i].id && d.shapes[i].edges == rs.built_shapes[i].edges;
  }
  if (!same) {
    refuse("a copy's points, curves and shapes come from its original, so they can't be added, deleted "
           "or rearranged here (Break Reference to change its drawing)");
  }
  else {
    for (size_t i = 0; i < d.points.size(); i++) {
      const MeshPoint &p = d.points[i];
      const float2 delta = p.pos - rs.built_pos[i];
      if (std::fabs(delta.x) > 1e-7f || std::fabs(delta.y) > 1e-7f) {
        if (!rs.edit_points) {
          refuse("its points are locked: unlock them in the Reference tab (or the lock in the viewport header) to move them");
        }
        else {
          auto [it, added] = rs.points.try_emplace(p.id);
          if (added) {
            /* Nothing was moved before: a key made now starts from no offset. */
            it->second.offset_keys.last = float2();
            it->second.offset_keys.has_last = true;
          }
          it->second.offset += delta;
        }
      }
      if (p.radius != rs.built_radius[i]) {
        refuse("a copy's line widths follow its original");
      }
      if (p.weights != rs.built_weights[i] &&
          !only_lost_gone_bones(p.weights, rs.built_weights[i],
                                layer_armature(doc, rs.match_original ? source->id : ref.id)))
      {
        if (rs.match_original) {
          refuse("its pose is Same as Original, so it moves with the original's bones (choose Own Bones in "
                 "the Reference tab to bind it yourself)");
        }
        else {
          rs.weights[p.id] = p.weights;
        }
      }
    }
    for (size_t c = 0; c < d.curves.size(); c++) {
      for (size_t i = 0; i < d.curves[c].points.size(); i++) {
        const CurvePoint &now = d.curves[c].points[i], &was = rs.built_curves[c].points[i];
        if (now.curvature != was.curvature) {
          refuse("a copy's curve types follow its original");
        }
        if (now.edge_hidden != was.edge_hidden && int(i) < d.curves[c].segment_count()) {
          /* Shown or hidden on this copy alone; back to the original's, it follows it again. */
          const auto key = edge_key(d, d.curves[c], int(i));
          bool original = false;
          if (c < source->drawing.curves.size() && i < source->drawing.curves[c].points.size()) {
            original = source->drawing.curves[c].points[i].edge_hidden;
          }
          if (now.edge_hidden == original) {
            rs.edges.erase(key);
          }
          else {
            rs.edges[key] = now.edge_hidden;
          }
        }
        /* Handles turned or lengthened (R or S on its points, or dragged): offsets, as points. */
        for (const bool in : {true, false}) {
          const float2 delta = (in ? now.handle_in : now.handle_out) - (in ? was.handle_in : was.handle_out);
          if (std::fabs(delta.x) <= 1e-7f && std::fabs(delta.y) <= 1e-7f) {
            continue;
          }
          if (!rs.edit_points) {
            refuse("its points are locked: unlock them in the Reference tab (or the lock in the viewport header) to change its handles");
            continue;
          }
          auto [it, added] = rs.handles.try_emplace(handle_key(d, d.curves[c], int(i), in));
          if (added) {
            it->second.offset_keys.last = float2();
            it->second.offset_keys.has_last = true;
          }
          it->second.offset += delta;
        }
      }
    }
    for (size_t i = 0; i < d.shapes.size(); i++) {
      const Shape &now = d.shapes[i], &was = rs.built_shapes[i];
      const int si = drawing_shape_index(source->drawing, now.id);
      if (si < 0) {
        continue;
      }
      const Shape &original = source->drawing.shapes[si];
      const Style base = reference_swapped_style(ref, original.style);
      ReferenceLook look = rs.looks.count(now.id) ? rs.looks[now.id] : ReferenceLook();
      bool changed = false;
      if (!same_color(now.style.fill, was.style.fill)) {
        look.fill_set = !same_color(now.style.fill, base.fill);
        look.fill = now.style.fill;
        changed = true;
      }
      if (!same_color(now.style.line, was.style.line)) {
        look.line_set = !same_color(now.style.line, base.line);
        look.line = now.style.line;
        changed = true;
      }
      if (now.style.line_width != was.style.line_width) {
        look.width_set = now.style.line_width != base.line_width;
        look.line_width = now.style.line_width;
        changed = true;
      }
      if (!(now.style.brush == was.style.brush)) {
        look.brush_set = !(now.style.brush == base.brush);
        look.brush = now.style.brush;
        changed = true;
      }
      if (now.has_fill != was.has_fill) {
        look.has_fill_set = now.has_fill != original.has_fill;
        look.has_fill = now.has_fill;
        changed = true;
      }
      if (now.has_outline != was.has_outline) {
        look.has_outline_set = now.has_outline != original.has_outline;
        look.has_outline = now.has_outline;
        changed = true;
      }
      if (now.hidden != was.hidden) {
        look.hidden = now.hidden;
        changed = true;
      }
      if (now.name != was.name) {
        refuse("a copy's shapes are named by its original");
      }
      if (changed) {
        if (look.empty()) {
          rs.looks.erase(now.id);
        }
        else {
          rs.looks[now.id] = look;
        }
      }
    }
  }
  if (!refused.empty()) {
    /* Beside the cursor too: what was just tried isn't allowed on a copy. Locked points say so in
     * a bold line, and where to unlock them. */
    if (refused_locked) {
      report_at_mouse(ReportType::Warning, "\"" + ref.name + "\": " + refused,
                      "Go to the Reference tab in Properties to unlock", "Reference layer points locked");
    }
    else {
      std::string at_mouse = refused;
      at_mouse[0] = char(std::toupper(static_cast<unsigned char>(at_mouse[0])));
      report_at_mouse(ReportType::Warning, "\"" + ref.name + "\": " + refused, at_mouse);
    }
  }
}

void references_capture(Document &doc)
{
  visit_layers(doc.layers, [&](Layer &ref) {
    if (!ref.is_reference()) {
      return;
    }
    if (const Layer *source = layer_find(doc, ref.reference_source)) {
      for_each_layer_channel(ref, *source, [&](const char *name, auto &ch, auto &value, const auto &, auto &built) {
        if (!(value == built) || ch.animated()) {
          ref.reference.own_channels.insert(name);
        }
      });
    }
    if (!ref.is_group()) {
      capture_one(doc, ref);
    }
  });
  references_build(doc);
}

/* The world transform of a layer's parents and its own transform, without a parent bone's. */
static Transform2D placement(const Document &doc, const std::vector<const Layer *> &path)
{
  const Layer &layer = *path.back();
  const Transform2D parents = path.size() > 1 ? layer_world_transform(doc, path[path.size() - 2]->id) : Transform2D();
  return parents * layer.local_transform();
}

Transform2D reference_match_transform(const Document &doc, const Layer &ref)
{
  const Layer *source = layer_find(doc, ref.reference_source);
  std::vector<const Layer *> path;
  if (!source || !layer_path_to(doc.layers, ref.id, path)) {
    return Transform2D();
  }
  /* The top reference: this one, or the outermost referenced group it is in. Its own transform
   * takes the place of its original's; everything below follows the original. */
  size_t top = path.size() - 1;
  while (top > 0 && path[top - 1]->is_reference()) {
    top--;
  }
  std::vector<const Layer *> top_path(path.begin(), path.begin() + top + 1);
  const Layer *source_top = layer_find(doc, path[top]->reference_source);
  std::vector<const Layer *> source_path;
  if (!source_top || !layer_path_to(doc.layers, source_top->id, source_path)) {
    return Transform2D();
  }
  const Transform2D ref_world = layer_world_transform(doc, ref.id);
  return ref_world.inverted() * placement(doc, top_path) * placement(doc, source_path).inverted() *
         layer_world_transform(doc, source->id);
}

void reference_set_match(Document &doc, Layer &ref, bool on)
{
  ReferenceSettings &rs = ref.reference;
  rs.match_original = on;
  if (on || !rs.weights.empty()) {
    return;
  }
  /* Its own weights start as the original's, for its own bone layer's bones of the same names. */
  const Layer *source = layer_find(doc, ref.reference_source);
  const Layer *from = source ? layer_armature(doc, source->id) : nullptr;
  const Layer *to = layer_armature(doc, ref.id);
  if (!source || !from || !to) {
    return;
  }
  for (const MeshPoint &p : source->drawing.points) {
    std::vector<BoneWeight> weights;
    for (const BoneWeight &w : p.weights) {
      const Bone *bone = bone_find(*from, w.bone);
      if (const Bone *own = bone ? bone_find_by_name(*to, bone->name) : nullptr) {
        weights.push_back({own->id, w.weight});
      }
    }
    if (!weights.empty()) {
      rs.weights[p.id] = std::move(weights);
    }
  }
}

bool reference_kind_from_identifier(const std::string &id, ReferenceKind *r_kind)
{
  static const std::pair<const char *, ReferenceKind> KINDS[] = {
      {"ALL", ReferenceKind::All},       {"POINTS", ReferenceKind::Points}, {"LOOKS", ReferenceKind::Looks},
      {"SWAPS", ReferenceKind::Swaps},   {"WEIGHTS", ReferenceKind::Weights}, {"POINT", ReferenceKind::Point},
      {"LOOK", ReferenceKind::Look},     {"SWAP", ReferenceKind::Swap},     {"CHANNELS", ReferenceKind::Channels},
      {"CHANNEL", ReferenceKind::Channel}, {"SELECTED", ReferenceKind::Selected}, {"STYLES", ReferenceKind::Styles}};
  for (const auto &[name, kind] : KINDS) {
    if (id == name) {
      *r_kind = kind;
      return true;
    }
  }
  return false;
}

/* The ids of the points picked on a reference. */
static std::set<uint64_t> picked_points(const Layer &ref)
{
  std::set<uint64_t> ids;
  for (const MeshPoint &p : ref.drawing.points) {
    if (p.selected) {
      ids.insert(p.id);
    }
  }
  return ids;
}

/* How many style settings a look changes (hiding aside). */
static int look_settings_count(const ReferenceLook &l)
{
  return int(l.fill_set) + int(l.line_set) + int(l.width_set) + int(l.brush_set) + int(l.has_fill_set) +
         int(l.has_outline_set);
}

/* Clears one setting of a look ("fill", "line", "line_width", "brush", "has_fill", "has_outline",
 * "hidden"). */
static void clear_look_setting(ReferenceLook &look, const std::string &setting)
{
  if (setting == "fill") {
    look.fill_set = false;
  }
  else if (setting == "line") {
    look.line_set = false;
  }
  else if (setting == "line_width") {
    look.width_set = false;
  }
  else if (setting == "brush") {
    look.brush_set = false;
  }
  else if (setting == "has_fill") {
    look.has_fill_set = false;
  }
  else if (setting == "has_outline") {
    look.has_outline_set = false;
  }
  else if (setting == "hidden") {
    look.hidden = false;
  }
}

int reference_revert(Document &doc, Layer &ref, ReferenceKind kind, uint64_t which, const std::string &detail)
{
  (void)doc;
  ReferenceSettings &rs = ref.reference;
  int gone = 0;
  const bool all = kind == ReferenceKind::All;
  if (all || kind == ReferenceKind::Points) {
    gone += int(rs.points.size() + rs.handles.size());
    rs.points.clear();
    rs.handles.clear();
  }
  if (all || kind == ReferenceKind::Channels) {
    gone += int(rs.own_channels.size());
    for (const std::string &name : std::set<std::string>(rs.own_channels)) {
      reference_sync_channel(ref, name);
    }
  }
  if (kind == ReferenceKind::Channel && rs.own_channels.count(detail)) {
    reference_sync_channel(ref, detail);
    gone++;
  }
  if (all || kind == ReferenceKind::Looks) {
    gone += int(rs.looks.size() + rs.edges.size());
    rs.looks.clear();
    rs.edges.clear();
  }
  if (all || kind == ReferenceKind::Swaps || kind == ReferenceKind::Styles) {
    gone += int(rs.swaps.size());
    rs.swaps.clear();
  }
  if (kind == ReferenceKind::Styles) {
    /* Every look but hiding. */
    for (auto it = rs.looks.begin(); it != rs.looks.end();) {
      const bool hidden = it->second.hidden;
      gone += look_settings_count(it->second) > 0;
      it->second = ReferenceLook();
      it->second.hidden = hidden;
      it = it->second.empty() ? rs.looks.erase(it) : std::next(it);
    }
  }
  if (all || kind == ReferenceKind::Weights) {
    gone += int(rs.weights.size());
    rs.weights.clear();
  }
  if (kind == ReferenceKind::Point || kind == ReferenceKind::Selected) {
    const std::set<uint64_t> ids = kind == ReferenceKind::Point ? std::set<uint64_t>{which} : picked_points(ref);
    gone += int(std::erase_if(rs.points, [&](const auto &e) { return ids.count(e.first) > 0; }));
    gone += int(std::erase_if(rs.handles, [&](const auto &e) { return ids.count(e.first.point) > 0; }));
  }
  if (kind == ReferenceKind::Look) {
    if (detail.empty()) {
      gone += int(rs.looks.erase(which));
    }
    else if (const auto it = rs.looks.find(which); it != rs.looks.end()) {
      clear_look_setting(it->second, detail);
      gone++;
      if (it->second.empty()) {
        rs.looks.erase(it);
      }
    }
  }
  if (kind == ReferenceKind::Swap && which < rs.swaps.size()) {
    rs.swaps.erase(rs.swaps.begin() + std::ptrdiff_t(which));
    gone++;
  }
  return gone;
}

/* Adds an offset to an original's value (a point's place, a handle): to it, and to its keys (a
 * keyed offset keys it at every frame either has a key). */
static void add_offset(float2 &value, Channel<float2> &ch, const ReferencePoint &o)
{
  if (!o.offset_keys.animated()) {
    value += o.offset;
    for (Key<float2> &k : ch.keys) {
      k.value += o.offset;
    }
    ch.last += o.offset;
    return;
  }
  std::set<int> frames{0};
  for (const Key<float2> &k : ch.keys) {
    frames.insert(k.frame);
  }
  for (const Key<float2> &k : o.offset_keys.keys) {
    frames.insert(k.frame);
  }
  std::vector<std::pair<int, float2>> values;
  for (const int f : frames) {
    const float2 base = ch.animated() ? ch.evaluate(f) : value;
    values.push_back({f, base + o.offset_keys.evaluate(f)});
  }
  for (const auto &[f, v] : values) {
    ch.set_key(f, v);
  }
  value += o.offset;
  ch.last = value;
  ch.has_last = true;
}

static void add_offset(MeshPoint &p, const ReferencePoint &o)
{
  add_offset(p.pos, p.pos_keys, o);
}

/* Adds a reference's handle offsets to a drawing's handles (and their keys). `only`: just those of
 * this point (0: all). Returns how many. */
static int add_handle_offsets(VectorDrawing &d, const ReferenceSettings &rs, uint64_t only)
{
  int n = 0;
  for (Curve &curve : d.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      for (const bool in : {true, false}) {
        const ReferenceHandleKey key = handle_key(d, curve, i, in);
        if (only != 0 && key.point != only) {
          continue;
        }
        if (const auto it = rs.handles.find(key); it != rs.handles.end()) {
          add_offset(in ? cp.handle_in : cp.handle_out, in ? cp.handle_in_keys : cp.handle_out_keys, it->second);
          cp.handles_set = true;
          n++;
        }
      }
    }
  }
  return n;
}

void reference_break(Document &doc, Layer &ref)
{
  const Layer *source = layer_find(doc, ref.reference_source);
  if (source && !ref.is_group() && !source->is_group()) {
    /* Built without its offsets, which then go into the points and their keys. */
    Layer plain = ref;
    plain.reference.points.clear();
    plain.reference.handles.clear();
    VectorDrawing d = reference_build(doc, plain, false);
    if (!ref.reference.follow_animation) {
      /* Only the original's setup shape showed: none of its animation. */
      for (MeshPoint &p : d.points) {
        p.pos_keys.keys.clear();
        p.radius_keys.keys.clear();
      }
      for (Curve &curve : d.curves) {
        for (CurvePoint &cp : curve.points) {
          cp.curvature_keys.keys.clear();
          cp.handle_in_keys.keys.clear();
          cp.handle_out_keys.keys.clear();
        }
      }
    }
    add_handle_offsets(d, ref.reference, 0);
    for (MeshPoint &p : d.points) {
      if (const auto it = ref.reference.points.find(p.id); it != ref.reference.points.end()) {
        add_offset(p, it->second);
      }
      p.selected = drawing_point_index(ref.drawing, p.id) >= 0 &&
                   ref.drawing.points[drawing_point_index(ref.drawing, p.id)].selected;
    }

    std::erase_if(d.shapes, [](const Shape &s) { return s.hidden; });
    ref.drawing = std::move(d);
  }
  ref.reference_source = 0;
  ref.reference = ReferenceSettings();
}

int reference_apply(Document &doc, Layer &ref, ReferenceKind kind, uint64_t which, const std::string &detail)
{
  Layer *source = layer_find(doc, ref.reference_source);
  if (!source) {
    return 0;
  }
  ReferenceSettings &rs = ref.reference;
  VectorDrawing &original = source->drawing;
  int applied = 0;
  const bool all = kind == ReferenceKind::All;
  /* Points (their handles first): all, one, or the picked ones. */
  if (all || kind == ReferenceKind::Points || kind == ReferenceKind::Point || kind == ReferenceKind::Selected) {
    const bool some = kind == ReferenceKind::Point || kind == ReferenceKind::Selected;
    const std::set<uint64_t> ids = kind == ReferenceKind::Point ? std::set<uint64_t>{which}
                                   : kind == ReferenceKind::Selected ? picked_points(ref)
                                                                     : std::set<uint64_t>{};
    if (some) {
      for (const uint64_t id : ids) {
        applied += add_handle_offsets(original, rs, id);
      }
    }
    else {
      applied += add_handle_offsets(original, rs, 0);
    }
    std::erase_if(rs.handles, [&](const auto &e) { return !some || ids.count(e.first.point) > 0; });
    for (auto it = rs.points.begin(); it != rs.points.end();) {
      if (some && !ids.count(it->first)) {
        ++it;
        continue;
      }
      if (const int i = drawing_point_index(original, it->first); i >= 0) {
        add_offset(original.points[i], it->second);
        applied++;
      }
      it = rs.points.erase(it);
    }
  }
  /* Its own layer channels go into the original's (keys and all): all, or one. */
  if (all || kind == ReferenceKind::Channels || kind == ReferenceKind::Channel) {
    for_each_layer_channel(ref, *source, [&](const char *name, auto &, auto &, const auto &, auto &) {
      if (!rs.own_channels.count(name) || (kind == ReferenceKind::Channel && detail != name)) {
        return;
      }
      const std::string n = name;
      if (n == "position") {
        source->position = ref.position;
        source->position_keys.keys = ref.position_keys.keys;
        source->position_keys.last = source->position;
      }
      else if (n == "rotation") {
        source->rotation = ref.rotation;
        source->rotation_keys.keys = ref.rotation_keys.keys;
        source->rotation_keys.last = source->rotation;
      }
      else if (n == "scale") {
        source->scale = ref.scale;
        source->scale_keys.keys = ref.scale_keys.keys;
        source->scale_keys.last = source->scale;
      }
      else {
        source->opacity = ref.opacity;
        source->opacity_keys.keys = ref.opacity_keys.keys;
        source->opacity_keys.last = source->opacity;
      }
      reference_sync_channel(ref, n);
      applied++;
    });
  }
  /* Looks: all, one shape's, or one setting of it (hiding stays this reference's). */
  if (all || kind == ReferenceKind::Looks || kind == ReferenceKind::Look || kind == ReferenceKind::Styles) {
    for (auto it = rs.looks.begin(); it != rs.looks.end();) {
      if (kind == ReferenceKind::Look && it->first != which) {
        ++it;
        continue;
      }
      ReferenceLook push = it->second;
      push.hidden = false;
      if (!detail.empty()) {
        /* Just that setting. */
        for (const char *s : {"fill", "line", "line_width", "brush", "has_fill", "has_outline"}) {
          if (detail != s) {
            clear_look_setting(push, s);
          }
        }
      }
      if (const int i = drawing_shape_index(original, it->first); i >= 0 && !push.empty()) {
        apply_look(original.shapes[i], push);
        original.shapes[i].hidden = false;
        applied++;
      }
      for (const char *s : {"fill", "line", "line_width", "brush", "has_fill", "has_outline"}) {
        if (detail.empty() || detail == s) {
          clear_look_setting(it->second, s);
        }
      }
      it = it->second.empty() ? rs.looks.erase(it) : std::next(it);
    }
  }
  /* Colour swaps recolour the original's shapes. */
  for (size_t i = 0; i < rs.swaps.size();) {
    if (all || kind == ReferenceKind::Swaps || kind == ReferenceKind::Styles ||
        (kind == ReferenceKind::Swap && i == which)) {
      const ReferenceSwap sw = rs.swaps[i];
      for (Shape &shape : original.shapes) {
        if (sw.fill && same_color(shape.style.fill, sw.from)) {
          shape.style.fill = sw.to;
        }
        if (sw.line && same_color(shape.style.line, sw.from)) {
          shape.style.line = sw.to;
        }
      }
      rs.swaps.erase(rs.swaps.begin() + std::ptrdiff_t(i));
      applied++;
      if (kind == ReferenceKind::Swap) {
        break;
      }
    }
    else {
      i++;
    }
  }
  return applied;
}

/* How many changes of its own a reference has. */
int reference_change_count(const Layer &ref)
{
  const ReferenceSettings &rs = ref.reference;
  return int(rs.points.size() + rs.handles.size() + rs.looks.size() + rs.edges.size() + rs.swaps.size() +
             rs.own_channels.size() +
             (rs.weights.empty() ? 0 : 1));
}

int reference_remove_unused(Document &doc, Layer &ref)
{
  const Layer *source = layer_find(doc, ref.reference_source);
  if (!source) {
    return 0;
  }
  ReferenceSettings &rs = ref.reference;
  int gone = 0;
  std::erase_if(rs.points, [&](const auto &e) {
    const bool unused = drawing_point_index(source->drawing, e.first) < 0;
    gone += unused;
    return unused;
  });
  std::erase_if(rs.weights, [&](const auto &e) {
    const bool unused = drawing_point_index(source->drawing, e.first) < 0;
    gone += unused;
    return unused;
  });
  std::erase_if(rs.handles, [&](const auto &e) {
    const bool unused = drawing_point_index(source->drawing, e.first.point) < 0 ||
                        (e.first.toward != 0 && drawing_point_index(source->drawing, e.first.toward) < 0);
    gone += unused;
    return unused;
  });
  std::erase_if(rs.looks, [&](const auto &e) {
    const bool unused = drawing_shape_index(source->drawing, e.first) < 0;
    gone += unused;
    return unused;
  });
  std::erase_if(rs.edges, [&](const auto &e) {
    const bool unused = drawing_point_index(source->drawing, e.first.first) < 0 ||
                        drawing_point_index(source->drawing, e.first.second) < 0;
    gone += unused;
    return unused;
  });
  return gone;
}

void reference_sync_channel(Layer &ref, const std::string &name)
{
  ReferenceSettings &rs = ref.reference;
  rs.own_channels.erase(name);
  /* Its keys go: the original's animation shows again (on the next build). */
  if (name == "position") {
    ref.position_keys.keys.clear();
  }
  else if (name == "rotation") {
    ref.rotation_keys.keys.clear();
  }
  else if (name == "scale") {
    ref.scale_keys.keys.clear();
  }
  else if (name == "opacity") {
    ref.opacity_keys.keys.clear();
  }
}

void reference_hide_shape(Layer &ref, uint64_t shape_id, bool hide)
{
  ReferenceLook &look = ref.reference.looks[shape_id];
  look.hidden = hide;
  if (look.empty()) {
    ref.reference.looks.erase(shape_id);
  }
}

void reference_swap_color(Layer &ref, Color from, Color to, bool fill, bool line)
{
  std::vector<ReferenceSwap> &swaps = ref.reference.swaps;
  std::erase_if(swaps, [&](const ReferenceSwap &s) { return same_color(s.from, from) && s.fill == fill && s.line == line; });
  if (!same_color(from, to)) {
    swaps.push_back({from, to, fill, line});
  }
}

}  // namespace sky
