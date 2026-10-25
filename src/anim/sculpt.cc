#include "anim/sculpt.hh"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "anim/deform.hh"
#include "data/detail.hh"
#include "data/document.hh"
#include "data/layer_mirror.hh"
#include "data/vector.hh"

namespace oanim {

static constexpr const char *BRUSH_IDS[SCULPT_BRUSH_COUNT] = {"GRAB", "PUSH", "SMOOTH", "THICKNESS", "PINCH", "TWIST"};
static constexpr const char *BRUSH_NAMES[SCULPT_BRUSH_COUNT] = {"Grab", "Push", "Smooth", "Thickness", "Pinch", "Twist"};
static constexpr const char *FALLOFF_IDS[3] = {"SMOOTH", "SPHERE", "CONSTANT"};

const char *sculpt_brush_id(SculptBrush brush)
{
  return BRUSH_IDS[std::clamp(int(brush), 0, SCULPT_BRUSH_COUNT - 1)];
}

const char *sculpt_brush_name(SculptBrush brush)
{
  return BRUSH_NAMES[std::clamp(int(brush), 0, SCULPT_BRUSH_COUNT - 1)];
}

bool sculpt_brush_from_id(const std::string &id, SculptBrush *r_brush)
{
  for (int i = 0; i < SCULPT_BRUSH_COUNT; i++) {
    if (id == BRUSH_IDS[i]) {
      *r_brush = SculptBrush(i);
      return true;
    }
  }
  return false;
}

const char *sculpt_falloff_id(SculptFalloff falloff)
{
  return FALLOFF_IDS[std::clamp(int(falloff), 0, 2)];
}

bool sculpt_falloff_from_id(const std::string &id, SculptFalloff *r_falloff)
{
  for (int i = 0; i < 3; i++) {
    if (id == FALLOFF_IDS[i]) {
      *r_falloff = SculptFalloff(i);
      return true;
    }
  }
  return false;
}

float sculpt_falloff(SculptFalloff falloff, float t)
{
  if (t >= 1.0f) {
    return 0.0f;
  }
  t = std::max(t, 0.0f);
  switch (falloff) {
    case SculptFalloff::Smooth:
      /* 1 - smoothstep. */
      return 1.0f - t * t * (3.0f - 2.0f * t);
    case SculptFalloff::Sphere:
      return std::sqrt(1.0f - t * t);
    case SculptFalloff::Constant:
      return 1.0f;
  }
  return 0.0f;
}

namespace {

bool sculptable(const Layer &layer)
{
  return layer.has_drawing() && (!layer.is_reference() || layer.reference.edit_points);
}

float weight_at(const SculptDab &dab, float2 p)
{
  return sculpt_falloff(dab.falloff, distance(p, dab.center) / std::max(dab.radius, 1e-9f));
}

/* The curve's segments: segment s runs from point s to point s + 1 (the last of a closed curve back to
 * the first). */
int segment_count(const SculptStroke::Curve &c)
{
  const int n = int(c.points.size());
  return c.closed ? n : n - 1;
}

/* The samples of segment s, from its first point's sample to its last's (both in). */
std::vector<int> segment_samples(const SculptStroke::Curve &c, int s)
{
  const int n = int(c.points.size());
  const int first = c.anchor[size_t(s)];
  const int last = c.anchor[size_t((s + 1) % n)];
  const int total = int(c.pos.size());
  std::vector<int> out;
  for (int k = first;; k = (k + 1) % total) {
    out.push_back(k);
    if (k == last && out.size() > 1) {
      break;
    }
  }
  return out;
}

float2 normalized(float2 v)
{
  const float len = length(v);
  return len > 1e-12f ? v / len : float2();
}

/* The handles (as offsets: `r_h1` from p0, `r_h2` from p3) of the cubic from p0 to p3 nearest the samples
 * `pts` (least squares). The samples were made evenly spaced in the segment's own parameter, and keep it as
 * they move: fitted at those parameters, samples nobody moved give back the curve they came from exactly
 * (guessing the parameters from distances along them bends sharply curved segments out of shape). A
 * handle given (`fix1`, `fix2`) stays as it is; `was` is the longest the segment's handles were. */
void fit_handles(const std::vector<float2> &pts, float2 p0, float2 p3, const float2 *fix1, const float2 *fix2,
                 float was, float2 *r_h1, float2 *r_h2)
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
      const float t = float(k) / float(m - 1), s = 1.0f - t;
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

}  // namespace

SculptStroke sculpt_stroke_begin(const Document &doc, const std::vector<uint64_t> &layers, const SculptDab &dab)
{
  SculptStroke stroke;
  /* A few samples across the brush, so it works on the line at its own scale. */
  const float spacing = std::max(dab.radius / 6.0f, 1e-6f);
  for (const uint64_t id : layers) {
    const Layer *layer = layer_find(doc, id);
    if (!layer || !sculptable(*layer)) {
      continue;
    }
    const VectorDrawing &drawing = layer->drawing;
    SculptStroke::Layer s;
    s.id = id;
    const Transform2D to_document = layer_world_transform(doc, id);
    std::vector<Transform2D> skins = layer_point_skins(doc, *layer);
    skins.resize(drawing.points.size());
    s.to_shown.resize(skins.size());
    for (size_t i = 0; i < skins.size(); i++) {
      s.to_shown[i] = to_document * skins[i];
    }
    /* Its own axis as it shows: where its points are taken (as the head bone carries a face whole). */
    s.mirror_self = dab.mirror_x && !layer_mirror_partner(const_cast<Document &>(doc), id);
    if (s.mirror_self) {
      const Transform2D shown = s.to_shown.empty() ? to_document : s.to_shown[0];
      s.mirror = shown * Transform2D::scaling({-1.0f, 1.0f}) * shown.inverted();
    }
    SculptDab mirrored = dab;
    mirrored.center = s.mirror.apply(dab.center);
    /* The drawing where its points show (moved by the bones, as the viewport has it: stored handles turn
     * with their point, the others are worked out from where the points went), then the curves as the
     * level being edited shows them; their points keep the drawing's indices. */
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
    const VectorDrawing edited = drawing_has_detail(posed) ? drawing_detail_shown_curves(posed) : posed;
    std::vector<std::vector<float>> factors_in, factors_out;
    drawing_detail_shown_handle_factors(posed, factors_in, factors_out);
    for (int ci = 0; ci < int(edited.curves.size()) && ci < int(drawing.curves.size()); ci++) {
      const Curve &shown = edited.curves[size_t(ci)];
      const Curve &real = drawing.curves[size_t(ci)];
      SculptStroke::Curve c;
      c.curve = ci;
      c.closed = shown.closed;
      /* Each shown point's place on the real curve. */
      int from = 0;
      for (const CurvePoint &cp : shown.points) {
        int found = -1;
        for (int k = from; k < int(real.points.size()); k++) {
          if (real.points[size_t(k)].point == cp.point) {
            found = k;
            break;
          }
        }
        if (found < 0) {
          break;
        }
        c.points.push_back(found);
        from = found + 1;
      }
      const int n = int(c.points.size());
      if (n < 2 || n != int(shown.points.size())) {
        continue;
      }
      const auto selection = [&](int i) {
        const MeshPoint &p = drawing.points[size_t(shown.points[size_t(i)].point)];
        return !dab.selected_only || p.selected ? 1.0f : 0.0f;
      };
      const int segments = c.closed ? n : n - 1;
      c.anchor.resize(size_t(n));
      c.handle_in.resize(size_t(n));
      c.handle_out.resize(size_t(n));
      c.factor_in = factors_in[size_t(ci)];
      c.factor_out = factors_out[size_t(ci)];
      for (int seg = 0; seg < segments; seg++) {
        const int a = seg, b = (seg + 1) % n;
        const int pa = shown.points[size_t(a)].point, pb = shown.points[size_t(b)].point;
        float2 ctrl[4];
        curve_segment_bezier(edited, shown, seg, ctrl);
        float2 doc_ctrl[4];
        for (int k = 0; k < 4; k++) {
          doc_ctrl[k] = to_document.apply(ctrl[k]);
        }
        c.handle_out[size_t(a)] = doc_ctrl[1] - doc_ctrl[0];
        c.handle_in[size_t(b)] = doc_ctrl[2] - doc_ctrl[3];
        const float reach = distance(doc_ctrl[0], doc_ctrl[1]) + distance(doc_ctrl[1], doc_ctrl[2]) +
                            distance(doc_ctrl[2], doc_ctrl[3]);
        const int count = std::clamp(int(std::ceil(reach / spacing)), 8, 256);
        c.anchor[size_t(a)] = int(c.pos.size());
        const float wa = drawing.points[size_t(pa)].radius, wb = drawing.points[size_t(pb)].radius;
        const float sa = selection(a), sb = selection(b);
        for (int k = 0; k < count; k++) {
          const float t = float(k) / float(count);
          c.pos.push_back(bezier_eval(doc_ctrl, t));
          c.width.push_back(wa + (wb - wa) * t);
          c.select.push_back(sa + (sb - sa) * t);
        }
        if (!c.closed && seg == segments - 1) {
          c.anchor[size_t(b)] = int(c.pos.size());
          c.pos.push_back(doc_ctrl[3]);
          c.width.push_back(wb);
          c.select.push_back(sb);
        }
      }
      c.in_line.resize(size_t(n));
      for (int i = 0; i < n; i++) {
        const float2 hi = normalized(c.handle_in[size_t(i)]), ho = normalized(c.handle_out[size_t(i)]);
        c.in_line[size_t(i)] = length(hi) > 0.0f && length(ho) > 0.0f && dot(hi, ho) < -0.99999f;
      }
      c.width_start = c.width;
      if (dab.brush == SculptBrush::Grab) {
        c.grab.resize(c.pos.size());
        for (size_t k = 0; k < c.pos.size(); k++) {
          c.grab[k] = weight_at(dab, c.pos[k]) * c.select[k];
        }
        if (s.mirror_self) {
          c.grab_mirror.resize(c.pos.size());
          for (size_t k = 0; k < c.pos.size(); k++) {
            c.grab_mirror[k] = weight_at(mirrored, c.pos[k]) * c.select[k];
          }
        }
      }
      s.curves.push_back(std::move(c));
    }
    if (!s.curves.empty()) {
      stroke.layers.push_back(std::move(s));
    }
  }
  return stroke;
}

/* X mirror within a layer: each point the dab changed and its image across the layer's axis, found where
 * the points rested before the dab (on the setup frame the dab moves the rest itself), whatever is
 * selected (sculpting doesn't go by the selection). A point the dab changed on both sides keeps its own. */
static std::vector<MirrorPair> sculpt_mirror_pairs(const std::vector<float2> &rest, const std::vector<int> &changed,
                                                   const std::vector<bool> &point_changed, float brush_side)
{
  std::vector<MirrorPair> pairs;
  for (const int a : changed) {
    const float2 here = rest[size_t(a)];
    const float eps = 1e-4f + 1e-4f * (std::fabs(here.x) + std::fabs(here.y));
    const float2 target{-here.x, here.y};
    if (distance(target, here) <= eps) {
      continue; /* On the axis: its own image. */
    }
    for (int b = 0; b < int(rest.size()); b++) {
      if (b == a || distance(rest[size_t(b)], target) > eps) {
        continue;
      }
      /* Brushed on one side only: that side leads. Both (the mirrored dab did the other): the side the
       * brush itself is on leads, once for the pair, so the two stay exact mirror images. */
      if (!point_changed[size_t(b)]) {
        pairs.push_back({a, b, true, false});
      }
      else if ((here.x >= 0.0f) == (brush_side >= 0.0f)) {
        pairs.push_back({a, b, true, false});
      }
      break;
    }
  }
  return pairs;
}

int sculpt_stroke_dab(Document &doc, SculptStroke &stroke, const SculptDab &dab)
{
  const float strength = std::clamp(dab.strength, 0.0f, 1.0f);
  /* Brushes that work on while held: a full step (amount 1) goes this far. */
  const float step = strength * std::clamp(dab.amount, 0.0f, 1.0f);
  const float sign = dab.invert ? -1.0f : 1.0f;
  int count = 0;
  /* X mirror: a layer named for a side (Ear_L) mirrors into its partner (Ear_R), as moves do. */
  std::vector<uint64_t> ids;
  for (const SculptStroke::Layer &sl : stroke.layers) {
    ids.push_back(sl.id);
  }
  const PartnerMirror partners = layer_mirror_partners_begin(doc, ids, dab.mirror_x);
  /* Thickness: the points whose width changed, by layer, for their mirror images. */
  std::vector<std::pair<uint64_t, std::vector<int>>> widened;
  for (SculptStroke::Layer &sl : stroke.layers) {
    Layer *layer = layer_find(doc, sl.id);
    if (!layer || !sculptable(*layer)) {
      continue;
    }
    VectorDrawing &drawing = layer->drawing;
    std::vector<bool> point_changed(drawing.points.size(), false);
    /* Where the points rest before this dab, for the X mirror's pairs. */
    const bool mirror_self = dab.mirror_x && layer_mirror_x_self(doc, sl.id, true);
    std::vector<float2> rest;
    /* The drawing as it was before this dab: what mirrors is the change, so a pose that isn't symmetric (a
     * turned head) stays as it is, only changed alike on both sides. */
    VectorDrawing before_dab;
    if (mirror_self) {
      for (const MeshPoint &p : drawing.points) {
        rest.push_back(point_setup_pos(p));
      }
      before_dab = drawing;
    }
    for (SculptStroke::Curve &c : sl.curves) {
      if (c.curve >= int(drawing.curves.size())) {
        continue;
      }
      Curve &real = drawing.curves[size_t(c.curve)];
      const size_t total = c.pos.size();
      const int n = int(c.points.size());
      if (n < 2 || c.points.back() >= int(real.points.size())) {
        continue;
      }
      std::vector<float2> moved_to = c.pos;
      std::vector<bool> width_changed(total, false);
      /* The brush as dabbed and, mirrored within the layer, again (its turn the other way): each works on
       * the line as the one before left it, weighed where the line was. */
      for (int side = 0; side < (sl.mirror_self ? 2 : 1); side++) {
        SculptDab brush = dab;
        float turn_sign = sign;
        const std::vector<float> *grab = &c.grab;
        if (side == 1) {
          brush.center = sl.mirror.apply(dab.center);
          brush.move = sl.mirror.apply_vector(dab.move);
          turn_sign = -sign;
          grab = &c.grab_mirror;
        }
        std::vector<float> w(total, 0.0f);
        if (brush.brush != SculptBrush::Grab) {
          for (size_t k = 0; k < total; k++) {
            w[k] = weight_at(brush, c.pos[k]) * c.select[k];
          }
        }
        switch (brush.brush) {
          case SculptBrush::Grab:
            for (size_t k = 0; k < total && k < grab->size(); k++) {
              moved_to[k] = moved_to[k] + brush.move * ((*grab)[k] * strength);
            }
            break;
          case SculptBrush::Push:
            for (size_t k = 0; k < total; k++) {
              moved_to[k] = moved_to[k] + brush.move * (w[k] * strength);
            }
            break;
          case SculptBrush::Pinch:
            for (size_t k = 0; k < total; k++) {
              moved_to[k] = moved_to[k] + (brush.center - moved_to[k]) * (w[k] * step * 0.5f * sign);
            }
            break;
          case SculptBrush::Twist:
            for (size_t k = 0; k < total; k++) {
              moved_to[k] = brush.center + rotate(moved_to[k] - brush.center, w[k] * step * 0.5f * turn_sign);
            }
            break;
          case SculptBrush::Smooth: {
            /* Toward the middle of the samples around each, a few on either side; an open curve's ends
             * stay, as Smooth Points keeps them. */
            const int window = 3;
            for (int pass = 0; pass < 2; pass++) {
              const std::vector<float2> was = moved_to;
              for (size_t k = 0; k < total; k++) {
                if (w[k] <= 0.0f || (!c.closed && (k == 0 || k + 1 == total))) {
                  continue;
                }
                float2 sum;
                int taken = 0;
                for (int d = -window; d <= window; d++) {
                  long j = long(k) + d;
                  if (c.closed) {
                    j = (j % long(total) + long(total)) % long(total);
                  }
                  else if (j < 0 || j >= long(total)) {
                    continue;
                  }
                  sum += was[size_t(j)];
                  taken++;
                }
                moved_to[k] = was[k] + (sum / float(taken) - was[k]) * (w[k] * step);
              }
            }
            break;
          }
          case SculptBrush::Thickness:
            for (size_t k = 0; k < total; k++) {
              if (w[k] > 0.0f) {
                c.width[k] = std::clamp(c.width[k] + w[k] * step * 0.25f * sign, 0.0f, 20.0f);
                width_changed[k] = true;
              }
            }
            break;
        }
      }
      std::vector<bool> moved(total, false);
      for (size_t k = 0; k < total; k++) {
        moved[k] = !(moved_to[k] == c.pos[k]);
      }
      c.pos = moved_to;

      /* The segments the dab touched are fitted again; the others stay exactly as they are. */
      const int segments = segment_count(c);
      std::vector<std::vector<int>> samples(static_cast<size_t>(segments));
      std::vector<bool> refit(size_t(segments), false);
      for (int s = 0; s < segments; s++) {
        samples[size_t(s)] = segment_samples(c, s);
        for (const int k : samples[size_t(s)]) {
          refit[size_t(s)] = refit[size_t(s)] || moved[size_t(k)];
        }
      }
      const auto real_cp = [&](int i) -> CurvePoint & { return real.points[size_t(c.points[size_t(i)])]; };
      const auto point_of = [&](int i) { return real_cp(i).point; };
      const auto held = [&](int i) { return dab.selected_only && !drawing.points[size_t(point_of(i))].selected; };
      /* Each touched segment's handles are fitted to its samples, every point going where its sample went;
       * a held point (Selected Only) keeps its place and handles. Document space until written. */
      std::vector<float2> new_out(static_cast<size_t>(n)), new_in(static_cast<size_t>(n));
      std::vector<bool> has_out(size_t(n), false), has_in(size_t(n), false);
      const auto stored_handle = [&](int i, bool pre) {
        return pre ? c.handle_in[size_t(i)] : c.handle_out[size_t(i)];
      };
      for (int s = 0; s < segments; s++) {
        if (!refit[size_t(s)]) {
          continue;
        }
        const int a = s, b = (s + 1) % n;
        std::vector<float2> pts;
        for (const int k : samples[size_t(s)]) {
          pts.push_back(c.pos[size_t(k)]);
        }
        const float2 kept1 = stored_handle(a, false), kept2 = stored_handle(b, true);
        float2 h1, h2;
        fit_handles(pts, pts.front(), pts.back(), held(a) ? &kept1 : nullptr, held(b) ? &kept2 : nullptr,
                    std::max(length(kept1), length(kept2)), &h1, &h2);
        new_out[size_t(a)] = h1;
        has_out[size_t(a)] = true;
        new_in[size_t(b)] = h2;
        has_in[size_t(b)] = true;
      }
      /* A smooth (aligned) point whose handles were in line keeps them so: opposite the handle on a side left as it was,
       * else along the average of the two fitted ones. */
      for (int i = 0; i < n; i++) {
        if (held(i) || !real_cp(i).aligned || !c.in_line[size_t(i)] || !(has_out[size_t(i)] || has_in[size_t(i)])) {
          continue;
        }
        const bool both_sides = c.closed || (i > 0 && i < n - 1);
        if (!both_sides) {
          continue;
        }
        float2 &out = new_out[size_t(i)], &in = new_in[size_t(i)];
        if (has_out[size_t(i)] && has_in[size_t(i)]) {
          const float2 along = normalized(normalized(out) - normalized(in));
          if (length(along) > 0.0f) {
            out = along * length(out);
            in = -along * length(in);
          }
        }
        else if (has_out[size_t(i)]) {
          const float2 opposite = -normalized(stored_handle(i, true));
          if (length(opposite) > 0.0f) {
            out = opposite * std::max(dot(out, opposite), length(out) * 0.25f);
          }
        }
        else {
          const float2 opposite = -normalized(stored_handle(i, false));
          if (length(opposite) > 0.0f) {
            in = opposite * std::max(dot(in, opposite), length(in) * 0.25f);
          }
        }
      }
      for (int i = 0; i < n; i++) {
        if (held(i) || !(has_out[size_t(i)] || has_in[size_t(i)])) {
          continue;
        }
        CurvePoint &cp = real_cp(i);
        const Transform2D back = sl.to_shown[size_t(cp.point)].inverted();
        if (has_out[size_t(i)]) {
          c.handle_out[size_t(i)] = new_out[size_t(i)];
        }
        if (has_in[size_t(i)]) {
          c.handle_in[size_t(i)] = new_in[size_t(i)];
        }
        /* Both stored, the side not fitted as it shows (a handle the curve worked out becomes a stored one). */
        cp.handle_in = back.apply_vector(c.handle_in[size_t(i)]) / std::max(c.factor_in[size_t(i)], 1e-6f);
        cp.handle_out = back.apply_vector(c.handle_out[size_t(i)]) / std::max(c.factor_out[size_t(i)], 1e-6f);
        cp.handles_set = true;
        drawing.points[size_t(cp.point)].pos = back.apply(c.pos[size_t(c.anchor[size_t(i)])]);
        point_changed[size_t(cp.point)] = true;
      }

      /* Thickness: each point's width changes by how much its stretch of the line did on average
       * (half way to the points either side). */
      if (dab.brush == SculptBrush::Thickness) {
        for (int i = 0; i < n; i++) {
          if (held(i)) {
            continue;
          }
          float sum = 0.0f;
          int taken = 0;
          bool any = false;
          for (const int s : {i - 1, i}) {
            if (s < 0 && !c.closed) {
              continue;
            }
            if (s >= segments) {
              continue;
            }
            const std::vector<int> &idx = samples[size_t((s + segments) % segments)];
            const bool before = s < i;
            const size_t half = idx.size() / 2;
            for (size_t j = before ? half : 0; j < (before ? idx.size() : half + 1); j++) {
              const size_t k = size_t(idx[j]);
              sum += c.width[k] - c.width_start[k];
              any = any || width_changed[k];
              taken++;
            }
          }
          if (any && taken > 0) {
            const int k = c.anchor[size_t(i)];
            MeshPoint &p = drawing.points[size_t(point_of(i))];
            p.radius = std::clamp(c.width_start[size_t(k)] + sum / float(taken), 0.0f, 20.0f);
            point_changed[size_t(point_of(i))] = true;
          }
        }
      }
    }
    std::vector<int> changed;
    for (size_t i = 0; i < point_changed.size(); i++) {
      if (point_changed[i]) {
        changed.push_back(int(i));
      }
    }
    count += int(changed.size());
    if (changed.empty()) {
      continue;
    }
    if (mirror_self && before_dab.curves.size() == drawing.curves.size() && before_dab.points.size() == drawing.points.size()) {
      const auto reflect = [](float2 v) { return float2{-v.x, v.y}; };
      /* A point on the axis (its own image) moves only along it, its handles changing as each other's
       * mirror image (the one the dab changed more leads). */
      for (int ci = 0; ci < int(drawing.curves.size()); ci++) {
        for (int k = 0; k < int(drawing.curves[size_t(ci)].points.size()); k++) {
          CurvePoint &cp = drawing.curves[size_t(ci)].points[size_t(k)];
          const float2 at = rest[size_t(cp.point)];
          if (!point_changed[size_t(cp.point)] || std::fabs(at.x) > 1e-4f + 1e-4f * std::fabs(at.y)) {
            continue;
          }
          const CurvePoint &old = before_dab.curves[size_t(ci)].points[size_t(k)];
          drawing.points[size_t(cp.point)].pos.x = before_dab.points[size_t(cp.point)].pos.x;
          const float2 d_in = cp.handle_in - old.handle_in, d_out = cp.handle_out - old.handle_out;
          if (length(d_in) >= length(d_out)) {
            cp.handle_out = old.handle_out + reflect(d_in);
          }
          else {
            cp.handle_in = old.handle_in + reflect(d_out);
          }
        }
      }
      /* Each pair: the side under the brush leads, its change mirrored onto the other. */
      const float brush_side = (sl.to_shown.empty() ? Transform2D() : sl.to_shown[0]).inverted().apply(dab.center).x;
      const std::vector<MirrorPair> pairs = sculpt_mirror_pairs(rest, changed, point_changed, brush_side);
      if (!pairs.empty()) {
        /* What a mirror image of the leading side would be, before and now: the image takes the difference. */
        VectorDrawing image_was = before_dab, image_now = drawing;
        drawing_apply_mirror(image_was, pairs);
        drawing_apply_mirror(image_now, pairs);
        std::vector<bool> is_image(drawing.points.size(), false);
        for (const MirrorPair &m : pairs) {
          is_image[size_t(m.dst)] = true;
          MeshPoint &dst = drawing.points[size_t(m.dst)];
          dst.pos = before_dab.points[size_t(m.dst)].pos + (image_now.points[size_t(m.dst)].pos - image_was.points[size_t(m.dst)].pos);
          if (dab.brush == SculptBrush::Thickness) {
            dst.radius = std::clamp(before_dab.points[size_t(m.dst)].radius +
                                        (drawing.points[size_t(m.src)].radius - before_dab.points[size_t(m.src)].radius),
                                    0.0f, 20.0f);
          }
          point_changed[size_t(m.dst)] = true;
        }
        for (int ci = 0; ci < int(drawing.curves.size()); ci++) {
          for (int k = 0; k < int(drawing.curves[size_t(ci)].points.size()); k++) {
            CurvePoint &cp = drawing.curves[size_t(ci)].points[size_t(k)];
            if (!is_image[size_t(cp.point)]) {
              continue;
            }
            const CurvePoint &old = before_dab.curves[size_t(ci)].points[size_t(k)];
            const CurvePoint &a = image_was.curves[size_t(ci)].points[size_t(k)];
            const CurvePoint &b = image_now.curves[size_t(ci)].points[size_t(k)];
            cp.handle_in = old.handle_in + (b.handle_in - a.handle_in);
            cp.handle_out = old.handle_out + (b.handle_out - a.handle_out);
            cp.handles_set = cp.handles_set || b.handles_set;
          }
        }
      }
    }
    if (dab.brush == SculptBrush::Thickness) {
      widened.push_back({sl.id, changed});
    }
    /* Finer detail rides on the points moved. */
    if (drawing_has_detail(drawing)) {
      drawing_detail_sync(drawing, true);
    }
  }
  count += layer_mirror_partners_end(doc, partners);
  /* Widths aren't part of a move: the partner's images take them here. */
  if (dab.mirror_x) {
    for (const auto &[id, points] : widened) {
      Layer *layer = layer_find(doc, id);
      Layer *partner = layer ? layer_mirror_partner(doc, id) : nullptr;
      if (!partner || !partner->has_drawing() || partner->is_reference()) {
        continue;
      }
      const std::vector<int> images = layer_mirror_images(doc, *layer, *partner, points);
      for (size_t k = 0; k < points.size() && k < images.size(); k++) {
        if (images[k] >= 0 && images[k] < int(partner->drawing.points.size())) {
          partner->drawing.points[size_t(images[k])].radius = layer->drawing.points[size_t(points[k])].radius;
          count++;
        }
      }
    }
  }
  return count;
}

}  // namespace oanim
