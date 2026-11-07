#include "data/vector.hh"

#include "data/detail.hh"
#include "data/fill_area.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>

namespace oanim {

namespace {
/* Structural edits on a drawing with detail keep its look (see drawing_detail_edit_begin). */
struct DetailScope {
  VectorDrawing &drawing;
  DetailEdit edit;
  explicit DetailScope(VectorDrawing &d) : drawing(d), edit(drawing_detail_edit_begin(d)) {}
  ~DetailScope() { drawing_detail_edit_end(drawing, edit); }
  DetailScope(const DetailScope &) = delete;
  DetailScope &operator=(const DetailScope &) = delete;
};
}  // namespace

int Curve::segment_count() const
{
  const int n = int(points.size());
  if (n < 2) {
    return 0;
  }
  return closed ? n : n - 1;
}

/* ---------------------------------------------------------------------------------
 * Curve geometry
 */

/* A handle worked out from the neighbours, for a point without its own yet. `mode` 0: smooth
 * (Moho's GetBezierHandle(), from curvature, weights and offsets), or straight when the curvature
 * is 0; 1: straight (a third of the way to the neighbour); 2: smooth even where curvature is 0. */
static float2 computed_handle(const VectorDrawing &drawing, const Curve &curve, int i, bool pre, int mode)
{
  const int n = int(curve.points.size());
  const CurvePoint &cp = curve.points[i];
  const float2 here = drawing.points[cp.point].pos;
  if (mode == 1 || (mode == 0 && cp.curvature <= 0.0f)) {
    /* A third of the way to the neighbour on that side; none past an open curve's end. */
    const int other = pre ? i - 1 : i + 1;
    if (n < 2 || (!curve.closed && (other < 0 || other >= n))) {
      return here;
    }
    const float2 there = drawing.points[curve.points[(other + n) % n].point].pos;
    return here + (there - here) / 3.0f;
  }
  const auto pos = [&](int index) { return drawing.points[curve.points[index].point].pos; };
  if (n < 2) {
    return here;
  }
  const auto in_range = [&](int p) {
    if (curve.closed) {
      return (p % n + n) % n;
    }
    return std::clamp(p, 0, n - 1);
  };
  const int post_id = in_range(i + 1);
  const int pre_id = in_range(i - 1);
  const int second_id = pre ? pre_id : post_id;
  if (second_id == i) {
    return here;
  }
  /* The tangent bisects the directions to the next point and from the previous one (as Moho draws
   * it), not the line between the neighbours: a point with one neighbour close by (a cheek's
   * corner) keeps the curve's own direction there. */
  const float2 to_next = pos(post_id) - here, from_prev = here - pos(pre_id);
  const float next_len = length(to_next), prev_len = length(from_prev);
  float2 dir = (next_len > 1e-9f ? to_next / next_len : float2()) + (prev_len > 1e-9f ? from_prev / prev_len : float2());
  if (length(dir) < 1e-9f) {
    /* Neighbours straight back the way it came: across, as the old line between them was. */
    dir = pos(post_id) - pos(pre_id);
  }
  dir = normalize(dir);
  if (pre) {
    dir = -dir;
  }
  const float curvature = mode == 2 ? std::max(cp.curvature, CURVATURE_SMOOTH) : cp.curvature;
  const float len = distance(pos(second_id), here) * curvature * (pre ? cp.weight_in : cp.weight_out);
  return here + rotate(dir * len, pre ? cp.offset_in : cp.offset_out);
}

float2 curve_handle(const VectorDrawing &drawing, const Curve &curve, int i, bool pre)
{
  const CurvePoint &cp = curve.points[i];
  if (!cp.handles_set) {
    return computed_handle(drawing, curve, i, pre, 0);
  }
  return drawing.points[cp.point].pos + (pre ? cp.handle_in : cp.handle_out);
}

/* Stores handle offsets, keeping a channel without keys from treating it as an edit. */
static void set_handles(const VectorDrawing &drawing, CurvePoint &cp, float2 in, float2 out, bool quiet)
{
  (void)drawing;
  cp.handle_in = in;
  cp.handle_out = out;
  cp.handles_set = true;
  if (quiet) {
    /* Not an edit: without keys, this is the setup shape (no change from it). */
    for (const bool pre : {true, false}) {
      Channel<float2> &ch = pre ? cp.handle_in_keys : cp.handle_out_keys;
      if (!ch.animated()) {
        (pre ? cp.setup_in : cp.setup_out) = pre ? in : out;
        (pre ? cp.change_in : cp.change_out) = {};
        ch.last = {};
        ch.has_last = true;
      }
    }
  }
}


/* The frame-0 key, or for a value without keys the one from before any edit not yet recorded
 * (an edit away from frame 0 is about to become a key, not the setup). */
template<typename T> static T setup_of(const Channel<T> &ch, const T &value)
{
  return ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value);
}

/* Just the points' setup-pose (frame 0) positions, light enough to build once per change. */
static VectorDrawing setup_points(const VectorDrawing &drawing)
{
  VectorDrawing setup;
  setup.points.resize(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    setup.points[i].pos = point_setup_pos(drawing.points[i]);
  }
  return setup;
}

static Curve setup_curve(const Curve &curve)
{
  Curve c;
  c.closed = curve.closed;
  c.points = curve.points;
  for (CurvePoint &cp : c.points) {
    cp.curvature = setup_of(cp.curvature_keys, cp.curvature);
    cp.handles_set = false;
  }
  return c;
}

/* Curve point i's handles (offsets from it) as worked out from its neighbours in the setup
 * pose: what a point that never had its own handles looks like on frame 0. */
static void setup_handles(const VectorDrawing &setup, const Curve &curve, int i, float2 *r_in, float2 *r_out)
{
  const Curve c = setup_curve(curve);
  const float2 here = setup.points[c.points[i].point].pos;
  *r_in = computed_handle(setup, c, i, true, 0) - here;
  *r_out = computed_handle(setup, c, i, false, 0) - here;
}

/* Makes sure a point's handles are stored (so they can be moved by hand): what it shows now.
 * A channel without keys remembers the setup-pose handles as its value before this edit, so a
 * first key away from frame 0 keys frame 0 with the setup shape (anim/animation.hh). */
static void own_handles(VectorDrawing &drawing, Curve &curve, int i)
{
  CurvePoint &cp = curve.points[i];
  if (cp.handles_set) {
    return;
  }
  const float2 here = drawing.points[cp.point].pos;
  const float2 in = computed_handle(drawing, curve, i, true, 0) - here;
  const float2 out = computed_handle(drawing, curve, i, false, 0) - here;
  float2 setup_in, setup_out;
  setup_handles(setup_points(drawing), curve, i, &setup_in, &setup_out);
  cp.handle_in = in;
  cp.handle_out = out;
  cp.handles_set = true;
  /* Without keys, the setup shape is what the neighbours give in the setup pose, and what it
   * shows now (posed) is a change from it, as if just edited. */
  for (const bool pre : {true, false}) {
    Channel<float2> &ch = pre ? cp.handle_in_keys : cp.handle_out_keys;
    if (!ch.animated()) {
      float2 &setup = pre ? cp.setup_in : cp.setup_out;
      setup = pre ? setup_in : setup_out;
      (pre ? cp.change_in : cp.change_out) = handle_change(setup, pre ? in : out, 0.0f);
      ch.last = {};
      ch.has_last = true;
    }
  }
}

void curve_point_set_handles(CurvePoint &cp, float2 in, float2 out)
{
  set_handles(VectorDrawing(), cp, in, out, true);
}

void drawing_init_handles(VectorDrawing &drawing)
{
  /* Handles are setup data: worked out from the setup pose, they hold on every frame. */
  VectorDrawing setup;
  bool built = false;
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (cp.handles_set) {
        continue;
      }
      if (!built) {
        setup = setup_points(drawing);
        built = true;
      }
      float2 in, out;
      setup_handles(setup, curve, i, &in, &out);
      /* Smooth handles come out in line; keep them so. */
      cp.aligned = cp.curvature > 0.0f;
      set_handles(drawing, cp, in, out, true);
    }
  }
}

void curve_point_move_handle(VectorDrawing &drawing, Curve &curve, int i, bool pre, float2 pos, bool keep_other)
{
  own_handles(drawing, curve, i);
  CurvePoint &cp = curve.points[i];
  const float2 offset = pos - drawing.points[cp.point].pos;
  (pre ? cp.handle_in : cp.handle_out) = offset;
  if (cp.aligned && !keep_other && length(offset) > 1e-9f) {
    float2 &other = pre ? cp.handle_out : cp.handle_in;
    other = -normalize(offset) * length(other);
  }
}

void curve_point_align(VectorDrawing &drawing, Curve &curve, int i, bool turn_out)
{
  own_handles(drawing, curve, i);
  CurvePoint &cp = curve.points[i];
  const float2 in = cp.handle_in, out = cp.handle_out;
  if (turn_out ? length(in) > 1e-9f : length(out) <= 1e-9f) {
    if (length(in) > 1e-9f) {
      cp.handle_out = -normalize(in) * length(out);
    }
  }
  else if (length(out) > 1e-9f) {
    cp.handle_in = -normalize(out) * length(in);
  }
  cp.aligned = true;
}

void curve_point_align_both(VectorDrawing &drawing, Curve &curve, int i)
{
  own_handles(drawing, curve, i);
  CurvePoint &cp = curve.points[i];
  const float in_len = length(cp.handle_in), out_len = length(cp.handle_out);
  cp.aligned = true;
  if (in_len <= 1e-9f || out_len <= 1e-9f) {
    return;
  }
  /* Where the in handle points, and where the out handle would have it point: aligned, the two are the same. */
  const float2 a = cp.handle_in / in_len, b = -cp.handle_out / out_len;
  float2 back = a + b;
  if (length(back) < 1e-4f) {
    /* Folded back on each other: the way the curve runs through the point, from the one before to the one after. */
    const int n = int(curve.points.size());
    const bool has_before = curve.closed || i > 0, has_after = curve.closed || i + 1 < n;
    if (!has_before || !has_after) {
      return;
    }
    const float2 before = drawing.points[curve.points[size_t((i - 1 + n) % n)].point].pos;
    const float2 after = drawing.points[curve.points[size_t((i + 1) % n)].point].pos;
    back = before - after;
    if (length(back) < 1e-9f) {
      return;
    }
  }
  back = normalize(back);
  cp.handle_in = back * in_len;
  cp.handle_out = -back * out_len;
}

static float wrap_turn(float a);

/* One timeline of an aligned point's two handle channels (keys hold each handle's turn and growth from its setup):
 * the follower takes the other's turn at every frame either is keyed. `setup_frame`: an action's keys start after
 * frame 0, which is the setup (no change). */
static bool align_timeline(std::vector<Key<float2>> &in, std::vector<Key<float2>> &out, AlignFollow follow,
                           bool setup_frame)
{
  if (in.empty() && out.empty()) {
    return false;
  }
  const auto evaluated = [setup_frame](const std::vector<Key<float2>> &keys) {
    Channel<float2> ch;
    ch.keys = keys;
    if (setup_frame && !keys.empty() && !ch.key_at(0)) {
      ch.set_key(0, float2());
    }
    return ch;
  };
  const Channel<float2> ci = evaluated(in), co = evaluated(out);
  std::set<int> frames;
  for (const auto *keys : {&in, &out}) {
    for (const Key<float2> &k : *keys) {
      frames.insert(k.frame);
    }
  }
  const auto value = [](const Channel<float2> &ch, int frame) { return ch.animated() ? ch.evaluate(frame) : float2(); };
  const auto rebuilt = [&](const std::vector<Key<float2>> &own, const std::vector<Key<float2>> &other,
                           const Channel<float2> &own_ch, const auto &turn) {
    std::vector<Key<float2>> keys;
    for (const int f : frames) {
      const auto at = [f](const Key<float2> &k) { return k.frame == f; };
      const auto mine = std::find_if(own.begin(), own.end(), at);
      /* A frame only the other is keyed on takes its key's timing. */
      Key<float2> k = mine != own.end() ? *mine : *std::find_if(other.begin(), other.end(), at);
      k.frame = f;
      k.value = float2(turn(f), value(own_ch, f).y);
      keys.push_back(k);
    }
    return keys;
  };
  const std::vector<Key<float2>> was_in = in, was_out = out;
  if (follow == AlignFollow::In) {
    in = rebuilt(was_in, was_out, ci, [&](int f) { return value(co, f).x; });
  }
  else if (follow == AlignFollow::Out) {
    out = rebuilt(was_out, was_in, co, [&](int f) { return value(ci, f).x; });
  }
  else {
    const auto halfway = [&](int f) {
      const float a = value(ci, f).x;
      return a + wrap_turn(value(co, f).x - a) * 0.5f;
    };
    in = rebuilt(was_in, was_out, ci, halfway);
    out = rebuilt(was_out, was_in, co, halfway);
  }
  const auto same = [](const std::vector<Key<float2>> &a, const std::vector<Key<float2>> &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Key<float2> &x, const Key<float2> &y) {
             return x.frame == y.frame && x.value == y.value;
           });
  };
  return !same(in, was_in) || !same(out, was_out);
}

bool curve_point_align_keys(CurvePoint &cp, AlignFollow follow)
{
  Channel<float2> &ci = cp.handle_in_keys, &co = cp.handle_out_keys;
  bool changed = false;
  /* The setups in line first: then the same turn keeps them so. */
  const float2 si = handle_setup(cp, true), so = handle_setup(cp, false);
  const float li = length(si), lo = length(so);
  float2 new_in = si, new_out = so;
  if (li > 1e-9f && lo > 1e-9f) {
    if (follow == AlignFollow::In) {
      new_in = -so / lo * li;
    }
    else if (follow == AlignFollow::Out) {
      new_out = -si / li * lo;
    }
    else if (const float2 back = si / li - so / lo; length(back) > 1e-6f) {
      new_in = normalize(back) * li;
      new_out = -normalize(back) * lo;
    }
  }
  std::set<std::string> names;
  for (const auto *ch : {&ci, &co}) {
    for (const auto &[name, keys] : ch->actions) {
      names.insert(name);
    }
  }
  const bool keyed = ci.animated() || co.animated() || !names.empty();
  if (!keyed) {
    return false;
  }
  /* Keys with no setup of their own yet: the handle as it is. */
  cp.setup_in = new_in;
  cp.setup_out = new_out;
  changed = new_in != si || new_out != so;
  changed |= align_timeline(ci.keys, co.keys, follow, false);
  for (const std::string &name : names) {
    std::vector<Key<float2>> in, out;
    if (auto it = ci.actions.find(name); it != ci.actions.end()) {
      in = it->second;
    }
    if (auto it = co.actions.find(name); it != co.actions.end()) {
      out = it->second;
    }
    /* "" is the mainline, put away while an action is edited: it has its own frame 0. */
    if (align_timeline(in, out, follow, !name.empty())) {
      changed = true;
      for (auto [ch, keys] : {std::pair{&ci, &in}, std::pair{&co, &out}}) {
        if (keys->empty()) {
          ch->actions.erase(name);
        }
        else {
          ch->actions[name] = *keys;
        }
      }
    }
  }
  for (Channel<float2> *ch : {&ci, &co}) {
    ch->has_last = true;
  }
  return changed;
}

void curve_segment_points(const Curve &curve, int segment, int *r_from, int *r_to)
{
  const int n = int(curve.points.size());
  *r_from = curve.points[segment].point;
  *r_to = curve.points[(segment + 1) % n].point;
}

void curve_segment_bezier(const VectorDrawing &drawing, const Curve &curve, int segment, float2 r_ctrl[4])
{
  const int n = int(curve.points.size());
  const int next = (segment + 1) % n;
  r_ctrl[0] = drawing.points[curve.points[segment].point].pos;
  r_ctrl[1] = curve_handle(drawing, curve, segment, false);
  r_ctrl[2] = curve_handle(drawing, curve, next, true);
  r_ctrl[3] = drawing.points[curve.points[next].point].pos;
}

float2 bezier_eval(const float2 c[4], float t)
{
  const float u = 1.0f - t;
  return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
}

float2 bezier_sample(const float2 c[4], int i, int steps)
{
  if (2 * i == steps) {
    return ((c[0] + c[3]) + (c[1] + c[2]) * 3.0f) * 0.125f;
  }
  if (2 * i < steps) {
    return bezier_eval(c, float(i) / float(steps));
  }
  const float2 reversed[4] = {c[3], c[2], c[1], c[0]};
  return bezier_eval(reversed, float(steps - i) / float(steps));
}

int bezier_steps(const float2 ctrl[4], int min_steps, float tolerance)
{
  if (tolerance <= 0.0f) {
    return min_steps;
  }
  /* Wang's formula: n pieces keep a cubic within `tolerance` of its chords when
   * n >= sqrt(3/4 * L / tolerance), L the larger second difference of the control points. Each worked out as
   * (outer + outer) - 2 inner, which gives the very same number for the segment run the other way, so two outlines
   * sharing an edge cut it into as many pieces (and bezier_sample gives them the same points). */
  const float L = std::max(length((ctrl[0] + ctrl[2]) - ctrl[1] * 2.0f), length((ctrl[1] + ctrl[3]) - ctrl[2] * 2.0f));
  const int n = int(std::ceil(std::sqrt(0.75f * L / tolerance)));
  return std::clamp(n, min_steps, 1024);
}

void curve_segment_sample(const VectorDrawing &drawing, const Curve &curve, int segment, int steps,
                          std::vector<float2> &r_points, float tolerance)
{
  float2 ctrl[4];
  curve_segment_bezier(drawing, curve, segment, ctrl);
  steps = bezier_steps(ctrl, steps, tolerance);
  for (int i = 0; i < steps; i++) {
    r_points.push_back(bezier_sample(ctrl, i, steps));
  }
}

bool drawing_ensure_ids(VectorDrawing &drawing)
{
  /* Nearly always every id is there and its own (this runs before each undo step, over every drawing):
   * seen with a sorted list, without a set's allocation per point. */
  {
    std::vector<uint64_t> point_ids, shape_ids;
    point_ids.reserve(drawing.points.size());
    shape_ids.reserve(drawing.shapes.size());
    bool fine = true;
    for (const MeshPoint &p : drawing.points) {
      fine = fine && p.id != 0 && p.id < drawing.next_id;
      point_ids.push_back(p.id);
    }
    for (const Shape &sh : drawing.shapes) {
      fine = fine && sh.id != 0 && sh.id < drawing.next_id;
      shape_ids.push_back(sh.id);
    }
    if (fine) {
      std::sort(point_ids.begin(), point_ids.end());
      std::sort(shape_ids.begin(), shape_ids.end());
      if (std::adjacent_find(point_ids.begin(), point_ids.end()) == point_ids.end() &&
          std::adjacent_find(shape_ids.begin(), shape_ids.end()) == shape_ids.end())
      {
        return false;
      }
    }
  }
  for (const MeshPoint &p : drawing.points) {
    drawing.next_id = std::max(drawing.next_id, p.id + 1);
  }
  for (const Shape &s : drawing.shapes) {
    drawing.next_id = std::max(drawing.next_id, s.id + 1);
  }
  bool changed = false;
  std::set<uint64_t> seen;
  for (MeshPoint &p : drawing.points) {
    if (p.id == 0 || !seen.insert(p.id).second) {
      const uint64_t copied = p.id;
      p.id = drawing.next_id++;
      seen.insert(p.id);
      changed = true;
      /* A copy (Duplicate) is in its original's point groups too, with its weights. */
      for (PointGroup &g : drawing.groups) {
        if (const auto it = g.weights.find(copied); copied != 0 && it != g.weights.end()) {
          g.weights[p.id] = it->second;
        }
      }
    }
  }
  seen.clear();
  for (Shape &s : drawing.shapes) {
    if (s.id == 0 || !seen.insert(s.id).second) {
      s.id = drawing.next_id++;
      seen.insert(s.id);
      changed = true;
    }
  }
  return changed;
}

