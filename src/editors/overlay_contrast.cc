#include "editors/overlay_contrast.hh"

#include <algorithm>
#include <cmath>

#include "assets/asset_library.hh"

namespace oanim {

/* How far apart two colours look, 0-1: mostly by brightness, which is what makes a thin line stand out; a different
 * hue at the same brightness helps a little. */
static float contrast(const Color &a, const Color &b)
{
  const auto luma = [](const Color &c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; };
  const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  const float apart = std::sqrt((dr * dr + dg * dg + db * db) / 3.0f);
  return std::max(std::fabs(luma(a) - luma(b)), apart * 0.6f);
}

/* Hard to see below ENTER; an alternative already taken stays until LEAVE. */
constexpr float ENTER = 0.12f;
constexpr float LEAVE = 0.17f;

ContrastPick contrast_pick(const Color &under, const Color &base, ContrastPick previous)
{
  const float c = contrast(under, base);
  if (c >= (previous == ContrastPick::Base ? ENTER : LEAVE)) {
    return ContrastPick::Base;
  }
  const float under_luma = 0.299f * under.r + 0.587f * under.g + 0.114f * under.b;
  const float base_luma = 0.299f * base.r + 0.587f * base.g + 0.114f * base.b;
  return under_luma <= base_luma ? ContrastPick::Lighter : ContrastPick::Darker;
}

bool CanvasSample::at(float2 p, Color &r) const
{
  if (rgba.empty()) {
    return false;
  }
  const float2 px = document_to_pixels.apply(p);
  const int x = int(std::floor(px.x)), y = height - 1 - int(std::floor(px.y));
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return false;
  }
  const uint8_t *q = rgba.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
  const float alpha = q[3] / 255.0f;
  r = {q[0] / 255.0f + background.r * (1.0f - alpha), q[1] / 255.0f + background.g * (1.0f - alpha),
       q[2] / 255.0f + background.b * (1.0f - alpha), 1.0f};
  return true;
}

void contrast_pieces(const std::vector<float2> &points, float piece,
                     const std::function<bool(float2, Color &)> &under, const Color &base,
                     std::vector<ContrastPiece> &out)
{
  ContrastPick previous = ContrastPick::Base;
  for (size_t i = 0; i + 1 < points.size(); i++) {
    const float2 a = points[i], b = points[i + 1];
    const float len = length(b - a);
    if (len <= 0.0f) {
      continue; /* Standing still: nothing to draw. */
    }
    const int n = std::clamp(int(std::ceil(len / std::max(piece, 1e-9f))), 1, 4096);
    for (int k = 0; k < n; k++) {
      const float2 from = a + (b - a) * (float(k) / float(n));
      const float2 to = a + (b - a) * (float(k + 1) / float(n));
      Color c;
      const ContrastPick pick = under((from + to) * 0.5f, c) ? contrast_pick(c, base, previous) : ContrastPick::Base;
      /* Joined to the piece before on the same straight segment when they take the same colour: fewer lines. */
      if (k > 0 && !out.empty() && out.back().pick == pick && out.back().b.x == from.x && out.back().b.y == from.y) {
        out.back().b = to;
      }
      else {
        out.push_back({from, to, pick});
      }
      previous = pick;
    }
  }
}

Color color_from_rgb(uint32_t rgb)
{
  return {float((rgb >> 16) & 0xff) / 255.0f, float((rgb >> 8) & 0xff) / 255.0f, float(rgb & 0xff) / 255.0f, 1.0f};
}

ContrastColors wire_colors()
{
  const Preferences &p = preferences();
  return {color_from_rgb(p.wire_color), color_from_rgb(p.wire_lighter), color_from_rgb(p.wire_darker)};
}

ContrastColors path_colors()
{
  const Preferences &p = preferences();
  return {color_from_rgb(p.path_color), color_from_rgb(p.path_lighter), color_from_rgb(p.path_darker)};
}

ContrastColors contrast_colors_of(const Color &base)
{
  const auto mix = [&](float to, float t) {
    return Color{base.r + (to - base.r) * t, base.g + (to - base.g) * t, base.b + (to - base.b) * t, base.a};
  };
  return {base, mix(1.0f, 0.3f), mix(0.0f, 0.3f)};
}

}  // namespace oanim
