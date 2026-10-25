/* Annotation and drawing guide operators (data/annotations.hh): the Annotate tools' strokes, the eraser,
 * annotation layers, and making, changing and deleting guides. Undoable; none of them touch the drawing. */

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "anim/deform.hh"
#include "anim/merge.hh"
#include "core/log.hh"
#include "data/annotations.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

/* "x y x y ..." (spaces, commas or semicolons between) as points. */
static std::vector<float2> parse_points(const std::string &text)
{
  std::string spaced = text;
  std::replace_if(spaced.begin(), spaced.end(), [](char c) { return c == ',' || c == ';' || c == '|'; }, ' ');
  std::istringstream in(spaced);
  std::vector<float2> points;
  float x = 0.0f, y = 0.0f;
  while (in >> x >> y) {
    points.push_back({x, y});
  }
  return points;
}

/* "r g b" or "r g b a" (0 to 1). */
static bool parse_color(const std::string &text, Color *r_color)
{
  std::string spaced = text;
  std::replace(spaced.begin(), spaced.end(), ',', ' ');
  std::istringstream in(spaced);
  Color c;
  if (!(in >> c.r >> c.g >> c.b)) {
    return false;
  }
  if (!(in >> c.a)) {
    c.a = 1.0f;
  }
  c.r = std::clamp(c.r, 0.0f, 1.0f);
  c.g = std::clamp(c.g, 0.0f, 1.0f);
  c.b = std::clamp(c.b, 0.0f, 1.0f);
  c.a = std::clamp(c.a, 0.0f, 1.0f);
  *r_color = c;
  return true;
}

/* The `key=` index into a list of `size`, or `fallback` (the active one) without it; -1 when out of range. */
static int index_arg(Context &C, const char *key, int fallback, size_t size)
{
  const int i = op_has_arg(C, key) ? op_arg_int(C, key, -1) : fallback;
  return i >= 0 && i < int(size) ? i : -1;
}

static bool have_document(Context &C)
{
  return C.document != nullptr;
}

static bool have_guides(Context &C)
{
  return C.document && !C.document->guides.empty();
}

/* Deletes guide index= (the picked one by default). */
static OpStatus guide_delete_exec(Context &C)
{
  Document &doc = *C.document;
  const int i = index_arg(C, "index", doc.active_guide, doc.guides.size());
  if (i < 0) {
    return OpStatus::Cancelled;
  }
  doc.guides.erase(doc.guides.begin() + i);
  doc.active_guide = doc.active_guide == i ? -1 : doc.active_guide - (doc.active_guide > i ? 1 : 0);
  return OpStatus::Finished;
}

