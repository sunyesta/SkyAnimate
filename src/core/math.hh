#pragma once

#include <cstdint>

/* Small 2D math types used by the data model and the renderer. */

#include <cmath>

namespace oanim {

struct float2 {
  float x = 0.0f;
  float y = 0.0f;

  constexpr float2() = default;
  constexpr float2(float x, float y) : x(x), y(y) {}

  constexpr float2 operator+(float2 b) const { return {x + b.x, y + b.y}; }
  constexpr float2 operator-(float2 b) const { return {x - b.x, y - b.y}; }
  constexpr float2 operator-() const { return {-x, -y}; }
  constexpr float2 operator*(float s) const { return {x * s, y * s}; }
  constexpr float2 operator/(float s) const { return {x / s, y / s}; }
  float2 &operator+=(float2 b) { x += b.x; y += b.y; return *this; }
  float2 &operator-=(float2 b) { x -= b.x; y -= b.y; return *this; }
  float2 &operator*=(float s) { x *= s; y *= s; return *this; }
  constexpr bool operator==(const float2 &b) const = default;
};

inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
/* z of the 3D cross product: > 0 when b turns counter-clockwise from a. */
inline float cross(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }
inline float length(float2 v) { return std::sqrt(dot(v, v)); }
inline float distance(float2 a, float2 b) { return length(a - b); }
inline float2 normalize(float2 v)
{
  const float len = length(v);
  return len > 1e-12f ? v / len : float2(0.0f, 0.0f);
}
/* Rotates counter-clockwise by `angle` radians. */
inline float2 rotate(float2 v, float angle)
{
  const float c = std::cos(angle), s = std::sin(angle);
  return {v.x * c - v.y * s, v.x * s + v.y * c};
}
inline float2 perpendicular(float2 v) { return {-v.y, v.x}; }
inline float2 lerp(float2 a, float2 b, float t) { return a + (b - a) * t; }

/* Distance from p to the segment ab, and the parameter of the closest point. */
inline float distance_to_segment(float2 p, float2 a, float2 b, float *r_t = nullptr)
{
  const float2 ab = b - a;
  const float len2 = dot(ab, ab);
  float t = len2 > 0.0f ? dot(p - a, ab) / len2 : 0.0f;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  if (r_t) {
    *r_t = t;
  }
  return distance(p, a + ab * t);
}

/* Linear RGBA colour, components 0-1, not premultiplied. */
struct Color {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
  constexpr bool operator==(const Color &other) const = default;
};

/* A 2D affine transform: p' = (a*x + c*y + tx, b*x + d*y + ty). */
struct Transform2D {
  float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;

  float2 apply(float2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
  /* Applies the linear part only, for directions. */
  float2 apply_vector(float2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
  Transform2D operator*(const Transform2D &o) const
  {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
            a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
  }
  Transform2D inverted() const
  {
    const float det = a * d - b * c;
    const float inv = std::fabs(det) > 1e-20f ? 1.0f / det : 0.0f;
    const Transform2D r{d * inv, -b * inv, -c * inv, a * inv, 0.0f, 0.0f};
    const float2 t = r.apply_vector({tx, ty});
    return {r.a, r.b, r.c, r.d, -t.x, -t.y};
  }
  /* Average scale, for converting pixel sizes into this space. */
  float scale() const { return std::sqrt(std::fabs(a * d - b * c)); }

  static Transform2D translate(float2 t) { return {1, 0, 0, 1, t.x, t.y}; }
  static Transform2D rotate(float angle)
  {
    const float c = std::cos(angle), s = std::sin(angle);
    return {c, s, -s, c, 0, 0};
  }
  static Transform2D scaling(float2 s) { return {s.x, 0, 0, s.y, 0, 0}; }
};

inline constexpr float PI = 3.14159265358979323846f;

/* How a stroke turns at an outline sample: Smooth along a curve, Round or Sharp at the curve's own
 * points (their corner type). At a stroke's end, Round draws a round cap and Sharp a flat one. */
enum class StrokeJoin : uint8_t { Smooth, Round, Sharp };

}  // namespace oanim