std::string drawing_group_unique_name(const VectorDrawing &drawing, const std::string &base)
{
  const auto taken = [&](const std::string &name) {
    return std::any_of(drawing.groups.begin(), drawing.groups.end(), [&](const PointGroup &g) { return g.name == name; });
  };
  if (!taken(base)) {
    return base;
  }
  for (int n = 1;; n++) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%03d", n);
    if (!taken(base + suffix)) {
      return base + suffix;
    }
  }
}

void drawing_groups_prune(VectorDrawing &drawing)
{
  /* A grid with a node gone is ordinary drawing from then on. */
  std::erase_if(drawing.grids, [&](const DrawingGrid &g) { return !grid_valid(drawing, g); });
  if (drawing.groups.empty()) {
    return;
  }
  std::set<uint64_t> ids;
  for (const MeshPoint &p : drawing.points) {
    ids.insert(p.id);
  }
  for (PointGroup &g : drawing.groups) {
    std::erase_if(g.weights, [&](const auto &entry) { return !ids.contains(entry.first); });
  }
}

int drawing_point_index(const VectorDrawing &drawing, uint64_t id)
{
  for (int i = 0; i < int(drawing.points.size()); i++) {
    if (drawing.points[i].id == id) {
      return i;
    }
  }
  return -1;
}

int drawing_shape_index(const VectorDrawing &drawing, uint64_t id)
{
  for (int i = 0; i < int(drawing.shapes.size()); i++) {
    if (drawing.shapes[i].id == id) {
      return i;
    }
  }
  return -1;
}

VectorDrawing drawing_setup_pose(const VectorDrawing &drawing)
{
  VectorDrawing setup = drawing;
  for (MeshPoint &p : setup.points) {
    p.pos = point_setup_pos(p);
    p.radius = setup_of(p.radius_keys, p.radius);
  }
  for (Curve &curve : setup.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.curvature = setup_of(cp.curvature_keys, cp.curvature);
      cp.handle_in = handle_setup(cp, true);
      cp.handle_out = handle_setup(cp, false);
    }
  }
  /* Detail in its setup pose, placed on the setup curve. */
  for (MeshPoint &p : setup.points) {
    if (p.detail.level > 0 && (p.pos_keys.has_last || p.pos_keys.animated())) {
      p.detail.pos = p.detail.setup_pos;
    }
  }
  for (Curve &curve : setup.curves) {
    for (CurvePoint &cp : curve.points) {
      PointDetail &dt = setup.points[cp.point].detail;
      if (dt.level > 0) {
        dt.in = cp.handle_in_keys.has_last || cp.handle_in_keys.animated() ? dt.setup_in : dt.in;
        dt.out = cp.handle_out_keys.has_last || cp.handle_out_keys.animated() ? dt.setup_out : dt.out;
      }
    }
  }
  drawing_detail_sync(setup, false);
  return setup;
}

bool shape_chain(const VectorDrawing &drawing, const Shape &shape, std::vector<OrientedEdge> &r_chain,
                 bool *r_closed)
{
  r_chain.clear();
  *r_closed = false;
  if (shape.edges.empty()) {
    return false;
  }
  using Oriented = OrientedEdge;
  /* Chain the edges by shared points, so the order they're stored in doesn't matter. */
  const auto ends = [&](const ShapeEdge &e, int *from, int *to) {
    curve_segment_points(drawing.curves[e.curve], e.segment, from, to);
  };
  std::vector<bool> used(shape.edges.size(), false);
  std::deque<Oriented> chain;
  int first_from, last_to;
  ends(shape.edges[0], &first_from, &last_to);
  chain.push_back({shape.edges[0], false});
  used[0] = true;
  bool grew = true;
  while (grew) {
    grew = false;
    for (size_t i = 0; i < shape.edges.size(); i++) {
      if (used[i]) {
        continue;
      }
      int a, b;
      ends(shape.edges[i], &a, &b);
      if (a == last_to || b == last_to) {
        const bool reversed = (b == last_to);
        chain.push_back({shape.edges[i], reversed});
        last_to = reversed ? a : b;
      }
      else if (a == first_from || b == first_from) {
        const bool reversed = (a == first_from);
        chain.push_front({shape.edges[i], reversed});
        first_from = reversed ? b : a;
      }
      else {
        continue;
      }
      used[i] = true;
      grew = true;
    }
  }
  const bool connected = std::all_of(used.begin(), used.end(), [](bool u) { return u; });
  r_chain.assign(chain.begin(), chain.end());
  *r_closed = (first_from == last_to) && chain.size() > 1;
  return connected;
}

bool shape_outline(const VectorDrawing &drawing, const Shape &shape, int min_steps,
                   std::vector<float2> &r_points, std::vector<float> *r_radii, bool *r_closed,
                   std::vector<bool> *r_visible, std::vector<StrokeJoin> *r_joins, float tolerance)
{
  if (r_joins) {
    r_joins->clear();
  }

  r_points.clear();
  if (r_radii) {
    r_radii->clear();
  }
  if (r_visible) {
    r_visible->clear();
  }
  *r_closed = false;
  std::vector<OrientedEdge> chain;
  bool closed = false;
  const bool connected = shape_chain(drawing, shape, chain, &closed);
  if (chain.empty()) {
    return false;
  }
  for (const OrientedEdge &o : chain) {
    const Curve &curve = drawing.curves[o.edge.curve];
    float2 ctrl[4];
    curve_segment_bezier(drawing, curve, o.edge.segment, ctrl);
    int from, to;
    curve_segment_points(curve, o.edge.segment, &from, &to);
    if (o.reversed) {
      std::swap(ctrl[0], ctrl[3]);
      std::swap(ctrl[1], ctrl[2]);
      std::swap(from, to);
    }
    const int steps_per_segment = bezier_steps(ctrl, min_steps, tolerance);
    const float r0 = drawing.points[from].radius, r1 = drawing.points[to].radius;
    const bool visible = !curve.points[o.edge.segment].edge_hidden;
    /* The radius blends by distance along the segment, not by the curve parameter: samples
     * bunch up near points (a peaked segment eases in and out), and blending by parameter
     * would bend a tapered stroke's straight edges there. */
    std::vector<float2> samples(steps_per_segment + 1);
    std::vector<float> along(steps_per_segment + 1, 0.0f);
    for (int i = 0; i <= steps_per_segment; i++) {
      samples[i] = bezier_sample(ctrl, i, steps_per_segment);
      if (i > 0) {
        along[i] = along[i - 1] + distance(samples[i - 1], samples[i]);
      }
    }
    const float total = along.back();
    for (int i = 0; i < steps_per_segment; i++) {
      r_points.push_back(samples[i]);
      const float blend = total > 1e-9f ? along[i] / total : float(i) / float(steps_per_segment);
      if (r_radii) {
        r_radii->push_back(r0 + (r1 - r0) * blend);
      }
      if (r_visible) {
        r_visible->push_back(visible);
      }
      if (r_joins) {
        r_joins->push_back(i != 0 ? StrokeJoin::Smooth : StrokeJoin::Round);
      }
    }
  }
  *r_closed = closed;
  if (!*r_closed) {
    /* Open runs end on their last point. */
    int last_to, unused;
    const OrientedEdge &end = chain.back();
    curve_segment_points(drawing.curves[end.edge.curve], end.edge.segment, &unused, &last_to);
    if (end.reversed) {
      last_to = unused;
    }
    r_points.push_back(drawing.points[last_to].pos);
    if (r_radii) {
      r_radii->push_back(drawing.points[last_to].radius);
    }
    if (r_visible) {
      r_visible->push_back(false);
    }
    if (r_joins) {
      r_joins->push_back(StrokeJoin::Round);
    }
  }
  return connected;
}

/* An outline-only shape's runs where its lines meet three or more at a point (a grid's crossings, a branch):
 * each curve's run of segments on its own, joined to another only where just those two meet. Walking edges
 * into chains could turn back on itself there, and a stroke folded on itself draws as a hairline. Empty when
 * no point has three edges (the shape is chained as before). */
static std::vector<Shape> shape_line_runs(const VectorDrawing &drawing, const Shape &shape)
{
  std::map<int, int> degree;
  const auto ends = [&](const ShapeEdge &e, int *a, int *b) {
    curve_segment_points(drawing.curves[size_t(e.curve)], e.segment, a, b);
  };
  bool junction = false;
  for (const ShapeEdge &e : shape.edges) {
    int a, b;
    ends(e, &a, &b);
    junction |= ++degree[a] >= 3;
    junction |= ++degree[b] >= 3;
  }
  if (!junction) {
    return {};
  }
  /* Each curve's segments in the shape, in runs of neighbours along it (round the end of a closed curve). */
  struct Run {
    std::vector<ShapeEdge> edges;
    int first, last;
  };
  std::vector<Run> runs;
  std::vector<int> order;
  for (const ShapeEdge &e : shape.edges) {
    if (std::find(order.begin(), order.end(), e.curve) == order.end()) {
      order.push_back(e.curve);
    }
  }
  for (const int c : order) {
    const Curve &curve = drawing.curves[size_t(c)];
    std::vector<int> segs;
    for (const ShapeEdge &e : shape.edges) {
      if (e.curve == c) {
        segs.push_back(e.segment);
      }
    }
    std::sort(segs.begin(), segs.end());
    segs.erase(std::unique(segs.begin(), segs.end()), segs.end());
    std::vector<std::vector<int>> pieces;
    for (const int s : segs) {
      if (pieces.empty() || pieces.back().back() != s - 1) {
        pieces.push_back({});
      }
      pieces.back().push_back(s);
    }
    if (curve.closed && pieces.size() > 1 && pieces.front().front() == 0 &&
        pieces.back().back() == curve.segment_count() - 1)
    {
      pieces.back().insert(pieces.back().end(), pieces.front().begin(), pieces.front().end());
      pieces.erase(pieces.begin());
    }
    for (const std::vector<int> &piece : pieces) {
      Run run;
      for (const int s : piece) {
        run.edges.push_back({c, s});
      }
      int a, b;
      ends(run.edges.front(), &a, &b);
      run.first = a;
      ends(run.edges.back(), &a, &b);
      run.last = b;
      runs.push_back(std::move(run));
    }
  }
  /* Runs meeting at a point only the two of them reach join into one (the order of a run's edges doesn't
   * matter: drawing chains them by their points). */
  bool joined = true;
  while (joined) {
    joined = false;
    for (size_t i = 0; i < runs.size() && !joined; i++) {
      for (size_t j = i + 1; j < runs.size() && !joined; j++) {
        Run &r = runs[i];
        const Run &q = runs[j];
        if (r.first == r.last || q.first == q.last) {
          continue;
        }
        for (const int p : {r.first, r.last}) {
          if (degree[p] != 2 || (q.first != p && q.last != p)) {
            continue;
          }
          const int r_other = p == r.first ? r.last : r.first, q_other = p == q.first ? q.last : q.first;
          r.edges.insert(r.edges.end(), q.edges.begin(), q.edges.end());
          r.first = r_other;
          r.last = q_other;
          runs.erase(runs.begin() + std::ptrdiff_t(j));
          joined = true;
          break;
        }
      }
    }
  }
  std::vector<Shape> out;
  for (const Run &run : runs) {
    Shape s = shape;
    s.edges = run.edges;
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<Shape> shape_runs(const VectorDrawing &drawing, const Shape &shape)
{
  if (!shape.has_fill && shape.edges.size() > 2) {
    std::vector<Shape> lines = shape_line_runs(drawing, shape);
    if (!lines.empty()) {
      return lines;
    }
  }
  std::vector<OrientedEdge> chain;
  bool closed = false;
  if (shape.edges.empty() || shape_chain(drawing, shape, chain, &closed)) {
    return {shape};
  }
  std::vector<Shape> runs;
  Shape rest = shape;
  while (!rest.edges.empty()) {
    shape_chain(drawing, rest, chain, &closed);
    Shape run = shape;
    run.edges.clear();
    for (const OrientedEdge &o : chain) {
      run.edges.push_back(o.edge);
      std::erase(rest.edges, o.edge);
    }
    runs.push_back(std::move(run));
  }
  return runs;
}

/* ---------------------------------------------------------------------------------
 * Editing helpers
 */

int drawing_add_point(VectorDrawing &drawing, float2 pos)
{
  drawing.points.push_back({pos});
  return int(drawing.points.size()) - 1;
}

/* Renumbers the shapes' curves and, where `segment_remap` has an entry for a curve (empty: it kept
 * its segments), their segments: segments that joined into one become one edge. A filled shape whose
 * outline lost a piece (a curve gone, or a segment, -1) is removed: it no longer goes round. A line
 * (outline only) keeps the segments it has left, and goes only with the last of them. */
static void shapes_remap_curves(VectorDrawing &drawing, const std::vector<int> &curve_remap,
                                const std::vector<std::vector<int>> &segment_remap)
{
  const auto new_segment = [&](const ShapeEdge &e) {
    const std::vector<int> &map = segment_remap[e.curve];
    return map.empty() ? e.segment : (e.segment >= 0 && e.segment < int(map.size()) ? map[e.segment] : -1);
  };
  const auto lost = [&](const ShapeEdge &e) { return curve_remap[e.curve] < 0 || new_segment(e) < 0; };
  std::erase_if(drawing.shapes, [&](const Shape &shape) {
    if (shape.has_fill) {
      return std::any_of(shape.edges.begin(), shape.edges.end(), lost);
    }
    return std::all_of(shape.edges.begin(), shape.edges.end(), lost);
  });
  for (Shape &shape : drawing.shapes) {
    std::vector<ShapeEdge> edges;
    for (const ShapeEdge &e : shape.edges) {
      if (lost(e)) {
        continue;
      }
      const ShapeEdge moved{curve_remap[e.curve], new_segment(e)};
      if (edges.empty() || !(edges.back() == moved)) {
        edges.push_back(moved);
      }
    }
    while (edges.size() > 1 && edges.back() == edges.front()) {
      edges.pop_back();
    }
    shape.edges = std::move(edges);
  }
}

/* Where each segment of a curve goes when the marked points leave it (`removed`, one per curve
 * point; `closed_after`: still a loop): the two segments either side of a removed point join into
 * one; a segment past the last kept point at an open end, -1. */
static std::vector<int> curve_segment_remap(const std::vector<bool> &removed, bool closed_before, bool closed_after)
{
  const int n = int(removed.size());
  std::vector<int> new_index(n, -1);
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (!removed[i]) {
      new_index[i] = m++;
    }
  }
  const int segments = closed_before ? n : n - 1;
  std::vector<int> map(std::max(segments, 0), -1);
  for (int s = 0; s < segments; s++) {
    /* The segment now starting here: from the last kept point at or before s... */
    int start = -1;
    for (int k = 0; k < n; k++) {
      const int i = s - k;
      if (i < 0 && !closed_before) {
        break;
      }
      const int wrapped = (i + n) % n;
      if (!removed[wrapped]) {
        start = new_index[wrapped];
        break;
      }
    }
    /* ...to the first kept point after it, which must be there. */
    bool end = false;
    for (int k = 1; k <= n; k++) {
      const int i = s + k;
      if (i >= n && !closed_before) {
        break;
      }
      if (!removed[i % n]) {
        end = true;
        break;
      }
    }
    if (start < 0 || !end || m < 2) {
      continue;
    }
    const int new_segments = closed_after ? m : m - 1;
    map[s] = start < new_segments ? start : -1;
  }
  return map;
}

void drawing_remove_points(VectorDrawing &drawing, const std::vector<bool> &remove)
{
  DetailScope detail_scope(drawing);
  std::vector<int> point_remap(drawing.points.size(), -1);
  std::vector<MeshPoint> points;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (!remove[i]) {
      point_remap[i] = int(points.size());
      points.push_back(drawing.points[i]);
    }
  }

  std::vector<int> curve_remap(drawing.curves.size(), -1);
  std::vector<std::vector<int>> segment_remap(drawing.curves.size());
  std::vector<Curve> curves;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve curve = drawing.curves[c];
    const bool closed_before = curve.closed;
    std::vector<bool> removed;
    for (const CurvePoint &cp : curve.points) {
      removed.push_back(remove[cp.point]);
    }
    std::erase_if(curve.points, [&](const CurvePoint &cp) { return remove[cp.point]; });
    for (CurvePoint &cp : curve.points) {
      cp.point = point_remap[cp.point];
    }
    if (curve.closed && curve.points.size() < 3) {
      curve.closed = false;
    }
    if (curve.points.size() != removed.size()) {
      /* Shapes along it keep going round where the curve joins across the gap. */
      segment_remap[c] = curve_segment_remap(removed, closed_before, curve.closed);
    }
    if (curve.points.size() >= 2) {
      curve_remap[c] = int(curves.size());
      curves.push_back(std::move(curve));
    }
  }
  shapes_remap_curves(drawing, curve_remap, segment_remap);
  drawing.points = std::move(points);
  drawing.curves = std::move(curves);
  /* A point whose curve went with its neighbours (a line of two, one end deleted) stays, on no curve, as
   * Blender's Delete Vertices leaves the other end of an edge: only what was picked goes. */
  /* The points gone leave their groups. */
  drawing_groups_prune(drawing);
}