float2 guide_snap(const Document &doc, int guide, int point, float2 p, float radius, bool *r_snapped)
{
  if (r_snapped) {
    *r_snapped = false;
  }
  const DrawingGuide *self = guide >= 0 && guide < int(doc.guides.size()) ? &doc.guides[size_t(guide)] : nullptr;
  /* A point of the drawing, or another guide's handle, within reach: the nearest. */
  float best = radius;
  float2 found = p;
  bool any = false;
  const auto consider = [&](float2 q) {
    const float d = length(q - p);
    if (d < best) {
      best = d;
      found = q;
      any = true;
    }
  };
  for (const Layer *flat : document_layers_flat(doc)) {
    if (flat->type != LayerType::Vector || !layer_shown_in_document(doc, flat->id)) {
      continue;
    }
    const VectorDrawing drawing = layer_evaluated_drawing(doc, *flat, false);
    const Transform2D to_document = layer_world_transform(doc, flat->id);
    for (const MeshPoint &mp : drawing.points) {
      consider(to_document.apply(mp.pos));
    }
  }
  for (int g = 0; g < int(doc.guides.size()); g++) {
    const DrawingGuide &other = doc.guides[size_t(g)];
    if (g == guide || other.hidden) {
      continue;
    }
    for (const float2 &q : other.points) {
      consider(q);
    }
  }
  if (any) {
    if (r_snapped) {
      *r_snapped = true;
    }
    return found;
  }
  /* Another guide's straight line (a Line, a perspective's horizon): the nearest point on it. */
  best = radius;
  for (int g = 0; g < int(doc.guides.size()); g++) {
    const DrawingGuide &other = doc.guides[size_t(g)];
    if (g == guide || other.hidden || other.points.empty()) {
      continue;
    }
    float2 a = other.points[0], dir;
    if (other.type == GuideType::Line && other.points.size() >= 2) {
      dir = normalize(other.points[1] - a);
    }
    else if (other.type == GuideType::Perspective) {
      dir = other.points.size() >= 2 ? normalize(other.points[1] - a) : float2(1.0f, 0.0f);
    }
    else {
      continue;
    }
    const float2 on = a + dir * dot(p - a, dir);
    const float d = length(on - p);
    if (d < best) {
      best = d;
      found = on;
      any = true;
    }
  }
  if (any) {
    if (r_snapped) {
      *r_snapped = true;
    }
    return found;
  }
  /* A line's (a grid's, the horizon's) turn in steps of 15 degrees, about the guide's other handle. */
  if (self && self->points.size() >= 2 && point >= 0 && point < 2) {
    const float2 pivot = self->points[size_t(1 - point)];
    const float2 v = p - pivot;
    const float len = length(v);
    if (len > 1e-9f) {
      const float step = PI / 12.0f;
      const float angle = std::round(std::atan2(v.y, v.x) / step) * step;
      if (r_snapped) {
        *r_snapped = true;
      }
      return pivot + float2(std::cos(angle), std::sin(angle)) * len;
    }
  }
  return p;
}

