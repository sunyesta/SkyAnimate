#include "data/layer_mirror.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>

#include "data/bones.hh"

namespace oanim {

Layer *layer_mirror_partner(Document &doc, uint64_t id)
{
  const Layer *layer = layer_find(doc, id);
  if (!layer || !layer->has_drawing()) {
    return nullptr;
  }
  const std::string other = bone_mirror_name(layer->name);
  if (other == layer->name) {
    return nullptr;
  }
  int index = 0;
  if (std::vector<Layer> *siblings = layer_container(doc, id, &index)) {
    for (Layer &l : *siblings) {
      if (l.name == other && l.id != id && l.has_drawing()) {
        return &l;
      }
    }
  }
  for (const Layer *flat : document_layers_flat(doc)) {
    if (flat->name == other && flat->id != id && flat->has_drawing()) {
      return layer_find(doc, flat->id);
    }
  }
  return nullptr;
}

/* `base` named for a side the way `group` is (its suffix, prefix or Left / Right word); `base` itself
 * when the group's name has no side. */
static std::string sided_like(const std::string &base, const std::string &group)
{
  static const char *const suffixes[] = {"_L", "_R", ".L", ".R", "-L", "-R", " L", " R",
                                         "_l", "_r", ".l", ".r", "-l", "-r", " l", " r"};
  for (const char *t : suffixes) {
    const std::string s = t;
    if (group.size() > s.size() && group.compare(group.size() - s.size(), s.size(), s) == 0) {
      return base + s;
    }
  }
  static const char *const prefixes[] = {"L_", "R_", "L.", "R.", "L-", "R-", "L ", "R ",
                                         "l_", "r_", "l.", "r.", "l-", "r-", "l ", "r "};
  for (const char *t : prefixes) {
    const std::string s = t;
    if (group.size() > s.size() && group.compare(0, s.size(), s) == 0) {
      return s + base;
    }
  }
  for (const char *w : {"Left", "Right", "left", "right", "LEFT", "RIGHT"}) {
    if (group.find(w) != std::string::npos) {
      return base + " " + w;
    }
  }
  return base;
}

Layer &document_add_layer_mirrored(Document &doc, LayerType type, bool mirror_x)
{
  const uint64_t id = document_add_layer(doc, type, "").id;
  Layer *group = layer_parent(doc, id);
  if (!mirror_x || !group) {
    return *layer_find(doc, id);
  }
  const std::string other_group = bone_mirror_name(group->name);
  Layer *partner_group = nullptr;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (flat->name == other_group && flat->id != group->id && flat->is_group()) {
      partner_group = layer_find(doc, flat->id);
      break;
    }
  }
  if (other_group == group->name || !partner_group) {
    return *layer_find(doc, id);
  }
  /* Numbered so neither side's name is taken (Layer 2_L when Layer 1_L and _R are). */
  const auto taken = [&](const std::string &n) {
    for (const Layer *flat : document_layers_flat(doc)) {
      if (flat->name == n && flat->id != id) {
        return true;
      }
    }
    return false;
  };
  const std::string word = type == LayerType::Group ? "Group" : "Layer";
  if (sided_like(word, group->name) == word) {
    return *layer_find(doc, id); /* A side its name gives no way of writing (Arm_L.001): just the one. */
  }
  std::string name;
  for (int i = 1;; i++) {
    name = sided_like(word + " " + std::to_string(i), group->name);
    if (!taken(name) && !taken(bone_mirror_name(name))) {
      break;
    }
  }
  Layer &added = *layer_find(doc, id);
  added.name = name;
  Layer twin;
  twin.id = doc.next_layer_id++;
  twin.type = type;
  twin.name = bone_mirror_name(name);
  int index = 0;
  for (int i = 0; i < int(group->children.size()); i++) {
    if (group->children[i].id == id) {
      index = i;
    }
  }
  std::vector<Layer> &into = partner_group->children;
  into.insert(into.begin() + std::min(index, int(into.size())), std::move(twin));
  doc.active_layer = id;
  return *layer_find(doc, id);
}

void document_mirror_layer_names(Document &doc, const Document &previous)
{
  const std::vector<const Layer *> flat = document_layers_flat(doc);
  const std::vector<const Layer *> flat_before = document_layers_flat(previous);
  const auto taken = [&](const std::string &name) {
    for (const Layer *l : document_layers_flat(doc)) {
      if (l->name == name) {
        return true;
      }
    }
    return false;
  };
  std::vector<std::pair<uint64_t, std::string>> renames;
  for (const Layer *l : flat) {
    const Layer *was = layer_find(previous, l->id);
    if (!was || was->name == l->name) {
      continue;
    }
    const std::string old_other = bone_mirror_name(was->name), new_other = bone_mirror_name(l->name);
    if (old_other == was->name || new_other == l->name) {
      continue;
    }
    /* The old partner: the old other side's name then, unchanged now. */
    for (const Layer *p : flat_before) {
      if (p->name != old_other || p->id == l->id) {
        continue;
      }
      const Layer *now = layer_find(doc, p->id);
      if (now && now->name == p->name && !taken(new_other)) {
        renames.push_back({p->id, new_other});
      }
      break;
    }
  }
  for (const auto &[id, name] : renames) {
    if (Layer *l = layer_find(doc, id)) {
      l->name = name;
    }
  }
}

