/* The Draw tool's operators (data/ink_stroke.hh, assets/draw_presets.hh): a scripted stroke, picking, saving,
 * putting back and deleting presets, the presets popup, and F / Shift F to size the line. */

#include <sstream>
#include <string>
#include <vector>

#include "assets/draw_presets.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

/* How near (screen pixels) a stroke's end welds to a point. */
static constexpr float WELD_PIXELS = 8.0f;

/* Picks a preset, as clicking it on the shelf does: the pen going keeps what it was changed to (for coming back
 * to), and the one coming takes what it was last changed to, else its saved settings. Clicking a pen always
 * makes Draw the tool, as a brush in Blender's shelf does. */
static void preset_apply(ToolSettings &ts, const DrawPreset &p)
{
  ts.tool = Tool::Draw;
  ts.draw_mode = p.mode;
  if (p.mode != DrawMode::Freehand) {
    return;
  }
  ts.draw_changes[ts.draw_preset] = {ts.ink, ts.style.line_width};
  ts.draw_preset = p.name;
  ts.ink = p.ink;
  ts.style.line_width = p.line_width;
  if (const auto it = ts.draw_changes.find(p.name); it != ts.draw_changes.end()) {
    ts.ink = it->second.ink;
    ts.style.line_width = it->second.line_width;
  }
}

bool draw_stroke_commit(Context &C, VectorDrawing &drawing, InkStroke &ink)
{
  ToolSettings &ts = C.wm->tool_settings;
  if (ink.empty()) {
    return false;
  }
  ink.finish();
  const std::vector<InkPoint> &stroke = ink.points();
  const float pixel = ink.view().pixel;
  const bool closes = ts.draw_fill && stroke.size() > 8 && distance(stroke.front().pos, stroke.back().pos) < pixel * 14.0f;
  const std::vector<InkKnot> knots = ink_fit(stroke, ink.settings(), ink.view(), closes);
  C.wm->draw_last = ink;
  C.wm->draw_last_closed = closes;
  if (knots.size() < 2) {
    return false;
  }
  /* Before the stroke's own points are in, so it can't weld to itself. */
  int weld_first = -1, weld_last = -1;
  if (ts.draw_weld && !closes) {
    weld_first = drawing_pick_point(drawing, knots.front().pos, WELD_PIXELS * pixel);
    weld_last = drawing_pick_point(drawing, knots.back().pos, WELD_PIXELS * pixel);
    /* A point can't be both ends of an open line. */
    if (weld_last == weld_first) {
      weld_last = -1;
    }
  }
  Curve curve;
  for (MeshPoint &p : drawing.points) {
    p.selected = false;
  }
  drawing_deselect_handles(drawing);
  for (size_t i = 0; i < knots.size(); i++) {
    const InkKnot &knot = knots[i];
    int p = i == 0 ? weld_first : i + 1 == knots.size() ? weld_last : -1;
    if (p < 0) {
      p = drawing_add_point(drawing, knot.pos);
      drawing.points[p].radius = knot.radius;
    }
    CurvePoint cp{p};
    cp.curvature = knot.corner ? CURVATURE_PEAKED : CURVATURE_SMOOTH;
    cp.aligned = !knot.corner;
    curve_point_set_handles(cp, knot.handle_in, knot.handle_out);
    curve.points.push_back(cp);
  }
  curve.closed = closes && curve.points.size() >= 3;
  drawing.curves.push_back(curve);
  Shape shape;
  shape.style = ts.style;
  shape.has_fill = curve.closed;
  shape.name = "Stroke " + std::to_string(drawing.shapes.size() + 1);
  for (int s = 0; s < drawing.curves.back().segment_count(); s++) {
    shape.edges.push_back({int(drawing.curves.size()) - 1, s});
  }
  drawing.shapes.push_back(std::move(shape));
  return true;
}

/* "x,y,pressure,seconds;...": the stroke's samples in the active layer's space. */
static std::vector<InkSample> parse_samples(const std::string &text)
{
  std::vector<InkSample> samples;
  std::stringstream items(text);
  std::string item;
  while (std::getline(items, item, ';')) {
    InkSample s;
    char comma = 0;
    std::stringstream row(item);
    if (row >> s.pos.x >> comma >> s.pos.y >> comma >> s.pressure >> comma >> s.time) {
      samples.push_back(s);
    }
  }
  return samples;
}