void annotation_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "annotation.draw";
    ot.name = "Annotate";
    ot.description = "Add a stroke to the active annotation layer (made, \"Note\", when there's none): points=\"x y x y "
                     "...\" in document space, start= and end= (NONE, ARROW, ARROW_OPEN, SEGMENT, DOT) for its ends";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_document;
    ot.exec = [](Context &C) {
      AnnotationStroke stroke;
      stroke.points = parse_points(op_arg(C, "points"));
      if (stroke.points.size() < 2) {
        report(ReportType::Error, "points needs at least two points, \"x y x y\"");
        return OpStatus::Cancelled;
      }
      if ((op_has_arg(C, "start") && !annotation_end_from_identifier(op_arg(C, "start"), &stroke.start)) ||
          (op_has_arg(C, "end") && !annotation_end_from_identifier(op_arg(C, "end"), &stroke.end)))
      {
        report(ReportType::Error, "start and end are NONE, ARROW, ARROW_OPEN, SEGMENT or DOT");
        return OpStatus::Cancelled;
      }
      AnnotationLayer &layer = annotation_layer_ensure(*C.document);
      if (layer.locked) {
        report(ReportType::Warning, "The annotation layer \"" + layer.name + "\" is locked");
        return OpStatus::Cancelled;
      }
      layer.hidden = false;
      layer.strokes.push_back(std::move(stroke));
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "annotation.erase";
    ot.name = "Erase Annotation";
    ot.description = "Rub out the annotations within radius= of x=, y= (document space) on the shown, unlocked "
                     "layers, splitting the strokes it crosses";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_document;
    ot.exec = [](Context &C) {
      const float2 at(op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f));
      const float radius = std::max(op_arg_float(C, "radius", 0.1f), 1e-6f);
      return annotation_erase(*C.document, at, radius) ? OpStatus::Finished : OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "annotation.layer_add";
    ot.name = "Add Annotation Layer";
    ot.description = "Add an annotation layer (name=, \"Note\" by default) above the active one and make it active";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_document;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      AnnotationLayer layer;
      const std::string name = op_arg(C, "name");
      layer.name = annotation_layer_unique_name(doc, name.empty() ? "Note" : name);
      const int at = doc.annotations.empty() ? 0 : std::clamp(doc.active_annotation + 1, 0, int(doc.annotations.size()));
      doc.annotations.insert(doc.annotations.begin() + at, std::move(layer));
      doc.active_annotation = at;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "annotation.layer_remove";
    ot.name = "Remove Annotation Layer";
    ot.description = "Delete annotation layer index= (the active one by default) with its strokes";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document && !C.document->annotations.empty(); };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int i = index_arg(C, "index", doc.active_annotation, doc.annotations.size());
      if (i < 0) {
        return OpStatus::Cancelled;
      }
      doc.annotations.erase(doc.annotations.begin() + i);
      doc.active_annotation = std::clamp(doc.active_annotation - (doc.active_annotation >= i ? 1 : 0), 0,
                                         std::max(0, int(doc.annotations.size()) - 1));
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "annotation.layer_set";
    ot.name = "Set Annotation Layer";
    ot.description = "Change annotation layer index= (the active one by default): name=, color=\"r g b [a]\", "
                     "thickness= (screen pixels), hidden=, locked=, active=1 to make it the one new strokes go on";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document && !C.document->annotations.empty(); };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int i = index_arg(C, "index", doc.active_annotation, doc.annotations.size());
      if (i < 0) {
        return OpStatus::Cancelled;
      }
      AnnotationLayer &layer = doc.annotations[size_t(i)];
      if (op_has_arg(C, "name")) {
        const std::string name = op_arg(C, "name");
        if (!name.empty() && name != layer.name) {
          layer.name = annotation_layer_unique_name(doc, name);
        }
      }
      if (op_has_arg(C, "color") && !parse_color(op_arg(C, "color"), &layer.color)) {
        report(ReportType::Error, "color is \"r g b\" or \"r g b a\", 0 to 1");
        return OpStatus::Cancelled;
      }
      if (op_has_arg(C, "thickness")) {
        layer.thickness = std::clamp(op_arg_float(C, "thickness", layer.thickness), ANNOTATION_THICKNESS_MIN,
                                     ANNOTATION_THICKNESS_MAX);
      }
      if (op_has_arg(C, "hidden")) {
        layer.hidden = op_arg_bool(C, "hidden");
      }
      if (op_has_arg(C, "locked")) {
        layer.locked = op_arg_bool(C, "locked");
      }
      if (op_arg_bool(C, "active")) {
        doc.active_annotation = i;
      }
      C.undo_name = "Change Annotation Layer";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "guide.add";
    ot.name = "Add Guide";
    ot.description = "Add a drawing guide and pick it: type= LINE (an endless line through its two points), "
                     "PERSPECTIVE (one to three vanishing points) or GRID (origin, then a cell's corner); points=\"x "
                     "y x y ...\" (document space); lines= (perspective: how many round each point)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_document;
    ot.exec = [](Context &C) {
      DrawingGuide g;
      if (!guide_type_from_identifier(op_arg(C, "type", "LINE"), &g.type)) {
        report(ReportType::Error, "type is LINE, PERSPECTIVE or GRID");
        return OpStatus::Cancelled;
      }
      g.points = parse_points(op_arg(C, "points"));
      if (g.points.empty()) {
        report(ReportType::Error, "points needs the guide's handles, \"x y x y\"");
        return OpStatus::Cancelled;
      }
      g.lines = op_arg_int(C, "lines", g.lines);
      guide_validate(g);
      Document &doc = *C.document;
      doc.guides.push_back(std::move(g));
      doc.active_guide = int(doc.guides.size()) - 1;
      C.undo_name = std::string("Add ") + guide_type_name(doc.guides.back().type) + " Guide";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "guide.set";
    ot.name = "Set Guide";
    ot.description = "Change guide index= (the picked one by default): points=\"x y ...\" (a perspective's count "
                     "is how many vanishing points it has, 1 to 3), count= (a perspective's vanishing points: more "
                     "are added beside the others), lines=, hidden=, locked=, active=1 to pick it";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_guides;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int i = index_arg(C, "index", doc.active_guide, doc.guides.size());
      if (i < 0) {
        return OpStatus::Cancelled;
      }
      DrawingGuide &g = doc.guides[size_t(i)];
      if (op_has_arg(C, "points")) {
        const std::vector<float2> points = parse_points(op_arg(C, "points"));
        if (points.empty()) {
          report(ReportType::Error, "points needs the guide's handles, \"x y x y\"");
          return OpStatus::Cancelled;
        }
        g.points = points;
      }
      if (op_has_arg(C, "count") && g.type == GuideType::Perspective) {
        const int count = std::clamp(op_arg_int(C, "count", int(g.points.size())), 1, 3);
        /* A new vanishing point goes along the horizon, then below the middle of the first two. */
        while (int(g.points.size()) < count) {
          const float2 a = g.points[0];
          const float2 b = g.points.size() >= 2 ? g.points[1] : a + float2(1.0f, 0.0f);
          const float span = std::max(length(b - a), 0.1f);
          g.points.push_back(g.points.size() == 1 ? a + float2(span * 2.0f, 0.0f)
                                                  : (a + b) * 0.5f + float2(0.0f, -span * 0.8f));
        }
        g.points.resize(size_t(count));
      }
      if (op_has_arg(C, "lines")) {
        g.lines = op_arg_int(C, "lines", g.lines);
      }
      if (op_has_arg(C, "hidden")) {
        g.hidden = op_arg_bool(C, "hidden");
      }
      if (op_has_arg(C, "locked")) {
        g.locked = op_arg_bool(C, "locked");
      }
      if (op_arg_bool(C, "active")) {
        doc.active_guide = i;
      }
      guide_validate(g);
      C.undo_name = "Change Guide";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "guide.move_point";
    ot.name = "Move Guide Handle";
    ot.description = "Put handle point= of guide index= (the picked one by default) at x=, y= (document space); "
                     "snap=1 snaps it as Ctrl does (to a point of the drawing, another guide's handle or line, or "
                     "the line's turn in 15 degree steps) within radius= (document units)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_guides;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int i = index_arg(C, "index", doc.active_guide, doc.guides.size());
      if (i < 0) {
        return OpStatus::Cancelled;
      }
      DrawingGuide &g = doc.guides[size_t(i)];
      const int k = op_arg_int(C, "point", 0);
      if (k < 0 || k >= int(g.points.size()) || g.locked) {
        return OpStatus::Cancelled;
      }
      float2 to(op_arg_float(C, "x", g.points[size_t(k)].x), op_arg_float(C, "y", g.points[size_t(k)].y));
      if (op_arg_bool(C, "snap")) {
        to = guide_snap(doc, i, k, to, std::max(op_arg_float(C, "radius", 0.05f), 0.0f), nullptr);
      }
      g.points[size_t(k)] = to;
      guide_validate(g);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "guide.delete";
    ot.name = "Delete Guide";
    ot.description = "Delete guide index= (the picked one by default)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_guides;
    ot.exec = guide_delete_exec;
    operators().add(std::move(ot));
  }
  {
    /* X or Delete over the viewport with a guide tool: the picked guide goes (the keymap tries this first). */
    OperatorType ot;
    ot.idname = "guide.delete_picked";
    ot.name = "Delete Guide";
    ot.description = "Delete the picked guide, while a guide tool is active";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) {
      return C.document && C.wm && C.wm->tool_settings.tool == Tool::Annotate &&
             annotate_mode_is_guide(C.wm->tool_settings.annotate_mode) && C.document->active_guide >= 0 &&
             C.document->active_guide < int(C.document->guides.size());
    };
    ot.exec = guide_delete_exec;
    operators().add(std::move(ot));
  }
}

}  // namespace oanim
