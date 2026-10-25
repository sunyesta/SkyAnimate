#include "editors/statistics.hh"

#include <algorithm>

#include "data/bones.hh"
#include "data/detail.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

std::string statistics_number(int n)
{
  std::string digits = std::to_string(n < 0 ? -n : n);
  std::string out;
  for (size_t i = 0; i < digits.size(); i++) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out += ',';
    }
    out += digits[i];
  }
  return n < 0 ? "-" + out : out;
}

/* One drawing's points, curves, segments and shapes, as shown (at each curve's edit level). */
static void count_drawing(const VectorDrawing &stored, Statistics &s)
{
  VectorDrawing shown_curves;
  const VectorDrawing *drawing = &stored;
  if (drawing_has_detail(stored)) {
    shown_curves = drawing_detail_shown_curves(stored);
    drawing = &shown_curves;
  }
  const auto picked = [&](int point) {
    return point >= 0 && point < int(drawing->points.size()) && drawing->points[point].selected;
  };
  for (const MeshPoint &p : drawing->points) {
    if (p.detail.hidden) {
      continue;
    }
    s.points++;
    s.points_selected += p.selected ? 1 : 0;
  }
  for (const Curve &curve : drawing->curves) {
    if (curve.points.empty()) {
      continue;
    }
    s.curves++;
    s.curves_selected += std::all_of(curve.points.begin(), curve.points.end(),
                                     [&](const CurvePoint &cp) { return picked(cp.point); })
                             ? 1
                             : 0;
    for (int i = 0; i < curve.segment_count(); i++) {
      int a, b;
      curve_segment_points(curve, i, &a, &b);
      s.segments++;
      s.segments_selected += picked(a) && picked(b) ? 1 : 0;
    }
  }
  for (const Shape &shape : drawing->shapes) {
    s.shapes++;
    bool all = !shape.edges.empty();
    for (const ShapeEdge &e : shape.edges) {
      if (e.curve < 0 || e.curve >= int(drawing->curves.size()) || e.segment < 0 ||
          e.segment >= drawing->curves[e.curve].segment_count())
      {
        continue;
      }
      int a, b;
      curve_segment_points(drawing->curves[e.curve], e.segment, &a, &b);
      all = all && picked(a) && picked(b);
    }
    s.shapes_selected += all ? 1 : 0;
  }
}

static Statistics statistics_count(Document &doc)
{
  Statistics s;
  const LayerIndexScope layer_index(doc);
  Layer *active = document_active_layer(doc);
  s.name = active ? active->name : std::string();
  s.frame = doc.frame_current;
  s.frame_end = doc.frame_end;
  for (const Layer *l : document_layers_flat(doc)) {
    s.layers++;
    s.layers_selected += l->selected || l->id == doc.active_layer ? 1 : 0;
  }
  if (active && active->is_bone_layer()) {
    s.mode = Statistics::Mode::Bones;
    for (const Bone &b : active->bones) {
      if (!bone_shown(*active, b)) {
        continue;
      }
      s.bones++;
      s.bones_selected += b.selected || b.select_head || b.select_tail ? 1 : 0;
    }
    return s;
  }
  const std::vector<Layer *> editing = document_editable_layers(doc);
  if (!editing.empty()) {
    s.mode = Statistics::Mode::Drawings;
    s.drawings = int(editing.size());
    for (const Layer *l : editing) {
      count_drawing(l->drawing, s);
    }
    return s;
  }
  /* Nothing being edited: how much there is in all. */
  s.mode = Statistics::Mode::Document;
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->has_drawing()) {
      count_drawing(l->drawing, s);
    }
  }
  s.points_selected = s.curves_selected = s.segments_selected = s.shapes_selected = 0;
  return s;
}

Statistics document_statistics(Context &C, bool fresh)
{
  static Statistics kept;
  static uint64_t kept_key = 0;
  if (!C.document) {
    return {};
  }
  Document &doc = *C.document;
  /* What decides which drawings are edited, and the undo count for everything else that changes them. */
  uint64_t key = 1469598103934665603ull;
  const auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
  mix(uint64_t(reinterpret_cast<uintptr_t>(&doc)));
  mix(C.undo ? C.undo->change_count() : 0);
  mix(doc.active_layer);
  mix(uint64_t(uint32_t(doc.frame_current)));
  mix(uint64_t(uint32_t(doc.frame_end)));
  for (const Layer *l : document_layers_flat(doc)) {
    mix(l->id);
    mix(uint64_t(l->selected) | uint64_t(l->visible) << 1 | uint64_t(l->locked) << 2 |
        uint64_t(l->edit_descendants) << 3);
  }
  if (fresh || !C.undo || key != kept_key) {
    kept = statistics_count(doc);
    kept_key = C.undo ? key : 0;
  }
  return kept;
}

std::vector<std::string> statistics_parts(const Statistics &s, bool statistics, bool frame, const char *version)
{
  std::vector<std::string> parts;
  const auto of = [](const char *what, int selected, int all) {
    return std::string(what) + ":" + statistics_number(selected) + "/" + statistics_number(all);
  };
  if (statistics) {
    if (!s.name.empty() && s.mode != Statistics::Mode::Document) {
      parts.push_back(s.name);
    }
    if (s.mode == Statistics::Mode::Bones) {
      parts.push_back(of("Bones", s.bones_selected, s.bones));
    }
    else if (s.mode == Statistics::Mode::Drawings) {
      if (s.drawings > 1) {
        parts.push_back("Drawings:" + statistics_number(s.drawings));
      }
      parts.push_back(of("Points", s.points_selected, s.points));
      parts.push_back(of("Curves", s.curves_selected, s.curves));
      parts.push_back(of("Segments", s.segments_selected, s.segments));
      parts.push_back(of("Shapes", s.shapes_selected, s.shapes));
    }
    parts.push_back(of("Layers", s.layers_selected, s.layers));
    if (s.mode == Statistics::Mode::Document) {
      parts.push_back("Points:" + statistics_number(s.points));
    }
  }
  if (frame) {
    parts.push_back("Frame:" + statistics_number(s.frame) + "/" + statistics_number(s.frame_end));
  }
  if (version && *version) {
    parts.push_back(version);
  }
  return parts;
}

}  // namespace oanim
