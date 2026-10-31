#include "data/detail.hh"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>

namespace oanim {

/* drawing_detail_edit_begin / _end pairs in progress (the inner ones do nothing), and the detail
 * module's own raw edits, which must not turn curves final. */
static thread_local int g_edit_depth = 0;

namespace {

struct RawEdit {
  RawEdit() { g_edit_depth++; }
  ~RawEdit() { g_edit_depth--; }
  RawEdit(const RawEdit &) = delete;
  RawEdit &operator=(const RawEdit &) = delete;
};

/* A run: the detail points of one level between two parents (points of lower level), in curve
 * order, with where each sits along the parents' segment. */
struct Run {
  int level = 0;
  int a = 0, b = 0;
  std::vector<int> points;
  std::vector<float> ts;
};

}  // namespace

static int level_at(const VectorDrawing &drawing, const Curve &curve, int i)
{
  return drawing.points[curve.points[i].point].detail.level;
}

int curve_detail_max_level(const VectorDrawing &drawing, const Curve &curve)
{
  int level = 0;
  for (const CurvePoint &cp : curve.points) {
    level = std::max(level, drawing.points[cp.point].detail.level);
  }
  return level;
}

bool drawing_has_detail(const VectorDrawing &drawing)
{
  for (const MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      return true;
    }
  }
  return false;
}

/* A riding point stops riding: its Width keys hold what it had over the line's width, so they go. */
static void stop_riding(MeshPoint &p)
{
  if (p.detail.rides) {
    p.radius_keys = {};
  }
}

/* Whether a curve other than `curve` has the point between two others: a riding point that a line was welded to
 * goes on riding that curve, and takes the line's end along. */
static bool sits_inside_another_curve(const VectorDrawing &drawing, const Curve &curve, int point)
{
  for (const Curve &other : drawing.curves) {
    const int n = int(other.points.size());
    for (int i = 0; &other != &curve && i < n; i++) {
      if (other.points[i].point == point && n >= 3 && (other.closed || (i > 0 && i < n - 1))) {
        return true;
      }
    }
  }
  return false;
}

/* A detail point needs parents on both sides: an open curve's ends, and some point of a closed one,
 * are base points. */
static void normalize_levels(VectorDrawing &drawing, Curve &curve)
{
  const int n = int(curve.points.size());
  const auto make_base = [&](int i) {
    MeshPoint &p = drawing.points[curve.points[i].point];
    if (p.detail.level > 0 && !(p.detail.rides && sits_inside_another_curve(drawing, curve, curve.points[i].point))) {
      stop_riding(p);
      p.detail = {};
    }
  };
  if (n < 3) {
    for (int i = 0; i < n; i++) {
      make_base(i);
    }
    return;
  }
  if (!curve.closed) {
    make_base(0);
    make_base(n - 1);
    return;
  }
  for (int i = 0; i < n; i++) {
    if (level_at(drawing, curve, i) == 0) {
      return;
    }
  }
  make_base(0);
}

static std::vector<Run> curve_runs(const VectorDrawing &drawing, const Curve &curve)
{
  std::vector<Run> runs;
  const int n = int(curve.points.size());
  const int top = curve_detail_max_level(drawing, curve);
  for (int k = 1; k <= top; k++) {
    std::vector<int> parents;
    for (int i = 0; i < n; i++) {
      if (level_at(drawing, curve, i) < k) {
        parents.push_back(i);
      }
    }
    const int count = int(parents.size());
    if (count == 0) {
      continue;
    }
    const int pairs = curve.closed ? count : count - 1;
    for (int j = 0; j < pairs; j++) {
      Run run;
      run.level = k;
      run.a = parents[j];
      run.b = parents[(j + 1) % count];
      for (int i = (run.a + 1) % n; i != run.b; i = (i + 1) % n) {
        if (level_at(drawing, curve, i) == k) {
          run.points.push_back(i);
        }
      }
      if (run.points.empty()) {
        continue;
      }
      /* Strictly in order along the segment, and off its ends. */
      float last = 0.0f;
      for (const int i : run.points) {
        const float t = std::clamp(drawing.points[curve.points[i].point].detail.t, 1e-3f, 1.0f - 1e-3f);
        last = std::min(std::max(t, last + 1e-4f), 1.0f - 1e-4f);
        run.ts.push_back(last);
      }
      runs.push_back(std::move(run));
    }
  }
  return runs;
}

static float2 bezier_derivative(const float2 ctrl[4], float t)
{
  const float u = 1.0f - t;
  return (ctrl[1] - ctrl[0]) * (3.0f * u * u) + (ctrl[2] - ctrl[1]) * (6.0f * u * t) + (ctrl[3] - ctrl[2]) * (3.0f * t * t);
}

/* A parent segment's frame at t: along it, across it (to the left), and its length (the chord, or
 * a quarter of the control polygon where the chord is short, as on a closed curve's one segment). */
struct SegmentFrame {
  float2 along, across;
  float length = 1.0f;
  float2 local(float2 w) const { return float2(dot(w, along), dot(w, across)) / length; }
  float2 world(float2 v) const { return (along * v.x + across * v.y) * length; }
};

static SegmentFrame segment_frame(const float2 ctrl[4], float t)
{
  SegmentFrame f;
  const float chord = distance(ctrl[0], ctrl[3]);
  const float polygon = distance(ctrl[0], ctrl[1]) + distance(ctrl[1], ctrl[2]) + distance(ctrl[2], ctrl[3]);
  f.length = std::max({chord, 0.25f * polygon, 1e-6f});
  float2 d = bezier_derivative(ctrl, t);
  if (length(d) < 1e-9f) {
    d = ctrl[3] - ctrl[0];
  }
  if (length(d) < 1e-9f) {
    d = {1.0f, 0.0f};
  }
  f.along = d / length(d);
  f.across = {-f.along.y, f.along.x};
  return f;
}

/* The line's width on a segment `t` along it: it runs by distance, not by the curve parameter. */
static float segment_radius(const float2 ctrl[4], float r0, float r1, float t)
{
  float upto = 0.0f, total = 0.0f;
  float2 last = ctrl[0];
  for (int i = 1; i <= 24; i++) {
    const float at = float(i) / 24.0f;
    const float2 p = bezier_eval(ctrl, at);
    total += distance(last, p);
    upto += at <= t ? distance(last, p) : 0.0f;
    last = p;
  }
  return r0 + (r1 - r0) * (total > 1e-12f ? upto / total : t);
}