int drawing_merge_points(VectorDrawing &drawing, const std::vector<std::vector<int>> &groups)
{
  DetailScope detail_scope(drawing);
  drawing_ensure_ids(drawing);
  const int n_points = int(drawing.points.size());
  /* Where each point goes: the first of its group. */
  std::vector<int> into(static_cast<size_t>(n_points));
  std::iota(into.begin(), into.end(), 0);
  int merged = 0;
  for (const std::vector<int> &group : groups) {
    if (group.size() < 2) {
      continue;
    }
    const int keep = group.front();
    float2 middle(0.0f, 0.0f);
    for (const int p : group) {
      middle = middle + drawing.points[size_t(p)].pos;
      into[size_t(p)] = keep;
    }
    drawing.points[size_t(keep)].pos = middle / float(group.size());
    drawing.points[size_t(keep)].selected = true;
    merged += int(group.size()) - 1;
  }
  if (merged == 0) {
    return 0;
  }

  /* The shapes each merged point was on, by id, to put together the ones that met only at some of them. */
  std::vector<std::vector<uint64_t>> on_shapes(static_cast<size_t>(n_points));
  for (const Shape &shape : drawing.shapes) {
    for (const ShapeEdge &e : shape.edges) {
      int a, b;
      curve_segment_points(drawing.curves[size_t(e.curve)], e.segment, &a, &b);
      for (const int p : {a, b}) {
        if (std::find(on_shapes[size_t(p)].begin(), on_shapes[size_t(p)].end(), shape.id) == on_shapes[size_t(p)].end()) {
          on_shapes[size_t(p)].push_back(shape.id);
        }
      }
    }
  }

  /* Curves run through the kept point; where two of a group came one after the other, the one after goes, the
   * curve going on from the kept one (its handle out the one that went's). */
  std::vector<int> curve_remap(drawing.curves.size(), -1);
  std::vector<std::vector<int>> segment_remap(drawing.curves.size());
  std::vector<Curve> curves;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve curve = drawing.curves[c];
    const bool closed_before = curve.closed;
    for (CurvePoint &cp : curve.points) {
      cp.point = into[size_t(cp.point)];
    }
    const int n = int(curve.points.size());
    std::vector<bool> removed(size_t(n), false);
    for (int k = 1; k < n; k++) {
      int before = k - 1;
      while (before > 0 && removed[size_t(before)]) {
        before--;
      }
      if (curve.points[size_t(k)].point == curve.points[size_t(before)].point) {
        removed[size_t(k)] = true;
        curve.points[size_t(before)].handle_out = curve.points[size_t(k)].handle_out;
        curve.points[size_t(before)].handles_set = curve.points[size_t(before)].handles_set ||
                                                   curve.points[size_t(k)].handles_set;
      }
    }
    /* A loop's last point on its first. */
    if (curve.closed && n > 1 && !removed[0]) {
      int last = n - 1;
      while (last > 0 && removed[size_t(last)]) {
        last--;
      }
      if (last > 0 && curve.points[size_t(last)].point == curve.points[0].point) {
        removed[size_t(last)] = true;
        curve.points[0].handle_in = curve.points[size_t(last)].handle_in;
      }
    }
    std::vector<CurvePoint> kept;
    for (int k = 0; k < n; k++) {
      if (!removed[size_t(k)]) {
        kept.push_back(curve.points[size_t(k)]);
      }
    }
    curve.points = std::move(kept);
    if (curve.closed && curve.points.size() < 3) {
      curve.closed = false;
    }
    if (int(curve.points.size()) != n) {
      segment_remap[c] = curve_segment_remap(removed, closed_before, curve.closed);
    }
    if (curve.points.size() >= 2) {
      curve_remap[c] = int(curves.size());
      curves.push_back(std::move(curve));
    }
  }
  shapes_remap_curves(drawing, curve_remap, segment_remap);
  drawing.curves = std::move(curves);

  /* Shapes that met only at some of a group's points become one: a line with a line, a fill with a fill (a shape
   * on all of them, as a fill and its outline on one curve, stays as it is). */
  for (const std::vector<int> &group : groups) {
    if (group.size() < 2) {
      continue;
    }
    std::vector<uint64_t> all;
    for (const int p : group) {
      for (const uint64_t id : on_shapes[size_t(p)]) {
        if (std::find(all.begin(), all.end(), id) == all.end()) {
          all.push_back(id);
        }
      }
    }
    const auto on_every = [&](uint64_t id) {
      return std::all_of(group.begin(), group.end(), [&](int p) {
        return std::find(on_shapes[size_t(p)].begin(), on_shapes[size_t(p)].end(), id) != on_shapes[size_t(p)].end();
      });
    };
    for (const bool fill : {true, false}) {
      std::vector<size_t> together;
      for (size_t s = 0; s < drawing.shapes.size(); s++) {
        const Shape &shape = drawing.shapes[s];
        if (shape.has_fill == fill && std::find(all.begin(), all.end(), shape.id) != all.end() && !on_every(shape.id)) {
          together.push_back(s);
        }
      }
      if (together.size() < 2) {
        continue;
      }
      Shape &first = drawing.shapes[together.front()];
      for (size_t k = 1; k < together.size(); k++) {
        for (const ShapeEdge &e : drawing.shapes[together[k]].edges) {
          if (std::find(first.edges.begin(), first.edges.end(), e) == first.edges.end()) {
            first.edges.push_back(e);
          }
        }
      }
      for (size_t k = together.size() - 1; k >= 1; k--) {
        drawing.shapes.erase(drawing.shapes.begin() + std::ptrdiff_t(together[k]));
      }
    }
  }

  /* The points that went (no curve uses them now). */
  std::vector<bool> gone(drawing.points.size(), false);
  for (int p = 0; p < n_points; p++) {
    gone[size_t(p)] = into[size_t(p)] != p;
  }
  drawing_remove_points(drawing, gone);
  return merged;
}

std::vector<std::vector<int>> drawing_points_within(const VectorDrawing &drawing, float distance)
{
  std::vector<int> selected;
  for (int p = 0; p < int(drawing.points.size()); p++) {
    if (drawing.points[size_t(p)].selected) {
      selected.push_back(p);
    }
  }
  /* Those within reach of one another, by way of each other (union-find). */
  std::vector<int> parent(selected.size());
  std::iota(parent.begin(), parent.end(), 0);
  const auto root = [&](int i) {
    while (parent[size_t(i)] != i) {
      parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
      i = parent[size_t(i)];
    }
    return i;
  };
  for (size_t i = 0; i < selected.size(); i++) {
    for (size_t j = i + 1; j < selected.size(); j++) {
      if (length(drawing.points[size_t(selected[i])].pos - drawing.points[size_t(selected[j])].pos) <= distance) {
        parent[size_t(root(int(j)))] = root(int(i));
      }
    }
  }
  std::map<int, std::vector<int>> by_root;
  for (size_t i = 0; i < selected.size(); i++) {
    by_root[root(int(i))].push_back(selected[i]);
  }
  std::vector<std::vector<int>> groups;
  for (auto &[r, group] : by_root) {
    if (group.size() > 1) {
      groups.push_back(std::move(group));
    }
  }
  return groups;
}

void drawing_remove_curves(VectorDrawing &drawing, const std::vector<bool> &remove)
{
  std::vector<int> curve_remap(drawing.curves.size(), -1);
  std::vector<Curve> curves;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    if (!remove[c]) {
      curve_remap[c] = int(curves.size());
      curves.push_back(drawing.curves[c]);
    }
  }
  shapes_remap_curves(drawing, curve_remap, std::vector<std::vector<int>>(drawing.curves.size()));
  drawing.curves = std::move(curves);
}

CurveEnd drawing_curve_end_at(const VectorDrawing &drawing, int point)
{
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    if (curve.closed || curve.points.empty()) {
      continue;
    }
    if (curve.points.back().point == point) {
      return {int(c), true};
    }
    if (curve.points.front().point == point) {
      return {int(c), false};
    }
  }
  return {};
}

int drawing_extrude_branch(VectorDrawing &drawing, int point, const Style &style)
{
  const std::vector<ShapeEdge> touching = drawing_segments_at_point(drawing, point);
  const auto through = [&](const Shape &shape) {
    for (const ShapeEdge &e : shape.edges) {
      if (std::find(touching.begin(), touching.end(), e) != touching.end()) {
        return true;
      }
    }
    return false;
  };
  const int added = drawing_add_point(drawing, drawing.points[size_t(point)].pos);
  Curve curve;
  curve.closed = false;
  curve.points.push_back({point});
  curve.points.push_back({added});
  drawing.curves.push_back(std::move(curve));
  const ShapeEdge edge{int(drawing.curves.size()) - 1, 0};
  for (Shape &shape : drawing.shapes) {
    if (shape.has_outline && !shape.has_fill && through(shape)) {
      shape.edges.push_back(edge);
      return added;
    }
  }
  Shape line;
  line.name = "Line";
  line.style = style;
  for (const Shape &shape : drawing.shapes) {
    if (through(shape)) {
      line.name = shape.name;
      line.style = shape.style;
      line.material = shape.material;
      line.detail_level = shape.detail_level;
      break;
    }
  }
  line.has_outline = true;
  line.has_fill = false;
  line.edges.push_back(edge);
  drawing.shapes.push_back(std::move(line));
  return added;
}

void drawing_extrude_align(VectorDrawing &drawing, int point)
{
  const CurveEnd end = drawing_curve_end_at(drawing, point);
  if (end.curve < 0 || !end.at_end) {
    return;
  }
  Curve &curve = drawing.curves[size_t(end.curve)];
  const int n = int(curve.points.size());
  /* A branch or a lone point has no way it was going: its new segment runs straight, worked out from the points. */
  if (n < 3) {
    return;
  }
  const CurvePoint &grew_from = curve.points[size_t(n - 2)];
  const float2 here = drawing.points[size_t(grew_from.point)].pos;
  const float2 before = drawing.points[size_t(curve.points[size_t(n - 3)].point)].pos;
  const float2 chord = drawing.points[size_t(point)].pos - here;
  const float2 in = curve_handle(drawing, curve, n - 2, true) - here;
  const float chord_len = length(chord);
  const bool corner = !grew_from.aligned && grew_from.curvature <= 0.0f;
  if (chord_len < 1e-6f || (length(in) < 1e-6f && length(here - before) < 1e-6f)) {
    return;
  }
  own_handles(drawing, curve, n - 2);
  own_handles(drawing, curve, n - 1);
  /* New shape, not an edit: without keys it is what the setup holds too. */
  const auto set_handle = [](CurvePoint &cp, bool pre, float2 value) {
    (pre ? cp.handle_in : cp.handle_out) = value;
    Channel<float2> &ch = pre ? cp.handle_in_keys : cp.handle_out_keys;
    if (!ch.animated()) {
      (pre ? cp.setup_in : cp.setup_out) = value;
      (pre ? cp.change_in : cp.change_out) = {};
      ch.last = {};
      ch.has_last = true;
    }
  };
  CurvePoint &from = curve.points[size_t(n - 2)], &to = curve.points[size_t(n - 1)];
  if (corner) {
    /* A corner stays a corner: straight toward the new point, as a peaked point's handles run. */
    set_handle(from, false, chord / 3.0f);
    set_handle(to, true, chord / -3.0f);
    return;
  }
  /* The way the curve is going, and at the new end that way mirrored across the chord: one even bend. */
  const float2 way = length(in) > 1e-6f ? -normalize(in) : normalize(here - before);
  const float2 along = chord / chord_len;
  const float2 arrives = along * (2.0f * dot(way, along)) - way;
  /* Shorter the further it turns (none straight back), so a sharp turn doesn't swing out in a loop first. */
  const float reach = chord_len / 3.0f * (1.0f + dot(way, along)) * 0.5f;
  set_handle(from, false, way * reach);
  set_handle(to, true, arrives * -reach);
  from.aligned = to.aligned = true;
}

std::vector<int> drawing_extrude_selected(VectorDrawing &drawing, const Style &style)
{
  std::vector<int> new_points;
  const int count = int(drawing.points.size());
  for (int i = 0; i < count; i++) {
    if (!drawing.points[size_t(i)].selected) {
      continue;
    }
    const CurveEnd end = drawing_curve_end_at(drawing, i);
    if (end.curve < 0) {
      new_points.push_back(drawing_extrude_branch(drawing, i, style));
      continue;
    }
    if (!end.at_end) {
      drawing_reverse_curve(drawing, end.curve);
    }
    const int added = drawing_add_point(drawing, drawing.points[size_t(i)].pos);
    Curve &curve = drawing.curves[size_t(end.curve)];
    /* The end had no handle out (nothing to lead to); now it leads on to the new point, so it takes one the way
     * the curve came in, as long as its handle in, and the curve runs on smoothly rather than turning there. */
    CurvePoint &was_end = curve.points.back();
    if (was_end.handles_set && (was_end.aligned || was_end.curvature > 0.0f) && length(was_end.handle_out) < 1e-6f &&
        length(was_end.handle_in) > 1e-6f)
    {
      was_end.handle_out = -was_end.handle_in;
    }
    curve.points.push_back({added});
    /* Shapes that cover the whole curve grow with it. */
    const int new_segment = curve.segment_count() - 1;
    for (Shape &shape : drawing.shapes) {
      int on_curve = 0;
      for (const ShapeEdge &e : shape.edges) {
        on_curve += e.curve == end.curve;
      }
      if (on_curve == new_segment && !shape.has_fill) {
        shape.edges.push_back({end.curve, new_segment});
      }
    }
    new_points.push_back(added);
  }
  for (MeshPoint &p : drawing.points) {
    p.selected = false;
  }
  drawing_deselect_handles(drawing);
  for (const int p : new_points) {
    drawing.points[size_t(p)].selected = true;
  }
  return new_points;
}

std::vector<ShapeEdge> drawing_segments_at_point(const VectorDrawing &drawing, int point)
{
  std::vector<ShapeEdge> result;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    for (int s = 0; s < curve.segment_count(); s++) {
      int a, b;
      curve_segment_points(curve, s, &a, &b);
      if (a == point || b == point) {
        result.push_back({int(c), s});
      }
    }
  }
  return result;
}

int drawing_pick_point(const VectorDrawing &drawing, float2 pos, float radius)
{
  int best = -1;
  float best_dist = radius;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    const float d = distance(drawing.points[i].pos, pos);
    /* Detail above its curve's edit level can't be picked. */
    if (d <= best_dist && !drawing.points[i].detail.hidden) {
      best_dist = d;
      best = int(i);
    }
  }
  return best;
}

static constexpr int PICK_STEPS = 24;

bool drawing_pick_segment(const VectorDrawing &drawing, float2 pos, float radius, ShapeEdge *r_edge, float *r_t)
{
  float best = radius;
  bool found = false;
  std::vector<float2> samples;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    for (int s = 0; s < curve.segment_count(); s++) {
      float2 ctrl[4];
      curve_segment_bezier(drawing, curve, s, ctrl);
      float2 prev = ctrl[0];
      for (int i = 1; i <= PICK_STEPS; i++) {
        const float2 p = bezier_eval(ctrl, float(i) / PICK_STEPS);
        float t_local;
        const float d = distance_to_segment(pos, prev, p, &t_local);
        if (d <= best) {
          best = d;
          found = true;
          *r_edge = {int(c), s};
          *r_t = (float(i - 1) + t_local) / PICK_STEPS;
        }
        prev = p;
      }
    }
  }
  return found;
}

int drawing_pick_shape(const VectorDrawing &drawing, float2 pos, float radius)
{
  std::vector<float2> outline;
  for (int s = int(drawing.shapes.size()) - 1; s >= 0; s--) {
    const Shape &shape = drawing.shapes[s];
    /* Inside its fill (not in a hole), or on a line round any of its runs. */
    if (shape.has_fill && shape_fill_contains(drawing, shape, pos)) {
      return s;
    }
    for (const Shape &run : shape_runs(drawing, shape)) {
      bool closed;
      shape_outline(drawing, run, 12, outline, nullptr, &closed);
      const size_t n = outline.size();
      for (size_t i = 0; n >= 2 && i + 1 < n + (closed ? 1 : 0); i++) {
        if (distance_to_segment(pos, outline[i], outline[(i + 1) % n]) <= radius) {
          return s;
        }
      }
    }
  }
  return -1;
}

int drawing_split_segment(VectorDrawing &drawing, ShapeEdge edge, float t)
{
  Curve &curve = drawing.curves[edge.curve];
  float2 ctrl[4];
  curve_segment_bezier(drawing, curve, edge.segment, ctrl);
  const int point = drawing_add_point(drawing, bezier_eval(ctrl, t));
  /* The line's width there, which runs by distance along the segment, not by the curve parameter. */
  {
    float upto = 0.0f, total = 0.0f;
    float2 last = ctrl[0];
    for (int i = 1; i <= 32; i++) {
      const float2 p = bezier_eval(ctrl, float(i) / 32.0f);
      total += distance(last, p);
      upto += float(i) / 32.0f <= t ? distance(last, p) : 0.0f;
      last = p;
    }
    int from = 0, to = 0;
    curve_segment_points(curve, edge.segment, &from, &to);
    const float r0 = drawing.points[from].radius, r1 = drawing.points[to].radius;
    drawing.points[point].radius = r0 + (r1 - r0) * (total > 1e-12f ? upto / total : t);
  }
  /* A point added between two of a group's is in it, weighted between theirs (as Blender's
   * Subdivide does). */
  if (!drawing.groups.empty()) {
    drawing_ensure_ids(drawing);
    const uint64_t a = drawing.points[curve.points[edge.segment].point].id;
    const uint64_t b = drawing.points[curve.points[(edge.segment + 1) % int(curve.points.size())].point].id;
    for (PointGroup &g : drawing.groups) {
      const auto wa = g.weights.find(a), wb = g.weights.find(b);
      if (wa != g.weights.end() && wb != g.weights.end()) {
        g.weights[drawing.points[point].id] = wa->second + (wb->second - wa->second) * t;
      }
    }
  }

  /* Split the Bezier exactly (de Casteljau), so the line doesn't move: the ends' facing handles
   * shorten, and the new point gets aligned handles along the curve. */
  CurvePoint cp;
  cp.point = point;
  const int n = int(curve.points.size());
  own_handles(drawing, curve, edge.segment);
  own_handles(drawing, curve, (edge.segment + 1) % n);
  CurvePoint &from = curve.points[edge.segment];
  CurvePoint &to = curve.points[(edge.segment + 1) % n];
  const auto lerp2 = [](float2 a, float2 b, float u) { return a + (b - a) * u; };
  const float2 p01 = lerp2(ctrl[0], ctrl[1], t), p12 = lerp2(ctrl[1], ctrl[2], t), p23 = lerp2(ctrl[2], ctrl[3], t);
  const float2 p012 = lerp2(p01, p12, t), p123 = lerp2(p12, p23, t);
  const float2 mid = drawing.points[point].pos;
  from.handle_out = p01 - ctrl[0];
  to.handle_in = p23 - ctrl[3];
  cp.aligned = true;
  cp.handles_set = true;
  cp.handle_in = p012 - mid;
  cp.handle_out = p123 - mid;
  cp.curvature = 0.5f * (curve.points[edge.segment].curvature + curve.points[(edge.segment + 1) % n].curvature);
  cp.edge_hidden = curve.points[edge.segment].edge_hidden;
  if (cp.curvature > 0.0f) {
    cp.curvature = std::max(cp.curvature, CURVATURE_SMOOTH);
  }
  curve.points.insert(curve.points.begin() + edge.segment + 1, cp);

  /* Segments after the split move up by one; the split segment becomes two. */
  for (Shape &shape : drawing.shapes) {
    std::vector<ShapeEdge> edges;
    for (const ShapeEdge &e : shape.edges) {
      if (e.curve != edge.curve) {
        edges.push_back(e);
      }
      else if (e.segment < edge.segment) {
        edges.push_back(e);
      }
      else if (e.segment == edge.segment) {
        edges.push_back({e.curve, e.segment});
        edges.push_back({e.curve, e.segment + 1});
      }
      else {
        edges.push_back({e.curve, e.segment + 1});
      }
    }
    shape.edges = std::move(edges);
  }
  return point;
}

void drawing_remove_unused_points(VectorDrawing &drawing)
{
  std::vector<int> remap(drawing.points.size(), -1);
  std::vector<bool> used(drawing.points.size(), false);
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      used[cp.point] = true;
    }
  }
  std::vector<MeshPoint> points;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (used[i]) {
      remap[i] = int(points.size());
      points.push_back(drawing.points[i]);
    }
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.point = remap[cp.point];
    }
  }
  drawing.points = std::move(points);
}

/* Where segment s goes when curve point i is removed from a curve of n points, or -1 if the
 * segment disappears (an end segment of an open curve). */
static int segment_after_removal(int s, int i, int n, bool closed)
{
  if (!closed) {
    if (i == 0) {
      return s == 0 ? -1 : s - 1;
    }
    if (i == n - 1) {
      return s == n - 2 ? -1 : s;
    }
  }
  else if (i == 0) {
    /* The last segment and the first join into the new last segment. */
    return (s == 0 || s == n - 1) ? n - 2 : s - 1;
  }
  /* Segments i-1 and i join into i-1; later ones move down. */
  if (s == i) {
    return i - 1;
  }
  return s > i ? s - 1 : s;
}

