/* The viewport: the rendered document inside its camera frame, Blender-style navigation,
 * the tool bar on the left, tool settings in the header, and the editing tools themselves.
 *
 * Coordinates: document space has the camera frame 2 units tall with y up (as Moho's).
 * Screen space is ImGui pixels with y down. Each layer has its own space, mapped through
 * layer_world_transform(). */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"

#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>

#include "core/log.hh"
#include "anim/animation.hh"
#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "anim/layer_pick.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/detail.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "editors/brush_editor.hh"
#include "editors/editors.hh"
#include "editors/ui.hh"
#include "gpu/scene_renderer.hh"
#include "ops/edit_ops.hh"
#include "ops/media_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/event_system.hh"
#include "wm/file_drop.hh"
#include "wm/keymap.hh"

namespace oanim {

/* Blender's edit-mode colours. */
static constexpr ImU32 COL_VIEWPORT_BACK = IM_COL32(0x3d, 0x3d, 0x3d, 255);
static constexpr ImU32 COL_WIRE = IM_COL32(0x20, 0x20, 0x20, 170);
static constexpr ImU32 COL_WIRE_SELECTED = IM_COL32(0xff, 0x8c, 0x00, 220);
static constexpr ImU32 COL_POINT = IM_COL32(0x10, 0x10, 0x10, 230);
static constexpr ImU32 COL_POINT_SELECTED = IM_COL32(0xff, 0x8c, 0x00, 255);
static constexpr ImU32 COL_HIGHLIGHT = IM_COL32(0x47, 0x72, 0xb3, 255);
/* Handle colours, unselected and selected, from Blender's theme: free handles, and aligned ones
 * (in Blender's aligned pink) so it shows which points keep theirs in line. */
static constexpr ImU32 COL_HANDLE_FREE[2] = {IM_COL32(0x10, 0x10, 0x10, 255), IM_COL32(0xd0, 0x30, 0x30, 255)};
static constexpr ImU32 COL_HANDLE_ALIGNED[2] = {IM_COL32(0x80, 0x30, 0x60, 255), IM_COL32(0xf0, 0x90, 0xa0, 255)};
static constexpr ImU32 COL_HEADER = IM_COL32(0x30, 0x30, 0x30, 235);
static constexpr ImU32 COL_TOOLBAR = IM_COL32(0x28, 0x28, 0x28, 235);

/* Pick radius in screen pixels. */
static constexpr float PICK_PIXELS = 9.0f;
/* A right-click that cancelled something (a move, a turn, a one-off tool) is spent: its release
 * doesn't open a context menu. Cleared once the button is up. */
static bool g_right_click_spent = false;

/* ---------------------------------------------------------------------------------
 * View mapping
 */

struct ViewMapping {
  ImVec2 origin;
  ImVec2 size;
  float2 center;
  float zoom = 1.0f;

  ImVec2 to_screen(float2 p) const
  {
    return {origin.x + size.x * 0.5f + (p.x - center.x) * zoom, origin.y + size.y * 0.5f - (p.y - center.y) * zoom};
  }
  float2 to_document(ImVec2 s) const
  {
    return {center.x + (s.x - origin.x - size.x * 0.5f) / zoom, center.y - (s.y - origin.y - size.y * 0.5f) / zoom};
  }
};

/* The view's zoom range (screen pixels per document unit): the canvas is infinite. */
static constexpr float VIEW_ZOOM_MIN = 1e-3f, VIEW_ZOOM_MAX = 1e7f;

/* The zoom that fits the camera's frame (turned with its roll) in a region of `size`, with a margin. */
static float camera_fit_zoom(const Document &doc, ImVec2 size)
{
  const float2 half = camera_half_size(doc);
  const float roll = doc.camera.rotation * PI / 180.0f;
  const float c = std::fabs(std::cos(roll)), s = std::fabs(std::sin(roll));
  const float hx = half.x * c + half.y * s, hy = half.x * s + half.y * c;
  return std::clamp(std::min(size.x / (2.0f * hx * 1.15f), size.y / (2.0f * hy * 1.15f)), VIEW_ZOOM_MIN, VIEW_ZOOM_MAX);
}

/* Home: the view frames the camera. */
static void view_fit(Area &area, const Document &doc, ImVec2 size)
{
  area.view_center_x = doc.camera.position.x;
  area.view_center_y = doc.camera.position.y;
  area.view_zoom = camera_fit_zoom(doc, size);
  area.view_fit_width = size.x;
  area.view_fit_height = size.y;
}

/* The camera's frame on screen: bottom left, bottom right, top right, top left. */
static void camera_frame_screen(const Document &doc, const ViewMapping &view, ImVec2 r_corners[4])
{
  float2 corners[4];
  camera_frame_corners(doc, corners);
  for (int i = 0; i < 4; i++) {
    r_corners[i] = view.to_screen(corners[i]);
  }
}

/* How far `mouse` is from the camera frame's border, in screen pixels. */
static float camera_border_distance(const Document &doc, const ViewMapping &view, ImVec2 mouse)
{
  ImVec2 q[4];
  camera_frame_screen(doc, view, q);
  float best = FLT_MAX;
  for (int i = 0; i < 4; i++) {
    const float2 a(q[i].x, q[i].y), b(q[(i + 1) % 4].x, q[(i + 1) % 4].y), m(mouse.x, mouse.y);
    const float2 ab = b - a;
    const float t = std::clamp(dot(m - a, ab) / std::max(dot(ab, ab), 1e-12f), 0.0f, 1.0f);
    best = std::min(best, distance(m, a + ab * t));
  }
  return best;
}

/* Darkens the canvas outside the camera's frame (Blender's passepartout): four trapezoids from the
 * frame's edges out to the frame grown far past the region, so they meet without overlapping. */
static void draw_passepartout(ImDrawList *dl, const ImVec2 q[4], ImVec2 origin, ImVec2 size, float strength)
{
  if (strength <= 0.0f) {
    return;
  }
  const ImU32 col = IM_COL32(0, 0, 0, int(std::clamp(strength, 0.0f, 1.0f) * 255.0f + 0.5f));
  const ImVec2 c((q[0].x + q[2].x) * 0.5f, (q[0].y + q[2].y) * 0.5f);
  const auto length = [](ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); };
  const float half = 0.5f * std::min(length(q[1] - q[0]), length(q[3] - q[0]));
  if (half < 1e-3f) {
    dl->AddRectFilled(origin, origin + size, col);
    return;
  }
  const ImVec2 middle = origin + size * 0.5f;
  const float reach = length(middle - c) + length(size);
  const float grow = std::max(2.0f, 2.0f * reach / half + 2.0f);
  /* Without anti-aliasing the trapezoids' shared sides leave no seams. */
  const ImDrawListFlags flags = dl->Flags;
  dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
  for (int i = 0; i < 4; i++) {
    const ImVec2 a = q[i], b = q[(i + 1) % 4];
    const ImVec2 quad[4] = {a, c + (a - c) * grow, c + (b - c) * grow, b};
    dl->AddConvexPolyFilled(quad, 4, col);
  }
  dl->Flags = flags;
}

/* ---------------------------------------------------------------------------------
 * Interaction state. One tool or modal transform runs at a time, so these are shared.
 */

/* A running G R S or E. It works in document space, so points on several layers (each with its
 * own transform) move together. */
/* The signed angle (radians) from `from` to `to` around `pivot`; zero when either is on it. */
static float turn_between(float2 from, float2 to, float2 pivot)
{
  const float2 a = from - pivot, b = to - pivot;
  if (dot(a, a) < 1e-12f || dot(b, b) < 1e-12f) {
    return 0.0f;
  }
  return std::atan2(cross(a, b), dot(a, b));
}

/* Numbers typed while G, R or S runs, as Blender's numeric input: digits and a point type into the
 * field being typed (Move has X and Y: Tab goes to the other), minus flips its sign (before or after
 * the digits), Backspace takes the last character back, Ctrl Backspace clears it. Once anything is
 * typed the mouse stops moving the value; a field left empty counts as 0 (Move) or as the other
 * one (Scale). Read from the keys the transform takes, as text input is off while it runs. */
struct NumInput {
  std::string text[2];
  bool negative[2] = {false, false};
  int field = 0;

  bool any() const { return has(0) || has(1) || negative[0] || negative[1]; }
  bool has(int i) const
  {
    const std::string &t = text[i];
    return !t.empty() && t != ".";
  }
  float value(int i, float fallback) const
  {
    if (!has(i)) {
      return negative[i] ? -fallback : fallback;
    }
    const float v = std::strtof(text[i].c_str(), nullptr);
    return negative[i] ? -v : v;
  }
  /* The field as the header shows it: what's typed, with a caret in the field being typed. */
  std::string shown(int i, float live, const char *format) const
  {
    std::string s;
    if (has(i) || negative[i] || (i == field && any())) {
      s = (negative[i] ? "-" : "") + text[i];
      if (i == field) {
        s = "[" + s + "|]";
      }
      return s;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), format, double(live));
    return buf;
  }
};

/* Takes the typed keys of a running transform into `num`; `fields` is 2 for Move and Scale (Tab
 * switches), 1 otherwise. Returns whether anything changed. */
static bool modal_key_take(Context &C, const char *type, bool any_modifiers = false);
static bool num_input_keys(Context &C, NumInput &num, int fields)
{
  bool changed = false;
  std::vector<ModalKey> &keys = C.wm->modal_keys;
  for (auto it = keys.begin(); it != keys.end();) {
    std::string t = it->type;
    if (t.rfind("KEYPAD_", 0) == 0) {
      t = t.substr(7);
    }
    std::string &text = num.text[num.field];
    bool used = true;
    if (it->ctrl && t == "BACK_SPACE") {
      text.clear();
      num.negative[num.field] = false;
    }
    else if (it->ctrl) {
      used = false;
    }
    else if (t.size() == 1 && t[0] >= '0' && t[0] <= '9') {
      text.push_back(t[0]);
    }
    else if ((t == "." || t == "PERIOD" || t == "NUMPAD_PERIOD" || t == ",") && text.find('.') == std::string::npos) {
      text.push_back('.');
    }
    else if (t == "-" || t == "MINUS") {
      num.negative[num.field] = !num.negative[num.field];
    }
    else if (t == "BACK_SPACE") {
      if (!text.empty()) {
        text.pop_back();
      }
      else {
        num.negative[num.field] = false;
      }
    }
    else if (t == "TAB" && fields > 1) {
      num.field = (num.field + 1) % fields;
    }
    else {
      used = false;
    }
    if (used) {
      changed = true;
      it = keys.erase(it);
    }
    else {
      ++it;
    }
  }
  return changed;
}

struct ModalTransform {
  bool active = false;
  int area = 0;
  char kind = 'G';
  bool extrude = false;
  Document snapshot;
  /* A point, or one handle of a curve point (curve >= 0). A handle picked on its own is
   * `direct` (dragging it may turn its partner, as in Blender); the stored handles of a moved
   * point just come along. */
  struct Item {
    uint64_t layer;
    int point;
    float2 start;
    int curve = -1;
    int index = -1;
    bool pre = false;
    bool direct = false;
    /* A handle's point, in document space: what S and R work about when only handles move. */
    float2 origin;
    /* Shown -> stored (layer space): undoes the bones' move, so a posed point lands where it is
     * dragged. */
    Transform2D unskin;
    /* Symmetry: this point copies item `mirror_source` reflected (across x, y or both). */
    int mirror_source = -1;
    bool mirror_x = false;
    bool mirror_y = false;
    /* Proportional editing: a point (or its handle) dragged along by the selection, `reach` from
     * it, and where it was stored (a handle: its offset from its point), for when it gets none
     * of the move. */
    bool proportional = false;
    float reach = 0.0f;
    float2 rest;
  };
  std::vector<Item> items;
  float2 pivot;
  float2 start_mouse;
  /* Rotate counts whole turns, as Moho's angles do: the mouse's angle around the pivot is added up
   * from frame to frame instead of measured from the start, so circling twice turns 720 degrees. */
  float2 last_mouse;
  float turned = 0.0f;
  /* 0 free, 1 X only, 2 Y only. */
  int axis = 0;
  NumInput num;
  /* Started by dragging a handle: ends when the mouse button is let go. */
  bool tweak = false;
  /* Only handles move: S lengthens and R turns each handle about its own point. */
  bool about_points = false;
  /* Points are dragged along by proportional editing (its circle shows, the wheel sizes it). */
  bool proportional = false;
};

struct ToolDrag {
  bool active = false;
  int area = 0;
  ImVec2 start;
  bool box = false;
  /* Draw tool: the stroke so far, in layer space, with pressures. */
  std::vector<float2> stroke;
  std::vector<float> pressures;
  /* Pen tool: the point following the mouse until release. */
  int pen_point = -1;
  /* Shape tool: where the drag started, in layer space (snapped). */
  float2 shape_start;
};


/* The curve the Pen tool is adding to, and the outline shape that shows it. */
struct PenState {
  uint64_t layer = 0;
  int curve = -1;
  int shape = -1;
};

static ModalTransform g_modal;
/* A G/R/S/E request waiting for a click in progress to finish, and the area it's for. */
static char g_pending_transform = 0;
static int g_pending_area = 0;
static ToolDrag g_drag;
static PenState g_pen;

/* The pen is a mode of the Draw tool. */
static bool pen_active(const ToolSettings &ts)
{
  return ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Pen;
}
static std::map<int, std::unique_ptr<SceneRenderer>> g_renderers;

static void weight_paint_header(Context &C);
static void weight_select_header(Context &C);
static void transform_bones_header(Context &C);
static void transform_points_header(Context &C);

/* A layer's own drawing where its points show: moved by the bones (Blender's "on cage"). Points
 * keep their indices, so what is picked here is the same point in the stored drawing. */
struct Posed {
  const VectorDrawing *drawing = nullptr;
  /* Stored -> shown, per point. */
  std::vector<Transform2D> skins;
  bool moved = false;
  VectorDrawing storage;
};

static Posed posed(const Document &doc, const Layer &layer)
{
  Posed r;
  r.drawing = &layer.drawing;
  r.skins = layer_point_skins(doc, layer);
  r.skins.resize(layer.drawing.points.size());
  for (const Transform2D &t : r.skins) {
    r.moved |= t.a != 1.0f || t.b != 0.0f || t.c != 0.0f || t.d != 1.0f || t.tx != 0.0f || t.ty != 0.0f;
  }
  if (!r.moved) {
    return r;
  }
  r.storage.points = layer.drawing.points;
  r.storage.curves = layer.drawing.curves;
  r.storage.shapes = layer.drawing.shapes;
  for (size_t i = 0; i < r.storage.points.size(); i++) {
    r.storage.points[i].pos = r.skins[i].apply(r.storage.points[i].pos);
  }
  for (Curve &curve : r.storage.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.handle_in = r.skins[cp.point].apply_vector(cp.handle_in);
      cp.handle_out = r.skins[cp.point].apply_vector(cp.handle_out);
    }
  }
  r.drawing = &r.storage;
  return r;
}
static uint64_t pick_bone(const Document &doc, const Layer &armature, const ViewMapping &view, ImVec2 mouse);
static void layer_pick_click(Context &C, const ViewMapping &view, ImVec2 mouse, bool shift, bool ctrl);

/* ---------------------------------------------------------------------------------
 * Helpers
 */

static float2 snap_to_grid(float2 p, float grid)
{
  return {std::round(p.x / grid) * grid, std::round(p.y / grid) * grid};
}

static void deselect_all(VectorDrawing &drawing)
{
  for (MeshPoint &p : drawing.points) {
    p.selected = false;
  }
  drawing_deselect_handles(drawing);
}

/* The handles a viewport shows (and lets you pick): by its Handles overlay, and only while
 * points show. */
static HandleDisplay handles_shown(const ViewportOverlays &ov, const ToolSettings &ts)
{
  /* Weights are about points, not curve shapes: handles would only get in the way there. Edges
   * and Strokes modes pick lines, not points, so their handles go too. */
  if (!ov.show || !ov.points || (ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Freehand) ||
      ts.tool == Tool::WeightPaint || ts.tool == Tool::WeightSelect || ts.select_mode == SelectMode::Edge ||
      ts.select_mode == SelectMode::Stroke)
  {
    return HandleDisplay::None;
  }
  return ov.handles;
}

/* For the Shape handle display: the points of every shape any of whose points (or handles, picked
 * on their own) is selected. */
static const std::vector<bool> &shape_handle_points(const VectorDrawing &drawing)
{
  /* Worked out once per drawing per frame, not for every handle. */
  static const VectorDrawing *cached = nullptr;
  static int cached_frame = -1;
  static std::vector<bool> mask;
  if (cached == &drawing && cached_frame == ImGui::GetFrameCount() && mask.size() == drawing.points.size()) {
    return mask;
  }
  cached = &drawing;
  cached_frame = ImGui::GetFrameCount();
  mask.assign(drawing.points.size(), false);
  for (const Shape &shape : drawing.shapes) {
    bool touched = false;
    for (const ShapeEdge &e : shape.edges) {
      /* The segment's two curve points: a picked handle counts as much as a picked point. */
      const Curve &curve = drawing.curves[e.curve];
      const int n = int(curve.points.size());
      touched |= curve_point_selected(drawing, curve.points[e.segment]) ||
                 curve_point_selected(drawing, curve.points[(e.segment + 1) % n]);
    }
    if (!touched) {
      continue;
    }
    for (const ShapeEdge &e : shape.edges) {
      int a, b;
      curve_segment_points(drawing.curves[e.curve], e.segment, &a, &b);
      mask[a] = mask[b] = true;
    }
  }
  return mask;
}

static bool handle_visible(const VectorDrawing &drawing, const CurvePoint &cp, HandleDisplay display)
{
  /* Detail above its curve's edit level shows nothing to edit. */
  if (drawing.points[cp.point].detail.hidden) {
    return false;
  }
  switch (display) {
    case HandleDisplay::None:
      return false;
    case HandleDisplay::All:
      return true;
    case HandleDisplay::Selected:
      return curve_point_selected(drawing, cp);
    case HandleDisplay::Shape:
      return curve_point_selected(drawing, cp) || shape_handle_points(drawing)[cp.point];
  }
  return false;
}

struct HandleHit {
  Layer *layer = nullptr;
  int curve = -1;
  int index = -1;
  bool pre = false;
  /* Screen pixels from the mouse. */
  float distance = FLT_MAX;
};

/* The shown handle nearest the mouse, within picking distance, on any edited layer. */
static HandleHit pick_handle(Context &C, const ViewMapping &view, ImVec2 mouse, HandleDisplay display)
{
  HandleHit hit;
  if (display == HandleDisplay::None) {
    return hit;
  }
  for (Layer *layer : editable_layers(C)) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const Posed shown = posed(*C.document, *layer);
    const VectorDrawing &drawing = *shown.drawing;
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      const Curve &curve = drawing.curves[c];
      for (int i = 0; i < int(curve.points.size()); i++) {
        if (!handle_visible(drawing, curve.points[i], display)) {
          continue;
        }
        const float2 here = drawing.points[curve.points[i].point].pos;
        for (const bool pre : {true, false}) {
          const float2 h = curve_handle(drawing, curve, i, pre);
          if (distance(h, here) < 1e-9f) {
            continue;
          }
          const ImVec2 s = view.to_screen(to_document.apply(h));
          const float d = std::sqrt((s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y));
          if (d <= PICK_PIXELS && d < hit.distance) {
            hit = {layer, c, i, pre, d};
          }
        }
      }
    }
  }
  return hit;
}

static void push_undo(Context &C, const char *name)
{
  C.undo->push(name, *C.document);
}

/* Ends the pen's current curve, cleaning up anything too small to keep. */
static void pen_finish(Context &C)
{
  if (g_pen.layer) {
    if (Layer *layer = layer_find(*C.document, g_pen.layer)) {
      VectorDrawing &drawing = layer->drawing;
      std::erase_if(drawing.shapes, [](const Shape &shape) { return shape.edges.empty(); });
      std::vector<bool> remove(drawing.curves.size(), false);
      for (size_t c = 0; c < drawing.curves.size(); c++) {
        remove[c] = drawing.curves[c].points.size() < 2;
      }
      drawing_remove_curves(drawing, remove);
      drawing_remove_unused_points(drawing);
    }
  }
  g_pen = {};
}

/* ---------------------------------------------------------------------------------
 * Tool icons, drawn with lines so they scale with the interface.
 */

static void primitive_icon(ImDrawList *dl, PrimitiveKind kind, ImVec2 c, float s, ImU32 col);

/* Icons for the Draw tool's modes: a pencil for freehand, a curve with two points for the pen. */
static void draw_mode_icon(ImDrawList *dl, DrawMode mode, ImVec2 c, float s, ImU32 col)
{
  if (mode == DrawMode::Freehand) {
    const ImVec2 tip = c + ImVec2(-s * 0.75f, s * 0.75f);
    const ImVec2 dir = ImVec2(0.7071f, -0.7071f), side = ImVec2(0.7071f, 0.7071f) * (s * 0.22f);
    const ImVec2 body0 = tip + dir * (s * 0.45f), body1 = tip + dir * (s * 1.9f);
    dl->AddTriangleFilled(tip, body0 + side, body0 - side, col);
    dl->AddQuadFilled(body0 + side, body1 + side, body1 - side, body0 - side, col);
    return;
  }
  const float t = std::max(1.0f, s * 0.12f);
  const ImVec2 p0 = c + ImVec2(-s * 0.8f, s * 0.6f), p1 = c + ImVec2(s * 0.8f, -s * 0.6f);
  dl->AddBezierCubic(p0, c + ImVec2(-s * 0.6f, -s * 0.8f), c + ImVec2(s * 0.6f, s * 0.8f), p1, col, t, 16);
  dl->AddRectFilled(p0 - ImVec2(s * 0.2f, s * 0.2f), p0 + ImVec2(s * 0.2f, s * 0.2f), col);
  dl->AddRectFilled(p1 - ImVec2(s * 0.2f, s * 0.2f), p1 + ImVec2(s * 0.2f, s * 0.2f), col);
}

static void tool_icon(ImDrawList *dl, Tool tool, ImVec2 c, float s, ImU32 col)
{
  const float t = std::max(1.0f, s * 0.12f);
  switch (tool) {
    case Tool::Select: {
      /* Dashed selection box with an arrow. */
      const ImVec2 a = c - ImVec2(s * 0.8f, s * 0.8f), b = c + ImVec2(s * 0.5f, s * 0.5f);
      for (int i = 0; i < 4; i++) {
        const float f0 = i / 4.0f, f1 = f0 + 0.14f;
        dl->AddLine(ImLerp(a, ImVec2(b.x, a.y), f0), ImLerp(a, ImVec2(b.x, a.y), f1), col, t);
        dl->AddLine(ImLerp(a, ImVec2(a.x, b.y), f0), ImLerp(a, ImVec2(a.x, b.y), f1), col, t);
      }
      dl->AddTriangleFilled(c, c + ImVec2(s * 0.9f, s * 0.35f), c + ImVec2(s * 0.35f, s * 0.9f), col);
      break;
    }
    case Tool::SelectLayer: {
      /* Two stacked layers with an arrow on the top one. */
      const auto sheet = [&](float dy, bool filled) {
        const ImVec2 q[4] = {c + ImVec2(-s * 0.85f, dy), c + ImVec2(0, dy - s * 0.42f), c + ImVec2(s * 0.85f, dy),
                             c + ImVec2(0, dy + s * 0.42f)};
        if (filled) {
          dl->AddConvexPolyFilled(q, 4, col);
        }
        else {
          dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, t);
        }
      };
      sheet(s * 0.35f, false);
      sheet(-s * 0.15f, true);
      const ImVec2 tip = c + ImVec2(s * 0.15f, -s * 0.2f);
      dl->AddTriangleFilled(tip, tip + ImVec2(s * 0.75f, s * 0.3f), tip + ImVec2(s * 0.3f, s * 0.75f),
                            IM_COL32(0x20, 0x20, 0x20, 255));
      dl->AddTriangle(tip, tip + ImVec2(s * 0.75f, s * 0.3f), tip + ImVec2(s * 0.3f, s * 0.75f), col, t * 0.8f);
      break;
    }
    case Tool::Cursor:
      dl->AddCircle(c, s * 0.55f, col, 20, t);
      dl->AddLine(c - ImVec2(s * 0.9f, 0), c - ImVec2(s * 0.3f, 0), col, t);
      dl->AddLine(c + ImVec2(s * 0.3f, 0), c + ImVec2(s * 0.9f, 0), col, t);
      dl->AddLine(c - ImVec2(0, s * 0.9f), c - ImVec2(0, s * 0.3f), col, t);
      dl->AddLine(c + ImVec2(0, s * 0.3f), c + ImVec2(0, s * 0.9f), col, t);
      break;
    case Tool::Draw:
      draw_mode_icon(dl, DrawMode::Freehand, c, s, col);
      break;
    case Tool::Shape:
      primitive_icon(dl, PrimitiveKind::Box, c, s, col);
      break;
    case Tool::SplitCurve: {
      /* A line with a new point in the middle. */
      dl->AddLine(c + ImVec2(-s * 0.9f, s * 0.5f), c + ImVec2(s * 0.9f, -s * 0.5f), col, t);
      dl->AddCircleFilled(c, s * 0.28f, col);
      dl->AddLine(c + ImVec2(0, -s * 0.85f), c + ImVec2(0, -s * 0.45f), col, t);
      break;
    }
    case Tool::TransformBones: {
      /* A box with corner handles around a bone. */
      dl->AddRect(c - ImVec2(s * 0.8f, s * 0.8f), c + ImVec2(s * 0.8f, s * 0.8f), col, 0.0f, 0, t * 0.8f);
      for (const ImVec2 k : {ImVec2(-1, -1), ImVec2(1, -1), ImVec2(1, 1), ImVec2(-1, 1)}) {
        dl->AddCircleFilled(c + k * (s * 0.8f), s * 0.2f, col);
      }
      const ImVec2 head = c + ImVec2(-s * 0.45f, s * 0.45f), tail = c + ImVec2(s * 0.45f, -s * 0.45f);
      const ImVec2 side(s * 0.16f, s * 0.16f);
      const ImVec2 quad[4] = {head, head + (tail - head) * 0.25f + side, tail, head + (tail - head) * 0.25f - side};
      dl->AddConvexPolyFilled(quad, 4, col);
      break;
    }
    case Tool::TransformPoints: {
      /* A box with corner handles around three points on a curve. */
      dl->AddRect(c - ImVec2(s * 0.8f, s * 0.8f), c + ImVec2(s * 0.8f, s * 0.8f), col, 0.0f, 0, t * 0.8f);
      for (const ImVec2 k : {ImVec2(-1, -1), ImVec2(1, -1), ImVec2(1, 1), ImVec2(-1, 1)}) {
        dl->AddCircleFilled(c + k * (s * 0.8f), s * 0.2f, col);
      }
      const ImVec2 a = c + ImVec2(-s * 0.45f, s * 0.3f), m = c + ImVec2(0, -s * 0.3f), b = c + ImVec2(s * 0.45f, s * 0.3f);
      dl->AddBezierQuadratic(a, m, b, col, t * 0.8f);
      for (const ImVec2 q : {a, c + ImVec2(0, 0), b}) {
        dl->AddCircleFilled(q, s * 0.16f, col);
      }
      break;
    }
    case Tool::WeightSelect: {
      /* A dashed selection box around blue, green and red points. */
      const ImVec2 a = c - ImVec2(s * 0.85f, s * 0.85f), b = c + ImVec2(s * 0.85f, s * 0.85f);
      for (int i = 0; i < 4; i++) {
        const float f0 = i / 4.0f, f1 = f0 + 0.14f;
        dl->AddLine(ImLerp(a, ImVec2(b.x, a.y), f0), ImLerp(a, ImVec2(b.x, a.y), f1), col, t);
        dl->AddLine(ImLerp(a, ImVec2(a.x, b.y), f0), ImLerp(a, ImVec2(a.x, b.y), f1), col, t);
        dl->AddLine(ImLerp(b, ImVec2(a.x, b.y), f0), ImLerp(b, ImVec2(a.x, b.y), f1), col, t);
        dl->AddLine(ImLerp(b, ImVec2(b.x, a.y), f0), ImLerp(b, ImVec2(b.x, a.y), f1), col, t);
      }
      const ImU32 dots[3] = {IM_COL32(0x30, 0x50, 0xe0, 255), IM_COL32(0x40, 0xd0, 0x40, 255), IM_COL32(0xe0, 0x30, 0x30, 255)};
      for (int i = 0; i < 3; i++) {
        dl->AddCircleFilled(c + ImVec2(-s * 0.45f + s * 0.45f * i, s * (i == 1 ? -0.2f : 0.2f)), s * 0.2f, dots[i]);
      }
      break;
    }
    case Tool::WeightPaint: {
      /* A brush over a blue-to-red gradient. */
      const ImU32 cols[3] = {IM_COL32(0x30, 0x50, 0xe0, 255), IM_COL32(0x40, 0xd0, 0x40, 255), IM_COL32(0xe0, 0x30, 0x30, 255)};
      for (int i = 0; i < 3; i++) {
        dl->AddRectFilled(c + ImVec2(-s * 0.9f + s * 0.6f * i, s * 0.35f), c + ImVec2(-s * 0.3f + s * 0.6f * i, s * 0.85f),
                          cols[i]);
      }
      dl->AddLine(c + ImVec2(-s * 0.4f, s * 0.15f), c + ImVec2(s * 0.7f, -s * 0.9f), col, t * 1.6f);
      dl->AddCircleFilled(c + ImVec2(-s * 0.45f, s * 0.2f), s * 0.22f, col);
      break;
    }
  }
}

/* Icons for the Shape tool's shapes, as in Blender's primitive flyout. */
static void primitive_icon(ImDrawList *dl, PrimitiveKind kind, ImVec2 c, float s, ImU32 col)
{
  const float t = std::max(1.0f, s * 0.12f);
  const auto at = [&](float x, float y) { return c + ImVec2(x * s, y * s); };
  switch (kind) {
    case PrimitiveKind::Box:
      dl->AddRect(at(-0.75f, -0.6f), at(0.75f, 0.6f), col, 0.0f, 0, t);
      break;
    case PrimitiveKind::Circle:
      dl->AddCircle(c, s * 0.75f, col, 24, t);
      break;
  }
}

struct ToolInfo {
  Tool tool;
  const char *name;
  const char *description;
  const char *op_idname;
};

static const ToolInfo TOOLS[] = {
    {Tool::Select, "Select Box", "Click to select, drag to box select. Shift adds, Ctrl removes", "wm.tool_select"},
    {Tool::SelectLayer, "Select Layer",
     "Click the drawing to select its layer (the topmost shown there; click again in the same spot for the one "
     "below). Shift adds or takes out, Ctrl takes out. Media layers are never picked.\n"
     "Alt-click does this from any tool, Alt-Shift-click adds",
     "wm.tool_select_layer"},
    {Tool::Cursor, "Cursor", "Place the 2D cursor", "wm.tool_cursor"},
    {Tool::Draw, "Draw",
     "Draw: freehand strokes; tablet pressure sets the width.\n"
     "Pen: click to add points along a curve, drag to place each; click the first point to close it.\n"
     "Right-click to pick Draw or Pen",
     "wm.tool_draw"},
    {Tool::Shape, "Shape",
     "Drag to draw a box or circle; Shift keeps the sides even, Alt draws from the centre.\n"
     "Right-click to pick the shape",
     "wm.tool_shape"},
    {Tool::SplitCurve, "Split Curve",
     "Click a line to add a point there. Shift: at the middle. Ctrl-click a point: delete it, keeping the curve",
     "wm.tool_split_curve"},
    {Tool::TransformBones, "Transform Bones",
     "A box around the selected bones, as Moho's sz_transform_bones_gizmo: drag inside to move, around it to "
     "turn about the pivot, a handle to scale (Alt: from the pivot, Shift: keep proportions), the pivot to move "
     "it (Ctrl snaps to bone ends). Outside: click or drag a box to select (Shift adds, Alt removes). "
     "Unselected children stay where they are",
     "wm.tool_transform_bones"},
    {Tool::TransformPoints, "Transform Points",
     "A box around the selected points, as Transform Bones has for bones: drag inside to move, around it to turn "
     "about the pivot, a handle to scale (Alt: from the pivot, Shift: keep proportions), the pivot to move it "
     "(Ctrl snaps to a point). Shift while moving keeps to one axis, Ctrl snaps to a point. Outside: click or "
     "drag a box to select (Shift adds, Ctrl removes)",
     "wm.tool_transform_points"},
    {Tool::WeightSelect, "Weight Select",
     "Select points to give weights to: click or drag a box (Shift adds, Ctrl removes). Ctrl-click a bone to "
     "make it active. The header has Automatic Weights (as Blender's), Assign, Remove and Normalize",
     "wm.tool_weight_select"},
    {Tool::WeightPaint, "Weight Paint",
     "Paint how strongly the active bone moves the points, as in Blender: red follows it fully, blue not "
     "at all.\nCtrl paints the weight away. Ctrl-click a bone to paint for it",
     "wm.tool_weight_paint"},
};