/* `keep_level`: capturing, detail points up to this level (the ones shown at the curve's edit level)
 * that didn't move themselves keep their place when their segment moved, instead of riding it: what
 * is edited on a level stays as it is there (-1: every detail point rides). Riding points always ride.
 * `riding_only`: only they are placed, the rest staying where they are (bones have just put them there). */
static void curve_sync(VectorDrawing &drawing, Curve &curve, bool capture, int keep_level = -1, bool riding_only = false)
{
  normalize_levels(drawing, curve);
  const std::vector<Run> runs = curve_runs(drawing, curve);
  if (runs.empty()) {
    return;
  }
  const int n = int(curve.points.size());
  /* The curve as it stands at each level, built up level by level. */
  std::vector<float2> pos(n), in(n), out(n);
  for (int i = 0; i < n; i++) {
    pos[i] = drawing.points[curve.points[i].point].pos;
    in[i] = curve.points[i].handle_in;
    out[i] = curve.points[i].handle_out;
  }
  for (const Run &run : runs) {
    const float2 ctrl[4] = {pos[run.a], pos[run.a] + out[run.a], pos[run.b] + in[run.b], pos[run.b]};
    const int m = int(run.points.size());
    for (int j = 0; j < m; j++) {
      const int i = run.points[j];
      const float t = run.ts[j];
      const float before = j > 0 ? run.ts[j - 1] : 0.0f, after = j + 1 < m ? run.ts[j + 1] : 1.0f;
      const SegmentFrame frame = segment_frame(ctrl, t);
      const float2 at = bezier_eval(ctrl, t), d = bezier_derivative(ctrl, t);
      /* The segment split here (de Casteljau): the handles each piece gives this point. */
      const float2 split_in = d * (-(t - before) / 3.0f), split_out = d * ((after - t) / 3.0f);
      CurvePoint &cp = curve.points[i];
      MeshPoint &p = drawing.points[cp.point];
      PointDetail &dt = p.detail;
      if (riding_only && !dt.rides) {
        pos[i] = p.pos;
        in[i] = cp.handle_in;
        out[i] = cp.handle_out;
        continue;
      }
      if (dt.rides) {
        const float line = segment_radius(ctrl, drawing.points[curve.points[run.a].point].radius,
                                          drawing.points[curve.points[run.b].point].radius, t);
        if (capture && dt.built && p.radius != dt.built_radius) {
          dt.radius_offset = p.radius - line;
        }
        p.radius = line + dt.radius_offset;
        dt.built_radius = p.radius;
      }
      if (capture) {
        /* A shown point whose segment moved under it (its offsets now place it elsewhere) stays. */
        const bool keep = dt.built && dt.level <= keep_level && !dt.rides;
        if (!dt.built || p.pos != dt.built_pos || (keep && at + frame.world(dt.pos) != dt.built_pos)) {
          dt.pos = frame.local(p.pos - at);
        }
        if (!dt.built || cp.handle_in != dt.built_in ||
            (keep && split_in + frame.world(dt.in) != dt.built_in))
        {
          dt.in = frame.local(cp.handle_in - split_in);
        }
        if (!dt.built || cp.handle_out != dt.built_out ||
            (keep && split_out + frame.world(dt.out) != dt.built_out))
        {
          dt.out = frame.local(cp.handle_out - split_out);
        }
      }
      p.pos = at + frame.world(dt.pos);
      cp.handle_in = split_in + frame.world(dt.in);
      cp.handle_out = split_out + frame.world(dt.out);
      cp.handles_set = true;
      dt.built_pos = p.pos;
      dt.built_in = cp.handle_in;
      dt.built_out = cp.handle_out;
      dt.built = true;
      pos[i] = p.pos;
      in[i] = cp.handle_in;
      out[i] = cp.handle_out;
    }
    /* The parents' handles facing the run shorten as the segment splits. */
    out[run.a] = out[run.a] * run.ts.front();
    in[run.b] = in[run.b] * (1.0f - run.ts.back());
  }
}

void drawing_detail_ride(VectorDrawing &drawing)
{
  for (Curve &curve : drawing.curves) {
    const bool rides = std::any_of(curve.points.begin(), curve.points.end(),
                                   [&](const CurvePoint &cp) { return drawing.points[cp.point].detail.rides; });
    if (rides) {
      curve_sync(drawing, curve, false, -1, true);
    }
  }
}

bool drawing_curve_moves(const VectorDrawing &drawing, int curve)
{
  for (const CurvePoint &cp : drawing.curves[curve].points) {
    const MeshPoint &p = drawing.points[cp.point];
    if (p.pos_keys.animated() || !p.pos_keys.actions.empty() || p.radius_keys.animated() ||
        !p.radius_keys.actions.empty() || cp.curvature_keys.animated() || !cp.curvature_keys.actions.empty() ||
        cp.handle_in_keys.animated() || !cp.handle_in_keys.actions.empty() || cp.handle_out_keys.animated() ||
        !cp.handle_out_keys.actions.empty() || !p.weights.empty())
    {
      return true;
    }
  }
  return false;
}

bool drawing_riding_selected(const VectorDrawing &drawing)
{
  return std::any_of(drawing.points.begin(), drawing.points.end(),
                     [](const MeshPoint &p) { return p.selected && p.detail.rides; });
}

void drawing_riding_delete_selected(VectorDrawing &drawing)
{
  if (!drawing_riding_selected(drawing)) {
    return;
  }
  drawing_ensure_ids(drawing);
  std::vector<uint64_t> ids;
  for (const Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    for (int i = 0; i < n; i++) {
      const MeshPoint &p = drawing.points[curve.points[i].point];
      const int level = p.detail.level, before = (i + n - 1) % n, after = (i + 1) % n;
      if (p.selected && p.detail.rides && level_at(drawing, curve, before) <= level &&
          level_at(drawing, curve, after) <= level)
      {
        ids.push_back(p.id);
      }
    }
  }
  RawEdit raw;
  for (const uint64_t id : ids) {
    drawing_dissolve_point(drawing, drawing_point_index(drawing, id));
  }
  drawing_detail_sync(drawing, false);
}