void drawing_dissolve_point(VectorDrawing &drawing, int point)
{
  DetailScope detail_scope(drawing);
  std::vector<bool> remove_curve(drawing.curves.size(), false);
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve &curve = drawing.curves[c];
    for (int i = int(curve.points.size()) - 1; i >= 0; i--) {
      if (curve.points[i].point != point) {
        continue;
      }
      const int n = int(curve.points.size());
      for (Shape &shape : drawing.shapes) {
        std::vector<ShapeEdge> edges;
        for (ShapeEdge e : shape.edges) {
          if (e.curve == int(c)) {
            e.segment = segment_after_removal(e.segment, i, n, curve.closed);
            if (e.segment < 0) {
              continue;
            }
          }
          if (std::find(edges.begin(), edges.end(), e) == edges.end()) {
            edges.push_back(e);
          }
        }
        shape.edges = std::move(edges);
      }
      curve.points.erase(curve.points.begin() + i);
      if (curve.closed && curve.points.size() < 3) {
        /* Too few points to stay closed: keep the one segment left as an open line. */
        curve.closed = false;
        for (Shape &shape : drawing.shapes) {
          std::erase_if(shape.edges, [&](const ShapeEdge &e) {
            return e.curve == int(c) && e.segment >= curve.segment_count();
          });
        }
      }
    }
    if (curve.points.size() < 2) {
      remove_curve[c] = true;
    }
  }
  std::erase_if(drawing.shapes, [](const Shape &shape) { return shape.edges.empty(); });
  drawing_remove_curves(drawing, remove_curve);
  drawing_remove_unused_points(drawing);
  /* As Delete: the point leaves its groups, and a grid it was a node of is ordinary drawing from then on. */
  drawing_groups_prune(drawing);
}

/* Schneider's fit of one cubic from `p0` to `p3`, its handles along `t1` (out of p0) and `t2` (out of
 * p3, back along the curve), to the samples `d`: the two handle lengths, by least squares, with the
 * samples' places along the curve refined by Newton steps. */
/* The handles (as offsets: `r_h1` from p0, `r_h2` from p3) of the cubic from p0 to p3 nearest the samples
 * `pts` (least squares). The samples were made evenly spaced in the segment's own parameter, and keep it as
 * they move: fitted at those parameters, samples nobody moved give back the curve they came from exactly
 * (guessing the parameters from distances along them bends sharply curved segments out of shape). A
 * handle given (`fix1`, `fix2`) stays as it is; `was` is the longest the segment's handles were. */
void bezier_fit_handles(const std::vector<float2> &pts, const std::vector<float> &params, float2 p0, float2 p3,
                        const float2 *fix1, const float2 *fix2, float was, float2 *r_h1, float2 *r_h2)
{
  const size_t m = pts.size();
  float total = 0.0f;
  for (size_t k = 1; k < m; k++) {
    total += distance(pts[k], pts[k - 1]);
  }
  const float2 chord = p3 - p0;
  float2 h1 = fix1 ? *fix1 : chord / 3.0f, h2 = fix2 ? *fix2 : -chord / 3.0f;
  if (m > 2) {
    float s11 = 0.0f, s12 = 0.0f, s22 = 0.0f;
    float2 r1, r2;
    for (size_t k = 0; k < m; k++) {
      const float t = params[k], s = 1.0f - t;
      const float b0 = s * s * s, b1 = 3.0f * s * s * t, b2 = 3.0f * s * t * t, b3 = t * t * t;
      const float2 rest = pts[k] - (p0 * (b0 + b1) + p3 * (b2 + b3));
      s11 += b1 * b1;
      s12 += b1 * b2;
      s22 += b2 * b2;
      r1 += rest * b1;
      r2 += rest * b2;
    }
    if (fix1 && fix2) {
      /* Both kept. */
    }
    else if (fix1) {
      h2 = s22 > 1e-12f ? (r2 - h1 * s12) / s22 : h2;
    }
    else if (fix2) {
      h1 = s11 > 1e-12f ? (r1 - h2 * s12) / s11 : h1;
    }
    else {
      const float det = s11 * s22 - s12 * s12;
      if (det > 1e-12f * s11 * s22) {
        h1 = (r1 * s22 - r2 * s12) / det;
        h2 = (r2 * s11 - r1 * s12) / det;
      }
    }
  }
  /* A handle reaching far past the segment is a fit gone wrong: a third of the way along. */
  const float most = std::max({length(chord), total, was}) * 1.5f;
  if (!fix1 && length(h1) > most) {
    h1 = chord / 3.0f;
  }
  if (!fix2 && length(h2) > most) {
    h2 = -chord / 3.0f;
  }
  *r_h1 = h1;
  *r_h2 = h2;
}

void bezier_fit_handle_lengths(float2 p0, float2 p3, float2 t1, float2 t2, const std::vector<float2> &d,
                               float *r_a1, float *r_a2, bool steady)
{
  const float chord = distance(p0, p3);
  *r_a1 = *r_a2 = chord / 3.0f;
  const size_t n = d.size();
  if (n < 3 || chord < 1e-9f) {
    return;
  }
  /* Places along the curve by distance along the samples. */
  std::vector<float> u(n, 0.0f);
  for (size_t i = 1; i < n; i++) {
    u[i] = u[i - 1] + distance(d[i], d[i - 1]);
  }
  for (float &x : u) {
    x /= std::max(u.back(), 1e-9f);
  }
  float a1 = chord / 3.0f, a2 = chord / 3.0f;
  for (int round = 0; round < 24; round++) {
    double c00 = 0, c01 = 0, c11 = 0, x0 = 0, x1 = 0;
    for (size_t i = 0; i < n; i++) {
      const float t = u[i], s = 1.0f - t;
      const float b0 = s * s * s, b1 = 3 * s * s * t, b2 = 3 * s * t * t, b3 = t * t * t;
      const float2 A1 = t1 * b1, A2 = t2 * b2;
      const float2 rest = d[i] - (p0 * (b0 + b1) + p3 * (b2 + b3));
      c00 += dot(A1, A1);
      c01 += dot(A1, A2);
      c11 += dot(A2, A2);
      x0 += dot(rest, A1);
      x1 += dot(rest, A2);
    }
    const double det = c00 * c11 - c01 * c01;
    if (std::fabs(det) < 1e-12) {
      break;
    }
    a1 = float((x0 * c11 - x1 * c01) / det);
    a2 = float((c00 * x1 - c01 * x0) / det);
    /* A handle turned back or vanishing means no good fit this way: thirds of the chord (steady: held short, and on). */
    if (steady) {
      /* And never longer than the chord: longer, the segment loops on itself. */
      a1 = std::clamp(a1, chord * 1e-3f, chord);
      a2 = std::clamp(a2, chord * 1e-3f, chord);
    }
    else if (a1 < chord * 1e-3f || a2 < chord * 1e-3f) {
      a1 = a2 = chord / 3.0f;
      break;
    }
    /* Newton steps: each sample's place moves to the nearest point of the curve as now fitted. */
    const float2 c[4] = {p0, p0 + t1 * a1, p3 + t2 * a2, p3};
    for (size_t i = 1; i + 1 < n; i++) {
      const float t = u[i], s = 1.0f - t;
      const float2 q = bezier_eval(c, t);
      const float2 q1 = (c[1] - c[0]) * (3 * s * s) + (c[2] - c[1]) * (6 * s * t) + (c[3] - c[2]) * (3 * t * t);
      const float2 q2 = (c[2] - c[1] * 2.0f + c[0]) * (6 * s) + (c[3] - c[2] * 2.0f + c[1]) * (6 * t);
      const float num = dot(q - d[i], q1), den = dot(q1, q1) + dot(q - d[i], q2);
      if (std::fabs(den) > 1e-12f) {
        u[i] = std::clamp(t - num / den, 0.0f, 1.0f);
      }
    }
  }
  *r_a1 = a1;
  *r_a2 = a2;
}

void drawing_dissolve_points(VectorDrawing &drawing, const std::vector<int> &points)
{
  /* Points are followed by id through the removals. */
  drawing_ensure_ids(drawing);
  std::vector<bool> gone(drawing.points.size(), false);
  for (const int p : points) {
    if (p >= 0 && p < int(gone.size())) {
      gone[p] = true;
    }
  }
  /* First, while the curves are as they were: each span from a kept point over removed ones to the
   * next kept point, sampled, and the handle lengths that fit one segment to it. */
  struct Fit {
    uint64_t from = 0, to = 0;
    float2 out, in;
  };
  std::vector<Fit> fits;
  /* Detail points that go on their own (see below). */
  std::vector<uint64_t> detail_ids;
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    /* The spans: from a kept point over removed ones to the next kept point. */
    struct Span {
      int a = 0, b = 0, steps = 0;
    };
    std::vector<Span> spans;
    for (int a = 0; a < n; a++) {
      if (gone[curve.points[a].point] || (!curve.closed && a == n - 1)) {
        continue;
      }
      /* The run of removed points after a, and the kept point that ends it. */
      int b = a, steps = 0;
      do {
        b = (b + 1) % n;
        steps++;
      } while (gone[curve.points[b].point] && steps < n && (curve.closed || b < n - 1));
      if (steps < 2 || gone[curve.points[b].point] || b == a || (!curve.closed && b < a)) {
        continue;
      }
      /* Removed points that are all detail above both kept ones (the knife's, at a detail level) ride
       * on the curve of the level below, which goes on as it was: nothing to fit. */
      const int ends = std::max(drawing.points[curve.points[a].point].detail.level,
                                drawing.points[curve.points[b].point].detail.level);
      bool detail_only = true;
      for (int k = 1; k < steps; k++) {
        detail_only &= drawing.points[curve.points[(a + k) % n].point].detail.level > ends;
      }
      if (!detail_only) {
        spans.push_back({a, b, steps});
      }
      else {
        for (int k = 1; k < steps; k++) {
          detail_ids.push_back(drawing.points[curve.points[(a + k) % n].point].id);
        }
      }
    }
    /* The ends of a span keep their handles as they show now (worked out from their neighbours, they
     * would change with a neighbour gone). */
    for (const Span &s : spans) {
      own_handles(drawing, curve, s.a);
      own_handles(drawing, curve, s.b);
    }
    for (const Span &s : spans) {
      const int a = s.a, b = s.b, steps = s.steps;
      std::vector<float2> samples;
      float2 t1, t2;
      for (int k = 0; k < steps; k++) {
        float2 ctrl[4];
        curve_segment_bezier(drawing, curve, (a + k) % n, ctrl);
        if (k == 0) {
          t1 = length(ctrl[1] - ctrl[0]) > 1e-9f ? ctrl[1] - ctrl[0] : ctrl[3] - ctrl[0];
        }
        if (k == steps - 1) {
          t2 = length(ctrl[2] - ctrl[3]) > 1e-9f ? ctrl[2] - ctrl[3] : ctrl[0] - ctrl[3];
        }
        for (int j = k == 0 ? 0 : 1; j <= 24; j++) {
          samples.push_back(bezier_eval(ctrl, float(j) / 24.0f));
        }
      }
      if (length(t1) < 1e-9f || length(t2) < 1e-9f) {
        continue;
      }
      t1 = normalize(t1);
      t2 = normalize(t2);
      float a1 = 0.0f, a2 = 0.0f;
      bezier_fit_handle_lengths(samples.front(), samples.back(), t1, t2, samples, &a1, &a2);
      fits.push_back({drawing.points[curve.points[a].point].id, drawing.points[curve.points[b].point].id, t1 * a1, t2 * a2});
    }
  }
  /* Then the points go: detail riding on the level below as Delete Higher removes it (that level's
   * handles kept, so the curve is exactly what it was before the knife), the rest one at a time,
   * highest index first as each dissolve renumbers the ones after it... */
  std::vector<uint64_t> other_ids;
  for (int i = 0; i < int(gone.size()); i++) {
    if (gone[i] && std::find(detail_ids.begin(), detail_ids.end(), drawing.points[i].id) == detail_ids.end()) {
      other_ids.push_back(drawing.points[i].id);
    }
  }
  if (!detail_ids.empty()) {
    drawing_detail_remove_points(drawing, detail_ids);
  }
  std::vector<int> order;
  for (const uint64_t id : other_ids) {
    if (const int p = drawing_point_index(drawing, id); p >= 0) {
      order.push_back(p);
    }
  }
  std::sort(order.begin(), order.end());
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    drawing_dissolve_point(drawing, *it);
  }
  /* ...and the joined segments take the fitted handles (the other handle of each point stays). */
  for (const Fit &f : fits) {
    const int from = drawing_point_index(drawing, f.from), to = drawing_point_index(drawing, f.to);
    if (from < 0 || to < 0) {
      continue;
    }
    for (Curve &curve : drawing.curves) {
      const int n = int(curve.points.size());
      for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        if (curve.points[i].point != from || curve.points[j].point != to || (!curve.closed && j == 0)) {
          continue;
        }
        curve_point_move_handle(drawing, curve, i, false, drawing.points[from].pos + f.out, true);
        curve_point_move_handle(drawing, curve, j, true, drawing.points[to].pos + f.in, true);
      }
    }
  }
}

void drawing_reverse_curve(VectorDrawing &drawing, int curve_index)
{
  DetailScope detail_scope(drawing);
  Curve &curve = drawing.curves[curve_index];
  const int segments = curve.segment_count();
  std::vector<bool> hidden(segments);
  for (int s = 0; s < segments; s++) {
    hidden[s] = curve.points[s].edge_hidden;
  }
  std::reverse(curve.points.begin(), curve.points.end());
  for (CurvePoint &cp : curve.points) {
    std::swap(cp.weight_in, cp.weight_out);
    std::swap(cp.offset_in, cp.offset_out);
    std::swap(cp.handle_in, cp.handle_out);
    std::swap(cp.select_in, cp.select_out);
    /* The setup pose and the animation go with the handles. */
    std::swap(cp.setup_in, cp.setup_out);
    std::swap(cp.change_in, cp.change_out);
    std::swap(cp.handle_in_keys, cp.handle_out_keys);
  }
  if (curve.closed) {
    /* Keep the same first point: [p0, p(n-1), ..., p1]. */
    std::rotate(curve.points.rbegin(), curve.points.rbegin() + 1, curve.points.rend());
  }
  /* Either way, old segment s now runs the other way as segment (segments - 1 - s). */
  for (int k = 0; k < int(curve.points.size()); k++) {
    curve.points[k].edge_hidden = k < segments && hidden[segments - 1 - k];
  }
  for (Shape &shape : drawing.shapes) {
    for (ShapeEdge &e : shape.edges) {
      if (e.curve == curve_index) {
        e.segment = segments - 1 - e.segment;
      }
    }
  }
}

/* How many of `shape`'s edges run to or from point `p`: 1 where one of its runs stops there (a shape can be
 * several runs, as a Moho stroke shape of loose lines). */
static int shape_edges_at(const VectorDrawing &drawing, const Shape &shape, int p)
{
  int count = 0;
  for (const ShapeEdge &e : shape.edges) {
    if (e.curve < 0 || e.curve >= int(drawing.curves.size())) {
      continue;
    }
    int from, to;
    curve_segment_points(drawing.curves[size_t(e.curve)], e.segment, &from, &to);
    count += (from == p ? 1 : 0) + (to == p ? 1 : 0);
  }
  return count;
}

/* Two points that aren't both curve ends (Blender's F between any two vertices): a new segment of their own, welded
 * to both, drawn as the line through either draws (a line's shape takes it as another run; a filled shape lends a
 * line-only copy of its look, so its fill keeps going round). */
static bool make_segment_between(VectorDrawing &drawing, int a, int b)
{
  for (const Curve &curve : drawing.curves) {
    for (int s = 0; s < curve.segment_count(); s++) {
      int from, to;
      curve_segment_points(curve, s, &from, &to);
      if ((from == a && to == b) || (from == b && to == a)) {
        return false;
      }
    }
  }
  DetailScope detail_scope(drawing);
  Curve curve;
  CurvePoint ca, cb;
  ca.point = a;
  cb.point = b;
  curve.points = {ca, cb};
  drawing.curves.push_back(std::move(curve));
  const ShapeEdge added{int(drawing.curves.size()) - 1, 0};
  int line = -1, look = -1;
  for (int s = int(drawing.shapes.size()) - 1; s >= 0; s--) {
    const Shape &shape = drawing.shapes[size_t(s)];
    if (shape_edges_at(drawing, shape, a) == 0 && shape_edges_at(drawing, shape, b) == 0) {
      continue;
    }
    look = look < 0 ? s : look;
    if (!shape.has_fill && line < 0) {
      line = s;
    }
  }
  if (line >= 0) {
    drawing.shapes[size_t(line)].edges.push_back(added);
    return true;
  }
  Shape shape;
  if (look >= 0) {
    shape.style = drawing.shapes[size_t(look)].style;
    shape.material = drawing.shapes[size_t(look)].material;
  }
  shape.name = "Line";
  shape.has_fill = false;
  shape.has_outline = true;
  /* A fill with no outline has no width to show the line with: the default one. */
  if (shape.style.line_width <= 0.0f) {
    shape.style.line_width = Style().line_width;
  }
  shape.edges.push_back(added);
  drawing.shapes.push_back(std::move(shape));
  drawing_ensure_ids(drawing);
  return true;
}

bool drawing_make_segment(VectorDrawing &drawing, int a, int b)
{
  const int n_points = int(drawing.points.size());
  if (a == b || a < 0 || b < 0 || a >= n_points || b >= n_points) {
    return false;
  }
  /* The open curves each point ends; both ends of one curve close it. */
  const auto ends_of = [&](int p) {
    std::vector<CurveEnd> ends;
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      const Curve &curve = drawing.curves[c];
      if (curve.closed || curve.points.size() < 2) {
        continue;
      }
      if (curve.points.front().point == p) {
        ends.push_back({c, false});
      }
      if (curve.points.back().point == p) {
        ends.push_back({c, true});
      }
    }
    return ends;
  };
  const std::vector<CurveEnd> ends_a = ends_of(a), ends_b = ends_of(b);
  if (ends_a.empty() || ends_b.empty()) {
    return make_segment_between(drawing, a, b);
  }
  CurveEnd end_a = ends_a.front(), end_b = ends_b.front();
  for (const CurveEnd &ea : ends_a) {
    for (const CurveEnd &eb : ends_b) {
      if (ea.curve == eb.curve && ea.at_end != eb.at_end) {
        end_a = ea;
        end_b = eb;
      }
    }
  }
  const bool close = end_a.curve == end_b.curve;
  if (close && drawing.curves[end_a.curve].points.size() < 3) {
    return make_segment_between(drawing, a, b);
  }
  if (!close && std::find_if(ends_b.begin(), ends_b.end(), [&](const CurveEnd &e) { return e.curve != end_a.curve; }) ==
                    ends_b.end())
  {
    return make_segment_between(drawing, a, b);
  }
  if (!close && end_b.curve == end_a.curve) {
    end_b = *std::find_if(ends_b.begin(), ends_b.end(), [&](const CurveEnd &e) { return e.curve != end_a.curve; });
  }
  DetailScope detail_scope(drawing);
  /* The shapes that stop at either point, before anything moves: one of their runs ends there (a shape of
   * several runs too). */
  int shape_a = -1, shape_b = -1;
  for (int s = 0; s < int(drawing.shapes.size()); s++) {
    const bool at_a = shape_edges_at(drawing, drawing.shapes[s], a) == 1;
    const bool at_b = shape_edges_at(drawing, drawing.shapes[s], b) == 1;
    if (close) {
      /* Closing: the one that runs from one end to the other. */
      if (shape_a < 0 && at_a && at_b) {
        shape_a = s;
      }
      continue;
    }
    if (shape_a < 0 && at_a) {
      shape_a = s;
    }
    if (shape_b < 0 && at_b) {
      shape_b = s;
    }
  }

  /* A handle with nothing to say about the new segment's way leans a third of the way along it. */
  const auto lean = [&](CurvePoint &cp, bool out, int toward) {
    float2 &h = out ? cp.handle_out : cp.handle_in;
    if (cp.handles_set && length(h) < 1e-6f) {
      h = (drawing.points[toward].pos - drawing.points[cp.point].pos) / 3.0f;
    }
  };
  ShapeEdge added;
  if (close) {
    Curve &curve = drawing.curves[end_a.curve];
    curve.closed = true;
    curve.points.back().edge_hidden = false;
    lean(curve.points.back(), true, curve.points.front().point);
    lean(curve.points.front(), false, curve.points.back().point);
    added = {end_a.curve, curve.segment_count() - 1};
  }
  else {
    /* `a` last on its curve, `b` first on its own: then one runs on into the other. */
    if (!end_a.at_end) {
      drawing_reverse_curve(drawing, end_a.curve);
    }
    if (end_b.at_end) {
      drawing_reverse_curve(drawing, end_b.curve);
    }
    const int ca = end_a.curve, cb = end_b.curve;
    Curve &first = drawing.curves[ca];
    const int na = int(first.points.size());
    first.points.back().edge_hidden = false;
    lean(first.points.back(), true, b);
    lean(drawing.curves[cb].points.front(), false, a);
    first.points.insert(first.points.end(), drawing.curves[cb].points.begin(), drawing.curves[cb].points.end());
    drawing.curves.erase(drawing.curves.begin() + cb);
    const int joined = ca > cb ? ca - 1 : ca;
    for (Shape &shape : drawing.shapes) {
      for (ShapeEdge &e : shape.edges) {
        if (e.curve == cb) {
          e = {joined, na + e.segment};
        }
        else if (e.curve == ca) {
          e.curve = joined;
        }
        else if (e.curve > cb) {
          e.curve--;
        }
      }
    }
    added = {joined, na - 1};
  }
  /* The shapes run on along it: one each side become one. */
  int grown = shape_a >= 0 ? shape_a : shape_b;
  if (grown >= 0) {
    drawing.shapes[grown].edges.push_back(added);
    if (shape_a >= 0 && shape_b >= 0 && shape_a != shape_b) {
      Shape &into = drawing.shapes[shape_a];
      const std::vector<ShapeEdge> more = drawing.shapes[shape_b].edges;
      into.edges.insert(into.edges.end(), more.begin(), more.end());
      drawing.shapes.erase(drawing.shapes.begin() + shape_b);
      grown = shape_b < shape_a ? shape_a - 1 : shape_a;
    }
    std::vector<OrientedEdge> chain;
    bool closed = false;
    if (shape_chain(drawing, drawing.shapes[grown], chain, &closed) && closed) {
      /* Finished: it goes all the way round. */
      drawing.shapes[grown].has_fill = true;
    }
  }
  return true;
}