/* Tools that change the drawing or the rig, which only happens on the setup frame (0). */
bool tool_setup_only(Tool tool)
{
  switch (tool) {
    case Tool::Draw:
    case Tool::Shape:
    case Tool::SplitCurve:
    case Tool::WeightSelect:
    case Tool::WeightPaint:
      return true;
    default:
      return false;
  }
}

/* Away from frame 0 a setup-only tool hands over to Select, and comes back on frame 0. */
static void tool_follow_frame(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  const bool setup = C.document->frame_current == 0;
  if (!setup && tool_setup_only(ts.tool)) {
    ts.tool_before_posing = ts.tool_temporary ? ts.tool_return : ts.tool;
    ts.tool_switched_for_posing = true;
    ts.tool = Tool::Select;
    ts.tool_temporary = false;
  }
  else if (setup && ts.tool_switched_for_posing) {
    ts.tool = ts.tool_before_posing;
    ts.tool_switched_for_posing = false;
  }
}

/* The tools a layer offers: a bone layer has bone tools, a drawing the drawing tools. */
static bool tool_available(Tool tool, const Layer *active, bool in_bone_layer)
{
  const bool bones = active && active->is_bone_layer();
  if (tool == Tool::WeightPaint || tool == Tool::WeightSelect) {
    /* Weights are for drawings that bones move. */
    return !bones && active && active->has_drawing() && in_bone_layer;
  }
  switch (tool) {
    case Tool::Select:
    case Tool::SelectLayer:
    case Tool::Cursor:
      return true;
    case Tool::Draw:
    case Tool::Shape:
    case Tool::SplitCurve:
      return !bones;
    case Tool::TransformBones:
      return bones;
    case Tool::TransformPoints:
      return !bones;
    case Tool::WeightSelect:
    case Tool::WeightPaint:
      break;
  }
  return true;
}

/* A tool's flyout of variants, opened by right-clicking its tool bar button and closed by
 * right-clicking it again. Call right after the button. Returns the picked item, or -1.
 * `open_last_frame` is the flyout's own: ImGui closes a popup on a click outside it before this
 * runs, so it remembers whether the flyout was open to make the right-click a toggle. */
struct FlyoutItem {
  const char *name;
  std::function<void(ImDrawList *dl, ImVec2 center, float size)> icon;
  bool current;
};

static int tool_flyout(const char *id, bool &open_last_frame, ImVec2 button_pos, float button,
                       const std::vector<FlyoutItem> &items)
{
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !open_last_frame) {
    ImGui::OpenPopup(id);
  }
  ImGui::SetNextWindowPos(button_pos + ImVec2(button + 8.0f, 0.0f), ImGuiCond_Appearing);
  open_last_frame = ImGui::BeginPopup(id);
  if (!open_last_frame) {
    return -1;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  const float h = ImGui::GetFrameHeight();
  const float row = std::round(h * 1.25f);
  const float width = h * 6.0f;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  int picked = -1;
  for (int i = 0; i < int(items.size()); i++) {
    ImGui::PushID(i);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (items[i].current) {
      ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Selectable("##item", items[i].current, 0, ImVec2(width, row))) {
      picked = i;
    }
    if (items[i].current) {
      ImGui::PopStyleColor();
    }
    items[i].icon(dl, p + ImVec2(row * 0.5f, row * 0.5f), row * 0.28f);
    dl->AddText(p + ImVec2(row + 6.0f, (row - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(0xe6, 0xe6, 0xe6, 255),
                items[i].name);
    ImGui::PopID();
  }
  ImGui::EndPopup();
  return picked;
}

static void draw_flyout(Context &C, ImVec2 button_pos, float button)
{
  ToolSettings &ts = C.wm->tool_settings;
  static bool open_last_frame = false;
  const ImU32 col = IM_COL32(0xe6, 0xe6, 0xe6, 255);
  const DrawMode modes[] = {DrawMode::Freehand, DrawMode::Pen};
  std::vector<FlyoutItem> items;
  for (const DrawMode mode : modes) {
    items.push_back({mode == DrawMode::Pen ? "Pen" : "Draw",
                     [mode, col](ImDrawList *dl, ImVec2 c, float s) { draw_mode_icon(dl, mode, c, s, col); },
                     ts.tool == Tool::Draw && ts.draw_mode == mode});
  }
  if (const int picked = tool_flyout("##draw_flyout", open_last_frame, button_pos, button, items); picked >= 0) {
    ts.tool = Tool::Draw;
    ts.draw_mode = modes[picked];
  }
}

static void shape_flyout(Context &C, ImVec2 button_pos, float button)
{
  ToolSettings &ts = C.wm->tool_settings;
  static bool open_last_frame = false;
  const ImU32 col = IM_COL32(0xe6, 0xe6, 0xe6, 255);
  std::vector<FlyoutItem> items;
  for (const PrimitiveKind kind : PRIMITIVE_KINDS) {
    items.push_back({primitive_name(kind),
                     [kind, col](ImDrawList *dl, ImVec2 c, float s) { primitive_icon(dl, kind, c, s, col); },
                     ts.tool == Tool::Shape && ts.shape == kind});
  }
  if (const int picked = tool_flyout("##shape_flyout", open_last_frame, button_pos, button, items); picked >= 0) {
    ts.tool = Tool::Shape;
    ts.shape = PRIMITIVE_KINDS[picked];
  }
}

/* The tool bar down the left side of the viewport. Returns its width. */
static float toolbar_draw(Context &C, ImVec2 origin, float top)
{
  ToolSettings &ts = C.wm->tool_settings;
  const float h = ImGui::GetFrameHeight();
  const float button = std::round(h * 1.35f);
  const float pad = 4.0f;
  const float gap = 10.0f;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const Layer *active_layer = document_active_layer(*C.document);
  const bool in_bone_layer = active_layer && layer_armature(*C.document, active_layer->id) != nullptr;
  const bool setup = C.document->frame_current == 0;
  /* Two sections: tools for every frame, then those for the setup frame (0) only. */
  std::vector<const ToolInfo *> shown;
  int first_setup = -1;
  for (const bool setup_section : {false, true}) {
    for (const ToolInfo &info : TOOLS) {
      if (tool_setup_only(info.tool) == setup_section && tool_available(info.tool, active_layer, in_bone_layer)) {
        if (setup_section && first_setup < 0) {
          first_setup = int(shown.size());
        }
        shown.push_back(&info);
      }
    }
  }
  if (!tool_available(ts.tool, active_layer, in_bone_layer)) {
    /* Switching between a drawing and a bone layer: fall back to Select. */
    ts.tool = Tool::Select;
  }
  const float split = first_setup > 0 ? gap : 0.0f;
  const ImVec2 panel_min = origin + ImVec2(pad, top);
  const ImVec2 panel_max =
      panel_min + ImVec2(button + pad * 2, (button + 2) * float(shown.size()) + pad * 2 + split);
  dl->AddRectFilled(panel_min, panel_max, COL_TOOLBAR, 6.0f);

  for (size_t i = 0; i < shown.size(); i++) {
    const ToolInfo &info = *shown[i];
    const bool in_setup_section = first_setup >= 0 && int(i) >= first_setup;
    const ImVec2 p = panel_min + ImVec2(pad, pad + float(i) * (button + 2) + (in_setup_section ? split : 0.0f));
    if (first_setup > 0 && int(i) == first_setup) {
      const float y = p.y - gap * 0.5f - 1.0f;
      dl->AddLine(ImVec2(p.x + 3.0f, y), ImVec2(p.x + button - 3.0f, y), IM_COL32(0x60, 0x60, 0x60, 255), 1.0f);
    }
    const bool usable = !tool_setup_only(info.tool) || setup;
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(int(i));
    if (ImGui::InvisibleButton("##tool", ImVec2(button, button)) && usable) {
      ts.tool = info.tool;
      ts.tool_switched_for_posing = false;
    }
    const bool hovered = ImGui::IsItemHovered() && usable;
    const bool active = ts.tool == info.tool;
    if (active || hovered) {
      dl->AddRectFilled(p, p + ImVec2(button, button), active ? COL_HIGHLIGHT : IM_COL32(0x45, 0x45, 0x45, 255),
                        4.0f);
    }
    const ImU32 icon_col = usable ? IM_COL32(0xe6, 0xe6, 0xe6, 255) : IM_COL32(0xe6, 0xe6, 0xe6, 70);
    const ImVec2 center = p + ImVec2(button, button) * 0.5f;
    const bool has_flyout = info.tool == Tool::Draw || info.tool == Tool::Shape;
    if (info.tool == Tool::Draw) {
      draw_mode_icon(dl, ts.draw_mode, center, button * 0.3f, icon_col);
      draw_flyout(C, p, button);
    }
    else if (info.tool == Tool::Shape) {
      primitive_icon(dl, ts.shape, center, button * 0.3f, icon_col);
      shape_flyout(C, p, button);
    }
    else {
      tool_icon(dl, info.tool, center, button * 0.3f, icon_col);
    }
    if (has_flyout) {
      /* Blender's corner mark for a tool with a flyout. */
      const ImVec2 corner = p + ImVec2(button - 3.0f, button - 3.0f);
      const float m = std::round(button * 0.16f);
      dl->AddTriangleFilled(corner, corner - ImVec2(m, 0), corner - ImVec2(0, m), IM_COL32(0xb0, 0xb0, 0xb0, 255));
    }
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      if (!usable) {
        ImGui::SetTooltip("%s\nOnly available on frame 0", info.name);
      }
      else {
        const std::string shortcut = keymaps().shortcut_for(info.op_idname);
        ImGui::SetTooltip("%s\n%s%s%s", info.name, info.description, shortcut.empty() ? "" : "\nShortcut: ",
                          shortcut.c_str());
      }
    }
    ImGui::PopID();
  }
  return panel_max.x - origin.x;
}

/* Colours for a toggle button that is on: blue, a little lighter under the mouse and when
 * pressed, as Blender draws them, instead of ImGui's grey hover. Pops three colours. */
static void toggle_on_colors_push()
{
  const ImVec4 blue = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
  const auto lighter = [&](float f) {
    return ImVec4(std::min(blue.x * f, 1.0f), std::min(blue.y * f, 1.0f), std::min(blue.z * f, 1.0f), blue.w);
  };
  ImGui::PushStyleColor(ImGuiCol_Button, blue);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lighter(1.15f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, lighter(1.3f));
}

/* Blender's overlays icon: two overlapping circles. */
static void overlays_icon(ImDrawList *dl, ImVec2 center, float size, ImU32 color)
{
  const float r = size * 0.24f;
  dl->AddCircle(center - ImVec2(r * 0.55f, 0), r, color, 0, 1.4f);
  dl->AddCircle(center + ImVec2(r * 0.55f, 0), r, color, 0, 1.4f);
}

/* Two columns of checkboxes, as in Blender's overlay popover. */
static void overlay_checkbox_pair(const char *a, bool *va, const char *b, bool *vb, float column)
{
  ImGui::Checkbox(a, va);
  if (b) {
    ImGui::SameLine(column);
    ImGui::Checkbox(b, vb);
  }
}

static bool overlay_section(const char *title)
{
  ImGui::Spacing();
  const std::string key = std::string("Viewport Overlays/") + title;
  ui_fold_next(key, true);
  const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
  ui_fold_record(key, true, open);
  return open;
}

/* The overlays toggle and the arrow that opens their settings, like Blender's. */
static void overlays_buttons(ViewportOverlays &ov, ToolSettings &ts, float h, float arrow_w)
{
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetStyle().FrameRounding);
  /* Pop by the state the colour was pushed for: the click below may change it. */
  const bool highlighted = ov.show;
  if (highlighted) {
    toggle_on_colors_push();
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (ImGui::Button("##overlays", ImVec2(h, h))) {
    ov.show = !ov.show;
  }
  if (highlighted) {
    ImGui::PopStyleColor(3);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Show Overlays\nShow or hide everything drawn over the artwork");
  }
  overlays_icon(ImGui::GetWindowDrawList(), at + ImVec2(h * 0.5f, h * 0.5f), h, IM_COL32(0xe6, 0xe6, 0xe6, 255));
  ImGui::SameLine(0.0f, 1.0f);
  if (ImGui::ArrowButtonEx("##overlays_menu", ImGuiDir_Down, ImVec2(arrow_w, h))) {
    ImGui::OpenPopup("##overlays_popover");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Viewport Overlays");
  }
  ImGui::PopStyleVar();

  ImGui::SetNextWindowPos(ImVec2(at.x + h + arrow_w, at.y + h + 2.0f), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
  if (!ImGui::BeginPopup("##overlays_popover")) {
    return;
  }
  /* Esc closes it, as in Blender (ImGui only does that with keyboard navigation on). */
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  const float column = h * 7.0f;
  ImGui::TextUnformatted("Viewport Overlays");
  if (overlay_section("Guides")) {
    overlay_checkbox_pair("Grid", &ov.grid, "2D Cursor", &ov.cursor, column);
    overlay_checkbox_pair("Camera Frame", &ov.camera_frame, nullptr, nullptr, column);
    ImGui::SetNextItemWidth(column * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    ImGui::DragFloat("##grid_scale", &ts.grid_size, 0.005f, 0.01f, 1.0f, "Scale  %.3f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Grid spacing in document units, also used when snapping to the grid");
    }
  }
  if (overlay_section("Text")) {
    overlay_checkbox_pair("General Info", &ov.info, nullptr, nullptr, column);
  }
  if (overlay_section("Drawing")) {
    overlay_checkbox_pair("Wireframe", &ov.wireframe, "Points", &ov.points, column);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Handles");
    ImGui::SameLine(column);
    ImGui::SetNextItemWidth(column - ImGui::GetStyle().ItemSpacing.x);
    static const char *const handle_modes[] = {"None", "Selected", "Shape", "All"};
    int mode = int(ov.handles);
    if (ImGui::Combo("##handles", &mode, handle_modes, 4)) {
      ov.handles = HandleDisplay(mode);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Which curve handles to show: none, the selected points', every handle of the shapes you're "
                        "working on (any of whose points is selected), or all of them");
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Mask Outlines", &ov.mask_outlines);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Outline the active layer's mask layers, dashed and labelled (\"inside head\").\n"
                        "Off: they only show while a mask's symbol or Layers field is under the mouse.");
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Guides", &ov.guides);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show guides: layers with Show in Renders off (references, Media layers).\n"
                        "Off, or with the overlays off (Shift Z), the viewport shows what renders.");
    }
  }
  if (overlay_section("Armature")) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bones");
    ImGui::SameLine(column);
    ImGui::SetNextItemWidth(column - ImGui::GetStyle().ItemSpacing.x);
    static const char *const armature_modes[] = {"Off", "Selected", "On"};
    int mode = int(ov.armature);
    if (ImGui::Combo("##armature", &mode, armature_modes, 3)) {
      ov.armature = ArmatureDisplay(mode);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Which bones to show: none, those of selected bone layers, or every bone layer's");
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Relationship Lines", &ov.relationship_lines);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("A dashed line from each bone that isn't connected to its parent's tip");
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Physics Ghosts", &ov.physics_ghosts);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Draw bones where their keys put them, so you pose exactly what you key, and pale ghosts\n"
                        "where bone dynamics swing them (the artwork follows the ghosts).\n"
                        "Off: bones are drawn where the dynamics have them.");
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Live Physics", &ov.live_physics);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Run bone dynamics live while you pose on a frame: dynamic bones trail, swing and\n"
                        "settle in real time as you drag or turn them, until the frame changes.\n"
                        "Off: they show the frame's own simulated result (lighter on the computer).");
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Custom Bone Shapes", &ov.bone_shapes);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Draw bones with a custom shape (Bone > Shape) as that shape, such as a pin.\n"
                        "Off: every bone is drawn as a bone.");
    }
  }
  ImGui::EndPopup();
}

/* A small toggle button for the header, highlighted when on. */
static bool header_toggle(const char *label, bool on, const char *tooltip)
{
  if (on) {
    toggle_on_colors_push();
  }
  const bool pressed = ImGui::Button(label);
  if (on) {
    ImGui::PopStyleColor(3);
  }
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return pressed;
}

/* The selected shapes' detail level (data/detail.hh): `r_level` -1 when they differ, `r_top` the
 * highest level among them. False when no shape is selected. */
static bool selected_shapes_detail(Context &C, int *r_level, int *r_top)
{
  bool any = false;
  *r_level = 0;
  *r_top = 0;
  for (Layer *layer : editable_layers(C)) {
    if (layer->is_reference()) {
      continue;
    }
    const VectorDrawing &drawing = layer->drawing;
    const std::vector<bool> selected = drawing_selected_shapes(drawing);
    for (size_t s = 0; s < drawing.shapes.size(); s++) {
      if (!selected[s]) {
        continue;
      }
      const Shape &shape = drawing.shapes[s];
      if (!any) {
        *r_level = shape.detail_level;
      }
      else if (*r_level != shape.detail_level) {
        *r_level = -1;
      }
      *r_top = std::max({*r_top, shape_detail_max_level(drawing, shape), shape.detail_level});
      any = true;
    }
  }
  return any;
}

/* Detail ‹ 1 / 3 ›: the selected shapes' detail level, stepped with the arrows, dragged or typed. */
static void detail_header(Context &C)
{
  int level = 0, top = 0;
  if (!selected_shapes_detail(C, &level, &top)) {
    return;
  }
  ImGui::SameLine(0, 14);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Detail");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("The selected shapes' detail level: their curves show their points up to it, and the knife "
                      "(K) adds detail points at it. One past the highest is an empty level to add detail at "
                      "(Ctrl 0 to Ctrl 4, Alt Page Up / Page Down)");
  }
  ImGui::SameLine(0, 4);
  if (ImGui::ArrowButton("##detail_down", ImGuiDir_Left)) {
    operator_call(C, "mesh.detail_level_set", {{"delta", "-1"}});
  }
  ImGui::SameLine(0, 2);
  int value = std::max(level, 0);
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 1.6f);
  if (ImGui::DragInt("##detail_level", &value, 0.05f, 0, DETAIL_LEVEL_MAX, level < 0 ? "-" : "%d",
                     ImGuiSliderFlags_AlwaysClamp) &&
      value != level)
  {
    operator_call(C, "mesh.detail_level_set", {{"level", std::to_string(value)}});
  }
  ImGui::SameLine(0, 4);
  ImGui::Text("/ %d", top);
  ImGui::SameLine(0, 4);
  if (ImGui::ArrowButton("##detail_up", ImGuiDir_Right)) {
    operator_call(C, "mesh.detail_level_set", {{"delta", "1"}});
  }
}

static bool color_swatch(const char *id, Color &color, const char *tooltip)
{
  float rgba[4] = {color.r, color.g, color.b, color.a};
  const bool changed = ui_color_edit(id, rgba, false);
  if (changed) {
    color = {rgba[0], rgba[1], rgba[2], rgba[3]};
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return changed;
}

/* Line width field in pixels of the document's height, which is easier to judge than units. */
static void width_field(const Document &doc, ToolSettings &ts)
{
  const float units_to_pixels = float(doc.height) * 0.5f;
  float pixels = ts.style.line_width * units_to_pixels;
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 4.0f);
  if (ImGui::DragFloat("##width", &pixels, 0.1f, 0.0f, 500.0f, "Width %.1f px", ImGuiSliderFlags_AlwaysClamp)) {
    ts.style.line_width = pixels / units_to_pixels;
  }
}

static bool menu_operator(Context &C, const char *label, const char *idname)
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return false;
  }
  const std::string shortcut = keymaps().shortcut_for(idname);
  if (ui_menu_item(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, operator_poll(C, *ot))) {
    operator_call(C, idname);
    return true;
  }
  return false;
}

/* The header: editor menu, Select and Drawing menus, the active tool's settings, snapping. */
/* A falloff's curve, as the icons in Blender's menu: how much moves, across the reach and back. */
static void falloff_glyph(ImDrawList *dl, ImVec2 min, float s, ProportionalFalloff falloff, ImU32 col)
{
  ImVec2 pts[25];
  const float pad = s * 0.18f;
  for (int i = 0; i < 25; i++) {
    const float x = float(i) / 12.0f - 1.0f;
    const float w = proportional_weight(falloff, std::fabs(x), 1.0f, i * 7 + 3);
    pts[i] = ImVec2(min.x + pad + (x + 1.0f) * 0.5f * (s - 2.0f * pad), min.y + s - pad - w * (s - 2.0f * pad));
  }
  dl->AddPolyline(pts, 25, col, ImDrawFlags_None, 1.2f);
}

/* Proportional editing in the header, as Blender's: a toggle (O), and a dropdown showing its
 * falloff, with Connected Only, the falloffs and the size. Only for points. */
static void proportional_buttons(ToolSettings &ts, float h, float arrow_w)
{
  const ImU32 ink = IM_COL32(0xe6, 0xe6, 0xe6, 255);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool on = ts.proportional;
  if (on) {
    toggle_on_colors_push();
  }
  if (ImGui::Button("##proportional", ImVec2(h, h))) {
    ts.proportional = !ts.proportional;
  }
  if (on) {
    ImGui::PopStyleColor(3);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Proportional Editing (O)\nMoving points drags the points around them too, less the\n"
                      "further they are. While moving, the wheel or Page Up / Page Down sizes its reach");
  }
  /* Its icon: a ring about a dot. */
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 c = at + ImVec2(h * 0.5f, h * 0.5f);
  dl->AddCircle(c, h * 0.3f, ink, 0, 1.5f);
  dl->AddCircleFilled(c, h * 0.11f, ink);
  ImGui::SameLine(0.0f, 1.0f);
  const ImVec2 menu_at = ImGui::GetCursorScreenPos();
  if (ImGui::Button("##proportional_menu", ImVec2(h + arrow_w * 0.6f, h))) {
    ImGui::OpenPopup("##proportional_popover");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Proportional Editing Falloff: %s", proportional_falloff_name(ts.proportional_falloff));
  }
  falloff_glyph(dl, menu_at, h, ts.proportional_falloff, ink);
  const ImVec2 tip = menu_at + ImVec2(h + arrow_w * 0.3f, h * 0.5f);
  dl->AddTriangleFilled(tip + ImVec2(-3.0f, -1.5f), tip + ImVec2(3.0f, -1.5f), tip + ImVec2(0.0f, 2.0f), ink);
  ImGui::PopStyleVar();

  ImGui::SetNextWindowPos(ImVec2(menu_at.x, menu_at.y + h + 2.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopup("##proportional_popover")) {
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::Checkbox("Connected Only", &ts.proportional_connected);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Only drag points joined to the selection along their curves, measured along them");
  }
  ImGui::Spacing();
  const float row_h = ImGui::GetFrameHeight();
  const float row_w = std::max(ImGui::CalcTextSize("Inverse Square").x + row_h + 24.0f, 170.0f);
  for (int i = 0; i < PROPORTIONAL_FALLOFF_COUNT; i++) {
    const ProportionalFalloff f = ProportionalFalloff(i);
    const ImVec2 row = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    if (ImGui::Selectable("##falloff", ts.proportional_falloff == f, 0, ImVec2(row_w, row_h))) {
      ts.proportional_falloff = f;
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopID();
    ImDrawList *pdl = ImGui::GetWindowDrawList();
    falloff_glyph(pdl, row, row_h, f, ink);
    pdl->AddText(row + ImVec2(row_h + 6.0f, (row_h - ImGui::GetTextLineHeight()) * 0.5f), ink,
                 proportional_falloff_name(f));
  }
  ImGui::Spacing();
  ImGui::SetNextItemWidth(row_w);
  ImGui::DragFloat("##proportional_size", &ts.proportional_size, 0.01f, 1e-4f, 1e6f, "Proportional Size  %.3f",
                   ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How far from the selection points are dragged along (document units)");
  }
  ImGui::EndPopup();
}

static void header_draw(Area &area, Context &C, ImVec2 origin, float width)
{
  Document &doc = *C.document;
  ToolSettings &ts = C.wm->tool_settings;
  const float h = ImGui::GetFrameHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(origin, origin + ImVec2(width, h + 8.0f), COL_HEADER);
  ImGui::SetCursorScreenPos(origin + ImVec2(0.0f, 4.0f));
  area_editor_type_menu(area);

  ImGui::SameLine();
  if (header_menu("Select")) {
    menu_operator(C, "All", "mesh.select_all");
    menu_operator(C, "None", "mesh.select_none");
    menu_operator(C, "Invert", "mesh.select_invert");
    ImGui::Separator();
    menu_operator(C, "Points", "mesh.select_mode_point");
    menu_operator(C, "Edges", "mesh.select_mode_edge");
    menu_operator(C, "Strokes", "mesh.select_mode_stroke");
    ImGui::EndPopup();
  }
  /* A bone layer's header has Blender's Armature menu in place of the Drawing one. */
  const Layer *header_layer = document_active_layer(doc);
  if (header_layer && header_layer->is_bone_layer()) {
    ImGui::SameLine();
    if (header_menu("Armature")) {
      menu_operator(C, "Add Bone", "armature.bone_primitive_add");
      menu_operator(C, "Extrude", "armature.extrude");
      menu_operator(C, "Duplicate", "armature.duplicate_move");
      menu_operator(C, "Subdivide", "armature.subdivide");
      ImGui::Separator();
      menu_operator(C, "Make Parent...", "armature.parent_set_menu");
      menu_operator(C, "Clear Parent...", "armature.parent_clear_menu");
      ImGui::Separator();
      menu_operator(C, "Symmetrize", "armature.symmetrize");
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Make the other side of the selected bones (all when none are selected) from their "
                          "_L / _R names, mirrored across the bone layer's vertical axis. Setup frame only");
      }
      ImGui::Separator();
      menu_operator(C, "Delete Bones", "armature.bone_delete");
      ImGui::EndPopup();
    }
  }
  else {
    ImGui::SameLine();
    if (header_menu("Drawing")) {
      if (ImGui::BeginMenu("Arrange")) {
        menu_operator(C, "Bring to Front", "mesh.shape_raise_to_front");
        menu_operator(C, "Bring Forward", "mesh.shape_raise");
        menu_operator(C, "Send Backward", "mesh.shape_lower");
        menu_operator(C, "Send to Back", "mesh.shape_lower_to_back");
        ImGui::EndMenu();
      }
      ImGui::Separator();
      menu_operator(C, "Align Handles", "mesh.handles_align");
      menu_operator(C, "Unalign Handles", "mesh.handles_unalign");
      menu_operator(C, "Peak", "mesh.peak");
      menu_operator(C, "Smooth", "mesh.smooth");
      ImGui::EndPopup();
    }
  }

  ImGui::SameLine(0.0f, h);
  /* On a bone layer: Auto IK, for posing by a bone's tip. */
  if (const Layer *bones = document_active_layer(doc); bones && bones->is_bone_layer() &&
                                                       (ts.tool == Tool::Select || ts.tool == Tool::TransformBones))
  {
    if (header_toggle("Auto IK", ts.auto_ik,
                      "Posing a bone by its tip turns its parents too, so the tip reaches the mouse (Moho's IK). "
                      "The chain stops at a bone Ignored by IK")) {
      ts.auto_ik = !ts.auto_ik;
    }
    ImGui::SameLine();
  }
  /* Points, Edges and Strokes only mean something with points to pick (not on a bone layer with
   * no layers editable in it, nor an empty drawing). */
  bool editable_points = false;
  for (const Layer *editing : editable_layers(C)) {
    editable_points = editable_points || !editing->drawing.points.empty();
  }
  /* A copy (reference layer): its points are locked or editable, the lock beside the select modes. */
  if (Layer *active = document_active_layer(doc); active && active->is_reference() && !active->is_group()) {
    const bool locked = !active->reference.edit_points;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (!locked) {
      toggle_on_colors_push();
    }
    if (ImGui::Button("##points_lock", ImVec2(h, h))) {
      operator_call(C, "layer.reference_set", {{"edit_points", locked ? "1" : "0"}});
    }
    if (!locked) {
      ImGui::PopStyleColor(3);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(locked ? "This copy's points are locked: they stay where the original has them (only looks\n"
                                 "change here). Click to unlock them."
                               : "This copy's points are editable: moves are kept on top of the original's.\n"
                                 "Click to lock them.");
    }
    /* A padlock, open when editable. */
    ImDrawList *hdl = ImGui::GetWindowDrawList();
    const ImVec2 c = at + ImVec2(h * 0.5f, h * 0.55f);
    const float s = h * 0.28f;
    const ImU32 col = IM_COL32(0xe6, 0xe6, 0xe6, 255);
    hdl->AddRectFilled(c + ImVec2(-s * 0.75f, -s * 0.05f), c + ImVec2(s * 0.75f, s * 0.95f), col, 1.5f);
    /* The shackle: both legs in the body when locked; open, lifted to the right. */
    const float shift = locked ? 0.0f : s * 0.55f, r = s * 0.5f, top = -s * 0.05f, mid = -s * 0.5f;
    hdl->PathLineTo(c + ImVec2(shift - r, locked ? top : mid));
    hdl->PathArcTo(c + ImVec2(shift, mid), r, PI, 2.0f * PI, 10);
    hdl->PathLineTo(c + ImVec2(shift + r, locked ? top : mid + s * 0.2f));
    hdl->PathStroke(col, 0, 1.6f);
    ImGui::SameLine(0, 4);
  }
  switch (ts.tool) {
    case Tool::SelectLayer:
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Pick");
      ImGui::SameLine();
      if (header_toggle("Layer", !ts.layer_pick_group, "Pick the layer clicked")) {
        ts.layer_pick_group = false;
      }
      ImGui::SameLine(0, 1);
      if (header_toggle("Group", ts.layer_pick_group,
                        "Pick the outermost group holding the layer clicked (to move a whole character)")) {
        ts.layer_pick_group = true;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("Click: select  |  again: the one below  |  Shift: add  |  Ctrl: remove  |  Alt-click from any tool");
      break;
    case Tool::Select:
      if (!editable_points) {
        break;
      }
      if (header_toggle("Points", ts.select_mode == SelectMode::Point, "Select points (1)")) {
        operator_call(C, "mesh.select_mode_point");
      }
      ImGui::SameLine(0, 1);
      if (header_toggle("Edges", ts.select_mode == SelectMode::Edge,
                        "Select edges: clicking a line selects the segment between two points (2)"))
      {
        operator_call(C, "mesh.select_mode_edge");
      }
      ImGui::SameLine(0, 1);
      if (header_toggle("Strokes", ts.select_mode == SelectMode::Stroke, "Select whole strokes (3)")) {
        operator_call(C, "mesh.select_mode_stroke");
      }
      break;
    case Tool::Draw:
    case Tool::Shape:
      ImGui::TextUnformatted(ts.tool == Tool::Shape         ? primitive_name(ts.shape)
                             : ts.draw_mode == DrawMode::Pen ? "Pen"
                                                             : "Draw");
      ImGui::SameLine();
      color_swatch("##line_color", ts.style.line, "Line colour for new strokes");
      ImGui::SameLine();
      width_field(doc, ts);
      ImGui::SameLine();
      color_swatch("##fill_color", ts.style.fill, "Fill colour for new shapes");
      ImGui::SameLine();
      {
        /* The brush new strokes get, edited as the Material panel edits a shape's. */
        const std::string &name = ts.style.brush.name;
        if (ImGui::Button(((name.empty() ? std::string("Solid") : name) + "##tool_brush").c_str())) {
          ImGui::OpenPopup("##tool_brush_popup");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("Brush for new strokes: presets, shape, spacing");
        }
        if (ImGui::BeginPopup("##tool_brush_popup")) {
          if (ImGui::BeginTable("##tool_brush", 2, ImGuiTableFlags_SizingStretchProp, ImVec2(440.0f, 0))) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            BrushSettings brush = ts.style.brush;
            if (brush_editor(C, brush, ts.style, BrushTarget::Tool).changed) {
              ts.style.brush = brush;
            }
            ImGui::EndTable();
          }
          ImGui::EndPopup();
        }
      }
      ImGui::SameLine();
      if (ts.tool == Tool::Shape) {
        if (header_toggle("Fill", ts.draw_fill, "Fill boxes and circles")) {
          ts.draw_fill = !ts.draw_fill;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Drag to draw  |  Shift: even  |  Alt: from centre  |  Ctrl: grid");
      }
      else if (ts.draw_mode == DrawMode::Pen) {
        if (header_toggle("Auto Fill", ts.draw_fill, "Fill curves closed on their first point")) {
          ts.draw_fill = !ts.draw_fill;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Click to add points, drag to place  |  Click the first point to close  |  Ctrl: grid");
      }
      else {
        if (header_toggle("Auto Fill", ts.draw_fill, "Fill strokes that end where they started")) {
          ts.draw_fill = !ts.draw_fill;
        }
        ImGui::SameLine();
        if (header_toggle("Pressure", ts.use_pressure, "Tablet pressure sets the line width")) {
          ts.use_pressure = !ts.use_pressure;
        }
      }
      break;
    case Tool::SplitCurve:
      ImGui::TextDisabled(ts.tool_temporary
                              ? "Split once (K): click a line, then back to your tool  |  Shift: middle  |  Esc: cancel"
                              : "Click a line to add a point  |  Shift: middle  |  Ctrl-click a point: delete it");
      break;
    case Tool::Cursor:
      ImGui::TextDisabled("Click to place the cursor  |  Shift right-click places it from any tool");
      break;
    case Tool::WeightSelect:
      weight_select_header(C);
      break;
    case Tool::WeightPaint:
      weight_paint_header(C);
      break;
    case Tool::TransformBones:
      transform_bones_header(C);
      break;
    case Tool::TransformPoints:
      transform_points_header(C);
      break;
  }
  if (editable_points) {
    detail_header(C);
  }

  /* On the right, as in Blender: snapping, then the overlays toggle and its dropdown. */
  const ImGuiStyle &style = ImGui::GetStyle();
  const float snap_w = ImGui::CalcTextSize("Snap").x + style.FramePadding.x * 2.0f;
  const float arrow_w = h * 0.75f;
  const float axis_w = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
  const float mirror_w = h + 2.0f + axis_w * 2.0f + 1.0f + h * 0.6f;
  /* Proportional editing, with points to edit. */
  const float proportional_w = editable_points ? h + 1.0f + h + arrow_w * 0.6f + h * 0.6f : 0.0f;
  const float right_w = mirror_w + snap_w + proportional_w + h * 0.6f + h + 1.0f + arrow_w;
  ImGui::SameLine();
  const float right = origin.x + width - right_w - 6.0f;
  if (ImGui::GetCursorScreenPos().x < right) {
    ImGui::SetCursorScreenPos(ImVec2(right, ImGui::GetCursorScreenPos().y));
  }
  /* Blender's mesh symmetry: a butterfly, then X and Y. */
  {
    const ImVec2 c = ImGui::GetCursorScreenPos() + ImVec2(h * 0.5f, h * 0.5f);
    ImDrawList *hdl = ImGui::GetWindowDrawList();
    const ImU32 col = IM_COL32(0xd0, 0xd0, 0xd0, 255);
    const float s = h * 0.3f;
    for (const float side : {-1.0f, 1.0f}) {
      hdl->AddTriangleFilled(c + ImVec2(side * s * 0.12f, -s * 0.15f), c + ImVec2(side * s * 1.05f, -s * 0.95f),
                             c + ImVec2(side * s * 0.95f, s * 0.1f), col);
      hdl->AddTriangleFilled(c + ImVec2(side * s * 0.12f, s * 0.05f), c + ImVec2(side * s * 0.85f, s * 0.25f),
                             c + ImVec2(side * s * 0.5f, s * 0.9f), col);
    }
    hdl->AddLine(c + ImVec2(0, -s * 0.6f), c + ImVec2(0, s * 0.7f), col, 1.5f);
    ImGui::Dummy(ImVec2(h, h));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Mirror editing: moving a point also moves its mirror image across the layer's axes");
    }
  }
  ImGui::SameLine(0.0f, 2.0f);
  if (header_toggle("X", ts.mirror_x, "Mirror X\nEdits mirror across the layer's vertical axis")) {
    ts.mirror_x = !ts.mirror_x;
  }
  ImGui::SameLine(0.0f, 1.0f);
  if (header_toggle("Y", ts.mirror_y, "Mirror Y\nEdits mirror across the layer's horizontal axis")) {
    ts.mirror_y = !ts.mirror_y;
  }
  ImGui::SameLine(0.0f, h * 0.6f);
  if (header_toggle("Snap", ts.snap_grid || ts.snap_points, "Snapping options")) {
    ImGui::OpenPopup("##snapping");
  }
  if (ImGui::BeginPopup("##snapping")) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("Snap To");
    ImGui::Checkbox("Grid", &ts.snap_grid);
    ImGui::Checkbox("Points", &ts.snap_points);
    ImGui::EndPopup();
  }
  if (editable_points) {
    ImGui::SameLine(0.0f, h * 0.6f);
    proportional_buttons(ts, h, arrow_w);
  }
  ImGui::SameLine(0.0f, h * 0.6f);
  overlays_buttons(C.document->overlays, ts, h, arrow_w);
}