void drawing_riding_parents(VectorDrawing &drawing,
                            const std::function<void(MeshPoint &, const MeshPoint &, const MeshPoint &, float)> &f)
{
  for (const Curve &curve : drawing.curves) {
    for (const Run &run : curve_runs(drawing, curve)) {
      for (size_t j = 0; j < run.points.size(); j++) {
        MeshPoint &p = drawing.points[curve.points[run.points[j]].point];
        if (p.detail.rides) {
          f(p, drawing.points[curve.points[run.a].point], drawing.points[curve.points[run.b].point], run.ts[j]);
        }
      }
    }
  }
}

std::vector<uint64_t> drawing_riding_on_shapes(const VectorDrawing &drawing, const std::vector<bool> &shapes)
{
  std::set<int> curves;
  for (size_t s = 0; s < drawing.shapes.size() && s < shapes.size(); s++) {
    if (shapes[s]) {
      for (const int c : shape_curves(drawing, drawing.shapes[s])) {
        curves.insert(c);
      }
    }
  }
  std::vector<uint64_t> ids;
  for (const int c : curves) {
    for (const CurvePoint &cp : drawing.curves[c].points) {
      const MeshPoint &p = drawing.points[cp.point];
      if (p.detail.rides && std::find(ids.begin(), ids.end(), p.id) == ids.end()) {
        ids.push_back(p.id);
      }
    }
  }
  return ids;
}

void drawing_riding_skins(const VectorDrawing &drawing, std::vector<Transform2D> &skins)
{
  const bool riding = std::any_of(drawing.points.begin(), drawing.points.end(),
                                  [](const MeshPoint &p) { return p.detail.rides; });
  const auto differs = [](const Transform2D &s, const Transform2D &o) {
    return s.a != o.a || s.b != o.b || s.c != o.c || s.d != o.d || s.tx != o.tx || s.ty != o.ty;
  };
  /* A drawing moved whole carries them as it does the rest. */
  if (!riding || skins.size() < drawing.points.size() ||
      std::none_of(skins.begin(), skins.end(), [&](const Transform2D &s) { return differs(s, skins.front()); }))
  {
    return;
  }
  VectorDrawing posed = drawing;
  for (size_t i = 0; i < posed.points.size(); i++) {
    posed.points[i].pos = skins[i].apply(posed.points[i].pos);
  }
  for (Curve &curve : posed.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.handle_in = skins[size_t(cp.point)].apply_vector(cp.handle_in);
      cp.handle_out = skins[size_t(cp.point)].apply_vector(cp.handle_out);
    }
  }
  drawing_detail_ride(posed);
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    const auto frame_of = [&](const VectorDrawing &from, const Run &run, float t) {
      const Curve &on = from.curves[c];
      const float2 a = from.points[on.points[run.a].point].pos, b = from.points[on.points[run.b].point].pos;
      const float2 ctrl[4] = {a, a + on.points[run.a].handle_out, b + on.points[run.b].handle_in, b};
      return segment_frame(ctrl, t);
    };
    for (const Run &run : curve_runs(drawing, curve)) {
      for (size_t j = 0; j < run.points.size(); j++) {
        const int point = curve.points[run.points[j]].point;
        if (!drawing.points[point].detail.rides) {
          continue;
        }
        /* What takes the segment's frame in the drawing to its frame as it bends: an offset turns and scales with it. */
        const SegmentFrame from = frame_of(drawing, run, run.ts[j]), to = frame_of(posed, run, run.ts[j]);
        const float scale = to.length / from.length;
        Transform2D skin;
        skin.a = skin.d = dot(from.along, to.along) * scale;
        skin.b = (from.along.x * to.along.y - from.along.y * to.along.x) * scale;
        skin.c = -skin.b;
        const float2 at = posed.points[point].pos - skin.apply(drawing.points[point].pos);
        skin.tx = at.x;
        skin.ty = at.y;
        skins[size_t(point)] = skin;
      }
    }
  }
}

/* Whether a curve at edit level `level` shows a point. */
static bool shown_at(const MeshPoint &p, int level)
{
  return p.detail.level <= level || p.detail.rides;
}

std::vector<int> drawing_curve_detail_levels(const VectorDrawing &drawing)
{
  std::vector<int> levels(drawing.curves.size(), -1);
  for (const Shape &shape : drawing.shapes) {
    for (const ShapeEdge &e : shape.edges) {
      if (e.curve >= 0 && e.curve < int(levels.size())) {
        levels[e.curve] = std::max(levels[e.curve], shape.detail_level);
      }
    }
  }
  for (int &level : levels) {
    level = level < 0 ? INT_MAX : level;
  }
  return levels;
}

VectorDrawing drawing_detail_shown_curves(const VectorDrawing &drawing)
{
  VectorDrawing shown = drawing;
  if (!drawing_has_detail(drawing)) {
    return shown;
  }
  const std::vector<int> levels = drawing_curve_detail_levels(drawing);
  for (size_t c = 0; c < shown.curves.size(); c++) {
    std::erase_if(shown.curves[c].points, [&](const CurvePoint &cp) { return !shown_at(drawing.points[cp.point], levels[c]); });
  }
  drawing_detail_final_handles(shown);
  return shown;
}

void drawing_detail_update_shown(VectorDrawing &drawing)
{
  for (MeshPoint &p : drawing.points) {
    p.detail.hidden = false;
  }
  if (!drawing_has_detail(drawing)) {
    return;
  }
  const std::vector<int> levels = drawing_curve_detail_levels(drawing);
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    for (CurvePoint &cp : drawing.curves[c].points) {
      MeshPoint &p = drawing.points[cp.point];
      if (!shown_at(p, levels[c])) {
        p.detail.hidden = true;
        p.selected = false;
        cp.select_in = cp.select_out = false;
      }
    }
  }
}

void drawing_detail_sync(VectorDrawing &drawing, bool capture)
{
  if (drawing_has_detail(drawing)) {
    const std::vector<int> levels = capture ? drawing_curve_detail_levels(drawing) : std::vector<int>();
    for (size_t c = 0; c < drawing.curves.size(); c++) {
      Curve &curve = drawing.curves[c];
      if (curve_detail_max_level(drawing, curve) > 0) {
        curve_sync(drawing, curve, capture, capture ? levels[c] : -1);
      }
    }
  }
  drawing_detail_update_shown(drawing);
}