static std::vector<uint64_t> child_ids(const Layer *group)
{
  std::vector<uint64_t> ids;
  if (group) {
    for (const Layer &l : group->children) {
      ids.push_back(l.id);
    }
  }
  return ids;
}

void document_mirror_layer_order(Document &doc, const Document &previous)
{
  std::vector<uint64_t> groups;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (flat->is_group() && bone_mirror_name(flat->name) != flat->name) {
      groups.push_back(flat->id);
    }
  }
  for (const uint64_t id : groups) {
    Layer *group = layer_find(doc, id);
    const std::string other = bone_mirror_name(group->name);
    Layer *partner = nullptr;
    for (const Layer *flat : document_layers_flat(doc)) {
      if (flat->name == other && flat->is_group() && flat->id != id) {
        partner = layer_find(doc, flat->id);
        break;
      }
    }
    if (!partner) {
      continue;
    }
    const std::vector<uint64_t> now = child_ids(group), before = child_ids(layer_find(previous, id));
    const std::vector<uint64_t> partner_now = child_ids(partner), partner_before = child_ids(layer_find(previous, partner->id));
    /* Only a reorder (the same layers), and only on this side. */
    std::vector<uint64_t> a = now, b = before;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (now == before || a != b || partner_now != partner_before) {
      continue;
    }
    /* Each partner layer with a counterpart here, by where that counterpart now is. */
    std::vector<std::pair<int, int>> ranked; /* (counterpart's place, partner index) */
    for (int j = 0; j < int(partner->children.size()); j++) {
      const std::string name = bone_mirror_name(partner->children[j].name);
      for (int i = 0; i < int(group->children.size()); i++) {
        if (group->children[i].name == name && name != partner->children[j].name) {
          ranked.push_back({i, j});
          break;
        }
      }
    }
    if (ranked.size() < 2) {
      continue;
    }
    std::vector<int> slots;
    for (const auto &[place, j] : ranked) {
      slots.push_back(j);
    }
    std::sort(ranked.begin(), ranked.end());
    std::vector<Layer> children = partner->children;
    for (size_t k = 0; k < ranked.size(); k++) {
      children[slots[k]] = partner->children[ranked[k].second];
    }
    partner->children = std::move(children);
  }
}

bool layer_mirror_x_self(Document &doc, uint64_t id, bool mirror_x)
{
  return mirror_x && !layer_mirror_partner(doc, id);
}

/* Layer space in the setup pose -> the space the pair mirrors across: the bone layer's, inside one,
 * else the document's. */
static Transform2D mirror_space(Document &doc, uint64_t id)
{
  Transform2D axis;
  for (const Layer *p = layer_parent(doc, id); p; p = layer_parent(doc, p->id)) {
    if (p->is_bone_layer()) {
      axis = layer_setup_world_transform(doc, p->id);
      break;
    }
  }
  return axis.inverted() * layer_setup_world_transform(doc, id);
}

PartnerMirror layer_mirror_partners_begin(Document &doc, const std::vector<uint64_t> &ids, bool mirror_x)
{
  PartnerMirror m;
  if (!mirror_x) {
    return m;
  }
  m.editing = ids;
  for (const uint64_t id : ids) {
    const Layer *layer = layer_find(doc, id);
    if (layer && layer->type == LayerType::Vector && layer_mirror_partner(doc, id)) {
      m.before.push_back({id, layer->drawing});
    }
  }
  return m;
}

/* A point's neighbours along each curve it is on: (curve index, place, before, after). */
struct Slot {
  int curve, index, prev, next;
};

static std::vector<Slot> slots_of(const VectorDrawing &d, int point)
{
  std::vector<Slot> slots;
  for (int c = 0; c < int(d.curves.size()); c++) {
    const Curve &curve = d.curves[c];
    const int n = int(curve.points.size());
    for (int i = 0; i < n; i++) {
      if (curve.points[i].point != point) {
        continue;
      }
      const auto at = [&](int k) {
        if (!curve.closed && (k < 0 || k >= n)) {
          return -1;
        }
        return curve.points[(k % n + n) % n].point;
      };
      slots.push_back({c, i, at(i - 1), at(i + 1)});
    }
  }
  return slots;
}

