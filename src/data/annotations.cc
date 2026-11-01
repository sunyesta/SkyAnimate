#include "data/annotations.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "data/document.hh"

namespace oanim {

static constexpr const char *END_IDS[] = {"NONE", "ARROW", "ARROW_OPEN", "SEGMENT", "DOT"};
static constexpr const char *END_NAMES[] = {"None", "Arrow", "Open Arrow", "Segment", "Dot"};

const char *annotation_end_identifier(AnnotationEnd end)
{
  return END_IDS[int(end)];
}

const char *annotation_end_name(AnnotationEnd end)
{
  return END_NAMES[int(end)];
}

bool annotation_end_from_identifier(const std::string &identifier, AnnotationEnd *r_end)
{
  for (int i = 0; i < ANNOTATION_END_COUNT; i++) {
    if (identifier == END_IDS[i]) {
      *r_end = AnnotationEnd(i);
      return true;
    }
  }
  return false;
}

static constexpr const char *GUIDE_IDS[] = {"LINE", "PERSPECTIVE", "GRID"};
static constexpr const char *GUIDE_NAMES[] = {"Line", "Perspective", "Grid"};

const char *guide_type_identifier(GuideType type)
{
  return GUIDE_IDS[int(type)];
}

const char *guide_type_name(GuideType type)
{
  return GUIDE_NAMES[int(type)];
}

bool guide_type_from_identifier(const std::string &identifier, GuideType *r_type)
{
  for (int i = 0; i < 3; i++) {
    if (identifier == GUIDE_IDS[i]) {
      *r_type = GuideType(i);
      return true;
    }
  }
  return false;
}

AnnotationLayer *annotation_layer_active(Document &doc)
{
  if (doc.annotations.empty()) {
    return nullptr;
  }
  doc.active_annotation = std::clamp(doc.active_annotation, 0, int(doc.annotations.size()) - 1);
  return &doc.annotations[size_t(doc.active_annotation)];
}

AnnotationLayer &annotation_layer_ensure(Document &doc)
{
  if (doc.annotations.empty()) {
    doc.annotations.push_back(AnnotationLayer());
    doc.active_annotation = 0;
  }
  return *annotation_layer_active(doc);
}

std::string annotation_layer_unique_name(const Document &doc, const std::string &base)
{
  const auto taken = [&](const std::string &name) {
    return std::any_of(doc.annotations.begin(), doc.annotations.end(),
                       [&](const AnnotationLayer &l) { return l.name == name; });
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

/* Where along a→b (0 to 1) the segment is inside the circle: false when it isn't. */
static bool segment_inside(float2 a, float2 b, float2 center, float radius, float *r_t0, float *r_t1)
{
  const float2 d = b - a, f = a - center;
  const float qa = dot(d, d), qb = 2.0f * dot(f, d), qc = dot(f, f) - radius * radius;
  if (qa < 1e-20f) {
    *r_t0 = 0.0f;
    *r_t1 = 1.0f;
    return qc < 0.0f;
  }
  const float disc = qb * qb - 4.0f * qa * qc;
  if (disc <= 0.0f) {
    return false;
  }
  const float s = std::sqrt(disc);
  const float t0 = std::max((-qb - s) / (2.0f * qa), 0.0f), t1 = std::min((-qb + s) / (2.0f * qa), 1.0f);
  if (t0 >= t1) {
    return false;
  }
  *r_t0 = t0;
  *r_t1 = t1;
  return true;
}

bool annotation_erase(Document &doc, float2 center, float radius)
{
  bool changed = false;
  for (AnnotationLayer &layer : doc.annotations) {
    if (layer.hidden || layer.locked) {
      continue;
    }
    std::vector<AnnotationStroke> kept;
    for (const AnnotationStroke &stroke : layer.strokes) {
      const std::vector<float2> &pts = stroke.points;
      const auto inside = [&](float2 p) { return length(p - center) < radius; };
      if (pts.size() < 2) {
        if (!pts.empty() && !inside(pts[0])) {
          kept.push_back(stroke);
        }
        else {
          changed = true;
        }
        continue;
      }
      std::vector<AnnotationStroke> pieces;
      AnnotationStroke cur;
      bool cur_from_start = false;
      const auto finish = [&](bool reaches_end) {
        if (cur.points.size() >= 2) {
          cur.start = cur_from_start ? stroke.start : AnnotationEnd::None;
          cur.end = reaches_end ? stroke.end : AnnotationEnd::None;
          pieces.push_back(std::move(cur));
        }
        cur = AnnotationStroke();
        cur_from_start = false;
      };
      for (size_t k = 0; k + 1 < pts.size(); k++) {
        const float2 a = pts[k], b = pts[k + 1];
        if (cur.points.empty() && !inside(a)) {
          cur.points.push_back(a);
          cur_from_start = k == 0;
        }
        float t0 = 0.0f, t1 = 0.0f;
        if (!segment_inside(a, b, center, radius, &t0, &t1)) {
          cur.points.push_back(b);
          continue;
        }
        if (t0 > 0.0f) {
          cur.points.push_back(a + (b - a) * t0);
        }
        finish(false);
        if (t1 < 1.0f) {
          cur.points = {a + (b - a) * t1, b};
        }
      }
      finish(true);
      if (pieces.size() == 1 && pieces[0] == stroke) {
        kept.push_back(stroke);
        continue;
      }
      changed = true;
      for (AnnotationStroke &p : pieces) {
        kept.push_back(std::move(p));
      }
    }
    layer.strokes = std::move(kept);
  }
  return changed;
}

int guide_point_count(GuideType type, int count)
{
  return type == GuideType::Perspective ? std::clamp(count, 1, 3) : 2;
}

void guide_validate(DrawingGuide &guide)
{
  const int want = guide_point_count(guide.type, int(guide.points.size()));
  if (guide.points.empty()) {
    guide.points.push_back({0.0f, 0.0f});
  }
  while (int(guide.points.size()) < want) {
    guide.points.push_back(guide.points.back() + float2(1.0f, 0.0f));
  }
  guide.points.resize(size_t(want));
  if (guide.type != GuideType::Perspective && length(guide.points[1] - guide.points[0]) < 1e-5f) {
    guide.points[1] = guide.points[0] + float2(guide.type == GuideType::Grid ? 0.1f : 1.0f, 0.0f);
  }
  guide.lines = std::clamp(guide.lines, GUIDE_LINES_MIN, GUIDE_LINES_MAX);
}

/* The endless line through `p` along `dir` (a unit vector) across the box. */
static void line_across(float2 p, float2 dir, float2 lo, float2 hi, std::vector<std::pair<float2, float2>> &r)
{
  const float2 mid = (lo + hi) * 0.5f;
  const float reach = length(p - mid) + length(hi - lo);
  r.push_back({p - dir * reach, p + dir * reach});
}

void guide_segments(const DrawingGuide &guide, float2 lo, float2 hi, float min_cell,
                    std::vector<std::pair<float2, float2>> &r_segments)
{
  const std::vector<float2> &p = guide.points;
  switch (guide.type) {
    case GuideType::Line: {
      if (p.size() >= 2) {
        r_segments.push_back({p[0], p[1]});
      }
      return;
    }
    case GuideType::Perspective: {
      if (p.empty()) {
        return;
      }
      /* The horizon, and the fans turned so one of their lines lies along it. */
      const float2 along = p.size() >= 2 && length(p[1] - p[0]) > 1e-9f ? normalize(p[1] - p[0]) : float2(1.0f, 0.0f);
      line_across(p[0], along, lo, hi, r_segments);
      const float base = std::atan2(along.y, along.x);
      const int lines = std::clamp(guide.lines, GUIDE_LINES_MIN, GUIDE_LINES_MAX);
      for (const float2 &vp : p) {
        float reach = 0.0f;
        for (const float2 corner : {lo, hi, float2(lo.x, hi.y), float2(hi.x, lo.y)}) {
          reach = std::max(reach, length(corner - vp));
        }
        for (int k = 0; k < lines; k++) {
          const float angle = base + 2.0f * PI * float(k) / float(lines);
          r_segments.push_back({vp, vp + float2(std::cos(angle), std::sin(angle)) * reach});
        }
      }
      return;
    }
    case GuideType::Grid: {
      if (p.size() < 2) {
        return;
      }
      const float2 o = p[0], u = p[1] - p[0];
      const float cell = length(u);
      if (cell < std::max(min_cell, 1e-9f)) {
        return;
      }
      const float2 v(-u.y, u.x);
      float s0 = 1e30f, s1 = -1e30f, t0 = 1e30f, t1 = -1e30f;
      for (const float2 corner : {lo, hi, float2(lo.x, hi.y), float2(hi.x, lo.y)}) {
        const float s = dot(corner - o, u) / (cell * cell), t = dot(corner - o, v) / (cell * cell);
        s0 = std::min(s0, s);
        s1 = std::max(s1, s);
        t0 = std::min(t0, t);
        t1 = std::max(t1, t);
      }
      const int i0 = int(std::floor(s0)), i1 = int(std::ceil(s1)), j0 = int(std::floor(t0)), j1 = int(std::ceil(t1));
      if ((i1 - i0) + (j1 - j0) > 4000) {
        return;
      }
      for (int i = i0; i <= i1; i++) {
        r_segments.push_back({o + u * float(i) + v * t0, o + u * float(i) + v * t1});
      }
      for (int j = j0; j <= j1; j++) {
        r_segments.push_back({o + v * float(j) + u * s0, o + v * float(j) + u * s1});
      }
      return;
    }
  }
}

float guide_distance(const DrawingGuide &guide, float2 p, float2 lo, float2 hi, float min_cell)
{
  std::vector<std::pair<float2, float2>> segments;
  guide_segments(guide, lo, hi, min_cell, segments);
  float best = 1e30f;
  for (const auto &[a, b] : segments) {
    best = std::min(best, distance_to_segment(p, a, b));
  }
  return best;
}

/* The nearest of `angle`, in 45 degree steps, the line's `original` angle and that angle's opposite. */
static float guide_best_snap(float angle, float original)
{
  const auto off_by = [&](float to) {
    return std::abs(std::atan2(std::sin(angle - to), std::cos(angle - to)));
  };
  const float step = PI / 4.0f;
  const float snapped = std::round(angle / step) * step;
  const float to_snapped = off_by(snapped), to_original = off_by(original), to_opposite = off_by(original + PI);
  if (to_original < to_snapped && to_original < to_opposite) {
    return original;
  }
  return to_opposite < to_snapped ? original + PI : snapped;
}

std::vector<float2> guide_line_drag(GuideGrab grab, int end, const std::vector<float2> &start, float2 press,
                                    float2 mouse, bool constrain)
{
  std::vector<float2> points = start;
  if (start.size() < 2) {
    return points;
  }
  end = end == 0 ? 0 : 1;
  const auto along = [](float angle, float distance) {
    return float2(std::cos(angle), std::sin(angle)) * distance;
  };
  const auto angle_of = [](float2 v) { return std::atan2(v.y, v.x); };
  const float2 pivot = start[size_t(1 - end)];
  switch (grab) {
    case GuideGrab::None:
      break;
    case GuideGrab::Draw:
    case GuideGrab::End: {
      float2 v = mouse - pivot;
      if (constrain) {
        const float step = PI / 4.0f;
        const float angle = grab == GuideGrab::Draw ? std::round(angle_of(v) / step) * step
                                                    : guide_best_snap(angle_of(v), angle_of(start[size_t(end)] - pivot));
        v = along(angle, length(v));
      }
      points[size_t(end)] = pivot + v;
      break;
    }
    case GuideGrab::RotateEnd: {
      const float was = angle_of(start[size_t(end)] - pivot);
      float angle = angle_of(mouse - pivot);
      if (constrain) {
        angle = guide_best_snap(angle, was);
      }
      points[size_t(end)] = pivot + along(angle, length(start[size_t(end)] - pivot));
      break;
    }
    case GuideGrab::RotateCenter: {
      const float2 center = (start[0] + start[1]) * 0.5f;
      const float was = angle_of(start[1] - start[0]);
      float angle = was + angle_of(mouse - center) - angle_of(press - center);
      if (constrain) {
        angle = guide_best_snap(angle, was);
      }
      const float2 half = along(angle, length(start[1] - start[0]) * 0.5f);
      points[0] = center - half;
      points[1] = center + half;
      break;
    }
    case GuideGrab::Move: {
      float2 delta = mouse - press;
      if (constrain) {
        const float step = PI / 2.0f;
        delta = along(std::round(angle_of(delta) / step) * step, length(delta));
      }
      points[0] = start[0] + delta;
      points[1] = start[1] + delta;
      break;
    }
  }
  return points;
}

}  // namespace oanim