/* How much each handle shortens in the final shape: the product of its runs' splits. */
static void curve_handle_factors(const VectorDrawing &drawing, const Curve &curve, std::vector<float> &r_in,
                                 std::vector<float> &r_out)
{
  r_in.assign(curve.points.size(), 1.0f);
  r_out.assign(curve.points.size(), 1.0f);
  for (const Run &run : curve_runs(drawing, curve)) {
    r_out[run.a] *= run.ts.front();
    r_in[run.b] *= 1.0f - run.ts.back();
  }
}

static void curve_final_handles(VectorDrawing &drawing, Curve &curve)
{
  std::vector<float> in, out;
  curve_handle_factors(drawing, curve, in, out);
  for (size_t i = 0; i < curve.points.size(); i++) {
    curve.points[i].handle_in = curve.points[i].handle_in * in[i];
    curve.points[i].handle_out = curve.points[i].handle_out * out[i];
  }
}

void drawing_detail_handle_factors(const VectorDrawing &drawing, int curve, std::vector<float> &r_in,
                                   std::vector<float> &r_out)
{
  curve_handle_factors(drawing, drawing.curves[curve], r_in, r_out);
}

void drawing_detail_shown_handle_factors(const VectorDrawing &drawing, std::vector<std::vector<float>> &r_in,
                                         std::vector<std::vector<float>> &r_out)
{
  r_in.assign(drawing.curves.size(), {});
  r_out.assign(drawing.curves.size(), {});
  const bool detail = drawing_has_detail(drawing);
  const std::vector<int> levels = detail ? drawing_curve_detail_levels(drawing) : std::vector<int>();
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve shown = drawing.curves[c];
    if (detail) {
      std::erase_if(shown.points, [&](const CurvePoint &cp) { return !shown_at(drawing.points[cp.point], levels[c]); });
    }
    if (detail && curve_detail_max_level(drawing, shown) > 0) {
      curve_handle_factors(drawing, shown, r_in[c], r_out[c]);
    }
    else {
      r_in[c].assign(shown.points.size(), 1.0f);
      r_out[c].assign(shown.points.size(), 1.0f);
    }
  }
}

void drawing_detail_final_handles(VectorDrawing &drawing)
{
  if (!drawing_has_detail(drawing)) {
    return;
  }
  for (Curve &curve : drawing.curves) {
    if (curve_detail_max_level(drawing, curve) > 0) {
      curve_final_handles(drawing, curve);
    }
  }
}

/* The curve's detail worked out again from its final shape (every point where it shows, every
 * handle as drawn): points marked in `retime` (and the rest of their runs) get their place along
 * their parents' segment from how far along the curve they are between them. */
static void curve_from_final(VectorDrawing &drawing, Curve &curve, const std::vector<bool> &retime)
{
  normalize_levels(drawing, curve);
  const int n = int(curve.points.size());
  if (curve_detail_max_level(drawing, curve) == 0 || n < 3) {
    return;
  }
  /* How far along the curve each point is. */
  const int segments = curve.segment_count();
  std::vector<float> along(size_t(n) + 1, 0.0f);
  std::vector<float2> samples;
  for (int s = 0; s < segments; s++) {
    samples.clear();
    curve_segment_sample(drawing, curve, s, 16, samples);
    int a, b;
    curve_segment_points(curve, s, &a, &b);
    samples.push_back(drawing.points[b].pos);
    float l = 0.0f;
    for (size_t k = 0; k + 1 < samples.size(); k++) {
      l += distance(samples[k], samples[k + 1]);
    }
    along[size_t(s) + 1] = along[size_t(s)] + l;
  }
  const float total = along[size_t(segments)];
  for (const Run &run : curve_runs(drawing, curve)) {
    bool any = false;
    for (const int i : run.points) {
      any = any || retime[i];
    }
    if (!any) {
      continue;
    }
    const float from = along[run.a];
    float to = along[run.b];
    if (run.b <= run.a) {
      to += total;
    }
    for (const int i : run.points) {
      float at = along[i];
      if (at < from) {
        at += total;
      }
      drawing.points[curve.points[i].point].detail.t = (at - from) / std::max(to - from, 1e-9f);
    }
  }
  /* The handles at their own levels, and the offsets from the shape. */
  std::vector<float> in, out;
  curve_handle_factors(drawing, curve, in, out);
  for (int i = 0; i < n; i++) {
    CurvePoint &cp = curve.points[i];
    cp.handle_in = cp.handle_in / std::max(in[i], 1e-6f);
    cp.handle_out = cp.handle_out / std::max(out[i], 1e-6f);
    drawing.points[cp.point].detail.built = false;
  }
  curve_sync(drawing, curve, true);
}

static std::vector<uint64_t> curve_ids(const VectorDrawing &drawing, const Curve &curve)
{
  std::vector<uint64_t> ids;
  for (const CurvePoint &cp : curve.points) {
    ids.push_back(drawing.points[cp.point].id);
  }
  return ids;
}

static void curve_parent_ids(const VectorDrawing &drawing, const Curve &curve,
                             std::map<uint64_t, std::pair<uint64_t, uint64_t>> &r_parents)
{
  const auto id = [&](int i) { return drawing.points[curve.points[i].point].id; };
  for (const Run &run : curve_runs(drawing, curve)) {
    for (const int i : run.points) {
      r_parents[id(i)] = {id(run.a), id(run.b)};
    }
  }
}

static DetailEdit edit_begin(VectorDrawing &drawing, bool force)
{
  DetailEdit edit;
  if (g_edit_depth++ > 0 || !(force || drawing_has_detail(drawing))) {
    return edit;
  }
  edit.active = true;
  drawing_ensure_ids(drawing);
  drawing_detail_sync(drawing, true);
  edit.curves.resize(drawing.curves.size());
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve &curve = drawing.curves[c];
    if (curve_detail_max_level(drawing, curve) == 0) {
      continue;
    }
    DetailEdit::CurveState &state = edit.curves[c];
    state.ids = curve_ids(drawing, curve);
    for (const CurvePoint &cp : curve.points) {
      state.in.push_back(cp.handle_in);
      state.out.push_back(cp.handle_out);
    }
    curve_parent_ids(drawing, curve, edit.parents);
    curve_final_handles(drawing, curve);
  }
  return edit;
}

DetailEdit drawing_detail_edit_begin(VectorDrawing &drawing)
{
  return edit_begin(drawing, false);
}