/* ---------------------------------------------------------------------------------
 * Modal transform (G, R, S, E)
 */

/* The points and handles a transform moves, gathered into `modal` (as modal_start does, without
 * starting the modal: the Transform Points gizmo uses it too). False when nothing can move; then the
 * document is as it was. */
static bool modal_collect(Context &C, Area &area, char kind, float2 mouse_document, ModalTransform &modal);

static bool modal_start(Context &C, Area &area, char kind, float2 mouse_document)
{
  ModalTransform modal;
  if (!modal_collect(C, area, kind, mouse_document, modal)) {
    return false;
  }
  modal.active = true;
  g_modal = std::move(modal);
  C.wm->modal_running = true;
  C.wm->modal_keys.clear();
  return true;
}

static bool modal_collect(Context &C, Area &area, char kind, float2 mouse_document, ModalTransform &modal)
{
  /* A reference's points move only when unlocked (Points: Editable), and never get new ones (Extrude). */
  std::vector<Layer *> layers;
  bool held = false, held_picked = false;
  for (Layer *layer : editable_layers(C)) {
    if (layer->is_reference() && (!layer->reference.edit_points || kind == 'E')) {
      held = true;
      /* Some of its points (or handles) are selected: they can't come along. */
      const VectorDrawing &d = layer->drawing;
      held_picked |= std::any_of(d.points.begin(), d.points.end(), [](const MeshPoint &p) { return p.selected; });
      for (const Curve &curve : d.curves) {
        held_picked |= std::any_of(curve.points.begin(), curve.points.end(),
                                   [](const CurvePoint &cp) { return cp.select_in || cp.select_out; });
      }
      continue;
    }
    layers.push_back(layer);
  }
  /* Said whenever a copy's selected points stay put, even while other layers' points move. */
  if (held_picked || (layers.empty() && held)) {
    if (kind == 'E') {
      report_banner(ReportType::Warning, "A copy (reference layer) can't get new points: Break Reference to change its drawing");
    }
    else {
      report_banner(ReportType::Warning, "This copy's points are locked: unlock them in the Reference tab",
                      "Go to the Reference tab in Properties to unlock", "Reference layer points locked");
    }
  }
  if (layers.empty()) {
    return false;
  }
  modal.snapshot = *C.document;
  modal.area = area.id;
  modal.kind = kind == 'E' ? 'G' : kind;
  modal.extrude = kind == 'E';

  if (modal.extrude) {
    /* A new point beyond each selected curve end; they become the selection. */
    bool any = false;
    for (Layer *layer : layers) {
      VectorDrawing &drawing = layer->drawing;
      std::vector<int> new_points;
      for (int i = 0; i < int(drawing.points.size()); i++) {
        if (!drawing.points[i].selected) {
          continue;
        }
        CurveEnd end = drawing_curve_end_at(drawing, i);
        if (end.curve < 0) {
          continue;
        }
        if (!end.at_end) {
          drawing_reverse_curve(drawing, end.curve);
        }
        const int added = drawing_add_point(drawing, drawing.points[i].pos);
        Curve &curve = drawing.curves[end.curve];
        curve.points.push_back({added});
        /* Shapes that cover the whole curve grow with it. */
        const int new_segment = curve.segment_count() - 1;
        for (Shape &shape : drawing.shapes) {
          int on_curve = 0;
          for (const ShapeEdge &e : shape.edges) {
            on_curve += e.curve == end.curve;
          }
          if (on_curve == new_segment && !shape.has_fill) {
            shape.edges.push_back({end.curve, new_segment});
          }
        }
        new_points.push_back(added);
      }
      deselect_all(drawing);
      for (int p : new_points) {
        drawing.points[p].selected = true;
      }
      any |= !new_points.empty();
    }
    if (!any) {
      *C.document = modal.snapshot;
      report_banner(ReportType::Warning, "Extrude works from the ends of open curves");
      return false;
    }
  }

  float2 sum;
  int counted = 0;
  for (Layer *layer : layers) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    VectorDrawing &drawing = layer->drawing;
    const Posed shown = posed(*C.document, *layer);
    for (int i = 0; i < int(drawing.points.size()); i++) {
      if (drawing.points[i].selected) {
        const float2 p = to_document.apply(shown.drawing->points[i].pos);
        ModalTransform::Item item{layer->id, i, p};
        item.unskin = shown.skins[i].inverted();
        modal.items.push_back(item);
        sum += p;
        counted++;
      }
    }
    /* Symmetry: each moving point's mirror image (a point at the reflected spot in the setup pose,
     * across the layer's axes, so the pairs hold on every frame) moves with it, reflected. */
    const ToolSettings &ts = C.wm->tool_settings;
    if ((ts.mirror_x || ts.mirror_y) && !modal.extrude) {
      const int moving_end = int(modal.items.size());
      for (int k = 0; k < moving_end; k++) {
        const ModalTransform::Item source = modal.items[k];
        if (source.layer != layer->id || source.curve >= 0) {
          continue;
        }
        for (const MirrorPair &m : drawing_mirror_pairs(drawing, {source.point}, ts.mirror_x, ts.mirror_y)) {
          ModalTransform::Item item{layer->id, m.dst, to_document.apply(shown.drawing->points[m.dst].pos)};
          item.unskin = shown.skins[m.dst].inverted();
          item.mirror_source = k;
          item.mirror_x = m.x;
          item.mirror_y = m.y;
          modal.items.push_back(item);
        }
      }
    }
    /* Proportional editing: every other point the selection can reach comes along, by how far
     * it is (how much is worked out as it moves, as the size can change). Not for Extrude. */
    std::vector<float> reach;
    if (ts.proportional && !modal.extrude) {
      std::vector<float2> places;
      for (const MeshPoint &shown_point : shown.drawing->points) {
        places.push_back(to_document.apply(shown_point.pos));
      }
      reach = drawing_proportional_distances(drawing, places, ts.proportional_connected);
      std::vector<bool> taken(drawing.points.size(), false);
      for (const ModalTransform::Item &item : modal.items) {
        if (item.layer == layer->id && item.curve < 0) {
          taken[item.point] = true;
        }
      }
      for (int i = 0; i < int(drawing.points.size()); i++) {
        if (taken[i] || drawing.points[i].selected || drawing.points[i].detail.hidden || !std::isfinite(reach[i])) {
          continue;
        }
        ModalTransform::Item item{layer->id, i, places[i]};
        item.unskin = shown.skins[i].inverted();
        item.proportional = true;
        item.reach = reach[i];
        item.rest = drawing.points[i].pos;
        modal.items.push_back(item);
        modal.proportional = true;
      }
    }
    const auto dragged_along = [&](int point) {
      return !reach.empty() && !drawing.points[point].selected && !drawing.points[point].detail.hidden &&
             std::isfinite(reach[point]);
    };
    /* Handles after the points, so they're placed once their points have moved. */
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      Curve &curve = drawing.curves[c];
      for (int i = 0; i < int(curve.points.size()); i++) {
        CurvePoint &cp = curve.points[i];
        const bool along = dragged_along(cp.point);
        const bool point_moves = drawing.points[cp.point].selected || along;
        /* Every handle is stored: a moved point's handles turn and scale with it. */
        const bool stored = true;
        for (const bool pre : {true, false}) {
          const bool direct = !point_moves && (pre ? cp.select_in : cp.select_out);
          if (!direct && !(point_moves && stored)) {
            continue;
          }
          const float2 h = to_document.apply(curve_handle(*shown.drawing, shown.drawing->curves[c], i, pre));
          ModalTransform::Item item{layer->id, cp.point, h};
          item.unskin = shown.skins[cp.point].inverted();
          item.curve = c;
          item.index = i;
          item.pre = pre;
          item.direct = direct;
          item.origin = to_document.apply(shown.drawing->points[cp.point].pos);
          if (along) {
            item.proportional = true;
            item.reach = reach[cp.point];
            item.rest = pre ? cp.handle_in : cp.handle_out;
          }
          modal.items.push_back(item);
          if (direct) {
            sum += h;
            counted++;
          }
        }
      }
    }
  }
  if (counted == 0) {
    *C.document = modal.snapshot;
    return false;
  }
  modal.pivot = sum / float(counted);
  /* Handles alone: the mouse measures turns and lengths about their points. */
  modal.about_points = std::none_of(modal.items.begin(), modal.items.end(),
                                    [](const ModalTransform::Item &item) { return item.curve < 0; });
  if (modal.about_points) {
    float2 origins;
    for (const ModalTransform::Item &item : modal.items) {
      origins += item.origin;
    }
    modal.pivot = origins / float(modal.items.size());
  }
  modal.start_mouse = mouse_document;
  modal.last_mouse = mouse_document;
  return true;
}

static void modal_end(Context &C, bool confirm)
{
  if (confirm) {
    const char *name = g_modal.extrude ? "Extrude" : g_modal.tweak ? "Move Handle" : (g_modal.kind == 'R' ? "Rotate" : g_modal.kind == 'S' ? "Scale" : "Move");
    push_undo(C, name);
  }
  else {
    *C.document = g_modal.snapshot;
  }
  g_modal.active = false;
  g_modal.snapshot = Document();
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
}

/* Takes a key pressed since the last frame, without modifiers unless `any_modifiers`. */
static bool modal_key_take(Context &C, const char *type, bool any_modifiers)
{
  std::vector<ModalKey> &keys = C.wm->modal_keys;
  for (auto it = keys.begin(); it != keys.end(); ++it) {
    if (it->type == type && (any_modifiers || (!it->ctrl && !it->shift && !it->alt))) {
      keys.erase(it);
      return true;
    }
  }
  return false;
}

/* Each moved item's layer, and its way from document space into that layer. False when one is gone. */
using ModalLayers = std::map<uint64_t, std::pair<Layer *, Transform2D>>;
static bool modal_layers(Context &C, const ModalTransform &modal, ModalLayers &r_layers)
{
  for (const ModalTransform::Item &item : modal.items) {
    if (!r_layers.count(item.layer)) {
      Layer *layer = layer_find(*C.document, item.layer);
      if (!layer || item.point >= int(layer->drawing.points.size())) {
        return false;
      }
      r_layers[item.layer] = {layer, layer_world_transform(*C.document, item.layer).inverted()};
    }
  }
  return true;
}

/* Places one moved point or handle at a document-space position. Points come first in the list, so
 * a handle is placed relative to where its point now is. */
static void modal_place(ModalLayers &layers, const ModalTransform::Item &item, float2 p)
{
  if (item.mirror_source >= 0) {
    return; /* Placed from its source afterwards. */
  }
  const auto &[layer, to_layer] = layers[item.layer];
  VectorDrawing &drawing = layer->drawing;
  const float2 stored = item.unskin.apply(to_layer.apply(p));
  if (item.curve < 0) {
    drawing.points[item.point].pos = stored;
    return;
  }
  if (item.curve >= int(drawing.curves.size()) || item.index >= int(drawing.curves[item.curve].points.size())) {
    return;
  }
  Curve &curve = drawing.curves[item.curve];
  CurvePoint &cp = curve.points[item.index];
  if (item.direct) {
    /* An aligned point's other handle turns to stay in line, unless it is moving too. Alt moves
     * just this one, which breaks the line: the point is no longer aligned, so later moves don't
     * snap the other handle back. */
    const bool partner_moves = item.pre ? cp.select_out : cp.select_in;
    const bool alone = ImGui::GetIO().KeyAlt;
    curve_point_move_handle(drawing, curve, item.index, item.pre, stored, partner_moves || alone);
    if (alone && !partner_moves) {
      cp.aligned = false;
    }
  }
  else {
    (item.pre ? cp.handle_in : cp.handle_out) = stored - drawing.points[cp.point].pos;
  }
}

/* How much of the move an item gets: all, or by proportional editing's falloff. */
static float modal_weight(const ToolSettings &ts, const ModalTransform::Item &item)
{
  if (!item.proportional) {
    return 1.0f;
  }
  return ts.proportional ? proportional_weight(ts.proportional_falloff, item.reach, ts.proportional_size, item.point)
                         : 0.0f;
}

/* An item that gets none of the move goes back exactly where it was. */
static void modal_rest(ModalLayers &layers, const ModalTransform::Item &item)
{
  VectorDrawing &drawing = layers[item.layer].first->drawing;
  if (item.curve < 0) {
    drawing.points[item.point].pos = item.rest;
  }
  else if (item.curve < int(drawing.curves.size()) && item.index < int(drawing.curves[item.curve].points.size())) {
    CurvePoint &cp = drawing.curves[item.curve].points[item.index];
    (item.pre ? cp.handle_in : cp.handle_out) = item.rest;
  }
}

/* Mirrored points snap to their sources, reflected (in the layer's stored space), on any frame;
 * handles too; and handles dragged on their own: their mirror images follow. */
static void modal_mirror(Context &C, const ModalTransform &modal, ModalLayers &layers)
{
  for (const ModalTransform::Item &item : modal.items) {
    if (item.mirror_source < 0) {
      continue;
    }
    const ModalTransform::Item &source = modal.items[item.mirror_source];
    drawing_apply_mirror(layers[item.layer].first->drawing, {{source.point, item.point, item.mirror_x, item.mirror_y}});
  }
  if (C.wm->tool_settings.mirror_x || C.wm->tool_settings.mirror_y) {
    for (const ModalTransform::Item &item : modal.items) {
      if (item.curve < 0 || !item.direct) {
        continue;
      }
      VectorDrawing &drawing = layers[item.layer].first->drawing;
      for (const auto &[x, y] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
        if ((x && !C.wm->tool_settings.mirror_x) || (y && !C.wm->tool_settings.mirror_y)) {
          continue;
        }
        const int dst = drawing_find_mirror_point(drawing, item.point, x, y);
        if (dst >= 0) {
          drawing_mirror_handles(drawing, item.point, dst, x, y);
        }
      }
    }
  }
}

/* Runs the transform for this frame; returns a line describing it for the header. */
static std::string modal_update(Context &C, const ViewMapping &view)
{
  ImGuiIO &io = ImGui::GetIO();
  ModalLayers layers;
  if (!modal_layers(C, g_modal, layers)) {
    modal_end(C, false);
    return {};
  }
  const auto place = [&](const ModalTransform::Item &item, float2 p) { modal_place(layers, item, p); };
  ToolSettings &ts = C.wm->tool_settings;
  const auto weight = [&](const ModalTransform::Item &item) { return modal_weight(ts, item); };
  const auto rest = [&](const ModalTransform::Item &item) { modal_rest(layers, item); };
  /* Page Up and Page Down size proportional editing's reach (the wheel does too). */
  if (g_modal.proportional) {
    while (modal_key_take(C, "PAGE_UP", true)) {
      ts.proportional_size = std::min(ts.proportional_size * 1.1f, 1e6f);
    }
    while (modal_key_take(C, "PAGE_DOWN", true)) {
      ts.proportional_size = std::max(ts.proportional_size / 1.1f, 1e-4f);
    }
  }

  /* Keys while transforming, as in Blender. */
  if (modal_key_take(C, "X")) {
    g_modal.axis = g_modal.axis == 1 ? 0 : 1;
  }
  if (modal_key_take(C, "Y")) {
    g_modal.axis = g_modal.axis == 2 ? 0 : 2;
  }
  num_input_keys(C, g_modal.num, g_modal.kind == 'R' || g_modal.axis != 0 ? 1 : 2);
  if (modal_key_take(C, "ESC", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    modal_end(C, false);
    return {};
  }
  const bool confirm = modal_key_take(C, "RET", true) || modal_key_take(C, "KEYPAD_ENTER", true) ||
                       modal_key_take(C, "SPACE", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                       (g_modal.tweak && !ImGui::IsMouseDown(ImGuiMouseButton_Left));

  /* Hold still while the mouse position is unknown rather than jump to nowhere. */
  if (!ImGui::IsMousePosValid()) {
    if (confirm) {
      modal_end(C, true);
    }
    return "";
  }
  const float2 mouse = view.to_document(io.MousePos);
  const bool snap = io.KeyCtrl || C.wm->tool_settings.snap_grid;
  const NumInput &num = g_modal.num;
  if (g_modal.axis != 0) {
    /* Along one axis there's one field. */
    g_modal.num.field = 0;
  }
  const char *axis_label = g_modal.axis == 1 ? "  along X" : (g_modal.axis == 2 ? "  along Y" : "");

  std::string info;
  char text[128];
  if (g_modal.kind == 'G') {
    float2 delta = mouse - g_modal.start_mouse;
    if (snap) {
      delta = snap_to_grid(delta, C.wm->tool_settings.grid_size);
    }
    if (num.any()) {
      delta = g_modal.axis == 2 ? float2(0.0f, num.value(0, 0.0f))
              : g_modal.axis == 1 ? float2(num.value(0, 0.0f), 0.0f)
                                  : float2(num.value(0, 0.0f), num.value(1, 0.0f));
    }
    if (g_modal.axis == 1) {
      delta.y = 0.0f;
    }
    else if (g_modal.axis == 2) {
      delta.x = 0.0f;
    }
    for (const ModalTransform::Item &item : g_modal.items) {
      const float w = weight(item);
      w > 0.0f ? place(item, item.start + delta * w) : rest(item);
    }
    const std::string sx = g_modal.axis == 2 ? "" : num.shown(0, delta.x, "%.3f");
    const std::string sy = g_modal.axis == 1 ? "" : num.shown(g_modal.axis == 2 ? 0 : 1, delta.y, "%.3f");
    std::snprintf(text, sizeof(text), "%s  %s%s%s%s%s%s", g_modal.extrude ? "Extrude" : "Move",
                  g_modal.axis == 2 ? "" : "X: ", sx.c_str(), g_modal.axis == 0 ? "  " : "",
                  g_modal.axis == 1 ? "" : "Y: ", sy.c_str(), axis_label);
  }
  else if (g_modal.kind == 'R') {
    g_modal.turned += turn_between(g_modal.last_mouse, mouse, g_modal.pivot);
    g_modal.last_mouse = mouse;
    float degrees = g_modal.turned * 180.0f / PI;
    if (snap) {
      degrees = std::round(degrees / 5.0f) * 5.0f;
    }
    if (num.any()) {
      degrees = num.value(0, 0.0f);
    }
    const float angle = degrees * PI / 180.0f;
    for (const ModalTransform::Item &item : g_modal.items) {
      const float2 c = g_modal.about_points ? item.origin : g_modal.pivot;
      const float w = weight(item);
      w > 0.0f ? place(item, c + rotate(item.start - c, angle * w)) : rest(item);
    }
    std::snprintf(text, sizeof(text), "Rotate  %s\xC2\xB0", num.shown(0, degrees, "%.1f").c_str());
  }
  else {
    const float start = std::max(distance(g_modal.start_mouse, g_modal.pivot), 1e-6f);
    float factor = distance(mouse, g_modal.pivot) / start;
    if (snap) {
      factor = std::round(factor * 10.0f) / 10.0f;
    }
    float2 f(g_modal.axis == 2 ? 1.0f : factor, g_modal.axis == 1 ? 1.0f : factor);
    if (num.any()) {
      /* Typed: one number scales every way (or along the axis); with Y typed too, each its own. */
      const float x = num.value(0, num.has(1) ? 1.0f : 1.0f);
      factor = x;
      f = g_modal.axis == 1 ? float2(x, 1.0f)
          : g_modal.axis == 2 ? float2(1.0f, x)
          : num.has(1) || num.negative[1] ? float2(num.has(0) ? x : 1.0f, num.value(1, 1.0f))
                                          : float2(x, x);
    }
    for (const ModalTransform::Item &item : g_modal.items) {
      const float2 c = g_modal.about_points ? item.origin : g_modal.pivot;
      const float2 d = item.start - c;
      const float w = weight(item);
      if (w <= 0.0f) {
        rest(item);
        continue;
      }
      place(item, c + float2(d.x * (1.0f + (f.x - 1.0f) * w), d.y * (1.0f + (f.y - 1.0f) * w)));
    }
    if (g_modal.axis == 0 && (num.field == 1 || num.has(1))) {
      std::snprintf(text, sizeof(text), "Scale  X: %s  Y: %s", num.shown(0, f.x, "%.3f").c_str(),
                    num.shown(1, f.y, "%.3f").c_str());
    }
    else {
      std::snprintf(text, sizeof(text), "Scale  %s%s", num.shown(0, factor, "%.3f").c_str(), axis_label);
    }
  }
  modal_mirror(C, g_modal, layers);
  info = text;
  if (g_modal.proportional && ts.proportional) {
    char size_text[64];
    std::snprintf(size_text, sizeof(size_text), "   Proportional size: %.3f", ts.proportional_size);
    info += size_text;
  }
  if (confirm) {
    /* Keys that came in with the confirming click (a quick click then E) were meant for after
     * the transform, so they go to the keymap once it has ended. */
    const std::vector<ModalKey> after = std::move(C.wm->modal_keys);
    modal_end(C, true);
    for (const ModalKey &key : after) {
      wm_handle_key_event(C, KeyEvent{key.type, key.ctrl, key.shift, key.alt});
    }
  }
  else {
    /* Other keys pressed while transforming do nothing, as in Blender. */
    C.wm->modal_keys.clear();
  }
  return info;
}

/* ---------------------------------------------------------------------------------
 * Tools
 */

/* --- Shape tool ------------------------------------------------------------------- */

/* Box and circle corners snap to the grid (Snap > Grid, or Ctrl). */
static float2 shape_snap(Context &C, float2 p)
{
  const ToolSettings &ts = C.wm->tool_settings;
  if (ts.snap_grid || ImGui::GetIO().KeyCtrl) {
    return snap_to_grid(p, ts.grid_size);
  }
  return p;
}

/* A drag's corners: Shift keeps a box square and a circle round; Alt grows them from where
 * the drag started. */
static std::vector<float2> shape_drag_points(PrimitiveKind kind, float2 start, float2 end)
{
  const ImGuiIO &io = ImGui::GetIO();
  const bool boxlike = kind == PrimitiveKind::Box || kind == PrimitiveKind::Circle;
  float2 d = end - start;
  if (io.KeyShift && boxlike) {
    const float m = std::max(std::abs(d.x), std::abs(d.y));
    d = float2(std::copysign(m, d.x), std::copysign(m, d.y));
  }
  if (io.KeyAlt && boxlike) {
    return {start - d, start + d};
  }
  return {start, start + d};
}

/* Draws a shape that isn't added yet, in its style. */
static void shape_preview(ImDrawList *dl, const VectorDrawing &shape, const Transform2D &to_document,
                          const ViewMapping &view, float line_px, int quality)
{
  std::vector<float2> outline;
  std::vector<ImVec2> screen;
  for (const Shape &s : shape.shapes) {
    bool closed = false;
    outline.clear();
    shape_outline(shape, s, quality, outline, nullptr, &closed);
    screen.clear();
    for (const float2 &p : outline) {
      screen.push_back(view.to_screen(to_document.apply(p)));
    }
    if (screen.size() < 2) {
      continue;
    }
    const auto u32 = [](const Color &c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); };
    if (s.has_fill && closed && screen.size() >= 3) {
      dl->AddConcavePolyFilled(screen.data(), int(screen.size()), u32(s.style.fill));
    }
    dl->AddPolyline(screen.data(), int(screen.size()), u32(s.style.line), closed ? ImDrawFlags_Closed : ImDrawFlags_None,
                    std::max(1.0f, line_px));
  }
}

static void shape_commit(Context &C, VectorDrawing &drawing, PrimitiveKind kind, const std::vector<float2> &points)
{
  const ToolSettings &ts = C.wm->tool_settings;
  const VectorDrawing shape = primitive_drawing(kind, points, ts.style, ts.draw_fill);
  if (shape.curves.empty()) {
    return;
  }
  drawing_append_selected(drawing, shape);
  push_undo(C, primitive_name(kind));
}

/* What the Split Curve tool would cut: the nearest curve (or, with `point`, the nearest point)
 * under the mouse on any layer being edited. */
struct SplitHit {
  Layer *layer = nullptr;
  Transform2D to_document;
  ShapeEdge edge;
  float t = 0.0f;
  int point = -1;
};

static SplitHit split_pick(Context &C, const ViewMapping &view, float2 mouse_document, bool point)
{
  SplitHit hit;
  float best = FLT_MAX;
  for (Layer *layer : editable_layers(C)) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
    const float2 mouse = to_document.inverted().apply(mouse_document);
    /* Lines are cut where they show: detail's handles final. */
    VectorDrawing final_shape;
    const VectorDrawing *shape_ptr = &layer->drawing;
    if (drawing_has_detail(layer->drawing)) {
      final_shape = layer->drawing;
      drawing_detail_final_handles(final_shape);
      shape_ptr = &final_shape;
    }
    const VectorDrawing &drawing = *shape_ptr;
    float d = FLT_MAX;
    SplitHit candidate;
    if (point) {
      candidate.point = drawing_pick_point(drawing, mouse, PICK_PIXELS * pixel);
      if (candidate.point >= 0) {
        d = distance(drawing.points[candidate.point].pos, mouse) / pixel;
      }
    }
    else if (drawing_pick_segment(drawing, mouse, PICK_PIXELS * pixel, &candidate.edge, &candidate.t)) {
      float2 ctrl[4];
      curve_segment_bezier(drawing, drawing.curves[candidate.edge.curve], candidate.edge.segment, ctrl);
      d = distance(bezier_eval(ctrl, candidate.t), mouse) / pixel;
    }
    if (d < best) {
      best = d;
      candidate.layer = layer;
      candidate.to_document = to_document;
      hit = candidate;
    }
  }
  return hit;
}

/* After a split made with K, the tool K came from returns. */
static void split_curve_done(ToolSettings &ts)
{
  if (ts.tool_temporary) {
    ts.tool = ts.tool_return;
    ts.tool_temporary = false;
  }
}

/* Picks the nearest point (or, in Edges and Strokes mode, the line) under the mouse across every
 * editable layer, and selects it as Blender does: a plain click replaces the selection, Shift
 * toggles, Ctrl removes. */
static void select_click(Context &C, const ViewMapping &view, float2 mouse_document, HandleDisplay handles)
{
  const ImGuiIO &io = ImGui::GetIO();
  const std::vector<Layer *> layers = editable_layers(C);
  Layer *hit_layer = nullptr;
  std::vector<int> picked;
  ShapeEdge hit_edge{-1, 0};
  float best = FLT_MAX;
  const HandleHit handle = pick_handle(C, view, view.to_screen(mouse_document), handles);
  Layer &camera = document_camera(*C.document);
  for (Layer *layer : layers) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
    const float2 mouse = to_document.inverted().apply(mouse_document);
    const Posed shown = posed(*C.document, *layer);
    if (C.wm->tool_settings.select_mode == SelectMode::Edge) {
      /* Points don't show in Edges mode, so clicks pick lines (below). */
      continue;
    }
    if (const int p = drawing_pick_point(*shown.drawing, mouse, PICK_PIXELS * pixel); p >= 0) {
      const float d = distance(shown.drawing->points[p].pos, mouse) / pixel;
      if (d < best) {
        best = d;
        hit_layer = layer;
        picked = {p};
      }
    }
  }
  /* A handle nearer than any point takes the click: just that handle is selected. */
  if (handle.layer && handle.distance <= best) {
    if (!io.KeyShift && !io.KeyCtrl) {
      for (Layer *layer : layers) {
        deselect_all(layer->drawing);
      }
    }
    CurvePoint &cp = handle.layer->drawing.curves[handle.curve].points[handle.index];
    bool &flag = handle.pre ? cp.select_in : cp.select_out;
    flag = io.KeyCtrl ? false : (io.KeyShift ? !flag : true);
    camera.selected &= io.KeyShift || io.KeyCtrl;
    if (flag) {
      document_set_active_shape(*C.document, *handle.layer, cp.point);
    }
    return;
  }
  if (!hit_layer && C.wm->tool_settings.select_mode != SelectMode::Point) {
    /* A click on a line takes that edge (its two points) in Edges mode, and the whole line in
     * Strokes mode. In Points mode only points are picked: a click on a line selects nothing. */
    for (Layer *layer : layers) {
      const Transform2D to_document = layer_world_transform(*C.document, layer->id);
      const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
      ShapeEdge edge;
      float t;
      /* Where the lines show: posed by bones, and a reference matching its original where that is; at
       * the shape's detail level, as its wires are drawn. */
      const Posed shown = posed(*C.document, *layer);
      const VectorDrawing lines = drawing_detail_shown_curves(*shown.drawing);
      if (drawing_pick_segment(lines, to_document.inverted().apply(mouse_document), PICK_PIXELS * pixel, &edge, &t)) {
        int a, b;
        curve_segment_points(lines.curves[edge.curve], edge.segment, &a, &b);
        hit_layer = layer;
        picked = C.wm->tool_settings.select_mode == SelectMode::Edge ? std::vector<int>{a, b} : std::vector<int>{a};
        hit_edge = edge;
        break;
      }
    }
  }
  if (!io.KeyShift && !io.KeyCtrl) {
    for (Layer *layer : layers) {
      deselect_all(layer->drawing);
    }
  }
  if (hit_layer) {
    for (int p : picked) {
      MeshPoint &point = hit_layer->drawing.points[p];
      point.selected = io.KeyCtrl ? false : (io.KeyShift && picked.size() == 1 ? !point.selected : true);
    }
    drawing_expand_selection_by_mode(C, hit_layer->drawing);
    /* The shape clicked becomes the active one (its panels show it), unless the click took it out. */
    if (!picked.empty() && hit_layer->drawing.points[size_t(picked.front())].selected) {
      document_set_active_shape(*C.document, *hit_layer, picked.front(), hit_edge);
    }
  }
  /* The camera is picked by its frame's border where nothing else is, as Blender picks a camera by its
   * outline; a click elsewhere lets it go. */
  const ViewportOverlays &ov = C.document->overlays;
  if (!hit_layer && ov.show && ov.camera_frame &&
      camera_border_distance(*C.document, view, view.to_screen(mouse_document)) <= PICK_PIXELS)
  {
    camera.selected = io.KeyCtrl ? false : (io.KeyShift ? !camera.selected : true);
  }
  else if (!io.KeyShift && !io.KeyCtrl) {
    camera.selected = false;
  }
}