bool drawing_selection_bounds(const VectorDrawing &drawing, float2 *r_min, float2 *r_max)
{
  bool any = false;
  float2 lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  float2 hi(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
  for (const MeshPoint &p : drawing.points) {
    if (p.selected) {
      any = true;
      lo = {std::min(lo.x, p.pos.x), std::min(lo.y, p.pos.y)};
      hi = {std::max(hi.x, p.pos.x), std::max(hi.y, p.pos.y)};
    }
  }
  *r_min = lo;
  *r_max = hi;
  return any;
}

bool drawing_duplicate_selected(VectorDrawing &drawing)
{
  DetailScope detail_scope(drawing);
  const int old_point_count = int(drawing.points.size());
  const int old_curve_count = int(drawing.curves.size());
  std::vector<int> point_copy(old_point_count, -1);
  const auto copy_point = [&](int p) {
    if (point_copy[p] < 0) {
      MeshPoint point = drawing.points[p];
      drawing.points.push_back(point);
      point_copy[p] = int(drawing.points.size()) - 1;
    }
    return point_copy[p];
  };
  /* (curve, segment) in the original to (curve, segment) in the copy, for shapes. */
  std::map<std::pair<int, int>, ShapeEdge> segment_copy;

  for (int c = 0; c < old_curve_count; c++) {
    const Curve curve = drawing.curves[c];
    const int n = int(curve.points.size());
    const auto selected = [&](int k) { return drawing.points[curve.points[k].point].selected; };
    int selected_count = 0;
    for (int k = 0; k < n; k++) {
      selected_count += selected(k);
    }
    if (selected_count < 2) {
      continue;
    }
    if (curve.closed && selected_count == n) {
      Curve copy = curve;
      for (CurvePoint &cp : copy.points) {
        cp.point = copy_point(cp.point);
      }
      drawing.curves.push_back(std::move(copy));
      for (int s = 0; s < curve.segment_count(); s++) {
        segment_copy[{c, s}] = {int(drawing.curves.size()) - 1, s};
      }
      continue;
    }
    /* Start each run at a selected point whose predecessor isn't selected; on a closed curve
     * a run may wrap past the last point. */
    for (int start = 0; start < n; start++) {
      const int prev = curve.closed ? (start + n - 1) % n : start - 1;
      if (!selected(start) || (prev >= 0 && selected(prev))) {
        continue;
      }
      std::vector<int> run;
      std::vector<int> segments;
      int k = start;
      for (int steps = 0; steps < n && selected(k); steps++) {
        run.push_back(k);
        const int next = k + 1;
        if (next >= n && !curve.closed) {
          break;
        }
        if (selected(next % n) && steps + 1 < n) {
          segments.push_back(k);
        }
        k = next % n;
      }
      /* A point alone on this curve copies nothing here (copied, it would be left on no curve). */
      if (run.size() < 2) {
        continue;
      }
      Curve copy;
      for (const int r : run) {
        CurvePoint cp = curve.points[r];
        cp.point = copy_point(cp.point);
        copy.points.push_back(cp);
      }
      drawing.curves.push_back(std::move(copy));
      const int new_curve = int(drawing.curves.size()) - 1;
      for (int i = 0; i < int(segments.size()); i++) {
        segment_copy[{c, segments[i]}] = {new_curve, i};
      }
    }
  }
  if (int(drawing.curves.size()) == old_curve_count) {
    drawing.points.resize(old_point_count);
    return false;
  }

  /* A shape the copied pieces wholly cover is copied as it is. One they cover only part of gives the copy a
   * shape of its own in the same look, once however many shapes drew those pieces: a piece of a stroke keeps
   * its line (without the fill it can't close); a piece of a fill with no line stays a fill with no line (it
   * shows once closed), never gaining a stroke the original didn't have. */
  const int old_shape_count = int(drawing.shapes.size());
  std::vector<std::vector<ShapeEdge>> lines;
  for (int s = 0; s < old_shape_count; s++) {
    Shape copy = drawing.shapes[s];
    std::vector<ShapeEdge> edges;
    for (const ShapeEdge &edge : copy.edges) {
      if (auto it = segment_copy.find({edge.curve, edge.segment}); it != segment_copy.end()) {
        edges.push_back(it->second);
      }
    }
    if (edges.empty()) {
      continue;
    }
    const bool covered = edges.size() == copy.edges.size();
    if (!covered) {
      std::vector<ShapeEdge> sorted = edges;
      std::sort(sorted.begin(), sorted.end(), [](const ShapeEdge &x, const ShapeEdge &y) {
        return x.curve != y.curve ? x.curve < y.curve : x.segment < y.segment;
      });
      if (std::find(lines.begin(), lines.end(), sorted) != lines.end()) {
        continue;
      }
      lines.push_back(std::move(sorted));
      if (copy.has_outline) {
        copy.has_fill = false;
      }
    }
    copy.edges = std::move(edges);
    drawing.shapes.push_back(std::move(copy));
  }

  for (int p = 0; p < int(drawing.points.size()); p++) {
    drawing.points[p].selected = p >= old_point_count;
  }
  /* The selection is the copy now: handles picked on their own stay behind no more (moving the copy would
   * swing them over with it, the original's points staying put). */
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.select_in = cp.select_out = false;
    }
  }
  return true;
}

std::vector<bool> drawing_selected_shapes(const VectorDrawing &drawing)
{
  std::vector<bool> selected(drawing.shapes.size(), false);
  for (size_t s = 0; s < drawing.shapes.size(); s++) {
    for (const ShapeEdge &edge : drawing.shapes[s].edges) {
      int a, b;
      curve_segment_points(drawing.curves[edge.curve], edge.segment, &a, &b);
      if (drawing.points[a].selected || drawing.points[b].selected) {
        selected[s] = true;
        break;
      }
    }
  }
  return selected;
}

/* Whether every point along the shape's edges is selected. */
static bool shape_all_selected(const VectorDrawing &drawing, const Shape &shape)
{
  for (const ShapeEdge &edge : shape.edges) {
    int a, b;
    curve_segment_points(drawing.curves[edge.curve], edge.segment, &a, &b);
    if (!drawing.points[a].selected || !drawing.points[b].selected) {
      return false;
    }
  }
  return !shape.edges.empty();
}

int drawing_join_fills(VectorDrawing &drawing)
{
  std::vector<int> picked;
  std::vector<float2> outline;
  for (int s = 0; s < int(drawing.shapes.size()); s++) {
    const Shape &shape = drawing.shapes[s];
    if (!shape_all_selected(drawing, shape)) {
      continue;
    }
    bool goes_round = false;
    for (const Shape &run : shape_runs(drawing, shape)) {
      bool closed = false;
      shape_outline(drawing, run, 1, outline, nullptr, &closed);
      goes_round |= closed;
    }
    if (goes_round) {
      picked.push_back(s);
    }
  }
  if (picked.size() < 2) {
    return 0;
  }
  Shape &joined = drawing.shapes[picked.front()];
  for (size_t k = 1; k < picked.size(); k++) {
    for (const ShapeEdge &edge : drawing.shapes[picked[k]].edges) {
      if (std::find(joined.edges.begin(), joined.edges.end(), edge) == joined.edges.end()) {
        joined.edges.push_back(edge);
      }
    }
  }
  joined.has_fill = true;
  if (joined.fill_rule == FillRule::Loops) {
    joined.fill_rule = FillRule::EvenOdd;
  }
  for (size_t k = picked.size() - 1; k >= 1; k--) {
    drawing.shapes.erase(drawing.shapes.begin() + picked[k]);
  }
  return int(picked.size());
}

int drawing_separate_fills(VectorDrawing &drawing)
{
  int made = 0;
  for (int s = int(drawing.shapes.size()) - 1; s >= 0; s--) {
    if (!shape_all_selected(drawing, drawing.shapes[s])) {
      continue;
    }
    std::vector<Shape> runs = shape_runs(drawing, drawing.shapes[s]);
    if (runs.size() < 2) {
      continue;
    }
    const std::string name = drawing.shapes[s].name;
    drawing.shapes[s] = std::move(runs.front());
    for (size_t k = 1; k < runs.size(); k++) {
      Shape &piece = runs[k];
      piece.id = drawing.next_id++;
      piece.name = name + " " + std::to_string(k + 1);
      drawing.shapes.insert(drawing.shapes.begin() + s + int(k), std::move(piece));
      made++;
    }
  }
  return made;
}

bool drawing_arrange_shapes(VectorDrawing &drawing, ShapeArrange arrange)
{
  std::vector<bool> selected = drawing_selected_shapes(drawing);
  const int n = int(drawing.shapes.size());
  std::vector<int> order(n);
  for (int s = 0; s < n; s++) {
    order[s] = s;
  }
  switch (arrange) {
    case ShapeArrange::Forward:
      /* From the top down, so a block of selected shapes moves up together. */
      for (int s = n - 2; s >= 0; s--) {
        if (selected[order[s]] && !selected[order[s + 1]]) {
          std::swap(order[s], order[s + 1]);
        }
      }
      break;
    case ShapeArrange::Backward:
      for (int s = 1; s < n; s++) {
        if (selected[order[s]] && !selected[order[s - 1]]) {
          std::swap(order[s], order[s - 1]);
        }
      }
      break;
    case ShapeArrange::ToFront:
      std::stable_partition(order.begin(), order.end(), [&](int s) { return !selected[s]; });
      break;
    case ShapeArrange::ToBack:
      std::stable_partition(order.begin(), order.end(), [&](int s) { return selected[s]; });
      break;
  }
  bool changed = false;
  for (int s = 0; s < n; s++) {
    changed = changed || order[s] != s;
  }
  if (!changed) {
    return false;
  }
  std::vector<Shape> shapes;
  shapes.reserve(n);
  for (const int s : order) {
    shapes.push_back(std::move(drawing.shapes[s]));
  }
  drawing.shapes = std::move(shapes);
  return true;
}

float drawing_selected_mean_radius(const VectorDrawing &drawing)
{
  float sum = 0.0f;
  int count = 0;
  for (const MeshPoint &p : drawing.points) {
    if (p.selected) {
      sum += p.radius;
      count++;
    }
  }
  return count ? sum / float(count) : -1.0f;
}

void drawing_set_selected_mean_radius(VectorDrawing &drawing, float mean)
{
  const float current = drawing_selected_mean_radius(drawing);
  if (current < 0.0f) {
    return;
  }
  const float delta = std::clamp(mean, 0.0f, POINT_RADIUS_MAX) - current;
  for (MeshPoint &p : drawing.points) {
    if (p.selected) {
      p.radius = std::clamp(p.radius + delta, 0.0f, POINT_RADIUS_MAX);
    }
  }
}

/* ---------------------------------------------------------------------------------
 * Shape tool primitives
 */

const char *primitive_name(PrimitiveKind kind)
{
  switch (kind) {
    case PrimitiveKind::Box:
      return "Box";
    case PrimitiveKind::Circle:
      return "Circle";
    case PrimitiveKind::Grid:
      return "Grid";
  }
  return "Shape";
}

const char *primitive_identifier(PrimitiveKind kind)
{
  switch (kind) {
    case PrimitiveKind::Box:
      return "BOX";
    case PrimitiveKind::Circle:
      return "CIRCLE";
    case PrimitiveKind::Grid:
      return "GRID";
  }
  return "BOX";
}

bool primitive_from_identifier(const std::string &identifier, PrimitiveKind *r_kind)
{
  for (const PrimitiveKind kind : PRIMITIVE_KINDS) {
    if (identifier == primitive_identifier(kind)) {
      *r_kind = kind;
      return true;
    }
  }
  return false;
}

VectorDrawing primitive_drawing(PrimitiveKind kind, const std::vector<float2> &points, const Style &style,
                                bool fill, int columns, int rows)
{
  VectorDrawing drawing;
  if (kind == PrimitiveKind::Grid) {
    return points.size() < 2 ? drawing : grid_drawing(points.front(), points.back(), columns, rows, style);
  }
  constexpr float EPS = 1e-5f;
  /* Moho's circle curvature: four smooth points make a circle when each handle is 0.5523 of
   * the radius (the usual Bezier circle), and Moho measures handles from the neighbour. */
  constexpr float BEZIER_CIRCLE = 0.5523f;

  std::vector<float2> pos;
  std::vector<float> curvature;
  bool is_closed = false;
  const float2 a = points.empty() ? float2() : points.front();
  const float2 b = points.empty() ? float2() : points.back();

  switch (kind) {
    case PrimitiveKind::Box: {
      if (points.size() < 2 || std::abs(b.x - a.x) < EPS || std::abs(b.y - a.y) < EPS) {
        return drawing;
      }
      pos = {a, {b.x, a.y}, b, {a.x, b.y}};
      curvature.assign(4, CURVATURE_PEAKED);
      is_closed = true;
      break;
    }
    case PrimitiveKind::Grid:
      break;
    case PrimitiveKind::Circle: {
      const float rx = std::abs(b.x - a.x) * 0.5f, ry = std::abs(b.y - a.y) * 0.5f;
      if (points.size() < 2 || rx < EPS || ry < EPS) {
        return drawing;
      }
      const float2 c = (a + b) * 0.5f;
      const float neighbour = std::sqrt(rx * rx + ry * ry);
      pos = {{c.x + rx, c.y}, {c.x, c.y + ry}, {c.x - rx, c.y}, {c.x, c.y - ry}};
      const float on_x = BEZIER_CIRCLE * ry / neighbour, on_y = BEZIER_CIRCLE * rx / neighbour;
      curvature = {on_x, on_y, on_x, on_y};
      is_closed = true;
      break;
    }
  }

  Curve curve;
  curve.closed = is_closed;
  for (size_t k = 0; k < pos.size(); k++) {
    drawing.points.push_back({pos[k]});
    CurvePoint cp;
    cp.point = int(k);
    /* Box corners have curvature 0: their handles point straight along the sides. */
    cp.curvature = curvature[k];
    curve.points.push_back(cp);
  }
  drawing.curves.push_back(std::move(curve));

  Shape shape;
  shape.name = primitive_name(kind);
  shape.style = style;
  shape.has_outline = true;
  shape.has_fill = fill && is_closed;
  for (int seg = 0; seg < drawing.curves[0].segment_count(); seg++) {
    shape.edges.push_back({0, seg});
  }
  drawing.shapes.push_back(std::move(shape));
  return drawing;
}

void drawing_append_selected(VectorDrawing &dst, const VectorDrawing &src)
{
  for (MeshPoint &p : dst.points) {
    p.selected = false;
  }
  const int point_offset = int(dst.points.size());
  const int curve_offset = int(dst.curves.size());
  for (MeshPoint p : src.points) {
    p.selected = true;
    /* A new id here (a grid's nodes are named by their ids in `src`, found again below). */
    p.id = 0;
    dst.points.push_back(p);
  }
  drawing_ensure_ids(dst);
  for (DrawingGrid grid : src.grids) {
    for (uint64_t &node : grid.nodes) {
      const int k = drawing_point_index(src, node);
      node = k >= 0 ? dst.points[size_t(point_offset + k)].id : 0;
    }
    dst.grids.push_back(std::move(grid));
  }
  for (Curve curve : src.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.point += point_offset;
    }
    dst.curves.push_back(std::move(curve));
  }
  for (Shape shape : src.shapes) {
    for (ShapeEdge &edge : shape.edges) {
      edge.curve += curve_offset;
    }
    shape.name += " " + std::to_string(dst.shapes.size() + 1);
    dst.shapes.push_back(std::move(shape));
  }
}

std::vector<ShapeEdge> drawing_selected_edges(const VectorDrawing &drawing)
{
  std::vector<ShapeEdge> edges;
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    for (int s = 0; s < drawing.curves[c].segment_count(); s++) {
      int a, b;
      curve_segment_points(drawing.curves[c], s, &a, &b);
      if (drawing.points[a].selected && drawing.points[b].selected) {
        edges.push_back({c, s});
      }
    }
  }
  return edges;
}

void drawing_set_edges_hidden(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges, bool hidden)
{
  for (const ShapeEdge &e : edges) {
    drawing.curves[e.curve].points[e.segment].edge_hidden = hidden;
  }
}

std::vector<int> drawing_selected_points(const VectorDrawing &drawing)
{
  std::vector<int> points;
  for (int i = 0; i < int(drawing.points.size()); i++) {
    if (drawing.points[i].selected) {
      points.push_back(i);
    }
  }
  return points;
}

std::vector<int> shape_points(const VectorDrawing &drawing, const Shape &shape)
{
  std::vector<int> points;
  for (const ShapeEdge &e : shape.edges) {
    if (e.curve < 0 || e.curve >= int(drawing.curves.size())) {
      continue;
    }
    int a, b;
    curve_segment_points(drawing.curves[e.curve], e.segment, &a, &b);
    for (const int p : {a, b}) {
      if (std::find(points.begin(), points.end(), p) == points.end()) {
        points.push_back(p);
      }
    }
  }
  return points;
}

bool drawing_points_mean(const VectorDrawing &drawing, const std::vector<int> &points, float2 *r_mean)
{
  if (points.empty()) {
    return false;
  }
  float2 sum;
  for (const int p : points) {
    sum += drawing.points[p].pos;
  }
  *r_mean = sum / float(points.size());
  return true;
}