static OpStatus stroke_exec(Context &C)
{
  Document &doc = *C.document;
  ToolSettings &ts = C.wm->tool_settings;
  Layer *layer = document_active_layer(doc);
  std::vector<Layer *> layers = editable_layers(C);
  if (doc.frame_current != 0 || !layer || std::find(layers.begin(), layers.end(), layer) == layers.end()) {
    report(ReportType::Error, "draw.stroke draws on the active drawing, on the setup frame");
    return OpStatus::Cancelled;
  }
  const float pixel = op_arg_float(C, "pixel", 2.0f / std::max(float(doc.height), 1.0f));
  InkStroke ink;
  ink.begin(ts.ink, {pixel, ts.style.line_width * 0.5f, 2.0f / std::max(float(doc.height), 1.0f)});
  for (const InkSample &sample : parse_samples(op_arg(C, "samples"))) {
    ink.add(sample);
  }
  return draw_stroke_commit(C, layer->drawing, ink) ? OpStatus::Finished : OpStatus::Cancelled;
}

void draw_operators_register()
{
  const auto add = [](const char *idname, const char *name, const char *description, unsigned flag,
                      std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    ot.flag = flag;
    ot.poll = std::move(poll);
    ot.exec = std::move(exec);
    operators().add(std::move(ot));
  };
  const auto draw_tool = [](Context &C) { return C.wm->tool_settings.tool == Tool::Draw; };
  const auto stroke_pen = [](Context &C) {
    const ToolSettings &ts = C.wm->tool_settings;
    return ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Freehand;
  };

  add("draw.stroke", "Draw",
      "A stroke through the Stroke Pen's engine with the Draw tool's settings, on the active drawing: samples=\"x,y,"
      "pressure,seconds;...\" in the layer's space, pixel= the layer units a screen pixel stands for",
      OPTYPE_UNDO, [](Context &C) { return C.document != nullptr; }, stroke_exec);
  add("draw.preset_load", "Load Draw Preset", "Pick the pen named name= (as clicking it on the shelf)", 0, nullptr,
      [](Context &C) {
        const DrawPreset *p = draw_preset_find(op_arg(C, "name"));
        if (!p) {
          report(ReportType::Error, "No pen named \"" + op_arg(C, "name") + "\"");
          return OpStatus::Cancelled;
        }
        preset_apply(C.wm->tool_settings, *p);
        return OpStatus::Finished;
      });
  add("draw.preset_save", "Save Draw Preset",
      "Save the Draw tool's pen and settings to the asset library as name= (made unique), shown under Mine",
      OPTYPE_REGISTER, nullptr, [](Context &C) {
        ToolSettings &ts = C.wm->tool_settings;
        DrawPreset p;
        p.name = op_arg(C, "name", ts.draw_preset);
        p.ink = ts.ink;
        p.line_width = ts.style.line_width;
        std::string error;
        const std::string saved = draw_preset_save(p, &error);
        if (saved.empty()) {
          report(ReportType::Error, error);
          return OpStatus::Cancelled;
        }
        ts.draw_preset = saved;
        /* Saved as it is: nothing changed from that. */
        ts.draw_changes.erase(saved);
        report(ReportType::Info, "Saved pen \"" + saved + "\"");
        return OpStatus::Finished;
      });
  add("draw.preset_reset", "Put Back Draw Preset", "Put the pen named name= back to its saved settings", 0, nullptr,
      [](Context &C) {
        ToolSettings &ts = C.wm->tool_settings;
        const DrawPreset *p = draw_preset_find(op_arg(C, "name"));
        if (!p) {
          report(ReportType::Error, "No pen named \"" + op_arg(C, "name") + "\"");
          return OpStatus::Cancelled;
        }
        ts.draw_changes.erase(p->name);
        if (ts.draw_preset == p->name) {
          ts.ink = p->ink;
          ts.style.line_width = p->line_width;
        }
        return OpStatus::Finished;
      });
  add("draw.preset_remove", "Delete Draw Preset", "Delete the saved pen named name= from the asset library",
      OPTYPE_REGISTER, nullptr, [](Context &C) {
        std::string error;
        if (!draw_preset_remove(op_arg(C, "name"), &error)) {
          report(ReportType::Error, error);
          return OpStatus::Cancelled;
        }
        C.wm->tool_settings.draw_changes.erase(op_arg(C, "name"));
        return OpStatus::Finished;
      });
  add("draw.presets_popup", "Pens", "Show the Draw tool's pens at the mouse (Shift Space)", 0, draw_tool, [](Context &C) {
    C.wm->draw_popup_request = true;
    return OpStatus::Finished;
  });
  add("draw.width_adjust", "Adjust Line Width", "Size the line with the mouse (F); click to keep it", 0, stroke_pen,
      [](Context &C) {
        C.wm->draw_adjust_request = 'R';
        return OpStatus::Finished;
      });
  add("draw.opacity_adjust", "Adjust Line Opacity", "Set the line's opacity with the mouse (Shift F); click to keep it",
      0, stroke_pen, [](Context &C) {
        C.wm->draw_adjust_request = 'S';
        return OpStatus::Finished;
      });
}

}  // namespace oanim