static void select_box(Context &C, const ViewMapping &view, ImVec2 a, ImVec2 b, HandleDisplay handles)
{
  const ImGuiIO &io = ImGui::GetIO();
  const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
  for (Layer *layer : editable_layers(C)) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    if (!io.KeyShift && !io.KeyCtrl) {
      deselect_all(layer->drawing);
    }
    const auto inside = [&](float2 p) {
      const ImVec2 s = view.to_screen(to_document.apply(p));
      return s.x >= lo.x && s.x <= hi.x && s.y >= lo.y && s.y <= hi.y;
    };
    VectorDrawing &drawing = layer->drawing;
    const Posed shown = posed(*C.document, *layer);
    /* Handles shown before the box (whose points were selected then) can be boxed too. */
    std::vector<std::pair<CurvePoint *, int>> boxed;
    for (size_t c = 0; c < drawing.curves.size(); c++) {
      Curve &curve = drawing.curves[c];
      for (int i = 0; i < int(curve.points.size()); i++) {
        CurvePoint &cp = curve.points[i];
        if (handle_visible(drawing, cp, handles)) {
          const float2 here = shown.drawing->points[cp.point].pos;
          for (const bool pre : {true, false}) {
            const float2 h = curve_handle(*shown.drawing, shown.drawing->curves[c], i, pre);
            if (distance(h, here) > 1e-9f && inside(h)) {
              boxed.push_back({&cp, pre ? 0 : 1});
            }
          }
        }
      }
    }
    for (size_t i = 0; i < drawing.points.size(); i++) {
      if (inside(shown.drawing->points[i].pos) && !drawing.points[i].detail.hidden) {
        drawing.points[i].selected = !io.KeyCtrl;
      }
    }
    for (const auto &[cp, side] : boxed) {
      (side == 0 ? cp->select_in : cp->select_out) = !io.KeyCtrl;
    }
    drawing_expand_selection_by_mode(C, drawing);
  }
}

/* Douglas-Peucker: keeps the points that matter for the stroke's shape. */
static void simplify(const std::vector<float2> &pts, size_t first, size_t last, float tolerance,
                     std::vector<bool> &keep)
{
  if (last <= first + 1) {
    return;
  }
  float max_dist = 0.0f;
  size_t index = first;
  for (size_t i = first + 1; i < last; i++) {
    const float d = distance_to_segment(pts[i], pts[first], pts[last]);
    if (d > max_dist) {
      max_dist = d;
      index = i;
    }
  }
  if (max_dist > tolerance) {
    keep[index] = true;
    simplify(pts, first, index, tolerance, keep);
    simplify(pts, index, last, tolerance, keep);
  }
}

static void draw_tool_finish(Context &C, VectorDrawing &drawing, float pixel)
{
  ToolSettings &ts = C.wm->tool_settings;
  std::vector<float2> &stroke = g_drag.stroke;
  if (stroke.size() < 2) {
    return;
  }
  std::vector<bool> keep(stroke.size(), false);
  keep.front() = keep.back() = true;
  simplify(stroke, 0, stroke.size() - 1, pixel * 1.5f, keep);

  const bool closes = ts.draw_fill && stroke.size() > 8 && distance(stroke.front(), stroke.back()) < pixel * 14.0f;
  Curve curve;
  deselect_all(drawing);
  for (size_t i = 0; i < stroke.size(); i++) {
    if (!keep[i] || (closes && i == stroke.size() - 1)) {
      continue;
    }
    const int p = drawing_add_point(drawing, stroke[i]);
    if (ts.use_pressure) {
      drawing.points[p].radius = std::clamp(g_drag.pressures[i], 0.15f, 1.0f);
    }
    curve.points.push_back({p});
  }
  if (curve.points.size() < 2) {
    return;
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
  push_undo(C, "Draw");
}

static void pen_click(Context &C, Layer &layer, float2 mouse, float radius)
{
  ToolSettings &ts = C.wm->tool_settings;
  VectorDrawing &drawing = layer.drawing;
  if (g_pen.layer != layer.id) {
    pen_finish(C);
  }
  /* Undo or opening a file can take away the curve in progress. */
  if (g_pen.curve >= int(drawing.curves.size()) || g_pen.shape >= int(drawing.shapes.size())) {
    g_pen = {};
  }
  const int snap = ts.snap_points ? drawing_pick_point(drawing, mouse, radius) : -1;
  const float2 pos = snap >= 0 ? drawing.points[snap].pos
                               : (ts.snap_grid || ImGui::GetIO().KeyCtrl ? snap_to_grid(mouse, ts.grid_size) : mouse);

  /* Continue the curve in progress, or one whose end is the only selected point. */
  if (g_pen.curve < 0) {
    int selected = -1, count = 0;
    for (int i = 0; i < int(drawing.points.size()); i++) {
      if (drawing.points[i].selected) {
        selected = i;
        count++;
      }
    }
    const CurveEnd end = count == 1 ? drawing_curve_end_at(drawing, selected) : CurveEnd();
    if (end.curve >= 0) {
      if (!end.at_end) {
        drawing_reverse_curve(drawing, end.curve);
      }
      g_pen = {layer.id, end.curve, -1};
      /* Reuse an outline that covers the whole curve, so the new segment joins it. */
      for (size_t s = 0; s < drawing.shapes.size(); s++) {
        const Shape &shape = drawing.shapes[s];
        const bool all_on_curve = std::all_of(shape.edges.begin(), shape.edges.end(),
                                              [&](const ShapeEdge &e) { return e.curve == end.curve; });
        if (all_on_curve && int(shape.edges.size()) == drawing.curves[end.curve].segment_count()) {
          g_pen.shape = int(s);
        }
      }
    }
  }

  if (g_pen.curve >= 0) {
    Curve &curve = drawing.curves[g_pen.curve];
    if (snap >= 0 && snap == curve.points.front().point && curve.points.size() >= 3) {
      /* Clicking the first point closes the curve; both ends re-smooth across the join. */
      curve.closed = true;
      curve.points.front().handles_set = false;
      curve.points.back().handles_set = false;
      if (g_pen.shape >= 0) {
        Shape &shape = drawing.shapes[g_pen.shape];
        shape.edges.push_back({g_pen.curve, curve.segment_count() - 1});
        shape.has_fill = ts.draw_fill;
      }
      g_pen = {};
      push_undo(C, "Close Curve");
      return;
    }
    const bool weld = snap >= 0 && std::none_of(curve.points.begin(), curve.points.end(),
                                                [&](const CurvePoint &cp) { return cp.point == snap; });
    const int point = weld ? snap : drawing_add_point(drawing, pos);
    /* The previous end gets its handles again, now that it has a neighbour on both sides. */
    curve.points.back().handles_set = false;
    curve.points.push_back({point});
    if (g_pen.shape < 0) {
      Shape shape;
      shape.style = ts.style;
      shape.has_fill = false;
      shape.name = "Line " + std::to_string(drawing.shapes.size() + 1);
      drawing.shapes.push_back(shape);
      g_pen.shape = int(drawing.shapes.size()) - 1;
      for (int s = 0; s < curve.segment_count() - 1; s++) {
        drawing.shapes[g_pen.shape].edges.push_back({g_pen.curve, s});
      }
    }
    drawing.shapes[g_pen.shape].edges.push_back({g_pen.curve, curve.segment_count() - 1});
    deselect_all(drawing);
    drawing.points[point].selected = true;
    g_drag.pen_point = weld ? -1 : point;
    if (weld) {
      /* Welding to another curve ends this one, as in Moho. */
      g_pen = {};
    }
    return;
  }

  /* Start a new curve. */
  const int point = snap >= 0 ? snap : drawing_add_point(drawing, pos);
  Curve curve;
  curve.points.push_back({point});
  drawing.curves.push_back(curve);
  g_pen = {layer.id, int(drawing.curves.size()) - 1, -1};
  deselect_all(drawing);
  drawing.points[point].selected = true;
  g_drag.pen_point = snap >= 0 ? -1 : point;
}

/* ---------------------------------------------------------------------------------
 * Overlays
 */

static void draw_grid(ImDrawList *dl, const ViewMapping &view, float grid)
{
  float step = grid;
  while (step * view.zoom < 10.0f) {
    step *= 2.0f;
  }
  const float2 lo = view.to_document(view.origin + ImVec2(0, view.size.y));
  const float2 hi = view.to_document(view.origin + ImVec2(view.size.x, 0));
  const ImU32 col = IM_COL32(0, 0, 0, 28);
  for (float x = std::floor(lo.x / step) * step; x <= hi.x; x += step) {
    const float sx = view.to_screen({x, 0}).x;
    dl->AddLine(ImVec2(sx, view.origin.y), ImVec2(sx, view.origin.y + view.size.y), col);
  }
  for (float y = std::floor(lo.y / step) * step; y <= hi.y; y += step) {
    const float sy = view.to_screen({0, y}).y;
    dl->AddLine(ImVec2(view.origin.x, sy), ImVec2(view.origin.x + view.size.x, sy), col);
  }
}

static void draw_drawing_overlay(ImDrawList *dl, const VectorDrawing &drawing, const Transform2D &to_document,
                                 const ViewMapping &view, bool show_wires, bool show_points, int quality,
                                 HandleDisplay handles, bool point_squares = true)
{
  std::vector<float2> samples;
  /* Wires show each curve as it is edited: at its shape's detail level, through the points shown
   * (data/detail.hh); the stroke under them draws all of its detail. */
  VectorDrawing shown_curves;
  const VectorDrawing *lines = &drawing;
  if (drawing_has_detail(drawing)) {
    shown_curves = drawing_detail_shown_curves(drawing);
    lines = &shown_curves;
  }
  for (const Curve &curve : lines->curves) {
    if (!show_wires) {
      break;
    }
    for (int s = 0; s < curve.segment_count(); s++) {
      samples.clear();
      curve_segment_sample(*lines, curve, s, quality, samples,
                           0.5f / std::max(view.zoom * to_document.scale(), 1e-6f));
      int a, b;
      curve_segment_points(curve, s, &a, &b);
      samples.push_back(drawing.points[b].pos);
      const bool selected = drawing.points[a].selected && drawing.points[b].selected;
      std::vector<ImVec2> line;
      for (const float2 &p : samples) {
        line.push_back(view.to_screen(to_document.apply(p)));
      }
      const ImU32 col = selected ? COL_WIRE_SELECTED : COL_WIRE;
      if (curve.points[s].edge_hidden) {
        /* A hidden edge (Moho's Hide Edge) draws dashed and faint, so it can still be found. */
        const ImU32 faint = (col & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 110);
        for (size_t i = 0; i + 1 < line.size(); i += 2) {
          dl->AddLine(line[i], line[i + 1], faint, selected ? 1.6f : 1.0f);
        }
      }
      else {
        dl->AddPolyline(line.data(), int(line.size()), col, ImDrawFlags_None, selected ? 1.6f : 1.0f);
      }
    }
  }
  if (!show_points) {
    return;
  }
  /* Handles under the points: a line out to a dot, coloured by type, bright when selected. */
  for (const Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      const CurvePoint &cp = curve.points[i];
      if (!handle_visible(drawing, cp, handles)) {
        continue;
      }
      const float2 here = drawing.points[cp.point].pos;
      const ImVec2 at = view.to_screen(to_document.apply(here));
      const bool point_selected = drawing.points[cp.point].selected;
      for (const bool pre : {true, false}) {
        const float2 h = curve_handle(drawing, curve, i, pre);
        if (distance(h, here) < 1e-9f) {
          continue;
        }
        const bool selected = point_selected || (pre ? cp.select_in : cp.select_out);
        const ImU32 col = (cp.aligned ? COL_HANDLE_ALIGNED : COL_HANDLE_FREE)[selected ? 1 : 0];
        const ImVec2 end = view.to_screen(to_document.apply(h));
        dl->AddLine(at, end, col, 1.5f);
        dl->AddCircleFilled(end, 3.2f, (pre ? cp.select_in : cp.select_out) ? IM_COL32(0xff, 0xff, 0xff, 255) : col);
        dl->AddCircle(end, 3.2f, IM_COL32(0, 0, 0, 160));
      }
    }
  }
  if (!point_squares) {
    /* Edges mode shows lines only, as Blender's edge select mode. */
    return;
  }
  /* On a curve edited above level 0, its coarser points are drawn bigger than the level's own. */
  std::vector<bool> coarse(drawing.points.size(), false);
  if (drawing_has_detail(drawing)) {
    const std::vector<int> levels = drawing_curve_detail_levels(drawing);
    for (size_t c = 0; c < drawing.curves.size(); c++) {
      const int level = std::min(levels[c], curve_detail_max_level(drawing, drawing.curves[c]));
      for (const CurvePoint &cp : drawing.curves[c].points) {
        coarse[cp.point] = coarse[cp.point] || drawing.points[cp.point].detail.level < level;
      }
    }
  }
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < drawing.points.size(); i++) {
      const MeshPoint &p = drawing.points[i];
      if (p.selected != (pass == 1) || p.detail.hidden) {
        continue;
      }
      const float r = coarse[i] ? 4.0f : 3.0f;
      const ImVec2 s = view.to_screen(to_document.apply(p.pos));
      dl->AddRectFilled(s - ImVec2(r, r), s + ImVec2(r, r), p.selected ? COL_POINT_SELECTED : COL_POINT);
      if (!p.selected) {
        dl->AddRect(s - ImVec2(r, r), s + ImVec2(r, r), IM_COL32(0xff, 0xff, 0xff, 120));
      }
    }
  }
}

static void draw_cursor(ImDrawList *dl, ImVec2 c)
{
  /* Blender's red and white dashed ring with a crosshair. */
  const float r = 9.0f;
  for (int i = 0; i < 8; i++) {
    const float a0 = 2.0f * PI * i / 8.0f, a1 = a0 + 2.0f * PI / 8.0f;
    dl->PathArcTo(c, r, a0, a1, 6);
    dl->PathStroke(i % 2 ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xe0, 0x30, 0x30, 255), 0, 2.0f);
  }
  const ImU32 black = IM_COL32(0, 0, 0, 200);
  dl->AddLine(c - ImVec2(r + 6, 0), c - ImVec2(r - 3, 0), black);
  dl->AddLine(c + ImVec2(r - 3, 0), c + ImVec2(r + 6, 0), black);
  dl->AddLine(c - ImVec2(0, r + 6), c - ImVec2(0, r - 3), black);
  dl->AddLine(c + ImVec2(0, r - 3), c + ImVec2(0, r + 6), black);
}

/* The scene texture holds premultiplied colours; draw it with matching blending. */
static void premultiplied_blend(const ImDrawList *, const ImDrawCmd *)
{
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

/* ---------------------------------------------------------------------------------
 * The viewport
 */

/* ---------------------------------------------------------------------------------
 * Weight Paint
 */

/* Blender's weight colours: blue (0), cyan, green, yellow, red (1). */
static ImU32 weight_color(float w, int alpha = 255)
{
  w = std::clamp(w, 0.0f, 1.0f);
  const float stops[5][3] = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
  const float f = w * 4.0f;
  const int i = std::min(int(f), 3);
  const float t = f - float(i);
  const auto mix = [&](int c) { return stops[i][c] + (stops[i + 1][c] - stops[i][c]) * t; };
  return IM_COL32(int(mix(0) * 255.0f), int(mix(1) * 255.0f), int(mix(2) * 255.0f), alpha);
}

/* The bone layer the active drawing is in, as a writable layer. */
static Layer *weight_armature(Context &C)
{
  Layer *active = document_active_layer(*C.document);
  const Layer *armature = active ? layer_armature(*C.document, active->id) : nullptr;
  return armature ? layer_find(*C.document, armature->id) : nullptr;
}

/* The active bone, which the weight tools work on. */
static void weight_bone_combo(Layer *armature)
{
  const Bone *active = bone_find(*armature, armature->active_bone);
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 5.0f);
  if (ImGui::BeginCombo("##weight_bone", active ? active->name.c_str() : "Pick a bone")) {
    for (const Bone &b : armature->bones) {
      if (ImGui::Selectable(b.name.c_str(), b.id == armature->active_bone)) {
        armature->active_bone = b.id;
        for (Bone &other : armature->bones) {
          bone_select(other, other.id == b.id);
        }
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("The bone whose weights you paint or assign (Ctrl-click a bone in the viewport)");
  }
}

static void weight_select_header(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  Layer *armature = weight_armature(C);
  if (!armature) {
    return;
  }
  weight_bone_combo(armature);
  ImGui::SameLine();
  if (ImGui::Button("Automatic Weights")) {
    operator_call(C, "weight.automatic");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Bind the selected points (all when none are selected) to every bone: nearer bones pull "
                      "harder, as Blender's Automatic Weights");
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 4.0f);
  ImGui::SliderFloat("##assign_weight", &ts.weight_value, 0.0f, 1.0f, "Weight %.2f");
  ImGui::SameLine();
  if (ImGui::Button("Assign")) {
    operator_call(C, "weight.assign");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Give the selected points this weight for the active bone (1.0: they follow it alone)");
  }
  ImGui::SameLine(0, 1);
  if (ImGui::Button("Remove")) {
    operator_call(C, "weight.remove");
  }
  ImGui::SameLine(0, 1);
  if (ImGui::Button("Normalize")) {
    operator_call(C, "weight.normalize");
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto Normalize", &ts.weight_normalize);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Keep each point's weights adding up to 1 when assigning or painting");
  }
}

static void weight_paint_header(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  Layer *armature = weight_armature(C);
  if (!armature) {
    return;
  }
  weight_bone_combo(armature);
  const float w = ImGui::GetFrameHeight() * 4.0f;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(w);
  ImGui::SliderFloat("##weight", &ts.weight_value, 0.0f, 1.0f, "Weight %.2f");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(w);
  ImGui::DragFloat("##radius", &ts.weight_radius, 0.5f, 2.0f, 500.0f, "Radius %.0f px", ImGuiSliderFlags_AlwaysClamp);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(w);
  ImGui::SliderFloat("##strength", &ts.weight_strength, 0.0f, 1.0f, "Strength %.2f");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 3.5f);
  static const char *const modes[] = {"Draw", "Add", "Subtract"};
  ImGui::Combo("##weight_mode", &ts.weight_mode, modes, 3);
  ImGui::SameLine();
  ImGui::Checkbox("Normalize", &ts.weight_normalize);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Keep each point's weights adding up to 1, as Blender's Auto Normalize");
  }
  ImGui::SameLine();
  ImGui::Checkbox("Selection Mask", &ts.weight_mask);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Paint only the selected points (select them with Weight Select), as Blender's selection "
                      "masking");
  }
}

/* The active drawing coloured by how strongly the active bone moves it. */
static void draw_weight_overlay(ImDrawList *dl, Context &C, const Layer &layer, const ViewMapping &view, int quality)
{
  const Document &doc = *C.document;
  const Layer *armature = layer_armature(doc, layer.id);
  if (!armature) {
    return;
  }
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  const std::vector<float> weights = layer_point_bone_weights(doc, layer, armature->active_bone);
  const Transform2D to_document = layer_world_transform(doc, layer.id);
  /* Where the stroke shows: points and their handles moved by the bones. */
  const Posed posed_drawing = posed(doc, layer);
  const VectorDrawing &shown = *posed_drawing.drawing;
  std::vector<float2> samples;
  for (const Curve &curve : shown.curves) {
    for (int s = 0; s < curve.segment_count(); s++) {
      samples.clear();
      curve_segment_sample(shown, curve, s, quality, samples);
      int a, b;
      curve_segment_points(curve, s, &a, &b);
      samples.push_back(shown.points[b].pos);
      for (size_t k = 0; k + 1 < samples.size(); k++) {
        const float t = (float(k) + 0.5f) / float(samples.size() - 1);
        const ImU32 col = weight_color(weights[a] + (weights[b] - weights[a]) * t, 230);
        dl->AddLine(view.to_screen(to_document.apply(samples[k])), view.to_screen(to_document.apply(samples[k + 1])),
                    col, 4.0f);
      }
    }
  }
  for (size_t i = 0; i < shown.points.size(); i++) {
    const ImVec2 s = view.to_screen(to_document.apply(shown.points[i].pos));
    const bool selected = drawing.points[i].selected;
    dl->AddCircleFilled(s, selected ? 5.5f : 4.5f, weight_color(weights[i]));
    dl->AddCircle(s, selected ? 5.5f : 4.5f, selected ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0, 0, 0, 200), 0,
                  selected ? 2.0f : 1.0f);
  }
}

static bool g_weight_painting = false;

/* Paints while the left button is down; Ctrl-click picks the bone. */
static void weight_paint_input(Context &C, Layer &layer, const ViewMapping &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  const ToolSettings &ts = C.wm->tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  Layer *armature = weight_armature(C);
  if (!armature) {
    return;
  }
  if (hovered) {
    dl->AddCircle(io.MousePos, ts.weight_radius, IM_COL32(0xff, 0xff, 0xff, 200), 48, 1.5f);
  }
  if (hovered && io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (const uint64_t bone = pick_bone(doc, *armature, view, io.MousePos)) {
      armature->active_bone = bone;
      for (Bone &b : armature->bones) {
        bone_select(b, b.id == bone);
      }
      return;
    }
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g_weight_painting = true;
  }
  if (!g_weight_painting) {
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    g_weight_painting = false;
    push_undo(C, "Weight Paint");
    return;
  }
  if (!bone_find(*armature, armature->active_bone)) {
    report_banner(ReportType::Warning, "Pick a bone to paint its weights (Ctrl-click a bone)");
    g_weight_painting = false;
    return;
  }
  /* The brush works where points show (posed), in the layer's space. */
  const Transform2D to_document = layer_world_transform(doc, layer.id);
  const Transform2D to_layer = to_document.inverted();
  const std::vector<Transform2D> skins = layer_point_skins(doc, layer);
  const float2 mouse = to_layer.apply(view.to_document(io.MousePos));
  const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
  WeightPaintBrush brush;
  brush.radius = ts.weight_radius * pixel;
  brush.bone = armature->active_bone;
  brush.weight = ts.weight_value;
  brush.strength = ts.weight_strength * std::min(1.0f, io.DeltaTime * 10.0f);
  brush.mode = io.KeyCtrl ? WeightPaintMode::Subtract : WeightPaintMode(std::clamp(ts.weight_mode, 0, 2));
  brush.normalize = ts.weight_normalize;
  brush.selected_only = ts.weight_mask;
  /* Brush at the mouse, in each point's stored space: inverse of its skin. */
  brush.center = mouse;
  std::vector<float2> shown(layer.drawing.points.size());
  for (size_t i = 0; i < shown.size(); i++) {
    shown[i] = skins[i].apply(layer.drawing.points[i].pos);
  }
  brush.positions = &shown;
  layer_weight_paint(doc, layer, brush);
}

/* Weight Select: a click picks points, a drag box-selects them (Shift adds, Ctrl removes), and
 * Ctrl-click on a bone makes it the active one. */
struct WeightSelectDrag {
  bool active = false;
  int area = 0;
  ImVec2 start;
};
static WeightSelectDrag g_weight_select;

