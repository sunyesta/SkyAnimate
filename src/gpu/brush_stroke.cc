#include "gpu/brush_stroke.hh"

#include <algorithm>
#include <cmath>

namespace sky {

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

void brush_stroke(const std::vector<float2> &points, const std::vector<float> &half_widths, bool closed,
                  const BrushSettings &brush, Color color, const std::vector<float> &aspects, float min_step,
                  uint32_t seed, BrushDabs &r_dabs)
{
  const int n = int(points.size());
  const int images = int(aspects.size());
  if (n < 2 || images == 0) {
    return;
  }
  if (int(r_dabs.per_image.size()) < images) {
    r_dabs.per_image.resize(images);
  }
  /* Arc length at each point (and back to the start when closed). */
  const int pieces = closed ? n : n - 1;
  std::vector<float> at(pieces + 1, 0.0f);
  for (int i = 0; i < pieces; i++) {
    at[i + 1] = at[i] + distance(points[i], points[(i + 1) % n]);
  }
  const float length = at[pieces];
  if (length <= 0.0f) {
    return;
  }

  float h0, s0, v0;
  rgb_to_hsv(color.r, color.g, color.b, h0, s0, v0);
  const bool drift = brush.hue_drift != 0.0f || brush.sat_drift != 0.0f || brush.val_drift != 0.0f;
  const float spacing = std::max(brush.spacing, 0.01f);
  const float wave = std::max(brush.size_variation_scale, 0.001f) * FRAME_HEIGHT;

  int piece = 0;
  int k = 0;
  /* A closed line stops a dab short of where it began, so the seam doesn't double up. */
  for (float s = 0.0f; s <= length && k < MAX_DABS; k++) {
    while (piece < pieces - 1 && at[piece + 1] < s) {
      piece++;
    }
    const int i = piece, j = (piece + 1) % n;
    const float seg = at[piece + 1] - at[piece];
    const float t = seg > 0.0f ? std::clamp((s - at[piece]) / seg, 0.0f, 1.0f) : 0.0f;
    const float2 p = points[i] + (points[j] - points[i]) * t;
    const float2 dir = seg > 0.0f ? (points[j] - points[i]) / seg : float2(1.0f, 0.0f);
    const float half = half_widths[i] + (half_widths[j] - half_widths[i]) * t;

    float size = 2.0f * half;
    if (brush.size_variation_amp > 0.0f) {
      size *= std::max(0.0f, 1.0f + brush.size_variation_amp * smooth_noise(seed ^ 0xa511e9b3U, s / wave));
    }
    const float step = std::max(spacing * 2.0f * half, min_step);

    /* Upright, or the image's top along the line. */
    float angle = brush.align ? std::atan2(dir.y, dir.x) - PI * 0.5f : 0.0f;
    angle += (random01(seed, uint32_t(k) * 3 + 0) - 0.5f) * brush.jitter;
    if (brush.angle_drift != 0.0f) {
      angle += brush.angle_drift * smooth_noise(seed ^ 0x63d83595U, s / std::max(40.0f * half, 1e-6f));
    }

    const int image = brush.random_order ? int(random01(seed, uint32_t(k) * 3 + 1) * float(images)) % images
                                         : k % images;
    Color c = color;
    if (drift) {
      const float r1 = random01(seed, uint32_t(k) * 3 + 2), r2 = random01(seed ^ 0x1b873593U, uint32_t(k)),
                  r3 = random01(seed ^ 0xcc9e2d51U, uint32_t(k));
      hsv_to_rgb(h0 + (r1 - 0.5f) * 2.0f * brush.hue_drift,
                 std::clamp(s0 + (r2 - 0.5f) * 2.0f * brush.sat_drift, 0.0f, 1.0f),
                 std::clamp(v0 + (r3 - 0.5f) * 2.0f * brush.val_drift, 0.0f, 1.0f), c.r, c.g, c.b);
    }

    if (size > 0.0f) {
      /* The image's longer side spans the line's width. */
      const float aspect = aspects[image] > 0.0f ? aspects[image] : 1.0f;
      const float hx = aspect >= 1.0f ? size * 0.5f : size * 0.5f * aspect;
      const float hy = aspect >= 1.0f ? size * 0.5f / aspect : size * 0.5f;
      const float ca = std::cos(angle), sa = std::sin(angle);
      const float2 ax(ca * hx, sa * hx), ay(-sa * hy, ca * hy);
      const BrushVertex tl{p.x - ax.x + ay.x, p.y - ax.y + ay.y, 0, 0, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
      const BrushVertex tr{p.x + ax.x + ay.x, p.y + ax.y + ay.y, 1, 0, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
      const BrushVertex br{p.x + ax.x - ay.x, p.y + ax.y - ay.y, 1, 1, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
      const BrushVertex bl{p.x - ax.x - ay.x, p.y - ax.y - ay.y, 0, 1, c.r * c.a, c.g * c.a, c.b * c.a, c.a};
      std::vector<BrushVertex> &out = r_dabs.per_image[image];
      out.insert(out.end(), {tl, tr, br, tl, br, bl});
      r_dabs.count++;
    }
    s += step;
    if (closed && s > length - step * 0.5f) {
      break;
    }
  }
}

}  // namespace sky