void drawing_points_move(VectorDrawing &drawing, const std::vector<int> &points, float2 delta)
{
  for (const int p : points) {
    drawing.points[p].pos += delta;
  }
}

void drawing_deselect_handles(VectorDrawing &drawing)
{
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.select_in = cp.select_out = false;
    }
  }
}

bool curve_point_selected(const VectorDrawing &drawing, const CurvePoint &cp)
{
  return drawing.points[cp.point].selected || cp.select_in || cp.select_out;
}

int drawing_align_selected_handles(VectorDrawing &drawing, bool align,
                                   std::vector<std::tuple<int, int, AlignFollow>> *r_aligned)
{
  int changed = 0;
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    Curve &curve = drawing.curves[size_t(c)];
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (!curve_point_selected(drawing, cp)) {
        continue;
      }
      if (align) {
        /* One handle picked: that one turns into line (the handle you pick is the one you are aligning). The point
         * picked (or both handles): both turn, halfway each. */
        const bool point = drawing.points[cp.point].selected || (cp.select_in && cp.select_out);
        if (point) {
          curve_point_align_both(drawing, curve, i);
        }
        else {
          curve_point_align(drawing, curve, i, cp.select_out);
        }
        if (r_aligned) {
          /* curve_point_align turns the out handle when the out is picked (or the in has no length to follow). */
          r_aligned->emplace_back(c, i, point ? AlignFollow::Both : cp.select_out ? AlignFollow::Out : AlignFollow::In);
        }
        changed++;
      }
      else if (cp.aligned) {
        own_handles(drawing, curve, i);
        curve.points[i].aligned = false;
        changed++;
      }
    }
  }
  return changed;
}

int drawing_selected_alignment(const VectorDrawing &drawing)
{
  int found = -1;
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      if (!curve_point_selected(drawing, cp)) {
        continue;
      }
      const int a = cp.aligned ? 1 : 0;
      if (found == -1) {
        found = a;
      }
      else if (found != a) {
        return -2;
      }
    }
  }
  return found;
}

int drawing_set_selected_handles_shape(VectorDrawing &drawing, bool peak)
{
  int changed = 0;
  for (Curve &curve : drawing.curves) {
    std::vector<std::pair<float2, float2>> handles(curve.points.size());
    for (int i = 0; i < int(curve.points.size()); i++) {
      const float2 here = drawing.points[curve.points[i].point].pos;
      handles[i] = {computed_handle(drawing, curve, i, true, peak ? 1 : 2) - here,
                    computed_handle(drawing, curve, i, false, peak ? 1 : 2) - here};
    }
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (!drawing.points[cp.point].selected) {
        continue;
      }
      own_handles(drawing, curve, i);
      cp.handle_in = handles[i].first;
      cp.handle_out = handles[i].second;
      cp.aligned = !peak;
      cp.curvature = peak ? CURVATURE_PEAKED : std::max(cp.curvature, CURVATURE_SMOOTH);
      changed++;
    }
  }
  return changed;
}

void drawing_repair_stale_handle_keys(VectorDrawing &drawing)
{
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (!cp.handles_set) {
        continue;
      }
      Key<float2> *in = cp.handle_in_keys.key_at(0);
      Key<float2> *out = cp.handle_out_keys.key_at(0);
      if (!in || !out || !(in->value == float2()) || !(out->value == float2())) {
        continue;
      }
      /* Both handles zero on frame 0: the stale values the bug keyed. Use the handles the point
       * had in the setup pose, worked out from its neighbours. */
      CurvePoint saved = cp;
      cp.handles_set = false;
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      float2 setup_in, setup_out;
      setup_handles(setup_points(drawing), curve, i, &setup_in, &setup_out);
      cp = saved;
      cp.handle_in_keys.key_at(0)->value = setup_in;
      cp.handle_out_keys.key_at(0)->value = setup_out;
    }
  }
}

static int delete_edges_keep_points(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges)
{
  DetailScope detail_scope(drawing);
  const auto doomed = [&](int c, int seg) {
    return std::find(edges.begin(), edges.end(), ShapeEdge{c, seg}) != edges.end();
  };
  /* Where each old segment is now: (new curve, new segment), or curve -1 when deleted. */
  std::vector<std::vector<ShapeEdge>> moved(drawing.curves.size());
  std::vector<Curve> curves;
  int deleted = 0;
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    const Curve &curve = drawing.curves[c];
    const int segs = curve.segment_count();
    moved[c].assign(std::max(segs, 0), ShapeEdge{-1, -1});
    int first_doomed = -1;
    for (int s = 0; s < segs && first_doomed < 0; s++) {
      first_doomed = doomed(c, s) ? s : -1;
    }
    if (first_doomed < 0) {
      for (int s = 0; s < segs; s++) {
        moved[c][s] = {int(curves.size()), s};
      }
      curves.push_back(curve);
      continue;
    }
    /* Walk the segments, starting just after a deleted one on a closed curve, and cut a new open
     * curve at every deleted segment. */
    const int n = int(curve.points.size());
    const int start = curve.closed ? first_doomed + 1 : 0;
    Curve run;
    const auto flush = [&] {
      if (run.points.size() >= 2) {
        run.closed = false;
        run.points.back().edge_hidden = false;
        curves.push_back(run);
      }
      run = Curve();
    };
    for (int k = 0; k < segs; k++) {
      const int s = (start + k) % std::max(segs, 1);
      if (doomed(c, s)) {
        deleted++;
        flush();
        continue;
      }
      if (run.points.empty()) {
        run.points.push_back(curve.points[s]);
      }
      moved[c][s] = {int(curves.size()), int(run.points.size()) - 1};
      run.points.push_back(curve.points[(s + 1) % n]);
    }
    flush();
  }
  /* Shapes: their surviving segments, renumbered; a shape is kept if anything is left. */
  std::vector<Shape> shapes;
  for (Shape shape : drawing.shapes) {
    std::vector<ShapeEdge> kept;
    bool lost = false;
    for (const ShapeEdge &e : shape.edges) {
      const bool known = e.curve >= 0 && e.curve < int(moved.size()) && e.segment >= 0 &&
                         e.segment < int(moved[e.curve].size());
      if (known && moved[e.curve][e.segment].curve >= 0) {
        kept.push_back(moved[e.curve][e.segment]);
      }
      else {
        lost = true;
      }
    }
    if (kept.empty()) {
      continue;
    }
    shape.edges = kept;
    shapes.push_back(std::move(shape));
    if (lost) {
      /* A gap in its outline: it can't fill unless what is left still closes. */
      VectorDrawing probe;
      probe.points = drawing.points;
      probe.curves = curves;
      std::vector<float2> outline;
      bool closed = false;
      shape_outline(probe, shapes.back(), 1, outline, nullptr, &closed);
      if (!closed) {
        shapes.back().has_fill = false;
      }
    }
  }
  drawing.curves = std::move(curves);
  drawing.shapes = std::move(shapes);
  return deleted;
}

int drawing_delete_edges(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges)
{
  /* The deleted edges' points: those left on no curve go too, as Blender's Delete Edges takes the vertices it
   * leaves loose. A point loose before stays. */
  std::vector<bool> ends(drawing.points.size(), false);
  for (const ShapeEdge &e : edges) {
    if (e.curve >= 0 && e.curve < int(drawing.curves.size()) && e.segment >= 0 &&
        e.segment < drawing.curves[size_t(e.curve)].segment_count())
    {
      int a, b;
      curve_segment_points(drawing.curves[size_t(e.curve)], e.segment, &a, &b);
      ends[size_t(a)] = ends[size_t(b)] = true;
    }
  }
  const int deleted = delete_edges_keep_points(drawing, edges);
  if (deleted > 0) {
    std::vector<bool> loose = ends;
    for (const Curve &curve : drawing.curves) {
      for (const CurvePoint &cp : curve.points) {
        loose[size_t(cp.point)] = false;
      }
    }
    if (std::find(loose.begin(), loose.end(), true) != loose.end()) {
      drawing_remove_points(drawing, loose);
    }
  }
  return deleted;
}

static float2 reflect2(float2 v, bool x, bool y)
{
  return {x ? -v.x : v.x, y ? -v.y : v.y};
}

float2 point_setup_pos(const MeshPoint &point)
{
  /* Not seen by the keys yet (just drawn): where it is, is its setup. */
  return point.pos_keys.has_last || point.pos_keys.animated() ? point.setup_pos : point.pos;
}

float2 handle_setup(const CurvePoint &cp, bool in)
{
  const Channel<float2> &ch = in ? cp.handle_in_keys : cp.handle_out_keys;
  return ch.has_last || ch.animated() ? (in ? cp.setup_in : cp.setup_out) : (in ? cp.handle_in : cp.handle_out);
}

static float wrap_turn(float a)
{
  return a - 2.0f * PI * std::floor((a + PI) / (2.0f * PI));
}

float2 handle_change(float2 setup, float2 handle, float near_turn)
{
  const float setup_length = length(setup), handle_length = length(handle);
  const float setup_angle = setup_length > 1e-9f ? std::atan2(setup.y, setup.x) : 0.0f;
  const float handle_angle = handle_length > 1e-9f ? std::atan2(handle.y, handle.x) : setup_angle + near_turn;
  return {near_turn + wrap_turn(handle_angle - setup_angle - near_turn), handle_length - setup_length};
}

float2 handle_from_change(float2 setup, float2 change)
{
  if (change == float2()) {
    return setup;
  }
  const float setup_length = length(setup);
  const float angle = (setup_length > 1e-9f ? std::atan2(setup.y, setup.x) : 0.0f) + change.x;
  const float l = std::max(0.0f, setup_length + change.y);
  return {std::cos(angle) * l, std::sin(angle) * l};
}

void drawing_changes_from_values(VectorDrawing &drawing)
{
  /* Detail edits since it was last placed become its offsets; its keys hold their change from the
   * setup. */
  drawing_detail_sync(drawing, true);
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      PointDetail &dt = p.detail;
      if (!p.pos_keys.has_last && !p.pos_keys.animated()) {
        dt.setup_pos = dt.pos;
        p.setup_pos = p.pos;
        p.transform = {};
      }
      else if (dt.pos != dt.setup_pos + p.transform) {
        p.transform = dt.pos - dt.setup_pos;
      }
      continue;
    }
    if (!p.pos_keys.has_last && !p.pos_keys.animated()) {
      p.setup_pos = p.pos;
      p.transform = {};
    }
    else if (p.pos != p.setup_pos + p.transform) {
      p.transform = p.pos - p.setup_pos;
    }
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      PointDetail &dt = drawing.points[cp.point].detail;
      for (const bool in : {true, false}) {
        const Channel<float2> &ch = in ? cp.handle_in_keys : cp.handle_out_keys;
        float2 &change = in ? cp.change_in : cp.change_out;
        if (dt.level > 0) {
          const float2 offset = in ? dt.in : dt.out;
          float2 &detail_setup = in ? dt.setup_in : dt.setup_out;
          if (!ch.has_last && !ch.animated()) {
            detail_setup = offset;
            (in ? cp.setup_in : cp.setup_out) = in ? cp.handle_in : cp.handle_out;
            change = {};
          }
          else if (offset != detail_setup + change) {
            change = offset - detail_setup;
          }
          continue;
        }
        const float2 handle = in ? cp.handle_in : cp.handle_out;
        float2 &setup = in ? cp.setup_in : cp.setup_out;
        if (!ch.has_last && !ch.animated()) {
          setup = handle;
          change = {};
        }
        else if (handle != handle_from_change(setup, change)) {
          change = handle_change(setup, handle, change.x);
        }
      }
    }
  }
}

void drawing_values_from_changes(VectorDrawing &drawing)
{
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      p.detail.pos = p.detail.setup_pos + p.transform;
      continue;
    }
    p.pos = p.setup_pos + p.transform;
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      if (PointDetail &dt = drawing.points[cp.point].detail; dt.level > 0) {
        dt.in = dt.setup_in + cp.change_in;
        dt.out = dt.setup_out + cp.change_out;
        continue;
      }
      cp.handle_in = handle_from_change(cp.setup_in, cp.change_in);
      cp.handle_out = handle_from_change(cp.setup_out, cp.change_out);
    }
  }
  /* Detail placed from its offsets, on the curve as it now is. */
  drawing_detail_sync(drawing, false);
}

void drawing_take_setup(VectorDrawing &drawing)
{
  drawing_detail_sync(drawing, true);
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      p.detail.setup_pos = p.detail.pos - p.pos_keys.from_action_bones;
      p.transform = p.detail.pos - p.detail.setup_pos;
      p.setup_pos = p.pos;
      continue;
    }
    p.setup_pos = p.pos - p.pos_keys.from_action_bones;
    p.transform = p.pos - p.setup_pos;
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.setup_in = cp.handle_in;
      cp.setup_out = cp.handle_out;
      cp.change_in = cp.change_out = {};
      if (PointDetail &dt = drawing.points[cp.point].detail; dt.level > 0) {
        dt.setup_in = dt.in;
        dt.setup_out = dt.out;
      }
    }
  }
}

int drawing_clear_selected_pose(VectorDrawing &drawing, bool positions, bool handles)
{
  /* Back to the setup pose plus what action bone actions add now (their part of the channel), so a cleared
   * point lands where the action bones put it rather than keying a change that cancels them. */
  int changed = 0;
  /* Detail goes back to its setup offsets, and rides its curve from there. */
  drawing_detail_sync(drawing, true);
  bool detail = false;
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      MeshPoint &p = drawing.points[cp.point];
      PointDetail &dt = p.detail;
      if (dt.level == 0) {
        continue;
      }
      const float2 rest = dt.setup_pos + p.pos_keys.from_action_bones;
      if (positions && p.selected && dt.pos != rest && (p.pos_keys.has_last || p.pos_keys.animated())) {
        dt.pos = rest;
        changed++;
        detail = true;
      }
      for (const bool in : {true, false}) {
        const Channel<float2> &ch = in ? cp.handle_in_keys : cp.handle_out_keys;
        float2 &offset = in ? dt.in : dt.out;
        const float2 setup = (in ? dt.setup_in : dt.setup_out) + ch.from_action_bones;
        if (handles && (p.selected || (in ? cp.select_in : cp.select_out)) && offset != setup &&
            (ch.has_last || ch.animated()))
        {
          offset = setup;
          changed++;
          detail = true;
        }
      }
    }
  }
  if (detail) {
    drawing_detail_sync(drawing, false);
  }
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      continue;
    }
    const float2 rest = point_setup_pos(p) + p.pos_keys.from_action_bones;
    if (positions && p.selected && p.pos != rest) {
      p.pos = rest;
      changed++;
    }
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      if (!handles) {
        break;
      }
      const bool point = drawing.points[cp.point].selected;
      if (drawing.points[cp.point].detail.level > 0) {
        continue;
      }
      for (const bool in : {true, false}) {
        float2 &handle = in ? cp.handle_in : cp.handle_out;
        /* A handle's action bone part is a change of its Angle and Length. */
        const float2 rest = handle_from_change(handle_setup(cp, in), (in ? cp.handle_in_keys : cp.handle_out_keys).from_action_bones);
        if (cp.handles_set && (point || (in ? cp.select_in : cp.select_out)) && handle != rest) {
          handle = rest;
          changed++;
        }
      }
    }
  }
  return changed;
}

void drawing_new_points_follow(VectorDrawing &drawing, int frame)
{
  const auto is_new = [&](int pi) {
    const MeshPoint &p = drawing.points[pi];
    /* New detail rides its curve: it moves with it already. */
    return !p.pos_keys.has_last && !p.pos_keys.animated() && p.detail.level == 0;
  };
  std::vector<bool> done(drawing.points.size(), false);
  for (const Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    for (int i = 0; i < n; i++) {
      const int pi = curve.points[i].point;
      if (done[pi] || !is_new(pi)) {
        continue;
      }
      /* The nearest recorded point each way along the curve, and how far along the curve it is. */
      const auto neighbour = [&](int step, float *r_distance) -> int {
        float d = 0.0f;
        int j = i;
        for (int walked = 0; walked < n; walked++) {
          const int k = j + step;
          if (k < 0 || k >= n) {
            if (!curve.closed) {
              return -1;
            }
          }
          const int next = (k + n) % n;
          d += distance(drawing.points[curve.points[j].point].pos, drawing.points[curve.points[next].point].pos);
          j = next;
          if (j == i) {
            return -1;
          }
          if (!is_new(curve.points[j].point)) {
            *r_distance = d;
            return curve.points[j].point;
          }
        }
        return -1;
      };
      float d_prev = 0.0f, d_next = 0.0f;
      const int prev = neighbour(-1, &d_prev), next = neighbour(1, &d_next);
      const bool prev_moves = prev >= 0 && drawing.points[prev].pos_keys.animated();
      const bool next_moves = next >= 0 && drawing.points[next].pos_keys.animated();
      if (!prev_moves && !next_moves) {
        continue;
      }
      /* Between two, blended by distance along the curve; past an end, the end's motion. */
      const float t = prev >= 0 && next >= 0 && d_prev + d_next > 1e-9f ? d_prev / (d_prev + d_next) : (prev >= 0 ? 0.0f : 1.0f);
      const auto change_of = [&](int q, int f) -> float2 {
        if (q < 0) {
          return {};
        }
        const MeshPoint &m = drawing.points[q];
        return m.pos_keys.animated() ? m.pos_keys.evaluate(f) : m.pos - m.setup_pos;
      };
      const auto change_at = [&](int f) { return change_of(prev, f) * (1.0f - t) + change_of(next, f) * t; };
      std::set<int> frames{0};
      for (const int q : {prev, next}) {
        if (q >= 0) {
          for (const Key<float2> &k : drawing.points[q].pos_keys.keys) {
            frames.insert(k.frame);
          }
        }
      }
      MeshPoint &p = drawing.points[pi];
      const float2 now = change_at(std::max(frame, 0));
      p.setup_pos = p.pos - now;
      for (const int f : frames) {
        Key<float2> &key = p.pos_keys.set_key(f, change_at(f));
        /* Timed as the neighbour it follows most. */
        const int lead = t <= 0.5f ? (prev_moves ? prev : next) : (next_moves ? next : prev);
        if (const Key<float2> *src = drawing.points[lead].pos_keys.key_at(f)) {
          key_settings_apply(key, key_settings(*src));
        }
      }
      p.transform = now;
      p.pos_keys.last = now;
      p.pos_keys.has_last = true;
      done[pi] = true;
    }
  }
}

int drawing_find_mirror_point(const VectorDrawing &drawing, int point, bool x, bool y)
{
  const float2 here = point_setup_pos(drawing.points[point]);
  const float2 target = reflect2(here, x, y);
  const float eps = 1e-4f + 1e-4f * (std::fabs(here.x) + std::fabs(here.y));
  if (distance(target, here) <= eps) {
    return -1;
  }
  /* The nearest: two points almost on top of each other (a mouth's corner) each have their own image. */
  int image = -1;
  float nearest = eps;
  for (int j = 0; j < int(drawing.points.size()); j++) {
    const float d = distance(point_setup_pos(drawing.points[j]), target);
    if (j != point && d <= nearest) {
      image = j;
      nearest = d;
    }
  }
  return image;
}