void drawing_detail_edit_end(VectorDrawing &drawing, DetailEdit &edit)
{
  g_edit_depth--;
  if (!edit.active) {
    return;
  }
  edit.active = false;
  drawing_ensure_ids(drawing);
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve &curve = drawing.curves[c];
    normalize_levels(drawing, curve);
    if (curve_detail_max_level(drawing, curve) == 0) {
      continue;
    }
    const std::vector<uint64_t> ids = curve_ids(drawing, curve);
    if (c < edit.curves.size() && edit.curves[c].ids == ids) {
      /* Untouched: back as it was. */
      for (size_t i = 0; i < curve.points.size(); i++) {
        curve.points[i].handle_in = edit.curves[c].in[i];
        curve.points[i].handle_out = edit.curves[c].out[i];
      }
      continue;
    }
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> parents;
    curve_parent_ids(drawing, curve, parents);
    std::vector<bool> retime(curve.points.size(), false);
    for (size_t i = 0; i < curve.points.size(); i++) {
      const uint64_t id = ids[i];
      const auto now = parents.find(id), was = edit.parents.find(id);
      retime[i] = now != parents.end() && (was == edit.parents.end() || was->second != now->second);
    }
    curve_from_final(drawing, curve, retime);
  }
  drawing_detail_update_shown(drawing);
}

int drawing_knife_level(const VectorDrawing &drawing, int curve)
{
  const std::vector<int> levels = drawing_curve_detail_levels(drawing);
  if (curve < 0 || curve >= int(levels.size()) || levels[curve] == INT_MAX) {
    return 0;
  }
  return std::min(levels[curve], DETAIL_LEVEL_MAX);
}

int drawing_detail_split(VectorDrawing &drawing, ShapeEdge edge, float t, int level)
{
  /* A plain point on a curve that moves would change the line on every frame the curve has moved. */
  const bool rides = level == 0 && drawing_curve_moves(drawing, edge.curve);
  /* A cut into a run whose detail points are all where the splits of their parents' segment put them
   * (none moved since: no offsets), or into a segment with nothing finer on it yet: the cut is one
   * more exact split of the parents' segment, at a place along it known exactly, and the parents keep
   * their handles. Worked out from the final shape instead, the run's places would be guessed from
   * the length along the curve, and the parents' handles from that guess: removing the points later
   * wouldn't give back the curve exactly. */
  drawing_ensure_ids(drawing);
  /* What has moved, by the latest edits too. */
  drawing_detail_sync(drawing, true);
  int clamped = rides ? 1 : std::clamp(level, 0, DETAIL_LEVEL_MAX);
  Curve &cut = drawing.curves[edge.curve];
  const int n = int(cut.points.size());
  const auto level_of = [&](int i) { return drawing.points[cut.points[i].point].detail.level; };
  const auto untouched = [&](int i) {
    const PointDetail &d = drawing.points[cut.points[i].point].detail;
    return length(d.pos) + length(d.in) + length(d.out) < 1e-6f &&
           length(d.setup_pos) + length(d.setup_in) + length(d.setup_out) < 1e-6f;
  };
  int ia = 0, ib = 0;
  const auto exact_at = [&](int at) {
    bool can = at > 0;
    ia = edge.segment;
    ib = (edge.segment + 1) % n;
    for (int guard = 0; can && level_of(ia) == at && guard < n; guard++) {
      can = untouched(ia) && (cut.closed || ia > 0);
      ia = (ia + n - 1) % n;
    }
    for (int guard = 0; can && level_of(ib) == at && guard < n; guard++) {
      can = untouched(ib) && (cut.closed || ib < n - 1);
      ib = (ib + 1) % n;
    }
    return can && level_of(ia) < at && level_of(ib) < at && ia != ib && cut.points[ia].handles_set &&
           cut.points[ib].handles_set;
  };
  bool exact = exact_at(clamped);
  /* A riding point keeps the curve exactly: past detail that has moved, it goes a level up, where it is the only
   * thing on its segment. */
  for (int up = 2; rides && !exact && up <= DETAIL_LEVEL_MAX; up++) {
    exact = exact_at(up);
    clamped = exact ? up : clamped;
  }
  if (rides && !exact) {
    exact_at(clamped);
  }
  level = rides ? clamped : level;
  /* An exact cut changes nothing else: the rest of the curve goes back as it was. */
  struct Kept {
    PointDetail detail;
    float2 in, out;
  };
  std::map<uint64_t, Kept> kept;
  float t_parent = t;
  if (exact) {
    for (const CurvePoint &cp : cut.points) {
      kept[drawing.points[cp.point].id] = {drawing.points[cp.point].detail, cp.handle_in, cp.handle_out};
    }
    const int from = edge.segment, to = (edge.segment + 1) % n;
    const float ta = from == ia ? 0.0f : drawing.points[cut.points[from].point].detail.t;
    const float tb = to == ib ? 1.0f : drawing.points[cut.points[to].point].detail.t;
    t_parent = ta + (tb - ta) * t;
  }

  DetailEdit edit = edit_begin(drawing, level > 0);
  const int point = drawing_split_segment(drawing, edge, t);
  drawing.points[point].detail = {};
  drawing.points[point].detail.level = clamped;
  drawing.points[point].detail.rides = rides;
  drawing_ensure_ids(drawing);
  const uint64_t id = drawing.points[point].id;
  drawing_detail_edit_end(drawing, edit);

  if (exact) {
    Curve &curve = drawing.curves[edge.curve];
    for (CurvePoint &cp : curve.points) {
      MeshPoint &p = drawing.points[cp.point];
      if (const auto it = kept.find(p.id); it != kept.end()) {
        p.detail = it->second.detail;
        cp.handle_in = it->second.in;
        cp.handle_out = it->second.out;
        continue;
      }
      PointDetail &d = p.detail;
      d.t = t_parent;
      d.pos = d.in = d.out = {};
      d.setup_pos = d.setup_in = d.setup_out = {};
      d.built = false;
    }
    RawEdit raw;
    drawing_detail_sync(drawing, false);
  }
  return drawing_point_index(drawing, id);
}

std::vector<int> shape_curves(const VectorDrawing &drawing, const Shape &shape)
{
  std::set<int> curves;
  for (const ShapeEdge &e : shape.edges) {
    if (e.curve >= 0 && e.curve < int(drawing.curves.size())) {
      curves.insert(e.curve);
    }
  }
  return {curves.begin(), curves.end()};
}

