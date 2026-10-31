#include "data/line_cut.hh"

#include <algorithm>
#include <cmath>
#include <functional>

#include "data/detail.hh"

namespace oanim {

namespace {

/* A place on a curve: its segment plus how far along that, so 2.5 is halfway along segment 2. */
struct LineSample {
  float2 pos;
  double at = 0.0;
  /* The line's half width here. */
  float half = 0.0f;
};

struct Stretch {
  double from = 0.0, to = 0.0;
};

/* A curve as drawn: its samples, and which of its segments show as a line. */
struct Line {
  int curve = 0;
  bool closed = false;
  int segments = 0;
  std::vector<bool> drawn;
  std::vector<LineSample> samples;
  float2 lo, hi;
  bool selected = false;
  bool any_drawn = false;
};

}  // namespace

static LineSample sample_at(const VectorDrawing &drawing, const Curve &curve, double at, float half)
{
  const int segment = std::min(int(at), curve.segment_count() - 1);
  const float t = float(at - segment);
  float2 ctrl[4];
  curve_segment_bezier(drawing, curve, segment, ctrl);
  int from = 0, to = 0;
  curve_segment_points(curve, segment, &from, &to);
  const float radius = drawing.points[from].radius + (drawing.points[to].radius - drawing.points[from].radius) * t;
  const float u = 1.0f - t;
  LineSample s;
  s.pos = ctrl[0] * (u * u * u) + ctrl[1] * (3.0f * u * u * t) + ctrl[2] * (3.0f * u * t * t) + ctrl[3] * (t * t * t);
  s.at = at;
  s.half = radius * half;
  return s;
}

/* The drawing's curves that show as lines anywhere. */
static std::vector<Line> drawing_lines(const VectorDrawing &drawing, float pixel)
{
  const std::vector<bool> selected = drawing_selected_shapes(drawing);
  std::vector<Line> lines(drawing.curves.size());
  std::vector<float> half(drawing.curves.size(), 0.0f);
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    lines[c].curve = int(c);
    lines[c].closed = drawing.curves[c].closed;
    lines[c].segments = drawing.curves[c].segment_count();
    lines[c].drawn.assign(size_t(lines[c].segments), false);
  }
  for (size_t i = 0; i < drawing.shapes.size(); i++) {
    const Shape &shape = drawing.shapes[i];
    if (shape.hidden || !shape.has_outline) {
      continue;
    }
    for (const ShapeEdge &edge : shape.edges) {
      Line &line = lines[edge.curve];
      if (!drawing.curves[edge.curve].points[edge.segment].edge_hidden) {
        line.drawn[edge.segment] = true;
        line.any_drawn = true;
        line.selected = line.selected || selected[i];
        half[edge.curve] = std::max(half[edge.curve], shape.style.line_width * 0.5f);
      }
    }
  }
  std::vector<Line> result;
  for (Line &line : lines) {
    if (!line.any_drawn) {
      continue;
    }
    const Curve &curve = drawing.curves[line.curve];
    line.lo = {1e30f, 1e30f};
    line.hi = {-1e30f, -1e30f};
    for (int s = 0; s < line.segments; s++) {
      float2 ctrl[4];
      curve_segment_bezier(drawing, curve, s, ctrl);
      const float rough = distance(ctrl[0], ctrl[1]) + distance(ctrl[1], ctrl[2]) + distance(ctrl[2], ctrl[3]);
      const int steps = std::clamp(int(rough / (pixel * 2.0f)), 6, 200);
      for (int k = s == 0 ? 0 : 1; k <= steps; k++) {
        const LineSample sample = sample_at(drawing, curve, s + double(k) / steps, half[line.curve]);
        line.samples.push_back(sample);
        line.lo = {std::min(line.lo.x, sample.pos.x), std::min(line.lo.y, sample.pos.y)};
        line.hi = {std::max(line.hi.x, sample.pos.x), std::max(line.hi.y, sample.pos.y)};
      }
    }
    result.push_back(std::move(line));
  }
  return result;
}

static bool sweep_covers(const InkSweep &sweep, float2 p, float extra)
{
  if (sweep.centers.size() == 1) {
    return distance(p, sweep.centers[0]) <= sweep.radii[0] + extra;
  }
  for (size_t i = 0; i + 1 < sweep.centers.size(); i++) {
    float t = 0.0f;
    const float d = distance_to_segment(p, sweep.centers[i], sweep.centers[i + 1], &t);
    if (d <= sweep.radii[i] + (sweep.radii[i + 1] - sweep.radii[i]) * t + extra) {
      return true;
    }
  }
  return false;
}