void drawing_mirror_handles(VectorDrawing &drawing, int src, int dst, bool x, bool y)
{
  const auto neighbour = [&](const Curve &curve, int i, int dir) {
    const int n = int(curve.points.size());
    const int k = i + dir;
    if (!curve.closed && (k < 0 || k >= n)) {
      return -1;
    }
    return curve.points[(k % n + n) % n].point;
  };
  const auto near = [&](int a, float2 b) {
    return a >= 0 && distance(point_setup_pos(drawing.points[a]), b) <= 1e-4f + 1e-4f * (std::fabs(b.x) + std::fabs(b.y));
  };
  for (const Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      if (curve.points[i].point != src) {
        continue;
      }
      const CurvePoint &from = curve.points[i];
      const int prev = neighbour(curve, i, -1), next = neighbour(curve, i, 1);
      for (Curve &other : drawing.curves) {
        for (int j = 0; j < int(other.points.size()); j++) {
          if (other.points[j].point != dst) {
            continue;
          }
          /* Same direction when the neighbours mirror onto the same sides, else swapped. */
          const int oprev = neighbour(other, j, -1), onext = neighbour(other, j, 1);
          bool swap;
          if (prev >= 0 && near(oprev, reflect2(point_setup_pos(drawing.points[prev]), x, y))) {
            swap = false;
          }
          else if (prev >= 0 && near(onext, reflect2(point_setup_pos(drawing.points[prev]), x, y))) {
            swap = true;
          }
          else if (next >= 0 && near(onext, reflect2(point_setup_pos(drawing.points[next]), x, y))) {
            swap = false;
          }
          else if (next >= 0 && near(oprev, reflect2(point_setup_pos(drawing.points[next]), x, y))) {
            swap = true;
          }
          else {
            continue;
          }
          CurvePoint &to = other.points[j];
          to.aligned = from.aligned;
          to.handles_set = from.handles_set;
          to.curvature = from.curvature;
          to.handle_in = reflect2(swap ? from.handle_out : from.handle_in, x, y);
          to.handle_out = reflect2(swap ? from.handle_in : from.handle_out, x, y);
        }
      }
    }
  }
}

std::vector<MirrorPair> drawing_mirror_pairs(const VectorDrawing &drawing, const std::vector<int> &moving, bool mirror_x,
                                             bool mirror_y)
{
  std::vector<MirrorPair> pairs;
  for (const int src : moving) {
    const float2 here = point_setup_pos(drawing.points[src]);
    const float eps = 1e-4f + 1e-4f * (std::fabs(here.x) + std::fabs(here.y));
    for (const auto &[x, y] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
      if ((x && !mirror_x) || (y && !mirror_y)) {
        continue;
      }
      const float2 target = reflect2(here, x, y);
      if (distance(target, here) <= eps) {
        continue; /* On the axis: its own mirror. */
      }
      for (int j = 0; j < int(drawing.points.size()); j++) {
        if (!drawing.points[j].selected && distance(point_setup_pos(drawing.points[j]), target) <= eps) {
          pairs.push_back({src, j, x, y});
          break;
        }
      }
    }
  }
  return pairs;
}

void drawing_apply_mirror(VectorDrawing &drawing, const std::vector<MirrorPair> &pairs)
{
  for (const MirrorPair &m : pairs) {
    drawing.points[m.dst].pos = reflect2(drawing.points[m.src].pos, m.x, m.y);
    drawing_mirror_handles(drawing, m.src, m.dst, m.x, m.y);
  }
}

void drawing_mirror_selected_handles(VectorDrawing &drawing, bool mirror_x, bool mirror_y)
{
  if (!mirror_x && !mirror_y) {
    return;
  }
  std::vector<int> sources;
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      if (curve_point_selected(drawing, cp) &&
          std::find(sources.begin(), sources.end(), cp.point) == sources.end()) {
        sources.push_back(cp.point);
      }
    }
  }
  for (const int src : sources) {
    for (const auto &[x, y] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
      if ((x && !mirror_x) || (y && !mirror_y)) {
        continue;
      }
      const int dst = drawing_find_mirror_point(drawing, src, x, y);
      if (dst >= 0 && std::find(sources.begin(), sources.end(), dst) == sources.end()) {
        drawing_mirror_handles(drawing, src, dst, x, y);
      }
    }
  }
}

/* --- Proportional editing ----------------------------------------------------------------- */

static const char *const FALLOFF_NAMES[PROPORTIONAL_FALLOFF_COUNT][2] = {
    {"Smooth", "SMOOTH"}, {"Sphere", "SPHERE"}, {"Root", "ROOT"},         {"Inverse Square", "INVERSE_SQUARE"},
    {"Sharp", "SHARP"},   {"Linear", "LINEAR"}, {"Constant", "CONSTANT"}, {"Random", "RANDOM"}};

const char *proportional_falloff_name(ProportionalFalloff falloff)
{
  return FALLOFF_NAMES[int(falloff)][0];
}

const char *proportional_falloff_identifier(ProportionalFalloff falloff)
{
  return FALLOFF_NAMES[int(falloff)][1];
}

bool proportional_falloff_from_identifier(const std::string &id, ProportionalFalloff *r_falloff)
{
  for (int i = 0; i < PROPORTIONAL_FALLOFF_COUNT; i++) {
    if (id == FALLOFF_NAMES[i][1]) {
      *r_falloff = ProportionalFalloff(i);
      return true;
    }
  }
  return false;
}

std::vector<float> drawing_proportional_distances(const VectorDrawing &drawing, const std::vector<float2> &positions,
                                                  bool connected)
{
  const int n = int(drawing.points.size());
  std::vector<float> dist(n, std::numeric_limits<float>::infinity());
  std::vector<int> selected;
  for (int i = 0; i < n; i++) {
    if (drawing.points[i].selected) {
      dist[i] = 0.0f;
      selected.push_back(i);
    }
  }
  if (selected.empty() || int(positions.size()) < n) {
    return dist;
  }
  if (!connected) {
    for (int i = 0; i < n; i++) {
      for (const int s : selected) {
        dist[i] = std::min(dist[i], distance(positions[i], positions[s]));
      }
    }
    return dist;
  }
  /* Along the curves: out from the selection to each point's neighbours, nearest first. */
  std::vector<std::vector<int>> next(n);
  for (const Curve &curve : drawing.curves) {
    const int m = int(curve.points.size());
    for (int k = 0; k < m; k++) {
      const int a = curve.points[k].point;
      const int b = k + 1 < m ? curve.points[k + 1].point : (curve.closed && m > 2 ? curve.points[0].point : -1);
      if (b >= 0 && a != b && a < n && b < n) {
        next[a].push_back(b);
        next[b].push_back(a);
      }
    }
  }
  using Entry = std::pair<float, int>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  for (const int s : selected) {
    queue.push({0.0f, s});
  }
  while (!queue.empty()) {
    const auto [d, i] = queue.top();
    queue.pop();
    if (d > dist[i]) {
      continue;
    }
    for (const int j : next[i]) {
      const float through = d + distance(positions[i], positions[j]);
      if (through < dist[j]) {
        dist[j] = through;
        queue.push({through, j});
      }
    }
  }
  return dist;
}

float proportional_weight(ProportionalFalloff falloff, float distance, float size, int point)
{
  if (distance <= 0.0f) {
    return 1.0f;
  }
  if (!(distance < size)) {
    return 0.0f;
  }
  /* As Blender's: 1 at the selection, 0 at the edge of the size. */
  const float d = 1.0f - distance / size;
  switch (falloff) {
    case ProportionalFalloff::Smooth:
      return 3.0f * d * d - 2.0f * d * d * d;
    case ProportionalFalloff::Sphere:
      return std::sqrt(std::max(0.0f, 2.0f * d - d * d));
    case ProportionalFalloff::Root:
      return std::sqrt(d);
    case ProportionalFalloff::InverseSquare:
      return d * (2.0f - d);
    case ProportionalFalloff::Sharp:
      return d * d;
    case ProportionalFalloff::Linear:
      return d;
    case ProportionalFalloff::Constant:
      return 1.0f;
    case ProportionalFalloff::Random: {
      /* The same amount for a point every time (a hash of its index). */
      uint32_t x = uint32_t(point) * 2654435761u + 0x9e3779b9u;
      x ^= x >> 16;
      x *= 0x7feb352du;
      x ^= x >> 15;
      x *= 0x846ca68bu;
      x ^= x >> 16;
      return float(x >> 8) / float(1u << 24) * d;
    }
  }
  return d;
}

}  // namespace oanim

namespace oanim {

bool symmetrize_near_pair(float2 a, float2 b, float axis, float threshold)
{
  const float ax = a.x - axis, bx = b.x - axis;
  if ((ax > 0.0f) == (bx > 0.0f) || ax == 0.0f || bx == 0.0f) {
    return false;
  }
  const float d = distance(float2(-ax, a.y), float2(bx, b.y));
  return d <= threshold && d < std::fabs(ax) + std::fabs(bx);
}

SymmetrizeResult drawing_symmetrize(VectorDrawing &drawing, bool right_to_left, float threshold, bool setup_frame,
                                    const std::map<uint64_t, uint64_t> *weight_flip, const SymmetrizeMatch *match)
{
  SymmetrizeResult result;
  const auto skin = [&](int i) {
    return match && size_t(i) < match->skins.size() ? match->skins[size_t(i)] : Transform2D();
  };
  /* Matching the shape: where each point shows, before anything moves. */
  std::vector<float2> shown;
  if (match) {
    for (int i = 0; i < int(drawing.points.size()); i++) {
      shown.push_back(skin(i).apply(drawing.points[i].pos));
    }
  }
  const auto place = [&](int i) {
    return match ? shown[size_t(i)] : setup_frame ? drawing.points[i].pos : point_setup_pos(drawing.points[i]);
  };
  float axis = match ? match->axis : 0.0f;
  if (match && match->middle) {
    float lo = std::numeric_limits<float>::max(), hi = -lo;
    for (int i = 0; i < int(drawing.points.size()); i++) {
      if (drawing.points[i].selected) {
        lo = std::min(lo, shown[size_t(i)].x);
        hi = std::max(hi, shown[size_t(i)].x);
      }
    }
    axis = lo <= hi ? (lo + hi) * 0.5f : 0.0f;
  }
  const auto across = [&](float2 p) { return float2(2.0f * axis - p.x, p.y); };
  const auto handle_shown = [&](int i, float2 h) { return match ? skin(i).apply_vector(h) : h; };
  const auto handle_stored = [&](int i, float2 h) { return match ? skin(i).inverted().apply_vector(h) : h; };
  const float thr = std::max(threshold, 0.0f);
  std::vector<int> right, left, centre;
  for (int i = 0; i < int(drawing.points.size()); i++) {
    if (!drawing.points[i].selected) {
      continue;
    }
    const float x = place(i).x - axis;
    (x > thr ? right : x < -thr ? left : centre).push_back(i);
  }
  struct Candidate {
    float d;
    int r, l;
  };
  /* Each right point against each left one, the nearest reflection first. */
  const auto nearest_first = [&](const std::vector<int> &rs, const std::vector<int> &ls) {
    std::vector<Candidate> candidates;
    for (const int r : rs) {
      for (const int l : ls) {
        candidates.push_back({distance(across(place(r)), place(l)), r, l});
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.d < b.d; });
    return candidates;
  };
  /* A point the threshold counts as on the axis goes back to its side when it has a partner there. The points off the
   * axis are in this too, or one could take the partner of another. */
  if (!centre.empty()) {
    std::vector<int> rs = right, ls = left;
    const auto is_centre = [&](int i) { return std::find(centre.begin(), centre.end(), i) != centre.end(); };
    for (const int c : centre) {
      if (place(c).x != axis) {
        (place(c).x > axis ? rs : ls).push_back(c);
      }
    }
    std::set<int> partnered;
    for (const Candidate &c : nearest_first(rs, ls)) {
      if (!partnered.count(c.r) && !partnered.count(c.l) &&
          ((!is_centre(c.r) && !is_centre(c.l)) || symmetrize_near_pair(place(c.r), place(c.l), axis, thr)))
      {
        partnered.insert(c.r);
        partnered.insert(c.l);
      }
    }
    for (const int c : centre) {
      if (partnered.count(c)) {
        (place(c).x > axis ? right : left).push_back(c);
      }
    }
    centre.erase(std::remove_if(centre.begin(), centre.end(), [&](int c) { return partnered.count(c) > 0; }),
                 centre.end());
  }
  result.right = int(right.size());
  result.left = int(left.size());
  result.centre = int(centre.size());
  std::set<int> used;
  std::vector<std::pair<int, int>> pairs;
  /* Matching the shape: a line mirrored onto itself runs backwards, so a curve's two sides pair from opposite ends (a
   * closed one's from where they fit best). The nearest first could cross them: the points would mirror, the line
   * through them wouldn't. */
  if (match) {
    const auto on = [](const std::vector<int> &side, int point) {
      return std::find(side.begin(), side.end(), point) != side.end();
    };
    for (const Curve &curve : drawing.curves) {
      std::vector<int> r, l;
      for (const CurvePoint &cp : curve.points) {
        if (used.count(cp.point) || on(r, cp.point) || on(l, cp.point)) {
          continue;
        }
        if (on(right, cp.point)) {
          r.push_back(cp.point);
        }
        else if (on(left, cp.point)) {
          l.push_back(cp.point);
        }
      }
      const int n = int(r.size());
      if (n == 0 || n != int(l.size())) {
        continue;
      }
      int start = 0;
      float least = std::numeric_limits<float>::max();
      for (int s = 0; s < (curve.closed ? n : 1); s++) {
        float d = 0.0f;
        for (int i = 0; i < n; i++) {
          d += distance(across(place(r[size_t((s + i) % n)])), place(l[size_t(n - 1 - i)]));
        }
        if (d < least) {
          least = d;
          start = s;
        }
      }
      for (int i = 0; i < n; i++) {
        const int a = r[size_t((start + i) % n)], b = l[size_t(n - 1 - i)];
        used.insert(a);
        used.insert(b);
        pairs.push_back({a, b});
      }
    }
  }
  /* Pairs: the nearest reflection first, each point used once. */
  for (const Candidate &c : nearest_first(right, left)) {
    if (!used.count(c.r) && !used.count(c.l)) {
      used.insert(c.r);
      used.insert(c.l);
      pairs.push_back({c.r, c.l});
    }
  }
  if (right.size() != left.size() || (pairs.empty() && centre.empty())) {
    for (const int i : right) {
      if (!used.count(i)) {
        result.unmatched.push_back(i);
      }
    }
    for (const int i : left) {
      if (!used.count(i)) {
        result.unmatched.push_back(i);
      }
    }
    return result;
  }

  /* Every handle involved is stored, so it can be copied and set. */
  std::set<int> involved(centre.begin(), centre.end());
  for (const auto &[r, l] : pairs) {
    involved.insert(r);
    involved.insert(l);
  }
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      if (involved.count(curve.points[i].point)) {
        own_handles(drawing, curve, i);
      }
    }
  }
  /* The next point along a curve that a handle reaches: of the point's own level or coarser, as its handle is the
   * coarse one past finer detail. -1 past a line's end. */
  const auto neighbour = [&](const Curve &curve, int i, int dir) {
    const int n = int(curve.points.size());
    const int level = drawing.points[curve.points[i].point].detail.level;
    for (int k = i + dir, step = 1; step < n; k += dir, step++) {
      if (!curve.closed && (k < 0 || k >= n)) {
        return -1;
      }
      const int point = curve.points[(k % n + n) % n].point;
      if (drawing.points[point].detail.level <= level) {
        return point;
      }
    }
    return -1;
  };
  struct HandleOf {
    CurvePoint *curve_point;
    bool out;
    /* The point it reaches for, and the next along the curve, which is the one the line is drawn to. */
    int faces, next;
    /* How much it is shortened where it is drawn, for facing finer detail. */
    float drawn;
    float2 &handle() const
    {
      return out ? curve_point->handle_out : curve_point->handle_in;
    }
  };
  std::vector<std::vector<float>> drawn_in(drawing.curves.size()), drawn_out(drawing.curves.size());
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    drawing_detail_handle_factors(drawing, c, drawn_in[size_t(c)], drawn_out[size_t(c)]);
  }
  /* A point's handles, on every curve through it: a welded point has some on each. A line's end has none past it. */
  const auto handles_of = [&](int point) {
    std::vector<HandleOf> handles;
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      Curve &curve = drawing.curves[size_t(c)];
      const int n = int(curve.points.size());
      for (int i = 0; i < n; i++) {
        if (curve.points[i].point == point) {
          const int before = neighbour(curve, i, -1), after = neighbour(curve, i, 1);
          if (before >= 0) {
            handles.push_back({&curve.points[i], false, before, curve.points[(i + n - 1) % n].point,
                               drawn_in[size_t(c)][size_t(i)]});
          }
          if (after >= 0) {
            handles.push_back({&curve.points[i], true, after, curve.points[(i + 1) % n].point,
                               drawn_out[size_t(c)][size_t(i)]});
          }
        }
      }
    }
    return handles;
  };
  /* Which handle of `to` each one of `from` lands on, by what they face: its partner, else the point nearest its
   * reflection, the nearest first and each handle used once. Where everything was before anything moves. */
  std::map<int, int> partner;
  for (const auto &[r, l] : pairs) {
    partner[r] = l;
    partner[l] = r;
  }
  std::vector<float2> before;
  for (int i = 0; i < int(drawing.points.size()); i++) {
    before.push_back(place(i));
  }
  const auto gap = [&](int a, int b) {
    const auto it = partner.find(a);
    return it != partner.end() && it->second == b ? 0.0f : distance(across(before[size_t(a)]), before[size_t(b)]);
  };
  struct HandleMatch {
    HandleOf source, image;
    /* What goes across: the handle as drawn, when the lines are drawn to images of each other, one side's through a
     * detail point and the other's through a point of its own. Otherwise the handle as it is, past any detail: a detail
     * point on one side alone doesn't change that side's look. */
    float scale;
  };
  const auto match_handles = [&](const std::vector<HandleOf> &from, const std::vector<HandleOf> &to) {
    struct Fit {
      float d;
      size_t from, to;
      bool drawn;
    };
    std::vector<Fit> fits;
    for (size_t f = 0; f < from.size(); f++) {
      for (size_t t = 0; t < to.size(); t++) {
        const float reach = gap(from[f].faces, to[t].faces), line = gap(from[f].next, to[t].next);
        fits.push_back({std::min(reach, line), f, t, line <= reach});
      }
    }
    std::stable_sort(fits.begin(), fits.end(), [](const Fit &a, const Fit &b) { return a.d < b.d; });
    std::vector<HandleMatch> matched;
    std::set<size_t> used_from, used_to;
    for (const Fit &fit : fits) {
      if (!used_from.count(fit.from) && !used_to.count(fit.to)) {
        used_from.insert(fit.from);
        used_to.insert(fit.to);
        const HandleOf &source = from[fit.from], &image = to[fit.to];
        matched.push_back({source, image, fit.drawn ? source.drawn / image.drawn : 1.0f});
      }
    }
    return matched;
  };

  for (const auto &[r, l] : pairs) {
    const int src = right_to_left ? r : l, dst = right_to_left ? l : r;
    result.copied.push_back({src, dst});
    /* The copy's two handles stay in line only if they came from a pair in line. */
    std::map<const CurvePoint *, const CurvePoint *> given;
    for (const auto &[source, image, scale] : match_handles(handles_of(src), handles_of(dst))) {
      const CurvePoint &from = *source.curve_point;
      CurvePoint &to = *image.curve_point;
      image.handle() = handle_stored(dst, reflect2(handle_shown(src, source.handle() * scale), true, false));
      (image.out ? to.weight_out : to.weight_in) = source.out ? from.weight_out : from.weight_in;
      (image.out ? to.offset_out : to.offset_in) = -(source.out ? from.offset_out : from.offset_in);
      to.curvature = from.curvature;
      const auto [it, first] = given.try_emplace(&to, &from);
      to.aligned = from.aligned && (first || it->second == &from);
      to.handles_set = true;
    }
    MeshPoint &to = drawing.points[dst];
    const MeshPoint &from = drawing.points[src];
    if (match) {
      to.pos = skin(dst).inverted().apply(across(shown[size_t(src)]));
    }
    else if (setup_frame) {
      to.pos = reflect2(from.pos, true, false);
    }
    else {
      to.pos = point_setup_pos(to) + reflect2(from.pos - point_setup_pos(from), true, false);
    }
    to.radius = from.radius;
    if (weight_flip) {
      to.weights = from.weights;
      for (BoneWeight &w : to.weights) {
        if (const auto it = weight_flip->find(w.bone); it != weight_flip->end()) {
          w.bone = it->second;
        }
      }
    }
  }

  /* The axis's own points: on it, their handles mirroring each other (flat when within 10 degrees
   * of flat). */
  const float nearly_flat = std::tan(10.0f * PI / 180.0f);
  for (const int c : centre) {
    MeshPoint &p = drawing.points[c];
    if (match) {
      p.pos = skin(c).inverted().apply(float2(axis, shown[size_t(c)].y));
    }
    else {
      p.pos.x = setup_frame ? 0.0f : point_setup_pos(p).x;
    }
    /* Handles going each way, on any of its curves: the source side's set their images. */
    std::vector<HandleOf> sources, images;
    for (const HandleOf &handle : handles_of(c)) {
      if (!handle.curve_point->handles_set) {
        continue;
      }
      float2 h = handle_shown(c, handle.handle());
      if (std::fabs(h.y) <= nearly_flat * std::fabs(h.x)) {
        h.y = 0.0f;
      }
      handle.handle() = handle_stored(c, h);
      if (std::fabs(h.x) > 1e-9f) {
        ((h.x > 0.0f) == right_to_left ? sources : images).push_back(handle);
      }
    }
    for (const auto &[source, image, scale] : match_handles(sources, images)) {
      image.handle() = handle_stored(c, reflect2(handle_shown(c, source.handle() * scale), true, false));
    }
  }
  result.centre_points = centre;
  /* The shapes on the copied side give their looks to their images. */
  if (!match) {
    std::map<int, int> image;
    for (const auto &[src, dst] : result.copied) {
      image[src] = dst;
    }
    for (const int c : centre) {
      image[c] = c;
    }
    drawing_copy_shape_looks(drawing, drawing, image);
  }
  result.done = true;
  return result;
}