int shape_detail_max_level(const VectorDrawing &drawing, const Shape &shape)
{
  /* Riding points aren't detail to the eye: they show with the curve's own at every level. */
  int level = 0;
  for (const int c : shape_curves(drawing, shape)) {
    for (const CurvePoint &cp : drawing.curves[c].points) {
      const PointDetail &detail = drawing.points[cp.point].detail;
      level = std::max(level, detail.rides ? 0 : detail.level);
    }
  }
  return level;
}

int shape_detail_point_count(const VectorDrawing &drawing, const Shape &shape)
{
  int count = 0;
  for (const int c : shape_curves(drawing, shape)) {
    for (const CurvePoint &cp : drawing.curves[c].points) {
      count += drawing.points[cp.point].detail.level > 0 && !drawing.points[cp.point].detail.rides ? 1 : 0;
    }
  }
  return count;
}

int shape_detail_level_limit(const VectorDrawing &drawing, const Shape &shape)
{
  return std::min(shape_detail_max_level(drawing, shape) + 1, DETAIL_LEVEL_MAX);
}

/* The curves of the picked shapes. */
static std::set<int> picked_curves(const VectorDrawing &drawing, const std::vector<bool> &shapes)
{
  std::set<int> curves;
  for (size_t s = 0; s < drawing.shapes.size() && s < shapes.size(); s++) {
    if (shapes[s]) {
      for (const int c : shape_curves(drawing, drawing.shapes[s])) {
        curves.insert(c);
      }
    }
  }
  return curves;
}

bool drawing_detail_selected(const VectorDrawing &drawing)
{
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      const MeshPoint &p = drawing.points[cp.point];
      if (p.detail.level > 0 && !p.detail.rides && (p.selected || cp.select_in || cp.select_out)) {
        return true;
      }
    }
  }
  return false;
}

int drawing_detail_smooth_selected(VectorDrawing &drawing, float influence)
{
  drawing_detail_sync(drawing, true);
  const float keep = 1.0f - std::clamp(influence, 0.0f, 1.0f);
  int changed = 0;
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      MeshPoint &p = drawing.points[cp.point];
      PointDetail &dt = p.detail;
      if (dt.level == 0) {
        continue;
      }
      if (p.selected) {
        dt.pos = dt.pos * keep;
        dt.in = dt.in * keep;
        dt.out = dt.out * keep;
        changed++;
        continue;
      }
      for (const bool in : {true, false}) {
        if (in ? cp.select_in : cp.select_out) {
          (in ? dt.in : dt.out) = (in ? dt.in : dt.out) * keep;
          changed++;
        }
      }
    }
  }
  if (changed > 0) {
    drawing_detail_sync(drawing, false);
  }
  return changed;
}

int drawing_detail_raise_selected(VectorDrawing &drawing, int *r_skipped, bool *r_keys_dropped)
{
  *r_skipped = 0;
  *r_keys_dropped = false;
  RawEdit raw;
  drawing_detail_sync(drawing, true);
  std::vector<int> uses(drawing.points.size(), 0);
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      uses[cp.point]++;
    }
  }
  int raised = 0;
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    std::vector<int> picked;
    for (int i = 0; i < n; i++) {
      const MeshPoint &p = drawing.points[curve.points[i].point];
      if (p.selected && !p.detail.hidden) {
        picked.push_back(i);
      }
    }
    if (picked.empty()) {
      continue;
    }
    /* Each point's level as it would be, one picked point at a time. */
    std::vector<int> level(n);
    for (int i = 0; i < n; i++) {
      level[i] = drawing.points[curve.points[i].point].detail.level;
    }
    std::vector<bool> retime(n, false);
    for (const int i : picked) {
      const int up = level[i] + 1;
      bool ok = uses[curve.points[i].point] == 1 && up <= DETAIL_LEVEL_MAX && n >= 3 &&
                (curve.closed || (i != 0 && i != n - 1));
      if (ok && curve.closed) {
        int coarser = 0;
        for (int j = 0; j < n; j++) {
          coarser += j != i && level[j] < up ? 1 : 0;
        }
        ok = coarser >= 2;
      }
      if (!ok) {
        (*r_skipped)++;
        continue;
      }
      level[i] = up;
      retime[i] = true;
    }
    if (std::none_of(retime.begin(), retime.end(), [](bool b) { return b; })) {
      continue;
    }
    /* Worked out again from the curve as it shows, with the new levels. */
    curve_final_handles(drawing, curve);
    for (int i = 0; i < n; i++) {
      if (!retime[i]) {
        continue;
      }
      CurvePoint &cp = curve.points[i];
      MeshPoint &p = drawing.points[cp.point];
      if (p.pos_keys.animated() || cp.handle_in_keys.animated() || cp.handle_out_keys.animated() ||
          !p.pos_keys.actions.empty() || !cp.handle_in_keys.actions.empty() || !cp.handle_out_keys.actions.empty())
      {
        *r_keys_dropped = true;
      }
      stop_riding(p);
      p.pos_keys = {};
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      p.detail = {};
      p.detail.level = level[i];
      raised++;
    }
    curve_from_final(drawing, curve, retime);
  }
  drawing_detail_update_shown(drawing);
  return raised;
}