static void weight_select_input(Context &C, Area &area, const ViewMapping &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  const ImGuiIO &io = ImGui::GetIO();
  Layer *armature = weight_armature(C);
  if (!armature) {
    return;
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (io.KeyCtrl) {
      if (const uint64_t bone = pick_bone(doc, *armature, view, io.MousePos)) {
        armature->active_bone = bone;
        for (Bone &b : armature->bones) {
          bone_select(b, b.id == bone);
        }
        push_undo(C, "Select Bone");
        return;
      }
    }
    g_weight_select = {true, area.id, io.MousePos};
  }
  if (!g_weight_select.active || g_weight_select.area != area.id) {
    return;
  }
  const ImVec2 d = io.MousePos - g_weight_select.start;
  const bool dragged = d.x * d.x + d.y * d.y > 9.0f;
  if (dragged) {
    dl->AddRectFilled(g_weight_select.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
    dl->AddRect(g_weight_select.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (dragged) {
      select_box(C, view, g_weight_select.start, io.MousePos, HandleDisplay::None);
    }
    else {
      select_click(C, view, view.to_document(g_weight_select.start), HandleDisplay::None);
    }
    push_undo(C, "Select");
    g_weight_select = {};
  }
}

/* ---------------------------------------------------------------------------------
 * Bones
 */

static constexpr ImU32 COL_BONE = IM_COL32(0x9a, 0x9a, 0x9a, 150);
static constexpr ImU32 COL_BONE_EDGE = IM_COL32(0x10, 0x10, 0x10, 220);
static constexpr ImU32 COL_BONE_SELECTED = IM_COL32(0xff, 0x8c, 0x00, 190);
static constexpr ImU32 COL_BONE_ACTIVE = IM_COL32(0xff, 0xc0, 0x50, 220);
/* Frame 0 is the setup pose: a red border says so (the timeline is edged red too). */
static constexpr ImU32 COL_SETUP = IM_COL32(0xd6, 0x3c, 0x3c, 255);

struct BoneModal {
  bool active = false;
  int area = 0;
  char kind = 'G';
  uint64_t armature = 0;
  Document snapshot;
  /* The bones as they were; every update starts again from them. */
  std::vector<Bone> bones;
  bool setup_frame = true;
  /* Document -> bone layer space. */
  Transform2D to_armature;
  float2 pivot;
  float2 start_mouse;
  /* Whole turns count, as in ModalTransform. */
  float2 last_mouse;
  float turned = 0.0f;
  /* X or Y (0 none, 1 X, 2 Y): G along the document's axis; S along each bone's own axis while
   * posing (frame 0 only resizes, the same every way). */
  int axis = 0;
  /* S S: squash and stretch, longer and thinner (posing). */
  bool squash = false;
  /* Numbers typed while transforming. */
  NumInput num;
};
static BoneModal g_bone_modal;

/* A bone layer's posed bones, in document space. */
struct ScreenBone {
  ImVec2 head, tail;
  /* How thick it is posed, against how long (its X scale against its Y): drawn fatter or thinner. */
  float thickness = 1.0f;
  /* Which way the bone points on screen (a unit vector), for shapes drawn at its base alone. */
  ImVec2 dir = {1.0f, 0.0f};
  /* How the bone has moved since the setup pose, in document space (what moves a custom shape). */
  Transform2D moved;
};
/* Physics Ghosts on (Viewport Overlays): bones are drawn, picked and edited where their keys put
 * them; dynamics show as ghosts. Set as each viewport draws. */
static bool g_keyed_bones = true;
/* Custom Bone Shapes on (Viewport Overlays): bones with a shape are drawn and picked as it. */
static bool g_bone_shapes = true;
/* Relationship Lines on (Viewport Overlays): dashed lines from unconnected bones to their parents. */
static bool g_relationship_lines = true;
static constexpr float PIN_RADIUS = 6.0f;
/* The bone layer whose bones are being moved on the setup frame (0 for none): its picked bones are
 * drawn as bones meanwhile, whatever their shape, so their ends show where they go. Set as each
 * viewport draws. */
static uint64_t g_plain_armature = 0;

static bool shows_pin(const Bone &bone)
{
  return g_bone_shapes && bone.shape == BoneShape::Pin;
}

static float pin_radius(const Bone &bone)
{
  return PIN_RADIUS * std::clamp(bone.pin_size, 0.1f, 20.0f);
}

/* One outline of a custom bone shape, on screen. */
struct ShapeLoop {
  std::vector<ImVec2> points;
  /* For each point, whether the outline runs on from it to the next: false along hidden edges. */
  std::vector<bool> visible;
  bool closed = false;
  bool fill = false;
};

/* A Custom bone's shape on screen: its layer's drawing as on the setup frame, where that layer sits
 * at rest, moved as the bone has moved since the setup pose. Empty when it has no such layer (or
 * custom shapes aren't shown): it is drawn as a bone then. */
static std::vector<ShapeLoop> custom_shape_loops(const Document &doc, const Bone &bone, const ScreenBone &sb,
                                                 const ViewMapping &view)
{
  if (!g_bone_shapes || bone.shape != BoneShape::Custom || bone.shape_layer == 0) {
    return {};
  }
  const Layer *source = layer_find(doc, bone.shape_layer);
  if (!source || source->type != LayerType::Vector) {
    return {};
  }
  const VectorDrawing drawing = layer_setup_drawing(doc, *source, false);
  const Transform2D map = sb.moved * layer_setup_world_transform(doc, source->id);
  std::vector<ShapeLoop> loops;
  for (const Shape &shape : drawing.shapes) {
    if (shape.hidden || (!shape.has_fill && !shape.has_outline)) {
      continue;
    }
    std::vector<float2> outline;
    std::vector<bool> visible;
    bool closed = false;
    if (!shape_outline(drawing, shape, 8, outline, nullptr, &closed, &visible) || outline.size() < 2) {
      continue;
    }
    ShapeLoop loop;
    loop.closed = closed;
    /* Without an outline, no line at all; hidden edges (on the layer, or on the reference) leave gaps. */
    loop.visible = shape.has_outline ? visible : std::vector<bool>(outline.size(), false);
    loop.fill = shape.has_fill && closed && outline.size() >= 3;
    for (const float2 q : outline) {
      loop.points.push_back(view.to_screen(map.apply(q)));
    }
    loops.push_back(std::move(loop));
  }
  return loops;
}

/* A shade of a bone's colour set, at the see-through strength bones are drawn with. */
static ImU32 bone_shade(const Color &c, int alpha)
{
  return IM_COL32(int(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f), int(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                  int(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f), alpha);
}

static std::vector<ScreenBone> screen_bones(const Document &doc, const Layer &armature, const ViewMapping &view,
                                            bool keyed = g_keyed_bones)
{
  const Transform2D to_document = layer_world_transform(doc, armature.id);
  const Transform2D from_document = to_document.inverted();
  const std::vector<BonePose> poses = keyed ? armature_keyed_pose(armature, doc.frame_current == 0)
                                            : armature_pose(armature, doc.frame_current == 0);
  std::vector<ScreenBone> r(armature.bones.size());
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const float along = length(poses[i].pose.apply_vector({1.0f, 0.0f}));
    const float across = length(poses[i].pose.apply_vector({0.0f, 1.0f}));
    r[i] = {view.to_screen(to_document.apply(poses[i].head)), view.to_screen(to_document.apply(poses[i].tail)),
            along > 1e-9f ? std::clamp(across / along, 0.1f, 10.0f) : 1.0f};
    /* Along the bone's own axis, so a bone with no length still points somewhere. */
    const float2 ahead = poses[i].head + normalize(poses[i].pose.apply_vector({1.0f, 0.0f}));
    const ImVec2 d = view.to_screen(to_document.apply(ahead)) - r[i].head;
    const float n = std::sqrt(d.x * d.x + d.y * d.y);
    if (n > 1e-6f) {
      r[i].dir = d / n;
    }
    r[i].moved = to_document * poses[i].skin * from_document;
  }
  return r;
}

static float bone_joint_radius(const ScreenBone &sb)
{
  const ImVec2 d = sb.tail - sb.head;
  /* In proportion to the bone, as in Blender, so a small bone looks small; capped for long ones,
   * with a pixel floor so a tiny bone still shows. */
  return std::clamp(std::sqrt(d.x * d.x + d.y * d.y) * 0.08f, 1.0f, 6.5f);
}

/* Bones as Blender draws them in 2D: a long diamond from base to tip with a round, separately
 * selectable joint at each end. The active bone is drawn lighter. `ghost`: a bone layer you're not
 * working in, drawn as faint outlines only so it doesn't cover the artwork; `highlight_selected`
 * still draws its selected bones solid (the weight tools' bone). */
static constexpr ImU32 COL_BONE_GHOST = IM_COL32(0x80, 0x80, 0x80, 120);

/* A pin bone, as Moho draws one: a round head at the bone's base with a dot in the middle, and a
 * short tick showing which way it is turned. */
static void draw_pin(ImDrawList *dl, const ScreenBone &sb, float radius, ImU32 fill, ImU32 edge, float edge_width)
{
  const ImVec2 at = sb.head;
  if (fill) {
    dl->AddCircleFilled(at, radius, fill);
  }
  dl->AddCircle(at, radius, edge, 0, edge_width);
  dl->AddCircleFilled(at, std::max(1.8f, radius * 0.3f), edge);
  dl->AddLine(at + sb.dir * radius, at + sb.dir * (radius + std::max(5.0f, radius * 0.8f)), edge,
              std::max(edge_width, 1.5f));
}

/* A custom bone shape: its fills see-through and its outlines in the bone's shade. */
static void draw_custom_shape(ImDrawList *dl, const std::vector<ShapeLoop> &loops, ImU32 fill, ImU32 edge, float width)
{
  for (const ShapeLoop &loop : loops) {
    if (loop.fill && fill) {
      dl->AddConcavePolyFilled(loop.points.data(), int(loop.points.size()), fill);
    }
  }
  for (const ShapeLoop &loop : loops) {
    const size_t n = loop.points.size();
    if (std::all_of(loop.visible.begin(), loop.visible.end(), [](bool v) { return v; })) {
      dl->AddPolyline(loop.points.data(), int(n), edge, loop.closed ? ImDrawFlags_Closed : 0, width);
      continue;
    }
    /* Runs between hidden edges. */
    std::vector<ImVec2> run;
    for (size_t k = 0; k < n; k++) {
      if (!loop.visible[k]) {
        continue;
      }
      if (run.empty()) {
        run.push_back(loop.points[k]);
      }
      run.push_back(loop.points[(k + 1) % n]);
      if (k + 1 == n || !loop.visible[k + 1]) {
        dl->AddPolyline(run.data(), int(run.size()), edge, 0, width);
        run.clear();
      }
    }
  }
}

static void draw_bones(ImDrawList *dl, const Document &doc, const Layer &armature, const ViewMapping &view,
                       bool ghost = false, bool highlight_selected = false)
{
  const std::vector<ScreenBone> sbs = screen_bones(doc, armature, view);
  /* Physics ghosts: pale outlines where bone dynamics have the bones, for those they move. */
  if (g_keyed_bones && !ghost && !armature.dynamics_now.empty()) {
    const std::vector<ScreenBone> swung = screen_bones(doc, armature, view, false);
    const ImU32 col = IM_COL32(0x8c, 0xc8, 0xff, 200);
    for (size_t i = 0; i < swung.size() && i < sbs.size(); i++) {
      if (i < armature.bones.size() && !bone_shown(armature, armature.bones[i])) {
        continue;
      }
      const ImVec2 dh = swung[i].head - sbs[i].head, dt = swung[i].tail - sbs[i].tail;
      if (dh.x * dh.x + dh.y * dh.y < 0.25f && dt.x * dt.x + dt.y * dt.y < 0.25f) {
        continue;
      }
      if (i < armature.bones.size() && shows_pin(armature.bones[i])) {
        draw_pin(dl, swung[i], pin_radius(armature.bones[i]), IM_COL32(0x8c, 0xc8, 0xff, 40), col, 1.2f);
        continue;
      }
      if (const std::vector<ShapeLoop> loops = custom_shape_loops(doc, armature.bones[i], swung[i], view);
          i < armature.bones.size() && !loops.empty())
      {
        draw_custom_shape(dl, loops, IM_COL32(0x8c, 0xc8, 0xff, 40), col, 1.2f);
        continue;
      }
      const ImVec2 d = swung[i].tail - swung[i].head;
      const float len = std::sqrt(d.x * d.x + d.y * d.y);
      const float r = bone_joint_radius(swung[i]);
      const float w = std::clamp(std::clamp(len * 0.1f, r, 10.0f) * swung[i].thickness, 1.0f, 60.0f);
      const ImVec2 n = len > 0.0f ? ImVec2(-d.y / len, d.x / len) * w : ImVec2(0, 0);
      const ImVec2 mid = swung[i].head + d * 0.18f;
      const ImVec2 quad[4] = {swung[i].head, mid + n, swung[i].tail, mid - n};
      dl->AddConvexPolyFilled(quad, 4, IM_COL32(0x8c, 0xc8, 0xff, 40));
      dl->AddPolyline(quad, 4, col, ImDrawFlags_Closed, 1.2f);
      dl->AddCircle(swung[i].tail, r, col, 0, 1.0f);
    }
  }
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const Bone &bone = armature.bones[i];
    const auto parent = std::find_if(armature.bones.begin(), armature.bones.end(),
                                     [&](const Bone &b) { return b.id == bone.parent; });
    if (!g_relationship_lines || bone.connected || parent == armature.bones.end() || !bone_shown(armature, bone) ||
        !bone_shown(armature, *parent))
    {
      continue;
    }
    const ImVec2 from = sbs[parent - armature.bones.begin()].tail, to = sbs[i].head;
    const ImVec2 d = to - from;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    for (float t = 0.0f; t < len; t += 8.0f) {
      const float e = std::min(t + 4.0f, len);
      dl->AddLine(from + d * (t / len), from + d * (e / len), ghost ? COL_BONE_GHOST : IM_COL32(0x10, 0x10, 0x10, 200),
                  1.0f);
    }
  }
  /* Limit Angle, as Moho shows it: a red line from the base at the smallest angle the bone may
   * turn to and one at the largest, for the selected bones (relative to the parent as it is
   * posed). */
  if (!ghost) {
    const Transform2D to_document = layer_world_transform(doc, armature.id);
    const std::vector<BonePose> poses = g_keyed_bones ? armature_keyed_pose(armature, doc.frame_current == 0)
                                                      : armature_pose(armature, doc.frame_current == 0);
    for (size_t i = 0; i < armature.bones.size(); i++) {
      const Bone &bone = armature.bones[i];
      if (!(bone.selected || bone.select_head || bone.select_tail)) {
        continue;
      }
      for (const BoneConstraint &c : bone.constraints) {
        if (c.type != BoneConstraintType::LimitAngle || !c.enabled) {
          continue;
        }
        const BonePose *parent = bone_pose_find(poses, bone.parent);
        const float base = parent ? std::atan2(parent->pose.b, parent->pose.a) : 0.0f;
        const float reach = std::max(distance(poses[i].head, poses[i].tail), 1e-4f);
        for (const float limit : {c.min_angle, c.max_angle}) {
          const float a = base + limit;
          const float2 end = poses[i].head + float2(std::cos(a), std::sin(a)) * reach;
          dl->AddLine(view.to_screen(to_document.apply(poses[i].head)), view.to_screen(to_document.apply(end)),
                      IM_COL32(0xe8, 0x20, 0x20, 230), 1.5f);
        }
      }
    }
  }
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < armature.bones.size(); i++) {
      const Bone &bone = armature.bones[i];
      /* The active bone stands out only while it's selected, as in Blender. */
      const bool picked = bone.selected || bone.select_head || bone.select_tail;
      const bool active = bone.id == armature.active_bone && picked;
      if ((picked || active) != (pass == 1) || !bone_shown(armature, bone)) {
        continue;
      }
      /* Being moved on the setup frame: drawn as a bone until the move ends. */
      const bool as_bone = picked && armature.id == g_plain_armature;
      /* Its colour set's shades, at the strengths bones are drawn with (as Blender's bone colours:
       * selected and active bones show their own set's shades). */
      const BoneColors shades = bone_colors(bone);
      const bool plain = bone.color_set == 0;
      const ImU32 col_regular = bone_shade(shades.regular, 150), col_select = bone_shade(shades.select, 190),
                  col_active = bone_shade(shades.active, 220);
      const ImU32 col_ghost = plain ? COL_BONE_GHOST : bone_shade(shades.regular, 130);
      const bool faint = ghost && !(highlight_selected && picked);
      if (!as_bone && shows_pin(bone)) {
        if (faint) {
          draw_pin(dl, sbs[i], pin_radius(bone), 0, col_ghost, 1.0f);
        }
        else {
          const ImU32 fill = active ? col_active : picked ? col_select : col_regular;
          draw_pin(dl, sbs[i], pin_radius(bone), fill, active ? col_active : COL_BONE_EDGE, active ? 1.6f : 1.2f);
        }
        continue;
      }
      if (const std::vector<ShapeLoop> loops = as_bone ? std::vector<ShapeLoop>() : custom_shape_loops(doc, bone, sbs[i], view);
          !loops.empty())
      {
        if (faint) {
          draw_custom_shape(dl, loops, 0, col_ghost, 1.0f);
        }
        else {
          /* The outline in the shade itself, solid, so a shape of lines alone reads clearly. */
          const Color &edge = active ? shades.active : picked ? shades.select : shades.regular;
          draw_custom_shape(dl, loops, active ? col_active : picked ? col_select : col_regular, bone_shade(edge, 255),
                            active ? 2.5f : 2.0f);
        }
        continue;
      }
      const ImVec2 head = sbs[i].head, tail = sbs[i].tail;
      const ImVec2 d = tail - head;
      const float len = std::sqrt(d.x * d.x + d.y * d.y);
      const float r = bone_joint_radius(sbs[i]);
      const float w = std::clamp(std::clamp(len * 0.1f, r, 10.0f) * sbs[i].thickness, 1.0f, 60.0f);
      const ImVec2 n = len > 0.0f ? ImVec2(-d.y / len, d.x / len) * w : ImVec2(0, 0);
      const ImVec2 mid = head + d * 0.18f;
      const ImVec2 quad[4] = {head, mid + n, tail, mid - n};
      if (faint) {
        dl->AddPolyline(quad, 4, col_ghost, ImDrawFlags_Closed, 1.0f);
        dl->AddCircle(head, r, col_ghost, 0, 1.0f);
        dl->AddCircle(tail, r, col_ghost, 0, 1.0f);
        continue;
      }
      const ImU32 fill = active && bone.selected ? col_active : bone.selected ? col_select : col_regular;
      dl->AddConvexPolyFilled(quad, 4, fill);
      dl->AddPolyline(quad, 4, active ? col_active : COL_BONE_EDGE, ImDrawFlags_Closed, active ? 1.6f : 1.2f);
      const bool ends[2] = {bone_head_selected(armature, bone), bone_tail_selected(armature, bone)};
      const ImVec2 at[2] = {head, tail};
      for (int e = 0; e < 2; e++) {
        const ImU32 col = ends[e] ? (active ? col_active : col_select) : IM_COL32(0x2a, 0x2a, 0x2a, 230);
        dl->AddCircleFilled(at[e], r, col);
        dl->AddCircle(at[e], r, COL_BONE_EDGE, 0, 1.0f);
      }
    }
  }
}

/* What a click hits: a bone's head or tail circle (tried first), or its body. */
enum class BonePart { None, Body, Head, Tail };
struct BoneHit {
  uint64_t bone = 0;
  BonePart part = BonePart::None;
};

static BoneHit pick_bone_part(const Document &doc, const Layer &armature, const ViewMapping &view, ImVec2 mouse)
{
  const std::vector<ScreenBone> sbs = screen_bones(doc, armature, view);
  const float2 m(mouse.x, mouse.y);
  BoneHit best;
  float best_d = 1e30f;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    if (!bone_shown(armature, armature.bones[i])) {
      continue;
    }
    /* A pin is one round thing: clicking it picks the whole bone. */
    if (shows_pin(armature.bones[i])) {
      const float d = distance(m, float2(sbs[i].head.x, sbs[i].head.y));
      if (d <= pin_radius(armature.bones[i]) + 3.0f && d < best_d - 0.01f) {
        best_d = d;
        best = {armature.bones[i].id, BonePart::Body};
      }
      continue;
    }
    /* So is a custom shape: anywhere inside its fills, or near its lines. */
    if (const std::vector<ShapeLoop> loops = custom_shape_loops(doc, armature.bones[i], sbs[i], view); !loops.empty()) {
      float d = 1e30f;
      for (const ShapeLoop &loop : loops) {
        const size_t n = loop.points.size();
        bool inside = false;
        for (size_t a = 0, b = n - 1; a < n; b = a++) {
          const ImVec2 &pa = loop.points[a], &pb = loop.points[b];
          if ((pa.y > m.y) != (pb.y > m.y) && m.x < (pb.x - pa.x) * (m.y - pa.y) / (pb.y - pa.y) + pa.x) {
            inside = !inside;
          }
          /* Near a line that shows (not along a hidden edge). */
          if (b < loop.visible.size() && loop.visible[b]) {
            d = std::min(d, distance_to_segment(m, {pa.x, pa.y}, {pb.x, pb.y}));
          }
        }
        if (loop.fill && inside) {
          d = 0.0f;
        }
      }
      if (d <= 4.0f && d < best_d - 0.01f) {
        best_d = d;
        best = {armature.bones[i].id, BonePart::Body};
      }
      continue;
    }
    const float reach = bone_joint_radius(sbs[i]) + 3.0f;
    /* Tails before heads, so a shared joint picks the parent's tip. */
    const std::pair<ImVec2, BonePart> ends[2] = {{sbs[i].tail, BonePart::Tail}, {sbs[i].head, BonePart::Head}};
    for (const auto &[p, part] : ends) {
      const float d = distance(m, float2(p.x, p.y));
      if (d <= reach && d < best_d - 0.01f) {
        best_d = d;
        best = {armature.bones[i].id, part};
      }
    }
  }
  if (best.bone) {
    return best;
  }
  best_d = PICK_PIXELS * 1.5f;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    if (!bone_shown(armature, armature.bones[i])) {
      continue;
    }
    const float d = distance_to_segment(m, {sbs[i].head.x, sbs[i].head.y}, {sbs[i].tail.x, sbs[i].tail.y});
    if (d < best_d) {
      best_d = d;
      best = {armature.bones[i].id, BonePart::Body};
    }
  }
  return best;
}

/* The bone under the mouse (screen pixels), or 0. */
static uint64_t pick_bone(const Document &doc, const Layer &armature, const ViewMapping &view, ImVec2 mouse)
{
  return pick_bone_part(doc, armature, view, mouse).bone;
}

/* G, R or S on the selected bones, on any frame. G moves a bone picked by its body or base and
 * aims (and on the setup frame stretches) one picked by its tip; R turns bones about their bases;
 * S scales them. Children come along, as in Moho. */
/* G, R and S on an active Media layer move, turn and scale the layer itself (its Location, Rotation
 * and Scale; keyed away from frame 0, as any edit), as Blender does an object: turning and scaling
 * about the layer's centre (its picture's middle). X or Y keeps to one axis (G: the document's, S:
 * the layer's own), Ctrl snaps (the grid, 5°, tenths), a typed number sets it. */
struct LayerModal {
  bool active = false;
  int area = 0;
  char kind = 'G';
  uint64_t layer = 0;
  Document snapshot;
  float2 position, scale;
  float rotation = 0.0f;
  /* Document -> the layer's parent space (for G). */
  Transform2D to_parent;
  float2 pivot;
  float2 start_mouse, last_mouse;
  float turned = 0.0f;
  int axis = 0;
  NumInput num;
};
static LayerModal g_layer_modal;

static bool layer_modal_start(Context &C, Area &area, char kind, float2 mouse_document)
{
  Document &doc = *C.document;
  Layer *layer = document_active_layer(doc);
  if (!layer || !layer->is_media() || layer->locked || kind == 'E') {
    return false;
  }
  LayerModal modal;
  modal.area = area.id;
  modal.kind = kind;
  modal.layer = layer->id;
  const Transform2D world = layer_world_transform(doc, layer->id);
  modal.to_parent = (world * layer->local_transform().inverted()).inverted();
  modal.pivot = world.apply(float2());
  modal.position = layer->position;
  modal.rotation = layer->rotation;
  modal.scale = layer->scale;
  modal.snapshot = doc;
  modal.start_mouse = mouse_document;
  modal.last_mouse = mouse_document;
  modal.active = true;
  g_layer_modal = std::move(modal);
  C.wm->modal_running = true;
  C.wm->modal_keys.clear();
  return true;
}

static void layer_modal_end(Context &C, bool confirm)
{
  if (confirm) {
    push_undo(C, g_layer_modal.kind == 'R' ? "Rotate Layer" : g_layer_modal.kind == 'S' ? "Scale Layer" : "Move Layer");
  }
  else {
    const int frame = C.document->frame_current;
    *C.document = g_layer_modal.snapshot;
    C.document->frame_current = frame;
  }
  g_layer_modal.active = false;
  g_layer_modal.snapshot = Document();
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
}