/* Whether a line's box comes near what was brushed. */
static bool sweep_nears(const InkSweep &sweep, const Line &line, float margin)
{
  for (size_t i = 0; i < sweep.centers.size(); i++) {
    const float r = sweep.radii[i] + margin;
    const float2 c = sweep.centers[i];
    if (c.x + r >= line.lo.x && c.x - r <= line.hi.x && c.y + r >= line.lo.y && c.y - r <= line.hi.y) {
      return true;
    }
  }
  return false;
}

/* The stretches of a line `sweep` covers, out to where its edge meets the line. */
static std::vector<Stretch> covered_stretches(const VectorDrawing &drawing, const Line &line, const InkSweep &sweep,
                                              float half)
{
  const Curve &curve = drawing.curves[line.curve];
  const auto covered = [&](const LineSample &s) {
    const int segment = std::min(int(s.at), line.segments - 1);
    return line.drawn[segment] && sweep_covers(sweep, s.pos, s.half);
  };
  /* Where between two samples the edge is. */
  const auto edge = [&](LineSample in, LineSample out) {
    for (int i = 0; i < 14; i++) {
      const LineSample middle = sample_at(drawing, curve, 0.5 * (in.at + out.at), half);
      (covered(middle) ? in : out) = middle;
    }
    return in.at;
  };
  std::vector<Stretch> stretches;
  const std::vector<LineSample> &samples = line.samples;
  bool inside = covered(samples.front());
  double start = samples.front().at;
  for (size_t i = 1; i < samples.size(); i++) {
    const bool now = covered(samples[i]);
    if (now && !inside) {
      start = edge(samples[i], samples[i - 1]);
    }
    else if (!now && inside) {
      stretches.push_back({start, edge(samples[i - 1], samples[i])});
    }
    inside = now;
  }
  if (inside) {
    stretches.push_back({start, samples.back().at});
  }
  return stretches;
}