Transform2D layer_mirror_to_partner(Document &doc, uint64_t id, uint64_t partner)
{
  const Transform2D flip{-1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  return mirror_space(doc, partner).inverted() * flip * mirror_space(doc, id);
}

/* How near a reflected setup point must land on a partner's: 2% of the partner's size. */
static float partner_tolerance(const VectorDrawing &dst)
{
  float2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
  for (const MeshPoint &p : dst.points) {
    const float2 s = point_setup_pos(p);
    lo = float2(std::min(lo.x, s.x), std::min(lo.y, s.y));
    hi = float2(std::max(hi.x, s.x), std::max(hi.y, s.y));
  }
  return std::max(1e-3f, 0.02f * distance(lo, hi));
}

static int partner_image(const VectorDrawing &src, int point, const VectorDrawing &dst, const Transform2D &to_partner,
                         float tolerance)
{
  const float2 target = to_partner.apply(point_setup_pos(src.points[point]));
  int best = -1;
  float best_d = tolerance;
  for (int j = 0; j < int(dst.points.size()); j++) {
    const float dd = distance(point_setup_pos(dst.points[j]), target);
    if (dd <= best_d) {
      best = j;
      best_d = dd;
    }
  }
  return best;
}

std::vector<int> layer_mirror_images(Document &doc, const Layer &layer, const Layer &partner,
                                     const std::vector<int> &points)
{
  const Transform2D to_partner = layer_mirror_to_partner(doc, layer.id, partner.id);
  const float tolerance = partner_tolerance(partner.drawing);
  std::vector<int> images;
  for (const int p : points) {
    images.push_back(partner_image(layer.drawing, p, partner.drawing, to_partner, tolerance));
  }
  return images;
}

int layer_mirror_partners_end(Document &doc, const PartnerMirror &mirror)
{
  int changed = 0;
  for (const auto &[id, before] : mirror.before) {
    const Layer *layer = layer_find(doc, id);
    Layer *partner = layer_mirror_partner(doc, id);
    if (!layer || !partner || partner->type != LayerType::Vector ||
        (partner->is_reference() && !partner->reference.edit_points))
    {
      continue;
    }
    const VectorDrawing &now = layer->drawing;
    VectorDrawing &dst = partner->drawing;
    const bool partner_edited = std::find(mirror.editing.begin(), mirror.editing.end(), partner->id) !=
                                mirror.editing.end();
    if (now.points.size() != before.points.size() || dst.points.empty()) {
      continue;
    }
    const Transform2D to_partner = layer_mirror_to_partner(doc, id, partner->id);
    const float tolerance = partner_tolerance(dst);
    const auto image = [&](int src) { return partner_image(before, src, dst, to_partner, tolerance); };
    /* What the edit changed: a point's place, or a handle of it. */
    std::vector<bool> moved(now.points.size(), false);
    for (size_t i = 0; i < now.points.size(); i++) {
      moved[i] = now.points[i].pos != before.points[i].pos;
    }
    for (size_t c = 0; c < now.curves.size() && c < before.curves.size(); c++) {
      for (size_t i = 0; i < now.curves[c].points.size() && i < before.curves[c].points.size(); i++) {
        const CurvePoint &a = now.curves[c].points[i], &b = before.curves[c].points[i];
        if (a.handle_in != b.handle_in || a.handle_out != b.handle_out || a.aligned != b.aligned) {
          moved[a.point] = true;
        }
      }
    }
    for (int src = 0; src < int(now.points.size()); src++) {
      if (!moved[src]) {
        continue;
      }
      const int j = image(src);
      /* A selected image in a partner being edited too moves by itself, unless this point is on the
       * +x side: then the image follows it, reflected (the -x side follows, as in Blender). */
      if (j < 0 || (partner_edited && dst.points[j].selected &&
                    !(mirror_space(doc, id).apply(point_setup_pos(before.points[src])).x > 0.0f)))
      {
        continue;
      }
      dst.points[j].pos = to_partner.apply(now.points[src].pos);
      changed++;
      /* The handles, reflected: in and out swap where the partner's curve runs the other way (its
       * point before is the image of this one's point after). */
      for (const Slot &from : slots_of(now, src)) {
        for (const Slot &to : slots_of(dst, j)) {
          const auto is_image = [&](int mine, int theirs) { return mine >= 0 && theirs >= 0 && image(mine) == theirs; };
          bool swap;
          if (is_image(from.prev, to.prev) || is_image(from.next, to.next)) {
            swap = false;
          }
          else if (is_image(from.prev, to.next) || is_image(from.next, to.prev)) {
            swap = true;
          }
          else {
            continue;
          }
          const Curve &curve = now.curves[from.curve];
          const CurvePoint &a = curve.points[from.index];
          CurvePoint &b = dst.curves[to.curve].points[to.index];
          /* As they show (worked out from the neighbours while not set yet), stored from now on. */
          const float2 here = now.points[src].pos;
          const float2 in = curve_handle(now, curve, from.index, true) - here;
          const float2 out = curve_handle(now, curve, from.index, false) - here;
          b.handle_in = to_partner.apply_vector(swap ? out : in);
          b.handle_out = to_partner.apply_vector(swap ? in : out);
          b.handles_set = true;
          b.aligned = a.aligned;
          b.curvature = a.curvature;
        }
      }
    }
  }
  return changed;
}


SymmetrizeResult layer_symmetrize_with_partner(Document &doc, Layer &layer, Layer &partner, bool right_to_left,
                                               float threshold, bool setup_frame,
                                               const std::map<uint64_t, uint64_t> *weight_flip,
                                               std::vector<int> *r_unmatched)
{
  /* Both drawings as one, in the space the pair mirrors across, so the in-layer Symmetrize does it. */
  const Transform2D to_axis[2] = {mirror_space(doc, layer.id), mirror_space(doc, partner.id)};
  const VectorDrawing *parts[2] = {&layer.drawing, &partner.drawing};
  const int n = int(layer.drawing.points.size());
  VectorDrawing both;
  for (int k = 0; k < 2; k++) {
    const Transform2D &t = to_axis[k];
    for (MeshPoint p : parts[k]->points) {
      p.pos = t.apply(p.pos);
      p.setup_pos = t.apply(p.setup_pos);
      p.selected = k == 0 && p.selected && !p.detail.hidden;
      both.points.push_back(std::move(p));
    }
    for (Curve curve : parts[k]->curves) {
      for (CurvePoint &cp : curve.points) {
        cp.point += k == 0 ? 0 : n;
        cp.handle_in = t.apply_vector(cp.handle_in);
        cp.handle_out = t.apply_vector(cp.handle_out);
      }
      both.curves.push_back(std::move(curve));
    }
  }
  /* The partner's points that take part: the nearest to each selected point's reflection, one each. */
  struct Candidate {
    float d;
    int a, b;
  };
  std::vector<Candidate> candidates;
  const float thr = std::max(threshold, 0.0f);
  for (int a = 0; a < n; a++) {
    const float2 here = point_setup_pos(both.points[a]);
    if (!both.points[a].selected || std::fabs(here.x) <= thr) {
      continue;
    }
    for (int b = n; b < int(both.points.size()); b++) {
      if (!both.points[b].detail.hidden) {
        candidates.push_back({distance(float2(-here.x, here.y), point_setup_pos(both.points[b])), a, b});
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &x, const Candidate &y) { return x.d < y.d; });
  std::vector<bool> taken(both.points.size(), false);
  for (const Candidate &c : candidates) {
    if (!taken[c.a] && !taken[c.b]) {
      taken[c.a] = taken[c.b] = true;
      both.points[c.b].selected = true;
    }
  }
  const VectorDrawing was = both;
  SymmetrizeResult result = drawing_symmetrize(both, right_to_left, threshold, setup_frame, weight_flip);
  if (!result.done) {
    if (r_unmatched) {
      for (const int i : result.unmatched) {
        if (i < n) {
          r_unmatched->push_back(i);
        }
      }
    }
    return result;
  }
  /* The looks of the copied side's shapes, onto their images on the other. */
  std::map<int, int> to_partner, to_layer;
  for (const auto &[src, dst] : result.copied) {
    if (src < n && dst >= n) {
      to_partner[src] = dst - n;
    }
    else if (src >= n && dst < n) {
      to_layer[src - n] = dst;
    }
  }
  /* Back into each layer: what changed. */
  for (int k = 0; k < 2; k++) {
    VectorDrawing &d = k == 0 ? layer.drawing : partner.drawing;
    const Transform2D back = to_axis[k].inverted();
    const int base = k == 0 ? 0 : n;
    for (int i = 0; i < int(d.points.size()); i++) {
      const MeshPoint &now = both.points[base + i], &before = was.points[base + i];
      if (now.pos != before.pos) {
        d.points[i].pos = back.apply(now.pos);
      }
      d.points[i].radius = now.radius;
      d.points[i].weights = now.weights;
    }
    const int c0 = k == 0 ? 0 : int(layer.drawing.curves.size());
    for (int c = 0; c < int(d.curves.size()); c++) {
      for (int i = 0; i < int(d.curves[c].points.size()); i++) {
        const CurvePoint &now = both.curves[c0 + c].points[i], &before = was.curves[c0 + c].points[i];
        CurvePoint &to = d.curves[c].points[i];
        if (now.handle_in != before.handle_in || now.handle_out != before.handle_out ||
            now.handles_set != before.handles_set)
        {
          to.handle_in = back.apply_vector(now.handle_in);
          to.handle_out = back.apply_vector(now.handle_out);
          to.handles_set = now.handles_set;
        }
        to.weight_in = now.weight_in;
        to.weight_out = now.weight_out;
        to.offset_in = now.offset_in;
        to.offset_out = now.offset_out;
        to.curvature = now.curvature;
        to.aligned = now.aligned;
      }
    }
  }
  drawing_copy_shape_looks(layer.drawing, partner.drawing, to_partner);
  drawing_copy_shape_looks(partner.drawing, layer.drawing, to_layer);
  return result;
}


bool layer_mirror_shape(Document &doc, Layer &layer, int shape, Layer **r_layer, int *r_shape)
{
  if (layer.type != LayerType::Vector || shape < 0 || shape >= int(layer.drawing.shapes.size())) {
    return false;
  }
  const std::vector<int> points = shape_points(layer.drawing, layer.drawing.shapes[shape]);
  if (points.empty()) {
    return false;
  }
  Layer *other = layer_mirror_partner(doc, layer.id);
  std::vector<int> images;
  if (other) {
    if (other->type != LayerType::Vector) {
      return false;
    }
    images = layer_mirror_images(doc, layer, *other, points);
  }
  else {
    other = &layer;
    for (const int p : points) {
      images.push_back(drawing_find_mirror_point(layer.drawing, p, true, false));
    }
  }
  if (std::find(images.begin(), images.end(), -1) != images.end()) {
    return false;
  }
  std::sort(images.begin(), images.end());
  images.erase(std::unique(images.begin(), images.end()), images.end());
  for (int s = 0; s < int(other->drawing.shapes.size()); s++) {
    if (other == &layer && s == shape) {
      continue;
    }
    std::vector<int> mine = shape_points(other->drawing, other->drawing.shapes[s]);
    std::sort(mine.begin(), mine.end());
    mine.erase(std::unique(mine.begin(), mine.end()), mine.end());
    if (mine == images) {
      *r_layer = other;
      *r_shape = s;
      return true;
    }
  }
  return false;
}

static bool same_look(const Shape &a, const Shape &b)
{
  return a.has_fill == b.has_fill && a.has_outline == b.has_outline && a.style == b.style &&
         a.material == b.material;
}

/* The same shape in another state of the layer: by id, or by place while it has none yet. */
static const Shape *shape_before(const Layer *layer, const Shape &shape, int index)
{
  if (!layer) {
    return nullptr;
  }
  if (shape.id == 0) {
    return index < int(layer->drawing.shapes.size()) && layer->drawing.shapes[index].id == 0 ?
               &layer->drawing.shapes[index] :
               nullptr;
  }
  for (const Shape &s : layer->drawing.shapes) {
    if (s.id == shape.id) {
      return &s;
    }
  }
  return nullptr;
}

void document_mirror_looks(Document &doc, const Document &previous)
{
  std::vector<uint64_t> ids;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (flat->type == LayerType::Vector && !flat->drawing.shapes.empty()) {
      ids.push_back(flat->id);
    }
  }
  for (const uint64_t id : ids) {
    Layer *layer = layer_find(doc, id);
    const Layer *was = layer_find(previous, id);
    if (!layer || !was) {
      continue;
    }
    for (int s = 0; s < int(layer->drawing.shapes.size()); s++) {
      const Shape &shape = layer->drawing.shapes[s];
      const Shape *before = shape_before(was, shape, s);
      if (!before || same_look(shape, *before)) {
        continue;
      }
      Layer *other = nullptr;
      int image = -1;
      if (!layer_mirror_shape(doc, *layer, s, &other, &image)) {
        continue;
      }
      Shape &to = other->drawing.shapes[image];
      const Shape *to_before = shape_before(layer_find(previous, other->id), to, image);
      if (to_before && !same_look(to, *to_before)) {
        continue; /* Changed too (both sides edited): it keeps its own. */
      }
      to.has_fill = shape.has_fill;
      to.has_outline = shape.has_outline;
      to.style = shape.style;
      to.material = shape.material;
      to.material_last = shape.material_last;
    }
  }
}
/* --- New geometry ------------------------------------------------------------------------------ */

namespace {

/* Where a layer's new geometry is mirrored to: its partner, or itself across its own axis. */
struct MirrorTarget {
  Layer *layer = nullptr;
  Transform2D to;
  bool self = false;
};

float place_eps(float2 p)
{
  return 1e-4f + 1e-4f * (std::fabs(p.x) + std::fabs(p.y));
}

int point_at(const VectorDrawing &d, float2 pos)
{
  for (int i = 0; i < int(d.points.size()); i++) {
    if (distance(d.points[i].pos, pos) <= place_eps(pos)) {
      return i;
    }
  }
  return -1;
}

/* The segment joining points a and b in some curve (`r_reversed`: it runs from b to a). */
bool segment_between(const VectorDrawing &d, int a, int b, ShapeEdge *r_edge, bool *r_reversed)
{
  for (int c = 0; c < int(d.curves.size()); c++) {
    for (int s = 0; s < d.curves[c].segment_count(); s++) {
      int f, t;
      curve_segment_points(d.curves[c], s, &f, &t);
      if ((f == a && t == b) || (f == b && t == a)) {
        *r_edge = {c, s};
        *r_reversed = f == b;
        return true;
      }
    }
  }
  return false;
}

/* The handles of point `i` of a curve as they show, as offsets. */
std::pair<float2, float2> shown_handles(const VectorDrawing &d, const Curve &curve, int i)
{
  const float2 here = d.points[curve.points[i].point].pos;
  return {curve_handle(d, curve, i, true) - here, curve_handle(d, curve, i, false) - here};
}

MeshPoint image_point(const MeshPoint &p, const Transform2D &to)
{
  MeshPoint q = p;
  q.pos = to.apply(p.pos);
  q.setup_pos = to.apply(p.setup_pos);
  q.selected = false;
  q.id = 0;
  /* Weighted afresh (automatic weights) where it lands. */
  q.weights.clear();
  return q;
}

}  // namespace

/* An old point's mirror image in the target (-1 when it has none). */
static int old_image(Document &doc, Layer &layer, const MirrorTarget &target, int point)
{
  if (target.self) {
    return drawing_find_mirror_point(layer.drawing, point, true, false);
  }
  return layer_mirror_images(doc, layer, *target.layer, {point})[0];
}

static void mirror_new_geometry(Document &doc, Layer &layer, const Layer &was, const MirrorTarget &target)
{
  VectorDrawing &src = layer.drawing;
  const int n0 = int(was.drawing.points.size());
  const int s0 = int(was.drawing.shapes.size());
  const bool closed_one = [&] {
    for (int c = 0; c < int(src.curves.size()) && c < int(was.drawing.curves.size()); c++) {
      if (src.curves[c].closed && !was.drawing.curves[c].closed) {
        return true;
      }
    }
    return false;
  }();
  if (int(src.points.size()) <= n0 && int(src.shapes.size()) <= s0 && !closed_one) {
    return;
  }
  /* Only a step that added to what was there (the old points still first, in order): one that
   * rebuilt the drawing (a Boolean applied, levels rebuilt) isn't drawing new geometry. */
  for (int i = 0; i < n0; i++) {
    const uint64_t a = was.drawing.points[i].id, b = i < int(src.points.size()) ? src.points[i].id : 0;
    if (i >= int(src.points.size()) || (a != 0 && b != 0 && a != b)) {
      return;
    }
  }
  /* Work from a copy of this layer as it is, as the target may be the layer itself. */
  const VectorDrawing from = src;
  const auto is_new = [&](int p) { return p >= n0; };
  /* (a) Curves of new points only: a reflected copy, unless their image is there already. */
  std::map<int, int> curve_image;
  for (int c = 0; c < int(from.curves.size()); c++) {
    const Curve &curve = from.curves[c];
    if (curve.points.empty() ||
        !std::all_of(curve.points.begin(), curve.points.end(), [&](const CurvePoint &cp) { return is_new(cp.point); }))
    {
      continue;
    }
    VectorDrawing &dst = target.layer->drawing;
    bool there = true;
    for (const CurvePoint &cp : curve.points) {
      there = there && point_at(dst, target.to.apply(from.points[cp.point].pos)) >= 0;
    }
    if (there) {
      continue;
    }
    std::map<int, int> point_image;
    Curve copy = curve;
    for (int i = 0; i < int(curve.points.size()); i++) {
      const int p = curve.points[i].point;
      if (!point_image.count(p)) {
        const float2 at = target.to.apply(from.points[p].pos);
        int q = point_at(dst, at);
        if (q < 0) {
          dst.points.push_back(image_point(from.points[p], target.to));
          q = int(dst.points.size()) - 1;
        }
        point_image[p] = q;
      }
      CurvePoint &cp = copy.points[i];
      cp.point = point_image[p];
      const auto [in, out] = shown_handles(from, curve, i);
      cp.handle_in = target.to.apply_vector(in);
      cp.handle_out = target.to.apply_vector(out);
      cp.handles_set = true;
      cp.select_in = cp.select_out = false;
    }
    dst.curves.push_back(std::move(copy));
    curve_image[c] = int(dst.curves.size()) - 1;
  }
  std::vector<bool> shape_done(from.shapes.size(), false);
  for (int s = s0; s < int(from.shapes.size()); s++) {
    const Shape &shape = from.shapes[s];
    if (shape.edges.empty() || !std::all_of(shape.edges.begin(), shape.edges.end(),
                                            [&](const ShapeEdge &e) { return curve_image.count(e.curve) > 0; }))
    {
      continue;
    }
    Shape copy = shape;
    copy.id = 0;
    for (ShapeEdge &e : copy.edges) {
      e.curve = curve_image[e.curve];
    }
    target.layer->drawing.shapes.push_back(std::move(copy));
    shape_done[s] = true;
  }

  /* (b) New points in curves that were there: a knife's cut (one point between two old ones) cuts
   * the image segment at the reflected spot; points added at an open end (Extrude, the pen going on)
   * extend the image curve's end. */
  for (int c = 0; c < int(from.curves.size()); c++) {
    const Curve &curve = from.curves[c];
    const int m = int(curve.points.size());
    if (curve_image.count(c) || std::none_of(curve.points.begin(), curve.points.end(),
                                             [&](const CurvePoint &cp) { return !is_new(cp.point); }))
    {
      continue;
    }
    for (int i = 0; i < m; i++) {
      const int p = curve.points[i].point;
      /* Detail points (the knife at a detail level) stay on their own side. */
      if (!is_new(p) || from.points[p].detail.level > 0) {
        continue;
      }
      VectorDrawing &dst = target.layer->drawing;
      const float2 at = target.to.apply(from.points[p].pos);
      if (point_at(dst, at) >= 0) {
        continue; /* Mirrored already. */
      }
      const int prev_i = i > 0 ? i - 1 : (curve.closed ? m - 1 : -1);
      const int next_i = i + 1 < m ? i + 1 : (curve.closed ? 0 : -1);
      const int prev = prev_i >= 0 ? curve.points[prev_i].point : -1;
      const int next = next_i >= 0 ? curve.points[next_i].point : -1;
      const auto [in, out] = shown_handles(from, curve, i);
      if (prev >= 0 && next >= 0 && !is_new(prev) && !is_new(next)) {
        const int a = old_image(doc, layer, target, prev), b = old_image(doc, layer, target, next);
        ShapeEdge edge;
        bool reversed = false;
        if (a < 0 || b < 0 || !segment_between(dst, a, b, &edge, &reversed)) {
          continue;
        }
        /* Where along the image segment the reflected point is nearest. */
        float2 ctrl[4];
        curve_segment_bezier(dst, dst.curves[edge.curve], edge.segment, ctrl);
        float best_t = 0.5f, best_d = FLT_MAX;
        for (int k = 1; k < 256; k++) {
          const float t = float(k) / 256.0f;
          const float dd = distance(bezier_eval(ctrl, t), at);
          if (dd < best_d) {
            best_d = dd;
            best_t = t;
          }
        }
        const int q = drawing_split_segment(dst, edge, best_t);
        dst.points[q].pos = at;
        dst.points[q].setup_pos = target.to.apply(from.points[p].setup_pos);
        /* Its handles, and the facing handles of its neighbours, as the cut left them on this side. */
        for (Curve &curve_t : dst.curves) {
          for (int j = 0; j < int(curve_t.points.size()); j++) {
            CurvePoint &cp = curve_t.points[j];
            if (cp.point == q) {
              cp.handle_in = target.to.apply_vector(reversed ? out : in);
              cp.handle_out = target.to.apply_vector(reversed ? in : out);
            }
          }
        }
        const auto [prev_in, prev_out] = shown_handles(from, curve, prev_i);
        const auto [next_in, next_out] = shown_handles(from, curve, next_i);
        for (Curve &curve_t : dst.curves) {
          const int mt = int(curve_t.points.size());
          for (int j = 0; j < mt; j++) {
            CurvePoint &cp = curve_t.points[j];
            const int before = j > 0 ? curve_t.points[j - 1].point : (curve_t.closed ? curve_t.points[mt - 1].point : -1);
            const int after = j + 1 < mt ? curve_t.points[j + 1].point : (curve_t.closed ? curve_t.points[0].point : -1);
            if (cp.point == a && after == q) {
              cp.handle_out = target.to.apply_vector(prev_out);
            }
            if (cp.point == a && before == q) {
              cp.handle_in = target.to.apply_vector(prev_out);
            }
            if (cp.point == b && before == q) {
              cp.handle_in = target.to.apply_vector(next_in);
            }
            if (cp.point == b && after == q) {
              cp.handle_out = target.to.apply_vector(next_in);
            }
          }
        }
        continue;
      }
      /* At an open end: the old point it grows from. */
      const bool after_old = prev >= 0 && !is_new(prev) && next < 0;
      const bool before_old = next >= 0 && !is_new(next) && prev < 0;
      if (!after_old && !before_old) {
        continue;
      }
      const int e = old_image(doc, layer, target, after_old ? prev : next);
      if (e < 0) {
        continue;
      }
      CurveEnd end = drawing_curve_end_at(dst, e);
      if (end.curve < 0) {
        continue;
      }
      if (!end.at_end) {
        drawing_reverse_curve(dst, end.curve);
      }
      /* The run of new points from that end outward, in order. */
      std::vector<int> run;
      if (after_old) {
        for (int k = i; k < m && is_new(curve.points[k].point); k++) {
          run.push_back(k);
        }
      }
      else {
        for (int k = i; k >= 0 && is_new(curve.points[k].point); k--) {
          run.push_back(k);
        }
      }
      Curve &grown = dst.curves[end.curve];
      const int segments_before = grown.segment_count();
      for (const int k : run) {
        dst.points.push_back(image_point(from.points[curve.points[k].point], target.to));
        CurvePoint cp;
        cp.point = int(dst.points.size()) - 1;
        const auto [kin, kout] = shown_handles(from, curve, k);
        /* Grown at its end, the image runs outward as this side does when this side grows at its end too. */
        cp.handle_in = target.to.apply_vector(after_old ? kin : kout);
        cp.handle_out = target.to.apply_vector(after_old ? kout : kin);
        cp.handles_set = true;
        cp.curvature = curve.points[k].curvature;
        cp.aligned = curve.points[k].aligned;
        grown.points.push_back(cp);
      }
      /* Lines along the whole curve grow with it, as Extrude grows them. */
      const int segments_after = grown.segment_count();
      for (Shape &shape : dst.shapes) {
        int on_curve = 0;
        for (const ShapeEdge &edge : shape.edges) {
          on_curve += edge.curve == end.curve;
        }
        if (on_curve == segments_before && !shape.has_fill) {
          for (int sgm = segments_before; sgm < segments_after; sgm++) {
            shape.edges.push_back({end.curve, sgm});
          }
        }
      }
      i += int(run.size()) - 1;
    }
  }

  /* (d) A curve closed (Close Curve, F on its ends): the image curve closes too. */
  for (int c = 0; c < int(from.curves.size()) && c < int(was.drawing.curves.size()); c++) {
    const Curve &now = from.curves[c], &before = was.drawing.curves[c];
    if (!now.closed || before.closed || now.points.size() < 2 || before.points.empty() ||
        now.points.front().point != before.points.front().point)
    {
      continue;
    }
    const int a = old_image(doc, layer, target, now.points.front().point);
    const int b = old_image(doc, layer, target, now.points.back().point);
    if (a >= 0 && b >= 0 && a != b) {
      drawing_make_segment(target.layer->drawing, a, b);
    }
  }

  /* (c) New shapes over curves that were there (Fill): the same over the image segments. */
  for (int s = s0; s < int(from.shapes.size()); s++) {
    if (shape_done[s]) {
      continue;
    }
    const Shape &shape = from.shapes[s];
    VectorDrawing &dst = target.layer->drawing;
    std::vector<ShapeEdge> edges;
    bool ok = !shape.edges.empty();
    for (const ShapeEdge &e : shape.edges) {
      if (!ok) {
        break;
      }
      int a, b;
      curve_segment_points(from.curves[e.curve], e.segment, &a, &b);
      const int ia = is_new(a) ? point_at(dst, target.to.apply(from.points[a].pos)) : old_image(doc, layer, target, a);
      const int ib = is_new(b) ? point_at(dst, target.to.apply(from.points[b].pos)) : old_image(doc, layer, target, b);
      ShapeEdge edge;
      bool reversed;
      ok = ia >= 0 && ib >= 0 && segment_between(dst, ia, ib, &edge, &reversed);
      edges.push_back(edge);
    }
    if (!ok) {
      continue;
    }
    std::vector<ShapeEdge> sorted = edges;
    const auto edge_less = [](const ShapeEdge &x, const ShapeEdge &y) {
      return x.curve != y.curve ? x.curve < y.curve : x.segment < y.segment;
    };
    std::sort(sorted.begin(), sorted.end(), edge_less);
    bool there = false;
    for (const Shape &other : dst.shapes) {
      std::vector<ShapeEdge> theirs = other.edges;
      std::sort(theirs.begin(), theirs.end(), edge_less);
      there = there || (theirs == sorted && other.has_fill == shape.has_fill);
    }
    if (there) {
      continue;
    }
    Shape copy = shape;
    copy.id = 0;
    copy.edges = edges;
    dst.shapes.push_back(std::move(copy));
  }
  drawing_ensure_ids(target.layer->drawing);
}

void document_mirror_new_geometry(Document &doc, const Document &previous)
{
  std::vector<uint64_t> ids;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (flat->type == LayerType::Vector && !flat->is_reference()) {
      ids.push_back(flat->id);
    }
  }
  const Transform2D flip{-1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  for (const uint64_t id : ids) {
    Layer *layer = layer_find(doc, id);
    const Layer *was = layer_find(previous, id);
    if (!layer || !was) {
      continue;
    }
    MirrorTarget target;
    if (Layer *partner = layer_mirror_partner(doc, id)) {
      if (partner->type != LayerType::Vector || partner->is_reference() || !layer_find(previous, partner->id)) {
        continue;
      }
      target = {partner, layer_mirror_to_partner(doc, id, partner->id), false};
    }
    else {
      target = {layer, flip, true};
    }
    mirror_new_geometry(doc, *layer, *was, target);
  }
}

}  // namespace oanim