static std::string layer_modal_update(Context &C, const ViewMapping &view, ImDrawList *dl)
{
  LayerModal &lm = g_layer_modal;
  Layer *layer = layer_find(*C.document, lm.layer);
  if (!layer) {
    layer_modal_end(C, false);
    return {};
  }
  if (modal_key_take(C, "ESC", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    layer_modal_end(C, false);
    return {};
  }
  const bool confirm = modal_key_take(C, "RET", true) || modal_key_take(C, "KEYPAD_ENTER", true) ||
                       modal_key_take(C, "SPACE", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (modal_key_take(C, "X")) {
    lm.axis = lm.axis == 1 ? 0 : 1;
  }
  if (modal_key_take(C, "Y")) {
    lm.axis = lm.axis == 2 ? 0 : 2;
  }
  if (lm.kind == 'R') {
    lm.axis = 0;
  }
  num_input_keys(C, lm.num, lm.kind == 'R' || lm.axis != 0 ? 1 : 2);
  if (lm.axis != 0) {
    lm.num.field = 0;
  }
  const NumInput &num = lm.num;
  if (!ImGui::IsMousePosValid()) {
    if (confirm) {
      layer_modal_end(C, true);
    }
    return "";
  }
  const ImGuiIO &io = ImGui::GetIO();
  const float2 mouse = view.to_document(io.MousePos);
  const bool snap = io.KeyCtrl;
  char text[160];
  if (lm.kind == 'G') {
    float2 delta = mouse - lm.start_mouse;
    if (snap) {
      delta = snap_to_grid(delta, C.wm->tool_settings.grid_size);
    }
    if (num.any()) {
      delta = lm.axis == 2 ? float2(0.0f, num.value(0, 0.0f))
              : lm.axis == 1 ? float2(num.value(0, 0.0f), 0.0f)
                             : float2(num.value(0, 0.0f), num.value(1, 0.0f));
    }
    if (lm.axis == 1) {
      delta.y = 0.0f;
    }
    else if (lm.axis == 2) {
      delta.x = 0.0f;
    }
    layer->position = lm.position + lm.to_parent.apply_vector(delta);
    if (lm.axis == 0) {
      std::snprintf(text, sizeof(text), "Move Layer  X: %s  Y: %s", num.shown(0, delta.x, "%.3f").c_str(),
                    num.shown(1, delta.y, "%.3f").c_str());
    }
    else {
      std::snprintf(text, sizeof(text), "Move Layer  %s: %s  along %s", lm.axis == 1 ? "X" : "Y",
                    num.shown(0, lm.axis == 1 ? delta.x : delta.y, "%.3f").c_str(), lm.axis == 1 ? "X" : "Y");
    }
  }
  else {
    /* Blender's dashed line from the centre to the mouse. */
    const ImVec2 a = view.to_screen(lm.pivot), b = io.MousePos;
    const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    for (float t = 0.0f; t < len; t += 8.0f) {
      const float u0 = t / std::max(len, 1.0f), u1 = std::min(t + 4.0f, len) / std::max(len, 1.0f);
      dl->AddLine(ImVec2(a.x + (b.x - a.x) * u0, a.y + (b.y - a.y) * u0),
                  ImVec2(a.x + (b.x - a.x) * u1, a.y + (b.y - a.y) * u1), IM_COL32(0, 0, 0, 200), 1.0f);
    }
    if (lm.kind == 'R') {
      lm.turned += turn_between(lm.last_mouse, mouse, lm.pivot);
      lm.last_mouse = mouse;
      float degrees = lm.turned * 180.0f / PI;
      if (snap) {
        degrees = std::round(degrees / 5.0f) * 5.0f;
      }
      if (num.any()) {
        degrees = num.value(0, 0.0f);
      }
      layer->rotation = lm.rotation + degrees;
      std::snprintf(text, sizeof(text), "Rotate Layer  %s\xC2\xB0", num.shown(0, degrees, "%.1f").c_str());
    }
    else {
      const float start = std::max(distance(lm.start_mouse, lm.pivot), 1e-6f);
      float factor = distance(mouse, lm.pivot) / start;
      if (snap) {
        factor = std::round(factor * 10.0f) / 10.0f;
      }
      float2 f(lm.axis == 2 ? 1.0f : factor, lm.axis == 1 ? 1.0f : factor);
      if (num.any()) {
        const float x = num.value(0, 1.0f);
        factor = x;
        f = lm.axis == 1 ? float2(x, 1.0f)
            : lm.axis == 2 ? float2(1.0f, x)
            : num.has(1) || num.negative[1] ? float2(num.has(0) ? x : 1.0f, num.value(1, 1.0f))
                                            : float2(x, x);
      }
      layer->scale = {lm.scale.x * f.x, lm.scale.y * f.y};
      if (lm.axis == 0 && (num.field == 1 || num.has(1))) {
        std::snprintf(text, sizeof(text), "Scale Layer  X: %s  Y: %s", num.shown(0, f.x, "%.3f").c_str(),
                      num.shown(1, f.y, "%.3f").c_str());
      }
      else {
        std::snprintf(text, sizeof(text), "Scale Layer  %s%s", num.shown(0, factor, "%.3f").c_str(),
                      lm.axis == 1 ? "  along X" : lm.axis == 2 ? "  along Y" : "");
      }
    }
  }
  if (confirm) {
    layer_modal_end(C, true);
  }
  else {
    C.wm->modal_keys.clear();
  }
  return text;
}

/* Alt S: the selected points' radius scaled about their middle, as S scales their places. */
struct RadiusModal {
  bool active = false;
  int area = 0;
  Document snapshot;
  struct Item {
    uint64_t layer;
    int point;
    float start;
  };
  std::vector<Item> items;
  float2 pivot;
  float2 start_mouse;
  NumInput num;
};
static RadiusModal g_radius_modal;

static bool radius_modal_start(Context &C, Area &area, float2 mouse_document)
{
  RadiusModal modal;
  float2 sum;
  int count = 0;
  for (Layer *layer : editable_layers(C)) {
    if (layer->is_reference() && !layer->reference.edit_points) {
      continue;
    }
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    for (int i = 0; i < int(layer->drawing.points.size()); i++) {
      const MeshPoint &p = layer->drawing.points[size_t(i)];
      if (p.selected) {
        modal.items.push_back({layer->id, i, p.radius});
        sum += to_document.apply(p.pos);
        count++;
      }
    }
  }
  if (modal.items.empty()) {
    return false;
  }
  modal.snapshot = *C.document;
  modal.area = area.id;
  modal.pivot = sum / float(count);
  modal.start_mouse = mouse_document;
  modal.active = true;
  g_radius_modal = std::move(modal);
  C.wm->modal_running = true;
  C.wm->modal_keys.clear();
  return true;
}

static void radius_modal_end(Context &C, bool confirm)
{
  if (confirm) {
    push_undo(C, "Point Radius");
  }
  else {
    const int frame = C.document->frame_current;
    *C.document = g_radius_modal.snapshot;
    C.document->frame_current = frame;
  }
  g_radius_modal = {};
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
}

static std::string radius_modal_update(Context &C, const ViewMapping &view, ImDrawList *dl)
{
  RadiusModal &rm = g_radius_modal;
  const ImGuiIO &io = ImGui::GetIO();
  if (modal_key_take(C, "ESC", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    radius_modal_end(C, false);
    return {};
  }
  const bool confirm = modal_key_take(C, "RET", true) || modal_key_take(C, "KEYPAD_ENTER", true) ||
                       modal_key_take(C, "SPACE", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  num_input_keys(C, rm.num, 1);
  if (!ImGui::IsMousePosValid()) {
    if (confirm) {
      radius_modal_end(C, true);
    }
    return "";
  }
  const float2 mouse = view.to_document(io.MousePos);
  /* Blender's dashed line from the middle to the mouse. */
  const ImVec2 a = view.to_screen(rm.pivot), b = io.MousePos;
  const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
  for (float t = 0.0f; t < len; t += 8.0f) {
    const float u0 = t / std::max(len, 1.0f), u1 = std::min(t + 4.0f, len) / std::max(len, 1.0f);
    dl->AddLine(ImVec2(a.x + (b.x - a.x) * u0, a.y + (b.y - a.y) * u0),
                ImVec2(a.x + (b.x - a.x) * u1, a.y + (b.y - a.y) * u1), IM_COL32(0, 0, 0, 200), 1.0f);
  }
  float factor = distance(mouse, rm.pivot) / std::max(distance(rm.start_mouse, rm.pivot), 1e-6f);
  if (io.KeyCtrl) {
    factor = std::round(factor * 10.0f) / 10.0f;
  }
  if (rm.num.any()) {
    factor = rm.num.value(0, 1.0f);
  }
  factor = std::max(factor, 0.0f);
  for (const RadiusModal::Item &item : rm.items) {
    if (Layer *layer = layer_find(*C.document, item.layer); layer && item.point < int(layer->drawing.points.size())) {
      layer->drawing.points[size_t(item.point)].radius = std::clamp(item.start * factor, 0.0f, POINT_RADIUS_MAX);
    }
  }
  char text[96];
  std::snprintf(text, sizeof(text), "Radius  %s%s", rm.num.shown(0, factor, "%.3f").c_str(),
                rm.items.size() == 1 ? "" : "  (each point's own, scaled)");
  if (confirm) {
    radius_modal_end(C, true);
  }
  else {
    C.wm->modal_keys.clear();
  }
  return text;
}

static bool bone_modal_start(Context &C, Area &area, char kind, float2 mouse_document)
{
  Document &doc = *C.document;
  Layer *armature = document_active_layer(doc);
  if (!armature || !armature->is_bone_layer() || kind == 'E' || !armature_any_picked(*armature)) {
    return false;
  }
  BoneModal modal;
  modal.area = area.id;
  modal.kind = kind;
  modal.armature = armature->id;
  modal.setup_frame = doc.frame_current == 0;
  const Transform2D to_document = layer_world_transform(doc, armature->id);
  modal.to_armature = to_document.inverted();
  float2 sum;
  const std::vector<float2> heads = armature_picked_heads(*armature, modal.setup_frame);
  for (const float2 &h : heads) {
    sum += to_document.apply(h);
  }
  modal.pivot = sum / float(std::max<size_t>(heads.size(), 1));
  modal.bones = armature->bones;
  modal.snapshot = doc;
  modal.start_mouse = mouse_document;
  modal.last_mouse = mouse_document;
  modal.active = true;
  g_bone_modal = std::move(modal);
  C.wm->modal_running = true;
  C.wm->modal_keys.clear();
  return true;
}

static void bone_modal_end(Context &C, bool confirm)
{
  if (confirm) {
    push_undo(C, g_bone_modal.kind == 'R' ? "Rotate Bones" : g_bone_modal.kind == 'S' ? "Scale Bones" : "Move Bones");
  }
  else {
    const int frame = C.document->frame_current;
    *C.document = g_bone_modal.snapshot;
    C.document->frame_current = frame;
  }
  g_bone_modal.active = false;
  g_bone_modal.snapshot = Document();
  g_bone_modal.bones.clear();
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
}

static std::string bone_modal_update(Context &C, const ViewMapping &view)
{
  Layer *armature = layer_find(*C.document, g_bone_modal.armature);
  if (!armature) {
    bone_modal_end(C, false);
    return {};
  }
  if (modal_key_take(C, "ESC", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    bone_modal_end(C, false);
    return {};
  }
  const bool confirm = modal_key_take(C, "RET", true) || modal_key_take(C, "KEYPAD_ENTER", true) ||
                       modal_key_take(C, "SPACE", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  /* Keys while transforming, as in Blender: X or Y for an axis (again: none), S again (while
   * scaling) for squash and stretch, and a typed number. */
  BoneModal &bm = g_bone_modal;
  if (modal_key_take(C, "X")) {
    bm.axis = bm.axis == 1 ? 0 : 1;
    bm.squash = false;
  }
  if (modal_key_take(C, "Y")) {
    bm.axis = bm.axis == 2 ? 0 : 2;
    bm.squash = false;
  }
  if (modal_key_take(C, "S") && bm.kind == 'S' && !bm.setup_frame) {
    bm.squash = !bm.squash;
    bm.axis = 0;
  }
  if (bm.kind == 'R' || (bm.kind == 'S' && bm.setup_frame)) {
    /* Turning has no axis in 2D; frame 0 resizes bones the same every way. */
    bm.axis = 0;
  }
  num_input_keys(C, bm.num, bm.kind == 'G' && bm.axis == 0 ? 2 : 1);
  if (bm.axis != 0 || bm.kind != 'G') {
    bm.num.field = 0;
  }
  const NumInput &num = bm.num;
  if (!ImGui::IsMousePosValid()) {
    if (confirm) {
      bone_modal_end(C, true);
    }
    return "";
  }
  const ImGuiIO &io = ImGui::GetIO();
  const float2 mouse = view.to_document(io.MousePos);
  const bool snap = io.KeyCtrl;
  char text[160];
  if (g_bone_modal.kind == 'G') {
    float2 delta = mouse - g_bone_modal.start_mouse;
    if (snap) {
      delta = snap_to_grid(delta, C.wm->tool_settings.grid_size);
    }
    if (num.any()) {
      delta = bm.axis == 2 ? float2(0.0f, num.value(0, 0.0f))
              : bm.axis == 1 ? float2(num.value(0, 0.0f), 0.0f)
                             : float2(num.value(0, 0.0f), num.value(1, 0.0f));
    }
    if (bm.axis == 1) {
      delta.y = 0.0f;
    }
    else if (bm.axis == 2) {
      delta.x = 0.0f;
    }
    armature_grab_selected(*armature, g_bone_modal.bones, g_bone_modal.to_armature.apply_vector(delta),
                           g_bone_modal.setup_frame, C.wm->tool_settings.auto_ik);
    if (bm.axis == 0) {
      std::snprintf(text, sizeof(text), "Move Bones  X: %s  Y: %s", num.shown(0, delta.x, "%.3f").c_str(),
                    num.shown(1, delta.y, "%.3f").c_str());
    }
    else {
      std::snprintf(text, sizeof(text), "Move Bones  %s: %s  along %s", bm.axis == 1 ? "X" : "Y",
                    num.shown(0, bm.axis == 1 ? delta.x : delta.y, "%.3f").c_str(), bm.axis == 1 ? "X" : "Y");
    }
  }
  else if (g_bone_modal.kind == 'R') {
    g_bone_modal.turned += turn_between(g_bone_modal.last_mouse, mouse, g_bone_modal.pivot);
    g_bone_modal.last_mouse = mouse;
    float degrees = g_bone_modal.turned * 180.0f / PI;
    if (snap) {
      degrees = std::round(degrees / 5.0f) * 5.0f;
    }
    if (num.any()) {
      degrees = num.value(0, 0.0f);
    }
    armature_rotate_selected(*armature, g_bone_modal.bones, degrees * PI / 180.0f, g_bone_modal.setup_frame);
    std::snprintf(text, sizeof(text), "Rotate Bones  %s\xC2\xB0", num.shown(0, degrees, "%.1f").c_str());
  }
  else {
    const float start = std::max(distance(g_bone_modal.start_mouse, g_bone_modal.pivot), 1e-6f);
    float factor = distance(mouse, g_bone_modal.pivot) / start;
    if (snap) {
      factor = std::round(factor * 10.0f) / 10.0f;
    }
    if (num.any()) {
      factor = num.value(0, 1.0f);
    }
    armature_scale_selected(*armature, g_bone_modal.bones, factor, bm.setup_frame, bm.axis, bm.squash);
    std::snprintf(text, sizeof(text), "%s  %s%s",
                  bm.setup_frame ? "Resize Bones" : bm.squash ? "Squash & Stretch" : "Scale Bones",
                  num.shown(0, factor, "%.3f").c_str(),
                  bm.axis == 1   ? "  along the bones' X (thickness)"
                  : bm.axis == 2 ? "  along the bones' Y (length)"
                                 : "");
  }
  if (C.wm->tool_settings.mirror_x) {
    armature_mirror_edits(*armature, g_bone_modal.bones, g_bone_modal.setup_frame);
  }
  /* Each update starts the bones again from how they were, which undoes what smart actions did to
   * the bones they drive: play them again from the bones as they are now, so a smart bone being
   * turned moves its bones (and their points) as it turns. */
  animation_refresh_smart(*C.document);
  if (confirm) {
    bone_modal_end(C, true);
  }
  else {
    C.wm->modal_keys.clear();
  }
  return text;
}

/* Selects what a click hit, as Blender's edit-bone picking does. Shift adds to the selection and
 * makes the bone active; Shift on the active bone's selected part deselects it. */
static void bone_click_select(Layer &armature, BoneHit hit, bool extend)
{
  Bone *bone = bone_find(armature, hit.bone);
  if (!bone) {
    if (!extend) {
      armature_deselect_all(armature);
    }
    return;
  }
  const auto part_selected = [&]() {
    switch (hit.part) {
      case BonePart::Head:
        return bone_head_selected(armature, *bone);
      case BonePart::Tail:
        return bone_tail_selected(armature, *bone);
      default:
        return bone->selected;
    }
  };
  const bool deselect = extend && bone->id == armature.active_bone && part_selected();
  if (!extend) {
    armature_deselect_all(armature);
  }
  /* A connected bone's head is its parent's tail: one joint. */
  Bone *parent = bone->connected ? bone_find(armature, bone->parent) : nullptr;
  /* Only the clicked bone is flagged, so X deletes that bone; deselecting clears the shared
   * joint on both sides. */
  const auto set_head = [&](bool v) {
    bone->select_head = v;
    if (parent && !v) {
      parent->select_tail = false;
    }
  };
  const auto set_tail = [&](bool v) {
    bone->select_tail = v;
    for (Bone &child : armature.bones) {
      if (child.connected && child.parent == bone->id && !v) {
        child.select_head = false;
      }
    }
  };
  switch (hit.part) {
    case BonePart::Head:
      set_head(!deselect);
      break;
    case BonePart::Tail:
      set_tail(!deselect);
      break;
    default:
      set_head(!deselect);
      set_tail(!deselect);
      break;
  }
  armature_flush_selection(armature);
  armature.active_bone = bone->id;
}

/* ---------------------------------------------------------------------------------
 * Transform Bones tool: a box gizmo around the selected bones, after Moho's
 * sz_transform_bones_gizmo. Inside the box moves, the band around it turns about the pivot, the
 * eight handles scale (from the opposite side; Alt from the pivot; Shift keeps a corner's
 * proportions), and the pivot can be dragged (Ctrl snaps it to a bone end). Outside, a click picks a
 * bone and a drag box-selects (Shift adds, Alt removes). Unselected children stay where they are.
 */

enum class GizmoMode { None, Move, Rotate, Scale, Pivot, Select };

struct BoneGizmo {
  GizmoMode mode = GizmoMode::None;
  /* Scale handle: 0-3 corners (bottom-left, top-left, top-right, bottom-right), 4-7 sides (left,
   * right, top, bottom). */
  int handle = 0;
  int area = 0;
  uint64_t armature = 0;
  /* Where the pivot sits relative to the middle of the box (bone layer space). */
  float2 pivot_offset;
  int last_count = -1;
  /* At the press. */
  std::vector<Bone> bones;
  float2 lo, hi, pivot, start;
  ImVec2 start_screen;
  float2 last;
  float turned = 0.0f;
  bool changed = false;
  /* The pivot while dragging (bone layer space). */
  float2 live_pivot;
};
static BoneGizmo g_gizmo;

static constexpr float GIZMO_MARKER = 8.0f;
static constexpr ImU32 COL_GIZMO = IM_COL32(0xd8, 0x9a, 0x50, 230);

static int picked_bone_count(const Layer &armature)
{
  int n = 0;
  for (const Bone &b : armature.bones) {
    n += b.selected || b.select_head || b.select_tail;
  }
  return n;
}

/* The box's handle positions: four corners, then the middles of the left, right, top and bottom. */
static std::array<float2, 8> gizmo_handles(float2 lo, float2 hi)
{
  const float2 c = (lo + hi) * 0.5f;
  return {float2(lo.x, lo.y), float2(lo.x, hi.y), float2(hi.x, hi.y), float2(hi.x, lo.y),
          float2(lo.x, c.y),  float2(hi.x, c.y),  float2(c.x, hi.y),  float2(c.x, lo.y)};
}

/* The end of a bone (bone layer space) nearest `p`, skipping picked bones when asked. */
static bool nearest_bone_end(const Layer &armature, bool setup, float2 p, bool skip_picked, float2 &out)
{
  const std::vector<BonePose> poses = armature_keyed_pose(armature, setup);
  float best = 1e30f;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const Bone &b = armature.bones[i];
    if (skip_picked && (b.selected || b.select_head || b.select_tail)) {
      continue;
    }
    for (const float2 e : {poses[i].head, poses[i].tail}) {
      if (distance(e, p) < best) {
        best = distance(e, p);
        out = e;
      }
    }
  }
  return best < 1e30f;
}

/* Applies a map of bone layer space to the selected bones as one undo step (the header fields). */
static void bone_gizmo_apply(Context &C, Layer &armature, const Transform2D &map, const char *undo_name)
{
  const std::vector<Bone> original = armature.bones;
  const bool setup = C.document->frame_current == 0;
  armature_map_selected(armature, original, map, setup);
  if (C.wm->tool_settings.mirror_x) {
    armature_mirror_edits(armature, original, setup);
  }
  push_undo(C, undo_name);
}

static Transform2D map_about(float2 pivot, float2 offset, float2 scale, float angle)
{
  return Transform2D::translate(pivot + offset) * Transform2D::rotate(angle) * Transform2D::scaling(scale) *
         Transform2D::translate(-pivot);
}

static void transform_bones_header(Context &C)
{
  Document &doc = *C.document;
  Layer *armature = document_active_layer(doc);
  if (!armature || !armature->is_bone_layer()) {
    return;
  }
  float2 lo, hi;
  const bool any = armature_picked_bounds(*armature, doc.frame_current == 0, lo, hi);
  const float2 pivot = (lo + hi) * 0.5f + g_gizmo.pivot_offset;
  const float w = ImGui::GetFrameHeight() * 3.2f;
  ImGui::BeginDisabled(!any);
  ImGui::TextUnformatted("Position");
  ImGui::SameLine();
  float pos[2] = {any ? pivot.x : 0.0f, any ? pivot.y : 0.0f};
  ImGui::SetNextItemWidth(w * 2.0f);
  if (ImGui::InputFloat2("##gizmo_pos", pos, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) && any) {
    bone_gizmo_apply(C, *armature, Transform2D::translate(float2(pos[0], pos[1]) - pivot), "Move Bones");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Scale");
  ImGui::SameLine();
  float scale[2] = {1.0f, 1.0f};
  ImGui::SetNextItemWidth(w * 2.0f);
  if (ImGui::InputFloat2("##gizmo_scale", scale, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) && any) {
    bone_gizmo_apply(C, *armature, map_about(pivot, {}, {scale[0], scale[1]}, 0.0f), "Scale Bones");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Angle");
  ImGui::SameLine();
  float angle = 0.0f;
  ImGui::SetNextItemWidth(w);
  if (ImGui::InputFloat("##gizmo_angle", &angle, 0.0f, 0.0f, "%.1f\xC2\xB0", ImGuiInputTextFlags_EnterReturnsTrue) &&
      any)
  {
    bone_gizmo_apply(C, *armature, map_about(pivot, {}, {1, 1}, angle * PI / 180.0f), "Rotate Bones");
  }
  ImGui::SameLine();
  if (ImGui::Button("Flip H")) {
    bone_gizmo_apply(C, *armature, map_about(pivot, {}, {-1, 1}, 0.0f), "Flip Bones");
  }
  ImGui::SameLine(0, 1);
  if (ImGui::Button("Flip V")) {
    bone_gizmo_apply(C, *armature, map_about(pivot, {}, {1, -1}, 0.0f), "Flip Bones");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Symmetrize")) {
    operator_call(C, "armature.symmetrize");
  }
}

/* The gizmo's box on screen: at least GIZMO_MIN_BOX pixels each way (bones lying perfectly flat
 * still get a box to grab), with a turning band around it at least GIZMO_MIN_BAND pixels wide so it
 * can be reached past the handles. Returns the band's width (bone layer space). */
static constexpr float GIZMO_MIN_BOX = 48.0f;
static constexpr float GIZMO_MIN_BAND = 22.0f;
static float gizmo_box(const Transform2D &to_document, const ViewMapping &view, float2 &lo, float2 &hi)
{
  const float2 c = (lo + hi) * 0.5f;
  const ImVec2 a = view.to_screen(to_document.apply(c)), b = view.to_screen(to_document.apply(c + float2(1, 0)));
  const float pixels = std::max(std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)), 1e-6f);
  const float min_box = GIZMO_MIN_BOX / pixels;
  if (hi.x - lo.x < min_box) {
    lo.x = c.x - min_box * 0.5f;
    hi.x = c.x + min_box * 0.5f;
  }
  if (hi.y - lo.y < min_box) {
    lo.y = c.y - min_box * 0.5f;
    hi.y = c.y + min_box * 0.5f;
  }
  return std::max(std::max(hi.x - lo.x, hi.y - lo.y) * 0.1f, GIZMO_MIN_BAND / pixels);
}

static GizmoMode gizmo_hit(const Layer &armature, const Transform2D &to_document, const ViewMapping &view,
                           ImVec2 mouse, float2 mouse_armature, float2 lo, float2 hi, float2 pivot, int *handle)
{
  const float band = gizmo_box(to_document, view, lo, hi);
  const auto over = [&](float2 p) {
    const ImVec2 s = view.to_screen(to_document.apply(p));
    return std::abs(s.x - mouse.x) < GIZMO_MARKER && std::abs(s.y - mouse.y) < GIZMO_MARKER;
  };
  (void)armature;
  if (over(pivot)) {
    return GizmoMode::Pivot;
  }
  const std::array<float2, 8> handles = gizmo_handles(lo, hi);
  for (int i = 0; i < 8; i++) {
    if (over(handles[i])) {
      *handle = i;
      return GizmoMode::Scale;
    }
  }
  const float2 v = mouse_armature;
  if (v.x < lo.x - band || v.x > hi.x + band || v.y < lo.y - band || v.y > hi.y + band) {
    return GizmoMode::Select;
  }
  if (v.x < lo.x || v.x > hi.x || v.y < lo.y || v.y > hi.y) {
    return GizmoMode::Rotate;
  }
  return GizmoMode::Move;
}

static void transform_bones_input(Context &C, Area &area, Layer &armature, const ViewMapping &view, bool hovered,
                                  ImDrawList *dl)
{
  Document &doc = *C.document;
  const ImGuiIO &io = ImGui::GetIO();
  const bool setup = doc.frame_current == 0;
  const Transform2D to_document = layer_world_transform(doc, armature.id);
  const Transform2D to_armature = to_document.inverted();
  const float2 mouse = to_armature.apply(view.to_document(io.MousePos));
  const int count = picked_bone_count(armature);
  if (g_gizmo.armature != armature.id || g_gizmo.last_count != count) {
    if (g_gizmo.mode == GizmoMode::None) {
      g_gizmo.pivot_offset = {};
    }
    g_gizmo.armature = armature.id;
    g_gizmo.last_count = count;
  }
  float2 lo, hi;
  const bool any = armature_picked_bounds(armature, setup, lo, hi);
  const float2 pivot = (lo + hi) * 0.5f + g_gizmo.pivot_offset;
  if (any) {
    gizmo_box(to_document, view, lo, hi);
  }

  /* Press: what the mouse is on decides the drag. */
  if (hovered && g_gizmo.mode == GizmoMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    int handle = 0;
    const GizmoMode mode =
        any ? gizmo_hit(armature, to_document, view, io.MousePos, mouse, lo, hi, pivot, &handle) : GizmoMode::Select;
    g_gizmo.mode = mode;
    g_gizmo.handle = handle;
    g_gizmo.area = area.id;
    g_gizmo.bones = armature.bones;
    g_gizmo.lo = lo;
    g_gizmo.hi = hi;
    g_gizmo.pivot = pivot;
    g_gizmo.live_pivot = pivot;
    g_gizmo.start = mouse;
    g_gizmo.last = mouse;
    g_gizmo.start_screen = io.MousePos;
    g_gizmo.turned = 0.0f;
    g_gizmo.changed = false;
  }
  if (g_gizmo.mode != GizmoMode::None && g_gizmo.area == area.id) {
    const bool released = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right) && g_gizmo.mode != GizmoMode::Select;
    const ImVec2 d = io.MousePos - g_gizmo.start_screen;
    const bool dragged = d.x * d.x + d.y * d.y > 9.0f;
    switch (g_gizmo.mode) {
      case GizmoMode::Select: {
        if (dragged) {
          dl->AddRectFilled(g_gizmo.start_screen, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
          dl->AddRect(g_gizmo.start_screen, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
        }
        if (released) {
          if (dragged) {
            const ImVec2 a = g_gizmo.start_screen, b = io.MousePos;
            const ImVec2 lo_s(std::min(a.x, b.x), std::min(a.y, b.y)), hi_s(std::max(a.x, b.x), std::max(a.y, b.y));
            armature_select_bones_in(
                armature, setup,
                [&](float2 p) {
                  const ImVec2 s = view.to_screen(to_document.apply(p));
                  return s.x >= lo_s.x && s.x <= hi_s.x && s.y >= lo_s.y && s.y <= hi_s.y;
                },
                io.KeyShift, io.KeyAlt);
          }
          else if (io.KeyAlt) {
            /* Alt-drag takes bones out of the selection; an Alt-click picks the layer. */
            g_gizmo.mode = GizmoMode::None;
            layer_pick_click(C, view, g_gizmo.start_screen, io.KeyShift, false);
            break;
          }
          else {
            const uint64_t picked = pick_bone(doc, armature, view, io.MousePos);
            if (!io.KeyShift) {
              armature_deselect_all(armature);
            }
            if (Bone *b = bone_find(armature, picked)) {
              bone_select(*b, true);
              armature.active_bone = picked;
            }
          }
          g_gizmo.pivot_offset = {};
          push_undo(C, "Select Bones");
          g_gizmo.mode = GizmoMode::None;
        }
        break;
      }
      case GizmoMode::Pivot: {
        float2 target = mouse;
        if (io.KeyCtrl) {
          nearest_bone_end(armature, setup, mouse, false, target);
        }
        g_gizmo.pivot_offset = target - (g_gizmo.lo + g_gizmo.hi) * 0.5f;
        g_gizmo.live_pivot = target;
        if (released || cancel) {
          g_gizmo.mode = GizmoMode::None;
        }
        break;
      }
      default: {
        if (cancel) {
          armature.bones = g_gizmo.bones;
          g_gizmo.mode = GizmoMode::None;
          break;
        }
        float2 offset, scale(1.0f, 1.0f), origin = g_gizmo.pivot;
        float angle = 0.0f;
        if (g_gizmo.mode == GizmoMode::Move) {
          offset = mouse - g_gizmo.start;
          if (io.KeyShift) {
            (std::abs(offset.x) > std::abs(offset.y) ? offset.y : offset.x) = 0.0f;
          }
          float2 snap;
          if (io.KeyCtrl && nearest_bone_end(armature, setup, mouse, true, snap)) {
            offset = snap - g_gizmo.pivot;
          }
          g_gizmo.live_pivot = g_gizmo.pivot + offset;
        }
        else if (g_gizmo.mode == GizmoMode::Rotate) {
          g_gizmo.turned += turn_between(g_gizmo.last, mouse, g_gizmo.pivot);
          g_gizmo.last = mouse;
          angle = g_gizmo.turned;
          if (io.KeyShift) {
            const float step = PI / 4.0f;
            angle = std::round(angle / step) * step;
          }
        }
        else {
          const float2 lo0 = g_gizmo.lo, hi0 = g_gizmo.hi, c0 = (lo0 + hi0) * 0.5f;
          static const float2 opposite[8] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}};
          /* The anchor is the opposite corner or side (x/y: 0 = low, 1 = high, -1 = the middle). */
          const float2 o = opposite[g_gizmo.handle];
          float2 anchor(o.x < 0 ? c0.x : (o.x > 0 ? hi0.x : lo0.x), o.y < 0 ? c0.y : (o.y > 0 ? hi0.y : lo0.y));
          if (io.KeyAlt) {
            anchor = g_gizmo.pivot;
          }
          const float2 p0 = g_gizmo.start - anchor, p1 = mouse - anchor;
          if (std::abs(p0.x) > 1e-3f) {
            scale.x = p1.x / p0.x;
          }
          if (std::abs(p0.y) > 1e-3f) {
            scale.y = p1.y / p0.y;
          }
          if (g_gizmo.handle == 4 || g_gizmo.handle == 5) {
            scale.y = 1.0f;
          }
          if (g_gizmo.handle == 6 || g_gizmo.handle == 7) {
            scale.x = 1.0f;
          }
          if (io.KeyShift && g_gizmo.handle < 4) {
            scale.x = scale.y = (scale.x + scale.y) * 0.5f;
          }
          origin = anchor;
          const float2 rel = g_gizmo.pivot - anchor;
          g_gizmo.live_pivot = anchor + float2(rel.x * scale.x, rel.y * scale.y);
        }
        armature_map_selected(armature, g_gizmo.bones, map_about(origin, offset, scale, angle), setup);
        if (C.wm->tool_settings.mirror_x) {
          armature_mirror_edits(armature, g_gizmo.bones, setup);
        }
        g_gizmo.changed |= dragged;
        if (released) {
          if (g_gizmo.changed) {
            push_undo(C, "Transform Bones");
            /* The pivot stays where the drag left it. */
            float2 nlo, nhi;
            if (armature_picked_bounds(armature, setup, nlo, nhi)) {
              g_gizmo.pivot_offset = g_gizmo.live_pivot - (nlo + nhi) * 0.5f;
            }
          }
          else {
            armature.bones = g_gizmo.bones;
          }
          g_gizmo.mode = GizmoMode::None;
        }
        break;
      }
    }
  }

  /* The cursor says what a press would do. */
  if (hovered && g_gizmo.mode == GizmoMode::None && any) {
    int handle = 0;
    switch (gizmo_hit(armature, to_document, view, io.MousePos, mouse, lo, hi, pivot, &handle)) {
      case GizmoMode::Move:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        break;
      case GizmoMode::Rotate:
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        break;
      case GizmoMode::Scale:
        ImGui::SetMouseCursor(handle == 0 || handle == 2   ? ImGuiMouseCursor_ResizeNESW
                              : handle == 1 || handle == 3 ? ImGuiMouseCursor_ResizeNWSE
                              : handle < 6                 ? ImGuiMouseCursor_ResizeEW
                                                           : ImGuiMouseCursor_ResizeNS);
        break;
      default:
        break;
    }
  }

  /* The gizmo. */
  float2 dlo, dhi;
  if (g_gizmo.mode == GizmoMode::Select || !armature_picked_bounds(armature, setup, dlo, dhi)) {
    return;
  }
  const auto screen = [&](float2 p) { return view.to_screen(to_document.apply(p)); };
  const auto box = [&](float2 a, float2 b) {
    const ImVec2 pts[4] = {screen(a), screen({a.x, b.y}), screen(b), screen({b.x, a.y})};
    dl->AddPolyline(pts, 4, COL_GIZMO, ImDrawFlags_Closed, 1.0f);
  };
  const float band = gizmo_box(to_document, view, dlo, dhi);
  box(dlo, dhi);
  box(dlo - float2(band, band), dhi + float2(band, band));
  for (const float2 &h : gizmo_handles(dlo, dhi)) {
    dl->AddCircle(screen(h), GIZMO_MARKER, COL_GIZMO, 0, 2.0f);
  }
  const float2 shown_pivot = g_gizmo.mode != GizmoMode::None ? g_gizmo.live_pivot
                                                              : (dlo + dhi) * 0.5f + g_gizmo.pivot_offset;
  const ImVec2 pc = screen(shown_pivot);
  dl->AddCircle(pc, GIZMO_MARKER, COL_GIZMO, 0, 1.0f);
  dl->AddLine(pc - ImVec2(GIZMO_MARKER * 1.4f, 0), pc + ImVec2(GIZMO_MARKER * 1.4f, 0), COL_GIZMO, 1.0f);
  dl->AddLine(pc - ImVec2(0, GIZMO_MARKER * 1.4f), pc + ImVec2(0, GIZMO_MARKER * 1.4f), COL_GIZMO, 1.0f);
}

/* ---------------------------------------------------------------------------------
 * Transform Points tool: the same box gizmo around the selected points (and handles picked on
 * their own), in the active layer's space, on any frame: inside moves, the band turns about the
 * pivot, the eight handles scale, the pivot can be dragged (Ctrl snaps it to a point). Points move
 * as G, R and S move them (across every edited layer, through their bones, with mirror and
 * proportional editing). Outside, a click selects and a drag box-selects, as the Select tool does.
 */

struct PointGizmo {
  GizmoMode mode = GizmoMode::None;
  int handle = 0;
  int area = 0;
  /* The layer whose space it is in, and the selection it was laid round (a new one resets the pivot). */
  uint64_t layer = 0;
  uint64_t selection = 0;
  float2 pivot_offset;
  /* At the press: what moves, and the box. */
  ModalTransform moving;
  float2 lo, hi, pivot, start, last;
  ImVec2 start_screen;
  float turned = 0.0f;
  bool changed = false;
  float2 live_pivot;
};
static PointGizmo g_point_gizmo;

/* The gizmo's space: the active layer's (document space without one). */
static Transform2D point_gizmo_space(const Document &doc)
{
  return doc.active_layer ? layer_world_transform(doc, doc.active_layer) : Transform2D();
}

/* Where the selected points and handles picked on their own are shown, in the gizmo's space. */
static std::vector<float2> point_gizmo_places(Context &C, const Transform2D &to_gizmo, bool selected)
{
  std::vector<float2> places;
  for (Layer *layer : editable_layers(C)) {
    if (layer->is_reference() && !layer->reference.edit_points) {
      continue;
    }
    const Transform2D to = to_gizmo * layer_world_transform(*C.document, layer->id);
    const Posed shown = posed(*C.document, *layer);
    const VectorDrawing &d = *shown.drawing;
    for (int i = 0; i < int(d.points.size()); i++) {
      if (layer->drawing.points[i].selected == selected) {
        places.push_back(to.apply(d.points[i].pos));
      }
    }
    if (!selected) {
      continue;
    }
    for (int c = 0; c < int(d.curves.size()); c++) {
      for (int i = 0; i < int(d.curves[c].points.size()); i++) {
        const CurvePoint &cp = layer->drawing.curves[c].points[i];
        if (layer->drawing.points[cp.point].selected) {
          continue;
        }
        for (const bool pre : {true, false}) {
          if (pre ? cp.select_in : cp.select_out) {
            places.push_back(to.apply(curve_handle(d, d.curves[c], i, pre)));
          }
        }
      }
    }
  }
  return places;
}

static bool point_gizmo_bounds(Context &C, const Transform2D &to_gizmo, float2 &lo, float2 &hi)
{
  const std::vector<float2> places = point_gizmo_places(C, to_gizmo, true);
  if (places.empty()) {
    return false;
  }
  lo = hi = places.front();
  for (const float2 p : places) {
    lo = float2(std::min(lo.x, p.x), std::min(lo.y, p.y));
    hi = float2(std::max(hi.x, p.x), std::max(hi.y, p.y));
  }
  return true;
}

/* The unselected point (gizmo space) nearest `p`: what Ctrl snaps to. */
static bool nearest_point(Context &C, const Transform2D &to_gizmo, float2 p, float2 &out)
{
  float best = 1e30f;
  for (const float2 q : point_gizmo_places(C, to_gizmo, false)) {
    if (distance(q, p) < best) {
      best = distance(q, p);
      out = q;
    }
  }
  return best < 1e30f;
}

/* Moves everything gathered in `moving` by `map` (gizmo space), proportional editing blending it. */
static void point_gizmo_place(Context &C, ModalTransform &moving, const Transform2D &to_document,
                              const Transform2D &map)
{
  ModalLayers layers;
  if (!modal_layers(C, moving, layers)) {
    return;
  }
  const Transform2D in_document = to_document * map * to_document.inverted();
  for (const ModalTransform::Item &item : moving.items) {
    const float w = modal_weight(C.wm->tool_settings, item);
    if (w <= 0.0f) {
      modal_rest(layers, item);
      continue;
    }
    modal_place(layers, item, item.start + (in_document.apply(item.start) - item.start) * w);
  }
  modal_mirror(C, moving, layers);
}

/* The header's fields and buttons: mesh.transform_selected, one map of the selection as one undo step. */
static void point_gizmo_apply(Context &C, float2 pivot, float2 move, float2 scale, float degrees)
{
  const auto num = [](float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return std::string(buf);
  };
  operator_call(C, "mesh.transform_selected",
                {{"pivot_x", num(pivot.x)}, {"pivot_y", num(pivot.y)}, {"move_x", num(move.x)}, {"move_y", num(move.y)},
                 {"scale_x", num(scale.x)}, {"scale_y", num(scale.y)}, {"angle", num(degrees)}});
}

void viewport_operators_register()
{
  OperatorType ot;
  ot.idname = "mesh.transform_selected";
  ot.name = "Transform Points";
  ot.description = "Move, scale and turn the selected points (and handles picked on their own) about a pivot, in the "
                   "active layer's space: move_x/move_y, scale_x/scale_y, angle (degrees), pivot_x/pivot_y (default: "
                   "the middle of the selection), as the Transform Points tool's header does";
  ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr && !editable_layers(C).empty(); };
  ot.exec = [](Context &C) {
    const Transform2D to_document = point_gizmo_space(*C.document);
    float2 lo, hi;
    if (!point_gizmo_bounds(C, to_document.inverted(), lo, hi)) {
      return OpStatus::Cancelled;
    }
    const float2 middle = (lo + hi) * 0.5f;
    const float2 pivot(op_arg_float(C, "pivot_x", middle.x), op_arg_float(C, "pivot_y", middle.y));
    const float2 move(op_arg_float(C, "move_x", 0.0f), op_arg_float(C, "move_y", 0.0f));
    const float2 scale(op_arg_float(C, "scale_x", 1.0f), op_arg_float(C, "scale_y", 1.0f));
    const float angle = op_arg_float(C, "angle", 0.0f) * PI / 180.0f;
    static Area area;
    ModalTransform moving;
    if (!modal_collect(C, area, 'G', {}, moving)) {
      return OpStatus::Cancelled;
    }
    point_gizmo_place(C, moving, to_document, map_about(pivot, move, scale, angle));
    return OpStatus::Finished;
  };
  operators().add(std::move(ot));
}

static void transform_points_header(Context &C)
{
  Document &doc = *C.document;
  const Transform2D to_document = point_gizmo_space(doc);
  float2 lo, hi;
  const bool any = point_gizmo_bounds(C, to_document.inverted(), lo, hi);
  const float2 pivot = (lo + hi) * 0.5f + g_point_gizmo.pivot_offset;
  const float w = ImGui::GetFrameHeight() * 3.2f;
  ImGui::BeginDisabled(!any);
  ImGui::TextUnformatted("Position");
  ImGui::SameLine();
  float pos[2] = {any ? pivot.x : 0.0f, any ? pivot.y : 0.0f};
  ImGui::SetNextItemWidth(w * 2.0f);
  if (ImGui::InputFloat2("##points_pos", pos, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) && any) {
    point_gizmo_apply(C, pivot, float2(pos[0], pos[1]) - pivot, {1, 1}, 0.0f);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Where the pivot is (the active layer's space): type a place to move the selection there");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Scale");
  ImGui::SameLine();
  float scale[2] = {1.0f, 1.0f};
  ImGui::SetNextItemWidth(w * 2.0f);
  if (ImGui::InputFloat2("##points_scale", scale, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) && any) {
    point_gizmo_apply(C, pivot, {}, {scale[0], scale[1]}, 0.0f);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Angle");
  ImGui::SameLine();
  float angle = 0.0f;
  ImGui::SetNextItemWidth(w);
  if (ImGui::InputFloat("##points_angle", &angle, 0.0f, 0.0f, "%.1f\xC2\xB0", ImGuiInputTextFlags_EnterReturnsTrue) &&
      any)
  {
    point_gizmo_apply(C, pivot, {}, {1, 1}, angle);
  }
  ImGui::SameLine();
  if (ImGui::Button("Flip H##points")) {
    point_gizmo_apply(C, pivot, {}, {-1, 1}, 0.0f);
  }
  ImGui::SameLine(0, 1);
  if (ImGui::Button("Flip V##points")) {
    point_gizmo_apply(C, pivot, {}, {1, -1}, 0.0f);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Symmetrize##points")) {
    operator_call(C, "mesh.symmetrize");
  }
}

static void transform_points_input(Context &C, Area &area, const ViewMapping &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  const ImGuiIO &io = ImGui::GetIO();
  ToolSettings &ts = C.wm->tool_settings;
  const Transform2D to_document = point_gizmo_space(doc);
  const Transform2D to_gizmo = to_document.inverted();
  const float2 mouse = to_gizmo.apply(view.to_document(io.MousePos));
  PointGizmo &g = g_point_gizmo;
  const uint64_t selection = document_selection_hash(doc);
  if ((g.layer != doc.active_layer || g.selection != selection) && g.mode == GizmoMode::None) {
    g.pivot_offset = {};
    g.layer = doc.active_layer;
    g.selection = selection;
  }
  float2 lo, hi;
  const bool any = point_gizmo_bounds(C, to_gizmo, lo, hi);
  const float2 pivot = (lo + hi) * 0.5f + g.pivot_offset;
  if (any) {
    gizmo_box(to_document, view, lo, hi);
  }
  const Layer &space_layer = *document_active_layer(doc);

  /* Press: what the mouse is on decides the drag. */
  if (hovered && g.mode == GizmoMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    int handle = 0;
    GizmoMode mode =
        any ? gizmo_hit(space_layer, to_document, view, io.MousePos, mouse, lo, hi, pivot, &handle) : GizmoMode::Select;
    if (mode == GizmoMode::Move || mode == GizmoMode::Rotate || mode == GizmoMode::Scale) {
      g.moving = ModalTransform();
      if (!modal_collect(C, area, 'G', view.to_document(io.MousePos), g.moving)) {
        mode = GizmoMode::None;
      }
    }
    g.mode = mode;
    g.handle = handle;
    g.area = area.id;
    g.lo = lo;
    g.hi = hi;
    g.pivot = pivot;
    g.live_pivot = pivot;
    g.start = mouse;
    g.last = mouse;
    g.start_screen = io.MousePos;
    g.turned = 0.0f;
    g.changed = false;
  }
  if (g.mode != GizmoMode::None && g.area == area.id) {
    const bool released = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right) && g.mode != GizmoMode::Select;
    const ImVec2 d = io.MousePos - g.start_screen;
    const bool dragged = d.x * d.x + d.y * d.y > 9.0f;
    switch (g.mode) {
      case GizmoMode::Select: {
        if (dragged) {
          dl->AddRectFilled(g.start_screen, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
          dl->AddRect(g.start_screen, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
        }
        if (released) {
          if (dragged) {
            select_box(C, view, g.start_screen, io.MousePos, handles_shown(doc.overlays, ts));
          }
          else if (io.KeyAlt) {
            g.mode = GizmoMode::None;
            layer_pick_click(C, view, g.start_screen, io.KeyShift, false);
            break;
          }
          else {
            select_click(C, view, view.to_document(io.MousePos), handles_shown(doc.overlays, ts));
          }
          g.pivot_offset = {};
          push_undo(C, "Select");
          g.mode = GizmoMode::None;
        }
        break;
      }
      case GizmoMode::Pivot: {
        float2 target = mouse;
        if (io.KeyCtrl) {
          nearest_point(C, to_gizmo, mouse, target);
        }
        g.pivot_offset = target - (g.lo + g.hi) * 0.5f;
        g.live_pivot = target;
        if (released || cancel) {
          g.mode = GizmoMode::None;
        }
        break;
      }
      default: {
        if (cancel) {
          const int frame = doc.frame_current;
          doc = g.moving.snapshot;
          doc.frame_current = frame;
          g.mode = GizmoMode::None;
          break;
        }
        float2 offset, scale(1.0f, 1.0f), origin = g.pivot;
        float angle = 0.0f;
        if (g.mode == GizmoMode::Move) {
          offset = mouse - g.start;
          if (io.KeyShift) {
            (std::abs(offset.x) > std::abs(offset.y) ? offset.y : offset.x) = 0.0f;
          }
          float2 snap;
          if (io.KeyCtrl && nearest_point(C, to_gizmo, mouse, snap)) {
            offset = snap - g.pivot;
          }
          g.live_pivot = g.pivot + offset;
        }
        else if (g.mode == GizmoMode::Rotate) {
          g.turned += turn_between(g.last, mouse, g.pivot);
          g.last = mouse;
          angle = g.turned;
          if (io.KeyShift) {
            const float step = PI / 4.0f;
            angle = std::round(angle / step) * step;
          }
        }
        else {
          const float2 lo0 = g.lo, hi0 = g.hi, c0 = (lo0 + hi0) * 0.5f;
          static const float2 opposite[8] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}};
          /* The anchor is the opposite corner or side (x/y: 0 = low, 1 = high, -1 = the middle). */
          const float2 o = opposite[g.handle];
          float2 anchor(o.x < 0 ? c0.x : (o.x > 0 ? hi0.x : lo0.x), o.y < 0 ? c0.y : (o.y > 0 ? hi0.y : lo0.y));
          if (io.KeyAlt) {
            anchor = g.pivot;
          }
          const float2 p0 = g.start - anchor, p1 = mouse - anchor;
          if (std::abs(p0.x) > 1e-6f) {
            scale.x = p1.x / p0.x;
          }
          if (std::abs(p0.y) > 1e-6f) {
            scale.y = p1.y / p0.y;
          }
          if (g.handle == 4 || g.handle == 5) {
            scale.y = 1.0f;
          }
          if (g.handle == 6 || g.handle == 7) {
            scale.x = 1.0f;
          }
          if (io.KeyShift && g.handle < 4) {
            scale.x = scale.y = (scale.x + scale.y) * 0.5f;
          }
          origin = anchor;
          const float2 rel = g.pivot - anchor;
          g.live_pivot = anchor + float2(rel.x * scale.x, rel.y * scale.y);
        }
        point_gizmo_place(C, g.moving, to_document, map_about(origin, offset, scale, angle));
        g.changed |= dragged;
        if (released) {
          if (g.changed) {
            push_undo(C, "Transform Points");
            /* The pivot stays where the drag left it. */
            float2 nlo, nhi;
            if (point_gizmo_bounds(C, to_gizmo, nlo, nhi)) {
              g.pivot_offset = g.live_pivot - (nlo + nhi) * 0.5f;
            }
            g.selection = document_selection_hash(doc);
          }
          else {
            const int frame = doc.frame_current;
            doc = g.moving.snapshot;
            doc.frame_current = frame;
          }
          g.moving = ModalTransform();
          g.mode = GizmoMode::None;
        }
        break;
      }
    }
  }

  /* The cursor says what a press would do. */
  if (hovered && g.mode == GizmoMode::None && any) {
    int handle = 0;
    switch (gizmo_hit(space_layer, to_document, view, io.MousePos, mouse, lo, hi, pivot, &handle)) {
      case GizmoMode::Move:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        break;
      case GizmoMode::Rotate:
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        break;
      case GizmoMode::Scale:
        ImGui::SetMouseCursor(handle == 0 || handle == 2   ? ImGuiMouseCursor_ResizeNESW
                              : handle == 1 || handle == 3 ? ImGuiMouseCursor_ResizeNWSE
                              : handle < 6                 ? ImGuiMouseCursor_ResizeEW
                                                           : ImGuiMouseCursor_ResizeNS);
        break;
      default:
        break;
    }
  }

  /* The gizmo. */
  float2 dlo, dhi;
  if (g.mode == GizmoMode::Select || !point_gizmo_bounds(C, to_gizmo, dlo, dhi)) {
    return;
  }
  const auto screen = [&](float2 p) { return view.to_screen(to_document.apply(p)); };
  const auto box = [&](float2 a, float2 b) {
    const ImVec2 pts[4] = {screen(a), screen({a.x, b.y}), screen(b), screen({b.x, a.y})};
    dl->AddPolyline(pts, 4, COL_GIZMO, ImDrawFlags_Closed, 1.0f);
  };
  const float band = gizmo_box(to_document, view, dlo, dhi);
  box(dlo, dhi);
  box(dlo - float2(band, band), dhi + float2(band, band));
  for (const float2 &h : gizmo_handles(dlo, dhi)) {
    dl->AddCircle(screen(h), GIZMO_MARKER, COL_GIZMO, 0, 2.0f);
  }
  const float2 shown_pivot = g.mode != GizmoMode::None ? g.live_pivot : (dlo + dhi) * 0.5f + g.pivot_offset;
  const ImVec2 pc = screen(shown_pivot);
  dl->AddCircle(pc, GIZMO_MARKER, COL_GIZMO, 0, 1.0f);
  dl->AddLine(pc - ImVec2(GIZMO_MARKER * 1.4f, 0), pc + ImVec2(GIZMO_MARKER * 1.4f, 0), COL_GIZMO, 1.0f);
  dl->AddLine(pc - ImVec2(0, GIZMO_MARKER * 1.4f), pc + ImVec2(0, GIZMO_MARKER * 1.4f), COL_GIZMO, 1.0f);
}

/* Whether a shown point of an edited layer is under the mouse (screen pixels). */
static bool point_under(Context &C, const ViewMapping &view, ImVec2 mouse)
{
  for (Layer *layer : editable_layers(C)) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const Posed shown = posed(*C.document, *layer);
    for (const MeshPoint &p : shown.drawing->points) {
      const ImVec2 s = view.to_screen(to_document.apply(p.pos));
      if ((s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y) <= PICK_PIXELS * PICK_PIXELS) {
        return true;
      }
    }
  }
  return false;
}

/* The Select tool on a bone layer: a click picks a bone or one of its ends; a drag box-selects
 * the ends inside (Shift adds, Ctrl removes), as Blender's Select Box tool does. */
struct BoneBox {
  bool active = false;
  int area = 0;
  ImVec2 start;
};
static BoneBox g_bone_box;

static void bone_tool_input(Context &C, Area &area, Layer &armature, const ViewMapping &view, bool hovered,
                            ImDrawList *dl)
{
  Document &doc = *C.document;
  const ImGuiIO &io = ImGui::GetIO();
  if (C.wm->tool_settings.tool == Tool::TransformBones) {
    transform_bones_input(C, area, armature, view, hovered, dl);
    return;
  }
  if (hovered && C.wm->tool_settings.tool == Tool::Select && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    /* A shown handle of a drawing in a shown bone collection: pressing on it drags it at once, as on the
     * layer itself (Blender's tweak). */
    const HandleHit handle = io.KeyShift || io.KeyCtrl
                                 ? HandleHit()
                                 : pick_handle(C, view, io.MousePos, handles_shown(C.document->overlays, C.wm->tool_settings));
    if (handle.layer) {
      const CurvePoint &cp = handle.layer->drawing.curves[handle.curve].points[handle.index];
      if (!(handle.pre ? cp.select_in : cp.select_out)) {
        for (Layer *editing : editable_layers(C)) {
          deselect_all(editing->drawing);
        }
        CurvePoint &picked = handle.layer->drawing.curves[handle.curve].points[handle.index];
        (handle.pre ? picked.select_in : picked.select_out) = true;
      }
      if (modal_start(C, area, 'G', view.to_document(io.MousePos))) {
        g_modal.tweak = true;
      }
      return;
    }
    g_bone_box = {true, area.id, io.MousePos};
  }
  if (g_bone_box.active && g_bone_box.area == area.id) {
    const ImVec2 d = io.MousePos - g_bone_box.start;
    const bool dragged = d.x * d.x + d.y * d.y > 9.0f;
    if (dragged) {
      dl->AddRectFilled(g_bone_box.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
      dl->AddRect(g_bone_box.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      /* Drawings in shown bone collections: their points are picked along with the bones. */
      const std::vector<Layer *> point_layers = editable_layers(C);
      const HandleDisplay handles = handles_shown(C.document->overlays, C.wm->tool_settings);
      if (dragged) {
        const Transform2D to_document = layer_world_transform(doc, armature.id);
        const ImVec2 a = g_bone_box.start, b = io.MousePos;
        const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        armature_select_joints_in(
            armature, doc.frame_current == 0,
            [&](float2 p) {
              const ImVec2 s = view.to_screen(to_document.apply(p));
              return s.x >= lo.x && s.x <= hi.x && s.y >= lo.y && s.y <= hi.y;
            },
            io.KeyShift, io.KeyCtrl);
        if (!point_layers.empty()) {
          select_box(C, view, a, b, handles);
        }
        push_undo(C, "Box Select");
      }
      else {
        /* A point under the mouse wins over a bone's body (points are the smaller targets); a
         * bone's ball wins over a point. */
        const BoneHit hit = pick_bone_part(doc, armature, view, g_bone_box.start);
        /* A handle counts as a point here (Shift-click picks it as on the layer itself). */
        const bool on_point = !point_layers.empty() && (hit.part == BonePart::Body || !hit.bone) &&
                              (point_under(C, view, g_bone_box.start) ||
                               pick_handle(C, view, g_bone_box.start, handles).layer);
        if (on_point) {
          if (!io.KeyShift) {
            armature_deselect_all(armature);
          }
          select_click(C, view, view.to_document(g_bone_box.start), handles);
          push_undo(C, "Select");
        }
        else {
          bone_click_select(armature, hit, io.KeyShift);
          if (!io.KeyShift) {
            for (Layer *l : point_layers) {
              deselect_all(l->drawing);
            }
          }
          push_undo(C, hit.bone ? "Select Bone" : "Deselect");
        }
      }
      g_bone_box = {};
    }
  }
  /* Right-click with bones selected: their context menu, as in Blender; with only points of the
   * drawings edited along with the bones selected, theirs. */
  if (hovered && C.wm->tool_settings.tool == Tool::Select && !io.KeyShift &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right) &&
      !g_right_click_spent)
  {
    if (armature_any_picked(armature)) {
      ImGui::OpenPopup("##bone_menu");
    }
    else {
      bool any = false;
      for (Layer *editing : editable_layers(C)) {
        any |= drawing_selected_alignment(editing->drawing) != -1;
      }
      if (any) {
        ImGui::OpenPopup("##point_menu");
      }
    }
  }
}

/* Ctrl P and Alt P on bones: Blender's parent menus. */
static void bone_parent_menus(Context &C, bool hovered)
{
  WindowManager &wm = *C.wm;
  if (hovered && (wm.menu_request == "bone_parent_set" || wm.menu_request == "bone_parent_clear")) {
    ImGui::OpenPopup(wm.menu_request == "bone_parent_set" ? "##bone_parent_set" : "##bone_parent_clear");
    wm.menu_request.clear();
  }
  if (ui_menu_begin("##bone_menu")) {
    menu_operator(C, "Subdivide", "armature.subdivide");
    ImGui::Separator();
    menu_operator(C, "Extrude", "armature.extrude");
    menu_operator(C, "Duplicate", "armature.duplicate_move");
    ImGui::Separator();
    menu_operator(C, "Make Parent...", "armature.parent_set_menu");
    menu_operator(C, "Clear Parent...", "armature.parent_clear_menu");
    ImGui::Separator();
    menu_operator(C, "Delete", "armature.bone_delete");
    ui_menu_end();
  }
  if (ui_menu_begin("##bone_parent_set")) {
    ImGui::TextDisabled("Make Parent");
    ImGui::Separator();
    if (ui_menu_item("Connected")) {
      operator_call(C, "armature.parent_set", {{"type", "CONNECTED"}});
    }
    if (ui_menu_item("Keep Offset")) {
      operator_call(C, "armature.parent_set", {{"type", "OFFSET"}});
    }
    ui_menu_end();
  }
  if (ui_menu_begin("##bone_parent_clear")) {
    ImGui::TextDisabled("Clear Parent");
    ImGui::Separator();
    if (ui_menu_item("Clear Parent")) {
      operator_call(C, "armature.parent_clear", {{"type", "CLEAR"}});
    }
    if (ui_menu_item("Disconnect Bone")) {
      operator_call(C, "armature.parent_clear", {{"type", "DISCONNECT"}});
    }
    ui_menu_end();
  }
}

/* The top layer shown under the mouse: one with a filled shape there, or a line close by. */
static uint64_t layer_under_mouse(const Document &doc, const ViewMapping &view, ImVec2 mouse,
                                  const std::vector<uint64_t> &local_view)
{
  /* Local View hides the rest, so a click can't pick them. */
  const std::set<uint64_t> local = local_view_layers(doc, local_view);
  const float2 at = view.to_document(mouse);
  const float px = std::max(distance(view.to_document(mouse + ImVec2(1.0f, 0.0f)), at), 1e-9f);
  /* Top first, as drawn (Draw Order included). */
  const std::vector<const Layer *> order = draw_order_layers(
      doc, [&](const Layer &l) { return l.visible && (local.empty() || local.contains(l.id)); });
  const auto find = [&]() -> uint64_t {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const Layer &layer = **it;
      if (layer.type != LayerType::Vector) {
        continue;
      }
      const VectorDrawing drawing = layer_has_modifier_effect(doc, layer, false)
                                        ? layer_evaluated_drawing(doc, layer, false)
                                        : layer_drawing(doc, layer);
      const Transform2D to_document = layer_world_transform(doc, layer.id);
      const float2 p = to_document.inverted().apply(at);
      const float scale = std::max(length(to_document.apply_vector({1.0f, 0.0f})), 1e-9f);
      if (drawing_pick_shape(drawing, p, 4.0f * px / scale) >= 0) {
        return layer.id;
      }
    }
    return 0;
  };
  return find();
}

/* A mask layer's shapes as dashed outlines in the mask orange, with its name ("inside head"):
 * what the active layer's masks cut it to. */
static void draw_mask_outline(ImDrawList *dl, const Document &doc, const Layer &layer, const ViewMapping &view,
                              bool outside, bool strong)
{
  (void)outside;
  const ImU32 col = IM_COL32(0xff, 0x9a, 0x3c, strong ? 255 : 190);
  ImVec2 top(FLT_MAX, FLT_MAX);
  const auto add = [&](auto &&self, const Layer &l) -> void {
    for (const Layer &child : l.children) {
      self(self, child);
    }
    if (l.type != LayerType::Vector) {
      return;
    }
    const VectorDrawing drawing = layer_has_modifier_effect(doc, l, false) ? layer_evaluated_drawing(doc, l, false)
                                                                            : layer_drawing(doc, l);
    const Transform2D to_document = layer_world_transform(doc, l.id);
    for (const Shape &shape : drawing.shapes) {
      std::vector<float2> outline;
      bool closed = false;
      if (shape.hidden || !shape_outline(drawing, shape, 8, outline, nullptr, &closed) || outline.size() < 2) {
        continue;
      }
      /* Dashes of about 6 pixels, with 4-pixel gaps. */
      std::vector<ImVec2> pts;
      for (const float2 q : outline) {
        pts.push_back(view.to_screen(to_document.apply(q)));
        if (pts.back().y < top.y) {
          top = pts.back();
        }
      }
      if (closed) {
        pts.push_back(pts.front());
      }
      float along = 0.0f;
      for (size_t i = 0; i + 1 < pts.size(); i++) {
        const ImVec2 a = pts[i], b = pts[i + 1];
        const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        float s = 0.0f;
        while (s < len) {
          const float phase = std::fmod(along + s, 10.0f);
          const float step = std::min(len - s, phase < 6.0f ? 6.0f - phase : 10.0f - phase);
          if (phase < 6.0f && len > 0.0f) {
            const ImVec2 p0 = a + (b - a) * (s / len), p1 = a + (b - a) * ((s + step) / len);
            dl->AddLine(p0, p1, col, strong ? 2.0f : 1.5f);
          }
          s += std::max(step, 0.01f);
        }
        along += len;
      }
    }
  };
  add(add, layer);
  if (top.x < FLT_MAX) {
    const std::string label = std::string(outside ? "outside " : "inside ") + layer.name;
    dl->AddText(top + ImVec2(6.0f, -ImGui::GetFontSize() - 4.0f), col, label.c_str());
  }
}

/* A layer's (and everything inside it's) box on screen, outlined while it is the one In Front Of...
 * or Behind... points at. */
static void draw_layer_box(ImDrawList *dl, const Document &doc, const Layer &layer, const ViewMapping &view)
{
  ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
  const auto add = [&](auto &&self, const Layer &l) -> void {
    if (!l.visible) {
      return;
    }
    for (const Layer &child : l.children) {
      self(self, child);
    }
    if (l.type != LayerType::Vector) {
      return;
    }
    const Transform2D to_document = layer_world_transform(doc, l.id);
    for (const MeshPoint &p : layer_drawing(doc, l).points) {
      const ImVec2 s = view.to_screen(to_document.apply(p.pos));
      lo = ImVec2(std::min(lo.x, s.x), std::min(lo.y, s.y));
      hi = ImVec2(std::max(hi.x, s.x), std::max(hi.y, s.y));
    }
  };
  add(add, layer);
  if (lo.x > hi.x) {
    return;
  }
  dl->AddRect(lo - ImVec2(4, 4), hi + ImVec2(4, 4), IM_COL32(0, 0, 0, 150), 3.0f, 0, 3.0f);
  dl->AddRect(lo - ImVec2(4, 4), hi + ImVec2(4, 4), IM_COL32(0x6f, 0xb8, 0xff, 255), 3.0f, 0, 1.5f);
  dl->AddText(ImVec2(lo.x - 4.0f, lo.y - 6.0f - ImGui::GetFontSize()), IM_COL32(0x6f, 0xb8, 0xff, 255), layer.name.c_str());
}

/* ---------------------------------------------------------------------------------
 * Select Layer (the tool, and Alt-click from any tool)
 */

/* A click being made with the Select Layer tool or Alt: it picks on release, unless the mouse moved. */
struct LayerPickClick {
  bool active = false;
  int area = 0;
  ImVec2 start;
};
static LayerPickClick g_layer_click;
/* The last pick, for clicking again in the same spot to take the layer below. */
static ImVec2 g_layer_pick_pos(-1e9f, -1e9f);
static uint64_t g_layer_pick_last = 0;

/* What a click at `mouse` can pick, top first: a shown bone's bone layer (bones are drawn over
 * everything), then the drawing layers shown there (anim/layer_pick.hh), as groups with Pick: Group. */
static std::vector<uint64_t> layer_pick_hits(Context &C, const ViewMapping &view, ImVec2 mouse)
{
  const Document &doc = *C.document;
  const ViewportOverlays &ov = doc.overlays;
  const std::set<uint64_t> local = local_view_layers(doc, C.wm->local_view);
  std::vector<uint64_t> hits;
  if (ov.show && ov.armature != ArmatureDisplay::Off) {
    const auto visit = [&](auto &&self, const std::vector<Layer> &list) -> void {
      for (const Layer &l : list) {
        if (!l.visible || (!local.empty() && !local.contains(l.id))) {
          continue;
        }
        if (l.is_bone_layer() &&
            (ov.armature == ArmatureDisplay::On || l.selected || l.id == doc.active_layer) &&
            pick_bone(doc, l, view, mouse) != 0)
        {
          hits.push_back(l.id);
        }
        self(self, l.children);
      }
    };
    visit(visit, doc.layers);
  }
  LayerPickOptions options;
  options.tolerance = 4.0f / std::max(view.zoom, 1e-6f);
  options.show_guides = ov.show && ov.guides;
  options.local_view = C.wm->local_view;
  for (const uint64_t id : layers_at(doc, view.to_document(mouse), options)) {
    hits.push_back(id);
  }
  if (C.wm->tool_settings.layer_pick_group) {
    std::vector<uint64_t> groups;
    for (const uint64_t id : hits) {
      const uint64_t top = layer_pick_group(doc, id);
      if (std::find(groups.begin(), groups.end(), top) == groups.end()) {
        groups.push_back(top);
      }
    }
    hits = std::move(groups);
  }
  return hits;
}

/* The layer a click at `mouse` would pick: the top one, or, clicking again where the last pick was,
 * the one below it (round to the top again after the bottom). */
static uint64_t layer_pick_target(Context &C, const ViewMapping &view, ImVec2 mouse)
{
  const std::vector<uint64_t> hits = layer_pick_hits(C, view, mouse);
  if (hits.empty()) {
    return 0;
  }
  const ImVec2 d = mouse - g_layer_pick_pos;
  if (d.x * d.x + d.y * d.y <= 16.0f) {
    const auto at = std::find(hits.begin(), hits.end(), g_layer_pick_last);
    if (at != hits.end()) {
      return std::next(at) != hits.end() ? *std::next(at) : hits.front();
    }
  }
  return hits.front();
}

/* A pick click: Shift adds (or takes out), Ctrl takes out, else the layer alone. */
static void layer_pick_click(Context &C, const ViewMapping &view, ImVec2 mouse, bool shift, bool ctrl)
{
  const uint64_t id = layer_pick_target(C, view, mouse);
  if (id == 0) {
    g_layer_pick_last = 0;
    return;
  }
  const char *mode = ctrl ? "REMOVE" : shift ? "TOGGLE" : "SET";
  operator_call(C, "layer.select_at", {{"layer", std::to_string(id)}, {"mode", mode}});
  g_layer_pick_pos = mouse;
  g_layer_pick_last = id;
}

/* Whether a press starts a pick click: always with the Select Layer tool; with Alt from the tools
 * whose Alt-drags mean nothing else (not the Shape tool's centred shapes, a handle's drag or the
 * transform boxes, which pick on their own click instead). */
static bool layer_pick_wanted(Context &C, const ViewMapping &view)
{
  const ToolSettings &ts = C.wm->tool_settings;
  if (ts.tool == Tool::SelectLayer) {
    return true;
  }
  if (!ImGui::GetIO().KeyAlt || ts.tool == Tool::Shape || ts.tool == Tool::TransformBones ||
      ts.tool == Tool::TransformPoints)
  {
    return false;
  }
  if (ts.tool == Tool::Select &&
      pick_handle(C, view, ImGui::GetIO().MousePos, handles_shown(C.document->overlays, ts)).layer)
  {
    return false;
  }
  return true;
}

/* Runs a pick click: shows what it would pick, and picks on release if the mouse stayed put. */
static void layer_pick_update(Context &C, const ViewMapping &view, ImDrawList *dl)
{
  const ImGuiIO &io = ImGui::GetIO();
  if (const Layer *target = layer_find(*C.document, layer_pick_target(C, view, g_layer_click.start))) {
    draw_layer_box(dl, *C.document, *target, view);
  }
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    return;
  }
  const ImVec2 d = io.MousePos - g_layer_click.start;
  if (d.x * d.x + d.y * d.y <= 9.0f) {
    layer_pick_click(C, view, g_layer_click.start, io.KeyShift, io.KeyCtrl && !io.KeyAlt);
  }
  g_layer_click = {};
}

/* The bone or points of the timeline's Keyed Here item under the mouse, outlined in white so you
 * see which they are before choosing it. */
static void draw_keyed_hover(ImDrawList *dl, Context &C, const ViewMapping &view)
{
  const WindowManager::KeyedHover &hover = C.wm->keyed_hover;
  const Document &doc = *C.document;
  const Layer *layer = hover.layer ? layer_find(doc, hover.layer) : nullptr;
  if (!layer) {
    return;
  }
  const ImU32 white = IM_COL32(255, 255, 255, 235), shadow = IM_COL32(0, 0, 0, 160);
  const Transform2D to_document = layer_world_transform(doc, layer->id);
  if (layer->is_bone_layer()) {
    const std::vector<BonePose> poses = armature_pose(*layer, doc.frame_current == 0);
    for (const std::string &path : hover.paths) {
      if (path.rfind("bones[\"", 0) != 0) {
        continue;
      }
      const std::string name = path.substr(7, path.find("\"]", 7) - 7);
      for (size_t i = 0; i < layer->bones.size() && i < poses.size(); i++) {
        if (layer->bones[i].name != name) {
          continue;
        }
        const ImVec2 h = view.to_screen(to_document.apply(poses[i].head));
        const ImVec2 tl = view.to_screen(to_document.apply(poses[i].tail));
        dl->AddLine(h, tl, shadow, 6.0f);
        dl->AddLine(h, tl, white, 3.0f);
        dl->AddCircle(h, 9.0f, shadow, 0, 4.0f);
        dl->AddCircle(h, 9.0f, white, 0, 2.0f);
      }
    }
    return;
  }
  const Posed shown = posed(doc, *layer);
  const VectorDrawing &d = *shown.drawing;
  for (const std::string &path : hover.paths) {
    int index = -1;
    if (path.rfind("points[", 0) == 0) {
      index = std::atoi(path.c_str() + 7);
    }
    else if (path.rfind("curves[", 0) == 0) {
      const int c = std::atoi(path.c_str() + 7);
      const size_t open = path.find("points[");
      const int i = open == std::string::npos ? -1 : std::atoi(path.c_str() + open + 7);
      if (c >= 0 && c < int(d.curves.size()) && i >= 0 && i < int(d.curves[c].points.size())) {
        index = d.curves[c].points[i].point;
      }
    }
    if (index >= 0 && index < int(d.points.size())) {
      const ImVec2 s = view.to_screen(to_document.apply(d.points[index].pos));
      dl->AddCircle(s, 8.0f, shadow, 0, 4.0f);
      dl->AddCircle(s, 8.0f, white, 0, 2.0f);
    }
  }
}

/* The ViewportActionPanel, Blender's Adjust Last Operation: the last operator run in the viewport,
 * while it can be adjusted, floats in the bottom-left corner folded to its name (as Blender's
 * starts); open, its options show under it (labels on the left, values on the right), and changing
 * one runs it again with it. Open or folded stays as last left, as Blender's panel does. Colours and
 * sizes are Blender's: the menu background (#181818, outlined #242424), fields #282828 outlined
 * #3D3D3D. */
/* Kept by the project, as other panels are. */
static constexpr const char *ACTION_PANEL_KEY = "Viewport/Adjust Last Operation";

static void action_panel_draw(Context &C, ImVec2 origin, ImVec2 size, float left)
{
  const LastOperation *last = operator_last_redo(C);
  if (C.wm->action_panel_toggle_request) {
    ui_fold_record(ACTION_PANEL_KEY, false, !ui_fold_state(ACTION_PANEL_KEY, false));
    C.wm->action_panel_toggle_request = false;
    /* Folding it is kept with the project, but leaves what it adjusts adjustable. */
    if (last) {
      operator_last_redo_keep(C);
    }
  }
  const bool panel_open = ui_fold_state(ACTION_PANEL_KEY, false);
  if (!last || last->area != "Viewport") {
    return;
  }
  const OperatorType *ot = operators().find(last->idname);
  if (!ot || ot->props.empty()) {
    return;
  }
  const float unit = ImGui::GetFrameHeight();
  const float fs = ImGui::GetFontSize();
  const float pad = 6.0f, gap = 3.0f;
  const float header_h = unit + 4.0f;
  const float folded_w = std::max(unit * 7.5f, ImGui::CalcTextSize(ot->name.c_str()).x + unit * 2.5f);
  const float w = panel_open ? std::max(folded_w, unit * 14.0f) : folded_w;
  const float body_h = panel_open ? float(ot->props.size()) * (unit + gap) - gap + pad * 2.0f : 0.0f;
  const ImVec2 min(origin.x + left, origin.y + size.y - header_h - body_h - 8.0f);
  const ImVec2 max(min.x + w, min.y + header_h + body_h);
  if (min.y < origin.y + unit * 2.0f || max.x > origin.x + size.x) {
    return;
  }
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float round = 4.0f;
  dl->AddRectFilled(min, max, IM_COL32(0x18, 0x18, 0x18, 255), round);
  dl->AddRect(min, max, IM_COL32(0x24, 0x24, 0x24, 255), round, 0, 1.0f);
  /* The panel takes the clicks over it, not the viewport beneath. */
  ImGui::SetCursorScreenPos(min);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##action_panel", max - min);

  /* The header: open arrow and the operator's name; clicking anywhere on it folds or opens. */
  ImGui::SetCursorScreenPos(min);
  ImGui::SetNextItemAllowOverlap();
  if (ImGui::InvisibleButton("##action_panel_header", ImVec2(w, header_h))) {
    /* Shown the new way from the next frame. */
    ui_fold_record(ACTION_PANEL_KEY, false, !panel_open);
    operator_last_redo_keep(C);
  }
  const ImU32 text = IM_COL32(0xd9, 0xd9, 0xd9, 255);
  /* Blender's chevrons: > folded, v open. */
  const ImVec2 arrow(min.x + pad + fs * 0.45f, min.y + header_h * 0.5f);
  const float s = fs * 0.22f;
  if (panel_open) {
    const ImVec2 v[3] = {arrow + ImVec2(-s, -s * 0.5f), arrow + ImVec2(0, s * 0.5f), arrow + ImVec2(s, -s * 0.5f)};
    dl->AddPolyline(v, 3, text, 0, 1.5f);
  }
  else {
    const ImVec2 v[3] = {arrow + ImVec2(-s * 0.5f, -s), arrow + ImVec2(s * 0.5f, 0), arrow + ImVec2(-s * 0.5f, s)};
    dl->AddPolyline(v, 3, text, 0, 1.5f);
  }
  dl->AddText(ImVec2(min.x + pad + fs * 1.25f, min.y + (header_h - fs) * 0.5f), text, ot->name.c_str());
  if (!panel_open) {
    return;
  }

  /* The options, as Blender lays them out (labels split off to the left). */
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x28, 0x28, 0x28, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0x30, 0x30, 0x30, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0x22, 0x22, 0x22, 255));
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0x28, 0x28, 0x28, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0x30, 0x30, 0x30, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0x3d, 0x3d, 0x3d, 255));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0x18, 0x18, 0x18, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, round);
  const float inner = w - pad * 2.0f;
  const float label_w = inner * 0.4f;
  const float value_x = min.x + pad + label_w + 6.0f;
  const float value_w = max.x - pad - value_x;
  float y = min.y + header_h + pad - 2.0f;
  OpArgs args = last->args;
  bool changed = false;
  for (const OpProperty &prop : ot->props) {
    ImGui::PushID(prop.name.c_str());
    const std::string value = args[prop.name];
    if (prop.kind != OpProperty::Kind::Bool) {
      const float tw = ImGui::CalcTextSize(prop.label.c_str()).x;
      dl->AddText(ImVec2(value_x - 6.0f - tw, y + (unit - fs) * 0.5f), text, prop.label.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(value_x, y));
    if (prop.kind == OpProperty::Kind::Enum) {
      std::string shown = value;
      for (const auto &[id, label] : prop.items) {
        if (id == value) {
          shown = label;
        }
      }
      ImGui::SetNextItemWidth(value_w);
      const bool open = ImGui::BeginCombo("##value", shown.c_str(), ImGuiComboFlags_NoArrowButton);
      {
        /* Blender's dropdown chevron, at the right end of the field. */
        const ImVec2 b = ImGui::GetItemRectMax(), a = ImGui::GetItemRectMin();
        const ImVec2 c(b.x - unit * 0.5f, (a.y + b.y) * 0.5f);
        const float k = fs * 0.2f;
        const ImVec2 v[3] = {c + ImVec2(-k, -k * 0.5f), c + ImVec2(0, k * 0.5f), c + ImVec2(k, -k * 0.5f)};
        dl->AddPolyline(v, 3, text, 0, 1.5f);
      }
      if (open) {
        for (const auto &[id, label] : prop.items) {
          if (ImGui::Selectable(label.c_str(), id == value) && id != value) {
            args[prop.name] = id;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
    }
    else if (prop.kind == OpProperty::Kind::Float) {
      float v = std::strtof(value.c_str(), nullptr);
      ImGui::SetNextItemWidth(value_w);
      if (ImGui::DragFloat("##value", &v, prop.step, prop.min, prop.max, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        args[prop.name] = buf;
        changed = true;
      }
    }
    else if (prop.kind == OpProperty::Kind::Int) {
      int v = std::atoi(value.c_str());
      ImGui::SetNextItemWidth(value_w);
      if (ImGui::DragInt("##value", &v, 0.25f, int(prop.min), int(prop.max), "%d", ImGuiSliderFlags_AlwaysClamp)) {
        args[prop.name] = std::to_string(v);
        changed = true;
      }
    }
    else {
      bool on = value == "1" || value == "true" || value == "True";
      if (ImGui::Checkbox(prop.label.c_str(), &on)) {
        args[prop.name] = on ? "1" : "0";
        changed = true;
      }
    }
    if (!prop.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", prop.description.c_str());
    }
    ImGui::PopID();
    y += unit + gap;
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(7);
  if (changed) {
    operator_redo_last(C, args);
  }
}

static void viewport_draw_contents(Area &area, Context &C);

void viewport_draw(Area &area, Context &C)
{
  /* The overlays are the project's: changing one (its popover, Shift Z, a script) is unsaved. */
  const ViewportOverlays before = C.document->overlays;
  /* A file opened with its own view: the viewport looks where it was left (none: frames the camera). */
  Document &doc = *C.document;
  if (area.view_serial != doc.view_serial) {
    area.view_serial = doc.view_serial;
    area.view_zoom = doc.view.zoom;
    area.view_center_x = doc.view.center_x;
    area.view_center_y = doc.view.center_y;
    area.view_navigated = doc.view.zoom > 0.0f;
  }
  viewport_draw_contents(area, C);
  /* The project keeps where this viewport looks, to save (moving the view isn't an edit). */
  if (area.view_zoom > 0.0f) {
    doc.view = {area.view_center_x, area.view_center_y, area.view_zoom};
  }
  if (!(C.document->overlays == before) && C.undo) {
    C.undo->mark_changed();
  }
}

static void viewport_draw_contents(Area &area, Context &C)
{
  tool_follow_frame(C);
  Document &doc = *C.document;
  /* Detail rides its curves as they are edited (data/detail.hh). */
  for (const Layer *flat : document_layers_flat(doc)) {
    Layer *l = layer_find(doc, flat->id);
    if (l && l->type == LayerType::Vector && !l->is_reference() && drawing_has_detail(l->drawing)) {
      drawing_detail_sync(l->drawing, true);
    }
  }
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  ImGuiIO &io = ImGui::GetIO();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  if (size.x < 8.0f || size.y < 8.0f) {
    return;
  }
  if (area.view_zoom <= 0.0f ||
      (!area.view_navigated && (size.x != area.view_fit_width || size.y != area.view_fit_height))) {
    view_fit(area, doc, size);
  }

  /* The whole region takes mouse input; header and tool bar items drawn later win where they
   * overlap it. */
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##viewport", size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  /* While the layer eyedropper is on, the viewport only gives it the layer clicked. */
  const bool mouse_here = ImGui::IsItemHovered();
  const bool hovered = mouse_here && !ui_layer_eyedropper_active();
  const bool region_active = ImGui::IsItemActive();

  ViewMapping view{origin, size, {area.view_center_x, area.view_center_y}, area.view_zoom};
  /* In Front Of... / Behind...: the layer pointed at is the one it would go in front of or behind. */
  ui_draw_order_pick_update(C);
  ui_mask_pick_update(C);
  if (mouse_here && wm.draw_order_pick.mode != 0) {
    wm.draw_order_pick.hover = layer_under_mouse(doc, view, io.MousePos, wm.local_view);
  }
  if (mouse_here && ui_layer_eyedropper_active() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    ui_layer_eyedropper_pick(C, layer_under_mouse(doc, view, io.MousePos, wm.local_view));
  }

  /* --- Navigation: wheel zooms at the mouse, middle drag pans, Home frames the camera. --- */
  /* Through the picked camera the view is the camera's: it fits the frame, and moving and zooming the
   * view move and zoom the camera (Blender's Lock Camera to View), one undo step per gesture. */
  Layer &camera = document_camera(doc);
  const bool through_camera = camera.selected;
  if (through_camera) {
    area.view_center_x = camera.position.x;
    area.view_center_y = camera.position.y;
    area.view_zoom = camera_fit_zoom(doc, size);
    area.view_navigated = true;
    view = {origin, size, {area.view_center_x, area.view_center_y}, area.view_zoom};
  }
  /* While points are dragged along by proportional editing, the wheel sizes its reach instead
   * (up for more). */
  const bool proportional_sizing =
      g_modal.active && g_modal.area == area.id && g_modal.proportional && ts.proportional;
  /* The wheel or a middle drag moved the view this frame (only that moves the camera). */
  bool navigated = false;
  if (proportional_sizing && io.MouseWheel != 0.0f) {
    ts.proportional_size = std::clamp(ts.proportional_size * std::pow(1.1f, io.MouseWheel), 1e-4f, 1e6f);
  }
  else if (hovered && io.MouseWheel != 0.0f) {
    const float2 under_mouse = view.to_document(io.MousePos);
    const float factor = std::pow(1.15f, io.MouseWheel);
    view.zoom = std::clamp(view.zoom * factor, VIEW_ZOOM_MIN, VIEW_ZOOM_MAX);
    view.center = under_mouse - (under_mouse - view.center) * (area.view_zoom / view.zoom);
    area.view_navigated = true;
    navigated = true;
  }
  if (region_active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) &&
      (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
  {
    view.center -= float2(io.MouseDelta.x, -io.MouseDelta.y) / view.zoom;
    area.view_navigated = true;
    navigated = true;
  }
  if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
    view_fit(area, doc, size);
    area.view_navigated = false;
    view.center = {area.view_center_x, area.view_center_y};
    view.zoom = area.view_zoom;
  }
  if (through_camera) {
    static bool moving = false;
    static double last_wheel = 0.0;
    if (navigated) {
      camera.position = view.center;
      camera_set_zoom(doc, camera_zoom(doc) * view.zoom / area.view_zoom);
      view.zoom = camera_fit_zoom(doc, size);
      moving = true;
      last_wheel = io.MouseWheel != 0.0f ? ImGui::GetTime() : last_wheel;
    }
    /* The gesture is over once the middle button is up and the wheel has rested a moment. */
    if (moving && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && ImGui::GetTime() - last_wheel > 0.4) {
      moving = false;
      push_undo(C, "Move Camera");
    }
  }
  area.view_center_x = view.center.x;
  area.view_center_y = view.center.y;
  area.view_zoom = view.zoom;

  if (hovered) {
    wm.mouse_in_viewport = true;
    wm.mouse_document = view.to_document(io.MousePos);
  }

  /* --- Background, camera frame, rendered layers. --- */
  ImDrawList *dl = ImGui::GetWindowDrawList();
  /* The canvas is infinite: the background goes on past the camera's frame. */
  dl->AddRectFilled(origin, origin + size,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(doc.background.r, doc.background.g, doc.background.b, 1.0f)));

  const float fb_scale = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
  const int width_px = int(size.x * fb_scale), height_px = int(size.y * fb_scale);
  const float z = view.zoom * fb_scale;
  const Transform2D document_to_pixels{z, 0, 0, z, width_px * 0.5f - view.center.x * z,
                                       height_px * 0.5f - view.center.y * z};
  std::unique_ptr<SceneRenderer> &renderer = g_renderers[area.id];
  if (!renderer) {
    renderer = std::make_unique<SceneRenderer>();
  }
  /* While pointing for In Front Of... / Behind..., the moving layers show where they would go. */
  std::vector<std::pair<Layer *, float>> order_preview;
  const WindowManager::DrawOrderPick &pick = wm.draw_order_pick;
  ui_draw_order_preview_begin(doc, order_preview, pick.hover, pick.mode, pick.layers);
  /* Guides (layers not rendered) are overlays: with them off, the viewport shows what renders. */
  renderer->set_show_guides(doc.overlays.show && doc.overlays.guides);
  renderer->set_local_view(doc, wm.local_view);
  if (const unsigned int texture = renderer->render(doc, width_px, height_px, document_to_pixels, doc.viewport_quality,
                                                             doc.viewport_samples)) {
    dl->AddCallback(premultiplied_blend, nullptr);
    dl->AddImage(ImTextureRef(ImTextureID(intptr_t(texture))), origin, origin + size, ImVec2(0, 1), ImVec2(1, 0));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  }
  ui_draw_order_preview_end(order_preview);
  if (pick.mode != 0 && pick.hover != 0) {
    if (const Layer *target = layer_find(doc, pick.hover)) {
      draw_layer_box(dl, doc, *target, view);
    }
  }
  const ViewportOverlays &ov = C.document->overlays;
  g_keyed_bones = ov.physics_ghosts;
  g_bone_shapes = ov.bone_shapes;
  g_relationship_lines = ov.relationship_lines;
  /* The camera's frame, the canvas outside it darkened, and its name above it (orange while picked). */
  if (ov.show && (ov.camera_frame || through_camera)) {
    ImVec2 frame[4];
    camera_frame_screen(doc, view, frame);
    draw_passepartout(dl, frame, origin, size, doc.camera_passepartout);
    const ImU32 line = through_camera ? COL_WIRE_SELECTED : IM_COL32(0x1d, 0x1d, 0x1d, 255);
    dl->AddPolyline(frame, 4, line, ImDrawFlags_Closed, through_camera ? 2.0f : 1.0f);
    const Color &bg = doc.background;
    const float shade = (0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b) * (1.0f - doc.camera_passepartout);
    const ImU32 text = through_camera ? COL_WIRE_SELECTED
                       : shade > 0.5f ? IM_COL32(0x30, 0x30, 0x30, 255)
                                      : IM_COL32(0xc8, 0xc8, 0xc8, 255);
    dl->AddText(frame[3] + ImVec2(1.0f, -ImGui::GetTextLineHeight() - 2.0f), text, "Camera");
  }
  if (ov.show && ov.grid) {
    draw_grid(dl, view, ts.grid_size);
  }
  /* A file dragged in from Explorer over the viewport: where its picture would sit (fitted to the
   * camera's frame, as Add Media puts it), and what it would make. */
  if (const FileDrag &fd = file_drag(); fd.active && fd.x >= origin.x && fd.x < origin.x + size.x &&
                                        fd.y >= origin.y && fd.y < origin.y + size.y)
  {
    MediaClip clip;
    MediaInfo info;
    for (const std::string &f : fd.files) {
      if (!media_clip_preview(doc, f, 1, &clip, &info)) {
        continue;
      }
      if (info.kind != MediaKind::Audio) {
        Layer fitted;
        fitted.type = LayerType::Media;
        float2 q[4];
        media_quad(doc, fitted, info.source_width, info.source_height, q);
        ImVec2 s[4];
        for (int i = 0; i < 4; i++) {
          s[i] = view.to_screen(q[i]);
        }
        dl->AddConvexPolyFilled(s, 4, IM_COL32(0x6f, 0xb8, 0xff, 50));
        for (int i = 0; i < 4; i++) {
          const ImVec2 a = s[i], b = s[(i + 1) % 4];
          const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
          for (float t = 0.0f; t < len; t += 10.0f) {
            const float u0 = t / std::max(len, 1.0f), u1 = std::min(t + 5.0f, len) / std::max(len, 1.0f);
            dl->AddLine(ImVec2(a.x + (b.x - a.x) * u0, a.y + (b.y - a.y) * u0),
                        ImVec2(a.x + (b.x - a.x) * u1, a.y + (b.y - a.y) * u1), IM_COL32(0x6f, 0xb8, 0xff, 255), 2.0f);
          }
        }
      }
      const std::u8string name = std::filesystem::path(std::u8string(f.begin(), f.end())).filename().u8string();
      const std::string text = "New Media layer: " + std::string(name.begin(), name.end()) + "\n" + media_info_text(info) +
                               ", frames 1 to " + std::to_string(clip.length);
      const ImVec2 ts2 = ImGui::CalcTextSize(text.c_str());
      const ImVec2 at(fd.x + 16.0f, fd.y + 16.0f);
      dl->AddRectFilled(at - ImVec2(5, 4), at + ts2 + ImVec2(5, 4), IM_COL32(0x18, 0x18, 0x18, 230), 4.0f);
      dl->AddText(at, IM_COL32(0xf0, 0xf0, 0xf0, 255), text.c_str());
      break;
    }
  }

  /* --- Editing. --- */
  Layer *layer = nullptr;
  VectorDrawing *drawing = editable_drawing(C, &layer);
  const Transform2D to_document = layer ? layer_world_transform(doc, layer->id) : Transform2D();
  const Transform2D to_layer = to_document.inverted();
  const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
  const float radius = PICK_PIXELS * pixel;
  const float2 mouse_layer = to_layer.apply(view.to_document(io.MousePos));

  /* A K split that's cancelled (Esc or right-click), or a different tool picked meanwhile. */
  if (ts.tool_temporary && (ts.tool != Tool::SplitCurve ||
                            (hovered && (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                                         (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.KeyShift)))))
  {
    if (ts.tool == Tool::SplitCurve) {
      ts.tool = ts.tool_return;
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    }
    ts.tool_temporary = false;
  }

  std::string modal_info;
  if (wm.overlays_toggle_request && hovered) {
    C.document->overlays.show = !C.document->overlays.show;
    wm.overlays_toggle_request = false;
  }
  if (wm.transform_request && hovered && !g_modal.active && !g_bone_modal.active && !g_layer_modal.active &&
      !g_radius_modal.active) {
    g_pending_transform = wm.transform_request;
    g_pending_area = area.id;
    wm.transform_request = 0;
  }
  /* A quick click then E can reach the keymap before the click reaches the tool, so the
   * transform waits for the click to finish (a pen point to be placed); otherwise that click
   * would confirm it at once. Transforms also follow the mouse, so they wait until its
   * position is known (it isn't while another window has the focus). */
  if (g_pending_transform && g_pending_area == area.id && !g_modal.active && !g_drag.active &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      ImGui::IsMousePosValid()) {
    const float2 at = view.to_document(io.MousePos);
    if (g_pending_transform == 'W') {
      radius_modal_start(C, area, at);
    }
    else if (!layer_modal_start(C, area, g_pending_transform, at) && !bone_modal_start(C, area, g_pending_transform, at)) {
      modal_start(C, area, g_pending_transform, at);
    }
    g_pending_transform = 0;
  }
  Layer *armature_active = document_active_layer(doc);
  if (armature_active && !armature_active->is_bone_layer()) {
    armature_active = nullptr;
  }
  if (g_radius_modal.active) {
    if (g_radius_modal.area == area.id) {
      modal_info = radius_modal_update(C, view, dl);
    }
  }
  else if (g_layer_modal.active) {
    if (g_layer_modal.area == area.id) {
      modal_info = layer_modal_update(C, view, dl);
    }
  }
  else if (g_bone_modal.active) {
    if (g_bone_modal.area == area.id) {
      modal_info = bone_modal_update(C, view);
    }
  }
  else if (g_layer_click.active && g_layer_click.area == area.id) {
    layer_pick_update(C, view, dl);
  }
  else if (hovered && !g_modal.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && layer_pick_wanted(C, view)) {
    g_layer_click = {true, area.id, io.MousePos};
    layer_pick_update(C, view, dl);
  }
  else if (ts.tool == Tool::SelectLayer && !g_modal.active) {
    /* The tool: the layer a click would pick, outlined. */
    if (hovered) {
      if (const Layer *target = layer_find(doc, layer_pick_target(C, view, io.MousePos))) {
        draw_layer_box(dl, doc, *target, view);
      }
    }
  }
  else if (armature_active && !g_modal.active) {
    bone_tool_input(C, area, *armature_active, view, hovered, dl);
  }
  else if (ts.tool == Tool::WeightPaint && drawing && !g_modal.active) {
    weight_paint_input(C, *layer, view, hovered, dl);
  }
  else if (ts.tool == Tool::WeightSelect && drawing && !g_modal.active) {
    weight_select_input(C, area, view, hovered, dl);
  }
  else if (ts.tool == Tool::TransformPoints && drawing && !g_modal.active) {
    transform_points_input(C, area, view, hovered, dl);
  }
  else if (g_modal.active && g_modal.area == area.id) {
    modal_info = modal_update(C, view);
    /* Proportional editing's reach, a circle about the middle of what moves, as Blender's. */
    if (g_modal.active && g_modal.proportional && ts.proportional) {
      dl->AddCircle(view.to_screen(g_modal.pivot), ts.proportional_size * view.zoom, IM_COL32(0xff, 0xff, 0xff, 150),
                    0, 1.5f);
    }
    /* A key replayed after the transform may ask for another (click, then E); it starts once
     * the click is over. */
    if (wm.transform_request && !g_modal.active) {
      g_pending_transform = wm.transform_request;
      g_pending_area = area.id;
      wm.transform_request = 0;
    }
    /* The document may have been restored; look the layer up again. */
    drawing = editable_drawing(C, &layer);
  }
  else if (hovered && drawing) {
    /* Shift right-click places the cursor from any tool, as in Blender. */
    if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      ts.cursor = view.to_document(io.MousePos);
    }
    /* New artwork is drawn on the setup frame, so it exists on every frame (as bones are added
     * there); other frames animate what is drawn. */
    const bool draws = ts.tool == Tool::Draw || ts.tool == Tool::Shape;
    if (draws && doc.frame_current != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      report_banner(ReportType::Warning, "Switch to Setup (frame 0) to draw");
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      g_drag = {};
      g_drag.active = true;
      g_drag.area = area.id;
      g_drag.start = io.MousePos;
      switch (ts.tool) {
        case Tool::Draw:
          if (ts.draw_mode == DrawMode::Pen) {
            pen_click(C, *layer, mouse_layer, radius);
          }
          else {
            g_drag.stroke.push_back(mouse_layer);
            g_drag.pressures.push_back(wm.pen_pressure);
          }
          break;
        case Tool::Shape:
          g_drag.shape_start = shape_snap(C, mouse_layer);
          break;
        case Tool::SplitCurve: {
          /* Cuts whichever edited layer's curve is under the mouse. */
          const SplitHit hit = split_pick(C, view, view.to_document(io.MousePos), io.KeyCtrl);
          if (!hit.layer) {
            break;
          }
          VectorDrawing &cut = hit.layer->drawing;
          if (io.KeyCtrl) {
            drawing_dissolve_point(cut, hit.point);
            push_undo(C, "Delete Point");
          }
          else {
            /* At a shape's detail level above 0 the new point is detail of that level. */
            const int level = drawing_knife_level(cut, hit.edge.curve);
            const int p = drawing_detail_split(cut, hit.edge, io.KeyShift ? 0.5f : hit.t, level);
            for (Layer *editing : editable_layers(C)) {
              deselect_all(editing->drawing);
            }
            cut.points[p].selected = true;
            push_undo(C, level > 0 ? "Add Detail Point" : "Split Curve");
          }
          split_curve_done(ts);
          break;
        }
        case Tool::Select: {
          /* Pressing on a shown handle drags it at once, as Blender's tweak does. */
          const HandleHit handle = pick_handle(C, view, io.MousePos, handles_shown(C.document->overlays, ts));
          if (!handle.layer || io.KeyShift || io.KeyCtrl) {
            break;
          }
          const CurvePoint &cp = handle.layer->drawing.curves[handle.curve].points[handle.index];
          if (!(handle.pre ? cp.select_in : cp.select_out)) {
            for (Layer *editing : editable_layers(C)) {
              deselect_all(editing->drawing);
            }
            CurvePoint &picked = handle.layer->drawing.curves[handle.curve].points[handle.index];
            (handle.pre ? picked.select_in : picked.select_out) = true;
          }
          g_drag = {};
          if (modal_start(C, area, 'G', view.to_document(io.MousePos))) {
            g_modal.tweak = true;
          }
          break;
        }
        case Tool::Cursor:
          break;
      }
    }
  }
  else if (hovered && !drawing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ts.tool != Tool::Cursor) {
    const Layer *active = document_active_layer(doc);
    report(ReportType::Info, !active                 ? "Add a layer to draw on (Layers editor, +)"
                             : active->is_media()    ? "This is a Media layer: G, R and S move, turn and scale it; add a vector layer above it to draw over it"
                             : active->is_group()    ? "Select a vector layer to draw on"
                             : active->is_reference() ? "This is a reference layer: Break Reference to draw on it"
                                                      : "This layer is locked");
  }

  /* Dragging and releasing. */
  if (g_drag.active && g_drag.area == area.id && drawing) {
    const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
    switch (ts.tool) {
      case Tool::Select:
        g_drag.box |= dragging;
        if (g_drag.box) {
          dl->AddRectFilled(g_drag.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
          dl->AddRect(g_drag.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
        }
        break;
      case Tool::Draw:
        if (ts.draw_mode == DrawMode::Pen) {
          /* The pen's new point follows the mouse until release. */
          if (dragging && g_drag.pen_point >= 0 && g_drag.pen_point < int(drawing->points.size())) {
            drawing->points[g_drag.pen_point].pos =
                ts.snap_grid || io.KeyCtrl ? snap_to_grid(mouse_layer, ts.grid_size) : mouse_layer;
          }
          break;
        }
        if (distance(mouse_layer, g_drag.stroke.back()) > pixel * 1.5f) {
          g_drag.stroke.push_back(mouse_layer);
          g_drag.pressures.push_back(wm.pen_pressure);
        }
        {
          std::vector<ImVec2> line;
          for (const float2 &p : g_drag.stroke) {
            line.push_back(view.to_screen(to_document.apply(p)));
          }
          const float w = std::max(1.0f, ts.style.line_width * view.zoom * to_document.scale());
          const ImVec4 c(ts.style.line.r, ts.style.line.g, ts.style.line.b, ts.style.line.a);
          dl->AddPolyline(line.data(), int(line.size()), ImGui::ColorConvertFloat4ToU32(c), ImDrawFlags_None, w);
        }
        break;
      case Tool::Cursor:
        ts.cursor = view.to_document(io.MousePos);
        break;
      case Tool::Shape:
        if (dragging) {
          const VectorDrawing preview = primitive_drawing(
              ts.shape, shape_drag_points(ts.shape, g_drag.shape_start, shape_snap(C, mouse_layer)), ts.style,
              ts.draw_fill);
          shape_preview(dl, preview, to_document, view, ts.style.line_width * view.zoom * to_document.scale(),
                        std::clamp(doc.viewport_quality, 1, 64));
        }
        break;
      case Tool::SplitCurve:
        break;
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      switch (ts.tool) {
        case Tool::Select:
          if (g_drag.box) {
            select_box(C, view, g_drag.start, io.MousePos, handles_shown(C.document->overlays, ts));
          }
          else {
            select_click(C, view, view.to_document(io.MousePos), handles_shown(C.document->overlays, ts));
          }
          push_undo(C, "Select");
          break;
        case Tool::Draw:
          if (ts.draw_mode == DrawMode::Pen) {
            push_undo(C, "Add Point");
          }
          else {
            draw_tool_finish(C, *drawing, pixel);
          }
          break;
        case Tool::Shape: {
          /* A click without a drag draws nothing, as in Blender. */
          const ImVec2 moved = io.MousePos - g_drag.start;
          if (moved.x * moved.x + moved.y * moved.y > 9.0f) {
            shape_commit(C, *drawing, ts.shape,
                         shape_drag_points(ts.shape, g_drag.shape_start, shape_snap(C, mouse_layer)));
          }
          else if (io.KeyAlt) {
            /* Alt-drag draws from the centre; an Alt-click picks the layer, as from any tool. */
            layer_pick_click(C, view, g_drag.start, io.KeyShift, false);
          }
          break;
        }
        case Tool::Cursor:
        case Tool::SplitCurve:
          break;
      }
      g_drag = {};
    }
  }
  else if (g_drag.active && g_drag.area == area.id && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    g_drag = {};
  }
  if (!pen_active(ts) && g_pen.layer) {
    pen_finish(C);
  }

  /* Overlays for the layer being edited. */
  if (drawing && (ts.tool == Tool::WeightPaint || ts.tool == Tool::WeightSelect)) {
    draw_weight_overlay(dl, C, *layer, view, std::clamp(doc.viewport_quality, 1, 64));
  }
  else if (!drawing && ov.show && (ov.wireframe || ov.points)) {
    /* A bone layer being worked in: the wires and points of the drawings in its shown bone
     * collections. */
    for (Layer *editing : editable_layers(C)) {
      draw_drawing_overlay(dl, *posed(doc, *editing).drawing, layer_world_transform(doc, editing->id), view,
                           ov.wireframe, ov.points, std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts),
                           ts.select_mode != SelectMode::Edge);
    }
  }
  if (drawing) {
    if (ov.show && (ov.wireframe || ov.points)) {
      /* Every layer being edited shows its wires and points. */
      for (Layer *editing : editable_layers(C)) {
        if (editing->id == layer->id) {
          continue;
        }
        draw_drawing_overlay(dl, *posed(doc, *editing).drawing, layer_world_transform(doc, editing->id), view, ov.wireframe,
                             ov.points && !(ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Freehand),
                             std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts),
                             ts.select_mode != SelectMode::Edge);
      }
      const Posed shown = posed(doc, *layer);
      draw_drawing_overlay(dl, *shown.drawing, to_document, view, ov.wireframe, ov.points && !(ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Freehand),
                           std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts),
                           ts.select_mode != SelectMode::Edge);
    }
    if (hovered && ts.tool == Tool::SplitCurve && !io.KeyCtrl) {
      if (const SplitHit hit = split_pick(C, view, view.to_document(io.MousePos), false); hit.layer) {
        const VectorDrawing &cut = hit.layer->drawing;
        float2 ctrl[4];
        curve_segment_bezier(cut, cut.curves[hit.edge.curve], hit.edge.segment, ctrl);
        const ImVec2 s = view.to_screen(hit.to_document.apply(bezier_eval(ctrl, io.KeyShift ? 0.5f : hit.t)));
        dl->AddCircleFilled(s, 4.0f, COL_POINT_SELECTED);
      }
    }
    if (hovered && pen_active(ts) && ts.snap_points) {
      if (const int p = drawing_pick_point(*drawing, mouse_layer, radius); p >= 0) {
        dl->AddCircle(view.to_screen(to_document.apply(drawing->points[p].pos)), 7.0f, COL_HIGHLIGHT, 0, 2.0f);
      }
    }
  }
  /* Bones, as the Armature overlay says: every visible bone layer's, or only selected ones'. Unless
   * it's Off, the weight tools show the bones they work with, the selected (active) bone solid. */
  if (ov.show) {
    const Layer *painting = (ts.tool == Tool::WeightPaint || ts.tool == Tool::WeightSelect) &&
                                    ov.armature != ArmatureDisplay::Off
                                ? active_armature(C)
                                : nullptr;
    const auto visit = [&](auto &&self, const std::vector<Layer> &list, bool shown) -> void {
      for (const Layer &l : list) {
        const bool visible = shown && l.visible;
        if (l.is_bone_layer() && visible &&
            (ov.armature == ArmatureDisplay::On ||
             (ov.armature == ArmatureDisplay::Selected && (l.selected || l.id == doc.active_layer)) || &l == painting))
        {
          /* Only the bone layer being worked in is drawn solid. */
          g_plain_armature = doc.frame_current != 0 ? 0
                             : g_bone_modal.active               ? g_bone_modal.armature
                             : g_gizmo.mode != GizmoMode::None  ? g_gizmo.armature
                                                                 : 0;
          draw_bones(dl, doc, l, view, l.id != doc.active_layer, &l == painting);
        }
        self(self, l.children, visible);
      }
    };
    visit(visit, doc.layers, true);
  }
  draw_keyed_hover(dl, C, view);
  /* The active layer's masks, dashed (Mask Outlines); a mask symbol or field under the mouse,
   * stronger, whatever the overlay. */
  if (const Layer *masked = document_active_layer(doc); masked && ov.show && ov.mask_outlines) {
    for (const LayerMask &m : layer_mask_rows(doc, *masked)) {
      if (!m.enabled) {
        continue;
      }
      for (const uint64_t id : m.layers) {
        const bool hovered_mask = std::find(wm.mask_hover.begin(), wm.mask_hover.end(), id) != wm.mask_hover.end();
        if (const Layer *source = layer_find(doc, id); source && !hovered_mask) {
          draw_mask_outline(dl, doc, *source, view, mask_cuts(m), false);
        }
      }
    }
  }
  /* A material under the mouse in the Shape panel's list: every shape using it, outlined. */
  if (wm.material_hover != 0 && ImGui::GetFrameCount() - wm.material_hover_frame <= 1) {
    for (const Layer *l : document_layers_flat(doc)) {
      if (l->type != LayerType::Vector || !l->visible) {
        continue;
      }
      const VectorDrawing &d = layer_drawing(doc, *l);
      const Transform2D shape_to_document = layer_world_transform(doc, l->id);
      for (const Shape &shape : d.shapes) {
        if (shape.material != wm.material_hover || shape.hidden) {
          continue;
        }
        for (const Shape &run : shape_runs(d, shape)) {
          std::vector<float2> outline;
          bool closed = false;
          shape_outline(d, run, 8, outline, nullptr, &closed);
          std::vector<ImVec2> pts;
          for (const float2 q : outline) {
            pts.push_back(view.to_screen(shape_to_document.apply(q)));
          }
          if (pts.size() >= 2) {
            dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(0, 0, 0, 160), closed ? ImDrawFlags_Closed : 0, 4.0f);
            dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(0x6f, 0xb8, 0xff, 255), closed ? ImDrawFlags_Closed : 0,
                            2.0f);
          }
        }
      }
    }
  }
  for (const uint64_t id : wm.mask_hover) {
    if (const Layer *source = layer_find(doc, id)) {
      bool outside = false;
      if (const Layer *masked = document_active_layer(doc)) {
        for (const LayerMask &m : layer_mask_rows(doc, *masked)) {
          outside |= mask_cuts(m) && std::find(m.layers.begin(), m.layers.end(), id) != m.layers.end();
        }
      }
      draw_mask_outline(dl, doc, *source, view, outside, true);
    }
  }
  /* Whoever showed a mask set it this frame or the last. */
  wm.mask_hover.clear();
  for (const uint64_t id : wm.keyed_hover.boxes) {
    if (const Layer *l = layer_find(doc, id)) {
      draw_layer_box(dl, doc, *l, view);
    }
  }
  if (doc.frame_current == 0) {
    dl->AddRect(origin + ImVec2(1, 1), origin + size - ImVec2(1, 1), COL_SETUP, 0.0f, 0, 2.0f);
  }
  if (ov.show && ov.cursor) {
    draw_cursor(dl, view.to_screen(ts.cursor));
  }

  /* Right-click with points or handles selected: their context menu, as in Blender. */
  if (hovered && ts.tool == Tool::Select && !io.KeyShift && !g_modal.active && drawing &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right) &&
      !g_right_click_spent)
  {
    bool any = false;
    for (Layer *editing : editable_layers(C)) {
      any |= drawing_selected_alignment(editing->drawing) != -1;
    }
    /* With nothing selected: the active layer's Draw Order. */
    ImGui::OpenPopup(any ? "##point_menu" : "##draw_order");
  }
  if (hovered && wm.menu_request == "draw_order") {
    ImGui::OpenPopup("##draw_order");
    wm.menu_request.clear();
  }
  if (ui_menu_begin("##draw_order")) {
    const Layer *menu_layer = document_active_layer(doc);
    ImGui::TextDisabled("%s", menu_layer ? menu_layer->name.c_str() : "No layer");
    ui_draw_order_menu_items(C, 0);
    ImGui::Separator();
    if (ui_submenu_begin("Mask")) {
      ui_mask_menu_items(C, 0);
      ui_submenu_end();
    }
    ui_menu_end();
  }
  bone_parent_menus(C, hovered);
  ui_bone_collection_menus(C, hovered);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    g_right_click_spent = false;
  }
  /* X on points: Blender's Delete menu. */
  if (hovered && wm.menu_request == "mesh_delete") {
    ImGui::OpenPopup("##mesh_delete");
    wm.menu_request.clear();
  }
  if (ui_menu_begin("##mesh_delete")) {
    ImGui::TextDisabled("Delete");
    ImGui::Separator();
    menu_operator(C, "Points", "mesh.delete_points");
    menu_operator(C, "Edges", "mesh.delete_edges");
    ImGui::Separator();
    menu_operator(C, "Dissolve Points", "mesh.dissolve_points");
    ui_menu_end();
  }
  if (ui_menu_begin("##point_menu")) {
    /* Editing them from another layer (a bone layer, or several layers at once): go to theirs. */
    bool elsewhere = false;
    for (const Layer *editing : editable_layers(C)) {
      elsewhere |= editing->id != doc.active_layer && drawing_selected_alignment(editing->drawing) != -1;
    }
    if (elsewhere) {
      menu_operator(C, "Select Layers of Selected Points", "mesh.select_point_layers");
      ImGui::Separator();
    }
    menu_operator(C, "Align", "mesh.handles_align");
    menu_operator(C, "Unalign", "mesh.handles_unalign");
    ImGui::Separator();
    menu_operator(C, "Peak", "mesh.peak");
    menu_operator(C, "Smooth", "mesh.smooth");
    ImGui::Separator();
    menu_operator(C, "Symmetrize", "mesh.symmetrize");
    /* Detail points picked: back onto the curve of the level below. */
    bool detail = false;
    for (const Layer *editing : editable_layers(C)) {
      detail |= !editing->is_reference() && drawing_detail_selected(editing->drawing);
    }
    if (detail) {
      menu_operator(C, "Smooth to Lower Level", "mesh.detail_smooth");
    }
    ImGui::Separator();
    menu_operator(C, "Delete", "mesh.delete");
    ImGui::Separator();
    if (ui_submenu_begin("Draw Order")) {
      ui_draw_order_menu_items(C, 0);
      ui_submenu_end();
    }
    ui_menu_end();
  }

  /* Header, tool bar, and the info line in the corner. */
  header_draw(area, C, origin, size.x);
  const float header_h = ImGui::GetFrameHeight() + 8.0f;
  toolbar_draw(C, origin, header_h + 6.0f);

  const Layer *active = document_active_layer(doc);
  char info[256];
  /* The selected shapes' detail level too, while they have detail to show. */
  char detail[48] = "";
  if (int level = 0, top = 0; selected_shapes_detail(C, &level, &top) && top > 0) {
    if (level < 0) {
      std::snprintf(detail, sizeof(detail), "  |  Detail -/%d", top);
    }
    else {
      std::snprintf(detail, sizeof(detail), "  |  Detail %d/%d", level, top);
    }
  }
  /* Guides switched off in the overlays: how many are hidden, so a missing layer isn't a mystery. */
  char guides[48] = "";
  if (!doc.overlays.guides) {
    int hidden = 0;
    for (const Layer *l : document_layers_flat(doc)) {
      hidden += l->visible && !l->render_visible ? 1 : 0;
    }
    if (hidden > 0) {
      std::snprintf(guides, sizeof(guides), "  |  %d guide%s hidden", hidden, hidden == 1 ? "" : "s");
    }
  }
  std::snprintf(info, sizeof(info), "Camera%s  |  Frame %d  |  %s%s%s%s%s", wm.local_view.empty() ? "" : " (Local)",
                doc.frame_current,
                active ? active->name.c_str() : "No layer", active && active->is_reference() ? "  (reference)" : "",
                active && !active->render_visible ? "  (guide)" : "", detail, guides);
  const float bar_w = ImGui::GetFrameHeight() * 1.35f + 16.0f;
  action_panel_draw(C, origin, size, bar_w + 4.0f);
  if (ov.show && ov.info) {
    dl->AddText(origin + ImVec2(bar_w + 10.0f, header_h + 8.0f), IM_COL32(0xe6, 0xe6, 0xe6, 255), info);
  }
  if (!modal_info.empty()) {
    const ImVec2 text = ImGui::CalcTextSize(modal_info.c_str());
    const ImVec2 at = origin + ImVec2((size.x - text.x) * 0.5f, header_h + 8.0f);
    dl->AddRectFilled(at - ImVec2(8, 4), at + text + ImVec2(8, 4), IM_COL32(0x18, 0x18, 0x18, 220), 4.0f);
    dl->AddText(at, IM_COL32(0xff, 0xff, 0xff, 255), modal_info.c_str());
  }

  /* Claim the whole region so the window doesn't scroll. */
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(1, 1));
}

}  // namespace oanim
