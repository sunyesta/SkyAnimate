#include "gpu/brush_stroke.hh"

#include <algorithm>
#include <cmath>

namespace oanim {

/* A runaway stroke (a huge line at a tiny spacing) stops here rather than stall the app. */
static constexpr int MAX_DABS = 100000;
/* The frame is 2 document units tall; size variation's scale is a fraction of it. */
static constexpr float FRAME_HEIGHT = 2.0f;

static uint32_t hash(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

/* 0..1 from a seed and an index. */
static float random01(uint32_t seed, uint32_t i)
{
  return float(hash(seed ^ hash(i + 0x9e3779b9U)) & 0xffffff) / float(0x1000000);
}

/* Smooth noise, -1..1: random values at whole numbers, eased in between. */
static float smooth_noise(uint32_t seed, float x)
{
  const float f = std::floor(x);
  const float t = x - f;
  const uint32_t i = uint32_t(int32_t(f));
  const float a = random01(seed, i) * 2.0f - 1.0f;
  const float b = random01(seed, i + 1) * 2.0f - 1.0f;
  const float s = t * t * (3.0f - 2.0f * t);
  return a + (b - a) * s;
}

static void rgb_to_hsv(float r, float g, float b, float &h, float &s, float &v)
{
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  v = mx;
  s = mx > 0.0f ? d / mx : 0.0f;
  h = 0.0f;
  if (d > 0.0f) {
    h = mx == r ? (g - b) / d : (mx == g ? 2.0f + (b - r) / d : 4.0f + (r - g) / d);
    h = h / 6.0f;
    h -= std::floor(h);
  }
}

static void hsv_to_rgb(float h, float s, float v, float &r, float &g, float &b)
{
  h = (h - std::floor(h)) * 6.0f;
  const int i = int(h) % 6;
  const float f = h - std::floor(h);
  const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
  r = table[i][0];
  g = table[i][1];
  b = table[i][2];
}

uint32_t brush_seed(const BrushSettings &brush, uint64_t layer_id, int shape_index, int frame)
{
  uint32_t seed = hash(uint32_t(layer_id) ^ hash(uint32_t(layer_id >> 32)) ^ hash(uint32_t(shape_index) * 2654435761U));
  if (brush.randomize) {
    const int interval = std::max(1, brush.random_interval);
    const int step = frame >= 0 ? frame / interval : (frame - interval + 1) / interval;
    seed = hash(seed ^ hash(uint32_t(step) + 17U));
  }
  return seed;
}

/* --- Along the line ------------------------------------------------------------------------ */

/* How finely a segment is measured, in document units: fixed, so the dabs don't creep as the view
 * zooms (the outline drawn on screen is cut finer the closer you look). */
static constexpr float PATH_TOLERANCE = 0.0002f;
/* The closest dabs come (document units), whatever the spacing and width. */
static constexpr float MIN_STEP = 0.0005f;
/* How many times over a stretched segment fills its gaps (at 2, 4, 8 and 16 times its length). */
static constexpr int MAX_FILL_LEVEL = 4;
/* Thinning a squashed segment: every other dab fades out at half, every fourth at a quarter... */
static constexpr int MAX_THIN_LEVEL = 6;

/* A segment measured: points along it and the length up to each. */
struct EdgeTable {
  std::vector<float2> points;
  std::vector<float> at;
  float length = 0.0f;
};

static EdgeTable edge_table(const BrushPath::Edge &edge)
{
  EdgeTable t;
  const int steps = bezier_steps(edge.ctrl, 8, PATH_TOLERANCE);
  t.points.resize(steps + 1);
  t.at.assign(steps + 1, 0.0f);
  for (int i = 0; i <= steps; i++) {
    t.points[i] = bezier_eval(edge.ctrl, float(i) / float(steps));
    if (i > 0) {
      t.at[i] = t.at[i - 1] + distance(t.points[i - 1], t.points[i]);
    }
  }
  t.length = t.at.back();
  return t;
}

/* The point a fraction `u` of the way along a segment (by length), and the way it runs there. */
static float2 edge_point(const EdgeTable &t, float u, float2 *r_dir)
{
  const int n = int(t.points.size());
  *r_dir = float2(1.0f, 0.0f);
  if (t.length <= 1e-12f || n < 2) {
    const float2 d = t.points.back() - t.points.front();
    if (length(d) > 1e-12f) {
      *r_dir = normalize(d);
    }
    return t.points.front();
  }
  const float s = std::clamp(u, 0.0f, 1.0f) * t.length;
  int i = int(std::upper_bound(t.at.begin(), t.at.end(), s) - t.at.begin()) - 1;
  i = std::clamp(i, 0, n - 2);
  const float seg = t.at[i + 1] - t.at[i];
  const float2 d = t.points[i + 1] - t.points[i];
  if (seg > 0.0f) {
    *r_dir = d / seg;
  }
  return t.points[i] + d * (seg > 0.0f ? (s - t.at[i]) / seg : 0.0f);
}

BrushPath brush_path(const VectorDrawing &drawing, const Shape &shape, float extra_half)
{
  BrushPath path;
  std::vector<OrientedEdge> chain;
  shape_chain(drawing, shape, chain, &path.closed);
  const float half = shape.style.line_width * 0.5f;
  for (const OrientedEdge &o : chain) {
    const Curve &curve = drawing.curves[o.edge.curve];
    BrushPath::Edge e;
    curve_segment_bezier(drawing, curve, o.edge.segment, e.ctrl);
    int from, to;
    curve_segment_points(curve, o.edge.segment, &from, &to);
    if (o.reversed) {
      std::swap(e.ctrl[0], e.ctrl[3]);
      std::swap(e.ctrl[1], e.ctrl[2]);
      std::swap(from, to);
    }
    e.half0 = drawing.points[from].radius * half + extra_half;
    e.half1 = drawing.points[to].radius * half + extra_half;
    e.visible = !curve.points[o.edge.segment].edge_hidden;
    path.edges.push_back(e);
  }
  return path;
}

void brush_stroke(const BrushPath &shown, const BrushPath &rest_path, const BrushSettings &brush, Color color,
                  const std::vector<float> &aspects, float min_half, uint32_t seed, BrushDabs &r_dabs,
                  const BrushCut *cut, bool fade_thin)
{
  const int edges = int(shown.edges.size());
  const int images = int(aspects.size());
  if (edges == 0 || images == 0) {
    return;
  }
  if (int(r_dabs.per_image.size()) < images) {
    r_dabs.per_image.resize(images);
  }
  /* The rest shape lays the dabs out; without one (or a different one), the line itself. */
  const BrushPath &rest = int(rest_path.edges.size()) == edges ? rest_path : shown;
  std::vector<EdgeTable> rest_table, shown_table;
  for (int e = 0; e < edges; e++) {
    rest_table.push_back(edge_table(rest.edges[e]));
    shown_table.push_back(&rest == &shown ? rest_table.back() : edge_table(shown.edges[e]));
  }
  /* Where each segment starts along the rest line, and how stretched it is now. */
  std::vector<float> start(edges + 1, 0.0f), stretch(edges, 1.0f);
  for (int e = 0; e < edges; e++) {
    start[e + 1] = start[e] + rest_table[e].length;
    if (rest_table[e].length > 1e-9f) {
      stretch[e] = shown_table[e].length / rest_table[e].length;
    }
  }
  const float total = start[edges];
  if (total <= 0.0f) {
    return;
  }
  /* The segment at `s` along the rest line, and how far along it (0 to 1). */
  const auto locate = [&](float s, int *r_edge) {
    int e = int(std::upper_bound(start.begin(), start.end(), s) - start.begin()) - 1;
    e = std::clamp(e, 0, edges - 1);
    while (e > 0 && rest_table[e].length <= 1e-9f) {
      e--;
    }
    *r_edge = e;
    return rest_table[e].length > 1e-9f ? std::clamp((s - start[e]) / rest_table[e].length, 0.0f, 1.0f) : 0.0f;
  };
  const auto rest_half = [&](float s) {
    int e;
    const float u = locate(s, &e);
    return rest.edges[e].half0 + (rest.edges[e].half1 - rest.edges[e].half0) * u;
  };
  const auto stretch_at = [&](float s) {
    int e;
    locate(s, &e);
    return stretch[e];
  };

  float h0, s0, v0;
  rgb_to_hsv(color.r, color.g, color.b, h0, s0, v0);
  /* A section switched off keeps its values but doesn't apply. */
  const bool shape_dyn = brush.shape_dynamics, scatter_on = brush.scattering, transfer = brush.transfer;
  const bool drift = brush.color_dynamics && (brush.hue_drift != 0.0f || brush.sat_drift != 0.0f || brush.val_drift != 0.0f);
  const float spacing = std::max(brush.spacing, 0.01f);
  const float wave = std::max(brush.size_variation_scale, 0.001f) * FRAME_HEIGHT;
  /* How many places along the line (known once they're laid out), for the fades at its ends. */
  int places = 0;
  /* 0 to 1 over the first `in` and the last `out` places. */
  const auto fade = [&](int k, int in, int out) {
    float f = 1.0f;
    if (in > 0) {
      f = std::min(f, float(k + 1) / float(in));
    }
    if (out > 0) {
      f = std::min(f, float(places - k) / float(out));
    }
    return std::clamp(f, 0.0f, 1.0f);
  };

  /* End Lines at Edge: whether the dab at (edge e, u) is cut away. With Flat its middle decides
   * (the mask then cuts it straight). With Ends: Brush its middle must stay `stop` short of the
   * edge along the line: a half width at Reach 0, so the whole dab fits and just touches the edge;
   * less as Reach grows, until (past 0.5) the dab may go over by as much. */
  const auto cut_away = [&](int e, float u) {
    float2 dir;
    const float2 p = edge_point(shown_table[size_t(e)], u, &dir);
    if (!cut->taper) {
      return !cut->shows(p);
    }
    const BrushPath::Edge &edge = shown.edges[size_t(e)];
    const float half = std::max(edge.half0 + (edge.half1 - edge.half0) * u, min_half);
    const float stop = half * (1.0f - 2.0f * cut->reach);
    if (stop >= 0.0f) {
      return !cut->shows(p) || !cut->shows(p + dir * stop) || !cut->shows(p - dir * stop);
    }
    /* Reaching past the edge: kept while the edge is within that much of its middle. */
    return !cut->shows(p) && !cut->shows(p + dir * -stop) && !cut->shows(p - dir * -stop);
  };
  /* End Lines at Edge: for each place, how many places since the line was last cut away before
   * it and until it is next (a large number when it isn't), for the cut ends' taper and fade. */
  std::vector<int> since_cut, until_cut;
  const auto cut_fade = [&](int k, int in, int out) {
    if (!cut || !cut->taper || k < 0 || k >= int(since_cut.size())) {
      return 1.0f;
    }
    float f = 1.0f;
    if (in > 0) {
      f = std::min(f, float(since_cut[size_t(k)]) / float(in));
    }
    if (out > 0) {
      f = std::min(f, float(until_cut[size_t(k)]) / float(out));
    }
    return std::clamp(f, 0.0f, 1.0f);
  };

  /* One stamp: its middle `p`, the way the line runs there, its size and turn, and `id` for its own
   * random values. */
  const auto stamp = [&](float2 p, float size, float angle, float squash, int image, Color c, float shows) {
    if (size <= 0.0f || c.a <= 0.0f || r_dabs.count >= MAX_DABS) {
      return;
    }
    /* The image's longer side spans the line's width; Roundness squashes it across. */
    const float aspect = aspects[image] > 0.0f ? aspects[image] : 1.0f;
    const float hx = aspect >= 1.0f ? size * 0.5f : size * 0.5f * aspect;
    const float hy = (aspect >= 1.0f ? size * 0.5f / aspect : size * 0.5f) * squash;
    const float ca = std::cos(angle), sa = std::sin(angle);
    const float2 ax(ca * hx, sa * hx), ay(-sa * hy, ca * hy);
    const float u0 = brush.flip_x ? 1.0f : 0.0f, u1 = 1.0f - u0, v0 = brush.flip_y ? 1.0f : 0.0f, v1 = 1.0f - v0;
    const BrushVertex tl{p.x - ax.x + ay.x, p.y - ax.y + ay.y, u0, v0, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
    const BrushVertex tr{p.x + ax.x + ay.x, p.y + ax.y + ay.y, u1, v0, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
    const BrushVertex br{p.x + ax.x - ay.x, p.y + ax.y - ay.y, u1, v1, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
    const BrushVertex bl{p.x - ax.x - ay.x, p.y - ax.y - ay.y, u0, v1, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
    std::vector<BrushVertex> &out = r_dabs.per_image[image];
    out.insert(out.end(), {tl, tr, br, tl, br, bl});
    r_dabs.centers.push_back(p);
    r_dabs.alphas.push_back(shows);
    r_dabs.count++;
  };

  /* One dab, `s` along the rest line. `id` picks its random values (each dab its own, kept as
   * the line moves), `order` its image in turn and its place for the fades, `alpha` how much of it
   * shows. Everything that varies along the line goes by the rest line, so it stays put on the
   * artwork. With Scattering, a dab is several stamps, pushed off the line. */
  const auto dab = [&](float s, uint32_t id, int order, float alpha) {
    if (r_dabs.count >= MAX_DABS || alpha <= 0.0f) {
      return;
    }
    int e;
    const float u = locate(s, &e);
    const BrushPath::Edge &edge = shown.edges[e];
    if (!edge.visible) {
      return;
    }
    float2 dir;
    const float2 p = edge_point(shown_table[e], u, &dir);
    if (cut && cut->shows && cut_away(e, u)) {
      return;
    }
    const float true_half = edge.half0 + (edge.half1 - edge.half0) * u;
    const float half = std::max(true_half, min_half);
    /* Thinner than the smallest dab: fainter rather than wider, so its weight stays true. The dabs
     * keep the true width's spacing but are drawn wider, so about 1 / (spacing x thinness) of them
     * overlap at any spot: each shows just enough that together they come to the thinness. */
    float thin = 1.0f;
    if (fade_thin && true_half < min_half) {
      const float t = std::clamp(true_half / min_half, 0.0f, 1.0f);
      const float overlap = std::max(0.5f / (spacing * std::max(t, 1e-3f)), 1.0f);
      thin = 1.0f - std::pow(1.0f - t, 1.0f / overlap);
    }
    const float here = rest_half(s);
    const float width = 2.0f * half;

    float base_size = width;
    float base_angle = brush.tip_angle;
    if (shape_dyn) {
      if (brush.size_variation_amp > 0.0f) {
        base_size *= std::max(0.0f, 1.0f + brush.size_variation_amp * smooth_noise(seed ^ 0xa511e9b3U, s / wave));
      }
      base_size *= fade(order, brush.size_fade_in, brush.size_fade_out) *
                   cut_fade(order, brush.size_fade_in, brush.size_fade_out);
      /* Upright, or the image's top along the line. */
      if (brush.align) {
        base_angle += std::atan2(dir.y, dir.x) - PI * 0.5f;
      }
      if (brush.angle_drift != 0.0f) {
        base_angle += brush.angle_drift * smooth_noise(seed ^ 0x63d83595U, s / std::max(40.0f * here, 1e-6f));
      }
    }
    const float base_shows = std::clamp(alpha, 0.0f, 1.0f) * thin *
                             (transfer ? fade(order, brush.opacity_fade_in, brush.opacity_fade_out) *
                                             cut_fade(order, brush.opacity_fade_in, brush.opacity_fade_out)
                                       : 1.0f);
    int copies = 1;
    if (scatter_on && brush.count > 1) {
      copies = brush.count;
      if (brush.count_jitter > 0.0f) {
        copies = std::max(1, int(std::lround(float(copies) * (1.0f - brush.count_jitter * random01(seed ^ 0x2f8a1b7dU, id)))));
      }
    }
    for (int copy = 0; copy < copies; copy++) {
      /* The first stamp keeps the dab's own random values (as before Scattering); the others get
       * their own. */
      const uint32_t cid = copy == 0 ? id : hash(id * 31u + uint32_t(copy) * 0x9e3779b9U);
      float2 at = p;
      if (scatter_on && brush.scatter > 0.0f) {
        const float2 across(-dir.y, dir.x);
        at = at + across * ((random01(seed ^ 0x5bd1e995U, cid) * 2.0f - 1.0f) * brush.scatter * width);
        if (brush.scatter_both) {
          at = at + dir * ((random01(seed ^ 0x68e31da4U, cid) * 2.0f - 1.0f) * brush.scatter * width);
        }
      }
      float size = base_size, angle = base_angle, squash = std::clamp(brush.roundness, 0.01f, 1.0f);
      if (shape_dyn) {
        if (brush.size_jitter > 0.0f) {
          size *= std::max(brush.size_min, 1.0f - brush.size_jitter * random01(seed ^ 0x7a2c4e11U, cid));
        }
        angle += (random01(seed, cid * 3 + 0) - 0.5f) * brush.jitter;
        if (brush.roundness_jitter > 0.0f) {
          squash *= std::max(0.01f, 1.0f - brush.roundness_jitter * random01(seed ^ 0x3c6ef372U, cid));
        }
      }
      const int image = brush.random_order ? int(random01(seed, cid * 3 + 1) * float(images)) % images
                                           : order % images;
      Color c = color;
      if (drift) {
        const float r1 = random01(seed, cid * 3 + 2), r2 = random01(seed ^ 0x1b873593U, cid),
                    r3 = random01(seed ^ 0xcc9e2d51U, cid);
        hsv_to_rgb(h0 + (r1 - 0.5f) * 2.0f * brush.hue_drift,
                   std::clamp(s0 + (r2 - 0.5f) * 2.0f * brush.sat_drift, 0.0f, 1.0f),
                   std::clamp(v0 + (r3 - 0.5f) * 2.0f * brush.val_drift, 0.0f, 1.0f), c.r, c.g, c.b);
      }
      float shows = base_shows;
      if (transfer && brush.opacity_jitter > 0.0f) {
        shows *= 1.0f - brush.opacity_jitter * random01(seed ^ 0x85ebca6bU, cid);
      }
      c.a *= shows;
      stamp(at, size, angle, squash, image, c, shows);
    }
  };

  /* The dabs' places along the rest line, at the brush's spacing (of the rest width). A closed
   * line stops a dab short of where it began, so the seam doesn't double up. */
  std::vector<float> marks;
  for (float s = 0.0f; s <= total && int(marks.size()) < MAX_DABS;) {
    marks.push_back(s);
    const float step = std::max(spacing * 2.0f * rest_half(s), MIN_STEP);
    s += step;
    if (rest.closed && s > total - step * 0.5f) {
      break;
    }
  }
  /* How much of a stamp coming in or going out shows: as it fades, or (Pop In) all or nothing,
   * switching halfway. */
  const auto coming = [&](float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return brush.pop_in ? (a >= 0.5f ? 1.0f : 0.0f) : a;
  };
  /* Between two dabs on a stretched segment: one halfway, fading in from 1.5 to 2 times its rest
   * length, then halfway again from 3 to 4 times, and so on. `code` numbers it within the gap
   * (1, then 2 and 3, ...) for its own random values. */
  const auto fill = [&](auto &&self, float a, float b, int level, uint32_t code, int k) -> void {
    const float mid = (a + b) * 0.5f;
    const float hi = float(1 << level), lo = hi * 0.75f;
    const float r = stretch_at(mid);
    if (r <= lo) {
      return;
    }
    dab(mid, uint32_t(k) * 32u + code, k, coming((r - lo) / (hi - lo)));
    if (level < MAX_FILL_LEVEL) {
      self(self, a, mid, level + 1, code * 2, k);
      self(self, mid, b, level + 1, code * 2 + 1, k);
    }
  };
  const int count = int(marks.size());
  places = count;
  if (cut && cut->shows) {
    /* Which places the cut takes away, and from that how far each is from a cut on either side
     * (a closed line wraps round). */
    std::vector<char> gone(size_t(count), 0);
    for (int k = 0; k < count; k++) {
      int e;
      const float u = locate(marks[size_t(k)], &e);
      gone[size_t(k)] = shown.edges[size_t(e)].visible && cut_away(e, u);
    }
    constexpr int FAR = 1 << 20;
    since_cut.assign(size_t(count), FAR);
    until_cut.assign(size_t(count), FAR);
    const int laps = rest.closed ? 2 : 1;
    for (int pass = 0, last = -FAR; pass < laps * count; pass++) {
      const int k = pass % count;
      if (gone[size_t(k)]) {
        last = pass;
      }
      else if (last > -FAR) {
        since_cut[size_t(k)] = std::min(since_cut[size_t(k)], pass - last);
      }
    }
    for (int pass = laps * count - 1, next = FAR; pass >= 0; pass--) {
      const int k = pass % count;
      if (gone[size_t(k)]) {
        next = pass;
      }
      else if (next < FAR) {
        until_cut[size_t(k)] = std::min(until_cut[size_t(k)], next - pass);
      }
    }
  }
  for (int k = 0; k < count; k++) {
    /* On a squashed segment, every other dab fades out from 3/4 to 1/2 its rest length, every
     * fourth from 3/8 to 1/4, ... (the first always stays). */
    float alpha = 1.0f;
    if (k > 0) {
      int m = 1;
      for (int bits = k; (bits & 1) == 0 && m < MAX_THIN_LEVEL; bits >>= 1) {
        m++;
      }
      const float lo = std::ldexp(1.0f, -m), hi = lo * 1.5f;
      alpha = coming((stretch_at(marks[k]) - lo) / (hi - lo));
    }
    dab(marks[k], uint32_t(k) * 32u, k, alpha);
    const bool last = k + 1 >= count;
    if (!last || rest.closed) {
      fill(fill, marks[k], last ? total : marks[k + 1], 1, 1u, k);
    }
  }
}

}  // namespace oanim