int drawing_detail_lower_selected(VectorDrawing &drawing, int *r_skipped, bool *r_keys_dropped)
{
  *r_skipped = 0;
  *r_keys_dropped = false;
  RawEdit raw;
  drawing_detail_sync(drawing, true);
  int lowered = 0;
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    std::vector<int> level(n);
    for (int i = 0; i < n; i++) {
      level[i] = drawing.points[curve.points[i].point].detail.level;
    }
    std::vector<bool> down(n, false);
    for (int i = 0; i < n; i++) {
      const MeshPoint &p = drawing.points[curve.points[i].point];
      if (!p.selected || p.detail.hidden) {
        continue;
      }
      if (level[i] == 0) {
        (*r_skipped)++;
        continue;
      }
      level[i]--;
      down[i] = true;
    }
    if (std::none_of(down.begin(), down.end(), [](bool b) { return b; })) {
      continue;
    }
    /* A run a point went down from parts there: the rest of it stays what it was, an exact split of the piece it is
     * now on, so its places aren't guessed. */
    std::vector<float> parted(n, -1.0f);
    for (const Run &run : curve_runs(drawing, curve)) {
      for (size_t j = 0; j < run.points.size(); j++) {
        if (down[run.points[j]]) {
          continue;
        }
        float from = 0.0f, to = 1.0f;
        bool part = false;
        for (size_t k = 0; k < run.points.size(); k++) {
          if (down[run.points[k]]) {
            part = true;
            from = k < j ? run.ts[k] : from;
            to = k > j ? std::min(to, run.ts[k]) : to;
          }
        }
        if (part) {
          parted[run.points[j]] = (run.ts[j] - from) / std::max(to - from, 1e-6f);
        }
      }
    }
    /* What changes place: the lowered points (and so the whole run of their new level they now sit
     * in), and the finer detail either side that now has them for a parent, up to the nearest point
     * at their new level or coarser. */
    std::vector<bool> retime(n, false);
    for (int i = 0; i < n; i++) {
      if (!down[i]) {
        continue;
      }
      retime[i] = true;
      for (const int dir : {-1, 1}) {
        for (int k = 1; k < n; k++) {
          int j = i + dir * k;
          if (curve.closed) {
            j = ((j % n) + n) % n;
          }
          else if (j < 0 || j >= n) {
            break;
          }
          if (level[j] <= level[i]) {
            break;
          }
          retime[j] = true;
        }
      }
    }
    curve_final_handles(drawing, curve);
    for (int i = 0; i < n; i++) {
      if (parted[i] >= 0.0f) {
        drawing.points[curve.points[i].point].detail.t = parted[i];
        retime[i] = false;
      }
      if (!down[i]) {
        continue;
      }
      CurvePoint &cp = curve.points[i];
      MeshPoint &p = drawing.points[cp.point];
      if (p.pos_keys.animated() || cp.handle_in_keys.animated() || cp.handle_out_keys.animated() ||
          !p.pos_keys.actions.empty() || !cp.handle_in_keys.actions.empty() || !cp.handle_out_keys.actions.empty())
      {
        *r_keys_dropped = true;
      }
      stop_riding(p);
      p.pos_keys = {};
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      p.detail = {};
      p.detail.level = level[i];
      lowered++;
    }
    curve_from_final(drawing, curve, retime);
    /* A point down to level 0 is a base point: its setup is where it now is. */
    for (int i = 0; i < n; i++) {
      if (down[i] && level[i] == 0) {
        CurvePoint &cp = curve.points[i];
        MeshPoint &p = drawing.points[cp.point];
        p.setup_pos = p.pos;
        p.transform = float2();
        cp.setup_in = cp.handle_in;
        cp.setup_out = cp.handle_out;
        cp.change_in = cp.change_out = float2();
      }
    }
  }
  drawing_detail_update_shown(drawing);
  return lowered;
}

int drawing_detail_delete_higher(VectorDrawing &drawing, const std::vector<bool> &shapes)
{
  RawEdit raw;
  drawing_detail_sync(drawing, true);
  std::set<int> remove;
  for (size_t s = 0; s < drawing.shapes.size() && s < shapes.size(); s++) {
    if (!shapes[s]) {
      continue;
    }
    for (const int c : shape_curves(drawing, drawing.shapes[s])) {
      const Curve &curve = drawing.curves[c];
      for (const CurvePoint &cp : curve.points) {
        const PointDetail &detail = drawing.points[cp.point].detail;
        if (detail.level > drawing.shapes[s].detail_level && !detail.rides) {
          remove.insert(cp.point);
        }
      }
      /* Riding points aren't detail and stay, but for one on detail that goes. */
      for (const Run &run : curve_runs(drawing, curve)) {
        const bool lost = remove.count(curve.points[run.a].point) || remove.count(curve.points[run.b].point);
        for (const int i : run.points) {
          if (lost && drawing.points[curve.points[i].point].detail.rides) {
            remove.insert(curve.points[i].point);
          }
        }
      }
    }
  }
  /* The coarser points keep their handles as they are at their levels: the shape is then what that
   * level shows. Highest index first, as each dissolve removes its point. */
  for (auto it = remove.rbegin(); it != remove.rend(); ++it) {
    drawing_dissolve_point(drawing, *it);
  }
  drawing_detail_sync(drawing, false);
  return int(remove.size());
}

void drawing_detail_remove_points(VectorDrawing &drawing, const std::vector<uint64_t> &ids)
{
  RawEdit raw;
  drawing_detail_sync(drawing, true);
  std::set<int> remove;
  for (const uint64_t id : ids) {
    if (const int p = drawing_point_index(drawing, id); p >= 0) {
      remove.insert(p);
    }
  }
  for (auto it = remove.rbegin(); it != remove.rend(); ++it) {
    drawing_dissolve_point(drawing, *it);
  }
  drawing_detail_sync(drawing, false);
}

/* Turns a curve's detail points into base points as the curve shows (its handles final). Their keys
 * held offsets, so they go; the parents' handle keys scale with their handles. */
static int curve_apply(VectorDrawing &drawing, Curve &curve, bool *r_keys_dropped)
{
  std::vector<float> in, out;
  curve_handle_factors(drawing, curve, in, out);
  int changed = 0;
  for (size_t i = 0; i < curve.points.size(); i++) {
    CurvePoint &cp = curve.points[i];
    MeshPoint &p = drawing.points[cp.point];
    if (p.detail.level > 0) {
      if (p.pos_keys.animated() || cp.handle_in_keys.animated() || cp.handle_out_keys.animated() ||
          !p.pos_keys.actions.empty() || !cp.handle_in_keys.actions.empty() || !cp.handle_out_keys.actions.empty())
      {
        *r_keys_dropped = true;
      }
      p.pos_keys = {};
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      p.detail = {};
      changed++;
      continue;
    }
    /* A handle and its keys' lengths scale alike (the keys hold its turn and added length). */
    for (const bool side_in : {true, false}) {
      const float f = side_in ? in[i] : out[i];
      if (f == 1.0f) {
        continue;
      }
      (side_in ? cp.handle_in : cp.handle_out) = (side_in ? cp.handle_in : cp.handle_out) * f;
      (side_in ? cp.setup_in : cp.setup_out) = (side_in ? cp.setup_in : cp.setup_out) * f;
      (side_in ? cp.change_in : cp.change_out).y *= f;
      Channel<float2> &ch = side_in ? cp.handle_in_keys : cp.handle_out_keys;
      for (Key<float2> &k : ch.keys) {
        k.value.y *= f;
      }
      for (auto &[name, keys] : ch.actions) {
        for (Key<float2> &k : keys) {
          k.value.y *= f;
        }
      }
    }
  }
  return changed;
}