/* Splits curve `curve` at the stretches' ends, and gives the segments between: what to take away. */
static void cut_stretches(VectorDrawing &drawing, const Line &line, std::vector<Stretch> stretches,
                          std::vector<ShapeEdge> &r_edges)
{
  const int segments = line.segments;
  const auto snap = [&](double at) {
    const double whole = std::round(at);
    return std::abs(at - whole) < 1e-4 ? whole : at;
  };
  std::vector<double> cuts;
  for (Stretch &s : stretches) {
    s.from = snap(s.from);
    s.to = snap(s.to);
    for (const double at : {s.from, s.to}) {
      if (at != std::floor(at)) {
        cuts.push_back(at);
      }
    }
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  /* From the last back, so the segments before stay as they are; a second cut in a segment already cut is that
   * much further along what is left of it. */
  double done = -1.0;
  for (size_t i = cuts.size(); i-- > 0;) {
    const double at = cuts[i];
    const int segment = int(at);
    const double t = (at - segment) / (int(done) == segment && done > 0.0 ? done - segment : 1.0);
    if (drawing_curve_moves(drawing, line.curve)) {
      drawing_detail_split(drawing, {line.curve, segment}, float(t), 0);
    }
    else {
      drawing_split_segment(drawing, {line.curve, segment}, float(t));
    }
    done = at;
  }
  /* The curve's places now: every old point and every cut. */
  std::vector<double> places;
  for (int i = 0; i <= segments; i++) {
    places.push_back(double(i));
  }
  places.insert(places.end(), cuts.begin(), cuts.end());
  std::sort(places.begin(), places.end());
  for (size_t i = 0; i + 1 < places.size(); i++) {
    const double middle = 0.5 * (places[i] + places[i + 1]);
    const bool drawn = line.drawn[std::min(int(middle), segments - 1)];
    for (const Stretch &s : stretches) {
      if (drawn && middle > s.from && middle < s.to) {
        r_edges.push_back({line.curve, int(i)});
        break;
      }
    }
  }
}

/* Takes the segments away: gone, or hidden where a fill runs along them, and it needs them for its edge. */
static void edges_take(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges, const RidingFree &free)
{
  std::vector<bool> filled(drawing.curves.size(), false);
  for (const Shape &shape : drawing.shapes) {
    if (shape.has_fill) {
      for (const ShapeEdge &edge : shape.edges) {
        filled[edge.curve] = true;
      }
    }
  }
  std::vector<ShapeEdge> hide, remove;
  std::vector<uint64_t> riding;
  for (const ShapeEdge &edge : edges) {
    (filled[edge.curve] ? hide : remove).push_back(edge);
    for (const CurvePoint &cp : drawing.curves[edge.curve].points) {
      const MeshPoint &p = drawing.points[cp.point];
      if (p.detail.rides && !filled[edge.curve] && std::find(riding.begin(), riding.end(), p.id) == riding.end()) {
        riding.push_back(p.id);
      }
    }
  }
  if (!riding.empty() && free) {
    free(riding);
  }
  drawing_set_edges_hidden(drawing, hide, true);
  drawing_delete_edges(drawing, remove);
}

static std::vector<Stretch> whole_line(const Line &line)
{
  return {{0.0, double(line.segments)}};
}

/* The line's half width at its widest (a sample's half came from it, at a radius). */
static float line_half(const VectorDrawing &drawing, const Line &line)
{
  float half = 0.0f;
  for (const Shape &shape : drawing.shapes) {
    if (shape.has_outline && !shape.hidden) {
      for (const ShapeEdge &edge : shape.edges) {
        if (edge.curve == line.curve) {
          half = std::max(half, shape.style.line_width * 0.5f);
        }
      }
    }
  }
  return half;
}

bool drawing_lines_erase(VectorDrawing &drawing, const InkSweep &sweep, float pixel, bool whole, bool selected_only,
                         const RidingFree &free)
{
  std::vector<ShapeEdge> edges;
  std::vector<Line> lines = drawing_lines(drawing, pixel);
  for (const Line &line : lines) {
    const float half = line_half(drawing, line);
    if ((selected_only && !line.selected) || !sweep_nears(sweep, line, half)) {
      continue;
    }
    const std::vector<Stretch> covered = covered_stretches(drawing, line, sweep, half);
    if (!covered.empty()) {
      cut_stretches(drawing, line, whole ? whole_line(line) : covered, edges);
    }
  }
  edges_take(drawing, edges, free);
  return !edges.empty();
}

std::vector<std::vector<float2>> drawing_line_paths(const VectorDrawing &drawing, float pixel)
{
  std::vector<std::vector<float2>> paths;
  for (const Line &line : drawing_lines(drawing, pixel)) {
    std::vector<float2> path;
    for (const LineSample &s : line.samples) {
      path.push_back(s.pos);
    }
    paths.push_back(std::move(path));
  }
  return paths;
}

/* Where two pieces of line cross: how far along `a0`-`a1`. */
static bool pieces_cross(float2 a0, float2 a1, float2 b0, float2 b1, float *r_t)
{
  const float2 d = a1 - a0, e = b1 - b0;
  const float den = cross(d, e);
  if (std::abs(den) < 1e-14f) {
    return false;
  }
  const float2 f = b0 - a0;
  const float t = cross(f, e) / den, u = cross(f, d) / den;
  *r_t = t;
  return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

/* Where `line` crosses another line, or itself, or where another's end meets it, in order along it. */
static std::vector<double> line_crossings(const Line &line, const std::vector<Line> &lines,
                                          const std::vector<std::vector<float2>> &others, float pixel)
{
  std::vector<double> at;
  const std::vector<LineSample> &a = line.samples;
  const auto against = [&](const std::function<float2(size_t)> &point, size_t count, bool self, float2 lo, float2 hi) {
    if (lo.x > line.hi.x || hi.x < line.lo.x || lo.y > line.hi.y || hi.y < line.lo.y) {
      return;
    }
    for (size_t i = 0; i + 1 < a.size(); i++) {
      const float2 p = a[i].pos, q = a[i + 1].pos;
      const float2 box_lo(std::min(p.x, q.x), std::min(p.y, q.y)), box_hi(std::max(p.x, q.x), std::max(p.y, q.y));
      for (size_t j = 0; j + 1 < count; j++) {
        /* A line doesn't cross itself where it runs on, nor round its own join. */
        if (self && (j + 2 > i && j < i + 2)) {
          continue;
        }
        if (self && line.closed && ((i == 0 && j + 2 == count) || (j == 0 && i + 2 == a.size()))) {
          continue;
        }
        const float2 b0 = point(j), b1 = point(j + 1);
        if (std::max(b0.x, b1.x) < box_lo.x || std::min(b0.x, b1.x) > box_hi.x || std::max(b0.y, b1.y) < box_lo.y ||
            std::min(b0.y, b1.y) > box_hi.y)
        {
          continue;
        }
        float t = 0.0f;
        if (pieces_cross(p, q, b0, b1, &t)) {
          at.push_back(a[i].at + (a[i + 1].at - a[i].at) * t);
        }
      }
    }
  };
  /* An end stopping on this line, as a T: it reaches only so far, and needn't cross to meet it. */
  const auto end_meets = [&](float2 end) {
    float best = 1e30f;
    double place = 0.0;
    for (size_t i = 0; i + 1 < a.size(); i++) {
      float t = 0.0f;
      const float d = distance_to_segment(end, a[i].pos, a[i + 1].pos, &t);
      if (d < best) {
        best = d;
        place = a[i].at + (a[i + 1].at - a[i].at) * t;
      }
    }
    float half = 0.0f;
    for (const LineSample &s : a) {
      half = std::max(half, s.half);
    }
    if (best <= half + pixel * 1.5f) {
      at.push_back(place);
    }
  };
  for (const Line &other : lines) {
    const bool self = other.curve == line.curve;
    against([&](size_t i) { return other.samples[i].pos; }, other.samples.size(), self, other.lo, other.hi);
    if (!self && !other.closed) {
      end_meets(other.samples.front().pos);
      end_meets(other.samples.back().pos);
    }
  }
  for (const std::vector<float2> &path : others) {
    float2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
    for (const float2 &p : path) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    against([&](size_t i) { return path[i]; }, path.size(), false, lo, hi);
  }
  std::sort(at.begin(), at.end());
  at.erase(std::unique(at.begin(), at.end(), [](double x, double y) { return std::abs(x - y) < 1e-3; }), at.end());
  return at;
}

bool drawing_lines_trim(VectorDrawing &drawing, const InkSweep &sweep, const std::vector<std::vector<float2>> &others,
                        float pixel, bool selected_only, const RidingFree &free)
{
  const std::vector<Line> lines = drawing_lines(drawing, pixel);
  /* All worked out first: a cut changes the line it is on, which the others' crossings go by. */
  std::vector<std::pair<const Line *, std::vector<Stretch>>> cuts;
  for (const Line &line : lines) {
    const float half = line_half(drawing, line);
    if ((selected_only && !line.selected) || !sweep_nears(sweep, line, half)) {
      continue;
    }
    const std::vector<Stretch> touched = covered_stretches(drawing, line, sweep, half);
    if (touched.empty()) {
      continue;
    }
    const std::vector<double> crossings = line_crossings(line, lines, others, pixel);
    const double end = line.segments;
    std::vector<Stretch> pieces;
    if (crossings.empty()) {
      pieces = line.closed ? whole_line(line) : std::vector<Stretch>{{0.0, end}};
    }
    for (const Stretch &s : touched) {
      if (crossings.empty()) {
        break;
      }
      /* Out to the crossings either side of what was touched. */
      const auto after = std::lower_bound(crossings.begin(), crossings.end(), s.to);
      auto before = std::upper_bound(crossings.begin(), crossings.end(), s.from);
      const bool has_after = after != crossings.end(), has_before = before != crossings.begin();
      const double from = has_before ? *--before : 0.0, to = has_after ? *after : end;
      pieces.push_back({from, to});
      /* Round a closed line's join, to the crossing the other side of it. */
      if (line.closed && !has_before) {
        pieces.push_back({crossings.back(), end});
      }
      if (line.closed && !has_after) {
        pieces.push_back({0.0, crossings.front()});
      }
    }
    cuts.push_back({&line, std::move(pieces)});
  }
  std::vector<ShapeEdge> edges;
  for (auto &[line, pieces] : cuts) {
    cut_stretches(drawing, *line, std::move(pieces), edges);
  }
  edges_take(drawing, edges, free);
  return !edges.empty();
}

void drawing_trim_overshoots(VectorDrawing &drawing, int curve, float reach, float pixel)
{
  const std::vector<Line> lines = drawing_lines(drawing, pixel);
  const auto line = std::find_if(lines.begin(), lines.end(), [&](const Line &l) { return l.curve == curve; });
  if (line == lines.end() || line->closed) {
    return;
  }
  const std::vector<double> crossings = line_crossings(*line, lines, {}, pixel);
  if (crossings.empty()) {
    return;
  }
  /* How long a line it is from one place on it to another. */
  const auto length_between = [&](double from, double to) {
    float total = 0.0f;
    for (size_t i = 0; i + 1 < line->samples.size(); i++) {
      if (line->samples[i].at >= from && line->samples[i + 1].at <= to) {
        total += distance(line->samples[i].pos, line->samples[i + 1].pos);
      }
    }
    return total;
  };
  const double end = line->segments;
  const float head = length_between(0.0, crossings.front()), tail = length_between(crossings.back(), end);
  std::vector<Stretch> pieces;
  /* With one crossing, the line is what is on the longer side of it. */
  const bool one = crossings.size() == 1;
  if (head <= reach && (!one || head <= tail)) {
    pieces.push_back({0.0, crossings.front()});
  }
  if (tail <= reach && (!one || tail < head)) {
    pieces.push_back({crossings.back(), end});
  }
  std::vector<ShapeEdge> edges;
  cut_stretches(drawing, *line, std::move(pieces), edges);
  edges_take(drawing, edges, nullptr);
}

}  // namespace oanim