int drawing_copy_shape_looks(const VectorDrawing &from, VectorDrawing &to, const std::map<int, int> &image)
{
  const auto point_set = [](const VectorDrawing &d, const Shape &shape) {
    std::vector<int> points = shape_points(d, shape);
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
  };
  /* Worked out first, then set, as `from` and `to` may be the same drawing. */
  std::vector<std::pair<int, Shape>> looks;
  for (int s = 0; s < int(from.shapes.size()); s++) {
    std::vector<int> mapped;
    bool all = true;
    for (const int p : point_set(from, from.shapes[s])) {
      const auto it = image.find(p);
      all = all && it != image.end();
      if (all) {
        mapped.push_back(it->second);
      }
    }
    if (!all || mapped.empty()) {
      continue;
    }
    std::sort(mapped.begin(), mapped.end());
    mapped.erase(std::unique(mapped.begin(), mapped.end()), mapped.end());
    for (int t = 0; t < int(to.shapes.size()); t++) {
      if ((&from == &to && t == s) || point_set(to, to.shapes[t]) != mapped) {
        continue;
      }
      looks.push_back({t, from.shapes[s]});
    }
  }
  for (const auto &[t, look] : looks) {
    Shape &shape = to.shapes[t];
    shape.has_fill = look.has_fill;
    shape.has_outline = look.has_outline;
    shape.style = look.style;
    shape.material = look.material;
    shape.material_last = look.material_last;
  }
  return int(looks.size());
}

}  // namespace oanim

namespace oanim {

void drawing_mirror_setup_x(VectorDrawing &drawing)
{
  const auto flip = [](float2 v) { return float2(-v.x, v.y); };
  const auto across = [](float2 v) { return float2(v.x, -v.y); };
  for (MeshPoint &p : drawing.points) {
    /* From what shows on the setup frame. */
    p.pos = flip(p.pos);
    p.setup_pos = p.pos;
    p.transform = {};
    p.pos_keys = {};
    p.radius_keys = {};
    p.selected = false;
    /* Along its segment stays along it; across it changes side, the segment's turn reversed. */
    PointDetail &d = p.detail;
    d.pos = across(d.pos);
    d.in = across(d.in);
    d.out = across(d.out);
    d.setup_pos = d.pos;
    d.setup_in = d.in;
    d.setup_out = d.out;
    d.built = false;
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      /* Handles not stored yet follow the (flipped) neighbours by themselves. */
      if (cp.handles_set) {
        cp.handle_in = flip(cp.handle_in);
        cp.handle_out = flip(cp.handle_out);
        cp.setup_in = cp.handle_in;
        cp.setup_out = cp.handle_out;
      }
      cp.offset_in = -cp.offset_in;
      cp.offset_out = -cp.offset_out;
      cp.change_in = {};
      cp.change_out = {};
      cp.curvature_keys = {};
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      cp.select_in = cp.select_out = false;
    }
  }
}

/* ---------------------------------------------------------------------------------
 * Slide (G G)
 */

namespace {

/* A curve's line as it was before the slide: segment k runs from u = k to u = k + 1. */
struct SlidePath {
  std::vector<std::array<float2, 4>> segments;
  bool closed = false;

  int count() const { return int(segments.size()); }
  /* The segment u falls in and how far along it. `end`: a whole u is the end of the segment
   * before it rather than the start of the next. */
  int locate(float u, bool end, float *r_t) const
  {
    const int n = count();
    if (closed) {
      u = std::fmod(u, float(n));
      if (u < 0.0f) {
        u += float(n);
      }
    }
    int k = int(std::floor(u));
    if (end && float(k) == u) {
      k -= 1;
    }
    if (closed) {
      k = (k % n + n) % n;
      float t = u - std::floor(u);
      if (end && t == 0.0f) {
        t = 1.0f;
      }
      *r_t = t;
      return k;
    }
    k = std::clamp(k, 0, n - 1);
    *r_t = std::clamp(u - float(k), 0.0f, 1.0f);
    return k;
  }
  float2 eval(float u) const
  {
    float t;
    const int k = locate(u, false, &t);
    return bezier_eval(segments[k].data(), t);
  }
  /* Which way the line runs at u, coming in (`end`) or going out. */
  float2 tangent(float u, bool end) const
  {
    float t;
    const std::array<float2, 4> &c = segments[locate(u, end, &t)];
    const float s = 1.0f - t;
    float2 d = (c[1] - c[0]) * (3.0f * s * s) + (c[2] - c[1]) * (6.0f * s * t) + (c[3] - c[2]) * (3.0f * t * t);
    if (dot(d, d) < 1e-14f) {
      /* A handle of no length: the line leaves toward the far handle. */
      d = t < 0.5f ? (c[2] - c[0]) : (c[3] - c[1]);
      if (dot(d, d) < 1e-14f) {
        d = c[3] - c[0];
      }
    }
    return normalize(d);
  }
};

/* The Bezier that best follows the old line from ua to ub (ua < ub): exactly the piece of it where
 * both lie in one old segment, else a least-squares fit with the line's own directions at the
 * ends (Schneider's curve fitting, as Blender's curve fit does). */
void slide_fit(const SlidePath &path, float ua, float ub, float2 r_ctrl[4])
{
  const float2 a = path.eval(ua), b = path.eval(ub);
  float ta, tb;
  const int ka = path.locate(ua, false, &ta);
  const int kb = path.locate(ub, true, &tb);
  r_ctrl[0] = a;
  r_ctrl[3] = b;
  if (ka == kb && ta <= tb) {
    /* A piece of one cubic is a cubic: its handles are a third of its span of the derivative. */
    const std::array<float2, 4> &c = path.segments[ka];
    const auto deriv = [&](float t) {
      const float s = 1.0f - t;
      return (c[1] - c[0]) * (3.0f * s * s) + (c[2] - c[1]) * (6.0f * s * t) + (c[3] - c[2]) * (3.0f * t * t);
    };
    const float span = (tb - ta) / 3.0f;
    r_ctrl[1] = a + deriv(ta) * span;
    r_ctrl[2] = b - deriv(tb) * span;
    return;
  }
  const float2 out = path.tangent(ua, false);
  const float2 in = path.tangent(ub, true) * -1.0f;
  /* Samples of the old line, given parameters by how far along it they are. */
  std::vector<float2> samples;
  const int steps = std::max(8, int(std::ceil((ub - ua) * 16.0f)));
  for (int i = 0; i <= steps; i++) {
    samples.push_back(path.eval(ua + (ub - ua) * float(i) / float(steps)));
  }
  std::vector<float> along(samples.size(), 0.0f);
  for (size_t i = 1; i < samples.size(); i++) {
    along[i] = along[i - 1] + distance(samples[i], samples[i - 1]);
  }
  const float total = along.back();
  const float chord = distance(a, b);
  float alpha_a = chord / 3.0f, alpha_b = chord / 3.0f;
  std::vector<float> params(samples.size());
  for (size_t i = 0; i < samples.size(); i++) {
    params[i] = total > 1e-9f ? along[i] / total : 0.0f;
  }
  /* Fit, then move each sample's parameter to its nearest place on the fit (one Newton step) and
   * fit again: chord lengths alone leave the handles a little off on a long bend. */
  for (int pass = 0; pass < 5 && total > 1e-9f; pass++) {
    double c00 = 0.0, c01 = 0.0, c11 = 0.0, x0 = 0.0, x1 = 0.0;
    for (size_t i = 0; i < samples.size(); i++) {
      const float t = params[i], s = 1.0f - t;
      const float b0 = s * s * s, b1 = 3.0f * s * s * t, b2 = 3.0f * s * t * t, b3 = t * t * t;
      const float2 A1 = out * b1, A2 = in * b2;
      const float2 rest = samples[i] - (a * (b0 + b1) + b * (b2 + b3));
      c00 += dot(A1, A1);
      c01 += dot(A1, A2);
      c11 += dot(A2, A2);
      x0 += dot(rest, A1);
      x1 += dot(rest, A2);
    }
    const double det = c00 * c11 - c01 * c01;
    if (std::abs(det) > 1e-18) {
      const double fa = (x0 * c11 - c01 * x1) / det, fb = (c00 * x1 - c01 * x0) / det;
      /* A handle backwards or nearly none is no fit; the plain third of the chord is safer. */
      if (fa > 1e-4 * chord && fb > 1e-4 * chord) {
        alpha_a = float(fa);
        alpha_b = float(fb);
      }
      else {
        break;
      }
    }
    const float2 c[4] = {a, a + out * alpha_a, b + in * alpha_b, b};
    for (size_t i = 1; i + 1 < samples.size(); i++) {
      const float t = params[i], s = 1.0f - t;
      const float2 q = bezier_eval(c, t) - samples[i];
      const float2 d1 = (c[1] - c[0]) * (3.0f * s * s) + (c[2] - c[1]) * (6.0f * s * t) + (c[3] - c[2]) * (3.0f * t * t);
      const float2 d2 = (c[2] - c[1] * 2.0f + c[0]) * (6.0f * s) + (c[3] - c[2] * 2.0f + c[1]) * (6.0f * t);
      const float den = dot(d1, d1) + dot(q, d2);
      if (std::abs(den) > 1e-12f) {
        params[i] = std::clamp(t - dot(q, d1) / den, 0.0f, 1.0f);
      }
    }
  }
  r_ctrl[1] = a + out * alpha_a;
  r_ctrl[2] = b + in * alpha_b;
}

}  // namespace

int drawing_slide_points(VectorDrawing &drawing, const std::vector<int> &points, float factor)
{
  factor = std::clamp(factor, -0.999f, 0.999f);
  std::vector<bool> sliding(drawing.points.size(), false);
  for (const int p : points) {
    if (p >= 0 && p < int(drawing.points.size())) {
      sliding[p] = true;
    }
  }
  /* A point on two curves slides along the first. */
  std::vector<bool> taken(drawing.points.size(), false);
  int moved = 0;
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    const int segs = curve.segment_count();
    if (segs < 1) {
      continue;
    }
    std::vector<int> here;
    for (int i = 0; i < n; i++) {
      const int p = curve.points[i].point;
      if (sliding[p] && !taken[p]) {
        here.push_back(i);
      }
    }
    if (here.empty()) {
      continue;
    }
    for (int i = 0; i < n; i++) {
      own_handles(drawing, curve, i);
    }
    SlidePath path;
    path.closed = curve.closed;
    for (int k = 0; k < segs; k++) {
      std::array<float2, 4> c;
      curve_segment_bezier(drawing, curve, k, c.data());
      path.segments.push_back(c);
    }
    std::vector<float> u(n);
    std::vector<bool> slides(n, false);
    for (int i = 0; i < n; i++) {
      u[i] = float(i);
    }
    for (const int i : here) {
      taken[curve.points[i].point] = true;
      /* An open curve's end can only slide inward. */
      const float to = curve.closed ? float(i) + factor : std::clamp(float(i) + factor, 0.0f, float(n - 1));
      if (to != u[i]) {
        u[i] = to;
        slides[i] = true;
      }
    }
    /* Refit every segment next to a sliding point to the old line, then move the points. */
    std::vector<std::array<float2, 4>> fits(segs);
    std::vector<bool> refit(segs, false);
    for (int k = 0; k < segs; k++) {
      const int j = (k + 1) % n;
      if (!slides[k] && !slides[j]) {
        continue;
      }
      float ua = u[k], ub = u[j];
      if (curve.closed) {
        while (ub <= ua) {
          ub += float(n);
        }
        while (ub - ua > float(n)) {
          ub -= float(n);
        }
      }
      if (ub - ua < 1e-5f) {
        continue;
      }
      slide_fit(path, ua, ub, fits[k].data());
      refit[k] = true;
    }
    for (int i = 0; i < n; i++) {
      if (slides[i]) {
        drawing.points[curve.points[i].point].pos = path.eval(u[i]);
        moved++;
      }
    }
    for (int k = 0; k < segs; k++) {
      if (!refit[k]) {
        continue;
      }
      CurvePoint &from = curve.points[k];
      CurvePoint &to = curve.points[(k + 1) % n];
      from.handle_out = fits[k][1] - drawing.points[from.point].pos;
      to.handle_in = fits[k][2] - drawing.points[to.point].pos;
    }
  }
  return moved;
}

/* ---------------------------------------------------------------------------------
 * Smooth Points
 */

namespace {

/* Where a point is on a curve, skipping detail hidden above the curve's edit level: the shown
 * points before and after it (-1 at an open end). */
struct SmoothSlot {
  int curve, index, prev, next;
};

std::vector<SmoothSlot> smooth_slots(const VectorDrawing &drawing)
{
  std::vector<SmoothSlot> slots;
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    const Curve &curve = drawing.curves[c];
    std::vector<int> shown;
    for (int i = 0; i < int(curve.points.size()); i++) {
      if (!drawing.points[curve.points[i].point].detail.hidden) {
        shown.push_back(i);
      }
    }
    const int n = int(shown.size());
    if (n < 2) {
      continue;
    }
    for (int k = 0; k < n; k++) {
      int prev = k > 0 ? shown[k - 1] : (curve.closed ? shown[n - 1] : -1);
      int next = k + 1 < n ? shown[k + 1] : (curve.closed ? shown[0] : -1);
      slots.push_back({c, shown[k], prev, next});
    }
  }
  return slots;
}

}  // namespace

int drawing_smooth_points(VectorDrawing &drawing, const SmoothPointsSettings &s)
{
  const float factor = std::clamp(s.factor, 0.0f, 1.0f);
  const int repeat = std::clamp(s.repeat, 1, 1000);
  const int np = int(drawing.points.size());
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      own_handles(drawing, curve, i);
    }
  }
  const std::vector<SmoothSlot> slots = smooth_slots(drawing);
  /* Each point's neighbours along every curve it is on (a welded point averages them all, as a mesh
   * vertex does), and whether it ends an open curve. */
  std::vector<std::vector<int>> around(np);
  std::vector<bool> end(np, false);
  for (const SmoothSlot &slot : slots) {
    const Curve &curve = drawing.curves[slot.curve];
    const int p = curve.points[slot.index].point;
    for (const int j : {slot.prev, slot.next}) {
      if (j >= 0) {
        around[p].push_back(curve.points[j].point);
      }
      else {
        end[p] = true;
      }
    }
  }
  std::vector<bool> moves(np, false);
  int count = 0;
  for (int p = 0; p < np; p++) {
    const MeshPoint &mp = drawing.points[p];
    moves[p] = mp.selected && !mp.detail.hidden && !around[p].empty() && (s.ends || !end[p]);
    count += moves[p];
  }
  if (count == 0) {
    return 0;
  }
  /* One Laplacian step: each point `amount` of the way to its neighbours' middle (negative: away,
   * Taubin's inflating step, so Keep Shape doesn't shrink a shape). */
  const auto step = [&](float amount) {
    const std::vector<MeshPoint> was = drawing.points;
    for (int p = 0; p < np; p++) {
      if (!moves[p]) {
        continue;
      }
      float2 mid;
      float radius = 0.0f;
      for (const int q : around[p]) {
        mid += was[q].pos;
        radius += was[q].radius;
      }
      mid = mid / float(around[p].size());
      radius /= float(around[p].size());
      if (s.position) {
        drawing.points[p].pos = was[p].pos + (mid - was[p].pos) * amount;
      }
      if (s.radius) {
        drawing.points[p].radius = std::clamp(was[p].radius + (radius - was[p].radius) * amount, 0.0f,
                                              POINT_RADIUS_MAX);
      }
    }
  };
  /* The handles toward smooth ones: in line along the way from the point before to the one after,
   * each a third of its side's length. */
  const auto smooth_handles = [&]() {
    for (const SmoothSlot &slot : slots) {
      Curve &curve = drawing.curves[slot.curve];
      CurvePoint &cp = curve.points[slot.index];
      if (!moves[cp.point]) {
        continue;
      }
      const float2 here = drawing.points[cp.point].pos;
      const float2 before = slot.prev >= 0 ? drawing.points[curve.points[slot.prev].point].pos : here;
      const float2 after = slot.next >= 0 ? drawing.points[curve.points[slot.next].point].pos : here;
      const float2 dir = normalize(after - before);
      if (dot(dir, dir) < 0.5f) {
        continue;
      }
      if (slot.prev >= 0) {
        cp.handle_in = cp.handle_in + (dir * (-distance(here, before) / 3.0f) - cp.handle_in) * factor;
      }
      if (slot.next >= 0) {
        cp.handle_out = cp.handle_out + (dir * (distance(here, after) / 3.0f) - cp.handle_out) * factor;
      }
      cp.handles_set = true;
      /* All the way smooth, they stay in line from now on. */
      if (factor >= 0.999f) {
        cp.aligned = true;
      }
    }
  };
  /* A point goes half its Smoothing toward the middle, so 1 takes a zigzag flat in one go: a whole
   * step would flip it over instead (each point onto the other side). */
  const float lambda = factor * 0.5f;
  for (int r = 0; r < repeat; r++) {
    step(lambda);
    if (s.keep_shape && s.position) {
      /* Taubin: lambda then mu, mu just beyond -lambda (pass-band 0.1). */
      step(-lambda / (1.0f - 0.1f * lambda));
    }
    if (s.handles) {
      smooth_handles();
    }
  }
  return count;
}

}  // namespace oanim