int drawing_detail_apply(VectorDrawing &drawing, const std::vector<bool> &shapes, bool *r_keys_dropped)
{
  RawEdit raw;
  *r_keys_dropped = false;
  drawing_detail_sync(drawing, true);
  int changed = 0;
  for (const int c : picked_curves(drawing, shapes)) {
    changed += curve_apply(drawing, drawing.curves[c], r_keys_dropped);
  }
  for (size_t s = 0; s < drawing.shapes.size() && s < shapes.size(); s++) {
    if (shapes[s]) {
      drawing.shapes[s].detail_level = 0;
    }
  }
  drawing_detail_update_shown(drawing);
  return changed;
}

/* The points of a curve (its final shape) that can be detail at `tolerance`: one at a time, the point
 * nearest the curve its neighbours would make without it (their facing handles lengthened as a split
 * there would have shortened them, the split where it sits along the curve between them), while that is
 * within the tolerance. Ends of an open curve, points on other curves too (`welded`), and enough points
 * to keep a closed curve's shape stay. */
static std::vector<bool> curve_detail_candidates(const VectorDrawing &drawing, const Curve &curve,
                                                 const std::vector<bool> &welded, float tolerance)
{
  const int n = int(curve.points.size());
  std::vector<bool> detail(n, false);
  if (n < (curve.closed ? 4 : 3)) {
    return detail;
  }
  /* How far along the curve each point is. */
  std::vector<float> along(size_t(n) + 1, 0.0f);
  std::vector<float2> samples;
  for (int s = 0; s < curve.segment_count(); s++) {
    samples.clear();
    curve_segment_sample(drawing, curve, s, 16, samples);
    int a, b;
    curve_segment_points(curve, s, &a, &b);
    samples.push_back(drawing.points[b].pos);
    float l = 0.0f;
    for (size_t k = 0; k + 1 < samples.size(); k++) {
      l += distance(samples[k], samples[k + 1]);
    }
    along[size_t(s) + 1] = along[size_t(s)] + l;
  }
  const float total = along[size_t(curve.segment_count())];
  std::vector<float2> in(n), out(n);
  for (int i = 0; i < n; i++) {
    in[i] = curve.points[i].handle_in;
    out[i] = curve.points[i].handle_out;
  }
  int kept = n;
  const auto step = [&](int i, int dir) {
    for (int k = 1; k < n; k++) {
      const int j = curve.closed ? ((i + dir * k) % n + n) % n : i + dir * k;
      if (j < 0 || j >= n) {
        return -1;
      }
      if (!detail[j]) {
        return j;
      }
    }
    return -1;
  };
  const auto at = [&](int i) { return drawing.points[curve.points[i].point].pos; };
  for (;;) {
    if (kept <= (curve.closed ? 3 : 2)) {
      break;
    }
    int best = -1;
    float best_error = tolerance;
    float best_t = 0.5f;
    for (int i = 0; i < n; i++) {
      if (detail[i] || welded[curve.points[i].point] || (!curve.closed && (i == 0 || i == n - 1))) {
        continue;
      }
      const int a = step(i, -1), b = step(i, 1);
      if (a < 0 || b < 0 || a == b) {
        continue;
      }
      float from = along[a], to = along[b], here = along[i];
      if (to <= from) {
        to += total;
      }
      if (here < from) {
        here += total;
      }
      const float split = std::clamp((here - from) / std::max(to - from, 1e-9f), 0.05f, 0.95f);
      const float2 ctrl[4] = {at(a), at(a) + out[a] / split, at(b) + in[b] / (1.0f - split), at(b)};
      const float error = distance(at(i), bezier_eval(ctrl, split));
      if (error <= best_error) {
        best = i;
        best_error = error;
        best_t = split;
      }
    }
    if (best < 0) {
      break;
    }
    const int a = step(best, -1), b = step(best, 1);
    out[a] = out[a] / best_t;
    in[b] = in[b] / (1.0f - best_t);
    detail[best] = true;
    kept--;
  }
  return detail;
}

int drawing_detail_rebuild(VectorDrawing &drawing, const std::vector<bool> &shapes, float tolerance,
                           bool *r_keys_dropped)
{
  *r_keys_dropped = false;
  std::set<int> curves = picked_curves(drawing, shapes);
  /* A curve with riding points stays as it is: flattened, they would lose every pose but the setup. */
  std::erase_if(curves, [&](int c) {
    const std::vector<CurvePoint> &points = drawing.curves[c].points;
    return std::any_of(points.begin(), points.end(),
                       [&](const CurvePoint &cp) { return drawing.points[cp.point].detail.rides; });
  });
  /* Worked out on a copy: when no point can be detail, the drawing stays as it was. */
  VectorDrawing work = drawing;
  bool dropped = false;
  {
    RawEdit raw;
    drawing_detail_sync(work, true);
    for (const int c : curves) {
      curve_apply(work, work.curves[c], &dropped);
    }
  }
  /* Points on more than one curve stay base points (detail sits on one curve). */
  std::vector<bool> welded(work.points.size(), false);
  std::vector<int> uses(work.points.size(), 0);
  for (const Curve &curve : work.curves) {
    for (const CurvePoint &cp : curve.points) {
      welded[cp.point] = ++uses[cp.point] > 1;
    }
  }
  int made = 0;
  for (const int c : curves) {
    Curve &curve = work.curves[c];
    const std::vector<bool> detail = curve_detail_candidates(work, curve, welded, tolerance);
    std::vector<bool> retime(curve.points.size(), false);
    for (size_t i = 0; i < curve.points.size(); i++) {
      if (!detail[i]) {
        continue;
      }
      CurvePoint &cp = curve.points[i];
      MeshPoint &p = work.points[cp.point];
      if (p.pos_keys.animated() || cp.handle_in_keys.animated() || cp.handle_out_keys.animated()) {
        dropped = true;
      }
      p.pos_keys = {};
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      p.detail = {};
      p.detail.level = 1;
      retime[i] = true;
      made++;
    }
    curve_from_final(work, curve, retime);
  }
  if (made == 0) {
    return 0;
  }
  drawing = std::move(work);
  *r_keys_dropped = dropped;
  drawing_detail_update_shown(drawing);
  return made;
}

}  // namespace oanim
