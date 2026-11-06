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
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "core/log.hh"
#include "anim/controller.hh"
#include "anim/controller_preview.hh"
#include "anim/bone_family.hh"
#include "anim/animation.hh"
#include "anim/deform.hh"
#include "anim/merge.hh"
#include "anim/vector_deform.hh"
#include "anim/draw_order.hh"
#include "anim/follow_point.hh"
#include "anim/switch.hh"
#include "anim/dynamics.hh"
#include "data/driver.hh"
#include "anim/keep_poses.hh"
#include "anim/motion_path.hh"
#include "anim/onion.hh"
#include "anim/sculpt.hh"
#include "assets/asset_library.hh"
#include "assets/draw_presets.hh"
#include "assets/sculpt_presets.hh"
#include "anim/layer_pick.hh"
#include "data/layer_mirror.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/detail.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "editors/brush_editor.hh"
#include "media/media.hh"
#include "editors/editors.hh"
#include "editors/ui.hh"
#include "editors/text_tool.hh"
#include "text/text_outline.hh"
#include "gpu/overlay_renderer.hh"
#include "gpu/scene_renderer.hh"
#include "gpu/tessellate.hh"
#include "ops/edit_ops.hh"
#include "ops/media_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "python/py_runtime.hh"
#include "wm/context.hh"
#include "editors/num_input.hh"
#include "editors/overlay_contrast.hh"
#include "wm/event_system.hh"
#include "wm/pen_input.hh"
#include "wm/file_drop.hh"
#include "wm/keymap.hh"

namespace oanim {


/* Blender's edit-mode colours. */
static constexpr ImU32 COL_VIEWPORT_BACK = IM_COL32(0x3d, 0x3d, 0x3d, 255);
/* What the viewport being drawn last drew, for overlays to pick colours that read on it (editors/overlay_contrast.hh);
 * null when none has been read back yet. */
static const CanvasSample *g_canvas_now = nullptr;
/* `a` blended toward `b` by `t` (ImGui colours, each channel). */
static ImU32 u32_mix(ImU32 a, ImU32 b, float t)
{
  ImU32 out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const float x = float((a >> shift) & 0xff), y = float((b >> shift) & 0xff);
    out |= ImU32(std::clamp(int(x + (y - x) * t + 0.5f), 0, 255)) << shift;
  }
  return out;
}


static ImU32 contrast_u32(const Color &c, float alpha)
{
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, alpha));
}
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

/* Click reach is in screen points: times the display's scale (the Interface Scale, so it feels the same on any
 * screen) and Preferences' Click Reach. Set each frame (and by the test hooks) from the window manager. */
static float g_reach_scale = 1.0f;
static void reach_update(const Context &C)
{
  g_reach_scale = (C.wm ? C.wm->ui_scale : 1.0f) * preferences().click_reach;
}
float click_reach_pixels(float points)
{
  return points * g_reach_scale;
}

/* How near (pixels) a line or a bone's edge picks. */
static float pick_line_pixels()
{
  return click_reach_pixels(9.0f);
}

/* How far from a point (or handle's end) drawn `size` pixels across a click still picks it. */
static float pick_pixels(float size)
{
  /* Generous, as the nearest point or handle within reach wins: a click needn't land on one. */
  return click_reach_pixels(std::max(20.0f, size * 0.5f + 10.0f));
}
/* Motion path dots, keys and handle tips. */
static float pick_path_pixels()
{
  return click_reach_pixels(14.0f);
}
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

static bool modal_key_take(Context &C, const char *type, bool any_modifiers = false);

/* The mouse as G, R and S follow it: held Shift, it moves a tenth as far (Blender's precision), from
 * wherever it was when Shift went down, so nothing jumps as Shift is pressed or let go. A Shift still held
 * from the key that started it (Shift D, Shift E) counts only once let go and pressed again, as in Blender:
 * otherwise the copy would crawl until the key came up. */
struct PreciseMouse {
  float2 raw, at;
  bool armed = true;
  void reset(float2 mouse)
  {
    raw = at = mouse;
    /* No UI in background runs (tests): nothing held. */
    armed = !ImGui::GetCurrentContext() || !ImGui::GetIO().KeyShift;
  }
  float2 update(float2 mouse, bool shift)
  {
    armed |= !shift;
    at += (mouse - raw) * (shift && armed ? 0.1f : 1.0f);
    raw = mouse;
    return at;
  }
};

struct ModalTransform {
  /* The mouse with Shift's precision (G, R and S follow this). */
  PreciseMouse precise;
  bool active = false;
  int area = 0;
  char kind = 'G';
  bool extrude = false;
  /* An extrude's new points, by layer: their segments' handles follow them. */
  std::map<uint64_t, std::vector<int>> extruded;
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
    /* A handle moved by itself: how much shorter it shows than it is stored. */
    float shown_scale = 1.0f;
    /* A handle as it is stored, from its point: a move changes it by as much as it shows moved. What shows is the handle
     * as a deform bends it, which no skin takes back to this. */
    float2 stored_from;
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
  /* Started by dragging a point (tweak too): its undo step is a Move. */
  bool tweak_points = false;
  /* Only handles move: S lengthens and R turns each handle about its own point. */
  bool about_points = false;
  /* Points are dragged along by proportional editing (its circle shows, the wheel sizes it). */
  bool proportional = false;
  /* G pressed again: the points slide along their curves instead (drawing_slide_points). */
  bool slide = false;
  float slide_factor = 0.0f;
};

/* What a press in the Select tool grabs to drag (Blender's tweak): a point, or in Edges mode a segment's two points.
 * No layer: the drag box-selects. */
struct PressGrab {
  uint64_t layer = 0;
  std::vector<int> points;
  ShapeEdge edge{-1, 0};
  /* How far (pixels) the mouse must go before it moves: further with a pen, so a tap never nudges. */
  float threshold = 3.0f;
  /* How far (pixels) the press was from the point grabbed: a handle in reach takes the press only if no nearer. */
  float distance = FLT_MAX;
};

struct ToolDrag {
  bool active = false;
  int area = 0;
  ImVec2 start;
  bool box = false;
  PressGrab grab;
  /* Draw tool: the stroke so far, and what it will make. */
  InkStroke ink;
  DrawMode ink_mode = DrawMode::Freehand;
  /* The pen's touch the stroke follows (0: the mouse's), and how many of its samples are in. */
  uint64_t pen_stroke = 0;
  size_t pen_taken = 0;
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
static void sculpt_header(Context &C);
static void draw_header(Context &C);
static OpArgs weight_args();
static OpArgs weight_automatic_args(Context &C, bool selected_bones);
static Layer *weight_armature(Context &C);
static bool bone_picked(const Bone &bone);
static std::vector<uint64_t> editable_layer_ids(Context &C);

/* A layer's own drawing where its points show: moved by the bones (Blender's "on cage"). Points
 * keep their indices, so what is picked here is the same point in the stored drawing. */
struct Posed {
  const VectorDrawing *drawing = nullptr;
  /* Stored -> shown, per point. */
  std::vector<Transform2D> skins;
  bool moved = false;
  VectorDrawing storage;
  /* With Resample's points between the drawing's own, bent where they are: the curves as they show, which the wire
   * goes along (see posed_wires). `drawing` keeps the drawing's own points, to edit. */
  VectorDrawing wires;
};

/* The curves the wire and the picking of lines go along. */
static const VectorDrawing *posed_wires(const Posed &shown)
{
  return shown.wires.curves.empty() ? nullptr : &shown.wires;
}

static Posed posed(const Document &doc, const Layer &layer)
{
  Posed r;
  r.drawing = &layer.drawing;
  /* Nothing can move it (no Armature modifier, not a reference, following no bone, nor in a group that
   * does): as stored, without working out each point's skin. */
  const bool armature = std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [](const Modifier &m) {
    return m.type == ModifierType::Armature && m.show_viewport;
  });
  const bool deformed = layer_has_group_deforms(doc, layer, false) ||
                        std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [](const Modifier &m) {
                          return modifier_is_deform(m.type) && m.show_viewport && m.deform_layer != 0;
                        });
  const bool resampled = std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [](const Modifier &m) {
    return m.type == ModifierType::Resample && m.show_viewport;
  });
  if (!armature && !deformed && !layer.is_reference() && layer.followed_bone() == 0 && !layer.points_follow_bone() &&
      !layer_binding_group(doc, layer.id))
  {
    r.skins.assign(layer.drawing.points.size(), Transform2D());
    return r;
  }
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
  if (deformed || resampled) {
    /* A deform modifier bends the segments between the points too: each as one Bezier bent as near as it can be
     * (the wire leaves out the points Precision adds), its handles stored for drawing (same curves, the points'
     * moves in the skins). Resample's points bend with the rest, and no segment between the drawing's own could
     * follow that: the wire goes through them, and the drawing's own points keep their handles, whole, to edit. */
    VectorDeformWireScope wire;
    VectorDrawing shown = layer_evaluated_drawing(doc, layer, false);
    /* Fitted by a Resample Self: the wire takes the fit, the handles stay as stored (layer_handles_fitted). */
    const bool fitted = layer_handles_fitted(doc, layer);
    bool added = fitted;
    for (size_t c = 0; c < r.storage.curves.size() && c < shown.curves.size(); c++) {
      Curve &curve = r.storage.curves[c];
      if (curve.points.size() != shown.curves[c].points.size()) {
        added = true;
        continue;
      }
      for (size_t k = 0; deformed && !fitted && k < curve.points.size(); k++) {
        curve.points[k].handle_in = shown.curves[c].points[k].handle_in;
        curve.points[k].handle_out = shown.curves[c].points[k].handle_out;
        curve.points[k].handles_set = true;
      }
    }
    if (added && !drawing_has_detail(layer.drawing) && shown.curves.size() >= r.storage.curves.size()) {
      shown.curves.resize(r.storage.curves.size());
      r.wires = std::move(shown);
    }
    if (fitted) {
      /* Each handle's end where the stack puts it. */
      std::vector<StackPlace> ends;
      for (const Curve &curve : layer.drawing.curves) {
        for (int k = 0; k < int(curve.points.size()); k++) {
          ends.push_back({curve.points[size_t(k)].point, curve_handle(layer.drawing, curve, k, true)});
          ends.push_back({curve.points[size_t(k)].point, curve_handle(layer.drawing, curve, k, false)});
        }
      }
      const std::vector<float2> placed = layer_places_shown(doc, layer, ends);
      size_t next = 0;
      for (Curve &curve : r.storage.curves) {
        for (CurvePoint &cp : curve.points) {
          const float2 at = r.storage.points[size_t(cp.point)].pos;
          cp.handle_in = placed[next++] - at;
          cp.handle_out = placed[next++] - at;
          cp.handles_set = true;
        }
      }
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
   * and Strokes modes pick lines, not points, so their handles go too; and the Transform Points box
   * works on the points as a whole. */
  if (!ov.show || !ov.points || (ts.tool == Tool::Draw && ts.draw_mode != DrawMode::Pen) ||
      ts.tool == Tool::WeightPaint || ts.tool == Tool::WeightSelect || ts.tool == Tool::TransformPoints ||
      ts.tool == Tool::TransformLayer || (ts.tool == Tool::MotionPath && ts.path_edit_layer) ||
      ts.tool == Tool::Sculpt || ts.select_mode == SelectMode::Edge || ts.select_mode == SelectMode::Stroke)
  {
    return HandleDisplay::None;
  }
  return ov.handles;
}

/* Whether the edited drawings' points show: not while drawing freehand or while a gizmo is dragged, and in
 * Sculpt only with Selected Only (as Blender's sculpt modes show points only with the selection mask on). */
static bool point_gizmo_busy();
static bool points_overlay_shown(const ViewportOverlays &ov, const ToolSettings &ts)
{
  /* Transform Layer moves the layer as a whole: its points would only look picked. */
  return ov.points && !(ts.tool == Tool::Draw && ts.draw_mode != DrawMode::Pen) &&
         !(ts.tool == Tool::Sculpt && !ts.sculpt_selected_only) && ts.tool != Tool::TransformLayer &&
         !(ts.tool == Tool::MotionPath && ts.path_edit_layer) && !point_gizmo_busy();
}

/* The onion skins' ghosts: worked out again only when the document, the frame or the ghosts change. */
static void draw_onion_skins(Context &C, const ViewMapping &view, ImDrawList *dl)
{
  const Document &doc = *C.document;
  const std::vector<uint64_t> layers = onion_layers(doc, editable_layers(C));
  const std::vector<OnionGhost> ghosts = onion_ghosts(doc, layers);
  static struct {
    const Document *doc = nullptr;
    uint64_t changes = ~uint64_t(0);
    int frame = 0;
    std::vector<uint64_t> layers;
    std::vector<std::pair<int, int>> ghosts;
    std::vector<OnionWires> wires;
  } cache;
  std::vector<std::pair<int, int>> key;
  for (const OnionGhost &g : ghosts) {
    key.emplace_back(g.frame, g.after ? g.distance : -g.distance);
  }
  const uint64_t changes = C.undo ? C.undo->change_count() : 0;
  if (cache.doc != &doc || cache.changes != changes || cache.frame != doc.frame_current || cache.layers != layers ||
      cache.ghosts != key)
  {
    cache.doc = &doc;
    cache.changes = changes;
    cache.frame = doc.frame_current;
    cache.layers = layers;
    cache.ghosts = key;
    cache.wires = onion_wires(doc, layers, ghosts);
  }
  /* The furthest first, so the nearest show on top. */
  std::vector<const OnionWires *> order;
  for (const OnionWires &w : cache.wires) {
    order.push_back(&w);
  }
  std::stable_sort(order.begin(), order.end(),
                   [](const OnionWires *a, const OnionWires *b) { return a->ghost.distance > b->ghost.distance; });
  std::vector<ImVec2> screen;
  for (const OnionWires *w : order) {
    const Color &c = w->ghost.after ? doc.onion_color_after : doc.onion_color_before;
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a * onion_ghost_alpha(doc, w->ghost)));
    for (const std::vector<float2> &line : w->lines) {
      screen.clear();
      for (const float2 p : line) {
        screen.push_back(view.to_screen(p));
      }
      dl->AddPolyline(screen.data(), int(screen.size()), col, ImDrawFlags_None, 1.4f);
    }
  }
}

static void push_undo(Context &C, const char *name);

/* The Motion Path tool's drag: a dot pressed (its frame gone to, its key selected), then moved. */
static struct {
  bool active = false;
  bool dragging = false;
  /* Alt Shift on a dot between keys: a breakdown slid along the arc; Alt alone: a key added there. */
  bool breakdown = false;
  bool adding = false;
  bool on_key = false;
  MotionPathTarget target;
  int frame = 0;
  /* The range drawn when it started, kept while dragging so the path doesn't jump about under the mouse. */
  int first = 0, last = 0;
  ImVec2 press;
  /* The arc between the keys either side (document space), for a breakdown. */
  std::vector<float2> arc;
  /* A path handle pressed instead of a dot: 1 its key's in handle, 2 its out; `offset` from where the path is drawn to
   * document space on its key's frame (zero unless drawn relative to something). */
  int handle = 0;
  float2 offset;
  /* A control point dragged instead (>= 0): which of the stretch ending on `frame`, moved as seen on `near`. */
  int control = -1;
  int near_frame = 0;
  Document snapshot;
  /* Each update changes what's drawn without an undo step: this tells the cache. */
  uint64_t serial = 0;
} g_path_drag;

/* The control point under the mouse (set as the paths draw): its stretch's end key, which one, its nearest frame and
 * where it is drawn less where it is. */
struct PathControlHover {
  MotionPathTarget target;
  int end = 0, index = 0, near_frame = 0;
  float2 offset;
};
static std::optional<PathControlHover> g_path_control_hover;

/* Where along `arc` (0 at its start, 1 at its end, by length) the nearest place to `at` is. */
static float arc_fraction(const std::vector<float2> &arc, float2 at)
{
  float total = 0.0f, best = std::numeric_limits<float>::max(), best_along = 0.0f;
  for (size_t i = 1; i < arc.size(); i++) {
    const float2 a = arc[i - 1], ab = arc[i] - a;
    const float len = length(ab);
    const float t = len > 1e-9f ? std::clamp(((at.x - a.x) * ab.x + (at.y - a.y) * ab.y) / (len * len), 0.0f, 1.0f)
                                : 0.0f;
    const float d = length(a + ab * t - at);
    if (d < best) {
      best = d;
      best_along = total + len * t;
    }
    total += len;
  }
  return total > 1e-9f ? best_along / total : 0.0f;
}

/* Arguments naming a target for the motion path operators. */
static OpArgs motion_path_target_args(const Document &doc, const MotionPathTarget &t)
{
  OpArgs args = {{"layer", std::to_string(t.layer)}};
  if (const Layer *l = layer_find(doc, t.layer)) {
    for (const Bone &b : l->bones) {
      if (b.id == t.bone) {
        args["bone"] = b.name;
      }
    }
  }
  if (t.point) {
    args["point"] = std::to_string(t.point);
  }
  if (t.head) {
    args["head"] = "1";
  }
  return args;
}

/* A selected position key's path handles as drawn: where its key and handle ends show (where the path is drawn). */
struct PathHandlesShown {
  MotionPathTarget target;
  int frame = 0;
  float2 key, in, out;
  PathHandles mode = PathHandles::Auto;
  bool first = false, last = false;
  /* Drawn position less document space on the key's frame. */
  float2 offset;
  /* An Auto side with no length: drawn short (PATH_HANDLE_STUB pixels) along these ways, drawn position. */
  bool stub_in = false, stub_out = false;
  float2 stub_in_way, stub_out_way;
};

/* How long, in pixels, an Auto handle with no length is drawn, so it can be grabbed. */
static constexpr float PATH_HANDLE_STUB = 30.0f;

/* The Edit brush's selection: the dots selected (`wm.path_dots`), then the keys selected on the paths traced. */
static std::vector<std::pair<MotionPathTarget, int>> path_edit_selection(Context &C)
{
  std::vector<std::pair<MotionPathTarget, int>> all = C.wm->path_dots;
  for (const auto &k : motion_path_selected_keys(*C.document, motion_path_targets(C))) {
    all.push_back(k);
  }
  return all;
}

static bool path_dot_selected(const WindowManager &wm, const MotionPathTarget &t, int frame)
{
  for (const auto &[dt, df] : wm.path_dots) {
    if (dt.same(t) && df == frame) {
      return true;
    }
  }
  return false;
}

/* Where a dot is drawn on screen: on its path if shown, else where it is on its frame. */
static ImVec2 path_dot_screen(const Document &doc, const ViewMapping &view, const std::vector<MotionPath> &paths,
                              const MotionPathTarget &t, int frame)
{
  for (const MotionPath &p : paths) {
    if (p.target.same(t) && frame >= p.first && frame - p.first < int(p.points.size())) {
      return view.to_screen(p.points[size_t(frame - p.first)]);
    }
  }
  float2 at{0.0f, 0.0f};
  motion_path_position_at(doc, t, frame, at);
  return view.to_screen(at);
}

/* A press on something already selected changes the selection only if it is let go of without a drag (so a drag
 * doesn't lose the others): the select it makes then. */
static std::optional<OpArgs> g_path_select_on_release;
/* After a move (G R S, a drag, a brush stroke) the dots' ghost waits for the mouse to move on. */
static struct {
  bool on = false;
  float2 at;
} g_path_hover_quiet;

/* `moving`: something is being moved now. Returns it; keeps the ghost quiet until the mouse leaves where the move
 * ended. */
bool path_hover_quiet_update(bool moving, float2 mouse)
{
  auto &q = g_path_hover_quiet;
  if (moving) {
    q.on = true;
    q.at = mouse;
  }
  else if (q.on && (std::fabs(mouse.x - q.at.x) > 2.0f || std::fabs(mouse.y - q.at.y) > 2.0f)) {
    q.on = false;
  }
  return moving;
}

bool path_hover_quiet()
{
  return g_path_hover_quiet.on;
}

static bool path_hover_quiet_update(bool moving)
{
  return path_hover_quiet_update(moving, float2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y));
}
/* A plain press on the dot of the frame shown, over the bone or point it is the path of: a drag moves the path there,
 * a click (let go without moving) picks the bone or point, at this place. */
static std::optional<ImVec2> g_path_pick_on_release;

/* The Edit brush's dot drag: a dot between keys slid along its path, from the document as it was pressed each time
 * the mouse moves (or the wheel sizes the proportional reach), one undo step; Esc or a right click cancels. */
static struct {
  bool active = false;
  bool dragging = false;
  bool changed = false;
  bool again = false;
  MotionPathTarget target;
  int frame = 0;
  ImVec2 press;
  float2 offset;
  Document snapshot;
} g_path_slide;

/* Ends a drag, a slide or a modal: the paths may take a new range again. */
static void path_drag_finish()
{
  const uint64_t serial = g_path_drag.serial + 1;
  g_path_drag = {};
  g_path_drag.serial = serial;
}

static bool motion_path_pick_click(Context &C, const ViewMapping &view, ImVec2 at, bool extend, bool apply = true);
static bool motion_path_vector_active();
static bool vector_under(Context &C, const ViewMapping &view, ImVec2 mouse);
static bool motion_path_vector_press(Context &C, Area &area, const ViewMapping &view, ImVec2 at);
static void motion_path_vector_update(Context &C, Area &area, const ViewMapping &view, ImDrawList *dl);

static void motion_path_slide_update(Context &C, const ViewMapping &view)
{
  Document &doc = *C.document;
  ToolSettings &ts = C.wm->tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  auto &g = g_path_slide;
  const auto finish = [&]() {
    g.active = false;
    g.snapshot = Document();
    path_drag_finish();
  };
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    doc = std::move(g.snapshot);
    g_right_click_spent = true;
    g_path_select_on_release.reset();
    g_path_pick_on_release.reset();
    finish();
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const bool clicked = !g.dragging;
    if (g.changed) {
      push_undo(C, "Slide Path Dot");
    }
    finish();
    if (clicked && g_path_pick_on_release) {
      motion_path_pick_click(C, view, *g_path_pick_on_release, false);
    }
    else if (clicked && g_path_select_on_release) {
      operator_call(C, "anim.motion_path_select", *g_path_select_on_release);
    }
    g_path_select_on_release.reset();
    g_path_pick_on_release.reset();
    return;
  }
  if (ts.proportional && (ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_PageDown))) {
    ts.path_proportional_size = std::clamp(
        ts.path_proportional_size + (ImGui::IsKeyPressed(ImGuiKey_PageUp) ? 1.0f : -1.0f), 1.0f, 500.0f);
    g.again = true;
  }
  const float dx = io.MousePos.x - g.press.x, dy = io.MousePos.y - g.press.y;
  if (!g.dragging && dx * dx + dy * dy < 3.0f * 3.0f) {
    return;
  }
  if (!g.dragging || io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || g.again) {
    g.dragging = true;
    g.again = false;
    PathSlide slide;
    slide.proportional = ts.proportional;
    slide.size = ts.path_proportional_size;
    slide.falloff = ts.proportional_falloff;
    doc = g.snapshot;
    g.changed = motion_path_slide(doc, g.target, g.frame, view.to_document(io.MousePos) - g.offset, slide);
    g_path_drag.serial++;
  }
}


/* The Motion Path tool's mouse with Edit. A press on a key or a dot selects it (Shift adds or takes it out, a double
 * click also goes to its frame; on empty space, nothing is selected). Dragging a key moves it, a dot between keys
 * slides it along its path (with proportional editing), a selected key's handle bends the path (Alt breaks the
 * pair). Alt on the path adds a key there, a drag moving it (Ctrl: onto the line between the keys; Alt Shift: a
 * breakdown along the arc). Edits happen on the dot's own frame, the frame shown staying; Esc cancels. */
PathPress motion_path_press_kind(bool on_handle, bool on_path, bool on_frame_shown, bool thing_under, bool alt)
{
  /* The path first (the tool edits paths: a dot drawn over a bone doesn't lose its click), but the dot of the frame
   * shown sits on the bone or point it traces, so there a click picks that and a drag moves the path. */
  if (on_handle) {
    return PathPress::Handle;
  }
  if (on_path) {
    return !alt && on_frame_shown && thing_under ? PathPress::PathOrPick : PathPress::Path;
  }
  return thing_under ? PathPress::Pick : PathPress::Deselect;
}

static void motion_path_tool_input(Context &C, Area &area, const ViewMapping &view, const MotionPath *hover_path,
                                   int hover_frame, int first, int last, const PathHandlesShown *hover_handle,
                                   bool hover_out, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  ImGuiIO &io = ImGui::GetIO();
  auto &g = g_path_drag;
  if (motion_path_vector_active()) {
    motion_path_vector_update(C, area, view, dl);
    return;
  }
  if (g_path_slide.active) {
    motion_path_slide_update(C, view);
    return;
  }
  if (!g.active && g_path_control_hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    const PathControlHover hc = *g_path_control_hover;
    OpArgs pick = motion_path_target_args(doc, hc.target);
    pick["frame"] = std::to_string(hc.end);
    pick["control"] = std::to_string(hc.index);
    if (io.KeyShift) {
      pick["extend"] = "1";
      operator_call(C, "anim.motion_path_handle_select", pick);
      return;
    }
    /* Picked alone, and a drag moves it. */
    operator_call(C, "anim.motion_path_handle_select", pick);
    g.active = true;
    g.dragging = false;
    g.breakdown = false;
    g.adding = false;
    g.on_key = false;
    g.handle = 0;
    g.control = hc.index;
    g.near_frame = hc.near_frame;
    g.target = hc.target;
    g.frame = hc.end;
    g.first = first;
    g.last = last;
    g.press = io.MousePos;
    g.offset = hc.offset;
    g.snapshot = doc;
    return;
  }
  if (!g.active && hover_handle && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    OpArgs pick = motion_path_target_args(doc, hover_handle->target);
    pick["frame"] = std::to_string(hover_handle->frame);
    pick["out"] = hover_out ? "1" : "0";
    if (io.KeyShift) {
      /* Shift: in or out of the handles picked, nothing dragged. */
      pick["extend"] = "1";
      operator_call(C, "anim.motion_path_handle_select", pick);
      return;
    }
    /* Picked alone (keys, dots and other handles let go), and a drag moves it. */
    operator_call(C, "anim.motion_path_handle_select", pick);
    g.active = true;
    g.dragging = false;
    g.breakdown = false;
    g.adding = false;
    g.on_key = true;
    g.target = hover_handle->target;
    g.frame = hover_handle->frame;
    g.first = first;
    g.last = last;
    g.press = io.MousePos;
    g.handle = hover_out ? 2 : 1;
    g.offset = hover_handle->offset;
    g.snapshot = doc;
    return;
  }
  if (!g.active) {
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      return;
    }
    /* Anything but a path handle pressed: the handles picked are let go (never picked with anything else). */
    C.wm->path_handle_picks.clear();
    /* A bone or point under the press (of the active bone layer and its family, or the drawings being edited). */
    const bool thing_under = !io.KeyAlt && hovered && motion_path_pick_click(C, view, io.MousePos, false, false);
    const PathPress kind = motion_path_press_kind(false, hover_path != nullptr,
                                                  hover_path && hover_frame == doc.frame_current, thing_under,
                                                  io.KeyAlt);
    g_path_pick_on_release.reset();
    /* Off the paths the drawing is edited as with the Select tool: a point or handle clicked or dragged, a line
     * clicked, a box dragged from empty space. */
    if ((kind == PathPress::Pick || kind == PathPress::Deselect) && hovered && !io.KeyAlt &&
        motion_path_vector_press(C, area, view, io.MousePos))
    {
      return;
    }
    if (kind == PathPress::Pick) {
      /* A bone: picked, as the Select tool's click (Shift adds), so its path shows. */
      motion_path_pick_click(C, view, io.MousePos, io.KeyShift);
      return;
    }
    if (!hover_path) {
      /* A click on nothing: no path keys or dots selected, as in Blender; the bones and points picked stay, so
       * their paths do. */
      if (hovered && !io.KeyShift && !io.KeyCtrl && !io.KeyAlt) {
        operator_call(C, "anim.motion_path_select", {{"none", "1"}});
      }
      return;
    }
    const bool pick_on_click = kind == PathPress::PathOrPick && !io.KeyShift;
    const MotionPathTarget target = hover_path->target;
    /* Frame 0's key, the setup pose: picked for its out handle, but never moved (nor a key added on it). */
    const bool on_setup = hover_frame == 0 && motion_path_is_key(doc, target, 0);
    const bool on_key = on_setup ||
                        std::binary_search(hover_path->keys.begin(), hover_path->keys.end(), hover_frame);
    OpArgs args = motion_path_target_args(doc, target);
    args["frame"] = std::to_string(hover_frame);
    /* Drawn where it is relative to the frame shown: the mouse less this is where it goes on the dot's own frame. */
    float2 offset{0.0f, 0.0f};
    float2 own;
    if (hover_frame != doc.frame_current && hover_frame >= hover_path->first &&
        hover_frame - hover_path->first < int(hover_path->points.size()) &&
        motion_path_position_at(doc, target, hover_frame, own))
    {
      offset = hover_path->points[size_t(hover_frame - hover_path->first)] - own;
    }
    if (!io.KeyAlt) {
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        args["go"] = "1";
        operator_call(C, "anim.motion_path_select", args);
        return;
      }
      if (io.KeyShift) {
        args["extend"] = "1";
        operator_call(C, "anim.motion_path_select", args);
        return;
      }
      if (on_setup) {
        if (pick_on_click) {
          /* The setup key never moves: on its bone or point, a click is a pick. */
          motion_path_pick_click(C, view, io.MousePos, false);
        }
        else {
          operator_call(C, "anim.motion_path_select", args);
        }
        return;
      }
      const bool selected = on_key ? motion_path_key_selected(doc, target, hover_frame)
                                   : path_dot_selected(*C.wm, target, hover_frame);
      g_path_select_on_release.reset();
      if (pick_on_click) {
        /* Nothing selected yet: let go without moving, it picks; dragged, it moves the path there. */
        g_path_pick_on_release = io.MousePos;
      }
      else if (selected) {
        g_path_select_on_release = args;
      }
      else {
        operator_call(C, "anim.motion_path_select", args);
      }
      if (!on_key) {
        auto &sl = g_path_slide;
        sl.active = true;
        sl.dragging = false;
        sl.changed = false;
        sl.again = false;
        sl.target = target;
        sl.frame = hover_frame;
        sl.press = io.MousePos;
        sl.offset = offset;
        sl.snapshot = doc;
        /* The paths keep the range they had while the drag goes on. */
        g.active = true;
        g.first = first;
        g.last = last;
        return;
      }
    }
    if (on_setup) {
      return;
    }
    if (io.KeyAlt && !io.KeyShift && !on_key) {
      /* Alt between keys: a control point there (it shapes the path only), picked; a drag moves it. */
      if (operator_call(C, "anim.motion_path_control_add", args) != OpStatus::Finished ||
          C.wm->path_handle_picks.empty())
      {
        return;
      }
      const MotionPathHandlePick made = C.wm->path_handle_picks.front();
      g.active = true;
      g.dragging = false;
      g.breakdown = false;
      g.adding = false;
      g.on_key = false;
      g.handle = 0;
      g.control = made.control;
      g.near_frame = hover_frame;
      g.target = target;
      g.frame = made.frame;
      g.first = first;
      g.last = last;
      g.press = io.MousePos;
      g.offset = offset;
      g.snapshot = doc;
      return;
    }
    g.active = true;
    g.dragging = false;
    g.handle = 0;
    g.target = target;
    g.frame = hover_frame;
    g.first = first;
    g.last = last;
    g.press = io.MousePos;
    g.on_key = on_key;
    g.breakdown = io.KeyAlt && io.KeyShift && !on_key;
    g.adding = io.KeyAlt && !on_key && !g.breakdown;
    g.offset = offset;
    g.snapshot = doc;
    g.arc.clear();
    if (g.breakdown) {
      int before = 0, after = 0;
      motion_path_neighbour_keys(doc, g.target, g.frame, before, after);
      if (before && after) {
        g.arc = motion_paths_make(doc, {g.target}, before, after).front().points;
      }
      else {
        g.breakdown = false;
        g.adding = true;
      }
    }
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (g.dragging) {
      doc = std::move(g.snapshot);
    }
    g_path_select_on_release.reset();
    g_path_pick_on_release.reset();
    path_drag_finish();
    g_right_click_spent = true;
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const MotionPathTarget target = g.target;
    const int frame = g.frame;
    const bool dragged = g.dragging, made = g.adding || g.breakdown;
    if (dragged) {
      push_undo(C, g.control >= 0 ? "Move Path Control Point"
                   : g.handle      ? "Move Motion Path Handle"
                   : g.breakdown ? "Motion Path Breakdown"
                   : g.on_key    ? "Move Motion Path Key"
                                 : "Add Motion Path Key");
    }
    path_drag_finish();
    OpArgs args = motion_path_target_args(doc, target);
    args["frame"] = std::to_string(frame);
    if (made && !dragged) {
      /* Alt click: a breakdown there, as it is. */
      operator_call(C, "anim.motion_path_key_add", args);
    }
    else if (made) {
      /* The key just made, selected. */
      operator_call(C, "anim.motion_path_select", args);
    }
    else if (!dragged && g_path_pick_on_release) {
      motion_path_pick_click(C, view, *g_path_pick_on_release, false);
    }
    else if (!dragged && g_path_select_on_release) {
      operator_call(C, "anim.motion_path_select", *g_path_select_on_release);
    }
    g_path_select_on_release.reset();
    g_path_pick_on_release.reset();
    return;
  }
  const float dx = io.MousePos.x - g.press.x, dy = io.MousePos.y - g.press.y;
  if (!g.dragging && dx * dx + dy * dy < 4.0f * 4.0f) {
    return;
  }
  g.dragging = true;
  float2 at = view.to_document(io.MousePos);
  if (g.control >= 0) {
    /* From where it was, each move: the path through it is worked out again. */
    doc = g.snapshot;
    motion_path_control_set(doc, g.target, g.frame, g.control, g.near_frame, at - g.offset);
    g.serial++;
    return;
  }
  if (g.handle) {
    /* Only the key's handles change: no record, the keys are what's edited. */
    motion_path_handle_set(doc, g.target, g.frame, g.handle == 2, at - g.offset, io.KeyAlt);
    animation_evaluate(doc, doc.frame_current);
    g.serial++;
    return;
  }
  {
    /* Edited on the dot's frame, the frame shown staying. */
    const MotionPathOnFrame on(doc, g.frame);
    if (g.breakdown) {
      motion_path_breakdown(doc, g.target, arc_fraction(g.arc, at));
    }
    else {
      at = at - g.offset;
      if (io.KeyCtrl) {
        motion_path_straighten(doc, g.target, at);
      }
      motion_path_place(doc, g.target, at);
    }
    /* Keyed as it goes, so the path (worked out from keys) follows the mouse. */
    motion_path_record(doc);
    if (g.adding || g.breakdown) {
      /* Alt makes breakdowns, never main keys. */
      motion_path_key_type_set(doc, g.target, g.frame, KeyType::Breakdown);
    }
  }
  g.serial++;
}

static float adjust_reach(float &base_x, bool &slow, float &slow_x, float &slow_reach);

/* Alt S with Edit: the timing around each dot and key selected scaled, the mouse going right spreading the dots out
 * (faster), left bringing them closer (slower); the mouse starts anywhere, so its distance from a dot couldn't say
 * which. Numbers typed set the factor, as Blender's S. */
static struct {
  bool active = false;
  bool again = false;
  std::vector<std::pair<MotionPathTarget, int>> pivots;
  float press_x = 0.0f;
  float factor = 1.0f;
  NumInput num;
  PreciseMouse precise;
  Document snapshot;
} g_path_scale;
/* Screen pixels to the right for twice the spacing. */
static constexpr float PATH_SCALE_PX = 150.0f;

/* G R S on the selected keys: one key's handles turned or scaled about it (G moves it), several keys moved, turned or
 * scaled about their centre, their handles going with them. Numbers typed, Ctrl snapping, Shift finer, as Blender's
 * transform; a click or Enter keeps it, Esc or a right click cancels. */
static struct {
  bool active = false;
  char kind = 0;
  std::vector<std::pair<MotionPathTarget, int>> keys;
  /* Path handles picked: these instead, each turned or scaled about its key. */
  std::vector<MotionPathHandlePick> handles;
  ImVec2 pivot, press;
  NumInput num;
  PreciseMouse precise;
  Document snapshot;
} g_path_keys;

/* A dashed line from `from` to `to` and `text` by the mouse, as the modals show. */
static void path_modal_overlay(ImDrawList *dl, ImVec2 from, ImVec2 to, const std::string &text)
{
  const float len = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
  for (float t = 0.0f; t < len; t += 12.0f) {
    const float a = t / std::max(len, 1e-3f), b = std::min(t + 6.0f, len) / std::max(len, 1e-3f);
    dl->AddLine(from + (to - from) * a, from + (to - from) * b, IM_COL32(20, 20, 20, 200), 1.5f);
  }
  dl->AddText(to + ImVec2(15.0f, 9.0f), IM_COL32(20, 20, 20, 220), text.c_str());
  dl->AddText(to + ImVec2(14.0f, 8.0f), IM_COL32(255, 255, 255, 255), text.c_str());
}

/* The Edit brush's modals, Alt S and G R S, started by their shortcuts' requests; whether one is running (it takes
 * the mouse, and nothing else does). */
static bool motion_path_edit_modals(Context &C, const ViewMapping &view, ImDrawList *dl, bool hovered, int first,
                                    int last, const std::vector<MotionPath> &paths)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  auto &sc = g_path_scale;
  auto &gk = g_path_keys;
  const bool idle = !sc.active && !gk.active && !g_path_drag.active && !g_path_slide.active;
  const auto start = [&]() {
    C.wm->modal_running = true;
    C.wm->modal_keys.clear();
    g_path_drag.active = true;
    g_path_drag.first = first;
    g_path_drag.last = last;
  };
  const auto stop = [&]() {
    C.wm->modal_running = false;
    C.wm->modal_keys.clear();
    path_drag_finish();
  };
  if (hovered && wm.path_scale_request) {
    wm.path_scale_request = false;
    std::vector<std::pair<MotionPathTarget, int>> pivots = path_edit_selection(C);
    if (idle && !pivots.empty()) {
      sc.active = true;
      sc.again = false;
      sc.pivots = std::move(pivots);
      sc.press_x = io.MousePos.x;
      sc.factor = 1.0f;
      sc.num = NumInput();
      sc.precise.reset(float2{io.MousePos.x, io.MousePos.y});
      sc.snapshot = doc;
      start();
    }
  }
  if (hovered && wm.path_keys_request) {
    const char kind = wm.path_keys_request;
    wm.path_keys_request = 0;
    std::vector<std::pair<MotionPathTarget, int>> keys =
        motion_path_selected_keys(doc, motion_path_targets(C));
    std::vector<MotionPathHandlePick> handles;
    for (const MotionPathHandlePick &hp : wm.path_handle_picks) {
      float2 k, hi, ho;
      if (motion_path_handles(doc, hp.target, hp.frame, k, hi, ho)) {
        handles.push_back(hp);
      }
    }
    if (!handles.empty()) {
      /* Handles picked: they alone; turned and scaled about the first one's key. */
      keys.clear();
      for (const MotionPathHandlePick &hp : handles) {
        keys.push_back({hp.target, hp.frame});
      }
    }
    if (idle && !sc.active && !keys.empty()) {
      gk.active = true;
      gk.kind = kind;
      gk.keys = std::move(keys);
      gk.handles = std::move(handles);
      ImVec2 c(0.0f, 0.0f);
      for (const auto &[t, f] : gk.keys) {
        c = c + path_dot_screen(doc, view, paths, t, f);
      }
      gk.pivot = gk.handles.empty() ? c * (1.0f / float(gk.keys.size()))
                                    : path_dot_screen(doc, view, paths, gk.keys[0].first, gk.keys[0].second);
      gk.press = io.MousePos;
      gk.num = NumInput();
      gk.precise.reset(float2{io.MousePos.x, io.MousePos.y});
      gk.snapshot = doc;
      start();
    }
  }
  if (sc.active) {
    const auto end = [&](bool keep) {
      if (keep) {
        push_undo(C, "Scale Path Timing");
      }
      else {
        doc = std::move(sc.snapshot);
      }
      sc.active = false;
      sc.snapshot = Document();
      stop();
    };
    if (modal_key_take(C, "ESC", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      end(false);
      return true;
    }
    const bool typed = num_input_keys(C, sc.num, 1);
    const bool confirm = modal_key_take(C, "RET", true) || modal_key_take(C, "KEYPAD_ENTER", true) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const float2 m = sc.precise.update(float2{io.MousePos.x, io.MousePos.y}, io.KeyShift);
    float f = std::pow(2.0f, (m.x - sc.press_x) / PATH_SCALE_PX);
    if (io.KeyCtrl) {
      f = std::round(f * 10.0f) / 10.0f;
    }
    if (sc.num.any()) {
      f = sc.num.value(0, 1.0f);
    }
    f = std::max(f, 0.0f);
    if (f != sc.factor || typed || sc.again) {
      sc.factor = f;
      sc.again = false;
      /* Always soft (as the timing chart's): scaling the timing around a dot evenly would kink it at the reach's
       * edges. O is left as it is. */
      PathSlide slide;
      slide.proportional = true;
      slide.size = ts.path_proportional_size;
      slide.falloff = ts.proportional_falloff;
      doc = sc.snapshot;
      for (const auto &[t, fr] : sc.pivots) {
        motion_path_scale_spacing(doc, t, fr, f, slide);
      }
      g_path_drag.serial++;
    }
    /* The dots it scales, tinted by how much. */
    for (const auto &[t, pivot] : sc.pivots) {
      for (const MotionPath &p : paths) {
        if (!p.target.same(t)) {
          continue;
        }
        for (size_t k = 0; k < p.points.size(); k++) {
          const int fr = p.first + int(k);
          const float d = float(std::abs(fr - pivot));
          const float w = proportional_weight(ts.proportional_falloff, d, ts.path_proportional_size, fr);
          if (w > 0.0f && fr != pivot) {
            dl->AddCircle(view.to_screen(p.points[k]), 5.5f, IM_COL32(255, 160, 40, int(60.0f + 195.0f * w)), 0,
                          1.5f);
          }
        }
      }
      dl->AddCircle(path_dot_screen(doc, view, paths, t, pivot), 6.5f, IM_COL32(255, 160, 40, 255), 0, 2.0f);
    }
    path_modal_overlay(dl, ImVec2(sc.press_x, io.MousePos.y), io.MousePos,
                       "Scale Timing " + sc.num.shown(0, sc.factor, "%.2f") + "   (left: closer, right: further)");
    if (confirm) {
      end(true);
    }
    return true;
  }
  if (gk.active) {
    const auto end = [&](bool keep) {
      if (keep) {
        if (!gk.handles.empty()) {
          push_undo(C, gk.kind == 'G'   ? "Move Path Handles"
                       : gk.kind == 'R' ? "Rotate Path Handles"
                                        : "Scale Path Handles");
        }
        else {
          push_undo(C, gk.kind == 'G' ? "Move Path Keys" : gk.kind == 'R' ? "Rotate Path Keys" : "Scale Path Keys");
        }
      }
      else {
        doc = std::move(gk.snapshot);
      }
      gk.active = false;
      gk.snapshot = Document();
      stop();
    };
    if (modal_key_take(C, "ESC", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      end(false);
      return true;
    }
    num_input_keys(C, gk.num, gk.kind == 'G' ? 2 : 1);
    const bool confirm = modal_key_take(C, "RET", true) || modal_key_take(C, "KEYPAD_ENTER", true) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const float2 m = gk.precise.update(float2{io.MousePos.x, io.MousePos.y}, io.KeyShift);
    const float2 md = view.to_document(ImVec2(m.x, m.y)), pd = view.to_document(gk.press),
                 cd = view.to_document(gk.pivot);
    float2 offset = md - pd;
    float angle = std::atan2(md.y - cd.y, md.x - cd.x) - std::atan2(pd.y - cd.y, pd.x - cd.x);
    angle = std::remainder(angle, 6.2831853f);
    const float press_r = std::max(std::sqrt((gk.press.x - gk.pivot.x) * (gk.press.x - gk.pivot.x) +
                                             (gk.press.y - gk.pivot.y) * (gk.press.y - gk.pivot.y)),
                                   1.0f);
    float factor = std::sqrt((m.x - gk.pivot.x) * (m.x - gk.pivot.x) + (m.y - gk.pivot.y) * (m.y - gk.pivot.y)) /
                   press_r;
    if (io.KeyCtrl) {
      const float snap = 5.0f * 3.14159265f / 180.0f;
      angle = std::round(angle / snap) * snap;
      factor = std::round(factor * 10.0f) / 10.0f;
    }
    if (gk.num.any()) {
      offset = float2{gk.num.value(0, offset.x), gk.num.value(1, offset.y)};
      angle = gk.num.value(0, angle * 180.0f / 3.14159265f) * 3.14159265f / 180.0f;
      factor = gk.num.value(0, factor);
    }
    doc = gk.snapshot;
    if (!gk.handles.empty()) {
      motion_path_handles_transform(doc, gk.handles, gk.kind, offset, angle, factor);
    }
    else {
      motion_path_keys_transform(doc, gk.keys, gk.kind, offset, angle, factor);
    }
    g_path_drag.serial++;
    std::string text;
    if (gk.kind == 'G') {
      text = "Move " + gk.num.shown(0, offset.x, "%.3f") + ", " + gk.num.shown(1, offset.y, "%.3f");
    }
    else if (gk.kind == 'R') {
      text = "Rotate " + gk.num.shown(0, angle * 180.0f / 3.14159265f, "%.1f") + " deg";
    }
    else {
      text = "Scale " + gk.num.shown(0, factor, "%.3f");
    }
    text += !gk.handles.empty()                        ? (gk.handles.size() == 1 ? "   (the handle)" : "   (the handles)")
            : gk.keys.size() == 1 && gk.kind != 'G' ? "   (the key's handles)"
                                                    : "";
    dl->AddCircle(gk.pivot, 4.0f, IM_COL32(255, 160, 40, 255), 0, 2.0f);
    path_modal_overlay(dl, gk.kind == 'G' ? gk.press : gk.pivot, io.MousePos, text);
    if (confirm) {
      end(true);
    }
    return true;
  }
  return false;
}

/* The dots selected with Edit, and the one under the mouse: an orange ring and how fast it goes; while one slides
 * with proportional editing, the dots going with it tinted by how much. */
static void draw_selected_path_dots(Context &C, const ViewMapping &view, ImDrawList *dl, const MotionPath *hover_path,
                                    int hover_frame, const std::vector<MotionPath> &paths)
{
  const Document &doc = *C.document;
  const ToolSettings &ts = C.wm->tool_settings;
  const auto &g = g_path_slide;
  std::vector<std::pair<MotionPathTarget, int>> shown = C.wm->path_dots;
  if (g.active) {
    shown.push_back({g.target, g.frame});
  }
  else if (hover_path && !std::binary_search(hover_path->keys.begin(), hover_path->keys.end(), hover_frame)) {
    shown.push_back({hover_path->target, hover_frame});
    /* The timeline's timing chart rings its tick. */
    C.wm->path_hover_target = hover_path->target;
    C.wm->path_hover_frame = hover_frame;
    C.wm->path_hover_tick = ImGui::GetFrameCount();
  }
  else if (ImGui::GetFrameCount() - C.wm->path_hover_tick <= 1) {
    /* A tick hovered in the timing chart: its dot. */
    shown.push_back({C.wm->path_hover_target, C.wm->path_hover_frame});
  }
  for (const auto &[t, frame] : shown) {
    for (const MotionPath &p : paths) {
      if (!p.target.same(t) || frame < p.first || frame - p.first >= int(p.points.size())) {
        continue;
      }
      if (g.active && ts.proportional && t.same(g.target) && frame == g.frame) {
        int before = 0, after = 0;
        motion_path_neighbour_keys(doc, p.target, frame, before, after);
        for (int f = std::max(before + 1, p.first);
             f < (after ? after : p.first + int(p.points.size())) && f - p.first < int(p.points.size()); f++)
        {
          const float w =
              proportional_weight(ts.proportional_falloff, float(std::abs(f - frame)), ts.path_proportional_size, f);
          if (w > 0.0f && f != frame) {
            dl->AddCircle(view.to_screen(p.points[size_t(f - p.first)]), 5.5f,
                          IM_COL32(255, 160, 40, int(60.0f + 195.0f * w)), 0, 1.5f);
          }
        }
      }
      const ImVec2 s = view.to_screen(p.points[size_t(frame - p.first)]);
      dl->AddCircle(s, 6.5f, IM_COL32(20, 20, 20, 220), 0, 3.0f);
      dl->AddCircle(s, 6.5f, IM_COL32(255, 160, 40, 255), 0, 1.6f);
      const float speed = motion_path_dot_speed(doc, p.target, frame);
      if (speed > 0.0f) {
        char text[32];
        std::snprintf(text, sizeof(text), "x%.2f", double(speed));
        dl->AddText(s + ImVec2(9.0f, -18.0f), IM_COL32(20, 20, 20, 220), text);
        dl->AddText(s + ImVec2(8.0f, -19.0f), IM_COL32(255, 255, 255, 255), text);
      }
    }
  }
}

/* A path brush's stroke (anim/path_brush.hh), and F / Shift F sizing it. */
static struct {
  char adjust = 0;
  int adjust_area = 0;
  ImVec2 anchor;
  float base_x = 0.0f;
  bool slow = false;
  float slow_x = 0.0f, slow_reach = 0.0f;
  float was = 0.0f;
  bool active = false;
  ImVec2 last;
  float2 press;
  std::vector<PathGrabKey> grab;
  /* The shape brushes: the Sculpt tool's stroke on the paths that can take it, the others (a bone turning its tip)
   * moved by their keys as before; where the mouse was at the last dab. */
  PathSculptStroke sculpt;
  std::vector<MotionPathTarget> unsculpted;
  float2 sculpt_last;
  /* Slide's dot (the nearest the press), how far its pull reaches in frames, and where it is drawn less where it is. */
  bool slide = false;
  MotionPathTarget slide_target;
  int slide_frame = 0, slide_reach = 1;
  float2 slide_offset;
  /* Whether the stroke has changed anything, so a stroke that touched nothing makes no undo step. */
  bool changed = false;
  /* The brush the stroke paints with (Shift's, held when it began). */
  PathBrush brush = PathBrush::Edit;
  /* Edit's Shift press: no stroke till the mouse moves, and a Shift click on a dot or key (`pick`) adds it to the
   * selection instead. */
  bool pending = false;
  std::optional<OpArgs> pick;
  ImVec2 press_px;
  Document snapshot;
} g_path_brush;
/* Shift F: this many pixels from the anchor is full strength. */
static constexpr float PATH_STRENGTH_PX = 200.0f;

/* The Motion Path tool with a brush other than Edit, or Edit with Shift held (its Blend Speed): its circle, F and
 * Shift F, and a stroke (one undo step; Esc or a right click cancels). Shift swaps in Grab's Smooth and Edit's Blend
 * Speed, each with its own settings. Grab moves what was under it when pressed by how far the mouse has gone; the
 * others dab as the mouse moves, a quarter of the radius apart. With Edit, `pick` is the dot or key under the mouse:
 * a Shift click there (no drag) adds it to the selection. */
static void motion_path_brush_input(Context &C, Area &area, const ViewMapping &view, ImDrawList *dl, bool hovered,
                                    int first, int last, const std::optional<OpArgs> &pick = std::nullopt)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  auto &g = g_path_brush;
  /* F and Shift F size the brush picked (Edit's: Shift's, Blend Speed). */
  PathBrushSettings &picked = ts.path_brush_settings(
      path_brush_has_radius(ts.path_brush) ? ts.path_brush : path_brush_shifted(ts.path_brush));
  if (wm.path_adjust_request && hovered && !g.active) {
    g.adjust = wm.path_adjust_request;
    g.adjust_area = area.id;
    g.was = g.adjust == 'R' ? picked.radius : picked.strength;
    const float reach = g.adjust == 'R' ? picked.radius : picked.strength * PATH_STRENGTH_PX;
    g.anchor = io.MousePos;
    g.base_x = io.MousePos.x - reach;
    g.slow = false;
  }
  wm.path_adjust_request = 0;
  if (g.adjust && g.adjust_area == area.id) {
    const float reach = adjust_reach(g.base_x, g.slow, g.slow_x, g.slow_reach);
    char text[64];
    if (g.adjust == 'R') {
      picked.radius = std::clamp(reach, 2.0f, 1000.0f);
      dl->AddCircle(g.anchor, picked.radius, IM_COL32(0xff, 0xff, 0xff, 230), 64, 1.5f);
      std::snprintf(text, sizeof(text), "Radius %.0f px", double(picked.radius));
    }
    else {
      picked.strength = std::clamp(reach / PATH_STRENGTH_PX, 0.0f, 1.0f);
      dl->AddCircle(g.anchor, PATH_STRENGTH_PX, IM_COL32(0xff, 0xff, 0xff, 120), 64, 1.0f);
      dl->AddCircleFilled(g.anchor, picked.strength * PATH_STRENGTH_PX, IM_COL32(0x47, 0x72, 0xb3, 90), 64);
      std::snprintf(text, sizeof(text), "Strength %.2f", double(picked.strength));
    }
    dl->AddText(g.anchor + ImVec2(-ImGui::CalcTextSize(text).x * 0.5f, -ImGui::GetFontSize() * 0.5f),
                IM_COL32(0xff, 0xff, 0xff, 255), text);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      g.adjust = 0;
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      (g.adjust == 'R' ? picked.radius : picked.strength) = g.was;
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      g.adjust = 0;
    }
    return;
  }
  /* Shift's brush while it is held (as the stroke began, once it has). */
  const PathBrush brush = g.active ? g.brush : io.KeyShift ? path_brush_shifted(ts.path_brush) : ts.path_brush;
  const PathBrushSettings &bs = ts.path_brush_settings(brush);
  /* The circle: red while Ctrl paints the opposite; Shift's brush named under it. */
  const bool invert = io.KeyCtrl && path_brush_is_speed(brush);
  if (hovered || g.active) {
    const ImU32 ring = invert ? IM_COL32(0xff, 0x60, 0x50, 230) : IM_COL32(0xff, 0xff, 0xff, 200);
    dl->AddCircle(io.MousePos, bs.radius, IM_COL32(20, 20, 20, 160), 48, 3.0f);
    dl->AddCircle(io.MousePos, bs.radius, ring, 48, 1.4f);
    if (brush != ts.path_brush) {
      const char *name = path_brush_name(brush);
      const ImVec2 at(io.MousePos.x - ImGui::CalcTextSize(name).x * 0.5f, io.MousePos.y + bs.radius + 4.0f);
      dl->AddText(at + ImVec2(1, 1), IM_COL32(20, 20, 20, 200), name);
      dl->AddText(at, IM_COL32(255, 255, 255, 230), name);
    }
  }
  const float radius = bs.radius / std::max(view.zoom, 1e-9f);
  const float2 mouse = view.to_document(io.MousePos);
  const auto dab = [&]() {
    PathDab d;
    d.center = mouse;
    d.radius = radius;
    d.strength = bs.strength;
    d.invert = invert;
    const std::vector<MotionPathTarget> targets = motion_path_targets(C);
    const int n = brush == PathBrush::Smooth ? motion_path_smooth_dab(doc, targets, first, last, d)
                                             : motion_path_speed_dab(doc, targets, first, last, brush, d);
    g.changed |= n > 0;
    g_path_drag.serial++;
  };
  const auto finish = [&]() {
    const uint64_t serial = g_path_drag.serial + 1;
    g_path_drag = {};
    g_path_drag.serial = serial;
    g.active = false;
    g.pending = false;
    g.pick.reset();
    g.grab.clear();
    g.sculpt = PathSculptStroke();
    g.unsculpted.clear();
    g.snapshot = Document();
  };
  if (!g.active) {
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      return;
    }
    g.active = true;
    g.brush = brush;
    g.changed = false;
    g.snapshot = doc;
    g.last = io.MousePos;
    g.press = mouse;
    g.press_px = io.MousePos;
    /* Edit's Shift: a click on a dot or key adds it to the selection; only a drag paints. */
    g.pending = ts.path_brush == PathBrush::Edit;
    g.pick = pick;
    /* The paths keep the range they had while the stroke goes on. */
    g_path_drag.active = true;
    g_path_drag.first = first;
    g_path_drag.last = last;
    if (g.pending) {
      return;
    }
    if (path_brush_is_sculpt(brush)) {
      std::vector<MotionPathTarget> own;
      for (const MotionPathTarget &t : motion_path_targets(C)) {
        (motion_path_sculptable(doc, t) ? own : g.unsculpted).push_back(t);
      }
      g.sculpt = motion_path_sculpt_begin(doc, own, first, last, motion_path_sculpt_dab_make(brush, bs, mouse, radius));
      g.sculpt_last = mouse;
      if (brush == PathBrush::Grab) {
        g.grab = motion_path_grab_keys(doc, g.unsculpted, first, last, mouse, radius);
      }
    }
    else if (brush == PathBrush::Slide) {
      /* The dot nearest the press, the dots in the circle going with it along the path. */
      g.slide = motion_path_slide_pick(doc, motion_path_targets(C), first, last, mouse, radius,
                                       g.slide_target, g.slide_frame, g.slide_reach, g.slide_offset);
    }
    else {
      dab();
    }
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    doc = std::move(g.snapshot);
    g_right_click_spent = true;
    finish();
    return;
  }
  if (g.pending) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      std::optional<OpArgs> args = g.pick;
      finish();
      if (args) {
        (*args)["extend"] = "1";
        operator_call(C, "anim.motion_path_select", *args);
      }
      return;
    }
    const float mx = io.MousePos.x - g.press_px.x, my = io.MousePos.y - g.press_px.y;
    if (mx * mx + my * my < 9.0f) {
      return;
    }
    g.pending = false;
    g.last = io.MousePos;
    dab();
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (g.changed) {
      push_undo(C, path_brush_name(brush));
    }
    finish();
    return;
  }
  if (brush == PathBrush::Slide) {
    if (g.slide && (io.MousePos.x != g.last.x || io.MousePos.y != g.last.y)) {
      doc = g.snapshot;
      PathSlide slide;
      slide.proportional = true;
      slide.size = float(g.slide_reach);
      slide.falloff = ts.proportional_falloff;
      g.changed |= motion_path_slide(doc, g.slide_target, g.slide_frame, mouse - g.slide_offset, slide);
      g.last = io.MousePos;
      g_path_drag.serial++;
    }
    return;
  }
  if (path_brush_is_sculpt(brush)) {
    SculptDab sd = motion_path_sculpt_dab_make(brush, bs, mouse, radius);
    sd.move = mouse - g.sculpt_last;
    /* As the Sculpt tool: Smooth and Surface Smooth work on while held, a tenth of a second a full step. */
    sd.amount = std::min(1.0f, io.DeltaTime * 10.0f);
    if (brush == PathBrush::Grab) {
      if (sd.move.x != 0.0f || sd.move.y != 0.0f) {
        /* The paths that can't be sculpted: their keys from where they were, following the mouse. */
        if (!g.grab.empty()) {
          doc = g.snapshot;
          g.changed |= motion_path_grab(doc, g.grab, mouse - g.press, bs.strength) > 0;
        }
        g.changed |= motion_path_sculpt_dab(doc, g.sculpt, sd) > 0;
        g_path_drag.serial++;
      }
    }
    else {
      g.changed |= motion_path_sculpt_dab(doc, g.sculpt, sd) > 0;
      const float dx = io.MousePos.x - g.last.x, dy = io.MousePos.y - g.last.y;
      const float spacing = std::max(bs.radius * 0.25f, 2.0f);
      if (brush == PathBrush::Smooth && !g.unsculpted.empty() && dx * dx + dy * dy >= spacing * spacing) {
        PathDab d;
        d.center = mouse;
        d.radius = radius;
        d.strength = bs.strength;
        g.changed |= motion_path_smooth_dab(doc, g.unsculpted, first, last, d) > 0;
        g.last = io.MousePos;
      }
      g_path_drag.serial++;
    }
    g.sculpt_last = mouse;
    return;
  }
  const float dx = io.MousePos.x - g.last.x, dy = io.MousePos.y - g.last.y;
  const float spacing = std::max(bs.radius * 0.25f, 2.0f);
  if (dx * dx + dy * dy >= spacing * spacing) {
    dab();
    g.last = io.MousePos;
  }
}

bool canvas_now_at(float2 at, Color &r)
{
  return g_canvas_now && g_canvas_now->at(at, r);
}

std::vector<PathHandleRun> path_handle_pieces(float2 key, float2 tip, bool tip_picked, float piece,
                                              const std::function<bool(float2, Color &)> &under)
{
  const Color own = color_from_rgb(preferences().path_handle_color);
  const Color orange{1.0f, 160.0f / 255.0f, 40.0f / 255.0f, 1.0f};
  /* ImGui lines take one colour: the fade (and the shades) in pieces short enough not to show. */
  int steps = std::clamp(int(std::ceil(length(tip - key) / std::max(piece, 1e-9f))), 1, 64);
  if (tip_picked) {
    steps = std::max(steps, 12);
  }
  std::vector<PathHandleRun> out;
  ContrastPick pick = ContrastPick::Base;
  for (int i = 0; i < steps; i++) {
    const float from = float(i) / float(steps), to = float(i + 1) / float(steps), mid = (from + to) * 0.5f;
    Color base = own;
    if (tip_picked) {
      base = Color{own.r + (orange.r - own.r) * mid, own.g + (orange.g - own.g) * mid,
                   own.b + (orange.b - own.b) * mid, 1.0f};
    }
    Color seen;
    pick = under && under(key + (tip - key) * mid, seen) ? contrast_pick(seen, base, pick) : ContrastPick::Base;
    const uint32_t color = contrast_u32(contrast_colors_of(base).of(pick), 1.0f);
    if (!out.empty() && out.back().color == color) {
      out.back().to = to;
    }
    else {
      out.push_back({from, to, color});
    }
  }
  return out;
}

bool path_dot_under_handles(float2 at, const std::vector<std::pair<float2, float2>> &handles, float reach)
{
  for (const auto &[a, b] : handles) {
    const float2 ab = b - a;
    const float len2 = dot(ab, ab);
    const float t = len2 > 1e-12f ? std::clamp(dot(at - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    if (length(at - (a + ab * t)) <= reach) {
      return true;
    }
  }
  return false;
}

/* Outside the shot (a path reaching back or on to the keys the shot opens and ends between): see-through. */
static constexpr float PATH_BEFORE_START = 0.35f;
/* A motion path handle's line, pixels: thicker than the path's, so lying along it, it reads as a bar over it. */
static constexpr float PATH_HANDLE_WIDTH = 2.5f;
static ImU32 u32_faded(ImU32 c, float alpha)
{
  return (c & 0x00FFFFFFu) | (ImU32(float(c >> 24) * alpha + 0.5f) << 24);
}

/* A path's colours: the default colour's lighter and darker are the Preferences'; a colour of its own makes its own. */
static ContrastColors path_contrast_colors(const MotionPath &p)
{
  return p.color == path_colors().base ? path_colors() : contrast_colors_of(p.color);
}

std::vector<PathQuietMark> motion_path_quiet_marks(const Document &doc, const MotionPath &p,
                                                   const CanvasSample *under)
{
  std::vector<PathQuietMark> out;
  const ContrastColors colors = path_contrast_colors(p);
  const int step = std::max(1, doc.path_step);
  for (size_t i = 0; i < p.points.size();) {
    size_t run = i + 1;
    while (run < p.points.size() && length(p.points[run] - p.points[i]) < 1e-6f) {
      run++;
    }
    bool shown = false, key = false, breakdown = false;
    for (size_t k = i; k < run; k++) {
      const int f = p.first + int(k);
      const bool on_key = std::binary_search(p.keys.begin(), p.keys.end(), f) ||
                          (f == 0 && motion_path_is_key(doc, p.target, 0));
      breakdown |= on_key && !key && motion_path_key_is_breakdown(doc, p.target, f);
      key |= on_key;
      shown |= on_key || (f - p.first) % step == 0;
    }
    if (shown) {
      Color c;
      const ContrastPick pick = under && under->at(p.points[i], c) ? contrast_pick(c, colors.base)
                                                                     : ContrastPick::Base;
      /* Keys a touch lighter than the dots around them, so they stand out a little; no outlines. */
      const ImU32 dot = contrast_u32(colors.of(pick), 1.0f);
      ImU32 fill = key ? u32_mix(dot, IM_COL32(255, 255, 255, 255), 0.15f) : dot;
      if (motion_path_frame_outside(doc, p.first + int(i))) {
        fill = u32_faded(fill, PATH_BEFORE_START);
      }
      out.push_back({p.first + int(i), p.points[i],
                     key ? (breakdown ? PathQuietMark::Breakdown : PathQuietMark::Key) : PathQuietMark::Dot, fill, 0u,
                     int(run - i)});
    }
    i = run;
  }
  return out;
}

std::vector<PathLinePiece> motion_path_line_pieces(const Document &doc, const MotionPath &p, const CanvasSample *under,
                                                   float piece)
{
  std::vector<PathLinePiece> out;
  if (p.points.size() < 2) {
    return out;
  }
  /* Where the dot goes past its key and back (overshoot) or backs up before its start: dashed, as it goes over the
   * path's continuation; the rest solid. */
  const std::vector<int> over = motion_path_overshoot_frames(doc, p.target);
  const auto is_over = [&](int frame) { return std::binary_search(over.begin(), over.end(), frame); };
  const ContrastColors colors = path_contrast_colors(p);
  const std::function<bool(float2, Color &)> under_at = [&](float2 q, Color &c) { return under && under->at(q, c); };
  /* The step into frame `f` is outside the shot (before Start, or from End on): faded. */
  const auto before = [&](int f) {
    return (doc.frame_start > 0 && f <= doc.frame_start) || f - 1 >= std::max(doc.frame_start, doc.frame_end);
  };
  std::vector<ContrastPiece> pieces;
  std::vector<float2> run = {p.points[0]};
  bool run_before = before(p.first + 1);
  const auto flush = [&]() {
    if (run.size() > 1) {
      pieces.clear();
      contrast_pieces(run, piece, under_at, colors.base, pieces);
      for (const ContrastPiece &k : pieces) {
        out.push_back({k.a, k.b, contrast_u32(colors.of(k.pick), 0.9f * (run_before ? PATH_BEFORE_START : 1.0f)),
                       false});
      }
    }
  };
  for (size_t i = 1; i < p.points.size(); i++) {
    const int f = p.first + int(i);
    if (!is_over(f) && !is_over(f - 1)) {
      if (before(f) != run_before) {
        flush();
        run = {p.points[i - 1]};
        run_before = before(f);
      }
      run.push_back(p.points[i]);
      continue;
    }
    flush();
    Color c;
    const ContrastPick pick = under_at((p.points[i - 1] + p.points[i]) * 0.5f, c) ? contrast_pick(c, colors.base)
                                                                                  : ContrastPick::Base;
    out.push_back({p.points[i - 1], p.points[i],
                   contrast_u32(colors.of(pick), 0.9f * (before(f) ? PATH_BEFORE_START : 1.0f)), true});
    run = {p.points[i]};
    run_before = before(f + 1);
  }
  flush();
  return out;
}

/* Motion paths (anim/motion_path.hh): a dot per frame, red where fast and blue where still, keys as diamonds and a
 * ring on the frame shown; hovering a dot shows the drawing there as a ghost. Worked out again only when the
 * document, the range or what is traced changes. With the Motion Path tool they show whatever the popover says, and
 * take its clicks. */
static void draw_motion_paths(Context &C, Area &area, const ViewMapping &view, ImDrawList *dl, bool hovered)
{
  const Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  for (const std::vector<float2> &line : wm.path_snapshot) {
    std::vector<ImVec2> screen;
    for (const float2 p : line) {
      screen.push_back(view.to_screen(p));
    }
    dl->AddPolyline(screen.data(), int(screen.size()), IM_COL32(220, 220, 220, 90), ImDrawFlags_None, 1.2f);
    for (const ImVec2 &s : screen) {
      dl->AddCircleFilled(s, 1.6f, IM_COL32(220, 220, 220, 90));
    }
  }
  const bool tool = wm.tool_settings.tool == Tool::MotionPath;
  motion_path_tool_on(tool);
  if (!tool && g_path_drag.active) {
    g_path_drag = {};
  }
  if (!doc.motion_paths && !tool) {
    return;
  }
  /* With any brush, Alt click on a bone or a point picks it, Alt Shift adds it, as the Select tool's click does: its
   * path shows. Its own spot wins over the path's dot drawn on it; elsewhere Alt on the path keeps its meaning. Asked
   * before there is a path at all: with nothing picked there may be none to draw. */
  if (tool && hovered && ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !g_path_drag.active && !g_path_slide.active && !g_path_brush.active &&
      motion_path_pick_click(C, view, ImGui::GetIO().MousePos, ImGui::GetIO().KeyShift))
  {
    return;
  }
  const std::vector<MotionPathTarget> targets = motion_path_targets(C);
  {
    std::vector<MotionPathTarget> dotted;
    for (const auto &[t, f] : wm.path_dots) {
      dotted.push_back(t);
    }
    motion_path_dots_picked(dotted);
  }
  /* Own Motion: which paths show only their own motion (those being edited). */
  std::vector<bool> own;
  for (const MotionPathTarget &t : targets) {
    own.push_back(motion_path_own_motion(doc, t));
  }
  int first = 0, last = 0;
  if (wm.playing && wm.play_range_first > 0) {
    first = wm.play_range_first;
    last = wm.play_range_last;
  }
  else if (g_path_drag.active) {
    first = g_path_drag.first;
    last = g_path_drag.last;
  }
  else if (!motion_path_range(doc, targets, first, last)) {
    return;
  }
  static struct {
    const Document *doc = nullptr;
    uint64_t changes = ~uint64_t(0), serial = 0;
    int first = 0, last = 0, frame = 0;
    std::vector<MotionPathTarget> targets;
    std::vector<bool> own;
    PathRelative relative = PathRelative::World;
    uint64_t relative_layer = 0, relative_bone = 0;
    std::vector<MotionPath> paths;
  } cache;
  const uint64_t changes = C.undo ? C.undo->change_count() : 0;
  /* Drawn relative to something, or showing only its own motion (the rest held as now), a path depends on where that
   * is now too. */
  const bool any_own = std::find(own.begin(), own.end(), true) != own.end();
  const int frame = doc.path_relative == PathRelative::World && !any_own ? 0 : doc.frame_current;
  if (cache.doc != &doc || cache.changes != changes || cache.serial != g_path_drag.serial || cache.first != first ||
      cache.last != last || cache.frame != frame || cache.targets != targets || cache.own != own ||
      cache.relative != doc.path_relative ||
      cache.relative_layer != doc.path_relative_layer || cache.relative_bone != doc.path_relative_bone)
  {
    cache.doc = &doc;
    cache.changes = changes;
    cache.serial = g_path_drag.serial;
    cache.first = first;
    cache.last = last;
    cache.frame = frame;
    cache.targets = targets;
    cache.own = own;
    cache.relative = doc.path_relative;
    cache.relative_layer = doc.path_relative_layer;
    cache.relative_bone = doc.path_relative_bone;
    cache.paths = motion_paths_make(doc, targets, first, last);
  }
  const auto u32 = [](const Color &c, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a * alpha));
  };
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  /* With a brush other than Edit the dots aren't picked, and only the brush's circle shows. */
  const bool editing = !tool || wm.tool_settings.path_brush == PathBrush::Edit;
  const bool moving = path_hover_quiet_update(wm.modal_running || g_path_brush.active || motion_path_vector_active() ||
                                              g_path_drag.active || g_path_slide.active);
  const bool hovering = hovered && editing && !moving &&
                        (!ImGui::IsAnyMouseDown() || (tool && ImGui::IsMouseClicked(ImGuiMouseButton_Left)));
  float best = pick_path_pixels() * pick_path_pixels();
  const MotionPath *hover_path = nullptr;
  int hover_frame = 0;
  const int step = std::max(1, doc.path_step);
  /* With the tool, the path handles of selected position keys. */
  std::vector<std::pair<MotionPathTarget, int>> handle_keys;
  /* Two layers: lines and plain dots under (0), keys, rings and handles over (1), so a handle lying along the path
   * reads as one bar over it; the plain dots wait until the handles are known, those a handle covers left out. */
  dl->ChannelsSplit(2);
  struct PlainDot {
    ImVec2 at;
    ImU32 under, fill;
  };
  std::vector<PlainDot> plain_dots;
  for (const MotionPath &p : cache.paths) {
    dl->ChannelsSetCurrent(0);
    std::vector<ImVec2> screen;
    for (const float2 q : p.points) {
      screen.push_back(view.to_screen(q));
    }
    if (p.ghost.size() > 1) {
      /* Own Motion: where it really goes, faint behind the path being edited. */
      std::vector<ImVec2> ghost;
      for (const float2 q : p.ghost) {
        ghost.push_back(view.to_screen(q));
      }
      dl->AddPolyline(ghost.data(), int(ghost.size()), u32(p.color, 0.45f), ImDrawFlags_None, 1.0f);
      for (size_t i = 0; i < ghost.size(); i += size_t(step)) {
        dl->AddCircleFilled(ghost[i], 2.0f, u32(p.color, 0.5f));
      }
    }
    if (doc.path_lines && screen.size() > 1) {
      /* Where the dot goes past its key and back (overshoot) or backs up before its start: dashed, as it goes over
       * the path's continuation; the rest solid. */
      for (const PathLinePiece &k : motion_path_line_pieces(doc, p, g_canvas_now, 6.0f / std::max(view.zoom, 1e-9f))) {
        const ImVec2 a = view.to_screen(k.a), b = view.to_screen(k.b);
        if (!k.dashed) {
          dl->AddLine(a, b, k.color, 1.5f);
          continue;
        }
        for (int d = 0; d < 6; d += 2) {
          dl->AddLine(ImVec2(a.x + (b.x - a.x) * float(d) / 6.0f, a.y + (b.y - a.y) * float(d) / 6.0f),
                      ImVec2(a.x + (b.x - a.x) * float(d + 1) / 6.0f, a.y + (b.y - a.y) * float(d + 1) / 6.0f),
                      k.color, 1.5f);
        }
      }
    }
    if (!tool) {
      /* Only looked at: dots and keys all in the path's colour, nothing lit, nothing under the mouse. */
      for (const PathQuietMark &m : motion_path_quiet_marks(doc, p, g_canvas_now)) {
        const ImVec2 s = view.to_screen(m.at);
        if (m.kind == PathQuietMark::Dot) {
          dl->AddCircleFilled(s, 3.0f, m.fill);
        }
        else {
          const float r = 4.5f;
          const ImVec2 d[4] = {{s.x, s.y - r}, {s.x + r, s.y}, {s.x, s.y + r}, {s.x - r, s.y}};
          if (m.kind == PathQuietMark::Key) {
            dl->AddConvexPolyFilled(d, 4, m.fill);
          }
          else {
            /* A breakdown: hollow, a ring of its colour. */
            dl->AddPolyline(d, 4, m.fill, ImDrawFlags_Closed, 1.6f);
          }
        }
        if (m.held > 1) {
          const std::string held = "x" + std::to_string(m.held);
          dl->AddText(ImVec2(s.x + 5.0f, s.y - 14.0f), m.fill, held.c_str());
        }
      }
      const int now = doc.frame_current - p.first;
      if (now >= 0 && now < int(screen.size())) {
        const ImU32 ring = contrast_u32(path_contrast_colors(p).base, 1.0f);
        dl->AddCircle(screen[size_t(now)], 7.0f, ring, 0, 1.8f);
      }
      continue;
    }
    dl->ChannelsSetCurrent(1);
    /* With the tool, its selected keys (as the timeline has them) outlined. */
    std::map<int, KeyFrameSummary> picked;
    if (tool) {
      if (Layer *layer = layer_find(*C.document, p.target.layer)) {
        picked = layers_key_summary({layer}, motion_path_key_filter(doc, p.target));
      }
    }
    /* Each dot's colour (by time through its stretch, the one being worked on lit), for its selected dots too. */
    std::vector<int> lit_dots;
    for (const auto &[t, f] : wm.path_dots) {
      if (t.same(p.target)) {
        lit_dots.push_back(f);
      }
    }
    const std::vector<Color> dot_colors = motion_path_dot_colors(doc, p, lit_dots);
    /* A dot every `step` frames (and on every key); frames it holds still over make one dot with how many. */
    for (size_t i = 0; i < screen.size();) {
      size_t run = i + 1;
      while (run < screen.size() && length(p.points[run] - p.points[i]) < 1e-6f) {
        run++;
      }
      bool shown = false, key = false, selected = false, breakdown = false;
      for (size_t k = i; k < run; k++) {
        const int f = p.first + int(k);
        /* Frame 0's key too (the setup's): it has an out handle. */
        const bool on_key = std::binary_search(p.keys.begin(), p.keys.end(), f) ||
                            (f == 0 && motion_path_is_key(doc, p.target, 0));
        breakdown |= on_key && !key && motion_path_key_is_breakdown(doc, p.target, f);
        key |= on_key;
        shown |= on_key || (f - p.first) % step == 0;
        if (const auto it = picked.find(f); it != picked.end() && it->second.selected > 0) {
          selected = true;
          if (on_key && editing && motion_path_has_handles(doc, p.target)) {
            handle_keys.push_back({p.target, f});
          }
        }
      }
      if (shown) {
        const ImVec2 s = screen[i];
        const float fade = motion_path_frame_outside(doc, p.first + int(i)) ? PATH_BEFORE_START : 1.0f;
        Color dot = dot_colors[i];
        dot.a *= fade;
        if (key && breakdown) {
          /* A breakdown: a dark blue circle, larger and light blue when selected. */
          const float r = selected ? 5.0f : 4.0f;
          dl->AddCircleFilled(s, r, selected ? IM_COL32(0x8c, 0xc8, 0xff, 255) : IM_COL32(0x1f, 0x5f, 0xd6, 255));
          dl->AddCircle(s, r, selected ? IM_COL32(255, 160, 40, 255) : IM_COL32(20, 20, 20, 220), 0,
                        selected ? 2.0f : 1.0f);
        }
        else if (key) {
          /* Keys plain white, so they stand apart from the coloured dots; selected, larger and ringed orange. */
          const float r = selected ? 6.0f : 4.5f;
          const ImVec2 d[4] = {{s.x, s.y - r}, {s.x + r, s.y}, {s.x, s.y + r}, {s.x - r, s.y}};
          dl->AddConvexPolyFilled(d, 4, u32_faded(IM_COL32(240, 240, 240, 255), fade));
          dl->AddPolyline(d, 4, u32_faded(selected ? IM_COL32(255, 160, 40, 255) : IM_COL32(20, 20, 20, 220), fade),
                          ImDrawFlags_Closed, selected ? 2.0f : 1.0f);
          if (p.first + int(i) == 0) {
            /* The setup pose's key: locked (its out handle still shapes the first stretch), so a hollow centre. */
            const float h = r * 0.45f;
            const ImVec2 c[4] = {{s.x, s.y - h}, {s.x + h, s.y}, {s.x, s.y + h}, {s.x - h, s.y}};
            dl->AddConvexPolyFilled(c, 4, u32_faded(IM_COL32(20, 20, 20, 230), fade));
          }
        }
        else {
          plain_dots.push_back({s, IM_COL32(20, 20, 20, int(200.0f * dot.a)), u32(dot, 1.0f)});
          (void)fade;
        }
        if (run - i > 1) {
          const std::string held = "x" + std::to_string(run - i);
          dl->AddText(ImVec2(s.x + 5.0f, s.y - 14.0f), u32(dot, 0.9f), held.c_str());
        }
        for (size_t k = i; k < run; k++) {
          const float dx = screen[k].x - mouse.x, dy = screen[k].y - mouse.y;
          /* A key wins over a dot about as near (it is drawn over it). */
          const float how_near = std::max(std::sqrt(dx * dx + dy * dy) - (key ? 2.0f : 0.0f), 0.0f);
          if (hovering && how_near * how_near < best) {
            best = how_near * how_near;
            hover_path = &p;
            hover_frame = p.first + int(k);
          }
        }
      }
      i = run;
    }
    const int now = doc.frame_current - p.first;
    if (now >= 0 && now < int(screen.size())) {
      dl->AddCircle(screen[size_t(now)], 7.0f, IM_COL32(20, 20, 20, 200), 0, 3.2f);
      dl->AddCircle(screen[size_t(now)], 7.0f, IM_COL32(255, 255, 255, 240), 0, 1.6f);
    }
  }
  /* A picked handle's key shows its handles though the key isn't selected (a handle is picked alone). */
  if (editing) {
    for (const MotionPathHandlePick &hp : wm.path_handle_picks) {
      const bool traced = std::any_of(cache.paths.begin(), cache.paths.end(),
                                      [&](const MotionPath &p) { return p.target.same(hp.target); });
      if (traced && std::none_of(handle_keys.begin(), handle_keys.end(), [&](const auto &k) {
            return k.first.same(hp.target) && k.second == hp.frame;
          }))
      {
        handle_keys.push_back({hp.target, hp.frame});
      }
    }
  }
  /* Each selected position key's handles: a line from the key to each end, hollow while Auto, filled once dragged
   * (square once broken). Worked out from copies of the document, so kept until something changes. */
  static struct {
    const Document *doc = nullptr;
    uint64_t changes = ~uint64_t(0), serial = 0;
    int frame = -1;
    std::vector<std::pair<MotionPathTarget, int>> keys;
    std::vector<PathHandlesShown> shown;
  } handles;
  if (handles.doc != &doc || handles.changes != changes || handles.serial != g_path_drag.serial ||
      handles.frame != doc.frame_current || handles.keys != handle_keys)
  {
    handles.doc = &doc;
    handles.changes = changes;
    handles.serial = g_path_drag.serial;
    handles.frame = doc.frame_current;
    handles.keys = handle_keys;
    handles.shown.clear();
    for (const auto &[target, f] : handle_keys) {
      PathHandlesShown h;
      h.target = target;
      h.frame = f;
      if (!motion_path_handles(doc, target, f, h.key, h.in, h.out, &h.mode)) {
        continue;
      }
      bool show_in = true, show_out = true;
      for (const MotionPath &p : cache.paths) {
        if (p.target.same(target) && f >= p.first && f - p.first < int(p.points.size())) {
          h.offset = p.points[size_t(f - p.first)] - h.key;
          motion_path_handle_sides(p, f, show_in, show_out);
          if (h.mode == PathHandles::Auto) {
            h.stub_in = show_in && length(h.in - h.key) < 1e-7f && motion_path_handle_stub(p, f, false, h.stub_in_way);
            h.stub_out = show_out && length(h.out - h.key) < 1e-7f &&
                         motion_path_handle_stub(p, f, true, h.stub_out_way);
          }
        }
      }
      h.first = !show_in || (length(h.in - h.key) < 1e-7f && !h.stub_in);
      h.last = !show_out || (length(h.out - h.key) < 1e-7f && !h.stub_out);
      handles.shown.push_back(h);
    }
  }
  std::vector<PathHandlesShown> shown_handles = handles.shown;
  for (PathHandlesShown &h : shown_handles) {
    const float2 at = h.key + h.offset;
    const ImVec2 k = view.to_screen(at);
    const auto stub_end = [&](float2 way) {
      const ImVec2 w = view.to_screen(at + way) - k;
      const float len = std::sqrt(w.x * w.x + w.y * w.y);
      const ImVec2 e = len > 1e-9f ? k + w * (PATH_HANDLE_STUB / len) : k;
      return view.to_document(e) - h.offset;
    };
    if (h.stub_in) {
      h.in = stub_end(h.stub_in_way);
    }
    if (h.stub_out) {
      h.out = stub_end(h.stub_out_way);
    }
  }
  const PathHandlesShown *hover_handle = nullptr;
  bool hover_out = false;
  float handle_best = pick_path_pixels() * pick_path_pixels();
  /* Which tip is under the mouse, first: it is drawn bigger. And the lines, to leave out the dots they cover. */
  std::vector<std::pair<float2, float2>> handle_lines;
  for (const PathHandlesShown &h : shown_handles) {
    const ImVec2 k = view.to_screen(h.key + h.offset);
    for (const bool out : {false, true}) {
      if (out ? h.last : h.first) {
        continue;
      }
      const ImVec2 e = view.to_screen((out ? h.out : h.in) + h.offset);
      handle_lines.push_back({float2{k.x, k.y}, float2{e.x, e.y}});
      const float dx = e.x - mouse.x, dy = e.y - mouse.y;
      if (tool && hovering && dx * dx + dy * dy < handle_best) {
        handle_best = dx * dx + dy * dy;
        hover_handle = &h;
        hover_out = out;
      }
    }
  }
  dl->ChannelsSetCurrent(0);
  for (const PlainDot &d : plain_dots) {
    if (path_dot_under_handles(float2{d.at.x, d.at.y}, handle_lines, PATH_HANDLE_WIDTH * 0.5f + 3.0f)) {
      continue;
    }
    dl->AddCircleFilled(d.at, 3.6f, d.under);
    dl->AddCircleFilled(d.at, 2.8f, d.fill);
  }
  dl->ChannelsSetCurrent(1);
  for (const PathHandlesShown &h : shown_handles) {
    const ImVec2 k = view.to_screen(h.key + h.offset);
    for (const bool out : {false, true}) {
      if (out ? h.last : h.first) {
        continue;
      }
      const ImVec2 e = view.to_screen((out ? h.out : h.in) + h.offset);
      /* Picked (or being dragged): orange. */
      const bool dragged = (g_path_drag.active && g_path_drag.handle == (out ? 2 : 1) &&
                            g_path_drag.target.same(h.target) && g_path_drag.frame == h.frame) ||
                           std::find(wm.path_handle_picks.begin(), wm.path_handle_picks.end(),
                                     MotionPathHandlePick{h.target, h.frame, out}) != wm.path_handle_picks.end();
      /* Under the mouse (or being dragged): a little bigger and brighter, so it shows what a click takes. */
      const bool lit = dragged || (hover_handle == &h && hover_out == out);
      const auto brighten = [lit](ImU32 c) { return lit ? u32_mix(c, IM_COL32(255, 255, 255, 255), 0.3f) : c; };
      /* Violet, no outline: in pieces, each a lighter or darker shade only where hard to see; a picked tip orange
       * there, fading to the violet at the key (as vector handles fade). */
      const float2 key_at = h.key + h.offset, tip_at = (out ? h.out : h.in) + h.offset;
      const std::function<bool(float2, Color &)> under_at = [](float2 q, Color &c) { return canvas_now_at(q, c); };
      for (const PathHandleRun &r : path_handle_pieces(key_at, tip_at, dragged, 6.0f / std::max(view.zoom, 1e-9f),
                                                       under_at))
      {
        dl->AddLine(ImVec2(k.x + (e.x - k.x) * r.from, k.y + (e.y - k.y) * r.from),
                    ImVec2(k.x + (e.x - k.x) * r.to, k.y + (e.y - k.y) * r.to), brighten(r.color),
                    lit ? PATH_HANDLE_WIDTH + 0.7f : PATH_HANDLE_WIDTH);
      }
      const Color tip_base = dragged ? Color{1.0f, 160.0f / 255.0f, 40.0f / 255.0f, 1.0f}
                                     : color_from_rgb(preferences().path_handle_color);
      Color seen;
      const ImU32 fill = contrast_u32(
          canvas_now_at(tip_at, seen) ? contrast_colors_of(tip_base).of(contrast_pick(seen, tip_base)) : tip_base, 1.0f);
      /* The tip, twice a dot's size inside a thin white ring a gap away (a target, never taken for a path dot):
       * filled once dragged (Aligned or Broken), hollow while Auto. */
      const float r = lit ? 5.6f : 4.6f;
      if (h.mode != PathHandles::Auto) {
        dl->AddCircleFilled(e, r, brighten(fill));
      }
      else {
        dl->AddCircle(e, r - 1.0f, brighten(fill), 0, 2.2f);
      }
      dl->AddCircle(e, r + 2.2f, IM_COL32(255, 255, 255, 235), 0, 1.3f);
    }
  }
  /* Control points: small hollow squares (no frame: never a key's diamond or a dot), orange when picked. */
  g_path_control_hover.reset();
  if (editing) {
    static struct {
      const Document *doc = nullptr;
      uint64_t changes = ~uint64_t(0), serial = 0;
      int frame = -1;
      std::vector<std::pair<MotionPathTarget, std::vector<MotionPathControl>>> all;
    } controls;
    if (controls.doc != &doc || controls.changes != changes || controls.serial != g_path_drag.serial ||
        controls.frame != doc.frame_current)
    {
      controls.doc = &doc;
      controls.changes = changes;
      controls.serial = g_path_drag.serial;
      controls.frame = doc.frame_current;
      controls.all.clear();
      for (const MotionPath &p : cache.paths) {
        if (motion_path_has_handles(doc, p.target)) {
          controls.all.push_back({p.target, motion_path_controls(doc, p.target)});
        }
      }
    }
    float best_control = pick_path_pixels() * pick_path_pixels();
    for (const auto &[target, list] : controls.all) {
      const MotionPath *path = nullptr;
      for (const MotionPath &p : cache.paths) {
        if (p.target.same(target)) {
          path = &p;
        }
      }
      for (const MotionPathControl &c : list) {
        const int i = path ? c.near_frame - path->first : -1;
        if (i < 0 || i >= int(path->points.size())) {
          continue;
        }
        const float2 drawn_offset = path->points[size_t(i)] - c.curve;
        const ImVec2 s = view.to_screen(c.at + drawn_offset);
        const bool picked = std::find(wm.path_handle_picks.begin(), wm.path_handle_picks.end(),
                                      MotionPathHandlePick{target, c.end, false, c.index}) !=
                            wm.path_handle_picks.end();
        const bool dragged = g_path_drag.active && g_path_drag.control == c.index && g_path_drag.frame == c.end &&
                             g_path_drag.target.same(target);
        const float dx = s.x - mouse.x, dy = s.y - mouse.y;
        const bool lit = dragged;
        const float r = lit ? 5.0f : 4.0f;
        dl->AddRect(ImVec2(s.x - r - 1.5f, s.y - r - 1.5f), ImVec2(s.x + r + 1.5f, s.y + r + 1.5f),
                    IM_COL32(20, 20, 20, 200), 0.0f, 0, 1.2f);
        dl->AddRect(ImVec2(s.x - r, s.y - r), ImVec2(s.x + r, s.y + r),
                    picked || dragged ? IM_COL32(255, 160, 40, 255) : u32(path->color, 1.0f), 0.0f, 0, 2.0f);
        if (tool && hovering && !hover_handle && dx * dx + dy * dy < best_control) {
          best_control = dx * dx + dy * dy;
          g_path_control_hover = PathControlHover{target, c.end, c.index, c.near_frame, drawn_offset};
        }
      }
    }
  }
  dl->ChannelsMerge();
  if (g_path_control_hover) {
    /* A control point over a dot: the control point. */
    hover_path = nullptr;
  }
  if (hover_handle) {
    /* A handle over a dot: the handle, which is only there on a selected key. */
    hover_path = nullptr;
  }
  if (tool && !editing) {
    motion_path_brush_input(C, area, view, dl, hovered, first, last);
    return;
  }
  if (tool) {
    if (motion_path_edit_modals(C, view, dl, hovered, first, last, cache.paths)) {
      return;
    }
    draw_selected_path_dots(C, view, dl, hover_path, hover_frame, cache.paths);
    /* Shift held with Edit: Blend Speed's circle (a drag paints, a click on a dot or key adds it), and F sizing it.
     * Alt Shift stays Edit's breakdown. */
    const ImGuiIO &io = ImGui::GetIO();
    if (g_path_brush.active || g_path_brush.adjust || wm.path_adjust_request ||
        (io.KeyShift && !io.KeyAlt && !g_path_drag.active && !g_path_slide.active && !motion_path_vector_active() &&
         hovered && !((hover_handle || g_path_control_hover) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
    {
      std::optional<OpArgs> pick;
      if (hover_path) {
        pick = motion_path_target_args(doc, hover_path->target);
        (*pick)["frame"] = std::to_string(hover_frame);
      }
      /* Off the paths, a Shift click on a bone or point adds it (as the Select tool's); on a path it is Blend Speed's. */
      if (!g_path_brush.active && !hover_path && !hover_handle && hovered && io.KeyShift && !io.KeyAlt &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left) && motion_path_pick_click(C, view, io.MousePos, false, false))
      {
        motion_path_pick_click(C, view, io.MousePos, true);
        return;
      }
      motion_path_brush_input(C, area, view, dl, hovered, first, last, pick);
      return;
    }
  }
  if (tool && (hovered || g_path_drag.active || motion_path_vector_active())) {
    /* A copy: a double click goes to the dot's frame, which can make the paths again. */
    const std::optional<MotionPath> pressed = hover_path ? std::optional<MotionPath>(*hover_path) : std::nullopt;
    const std::optional<PathHandlesShown> pressed_handle =
        hover_handle ? std::optional<PathHandlesShown>(*hover_handle) : std::nullopt;
    motion_path_tool_input(C, area, view, pressed ? &*pressed : nullptr, hover_frame, first, last,
                           pressed_handle ? &*pressed_handle : nullptr, hover_out, hovered, dl);
    if (g_path_drag.active) {
      return;
    }
  }
  /* The hovered dot's frame as a ghost of the drawing there, with its number (not until the mouse moves on after a
   * move: what was just moved is what's looked at). */
  if (hover_path && !ImGui::IsAnyMouseDown() && !g_path_hover_quiet.on) {
    static struct {
      const Document *doc = nullptr;
      uint64_t changes = ~uint64_t(0);
      int frame = 0;
      std::vector<uint64_t> layers;
      std::vector<OnionWires> wires;
    } ghost;
    const std::vector<uint64_t> layers = motion_path_ghost_layers(doc, {hover_path->target});
    if (ghost.doc != &doc || ghost.changes != changes || ghost.frame != hover_frame || ghost.layers != layers) {
      ghost.doc = &doc;
      ghost.changes = changes;
      ghost.frame = hover_frame;
      ghost.layers = layers;
      ghost.wires = onion_wires(doc, layers, {OnionGhost{hover_frame, hover_frame > doc.frame_current, 1}});
    }
    std::vector<ImVec2> screen;
    for (const OnionWires &w : ghost.wires) {
      for (const std::vector<float2> &line : w.lines) {
        screen.clear();
        for (const float2 q : line) {
          screen.push_back(view.to_screen(q));
        }
        dl->AddPolyline(screen.data(), int(screen.size()), u32(hover_path->color, 0.7f), ImDrawFlags_None, 1.3f);
      }
    }
    const ImVec2 s = view.to_screen(hover_path->points[size_t(hover_frame - hover_path->first)]);
    dl->AddCircle(s, 5.0f, u32(hover_path->color, 1.0f), 0, 1.5f);
    const std::string label = std::to_string(hover_frame);
    dl->AddText(ImVec2(s.x + 7.0f, s.y + 2.0f), IM_COL32(255, 255, 255, 230), label.c_str());
  }
}

/* For the Shape handle display: the points of every shape any of whose points (or handles, picked
 * on their own) is selected. */
static const std::vector<bool> &shape_handle_points(const VectorDrawing &drawing)
{
  /* Worked out once per drawing per frame, not for every handle. */
  static const VectorDrawing *cached = nullptr;
  static int cached_frame = -1;
  static std::vector<bool> mask;
  /* (A background run has no ImGui and no frames: worked out every time.) */
  const int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -2;
  if (cached == &drawing && cached_frame == frame && frame != -2 && mask.size() == drawing.points.size()) {
    return mask;
  }
  cached = &drawing;
  cached_frame = frame;
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

/* Where a handle shows: where its line really bends, short of where it is stored when it faces detail. */
static float2 handle_end(const VectorDrawing &drawing, const ShownHandleFactors &factors, int c, int i, bool pre)
{
  const Curve &curve = drawing.curves[c];
  const float2 here = drawing.points[curve.points[i].point].pos;
  return here + (curve_handle(drawing, curve, i, pre) - here) * factors.of(c, i, pre);
}

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
    const ShownHandleFactors factors = drawing_detail_shown_factors(drawing);
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      const Curve &curve = drawing.curves[c];
      for (int i = 0; i < int(curve.points.size()); i++) {
        if (!handle_visible(drawing, curve.points[i], display)) {
          continue;
        }
        const float2 here = drawing.points[curve.points[i].point].pos;
        for (const bool pre : {true, false}) {
          const float2 h = handle_end(drawing, factors, c, i, pre);
          if (distance(h, here) < 1e-9f) {
            continue;
          }
          const ImVec2 s = view.to_screen(to_document.apply(h));
          const float d = std::sqrt((s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y));
          if (d <= pick_pixels(C.document->overlays.handle_size) && d < hit.distance) {
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
static void annotate_mode_icon(ImDrawList *dl, AnnotateMode mode, ImVec2 c, float s, ImU32 col);

/* Icons for the Draw tool's modes: a pencil for freehand, a curve with two points for the pen. */
static void draw_mode_icon(ImDrawList *dl, DrawMode mode, ImVec2 c, float s, ImU32 col)
{
  if (mode == DrawMode::FillPen) {
    /* A blob. */
    const ImVec2 points[] = {c + ImVec2(-s * 0.8f, s * 0.2f),  c + ImVec2(-s * 0.3f, -s * 0.7f), c + ImVec2(s * 0.5f, -s * 0.6f),
                             c + ImVec2(s * 0.8f, s * 0.1f),   c + ImVec2(s * 0.3f, s * 0.7f),   c + ImVec2(-s * 0.4f, s * 0.7f)};
    dl->AddConvexPolyFilled(points, 6, col);
    return;
  }
  if (mode == DrawMode::FillEraser) {
    /* An eraser block, leaning. */
    const ImVec2 u(0.7071f * s, -0.7071f * s), v(0.7071f * s * 0.5f, 0.7071f * s * 0.5f);
    dl->AddQuadFilled(c - u - v, c + u - v, c + u + v, c - u + v, col);
    return;
  }
  if (mode == DrawMode::StrokeEraser) {
    /* A line with a gap in it. */
    const float t = std::max(1.0f, s * 0.2f);
    dl->AddLine(c + ImVec2(-s, s * 0.5f), c + ImVec2(-s * 0.3f, s * 0.15f), col, t);
    dl->AddLine(c + ImVec2(s * 0.3f, -s * 0.15f), c + ImVec2(s, -s * 0.5f), col, t);
    return;
  }
  if (mode == DrawMode::Trimmer) {
    /* Two lines, and one stopped at the second. */
    const float t = std::max(1.0f, s * 0.2f);
    dl->AddLine(c + ImVec2(s * 0.3f, -s), c + ImVec2(s * 0.3f, s), col, t);
    dl->AddLine(c + ImVec2(-s, 0.0f), c + ImVec2(s * 0.3f, 0.0f), col, t);
    return;
  }
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
    case Tool::Annotate:
      annotate_mode_icon(dl, AnnotateMode::Draw, c, s, col);
      break;
    case Tool::TransformLayer: {
      /* A box with corner handles round a sheet (a layer). */
      dl->AddRect(c - ImVec2(s * 0.8f, s * 0.8f), c + ImVec2(s * 0.8f, s * 0.8f), col, 0.0f, 0, t * 0.8f);
      for (const ImVec2 k : {ImVec2(-1, -1), ImVec2(1, -1), ImVec2(1, 1), ImVec2(-1, 1)}) {
        dl->AddCircleFilled(c + k * (s * 0.8f), s * 0.2f, col);
      }
      dl->AddRectFilled(c - ImVec2(s * 0.35f, s * 0.35f), c + ImVec2(s * 0.35f, s * 0.35f), col, 1.0f);
      break;
    }
    case Tool::MotionPath: {
      /* Dots along an arc, a key's diamond in the middle. */
      for (int i = 0; i < 7; i++) {
        const float a = 3.14159f * (0.9f - 0.8f * float(i) / 6.0f);
        const ImVec2 p = c + ImVec2(std::cos(a) * s * 0.85f, s * 0.55f - std::sin(a) * s * 0.95f);
        if (i == 3) {
          const float r = s * 0.28f;
          const ImVec2 d[4] = {p + ImVec2(0, -r), p + ImVec2(r, 0), p + ImVec2(0, r), p + ImVec2(-r, 0)};
          dl->AddConvexPolyFilled(d, 4, col);
        }
        else {
          dl->AddCircleFilled(p, s * 0.12f, col);
        }
      }
      break;
    }
    case Tool::EditText:
      /* A T with a caret beside it. */
      dl->AddLine(c + ImVec2(-s * 0.8f, -s * 0.7f), c + ImVec2(s * 0.4f, -s * 0.7f), col, t * 1.3f);
      dl->AddLine(c + ImVec2(-s * 0.2f, -s * 0.7f), c + ImVec2(-s * 0.2f, s * 0.8f), col, t * 1.3f);
      dl->AddLine(c + ImVec2(s * 0.75f, -s * 0.1f), c + ImVec2(s * 0.75f, s * 0.8f), col, t * 0.8f);
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
    case Tool::Sculpt:
      sculpt_brush_icon(dl, SculptBrush::Grab, c, s * 1.05f, col, (col & 0x00ffffffu) | 0x60000000u);
      break;
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
    case PrimitiveKind::Grid:
      /* A box split in three each way. */
      dl->AddRect(at(-0.75f, -0.75f), at(0.75f, 0.75f), col, 0.0f, 0, t);
      for (const float k : {-0.25f, 0.25f}) {
        dl->AddLine(at(k, -0.75f), at(k, 0.75f), col, t * 0.8f);
        dl->AddLine(at(-0.75f, k), at(0.75f, k), col, t * 0.8f);
      }
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
    {Tool::TransformLayer, "Transform Layer",
     "A box round the active layer, as Moho's Transform Layer: drag inside to move it, around it to turn it about "
     "its origin, a handle to scale it (Shift keeps proportions, Alt scales from the origin). Esc or right-click "
     "cancels. Double-click a text layer to type in it",
     "wm.tool_transform_layer"},
    {Tool::EditText, "Edit Text",
     "Click the text to type in it (Shift-click or drag selects; Ctrl B, Ctrl I bold and italic). Drag a side "
     "handle to wrap the lines at that width (point text becomes a text box); double-click one for point text "
     "again. Esc finishes, back to Transform Layer",
     "wm.tool_edit_text"},
    {Tool::Annotate, "Annotate",
     "Notes and drawing guides over the drawing, on every frame; never rendered, and drawing takes no notice "
     "of them.\nAnnotate: drag to draw (Ctrl: erase). Annotate Line: drag a line, with arrowheads.\n"
     "Guide Line, Perspective, Grid: drag to make one; drag a handle to change it, its line to move it. Ctrl "
     "snaps. A Guide Line's ends resize it; on the picked one, drag past an end or alongside it to turn it. "
     "Double-click a guide line to duplicate it; right-click or Alt-click it, or press X over it or while "
     "dragging it, to delete it.\n"
     "Right-click the tool to pick the variant",
     "wm.tool_annotate"},
    {Tool::Draw, "Draw",
     "Stroke Pen: freehand lines; tablet pressure sets the width. The shelf below the view has the pens.\n"
     "Fill Pen: paints filled areas, merging with a fill of the same colour it overlaps; Ctrl cuts.\n"
     "Bezier Pen: click to add points along a curve, drag to place each; click the first point to close it.\n"
     "Fill Eraser: cuts the brushed area out of fills, leaving lines alone.\n"
     "Stroke Eraser: cuts lines where brushed, leaving fills alone.\n"
     "Vector Trimmer: brush across a line, and the touched piece goes, out to the nearest crossings.\n"
     "Right-click to pick the kind",
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
    {Tool::Sculpt, "Sculpt",
     "Sculpt the drawing with a brush, as Blender's Grease Pencil sculpt brushes: pick one on the shelf along the "
     "bottom (Grab, Push, Pinch, Twist, Smooth, Surface Smooth, Thickness, and brushes you saved). Drag over the lines; Ctrl "
     "inverts (Thickness thins, Pinch inflates, Twist turns back); hold Shift to smooth instead. F sizes the brush, "
     "Shift F sets its strength; "
     "G, P, S, T, I, W pick a brush. Away from frame 0 it poses the drawing (auto-key keys it)",
     "wm.tool_sculpt"},
    {Tool::MotionPath, "Motion Path",
     "Edit the motion paths of what's selected (shown with this tool even when the Onion Skin popover's Motion "
     "Paths is off). Click a dot to select its key and show its handles (between keys, the key ending that "
     "stretch); Alt click also goes to its frame. Drag a key to move it there; drag a dot between keys to add a "
     "key, the frame shown staying. Shift-drag between keys: a breakdown, sliding along the arc between the keys either side. Ctrl: onto the straight line between them. "
     "Dragging always keys, whatever auto-key says. The header sets the selected keys' interpolation, with a "
     "ball showing how it moves, and has Smooth Arc",
     "wm.tool_motion_path"},
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

/* The tool bar's boxes, in this order, each its tools for every frame and then, under a bar, those for
 * the setup frame (0) only: General (any layer), Drawing (vector layers: making artwork, and moving
 * what is drawn), Rig (bone layers, and the weights of drawings inside them). */
struct ToolBox {
  const char *name;
  std::vector<Tool> any_frame;
  std::vector<Tool> setup;
};
static const ToolBox TOOL_BOXES[] = {
    {"General tools",
     {Tool::Select, Tool::SelectLayer, Tool::TransformLayer, Tool::MotionPath, Tool::Cursor, Tool::Annotate},
     {}},
    {"Drawing tools", {Tool::TransformPoints, Tool::Sculpt}, {Tool::Draw, Tool::Shape, Tool::SplitCurve}},
    {"Rig tools", {Tool::TransformBones}, {Tool::WeightSelect, Tool::WeightPaint}},
    {"Text tools", {Tool::EditText}, {}},
};

/* How a tool shows for the active layer: not at all (it can never work there), greyed out with a reason
 * (something you can do makes it work: going to frame 0, a drawing editable with the bones), or ready. */
struct ToolState {
  bool shown = false;
  bool usable = false;
  const char *reason = "";
};

static ToolState tool_state(Context &C, Tool tool)
{
  const Document &doc = *C.document;
  const Layer *active = document_active_layer(*C.document);
  const bool bones = active && active->is_bone_layer();
  const bool drawing = active && active->has_drawing();
  const bool group = active && active->type == LayerType::Group;
  const bool in_bone_layer = active && layer_armature(*C.document, active->id) != nullptr;
  /* A group or bone layer the weight tools can work inside. */
  const bool weighs_inside = active && active->is_group() && (bones || in_bone_layer);
  /* Weights do nothing under a Parent Bone (or, for a drawing, it would stop having one: that's its choice). */
  const auto weight_reason = [&]() -> const char * {
    if (layer_binding_group(doc, active->id) || (active->is_group() && active->parent_bone != 0)) {
      return "A Parent Bone moves this as a whole: weights do nothing here";
    }
    return weighs_inside && document_weight_layers(*C.document).empty()
               ? "No drawings to weigh here: turn on Edit Descendants, or pick a drawing"
               : "";
  };
  ToolState state;
  switch (tool) {
    /* Select: where there is something to pick (points, bones); not text or a picture. */
    case Tool::Select:
      state.shown = drawing || bones || group;
      break;
    case Tool::SelectLayer:
    case Tool::Cursor:
    case Tool::Annotate:
    case Tool::MotionPath:
      state.shown = true;
      break;
    case Tool::TransformLayer:
      state.shown = active != nullptr;
      if (state.shown && active->locked) {
        state.reason = "The layer is locked: unlock it in the Layers editor";
        return state;
      }
      break;
    case Tool::EditText:
      state.shown = active && active->is_text();
      if (state.shown && active->locked) {
        state.reason = "The layer is locked: unlock it in the Layers editor";
        return state;
      }
      break;
    /* Moving and reshaping what is drawn: in a drawing, and through a bone layer or group that edits the
     * drawings inside it. */
    case Tool::TransformPoints:
    case Tool::Sculpt:
      state.shown = drawing || bones || group;
      if (state.shown && !drawing && editable_layers(C).empty()) {
        state.reason = bones ? "No drawings to edit here: turn on Editable in Bone Group for a drawing inside"
                             : "No drawings to edit here: turn on Edit Descendants";
        return state;
      }
      break;
    /* Making artwork: only in a drawing. */
    case Tool::Draw:
    case Tool::Shape:
    case Tool::SplitCurve:
      state.shown = drawing;
      break;
    case Tool::TransformBones:
      state.shown = bones;
      break;
    /* Weights: the active drawing's points, or those of the drawings in the active group, for the bones of the bone
     * layer they are in. */
    case Tool::WeightSelect:
      state.shown = (drawing && in_bone_layer) || weighs_inside;
      if (state.shown && *weight_reason()) {
        state.reason = weight_reason();
        return state;
      }
      break;
    /* Weight Paint also paints a drawing's deform modifiers' weights. */
    case Tool::WeightPaint: {
      const bool deforms = drawing && std::any_of(active->modifiers.begin(), active->modifiers.end(), [](const Modifier &m) {
        return modifier_is_deform(m.type);
      });
      state.shown = (drawing && in_bone_layer) || weighs_inside || deforms;
      if (state.shown && !deforms && *weight_reason()) {
        state.reason = weight_reason();
        return state;
      }
      break;
    }
  }
  if (state.shown && tool_setup_only(tool) && doc.frame_current != 0) {
    state.reason = "Setup frame only: press Tab to go to frame 0";
    return state;
  }
  state.usable = state.shown;
  return state;
}

/* Which kind of layer is active, for the tool remembered with each: a drawing, a bone layer, a group,
 * or anything else. */
static int tool_layer_kind(const Layer *active)
{
  return !active                              ? 5
         : active->has_drawing()              ? 0
         : active->is_bone_layer()            ? 1
         : active->type == LayerType::Group   ? 2
         : active->is_text()                  ? 4
                                              : 3;
}

/* The tool a kind of layer starts with, or falls back to: Select where there is something to pick, else
 * Transform Layer (text, a picture), else Select Layer (no layer). */
static Tool tool_fallback(Context &C)
{
  return tool_state(C, Tool::Select).shown ? Tool::Select
         : tool_state(C, Tool::TransformLayer).shown ? Tool::TransformLayer
                                                     : Tool::SelectLayer;
}

/* A tool's flyout of variants, opened by right-clicking its tool bar button and closed by
 * right-clicking it again. Call right after the button. Returns the picked item, or -1.
 * `open_last_frame` is the flyout's own: ImGui closes a popup on a click outside it before this
 * runs, so it remembers whether the flyout was open to make the right-click a toggle. */
struct FlyoutItem {
  const char *name;
  std::function<void(ImDrawList *dl, ImVec2 center, float size)> icon;
  bool current;
  /* A line above it, between groups of variants. */
  bool separator_before = false;
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
    if (items[i].separator_before) {
      ImGui::Separator();
    }
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
  const DrawMode modes[] = {DrawMode::Freehand,   DrawMode::FillPen,      DrawMode::Pen,
                            DrawMode::FillEraser, DrawMode::StrokeEraser, DrawMode::Trimmer};
  static const char *const names[] = {"Stroke Pen", "Fill Pen", "Bezier Pen", "Fill Eraser", "Stroke Eraser", "Vector Trimmer"};
  std::vector<FlyoutItem> items;
  for (int i = 0; i < 6; i++) {
    const DrawMode mode = modes[i];
    items.push_back({names[i], [mode, col](ImDrawList *dl, ImVec2 c, float s) { draw_mode_icon(dl, mode, c, s, col); },
                     ts.tool == Tool::Draw && ts.draw_mode == mode, mode == DrawMode::FillEraser});
  }
  if (const int picked = tool_flyout("##draw_flyout", open_last_frame, button_pos, button, items); picked >= 0) {
    draw_mode_set(ts, modes[picked]);
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

/* The Guide Line tool's icon, sz_draw_guides' own picture: its shape is in the alpha, so it takes the
 * button's colour. 0 when the file can't be read. */
static GLuint guide_line_icon(ImVec2 *r_size)
{
  static GLuint texture = 0;
  static ImVec2 size;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const std::u8string path = (python::scripts_dir() / "icons" / "sz_draw_guides.png").u8string();
    if (const MediaFramePtr frame = media_frame(std::string(path.begin(), path.end()), false, 0.0, true)) {
      std::vector<uint8_t> rgba = frame->rgba;
      for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgba[i] = rgba[i + 1] = rgba[i + 2] = 255;
      }
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame->width, frame->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      size = ImVec2(float(frame->width), float(frame->height));
    }
  }
  *r_size = size;
  return texture;
}

/* Icons for the Annotate tool's variants. */
static void annotate_mode_icon(ImDrawList *dl, AnnotateMode mode, ImVec2 c, float s, ImU32 col)
{
  const float t = std::max(1.0f, s * 0.12f);
  const auto at = [&](float x, float y) { return c + ImVec2(x * s, y * s); };
  const ImU32 guide = IM_COL32(0x6f, 0xc8, 0xff, 255);
  switch (mode) {
    case AnnotateMode::Draw:
      /* A scribble, as Blender's annotate icon. */
      dl->AddBezierCubic(at(-0.85f, 0.45f), at(-0.4f, -0.9f), at(0.1f, 0.9f), at(0.85f, -0.5f), col, t * 1.3f);
      break;
    case AnnotateMode::Line:
      dl->AddLine(at(-0.8f, 0.7f), at(0.55f, -0.55f), col, t * 1.3f);
      dl->AddTriangleFilled(at(0.85f, -0.85f), at(0.25f, -0.65f), at(0.65f, -0.25f), col);
      break;
    case AnnotateMode::Eraser: {
      const ImVec2 quad[4] = {at(-0.85f, 0.2f), at(-0.15f, -0.5f), at(0.6f, 0.25f), at(-0.1f, 0.95f)};
      dl->AddPolyline(quad, 4, col, ImDrawFlags_Closed, t);
      dl->AddLine(at(-0.5f, -0.15f), at(0.25f, 0.6f), col, t);
      break;
    }
    case AnnotateMode::GuideLine: {
      ImVec2 size;
      if (const GLuint icon = guide_line_icon(&size)) {
        /* Whole pixels, on the pixel grid: it's a 24 pixel picture. */
        const ImVec2 shown = size * std::max(1.0f, std::round(s * 2.6f / size.x));
        const ImVec2 corner = ImFloor(c - shown * 0.5f);
        dl->AddImage(ImTextureRef(ImTextureID(intptr_t(icon))), corner, corner + shown, ImVec2(0, 0), ImVec2(1, 1), col);
        break;
      }
      dl->AddLine(at(-0.95f, 0.75f), at(0.95f, -0.75f), guide, t);
      dl->AddCircleFilled(at(-0.5f, 0.39f), s * 0.2f, col);
      dl->AddCircleFilled(at(0.5f, -0.39f), s * 0.2f, col);
      break;
    }
    case AnnotateMode::GuidePerspective:
      dl->AddLine(at(-0.95f, -0.3f), at(0.95f, -0.3f), guide, t);
      for (const float x : {-0.9f, -0.3f, 0.3f, 0.9f}) {
        dl->AddLine(at(0.0f, -0.3f), at(x, 0.9f), col, t * 0.8f);
      }
      dl->AddCircleFilled(at(0.0f, -0.3f), s * 0.18f, col);
      break;
    case AnnotateMode::GuideGrid:
      for (const float k : {-0.8f, -0.27f, 0.27f, 0.8f}) {
        dl->AddLine(at(k, -0.8f), at(k, 0.8f), guide, t * 0.8f);
        dl->AddLine(at(-0.8f, k), at(0.8f, k), guide, t * 0.8f);
      }
      dl->AddCircleFilled(at(-0.8f, 0.8f), s * 0.2f, col);
      break;
  }
}

static const char *annotate_mode_name(AnnotateMode mode)
{
  switch (mode) {
    case AnnotateMode::Draw:
      return "Annotate";
    case AnnotateMode::Line:
      return "Annotate Line";
    case AnnotateMode::Eraser:
      return "Annotate Eraser";
    case AnnotateMode::GuideLine:
      return "Guide Line";
    case AnnotateMode::GuidePerspective:
      return "Guide Perspective";
    case AnnotateMode::GuideGrid:
      return "Guide Grid";
  }
  return "Annotate";
}

static constexpr AnnotateMode ANNOTATE_MODES[] = {AnnotateMode::Draw,      AnnotateMode::Line,
                                                  AnnotateMode::Eraser,    AnnotateMode::GuideLine,
                                                  AnnotateMode::GuidePerspective, AnnotateMode::GuideGrid};

static void annotate_flyout(Context &C, ImVec2 button_pos, float button)
{
  ToolSettings &ts = C.wm->tool_settings;
  static bool open_last_frame = false;
  const ImU32 col = IM_COL32(0xe6, 0xe6, 0xe6, 255);
  std::vector<FlyoutItem> items;
  for (const AnnotateMode mode : ANNOTATE_MODES) {
    items.push_back({annotate_mode_name(mode),
                     [mode, col](ImDrawList *dl, ImVec2 c, float s) { annotate_mode_icon(dl, mode, c, s, col); },
                     ts.tool == Tool::Annotate && ts.annotate_mode == mode, mode == AnnotateMode::GuideLine});
  }
  if (const int picked = tool_flyout("##annotate_flyout", open_last_frame, button_pos, button, items); picked >= 0) {
    ts.tool = Tool::Annotate;
    ts.annotate_mode = ANNOTATE_MODES[picked];
  }
}

static const ToolInfo &tool_info(Tool tool)
{
  for (const ToolInfo &info : TOOLS) {
    if (info.tool == tool) {
      return info;
    }
  }
  return TOOLS[0];
}

/* Another kind of layer made active: its own tool again (Select the first time); the one left is kept for when that
 * kind comes back. The weight tools and the Motion Path tool stay wherever they work: weighing goes from a drawing to
 * its group and back, and paths are traced across layers (an Alt click picks a bone or point in another). */
void tool_follow_active_layer(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  const int kind = tool_layer_kind(document_active_layer(*C.document));
  if (kind != ts.tool_layer_kind) {
    if (ts.tool_layer_kind >= 0) {
      ts.tool_for_layer_kind[ts.tool_layer_kind] = ts.tool_temporary ? ts.tool_return : ts.tool;
    }
    ts.tool_layer_kind = kind;
    const bool stays = (ts.tool == Tool::WeightPaint || ts.tool == Tool::WeightSelect || ts.tool == Tool::MotionPath) &&
                       tool_state(C, ts.tool).shown;
    ts.tool = stays ? ts.tool : ts.tool_for_layer_kind[kind];
    ts.tool_temporary = false;
  }
  if (!tool_state(C, ts.tool).shown) {
    ts.tool = tool_fallback(C);
  }
}

/* The tool bar down the left side of the viewport: its boxes (TOOL_BOXES), each a panel of its own, in
 * two columns when one would run past `max_height`. Returns its width. */
static float toolbar_draw(Context &C, ImVec2 origin, float top, float max_height)
{
  ToolSettings &ts = C.wm->tool_settings;
  const float h = ImGui::GetFrameHeight();
  const float button = std::round(h * 1.35f);
  const float pad = 4.0f;
  const float gap = 10.0f;
  const float step = button + 2.0f;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  tool_follow_active_layer(C);

  /* What each box shows. */
  struct Shown {
    std::vector<Tool> any_frame, setup;
  };
  std::vector<std::pair<const ToolBox *, Shown>> boxes;
  int rows_one_column = 0;
  for (const ToolBox &box : TOOL_BOXES) {
    Shown shown;
    for (const Tool t : box.any_frame) {
      if (tool_state(C, t).shown) {
        shown.any_frame.push_back(t);
      }
    }
    for (const Tool t : box.setup) {
      if (tool_state(C, t).shown) {
        shown.setup.push_back(t);
      }
    }
    if (shown.any_frame.empty() && shown.setup.empty()) {
      continue;
    }
    rows_one_column += int(shown.any_frame.size() + shown.setup.size());
    boxes.push_back({&box, std::move(shown)});
  }
  const auto box_height = [&](const Shown &shown, int columns) {
    const int a = (int(shown.any_frame.size()) + columns - 1) / columns;
    const int b = (int(shown.setup.size()) + columns - 1) / columns;
    return float(a + b) * step + pad * 2.0f + (a > 0 && b > 0 ? gap : 0.0f);
  };
  float height_one = 0.0f;
  for (const auto &[box, shown] : boxes) {
    height_one += box_height(shown, 1) + gap;
  }
  const int columns = height_one > max_height && rows_one_column > 1 ? 2 : 1;
  const float width = float(columns) * step - 2.0f + pad * 2.0f;

  float y = origin.y + top;
  int id = 0;
  for (const auto &[box, shown] : boxes) {
    const ImVec2 panel_min(origin.x + pad, y);
    const ImVec2 panel_max = panel_min + ImVec2(width, box_height(shown, columns));
    dl->AddRectFilled(panel_min, panel_max, COL_TOOLBAR, 6.0f);
    /* Hovering the box's edge names it. */
    ImGui::SetCursorScreenPos(panel_min);
    ImGui::PushID(box->name);
    /* Under its tool buttons, which take the mouse where they are. */
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##box", panel_max - panel_min);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenOverlappedByItem) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
      ImGui::SetTooltip("%s", box->name);
    }
    ImGui::PopID();
    float row_y = panel_min.y + pad;
    for (const bool setup_section : {false, true}) {
      const std::vector<Tool> &tools = setup_section ? shown.setup : shown.any_frame;
      if (tools.empty()) {
        continue;
      }
      if (setup_section && !shown.any_frame.empty()) {
        /* The bar: frame-0 tools below it. */
        const float bar = row_y + gap * 0.5f - 1.0f;
        dl->AddLine(ImVec2(panel_min.x + pad + 3.0f, bar), ImVec2(panel_max.x - pad - 3.0f, bar),
                    IM_COL32(0x60, 0x60, 0x60, 255), 1.0f);
        row_y += gap;
      }
      for (size_t k = 0; k < tools.size(); k++) {
        const ToolInfo &info = tool_info(tools[k]);
        const ToolState state = tool_state(C, info.tool);
        const ImVec2 p(panel_min.x + pad + float(int(k) % columns) * step, row_y + float(int(k) / columns) * step);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(id++);
        if (ImGui::InvisibleButton("##tool", ImVec2(button, button))) {
          if (state.usable) {
            ts.tool = info.tool;
          }
          else {
            /* A greyed-out tool says what makes it work. */
            report_banner(ReportType::Warning, state.reason);
          }
        }
        const bool hovered = ImGui::IsItemHovered() && state.usable;
        const bool active = ts.tool == info.tool;
        if (active || hovered) {
          dl->AddRectFilled(p, p + ImVec2(button, button), active ? COL_HIGHLIGHT : IM_COL32(0x45, 0x45, 0x45, 255),
                            4.0f);
        }
        const ImU32 icon_col = state.usable ? IM_COL32(0xe6, 0xe6, 0xe6, 255) : IM_COL32(0xe6, 0xe6, 0xe6, 70);
        const ImVec2 center = p + ImVec2(button, button) * 0.5f;
        const bool has_flyout = info.tool == Tool::Draw || info.tool == Tool::Shape || info.tool == Tool::Annotate;
        if (info.tool == Tool::Annotate) {
          annotate_mode_icon(dl, ts.annotate_mode, center, button * 0.3f, icon_col);
          annotate_flyout(C, p, button);
        }
        else if (info.tool == Tool::Draw) {
          draw_mode_icon(dl, ts.draw_mode, center, button * 0.3f, icon_col);
          if (state.usable) {
            draw_flyout(C, p, button);
          }
        }
        else if (info.tool == Tool::Shape) {
          primitive_icon(dl, ts.shape, center, button * 0.3f, icon_col);
          if (state.usable) {
            shape_flyout(C, p, button);
          }
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
          if (!state.usable) {
            ImGui::SetTooltip("%s\n%s", info.name, state.reason);
          }
          else {
            const std::string shortcut = keymaps().shortcut_for(info.op_idname);
            ImGui::SetTooltip("%s\n%s%s%s", info.name, info.description, shortcut.empty() ? "" : "\nShortcut: ",
                              shortcut.c_str());
          }
        }
        ImGui::PopID();
      }
      row_y += float((int(tools.size()) + columns - 1) / columns) * step;
    }
    y = panel_max.y + gap;
  }
  return width + pad;
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
    overlay_checkbox_pair("Camera Frame", &ov.camera_frame, "Annotations", &ov.annotations, column);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show the notes drawn with the Annotate tool (never rendered)");
    }
    ImGui::SetNextItemWidth(column * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    ui_drag_float("##grid_scale", &ts.grid_size, 0.005f, 0.01f, 1.0f, "Scale  %.3f", ImGuiSliderFlags_AlwaysClamp);
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
    /* Sizes, as Blender's Vertex Size: how big points and handles' ends are drawn. */
    const auto size_field = [&](const char *label, const char *id, float *value, const char *tip) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      ImGui::SameLine(column);
      ImGui::SetNextItemWidth(column - ImGui::GetStyle().ItemSpacing.x);
      ui_drag_float(id, value, 0.1f, OVERLAY_SIZE_MIN, OVERLAY_SIZE_MAX, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tip);
      }
    };
    size_field("Point Size", "##point_size", &ov.point_size,
               "How big points are drawn, in pixels across (clicks reach a little past them)");
    size_field("Handle Size", "##handle_size", &ov.handle_size,
               "How big the dots at handles' ends are drawn, in pixels across");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Mask Outlines", &ov.mask_outlines);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Outline the active layer's mask layers, dashed and labelled (\"inside head\").\n"
                        "Off: they only show while a mask's symbol or Layers field is under the mouse.");
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column);
    ImGui::Checkbox("Guides", &ov.guides);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show guides: layers with Show in Renders off (references, Media layers), and the drawing "
                        "guides made with the Annotate tool.\n"
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

/* Blender's shading icon: a sphere, lit from the top left. */
static void shading_icon(ImDrawList *dl, ImVec2 center, float size, ImU32 color)
{
  const float r = size * 0.3f;
  dl->AddCircle(center, r, color, 0, 1.4f);
  dl->AddCircleFilled(center - ImVec2(r * 0.35f, r * 0.35f), r * 0.32f, color);
}

/* The Viewport Shading dropdown, beside the overlays' as in Blender: how the viewports draw. */
static void shading_buttons(Context &C, ViewportShading &sh, float h, float arrow_w)
{
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetStyle().FrameRounding);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool open = ImGui::Button("##shading", ImVec2(h, h));
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Viewport Shading");
  }
  shading_icon(ImGui::GetWindowDrawList(), at + ImVec2(h * 0.5f, h * 0.5f), h, IM_COL32(0xe6, 0xe6, 0xe6, 255));
  ImGui::SameLine(0.0f, 1.0f);
  if (ImGui::ArrowButtonEx("##shading_menu", ImGuiDir_Down, ImVec2(arrow_w, h)) || open) {
    ImGui::OpenPopup("##shading_popover");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Viewport Shading");
  }
  ImGui::PopStyleVar();

  ImGui::SetNextWindowPos(ImVec2(at.x + h + arrow_w, at.y + h + 2.0f), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
  if (!ImGui::BeginPopup("##shading_popover")) {
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::TextUnformatted("Viewport Shading");
  if (overlay_section("Renderer")) {
    if (ImGui::RadioButton("Classic", !sh.gpu_geometry) && sh.gpu_geometry) {
      sh.gpu_geometry = false;
      C.undo->mark_changed();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Every fill cut into triangles on the CPU.");
    }
    if (ImGui::RadioButton("GPU Geometry", sh.gpu_geometry) && !sh.gpu_geometry) {
      sh.gpu_geometry = true;
      C.undo->mark_changed();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Being built, in stages: big fills drawn by the GPU (stencil-then-cover)\n"
                        "instead of cut into triangles on the CPU, masks kept in the depth buffer,\n"
                        "nesting as deep as they go (Classic: four levels), and see-through layers faded\n"
                        "as a whole, so their lines don't show their fills through them. Renders too.");
    }
  }
  if (overlay_section("Performance")) {
    if (ImGui::Checkbox("Layer Caching", &sh.layer_caching)) {
      C.undo->mark_changed(); /* Saved with the file. */
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Draw anew only the layers that changed, and those depending on them (masks, deforms);\n"
                        "the rest come from images kept of them, and a pan shows the same image moved.\n"
                        "Looks the same either way; off, every change draws every layer.");
    }
    if (ImGui::Checkbox("Zoom Preview", &sh.zoom_preview)) {
      C.undo->mark_changed();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("While a zoom moves, show the last image scaled over a blurry wider backdrop,\n"
                        "drawn sharp once the zoom rests. Off: each step of a zoom is drawn sharp.");
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
  if (ui_drag_int("##detail_level", &value, 0.05f, 0, DETAIL_LEVEL_MAX, level < 0 ? "-" : "%d",
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
/* A header value pen pressure can scale: its number field with the pressure toggle joined to its end, as
 * Blender's brush settings have. `factor`: a 0-1 bar. */
static bool pressure_field(const char *id, float *v, float speed, float soft_min, float soft_max, const char *format,
                           bool *pressure, const char *pressure_tip, bool factor = false,
                           float hard_min = UI_LIMIT_AUTO, float hard_max = UI_LIMIT_AUTO)
{
  ui_number_hard_limits(soft_min, soft_max, format, &hard_min, &hard_max);
  NumberField f;
  f.factor = factor;
  f.soft_min = soft_min;
  f.soft_max = soft_max;
  f.hard_min = hard_min;
  f.hard_max = hard_max;
  f.speed = speed;
  f.step = double(speed) * 10.0;
  f.format = format;
  f.pressure = pressure;
  f.pressure_tip = pressure_tip;
  double d = double(*v);
  if (!ui_number(id, &d, f)) {
    return false;
  }
  *v = float(d);
  return true;
}

static void width_field(const Document &doc, ToolSettings &ts, float &width, bool pressure)
{
  const float units_to_pixels = float(doc.height) * 0.5f;
  float pixels = width * units_to_pixels;
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * (pressure ? 6.0f : 4.5f));
  if (pressure ? pressure_field("##width", &pixels, 0.1f, 0.0f, 500.0f, "Width %.1f px", &ts.ink.use_pressure,
                                "Pen pressure sets the line's width")
               : ui_drag_float("##width", &pixels, 0.1f, 0.0f, 500.0f, "Width %.1f px"))
  {
    width = pixels / units_to_pixels;
  }
}

/* --- Annotations and drawing guides (data/annotations.hh): drawn over the drawing, made and changed by
 * the Annotate tool. --- */

/* The drawing guides' colour: the document's hue, `strength` of its opacity. */
static ImU32 guide_color(const GuideSettings &settings, float strength)
{
  ImVec4 c;
  ImGui::ColorConvertHSVtoRGB(float(settings.hue) / 360.0f, 1.0f, 1.0f, c.x, c.y, c.z);
  c.w = strength * float(settings.opacity) / 100.0f;
  return ImGui::ColorConvertFloat4ToU32(c);
}

/* What the view shows, in document space. */
static void view_box(const ViewMapping &view, float2 &r_lo, float2 &r_hi)
{
  const float2 a = view.to_document(view.origin), b = view.to_document(view.origin + view.size);
  r_lo = {std::min(a.x, b.x), std::min(a.y, b.y)};
  r_hi = {std::max(a.x, b.x), std::max(a.y, b.y)};
}

/* Grid cells under this many pixels across aren't drawn. */
static constexpr float GUIDE_MIN_CELL_PIXELS = 6.0f;
/* How near (pixels) the mouse picks a guide's handle or line, and Ctrl snaps. */
static constexpr float GUIDE_HANDLE_PICK = 9.0f;
static constexpr float GUIDE_LINE_PICK = 5.0f;
static constexpr float GUIDE_SNAP_PIXELS = 12.0f;
/* A Line guide, in pixels: its widths, the ticks across its ends, how near grabs an end or the line, and
 * how far round the picked one it can be turned from. A drag starts past GUIDE_DRAG_START. */
static constexpr float GUIDE_MEASURE_WIDTH = 2.0f;
static constexpr float GUIDE_MEASURE_PICKED_WIDTH = 4.0f;
static constexpr float GUIDE_MEASURE_TICK = 8.0f;
static constexpr float GUIDE_MEASURE_GRAB = 8.0f;
static constexpr float GUIDE_MEASURE_TURN = 40.0f;
static constexpr float GUIDE_DRAG_START = 2.0f;

/* An annotation line's end at `tip`, coming from `from` (screen space). */
static void annotation_end_draw(ImDrawList *dl, AnnotationEnd end, ImVec2 tip, ImVec2 from, float thickness, ImU32 col)
{
  if (end == AnnotationEnd::None) {
    return;
  }
  ImVec2 d = tip - from;
  const float len = std::sqrt(d.x * d.x + d.y * d.y);
  if (len < 1e-3f) {
    d = ImVec2(1.0f, 0.0f);
  }
  else {
    d = d / len;
  }
  const ImVec2 n(-d.y, d.x);
  const float size = thickness * 3.5f + 6.0f;
  const ImVec2 base = tip - d * size;
  switch (end) {
    case AnnotationEnd::Arrow:
      dl->AddTriangleFilled(tip + d * (thickness * 0.5f), base + n * (size * 0.5f), base - n * (size * 0.5f), col);
      break;
    case AnnotationEnd::OpenArrow:
      dl->AddLine(tip, base + n * (size * 0.55f), col, thickness);
      dl->AddLine(tip, base - n * (size * 0.55f), col, thickness);
      break;
    case AnnotationEnd::Segment:
      dl->AddLine(tip + n * (size * 0.5f), tip - n * (size * 0.5f), col, thickness);
      break;
    case AnnotationEnd::Dot:
      dl->AddCircleFilled(tip, thickness * 1.2f + 2.5f, col);
      break;
    case AnnotationEnd::None:
      break;
  }
}

/* A stroke on screen: its line, then its ends (each pointing along the last few pixels). */
static void annotation_stroke_draw(ImDrawList *dl, const ViewMapping &view, const std::vector<float2> &points,
                                   AnnotationEnd start, AnnotationEnd end, float thickness, ImU32 col)
{
  if (points.empty()) {
    return;
  }
  std::vector<ImVec2> line;
  line.reserve(points.size());
  for (const float2 &p : points) {
    line.push_back(view.to_screen(p));
  }
  if (line.size() == 1 || (line.size() == 2 && ImLengthSqr(line[1] - line[0]) < 1.0f)) {
    dl->AddCircleFilled(line[0], thickness * 0.5f + 0.5f, col);
  }
  else {
    dl->AddPolyline(line.data(), int(line.size()), col, ImDrawFlags_None, thickness);
  }
  /* Far enough back along the line to tell its direction. */
  const float back = thickness * 2.0f + 6.0f;
  const auto from_end = [&](bool at_start) {
    const int n = int(line.size());
    const ImVec2 tip = at_start ? line[0] : line[size_t(n - 1)];
    for (int k = 1; k < n; k++) {
      const ImVec2 q = at_start ? line[size_t(k)] : line[size_t(n - 1 - k)];
      if (ImLengthSqr(q - tip) >= back * back) {
        return q;
      }
    }
    return at_start ? line[size_t(n - 1)] : line[0];
  };
  annotation_end_draw(dl, start, line.front(), from_end(true), thickness, col);
  annotation_end_draw(dl, end, line.back(), from_end(false), thickness, col);
}

static ImU32 color_u32(const Color &c)
{
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
}

/* What's on a Line guide under the mouse. */
struct GuideMeasureHit {
  int guide = -1;
  GuideGrab grab = GuideGrab::None;
  int end = 1;
};

static float2 screen_point(const ViewMapping &view, float2 p)
{
  const ImVec2 s = view.to_screen(p);
  return {s.x, s.y};
}

/* A Line guide's end or line under `mouse` (screen): the last made first. Only shown, unlocked ones. */
static GuideMeasureHit pick_guide_measure(const Document &doc, const ViewMapping &view, ImVec2 mouse)
{
  const float2 m(mouse.x, mouse.y);
  for (int g = int(doc.guides.size()) - 1; g >= 0; g--) {
    const DrawingGuide &guide = doc.guides[size_t(g)];
    if (guide.type != GuideType::Line || guide.hidden || guide.locked || guide.points.size() < 2) {
      continue;
    }
    const float2 a = screen_point(view, guide.points[0]), b = screen_point(view, guide.points[1]);
    if (length(m - a) < GUIDE_MEASURE_GRAB) {
      return {g, GuideGrab::End, 0};
    }
    if (length(m - b) < GUIDE_MEASURE_GRAB) {
      return {g, GuideGrab::End, 1};
    }
    if (distance_to_segment(m, a, b) < GUIDE_MEASURE_GRAB) {
      return {g, GuideGrab::Move, 1};
    }
  }
  return {};
}

/* Round the picked Line guide: past an end turns that end, alongside it turns it about its middle. */
static GuideMeasureHit pick_guide_turn(const Document &doc, const ViewMapping &view, ImVec2 mouse)
{
  const int g = doc.active_guide;
  if (g < 0 || g >= int(doc.guides.size())) {
    return {};
  }
  const DrawingGuide &guide = doc.guides[size_t(g)];
  if (guide.type != GuideType::Line || guide.hidden || guide.locked || guide.points.size() < 2) {
    return {};
  }
  const float2 m(mouse.x, mouse.y);
  const float2 a = screen_point(view, guide.points[0]), b = screen_point(view, guide.points[1]);
  if (length(m - a) < GUIDE_MEASURE_TURN && dot(a - b, m - a) > 0.0f) {
    return {g, GuideGrab::RotateEnd, 0};
  }
  if (length(m - b) < GUIDE_MEASURE_TURN && dot(b - a, m - b) > 0.0f) {
    return {g, GuideGrab::RotateEnd, 1};
  }
  if (distance_to_segment(m, a, b) < GUIDE_MEASURE_TURN) {
    return {g, GuideGrab::RotateCenter, 1};
  }
  return {};
}

/* A Line guide on screen: its line with a tick across each end. */
static void guide_measure_draw(ImDrawList *dl, const ViewMapping &view, const DrawingGuide &guide, float width, ImU32 col)
{
  if (guide.points.size() < 2) {
    return;
  }
  const ImVec2 a = view.to_screen(guide.points[0]), b = view.to_screen(guide.points[1]);
  dl->AddLine(a, b, col, width);
  const ImVec2 d = b - a;
  const float len = std::sqrt(d.x * d.x + d.y * d.y);
  if (len > 0.0f) {
    const ImVec2 tick = ImVec2(-d.y, d.x) * (GUIDE_MEASURE_TICK / len);
    dl->AddLine(a - tick, a + tick, col, width);
    dl->AddLine(b - tick, b + tick, col, width);
  }
}

/* A Perspective or Grid guide's handle and its line under `mouse` (screen): the handle first (the picked
 * guide's before the others'), then the nearest line. Only shown, unlocked guides. */
static bool pick_guide_handle(const Document &doc, const ViewMapping &view, ImVec2 mouse, int *r_guide, int *r_point)
{
  float best = click_reach_pixels(GUIDE_HANDLE_PICK) * click_reach_pixels(GUIDE_HANDLE_PICK);
  bool found = false;
  for (int pass = 0; pass < 2 && !found; pass++) {
    for (int g = 0; g < int(doc.guides.size()); g++) {
      const DrawingGuide &guide = doc.guides[size_t(g)];
      if (guide.type == GuideType::Line || guide.hidden || guide.locked || ((g == doc.active_guide) != (pass == 0))) {
        continue;
      }
      for (int k = 0; k < int(guide.points.size()); k++) {
        const float d = ImLengthSqr(view.to_screen(guide.points[size_t(k)]) - mouse);
        if (d < best) {
          best = d;
          *r_guide = g;
          *r_point = k;
          found = true;
        }
      }
    }
  }
  return found;
}

static int pick_guide_line(const Document &doc, const ViewMapping &view, ImVec2 mouse)
{
  float2 lo, hi;
  view_box(view, lo, hi);
  const float px = 1.0f / std::max(view.zoom, 1e-9f);
  float best = GUIDE_LINE_PICK * px;
  int found = -1;
  for (int g = 0; g < int(doc.guides.size()); g++) {
    const DrawingGuide &guide = doc.guides[size_t(g)];
    if (guide.type == GuideType::Line || guide.hidden || guide.locked) {
      continue;
    }
    const float d = guide_distance(guide, view.to_document(mouse), lo, hi, GUIDE_MIN_CELL_PIXELS * px);
    if (d < best) {
      best = d;
      found = g;
    }
  }
  return found;
}

/* Annotations and drawing guides over the drawing, as the overlays say (a guide tool shows the guides
 * even with Guides off: they are what it works on). */
static void draw_annotation_overlays(ImDrawList *dl, Context &C, const ViewMapping &view, int hovered_guide)
{
  const Document &doc = *C.document;
  const ViewportOverlays &ov = doc.overlays;
  const ToolSettings &ts = C.wm->tool_settings;
  const bool guide_tool = ts.tool == Tool::Annotate && annotate_mode_is_guide(ts.annotate_mode);
  float2 lo, hi;
  view_box(view, lo, hi);
  const float px = 1.0f / std::max(view.zoom, 1e-9f);
  const GuideSettings &look = doc.guide_settings;
  if (look.show && ((ov.show && ov.guides) || guide_tool)) {
    std::vector<std::pair<float2, float2>> segments;
    for (int g = 0; g < int(doc.guides.size()); g++) {
      const DrawingGuide &guide = doc.guides[size_t(g)];
      if (guide.hidden) {
        continue;
      }
      const bool picked = guide_tool && g == doc.active_guide;
      if (guide.type == GuideType::Line) {
        guide_measure_draw(dl, view, guide, picked ? GUIDE_MEASURE_PICKED_WIDTH : GUIDE_MEASURE_WIDTH,
                           guide_color(look, 1.0f));
        continue;
      }
      segments.clear();
      guide_segments(guide, lo, hi, GUIDE_MIN_CELL_PIXELS * px, segments);
      const ImU32 col = guide_color(look, picked ? 0.9f : g == hovered_guide ? 0.75f : 0.47f);
      for (const auto &[a, b] : segments) {
        dl->AddLine(view.to_screen(a), view.to_screen(b), col, 1.0f);
      }
    }
  }
  if (ov.show && ov.annotations) {
    for (const AnnotationLayer &layer : doc.annotations) {
      if (layer.hidden) {
        continue;
      }
      const ImU32 col = color_u32(layer.color);
      for (const AnnotationStroke &stroke : layer.strokes) {
        annotation_stroke_draw(dl, view, stroke.points, stroke.start, stroke.end, layer.thickness, col);
      }
    }
  }
  if (guide_tool && look.show) {
    /* The handles to drag: every shown, unlocked Perspective's and Grid's, the picked guide's filled. */
    const ImU32 handle = guide_color(look, 1.0f);
    for (int g = 0; g < int(doc.guides.size()); g++) {
      const DrawingGuide &guide = doc.guides[size_t(g)];
      if (guide.type == GuideType::Line || guide.hidden || guide.locked) {
        continue;
      }
      for (const float2 &p : guide.points) {
        const ImVec2 s = view.to_screen(p);
        if (g == doc.active_guide) {
          dl->AddCircleFilled(s, 5.0f, handle);
          dl->AddCircle(s, 5.0f, IM_COL32(0x10, 0x10, 0x10, 255), 0, 1.0f);
        }
        else {
          dl->AddCircleFilled(s, 4.0f, IM_COL32(0x20, 0x20, 0x20, 220));
          dl->AddCircle(s, 4.0f, handle, 0, 1.5f);
        }
      }
    }
  }
}

/* A drag with the Annotate tool. */
struct AnnotateDrag {
  bool active = false;
  int area = -1;
  AnnotateMode mode = AnnotateMode::Draw;
  ImVec2 press;
  float2 press_document;
  /* A stroke being drawn (document space). */
  std::vector<float2> points;
  bool erasing = false;
  bool erased = false;
  /* A guide being changed: its handle (`point`; -1 moves all of it), or being made. */
  int guide = -1;
  int point = -1;
  bool creating = false;
  /* A Line guide: what the drag does to it. It's left alone until the mouse has gone GUIDE_DRAG_START, and
   * a new one is only made then. */
  GuideGrab grab = GuideGrab::None;
  bool dragged = false;
  bool duplicated = false;
  std::vector<float2> start_points;
  std::vector<DrawingGuide> guides_before;
  int active_before = -1;
  bool snapped = false;
};
static AnnotateDrag g_annotate;
/* A header field changed the document: one undo step once it's let go. */
static bool g_annotate_header_changed = false;

/* Ramer-Douglas-Peucker: the stroke with the points it doesn't need (within `tolerance`) left out. */
static std::vector<float2> simplify_polyline(const std::vector<float2> &points, float tolerance)
{
  if (points.size() < 3) {
    return points;
  }
  std::vector<bool> keep(points.size(), false);
  keep.front() = keep.back() = true;
  std::vector<std::pair<size_t, size_t>> stack = {{0, points.size() - 1}};
  while (!stack.empty()) {
    const auto [a, b] = stack.back();
    stack.pop_back();
    float worst = 0.0f;
    size_t at = a;
    for (size_t k = a + 1; k < b; k++) {
      const float d = distance_to_segment(points[k], points[a], points[b]);
      if (d > worst) {
        worst = d;
        at = k;
      }
    }
    if (worst > tolerance) {
      keep[at] = true;
      stack.push_back({a, at});
      stack.push_back({at, b});
    }
  }
  std::vector<float2> out;
  for (size_t k = 0; k < points.size(); k++) {
    if (keep[k]) {
      out.push_back(points[k]);
    }
  }
  return out;
}

static std::string points_text(const std::vector<float2> &points)
{
  std::string text;
  char buf[64];
  for (const float2 &p : points) {
    std::snprintf(buf, sizeof(buf), "%s%.9g %.9g", text.empty() ? "" : " ", double(p.x), double(p.y));
    text += buf;
  }
  return text;
}

static GuideType annotate_guide_type(AnnotateMode mode)
{
  return mode == AnnotateMode::GuidePerspective ? GuideType::Perspective
         : mode == AnnotateMode::GuideGrid      ? GuideType::Grid
                                                : GuideType::Line;
}

/* A perspective guide being made: its third vanishing point below the middle of the first two. */
static void perspective_third_point(DrawingGuide &g)
{
  if (g.type == GuideType::Perspective && g.points.size() == 3) {
    const float span = length(g.points[1] - g.points[0]);
    g.points[2] = (g.points[0] + g.points[1]) * 0.5f + float2(0.0f, -span * 0.8f);
  }
}

/* The Annotate tool's clicks and drags in the viewport. Returns the guide under the mouse (-1). */
static int annotate_tool_input(Context &C, const Area &area, const ViewMapping &view, bool hovered, ImDrawList *dl)
{
  ImGuiIO &io = ImGui::GetIO();
  ToolSettings &ts = C.wm->tool_settings;
  Document &doc = *C.document;
  AnnotateDrag &d = g_annotate;
  const AnnotateMode mode = d.active ? d.mode : ts.annotate_mode;
  const bool guide_mode = annotate_mode_is_guide(mode);
  const bool guides_shown = doc.guide_settings.show;
  const float2 mouse = view.to_document(io.MousePos);
  const float px = 1.0f / std::max(view.zoom, 1e-9f);
  const float eraser = ts.annotate_eraser_radius;
  int hovered_guide = -1;
  /* X or Delete took the guide being dragged: the drag is over. */
  if (C.wm->guide_deleted) {
    C.wm->guide_deleted = false;
    if (d.active) {
      d = {};
      return -1;
    }
  }
  if (hovered) {
    C.wm->guide_hover = -1;
  }
  /* Ctrl, or the pen's eraser end, erases notes while drawing them. */
  const bool erase_held =
      (mode == AnnotateMode::Draw && io.KeyCtrl) ||
      ((mode == AnnotateMode::Draw || mode == AnnotateMode::Line) && C.wm->pen_eraser && C.wm->pen_near);

  /* What's under the mouse: the eraser's circle, or a guide's handle or line. */
  if (hovered && (mode == AnnotateMode::Eraser || erase_held || d.erasing)) {
    dl->AddCircle(io.MousePos, eraser, IM_COL32(0xff, 0xff, 0xff, 170), 0, 1.0f);
  }
  if (guide_mode && guides_shown && hovered && !d.active) {
    int g = -1, k = -1;
    GuideMeasureHit measure = pick_guide_measure(doc, view, io.MousePos);
    if (measure.guide >= 0) {
      /* Line guides show nothing for the mouse; X or Delete over one deletes it, as a right-click on or
       * round one does. */
      C.wm->guide_hover = measure.guide;
    }
    else if (pick_guide_handle(doc, view, io.MousePos, &g, &k)) {
      dl->AddCircle(view.to_screen(doc.guides[size_t(g)].points[size_t(k)]), 8.0f, IM_COL32(0xff, 0xff, 0xff, 220), 0, 1.5f);
      hovered_guide = g;
    }
    else {
      measure = pick_guide_turn(doc, view, io.MousePos);
      hovered_guide = measure.guide >= 0 ? -1 : pick_guide_line(doc, view, io.MousePos);
    }
    if (measure.guide >= 0 && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      operator_call(C, "guide.delete", {{"index", std::to_string(measure.guide)}});
      g_right_click_spent = true;
      return -1;
    }
  }

  if (!d.active && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (guides_shown || !guide_mode)) {
    d = {};
    d.active = true;
    d.area = area.id;
    d.mode = mode;
    d.press = io.MousePos;
    d.press_document = mouse;
    if (mode == AnnotateMode::Eraser || erase_held) {
      d.erasing = true;
      d.erased = annotation_erase(doc, mouse, eraser * px);
    }
    else if (!guide_mode) {
      if (const AnnotationLayer *layer = annotation_layer_active(doc); layer && layer->locked) {
        report(ReportType::Warning, "The annotation layer \"" + layer->name + "\" is locked");
        d = {};
      }
      else {
        d.points = {mouse};
      }
    }
    else {
      d.guides_before = doc.guides;
      d.active_before = doc.active_guide;
      int g = -1, k = -1;
      GuideMeasureHit measure = pick_guide_measure(doc, view, io.MousePos);
      const bool handle = measure.guide < 0 && pick_guide_handle(doc, view, io.MousePos, &g, &k);
      if (measure.guide < 0 && !handle) {
        measure = pick_guide_turn(doc, view, io.MousePos);
      }
      if (measure.guide >= 0 && io.KeyAlt) {
        operator_call(C, "guide.delete", {{"index", std::to_string(measure.guide)}});
        d = {};
        return -1;
      }
      if (measure.guide >= 0) {
        d.guide = measure.guide;
        d.grab = measure.grab;
        d.point = measure.end;
        /* Double-clicking its line drags a copy away. */
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            (measure.grab == GuideGrab::Move || measure.grab == GuideGrab::RotateCenter))
        {
          DrawingGuide copy = doc.guides[size_t(measure.guide)];
          doc.guides.push_back(std::move(copy));
          d.guide = int(doc.guides.size()) - 1;
          d.duplicated = true;
        }
      }
      else if (handle) {
        d.guide = g;
        d.point = k;
      }
      else if ((g = pick_guide_line(doc, view, io.MousePos)) >= 0) {
        d.guide = g;
        d.point = -1;
      }
      else {
        d.creating = true;
        if (annotate_guide_type(mode) == GuideType::Line) {
          /* A click on empty canvas only drops the pick: the drag makes the guide. */
          d.grab = GuideGrab::Draw;
          d.point = 1;
        }
        else {
          /* On empty canvas: a new guide of the tool's kind, its first handle here, the drag placing the
           * next. */
          DrawingGuide made;
          made.type = annotate_guide_type(mode);
          made.lines = ts.guide_lines;
          float2 first = mouse;
          if (io.KeyCtrl) {
            first = guide_snap(doc, -1, 0, mouse, GUIDE_SNAP_PIXELS * px, nullptr);
          }
          const int count = guide_point_count(made.type, ts.guide_points);
          made.points.assign(size_t(count), first);
          doc.guides.push_back(std::move(made));
          d.guide = int(doc.guides.size()) - 1;
          d.point = count >= 2 ? 1 : 0;
        }
      }
      doc.active_guide = d.guide;
      C.wm->guide_hover = -1;
      if (d.guide >= 0) {
        d.start_points = doc.guides[size_t(d.guide)].points;
      }
    }
  }

  if (!d.active || d.area != area.id) {
    return hovered_guide;
  }
  /* Esc or right-click while dragging: as it was. */
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (guide_mode) {
      doc.guides = d.guides_before;
      doc.active_guide = d.active_before;
    }
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    d = {};
    return hovered_guide;
  }
  const ImVec2 moved_by = io.MousePos - d.press;
  const bool moved = moved_by.x * moved_by.x + moved_by.y * moved_by.y > 9.0f;
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (d.erasing) {
      d.erased |= annotation_erase(doc, mouse, eraser * px);
    }
    else if (mode == AnnotateMode::Draw || mode == AnnotateMode::Line) {
      if (mode == AnnotateMode::Draw && length(mouse - d.points.back()) > px * 2.0f) {
        d.points.push_back(mouse);
      }
      const AnnotationLayer shown = annotation_layer_active(doc) ? *annotation_layer_active(doc) : AnnotationLayer();
      const std::vector<float2> preview = mode == AnnotateMode::Line ? std::vector<float2>{d.points[0], mouse} : d.points;
      annotation_stroke_draw(dl, view, preview, mode == AnnotateMode::Line ? ts.annotate_start : AnnotationEnd::None,
                             mode == AnnotateMode::Line ? ts.annotate_end : AnnotationEnd::None, shown.thickness,
                             color_u32(shown.color));
    }
    else if (d.grab != GuideGrab::None) {
      d.dragged |= moved_by.x * moved_by.x + moved_by.y * moved_by.y > GUIDE_DRAG_START * GUIDE_DRAG_START;
      if (d.dragged && d.guide < 0) {
        DrawingGuide made;
        made.points = {d.press_document, d.press_document};
        doc.guides.push_back(std::move(made));
        d.guide = doc.active_guide = int(doc.guides.size()) - 1;
        d.start_points = doc.guides.back().points;
      }
      if (d.dragged && d.guide < int(doc.guides.size())) {
        DrawingGuide &g = doc.guides[size_t(d.guide)];
        /* Ctrl keeps it to its angle steps; a Line takes no notice of points or other guides. */
        g.points = guide_line_drag(d.grab, d.point, d.start_points, d.press_document, mouse, io.KeyCtrl);
      }
    }
    else if (d.guide >= 0 && d.guide < int(doc.guides.size())) {
      DrawingGuide &g = doc.guides[size_t(d.guide)];
      if (d.point >= 0) {
        float2 to = mouse;
        d.snapped = false;
        if (io.KeyCtrl) {
          to = guide_snap(doc, d.guide, d.point, mouse, GUIDE_SNAP_PIXELS * px, &d.snapped);
        }
        g.points[size_t(d.point)] = to;
        if (d.creating) {
          perspective_third_point(g);
        }
        if (d.snapped) {
          dl->AddCircle(view.to_screen(to), 9.0f, IM_COL32(0xff, 0xd0, 0x40, 255), 0, 2.0f);
        }
      }
      else {
        const float2 delta = mouse - d.press_document;
        for (size_t k = 0; k < g.points.size() && k < d.start_points.size(); k++) {
          g.points[k] = d.start_points[k] + delta;
        }
      }
    }
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (d.erasing) {
      if (d.erased) {
        push_undo(C, "Erase Annotation");
      }
    }
    else if (mode == AnnotateMode::Draw) {
      std::vector<float2> stroke = simplify_polyline(d.points, px * 0.6f);
      if (stroke.size() == 1) {
        /* A click: a dot. */
        stroke.push_back(stroke[0] + float2(px * 0.25f, 0.0f));
      }
      operator_call(C, "annotation.draw", {{"points", points_text(stroke)}});
    }
    else if (mode == AnnotateMode::Line) {
      if (moved) {
        operator_call(C, "annotation.draw",
                      {{"points", points_text({d.points[0], mouse})},
                       {"start", annotation_end_identifier(ts.annotate_start)},
                       {"end", annotation_end_identifier(ts.annotate_end)}});
      }
    }
    else if (d.grab != GuideGrab::None) {
      const bool turning = d.grab == GuideGrab::RotateEnd || d.grab == GuideGrab::RotateCenter;
      if (d.guide >= 0 && d.guide < int(doc.guides.size())) {
        DrawingGuide &g = doc.guides[size_t(d.guide)];
        if (length(g.points[1] - g.points[0]) < std::max(GUIDE_LINE_MIN_LENGTH, GUIDE_DRAG_START * px)) {
          /* Dragged down to nothing: it goes. */
          doc.guides.erase(doc.guides.begin() + d.guide);
          doc.active_guide = -1;
          if (!d.creating) {
            push_undo(C, "Delete Guide");
          }
        }
        else if (d.creating) {
          push_undo(C, "Add Line Guide");
        }
        else if (d.duplicated) {
          push_undo(C, "Duplicate Guide");
        }
        else if (d.dragged) {
          push_undo(C, turning ? "Rotate Guide" : d.grab == GuideGrab::End ? "Move Guide Handle" : "Move Guide");
        }
        else if (turning) {
          /* A click beside the picked guide drops the pick. */
          doc.active_guide = -1;
        }
      }
    }
    else if (d.guide >= 0 && d.guide < int(doc.guides.size())) {
      DrawingGuide &g = doc.guides[size_t(d.guide)];
      const bool lone_point = g.type == GuideType::Perspective && g.points.size() == 1;
      if (d.creating && !moved && !lone_point) {
        /* A click on empty canvas makes nothing, and picks nothing. */
        doc.guides = d.guides_before;
        doc.active_guide = -1;
      }
      else if (d.creating) {
        guide_validate(g);
        push_undo(C, (std::string("Add ") + guide_type_name(g.type) + " Guide").c_str());
      }
      else if (moved) {
        guide_validate(g);
        push_undo(C, d.point >= 0 ? "Move Guide Handle" : "Move Guide");
      }
    }
    d = {};
  }
  return hovered_guide;
}

/* The Annotate tool's header: the annotation layer and its look, or the picked guide's settings. */
static void annotate_header(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  Document &doc = *C.document;
  const AnnotateMode mode = ts.annotate_mode;
  const float h = ImGui::GetFrameHeight();
  ImGui::TextUnformatted(annotate_mode_name(mode));
  ImGui::SameLine();
  if (!annotate_mode_is_guide(mode)) {
    AnnotationLayer *layer = annotation_layer_active(doc);
    Color color = layer ? layer->color : AnnotationLayer().color;
    if (color_swatch("##annotation_color", color, "The annotation layer's colour")) {
      annotation_layer_ensure(doc).color = color;
      g_annotate_header_changed = true;
    }
    ImGui::SameLine();
    const std::string layer_name = layer ? layer->name : std::string("Note");
    if (ImGui::Button((layer_name + "  v##annotation_layers").c_str())) {
      ImGui::OpenPopup("##annotation_layers");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Annotation layers: the one new notes go on, and the others");
    }
    if (ImGui::BeginPopup("##annotation_layers")) {
      for (int i = int(doc.annotations.size()) - 1; i >= 0; i--) {
        AnnotationLayer &l = doc.annotations[size_t(i)];
        ImGui::PushID(i);
        if (header_toggle(l.hidden ? "Show" : "Hide", l.hidden, l.hidden ? "Hidden: show it" : "Hide its notes")) {
          operator_call(C, "annotation.layer_set", {{"index", std::to_string(i)}, {"hidden", l.hidden ? "0" : "1"}});
        }
        ImGui::SameLine(0, 2);
        if (header_toggle(l.locked ? "Unlock" : "Lock", l.locked, l.locked ? "Locked: unlock it" : "Keep it from changing")) {
          operator_call(C, "annotation.layer_set", {{"index", std::to_string(i)}, {"locked", l.locked ? "0" : "1"}});
        }
        ImGui::SameLine();
        if (ImGui::Selectable(l.name.c_str(), i == doc.active_annotation, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(h * 7.0f, 0)))
        {
          operator_call(C, "annotation.layer_set", {{"index", std::to_string(i)}, {"active", "1"}});
        }
        ImGui::PopID();
      }
      if (!doc.annotations.empty()) {
        AnnotationLayer &active = doc.annotations[size_t(doc.active_annotation)];
        char name[128];
        std::snprintf(name, sizeof(name), "%s", active.name.c_str());
        ImGui::SetNextItemWidth(h * 9.0f);
        if (ImGui::InputText("##annotation_name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) &&
            name[0] != '\0')
        {
          operator_call(C, "annotation.layer_set", {{"name", name}});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("The active layer's name (Enter renames it)");
        }
      }
      if (ImGui::Button("Add Layer")) {
        operator_call(C, "annotation.layer_add");
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(doc.annotations.empty());
      if (ImGui::Button("Delete Layer")) {
        operator_call(C, "annotation.layer_remove");
      }
      ImGui::EndDisabled();
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    float thickness = layer ? layer->thickness : AnnotationLayer().thickness;
    ImGui::SetNextItemWidth(h * 5.0f);
    if (ui_drag_float("##annotation_thickness", &thickness, 0.1f, ANNOTATION_THICKNESS_MIN, ANNOTATION_THICKNESS_MAX,
                         "Thickness %.0f px", ImGuiSliderFlags_AlwaysClamp))
    {
      annotation_layer_ensure(doc).thickness = thickness;
      g_annotate_header_changed = true;
    }
    if (mode == AnnotateMode::Line) {
      const auto end_combo = [&](const char *id, const char *label, AnnotationEnd &value) {
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(h * 5.0f);
        if (ImGui::BeginCombo(id, annotation_end_name(value))) {
          for (int k = 0; k < ANNOTATION_END_COUNT; k++) {
            if (ImGui::Selectable(annotation_end_name(AnnotationEnd(k)), int(value) == k)) {
              value = AnnotationEnd(k);
            }
          }
          ImGui::EndCombo();
        }
      };
      end_combo("##annotation_start", "Start", ts.annotate_start);
      end_combo("##annotation_end", "End", ts.annotate_end);
    }
    else if (mode == AnnotateMode::Eraser) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(h * 5.0f);
      ui_drag_float("##eraser_radius", &ts.annotate_eraser_radius, 0.5f, 2.0f, 200.0f, "Radius %.0f px",
                       ImGuiSliderFlags_AlwaysClamp);
    }
  }
  else {
    DrawingGuide *picked = doc.active_guide >= 0 && doc.active_guide < int(doc.guides.size())
                               ? &doc.guides[size_t(doc.active_guide)]
                               : nullptr;
    const bool picked_perspective = picked && picked->type == GuideType::Perspective;
    GuideSettings &look = doc.guide_settings;
    if (ImGui::Checkbox("Show guides", &look.show)) {
      g_annotate_header_changed = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(h * 3.5f);
    if (ui_drag_int("##guide_hue", &look.hue, 1.0f, 0, 360, "Hue %d", ImGuiSliderFlags_AlwaysClamp)) {
      g_annotate_header_changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Every guide's colour");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(h * 4.5f);
    if (ui_drag_int("##guide_opacity", &look.opacity, 1.0f, 0, 100, "Opacity %d%%", ImGuiSliderFlags_AlwaysClamp)) {
      g_annotate_header_changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How solid every guide is");
    }
    ImGui::SameLine();
    if (picked && picked->type == GuideType::Line && picked->points.size() >= 2) {
      ImGui::Text("Length: %.3f", double(length(picked->points[1] - picked->points[0])));
    }
    else {
      ImGui::TextUnformatted("Length: ---");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Mirror H")) {
      operator_call(C, "guide.mirror", {{"axis", "X"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Mirror the picked guide horizontally, across the document's centre");
    }
    ImGui::SameLine(0, 1);
    if (ImGui::Button("Mirror V")) {
      operator_call(C, "guide.mirror", {{"axis", "Y"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Mirror the picked guide vertically, across the document's centre");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(doc.guides.empty());
    if (ImGui::Button("Delete All")) {
      operator_call(C, "guide.delete_all");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (picked_perspective || mode == AnnotateMode::GuidePerspective) {
      /* The picked perspective guide's, else new ones'. */
      const int count = picked_perspective ? int(picked->points.size()) : ts.guide_points;
      ImGui::TextUnformatted("Points");
      for (int n = 1; n <= 3; n++) {
        ImGui::SameLine(0, n == 1 ? -1.0f : 1.0f);
        const std::string label = std::to_string(n) + "##guide_points";
        if (header_toggle(label.c_str(), count == n, "How many vanishing points: one, two or three point perspective")) {
          if (picked_perspective) {
            operator_call(C, "guide.set", {{"count", std::to_string(n)}});
          }
          else {
            ts.guide_points = n;
          }
        }
      }
      ImGui::SameLine();
      int lines = picked_perspective ? picked->lines : ts.guide_lines;
      ImGui::SetNextItemWidth(h * 4.5f);
      if (ui_drag_int("##guide_lines", &lines, 0.2f, GUIDE_LINES_MIN, GUIDE_LINES_MAX, "Lines %d",
                         ImGuiSliderFlags_AlwaysClamp))
      {
        if (picked_perspective) {
          picked->lines = lines;
          g_annotate_header_changed = true;
        }
        else {
          ts.guide_lines = lines;
        }
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("How many lines go round each vanishing point");
      }
      ImGui::SameLine();
    }
    if (picked) {
      if (header_toggle("Hide", picked->hidden, "Hide the picked guide")) {
        operator_call(C, "guide.set", {{"hidden", picked->hidden ? "0" : "1"}});
      }
      ImGui::SameLine(0, 1);
      if (header_toggle("Lock", picked->locked, "Lock the picked guide: its handles can't be dragged")) {
        operator_call(C, "guide.set", {{"locked", picked->locked ? "0" : "1"}});
      }
      ImGui::SameLine(0, 1);
      if (ImGui::Button("Delete")) {
        operator_call(C, "guide.delete");
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Delete the picked guide (X)");
      }
      ImGui::SameLine();
    }
  }
  if (g_annotate_header_changed && !ImGui::IsAnyItemActive()) {
    push_undo(C, "Change Annotation");
    g_annotate_header_changed = false;
  }
}

static bool menu_operator(Context &C, const char *label, const char *idname, const OpArgs &args = {})
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return false;
  }
  const std::string shortcut = args.empty() ? keymaps().shortcut_for(idname) : std::string();
  if (ui_menu_item(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, operator_poll(C, *ot))) {
    operator_call(C, idname, args);
    return true;
  }
  return false;
}

/* The Motion Path tool's Edit menus: V (handle types of the selected keys) and the right click's, for the keys and
 * dots selected. Each item is one undo step. */
static void motion_path_menus(Context &C, bool hovered)
{
  WindowManager &wm = *C.wm;
  const ToolSettings &ts = wm.tool_settings;
  const bool edit = C.document && ts.tool == Tool::MotionPath && ts.path_brush == PathBrush::Edit;
  if (hovered && wm.menu_request == "motion_path_handles") {
    ImGui::OpenPopup("##path_handles");
    wm.menu_request.clear();
  }
  if (edit && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right) && !g_right_click_spent && !g_path_drag.active)
  {
    if (!wm.path_handle_picks.empty()) {
      ImGui::OpenPopup("##path_handles");
    }
    else if (!path_edit_selection(C).empty()) {
      ImGui::OpenPopup("##path_menu");
    }
  }
  if (ui_menu_begin("##path_handles")) {
    ImGui::TextDisabled("Path Handles");
    ImGui::Separator();
    menu_operator(C, "Auto", "anim.motion_path_handles_type", {{"mode", "AUTO"}});
    menu_operator(C, "Aligned", "anim.motion_path_handles_type", {{"mode", "ALIGNED"}});
    menu_operator(C, "Broken", "anim.motion_path_handles_type", {{"mode", "BROKEN"}});
    ui_menu_end();
  }
  if (ui_menu_begin("##path_menu")) {
    const Document &doc = *C.document;
    const std::vector<std::pair<MotionPathTarget, int>> picked = path_edit_selection(C);
    const bool keys = picked.size() > wm.path_dots.size();
    ImGui::TextDisabled(keys ? "Path Keys" : "Path Dots");
    ImGui::Separator();
    if (keys) {
      menu_operator(C, "Align Handles", "anim.motion_path_handles_type", {{"mode", "ALIGNED"}});
      menu_operator(C, "Auto Handles", "anim.motion_path_handles_type", {{"mode", "AUTO"}});
      menu_operator(C, "Break Handles", "anim.motion_path_handles_type", {{"mode", "BROKEN"}});
      ImGui::Separator();
      menu_operator(C, "Delete Keys", "anim.motion_path_keys_delete");
    }
    if (!wm.path_dots.empty()) {
      menu_operator(C, "Even Spacing", "anim.motion_path_even_spacing");
    }
    menu_operator(C, "Smooth Arc", "anim.motion_path_smooth_arc");
    if (!picked.empty()) {
      ImGui::Separator();
      OpArgs args = motion_path_target_args(doc, picked.front().first);
      args["frame"] = std::to_string(picked.front().second);
      args["go"] = "1";
      menu_operator(C, "Go to Frame", "anim.motion_path_select", args);
    }
    ui_menu_end();
  }
}

/* The Add menu's items (Shift A), each with its icon drawn in the room the label leaves: Blender's dots on
 * the points. They go at the 2D cursor, in the active layer's space, the size of a quarter of the frame. */
enum class AddIcon { Point, Line, Square, Circle };

static void add_menu_icon(ImDrawList *dl, ImVec2 min, float s, AddIcon icon, ImU32 col)
{
  const ImVec2 c = min + ImVec2(s * 0.5f, s * 0.5f);
  const float r = s * 0.32f, dot = std::max(1.5f, s * 0.09f);
  switch (icon) {
    case AddIcon::Point:
      dl->AddCircle(c, dot * 2.0f, col, 0, 1.2f);
      dl->AddCircleFilled(c, dot, col);
      break;
    case AddIcon::Line: {
      const ImVec2 a = c + ImVec2(-r, r), b = c + ImVec2(r, -r);
      dl->AddLine(a, b, col, 1.4f);
      dl->AddCircleFilled(a, dot, col);
      dl->AddCircleFilled(b, dot, col);
      break;
    }
    case AddIcon::Square: {
      const ImVec2 a = c - ImVec2(r, r), b = c + ImVec2(r, r);
      dl->AddRect(a, b, col, 0.0f, 0, 1.4f);
      for (const ImVec2 &p : {a, b, ImVec2(a.x, b.y), ImVec2(b.x, a.y)}) {
        dl->AddCircleFilled(p, dot, col);
      }
      break;
    }
    case AddIcon::Circle:
      dl->AddCircle(c, r, col, 0, 1.4f);
      for (const ImVec2 &d : {ImVec2(r, 0), ImVec2(-r, 0), ImVec2(0, r), ImVec2(0, -r)}) {
        dl->AddCircleFilled(c + d, dot, col);
      }
      break;
  }
}

static std::string num_arg(float v);

static void add_menu_items(Context &C)
{
  Document &doc = *C.document;
  const ToolSettings &ts = C.wm->tool_settings;
  const Layer *layer = document_active_layer(doc);
  const float2 at = layer ? layer_world_transform(doc, layer->id).inverted().apply(ts.cursor) : ts.cursor;
  const std::string x = num_arg(at.x), y = num_arg(at.y), size = num_arg(0.5f);
  const std::string fill = ts.draw_fill ? "1" : "0";
  ImGui::TextDisabled("Add");
  ImGui::Separator();
  const auto item = [&](const char *label, AddIcon icon, const char *idname, const OpArgs &args) {
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const OperatorType *ot = operators().find(idname);
    const ImU32 ink = ImGui::GetColorU32(ot && operator_poll(C, *ot) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    menu_operator(C, label, idname, args);
    add_menu_icon(ImGui::GetWindowDrawList(), row, h, icon, ink);
  };
  item("      Point", AddIcon::Point, "mesh.primitive_point_add", {{"x", x}, {"y", y}});
  item("      Line", AddIcon::Line, "mesh.primitive_line_add", {{"x", x}, {"y", y}, {"length", size}});
  item("      Square", AddIcon::Square, "mesh.primitive_box_add",
       {{"x", x}, {"y", y}, {"width", size}, {"height", size}, {"fill", fill}});
  item("      Circle", AddIcon::Circle, "mesh.primitive_circle_add",
       {{"x", x}, {"y", y}, {"width", size}, {"height", size}, {"fill", fill}});
  if (doc.frame_current != 0 && ImGui::IsWindowHovered()) {
    ImGui::SetTooltip("Drawings are added on frame 0 (the setup frame)");
  }
}

/* The Apply menu's items (Ctrl A, and the Armature menu's Apply), as Blender's in pose mode. */
static void bone_apply_items(Context &C)
{
  menu_operator(C, "Pose as Rest Pose", "armature.apply_pose_as_rest");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("The bones' pose on this frame becomes the rest pose (frame 0). The drawings come along, "
                      "and every keyed frame of every action shows as it did");
  }
  menu_operator(C, "Selected as Rest Pose", "armature.apply_selected_as_rest");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("The same for the selected bones only; the others keep where they rest");
  }
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
static void proportional_buttons(ToolSettings &ts, float h, float arrow_w);

void motion_path_proportional_settings(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  const float h = ImGui::GetFrameHeight();
  proportional_buttons(ts, h, h * 0.75f);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(h * 5.5f);
  ImGui::DragFloat("##path_proportional_size", &ts.path_proportional_size, 0.1f, 1.0f, 500.0f, "Influence %.0f frames",
                   ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How many frames either side of a dragged dot slide with it, with proportional editing on "
                      "(O; the wheel or Page Up / Page Down while dragging), and how far Alt S reaches");
  }
}

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
  ui_drag_float("##proportional_size", &ts.proportional_size, 0.01f, 1e-4f, 1e6f, "Proportional Size  %.3f",
                   ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How far from the selection points are dragged along (document units)");
  }
  ImGui::EndPopup();
}

/* The brush new strokes get: the Brush Settings window, on the tool. */
static void tool_brush_button(Context &C)
{
  const Document &doc = *C.document;
  const ToolSettings &ts = C.wm->tool_settings;
  const float icon_h = ImGui::GetFrameHeight();
  const ImVec2 icon_at = ImGui::GetCursorScreenPos();
  if (ImGui::Button(("     " + brush_shown_name(doc, ts.style.brush) + "##tool_brush").c_str())) {
    brush_window_open(BrushTarget::Tool);
  }
  brush_preset_icon(ImGui::GetWindowDrawList(), icon_at + ImVec2(2.0f, 0.0f), icon_at + ImVec2(icon_h, icon_h),
                    ImGui::GetColorU32(ImGuiCol_Text));
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("The brush for new strokes: click for Brush Settings");
  }
}

/* A header button with a down arrow opening a dropdown of settings, as Blender's tool header popovers. True while
 * it is open: the caller fills it and ends it with ImGui::EndPopup. */
static bool header_dropdown(const char *label, const char *popup, const char *tooltip)
{
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float text_w = ImGui::CalcTextSize(label).x;
  if (ImGui::Button((std::string("##") + popup).c_str(), ImVec2(text_w + style.FramePadding.x * 2.0f + h * 0.55f, h))) {
    ImGui::OpenPopup(popup);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
  dl->AddText(at + ImVec2(style.FramePadding.x, style.FramePadding.y), ink, label);
  const ImVec2 tip = at + ImVec2(style.FramePadding.x + text_w + h * 0.3f, h * 0.5f);
  dl->AddTriangleFilled(tip + ImVec2(-3.5f, -1.5f), tip + ImVec2(3.5f, -1.5f), tip + ImVec2(0.0f, 2.5f), ink);
  ImGui::SetNextWindowPos(ImVec2(at.x, at.y + h + 2.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopup(popup)) {
    return false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  return true;
}

/* A full-width bar in a dropdown, with what it does on hovering. */
static void dropdown_bar(const char *id, float *v, float min, float max, const char *format, const char *tooltip)
{
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 11.0f);
  ui_slider_float(id, v, min, max, format);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
}

/* How pressure maps to size, as Harmony's Pen Pressure Feel: a Bezier from no pressure, no size to full pressure,
 * full size, its two handles dragged. */
static void pressure_curve_edit(InkSettings &ink)
{
  const float size = ImGui::GetFrameHeight() * 7.0f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##pressure_curve", ImVec2(size, size));
  const auto place = [&](float2 p) { return at + ImVec2(p.x, 1.0f - p.y) * size; };
  static float2 *dragged = nullptr;
  if (ImGui::IsItemActivated()) {
    const ImVec2 mouse = ImGui::GetIO().MousePos, to_a = mouse - place(ink.pressure_curve_a),
                 to_b = mouse - place(ink.pressure_curve_b);
    const bool nearer_a = to_a.x * to_a.x + to_a.y * to_a.y <= to_b.x * to_b.x + to_b.y * to_b.y;
    dragged = nearer_a ? &ink.pressure_curve_a : &ink.pressure_curve_b;
  }
  if (ImGui::IsItemActive() && dragged) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    *dragged = {std::clamp((mouse.x - at.x) / size, 0.0f, 1.0f), std::clamp(1.0f - (mouse.y - at.y) / size, 0.0f, 1.0f)};
  }
  else {
    dragged = nullptr;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive()) {
    ImGui::SetTooltip("Pen pressure (across) to size (up): drag the handles. Bowed up, a light touch draws wide; "
                      "bowed down, it takes a firm press");
  }
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 corner = at + ImVec2(size, size);
  dl->AddRectFilled(at, corner, IM_COL32(0x1e, 0x1e, 0x1e, 255));
  for (int i = 1; i < 4; i++) {
    const float f = size * float(i) / 4.0f;
    dl->AddLine(at + ImVec2(f, 0), at + ImVec2(f, size), IM_COL32(0x3a, 0x3a, 0x3a, 255));
    dl->AddLine(at + ImVec2(0, f), at + ImVec2(size, f), IM_COL32(0x3a, 0x3a, 0x3a, 255));
  }
  const ImVec2 from = place({0.0f, 0.0f}), to = place({1.0f, 1.0f});
  const ImVec2 a = place(ink.pressure_curve_a), b = place(ink.pressure_curve_b);
  dl->AddLine(from, a, IM_COL32(0x80, 0x80, 0x80, 255));
  dl->AddLine(to, b, IM_COL32(0x80, 0x80, 0x80, 255));
  dl->AddBezierCubic(from, a, b, to, IM_COL32(0xe6, 0xe6, 0xe6, 255), 2.0f, 32);
  dl->AddCircleFilled(a, 4.0f, IM_COL32(0x47, 0x72, 0xb3, 255));
  dl->AddCircleFilled(b, 4.0f, IM_COL32(0x47, 0x72, 0xb3, 255));
  dl->AddRect(at, corner, IM_COL32(0x60, 0x60, 0x60, 255));
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (ImGui::Button("Linear")) {
    const InkSettings even;
    ink.pressure_curve_a = even.pressure_curve_a;
    ink.pressure_curve_b = even.pressure_curve_b;
  }
  ImGui::EndGroup();
}

/* The Stroke dropdown: how the hand becomes a line. */
static void draw_stroke_dropdown(WindowManager &wm)
{
  ToolSettings &ts = wm.tool_settings;
  InkSettings &ink = ts.ink;
  ImGui::TextUnformatted("Stabilizer");
  static const char *const names[] = {"Off", "Smart", "Pulled String"};
  int stabilizer = int(ink.stabilizer);
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 11.0f);
  if (ImGui::Combo("##stabilizer", &stabilizer, names, 3)) {
    ink.stabilizer = InkStabilizer(stabilizer);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Smart leaves out a slow hand's wobble and follows a fast one. Pulled String draws a line pulled "
                      "behind the pen on a string: exact corners, a learned feel");
  }
  if (ink.stabilizer == InkStabilizer::Smart) {
    dropdown_bar("##smoothing", &ink.smoothing, 0.0f, 1.0f, "Smoothing %.2f", "How much wobble a slow line loses");
  }
  else if (ink.stabilizer == InkStabilizer::PulledString) {
    dropdown_bar("##string", &ink.string_length, 1.0f, 100.0f, "String %.0f px", "How long the string is, in screen pixels");
    ImGui::Checkbox("Catch Up", &ink.catch_up);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("End the line at the pen, rather than a string's length short of it");
    }
  }
  ImGui::Spacing();
  dropdown_bar("##fidelity", &ink.fidelity, 0.0f, 1.0f, "Fidelity %.2f",
               "How closely the line's points follow the hand: higher is closer and makes more of them");
  /* What the last stroke makes at this fidelity, worked out again only when one of them changes. */
  static float shown_fidelity = -1.0f;
  static size_t shown_samples = 0;
  static int shown_points = 0;
  const std::vector<InkPoint> &last = wm.draw_last.points();
  if (!last.empty()) {
    if (shown_fidelity != ink.fidelity || shown_samples != last.size()) {
      InkSettings settings = wm.draw_last.settings();
      settings.fidelity = ink.fidelity;
      shown_points = int(ink_fit(last, settings, wm.draw_last.view(), wm.draw_last_closed).size());
      /* A painted area's own points depend on what it merged with: nothing to say for those. */
      shown_fidelity = ink.fidelity;
      shown_samples = last.size();
    }
    if (!draw_mode_paints(ts.draw_mode)) {
      ImGui::TextDisabled("Last line makes %d points", shown_points);
    }
  }
  if (draw_mode_paints(ts.draw_mode)) {
    return;
  }
  ImGui::Spacing();
  ImGui::Checkbox("Close and Fill", &ts.draw_fill);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("A stroke ending where it started closes, and fills");
  }
  ImGui::Checkbox("Weld Ends", &ts.draw_weld);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("A stroke's ends join a point they finish on, so the two move together");
  }
  ImGui::Checkbox("Trim Overshoots", &ts.draw_trim);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("A stroke's ends past the last line they crossed go, on lifting");
  }
}

/* The Advanced dropdown: how the pen sets the line's width. */
static void draw_advanced_dropdown(ToolSettings &ts)
{
  InkSettings &ink = ts.ink;
  ImGui::TextUnformatted("Pressure");
  pressure_curve_edit(ink);
  dropdown_bar("##min_size", &ink.min_size, 0.0f, 1.0f, "Min Size %.2f", "The thinnest the line gets, as a part of its width");
  dropdown_bar("##pressure_smoothing", &ink.pressure_smoothing, 0.0f, 1.0f, "Pressure Smoothing %.2f",
               "Evens out pressure's jitter, which is what makes a slow line's width wobble");
  ImGui::Spacing();
  ImGui::TextUnformatted("Taper");
  dropdown_bar("##taper_start", &ink.taper_start, 0.0f, 300.0f, "Start %.0f px", "How long the line takes to widen from a point");
  dropdown_bar("##taper_end", &ink.taper_end, 0.0f, 300.0f, "End %.0f px", "How long the line takes to narrow to a point");
  ImGui::Spacing();
  ImGui::TextUnformatted("Width From");
  dropdown_bar("##speed_width", &ink.speed_width, 0.0f, 1.0f, "Speed %.2f", "A fast pen draws thinner: width without a tablet");
  dropdown_bar("##tilt_width", &ink.tilt_width, 0.0f, 1.0f, "Tilt %.2f", "A leaning pen draws wider, as the side of a pencil");
  ImGui::Spacing();
  dropdown_bar("##prediction", &ts.draw_prediction, 0.0f, 30.0f, "Prediction %.0f ms",
               "How far ahead of the pen the line is drawn, to keep up with the hand");
}

static void draw_header(Context &C)
{
  const Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  const float h = ImGui::GetFrameHeight();
  const DrawMode mode = ts.draw_mode;
  const bool bezier = mode == DrawMode::Pen, fill_pen = mode == DrawMode::FillPen, eraser = draw_mode_cuts(mode);
  const DrawPreset *preset = draw_preset_find(ts.draw_preset);
  const bool changed = preset && !bezier && (!(preset->ink == ts.ink) || preset->line_width != draw_width(ts, mode));
  const std::string name = bezier ? "Bezier Pen" : ts.draw_preset + (changed ? " *" : "");
  if (ImGui::Button((name + "##draw_preset").c_str())) {
    wm.draw_popup_request = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(changed ? "The pen, changed from its saved settings: click for the pens (Shift Space)"
                              : "The pen: click for the pens (Shift Space)");
  }
  ImGui::SameLine();
  if (!fill_pen && !eraser) {
    color_swatch("##line_color", ts.style.line, "Line colour for new strokes");
    ImGui::SameLine();
  }
  width_field(doc, ts, draw_width(ts, mode), !bezier);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(fill_pen || eraser ? "How wide the brush is (F)" : "How wide the line is (F)");
  }
  ImGui::SameLine();
  if (!eraser) {
    ImGui::SetNextItemWidth(h * 4.6f);
    ui_slider_float("##opacity", fill_pen ? &ts.style.fill.a : &ts.style.line.a, 0.0f, 1.0f, "Opacity %.2f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(fill_pen ? "How solid the fill is (Shift F)" : "How solid the line is (Shift F)");
    }
    ImGui::SameLine();
    color_swatch("##fill_color", ts.style.fill, fill_pen ? "The colour to paint with" : "Fill colour for new shapes");
    ImGui::SameLine();
  }
  if (!fill_pen && !eraser) {
    tool_brush_button(C);
    ImGui::SameLine();
  }
  if (bezier) {
    if (header_toggle("Auto Fill", ts.draw_fill, "Fill curves closed on their first point")) {
      ts.draw_fill = !ts.draw_fill;
    }
    return;
  }
  if (eraser) {
    if (mode == DrawMode::StrokeEraser) {
      if (header_toggle("Whole Line", ts.ink.erase_whole, "Take away the whole of every line touched, not just the part brushed")) {
        ts.ink.erase_whole = !ts.ink.erase_whole;
      }
      ImGui::SameLine(0, 2.0f);
    }
    if (header_toggle("All Layers", ts.draw_erase_all_layers, "Cut every drawing being edited, not only the active one")) {
      ts.draw_erase_all_layers = !ts.draw_erase_all_layers;
    }
    ImGui::SameLine(0, 2.0f);
    if (header_toggle("Selected Only", ts.draw_erase_selected, "Cut only the selected shapes")) {
      ts.draw_erase_selected = !ts.draw_erase_selected;
    }
    ImGui::SameLine();
  }
  if (header_dropdown("Stroke", "##draw_stroke", "Stabilizer and fidelity")) {
    draw_stroke_dropdown(wm);
    ImGui::EndPopup();
  }
  ImGui::SameLine(0, 2.0f);
  if (header_dropdown("Advanced", "##draw_advanced", "Pressure, tapers, width from speed and tilt, prediction")) {
    draw_advanced_dropdown(ts);
    ImGui::EndPopup();
  }
  ImGui::SameLine(0, 2.0f);
  if (header_dropdown("Cursor", "##draw_cursor", "What the mouse shows while drawing")) {
    ImGui::Checkbox("Width Circle", &ts.draw_cursor_circle);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show the width as a circle round the mouse");
    }
    ImGui::EndPopup();
  }
}

/* The Motion Path tool's header: the selected keys' interpolation, a ball moving that way beside it (hovering an
 * option moves it that way instead), the ease's strength, and Smooth Arc. */
static void motion_path_header(Context &C)
{
  const float h = ImGui::GetFrameHeight();
  /* Own Motion, and with a brush other than Edit its size and strength. Interpolation is the timeline's key header
   * (which follows the path); Smooth Arc, Even Spacing and the handles are in the path's menu; proportional editing
   * in the timeline's Motion Paths popover. */
  {
    Document &doc = *C.document;
    bool own = doc.path_own_motion;
    if (ImGui::Checkbox("Own Motion", &own)) {
      doc.path_own_motion = own;
      if (C.undo) {
        C.undo->mark_changed();
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("A path being edited (a key or dot picked) shows only its own motion: whatever else moves it "
                        "(a bone, a parent, a group) held as on this frame, the real path a faint ghost behind");
    }
    ImGui::SameLine();
    bool edit_layer = C.wm->tool_settings.path_edit_layer;
    if (ImGui::Checkbox("Edit Layer", &edit_layer)) {
      operator_call(C, "anim.motion_path_brush_set", {{"edit_layer", edit_layer ? "1" : "0"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Edit the layer itself: its origin's path, and the Transform Layer box to move, turn and "
                        "scale it (keyed on this frame). Clicks never pick its bones or points, and the timeline "
                        "shades its transform's stretch teal. Off: its bones and points, never its position");
    }
  }
  /* The brush picked on the shelf, and its settings. */
  ToolSettings &ts = C.wm->tool_settings;
  if (ts.path_brush != PathBrush::Edit) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path_brush_name(ts.path_brush));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(h * 4.5f);
    PathBrushSettings &bs = ts.path_brush_settings(ts.path_brush);
    ImGui::DragFloat("##path_radius", &bs.radius, 0.5f, 2.0f, 1000.0f, "Radius %.0f px",
                     ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The brush's size on screen (F)");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(h * 4.5f);
    ImGui::SliderFloat("##path_strength", &bs.strength, 0.0f, 1.0f, "Strength %.2f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How much each dab does (Shift F)");
    }
    if (path_brush_is_sculpt(ts.path_brush)) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(h * 3.6f);
      static const char *const falloffs[] = {"Smooth", "Sphere", "Constant"};
      int falloff = int(bs.falloff);
      if (ImGui::Combo("##path_falloff", &falloff, falloffs, 3)) {
        bs.falloff = SculptFalloff(falloff);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("How the brush fades toward its edge, as the Sculpt tool's: Smooth eases out, Sphere stays "
                          "strong almost to the edge, Constant is the same all over");
      }
    }
    if (ts.path_brush == PathBrush::SurfaceSmooth) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(h * 7.0f);
      ui_slider_float("##path_sensitivity", &bs.sensitivity, 0.001f, 1.0f, "Lump Sensitivity %.3f", 0, 0.0f, FLT_MAX);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("How big a lump Surface Smooth takes out of the path: 0 none, higher bigger ones, as the "
                          "Sculpt tool's");
      }
    }
  }
}

/* The header, and under it the Tool Settings bar unless it was hidden. */
static float header_rows_height()
{
  const float row = ImGui::GetFrameHeight() + 8.0f;
  return preferences().viewport_tool_settings ? row * 2.0f : row;
}

/* What the mouse does with the tool in hand, for the status bar. */
static const char *tool_hint(const ToolSettings &ts)
{
  switch (ts.tool) {
    case Tool::TransformLayer:
      return "Inside: move  |  corners, outside: turn  |  handle: scale (Shift: proportions)  |  edge: shear  |  "
             "Alt: about the pivot (drag it; double-click puts it back)";
    case Tool::SelectLayer:
      return "Click: select  |  again: the one below  |  Shift: add  |  Ctrl: remove  |  Alt-click from any tool";
    case Tool::Draw:
      return ts.draw_mode == DrawMode::Pen ? "Click to add points, drag to place  |  Click the first point to close  |  "
                                             "Ctrl: grid"
                                           : "";
    case Tool::Shape:
      return ts.shape == PrimitiveKind::Grid ? "Drag to draw  |  Shift: square  |  Alt: from centre  |  Ctrl: snap"
                                             : "Drag to draw  |  Shift: even  |  Alt: from centre  |  Ctrl: grid";
    case Tool::Annotate:
      if (annotate_mode_is_guide(ts.annotate_mode)) {
        return "Drag to make a guide  |  Drag an end or its line  |  Beside the picked one: turn  |  Ctrl: snap  |  "
               "Double-click: copy  |  Right-click or X: delete";
      }
      return ts.annotate_mode == AnnotateMode::Eraser ? "Drag to rub out notes"
             : ts.annotate_mode == AnnotateMode::Line ? "Drag to draw a line"
                                                      : "Drag to draw a note  |  Ctrl: erase";
    case Tool::SplitCurve:
      return ts.tool_temporary ? "Split once (K): click a line, then back to your tool  |  Shift: middle  |  Esc: cancel"
                               : "Click a line to add a point  |  Shift: middle  |  Ctrl-click a point: delete it";
    case Tool::Cursor:
      return "Click to place the cursor  |  Shift right-click places it from any tool";
    case Tool::MotionPath:
      switch (ts.path_brush) {
        case PathBrush::Edit:
          break;
        case PathBrush::Grab:
          return "Drag: the path under the brush follows, as the Sculpt tool's Grab, the keys keeping their timing  |  "
                 "Shift: Smooth  |  F: radius  |  Shift F: strength  |  Esc: cancel";
        case PathBrush::Smooth:
          return "Brush over a lumpy stretch: smooth it, as the Sculpt tool's Smooth  |  F: radius  |  Shift F: "
                 "strength  |  Esc: cancel";
        case PathBrush::SurfaceSmooth:
          return "Brush over the path: its lumps go, its arcs stay (Lump Sensitivity: how big)  |  F: radius  |  "
                 "Shift F: strength  |  Esc: cancel";
        case PathBrush::BlendSpeed:
          return "Paint: even out the spacing  |  Ctrl: sharpen it  |  F: radius  |  Shift F: strength  |  Esc: cancel";
        case PathBrush::Pinch:
          return "Paint: the dots crowd together (slower)  |  Ctrl: spread them (faster)  |  Shift: Blend Speed  |  "
                 "F: radius  |  Shift F: strength  |  Esc: cancel";
        case PathBrush::Slide:
          return "Drag: the dots in the circle slide along the path, the keys staying  |  Shift: Blend Speed  |  "
                 "F: radius  |  Esc: cancel";
      }
      return "Click: select  |  Shift: add  |  Shift drag: Blend Speed  |  Double click: its frame  |  Drag a key: "
             "move it, a dot: slide it (O: proportional)  |  Alt click: add a control point  |  Alt Shift: breakdown on "
             "the arc  |  G R S  |  Alt S: timing  |  V: handles  |  X: delete";
    default:
      return "";
  }
}

/* Under the header, Blender's Tool Settings: only what belongs to the tool in hand, on a strip just wide enough
 * for it, the drawing showing beside it. Its row stays when a tool has nothing to show, so switching tools moves
 * nothing below it. The wheel scrolls the strip when it doesn't fit. */
static void tool_settings_draw(Area &area, Context &C, ImVec2 origin, float width)
{
  Document &doc = *C.document;
  ToolSettings &ts = C.wm->tool_settings;
  const float row = ImGui::GetFrameHeight() + 8.0f;
  /* Over the viewport's button, so a click between the settings doesn't reach the drawing under them. It has to
   * come before them, so it takes last frame's width. */
  if (area.tool_settings_width > 0.0f) {
    const ImVec2 strip(std::min(area.tool_settings_width, width), row);
    ImGui::SetCursorScreenPos(origin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##tool_settings", strip);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(origin, origin + strip, false)) {
      area.tool_settings_scroll -= ImGui::GetIO().MouseWheel * row * 2.0f;
    }
  }
  /* The strip's width is known once the settings are drawn: it goes in the channel under them. */
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  ImGui::PushClipRect(origin, origin + ImVec2(width, row), true);
  ImGui::SetCursorScreenPos(origin + ImVec2(8.0f - area.tool_settings_scroll, 4.0f));
  ImGui::BeginGroup();
  /* A group with no items would measure out to the last item before it, the header's. */
  ImGui::Dummy(ImVec2(0.0f, 0.0f));
  ImGui::SameLine(0.0f, 0.0f);
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
  switch (ts.tool) {
    case Tool::TransformLayer:
      ImGui::Checkbox("Show Grid Lines", &ts.layer_grid);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Show the active layer's own grid across the viewport: squashed, turned and placed as the "
                          "layer is, its X axis red and its Y axis green through its origin");
      }
      break;
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
      break;
    case Tool::Select:
      break;
    case Tool::Draw:
    case Tool::Shape:
      if (ts.tool == Tool::Draw) {
        draw_header(C);
        break;
      }
      ImGui::TextUnformatted(primitive_name(ts.shape));
      ImGui::SameLine();
      color_swatch("##line_color", ts.style.line, "Line colour for new strokes");
      ImGui::SameLine();
      width_field(doc, ts, ts.style.line_width, false);
      ImGui::SameLine();
      color_swatch("##fill_color", ts.style.fill, "Fill colour for new shapes");
      ImGui::SameLine();
      tool_brush_button(C);
      ImGui::SameLine();
      if (ts.shape == PrimitiveKind::Grid) {
        /* A grid's cells: its lines are outlines only. */
        const float w = ImGui::GetFrameHeight() * 3.4f;
        ImGui::SetNextItemWidth(w);
        ui_drag_int("##grid_columns", &ts.grid_columns, 0.1f, 1, GRID_CELLS_MAX, "Columns %d");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("How many cells across a new grid has");
        }
        ImGui::SameLine(0, 1);
        ImGui::SetNextItemWidth(w);
        ui_drag_int("##grid_rows", &ts.grid_rows, 0.1f, 1, GRID_CELLS_MAX, "Rows %d");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("How many cells up a new grid has");
        }
        ts.grid_columns = std::clamp(ts.grid_columns, 1, GRID_CELLS_MAX);
        ts.grid_rows = std::clamp(ts.grid_rows, 1, GRID_CELLS_MAX);
      }
      else {
        if (header_toggle("Fill", ts.draw_fill, "Fill boxes and circles")) {
          ts.draw_fill = !ts.draw_fill;
        }
      }
      break;
    case Tool::Annotate:
      annotate_header(C);
      break;
    case Tool::SplitCurve:
    case Tool::Cursor:
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
    case Tool::Sculpt:
      sculpt_header(C);
      break;
    case Tool::MotionPath:
      motion_path_header(C);
      break;
  }
  ImGui::EndGroup();
  ImGui::PopClipRect();
  const float settings_w = ImGui::GetItemRectSize().x;
  area.tool_settings_width = settings_w > 0.0f ? settings_w + 16.0f : 0.0f;
  area.tool_settings_scroll = std::clamp(area.tool_settings_scroll, 0.0f,
                                         std::max(area.tool_settings_width - width, 0.0f));
  dl->ChannelsSetCurrent(0);
  if (area.tool_settings_width > 0.0f) {
    dl->AddRectFilled(origin, origin + ImVec2(std::min(area.tool_settings_width, width), row), COL_HEADER, 6.0f,
                      ImDrawFlags_RoundCornersBottomRight);
  }
  dl->ChannelsMerge();
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
  if (header_menu("View")) {
    Preferences &prefs = preferences();
    if (ImGui::MenuItem("Tool Settings", nullptr, &prefs.viewport_tool_settings)) {
      preferences_save();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The bar under the header, with the settings of the tool in hand");
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (header_menu("Select")) {
    const Layer *active_layer = document_active_layer(doc);
    if (active_layer && active_layer->is_bone_layer()) {
      menu_operator(C, "All", "armature.select_all");
      menu_operator(C, "None", "armature.select_none");
      menu_operator(C, "Invert", "armature.select_invert");
    }
    else {
      menu_operator(C, "All", "mesh.select_all");
      menu_operator(C, "None", "mesh.select_none");
      menu_operator(C, "Invert", "mesh.select_invert");
    }
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
      if (ImGui::BeginMenu("Apply")) {
        bone_apply_items(C);
        ImGui::EndMenu();
      }
      ImGui::Separator();
      menu_operator(C, "Delete Bones", "armature.bone_delete");
      ImGui::EndPopup();
    }
  }
  else if (ts.tool == Tool::WeightSelect || ts.tool == Tool::WeightPaint) {
    /* The weight tools' own menu, as Blender's Weights menu in weight paint mode. */
    ImGui::SameLine();
    if (header_menu("Weights")) {
      menu_operator(C, "Remove All Weights", "weight.remove_all", weight_args());
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Take the selected points out of every bone: they follow no bone");
      }
      menu_operator(C, "Clear Weights", "weight.clear", weight_args());
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Take every point of the active layer, or of the drawings in the active group, out of "
                          "every bone");
      }
      menu_operator(C, "Clean", "weight.clean", weight_args());
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Take the selected points (all when none are selected) out of bones that move them "
                          "less than 2%");
      }
      ImGui::Separator();
      menu_operator(C, "Automatic Weights", "weight.automatic", weight_automatic_args(C, false));
      {
        const Layer *armature = weight_armature(C);
        ImGui::BeginDisabled(!armature || std::none_of(armature->bones.begin(), armature->bones.end(), bone_picked));
        menu_operator(C, "Automatic: Selected Bones", "weight.automatic", weight_automatic_args(C, true));
        ImGui::EndDisabled();
      }
      menu_operator(C, "Assign", "weight.assign", weight_args());
      menu_operator(C, "Remove", "weight.remove", weight_args());
      menu_operator(C, "Normalize", "weight.normalize", weight_args());
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
      ImGui::EndPopup();
    }
  }

  ImGui::SameLine(0.0f, h);
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
  if (ts.tool == Tool::Select && editable_points) {
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
  }
  if (editable_points) {
    detail_header(C);
  }

  /* In the middle, as Blender's: mirror, snapping and proportional editing. Proportional's room is kept with no
   * points to edit, so the others stay put as the active layer changes. */
  const ImGuiStyle &style = ImGui::GetStyle();
  const float snap_w = ImGui::CalcTextSize("Snap").x + style.FramePadding.x * 2.0f;
  const float arrow_w = h * 0.75f;
  const float axis_w = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
  const float mirror_w = h + 2.0f + axis_w * 2.0f + 1.0f + h * 0.6f;
  const float proportional_w = h + 1.0f + h + arrow_w * 0.6f + h * 0.6f;
  ImGui::SameLine();
  const float middle = origin.x + (width - mirror_w - snap_w - proportional_w) * 0.5f;
  if (ImGui::GetCursorScreenPos().x < middle) {
    ImGui::SetCursorScreenPos(ImVec2(middle, ImGui::GetCursorScreenPos().y));
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
  if (header_toggle("X", ts.mirror_x, "Mirror X\nEdits mirror across the layer's vertical axis; a layer named for a side (Ear_L) mirrors into "
                    "its partner (Ear_R)")) {
    ts.mirror_x = !ts.mirror_x;
    C.undo->mark_changed(); /* Saved with the file. */
  }
  ImGui::SameLine(0.0f, 1.0f);
  if (header_toggle("Y", ts.mirror_y, "Mirror Y\nEdits mirror across the layer's horizontal axis")) {
    ts.mirror_y = !ts.mirror_y;
    C.undo->mark_changed();
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
  /* On the right, as in Blender: the overlays toggle and its dropdown, then Viewport Shading's. */
  const float shading_w = h * 0.3f + h + 1.0f + arrow_w;
  const float right = origin.x + width - (h + 1.0f + arrow_w + shading_w) - 6.0f;
  ImGui::SameLine(0.0f, h * 0.6f);
  if (ImGui::GetCursorScreenPos().x < right) {
    ImGui::SetCursorScreenPos(ImVec2(right, ImGui::GetCursorScreenPos().y));
  }
  overlays_buttons(C.document->overlays, ts, h, arrow_w);
  ImGui::SameLine(0.0f, h * 0.3f);
  shading_buttons(C, C.document->shading, h, arrow_w);
}

/* ---------------------------------------------------------------------------------
 * Modal transform (G, R, S, E)
 */

/* The points and handles a transform moves, gathered into `modal` (as modal_start does, without
 * starting the modal: the Transform Points gizmo uses it too). False when nothing can move; then the
 * document is as it was. */
static bool modal_collect(Context &C, Area &area, char kind, float2 mouse_document, ModalTransform &modal,
                          bool whole_snapshot = true);

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
  /* Extrude adds points as it starts. */
  std::set<uint64_t> moving;
  for (const ModalTransform::Item &item : g_modal.items) {
    moving.insert(item.layer);
  }
  for (const uint64_t id : moving) {
    if (Layer *layer = layer_find(*C.document, id)) {
      layer_touch(*layer);
    }
  }
  return true;
}

/* `whole_snapshot` off (an operator that moves the points at once, nothing to put back): the snapshot holds
 * only what X mirroring reads, the moving layers and their mirror partners as they were (nothing with it off).
 * Copying a big document whole takes tens of milliseconds. */
static bool modal_collect(Context &C, Area &area, char kind, float2 mouse_document, ModalTransform &modal,
                          bool whole_snapshot)
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
  if (whole_snapshot || kind == 'E') {
    modal.snapshot = *C.document;
  }
  else {
    modal.snapshot = Document();
    /* Only X mirroring reads it (modal_mirror_partners). */
    for (Layer *layer : C.wm->tool_settings.mirror_x ? layers : std::vector<Layer *>()) {
      modal.snapshot.layers.push_back(*layer);
      if (const Layer *partner = layer_mirror_partner(*C.document, layer->id)) {
        modal.snapshot.layers.push_back(*partner);
      }
    }
  }
  modal.area = area.id;
  modal.kind = kind == 'E' ? 'G' : kind;
  modal.extrude = kind == 'E';

  if (modal.extrude) {
    /* A new point beyond each selected curve end; they become the selection. */
    bool any = false;
    modal.extruded.clear();
    for (Layer *layer : layers) {
      std::vector<int> added = drawing_extrude_selected(layer->drawing, C.wm->tool_settings.style);
      any |= !added.empty();
      modal.extruded[layer->id] = std::move(added);
    }
    if (!any) {
      *C.document = modal.snapshot;
      report_banner(ReportType::Warning, "Select points to extrude");
      return false;
    }
  }

  /* Each layer's place and pose found by an index (this changes drawings, not which layers there are). */
  std::optional<LayerIndexScope> layer_index;
  layer_index.emplace(*C.document);
  int counted = 0;
  /* The selection's bounds (document space): S and R work about their middle. */
  float2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
  const auto bound = [&](float2 p) {
    lo = float2(std::min(lo.x, p.x), std::min(lo.y, p.y));
    hi = float2(std::max(hi.x, p.x), std::max(hi.y, p.y));
  };
  for (Layer *layer : layers) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    VectorDrawing &drawing = layer->drawing;
    const Posed shown = posed(*C.document, *layer);
    const ToolSettings &ts = C.wm->tool_settings;
    /* A sided layer (Ear_L) mirrors into its partner instead (modal_mirror). */
    const bool self_x = layer_mirror_x_self(*C.document, layer->id, ts.mirror_x);
    /* Both a point and its mirror image selected (both sides picked, or a duplicate of both): the
     * one on the -x side follows the other, reflected, as in Blender. */
    std::vector<bool> follows(drawing.points.size(), false);
    /* By point on the +x side: its selected image, which follows it. */
    std::map<int, int> follower;
    if (self_x && !modal.extrude) {
      std::vector<int> picked;
      for (int i = 0; i < int(drawing.points.size()); i++) {
        if (drawing.points[i].selected) {
          picked.push_back(i);
        }
      }
      for (const int i : picked) {
        const float2 here = point_setup_pos(drawing.points[i]);
        if (here.x <= 0.0f) {
          continue;
        }
        const float eps = 1e-4f + 1e-4f * (std::fabs(here.x) + std::fabs(here.y));
        for (const int j : picked) {
          if (j != i && !follows[j] && distance(point_setup_pos(drawing.points[j]), float2(-here.x, here.y)) <= eps) {
            follows[j] = true;
            follower[i] = j;
            break;
          }
        }
      }
    }
    for (int i = 0; i < int(drawing.points.size()); i++) {
      if (drawing.points[i].selected) {
        const float2 p = to_document.apply(shown.drawing->points[i].pos);
        bound(p);
        counted++;
        if (follows[i]) {
          continue;
        }
        ModalTransform::Item item{layer->id, i, p};
        item.unskin = shown.skins[i].inverted();
        modal.items.push_back(item);
      }
    }
    /* Symmetry: each moving point's mirror image (a point at the reflected spot in the setup pose,
     * across the layer's axes, so the pairs hold on every frame) moves with it, reflected. */
    if ((self_x || ts.mirror_y) && !modal.extrude) {
      const int moving_end = int(modal.items.size());
      for (int k = 0; k < moving_end; k++) {
        const ModalTransform::Item source = modal.items[k];
        if (source.layer != layer->id || source.curve >= 0) {
          continue;
        }
        /* A point with a selected image moves that one (and no unselected look-alike, such as the
         * original of a duplicate). */
        const auto f = follower.find(source.point);
        std::vector<MirrorPair> pairs =
            drawing_mirror_pairs(drawing, {source.point}, self_x && f == follower.end(), ts.mirror_y);
        if (f != follower.end()) {
          pairs.push_back({source.point, f->second, true, false});
        }
        for (const MirrorPair &m : pairs) {
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
    /* A point's place (document space), as proportional editing measures it. */
    const auto places_of = [&](int point) { return to_document.apply(shown.drawing->points[size_t(point)].pos); };
    const auto dragged_along = [&](int point) {
      return !reach.empty() && !drawing.points[point].selected && !drawing.points[point].detail.hidden &&
             std::isfinite(reach[point]);
    };
    /* Handles after the points, so they're placed once their points have moved. */
    const ShownHandleFactors factors = drawing_detail_shown_factors(*shown.drawing);
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
          /* By itself it moves from where it shows; with its point it comes along as it is stored. */
          const float2 h = to_document.apply(direct ? handle_end(*shown.drawing, factors, c, i, pre) :
                                                      curve_handle(*shown.drawing, shown.drawing->curves[c], i, pre));
          ModalTransform::Item item{layer->id, cp.point, h};
          /* A handle moved by itself, fitted: by the stretch at its end, which may be nothing like its point's (a point
           * in a squeezed cell, its handle reaching out of it). */
          item.unskin = direct && layer_handles_fitted(*C.document, *layer)
                            ? layer_place_unskin(*C.document, *layer, {cp.point, curve_handle(drawing, curve, i, pre)})
                            : shown.skins[cp.point].inverted();
          item.curve = c;
          item.index = i;
          item.pre = pre;
          item.direct = direct;
          item.shown_scale = direct ? factors.of(c, i, pre) : 1.0f;
          item.origin = to_document.apply(shown.drawing->points[cp.point].pos);
          item.stored_from = curve_handle(drawing, curve, i, pre) - drawing.points[cp.point].pos;
          if (along) {
            /* Its own reach, as Blender's proportional editing takes each Bezier handle as a point of its own:
             * a handle nearer the selection comes further than its point, so the curve bends into the falloff
             * instead of its handles riding along stiffly. Straight across, the nearest selected point; along
             * the curves (Connected Only), on from its own point or from the neighbour on its side. */
            item.proportional = true;
            item.rest = pre ? cp.handle_in : cp.handle_out;
            float own = std::numeric_limits<float>::infinity();
            if (!ts.proportional_connected) {
              for (int s = 0; s < int(drawing.points.size()); s++) {
                if (drawing.points[s].selected) {
                  own = std::min(own, distance(h, places_of(s)));
                }
              }
            }
            else {
              own = reach[cp.point] + distance(h, item.origin);
              const int m = int(curve.points.size());
              const int k = pre ? (i > 0 ? i - 1 : (curve.closed ? m - 1 : -1)) : (i + 1 < m ? i + 1 : (curve.closed ? 0 : -1));
              if (k >= 0 && k != i) {
                const int neighbour = curve.points[k].point;
                if (std::isfinite(reach[neighbour])) {
                  own = std::min(own, reach[neighbour] + distance(h, places_of(neighbour)));
                }
              }
            }
            item.reach = std::isfinite(own) ? own : reach[cp.point];
          }
          modal.items.push_back(item);
          if (direct) {
            bound(h);
            counted++;
          }
        }
      }
    }
  }
  layer_index.reset();
  if (counted == 0) {
    if (whole_snapshot) {
      *C.document = std::move(modal.snapshot);
    }
    return false;
  }
  /* S and R about the middle of the selection's bounds (as Mirror and the Transform Points box). */
  modal.pivot = (lo + hi) * 0.5f;
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
  modal.precise.reset(mouse_document);
  modal.last_mouse = mouse_document;
  return true;
}

static void modal_end(Context &C, bool confirm)
{
  if (confirm) {
    const char *name = g_modal.extrude ? "Extrude" : g_modal.tweak && !g_modal.tweak_points ? "Move Handle" : (g_modal.kind == 'R' ? "Rotate" : g_modal.kind == 'S' ? "Scale" : "Move");
    push_undo(C, name);
  }
  else {
    *C.document = std::move(g_modal.snapshot);
  }
  g_modal.active = false;
  g_modal.snapshot = Document();
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
}

/* Y while transforming: up and down only. Z too, as Blender's vertical axis in a 2D view. */
static bool modal_key_take_y(Context &C)
{
  const bool y = modal_key_take(C, "Y");
  const bool z = modal_key_take(C, "Z");
  return y || z;
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
  /* Every frame of a drag, for each moving layer: found by an index (this changes no layers). */
  const LayerIndexScope layer_index(*C.document);
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
    /* (A background run has no keys to read.) */
    const bool alone = ImGui::GetCurrentContext() && ImGui::GetIO().KeyAlt;
    const float2 here = drawing.points[cp.point].pos;
    const float2 moved = item.unskin.apply_vector(to_layer.apply_vector(p - item.start));
    curve_point_move_handle(drawing, curve, item.index, item.pre, here + item.stored_from + moved / item.shown_scale,
                            partner_moves || alone);
    if (alone && !partner_moves) {
      cp.aligned = false;
    }
  }
  else {
    /* With its point: changed by as much as it now shows differently from its point. */
    const float2 was = item.unskin.apply_vector(to_layer.apply_vector(item.start - item.origin));
    (item.pre ? cp.handle_in : cp.handle_out) = item.stored_from + (stored - drawing.points[cp.point].pos) - was;
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
/* X on: what moved in a sided layer (Ear_L) moves its partner's images (Ear_R), from the drawings as
 * the move began, so each update of a drag mirrors it afresh. */
static void modal_mirror_partners(Context &C, const ModalTransform &modal, ModalLayers &layers)
{
  if (!C.wm->tool_settings.mirror_x) {
    return;
  }
  PartnerMirror mirror;
  for (auto &[id, entry] : layers) {
    mirror.editing.push_back(id);
  }
  for (auto &[id, entry] : layers) {
    const Layer *was = layer_find(modal.snapshot, id);
    Layer *partner = layer_mirror_partner(*C.document, id);
    if (!was || !partner) {
      continue;
    }
    if (!layers.count(partner->id)) {
      if (const Layer *partner_was = layer_find(modal.snapshot, partner->id)) {
        partner->drawing = partner_was->drawing;
      }
    }
    mirror.before.push_back({id, was->drawing});
  }
  layer_mirror_partners_end(*C.document, mirror);
}

/* The layers a point move changed this update, and their mirror partners (see Revision). */
static void modal_touch(Context &C, ModalLayers &layers)
{
  for (auto &[id, entry] : layers) {
    layer_touch(*entry.first);
    if (C.wm->tool_settings.mirror_x) {
      if (Layer *partner = layer_mirror_partner(*C.document, id)) {
        layer_touch(*partner);
      }
    }
  }
}

static void modal_mirror(Context &C, const ModalTransform &modal, ModalLayers &layers)
{
  for (const ModalTransform::Item &item : modal.items) {
    if (item.mirror_source < 0) {
      continue;
    }
    const ModalTransform::Item &source = modal.items[item.mirror_source];
    drawing_apply_mirror(layers[item.layer].first->drawing, {{source.point, item.point, item.mirror_x, item.mirror_y}});
  }
  const ToolSettings &ts = C.wm->tool_settings;
  if (ts.mirror_x || ts.mirror_y) {
    for (const ModalTransform::Item &item : modal.items) {
      if (item.curve < 0 || !item.direct) {
        continue;
      }
      VectorDrawing &drawing = layers[item.layer].first->drawing;
      const bool self_x = layer_mirror_x_self(*C.document, item.layer, ts.mirror_x);
      for (const auto &[x, y] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
        if ((x && !self_x) || (y && !ts.mirror_y)) {
          continue;
        }
        const int dst = drawing_find_mirror_point(drawing, item.point, x, y);
        if (dst >= 0) {
          drawing_mirror_handles(drawing, item.point, dst, x, y);
        }
      }
    }
  }
  modal_mirror_partners(C, modal, layers);
}

/* Runs the transform for this frame; returns a line describing it for the header. */
/* Moves what `modal` gathered: by `delta` (G), turned `angle` radians (R) or scaled by `scale` (S)
 * about its pivot (each point's own with Individual Origins), proportional editing blending it, then
 * its mirror images. The drag does this every update, and Move, Rotate and Scale again from where it
 * began, so the ViewportActionPanel can change the amounts. */
/* Slide: the moving layers' drawings as they were when the move started. */
static void modal_slide_restore(Context &C, ModalLayers &layers)
{
  (void)C;
  for (auto &[id, entry] : layers) {
    if (const Layer *was = layer_find(g_modal.snapshot, id)) {
      entry.first->drawing = was->drawing;
    }
  }
}

static void modal_slide_apply(Context &C, ModalLayers &layers, float factor)
{
  modal_slide_restore(C, layers);
  for (auto &[id, entry] : layers) {
    VectorDrawing &drawing = entry.first->drawing;
    drawing_slide_points(drawing, drawing_selected_points(drawing), factor);
  }
}

/* How far the mouse has slid the first moving point: toward whichever neighbour on its curve the
 * mouse heads for, as the fraction of the way there (negative: toward the point before). */
static float modal_slide_factor(const ModalTransform &modal, ModalLayers &layers, float2 delta)
{
  for (const ModalTransform::Item &item : modal.items) {
    if (item.curve >= 0 || item.proportional || item.mirror_source >= 0) {
      continue;
    }
    const Layer *was = layer_find(modal.snapshot, item.layer);
    if (!was || !layers.count(item.layer)) {
      continue;
    }
    const Transform2D &to_layer = layers[item.layer].second;
    const float2 d = item.unskin.apply(to_layer.apply(item.start + delta)) - item.unskin.apply(to_layer.apply(item.start));
    const VectorDrawing &drawing = was->drawing;
    for (const Curve &curve : drawing.curves) {
      const int n = int(curve.points.size());
      for (int i = 0; i < n; i++) {
        if (curve.points[i].point != item.point || n < 2) {
          continue;
        }
        const float2 p = drawing.points[item.point].pos;
        const auto toward = [&](int j) {
          const float2 e = drawing.points[curve.points[j].point].pos - p;
          const float len2 = dot(e, e);
          return len2 > 1e-12f ? dot(d, e) / len2 : 0.0f;
        };
        const bool has_next = curve.closed || i + 1 < n, has_prev = curve.closed || i > 0;
        const float next = has_next ? toward((i + 1) % n) : -1e9f;
        const float prev = has_prev ? toward((i + n - 1) % n) : -1e9f;
        if (next >= prev) {
          return next > 0.0f ? next : 0.0f;
        }
        return prev > 0.0f ? -prev : 0.0f;
      }
    }
  }
  return 0.0f;
}

static void modal_apply(Context &C, const ModalTransform &modal, ModalLayers &layers, char kind, float2 delta,
                        float angle, float2 scale)
{
  const ToolSettings &ts = C.wm->tool_settings;
  /* The points, each to where it goes, and the handles moved by themselves, for the layers a deform bends. */
  std::map<uint64_t, std::vector<PointTarget>> targets;
  std::map<uint64_t, std::vector<HandleTarget>> handles;
  for (const ModalTransform::Item &item : modal.items) {
    const float w = modal_weight(ts, item);
    if (w <= 0.0f) {
      modal_rest(layers, item);
      continue;
    }
    const float2 c = modal.about_points ? item.origin : modal.pivot;
    float2 to;
    if (kind == 'G') {
      to = item.start + delta * w;
    }
    else if (kind == 'R') {
      to = c + rotate(item.start - c, angle * w);
    }
    else {
      const float2 d = item.start - c;
      to = c + float2(d.x * (1.0f + (scale.x - 1.0f) * w), d.y * (1.0f + (scale.y - 1.0f) * w));
    }
    modal_place(layers, item, to);
    if (item.curve < 0 && item.mirror_source < 0) {
      const Transform2D &to_layer = layers[item.layer].second;
      targets[item.layer].push_back({item.point, to_layer.apply(to), item.unskin,
                                     length(item.unskin.apply_vector(to_layer.apply_vector(to - item.start)))});
    }
    else if (item.direct && item.shown_scale == 1.0f && item.mirror_source < 0) {
      const Curve &curve = layers[item.layer].first->drawing.curves[size_t(item.curve)];
      const CurvePoint &cp = curve.points[size_t(item.index)];
      const Transform2D &to_layer = layers[item.layer].second;
      handles[item.layer].push_back({item.curve, item.index, item.pre, item.pre ? cp.select_out : cp.select_in,
                                     to_layer.apply(to), item.unskin,
                                     length(item.unskin.apply_vector(to_layer.apply_vector(to - item.start)))});
    }
  }
  for (const auto &[id, points] : targets) {
    layer_points_settle(*C.document, *layers[id].first, points);
  }
  for (const auto &[id, ends] : handles) {
    layer_handles_settle(*C.document, *layers[id].first, ends);
  }
  modal_mirror(C, modal, layers);
}

static std::string num_arg(float v)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", double(v));
  return buf;
}

/* A confirmed G, R or S on points: the document as it was, then Move, Rotate or Scale with the
 * amounts the drag ended on, as one undo step the ViewportActionPanel can adjust. An extrusion or a
 * handle tweak stays the drag's own step. */
/* A ViewportActionPanel checkbox kept for the next moves until the frame or the action being edited
 * changes, then off again: Keep Poses, Move Only Bones, Move Without Children. */
struct StickyOption {
  bool on = false;
  uint64_t frame_changes = 0;
  std::string action;
  uint64_t action_layer = 0;
};

static bool sticky_on(StickyOption &o, const Document &doc)
{
  if (o.on && (animation_frame_changes() != o.frame_changes || doc.action != o.action ||
               doc.action_layer != o.action_layer))
  {
    o.on = false;
  }
  return o.on;
}

static void sticky_set(StickyOption &o, const Document &doc, bool on)
{
  o = {on, animation_frame_changes(), doc.action, doc.action_layer};
}

/* Keep Poses: moves on the setup frame leave the other frames' keyed poses where they are. */
static StickyOption g_keep_poses;
/* Move Only Bones (posing): the drawings the bones bend stay where they are (see BoneCounter). */
static StickyOption g_move_only_bones;
/* Move Without Children (G, R and S on bones): unpicked children stay where they are. */
static StickyOption g_move_without_children;

static bool keep_poses_on(const Document &doc)
{
  return sticky_on(g_keep_poses, doc);
}

static bool move_only_bones_on(const Document &doc)
{
  return sticky_on(g_move_only_bones, doc);
}

static bool move_without_children_on(const Document &doc)
{
  return sticky_on(g_move_without_children, doc);
}

MoveOptions viewport_move_options(const Document &doc)
{
  return {keep_poses_on(doc), move_only_bones_on(doc), move_without_children_on(doc)};
}

static void modal_confirm(Context &C, float2 delta, float angle, float2 scale)
{
  if (g_modal.extrude || g_modal.tweak) {
    modal_end(C, true);
    return;
  }
  const char kind = g_modal.kind;
  *C.document = std::move(g_modal.snapshot);
  g_modal.active = false;
  g_modal.snapshot = Document();
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
  OpArgs args{{"setup", C.document->frame_current == 0 ? "1" : "0"}, {"keep_poses", keep_poses_on(*C.document) ? "1" : "0"}};
  if (g_modal.slide) {
    args["factor"] = num_arg(g_modal.slide_factor);
    operator_call(C, "mesh.slide", args);
  }
  else if (kind == 'G') {
    args["move_x"] = num_arg(delta.x);
    args["move_y"] = num_arg(delta.y);
    operator_call(C, "mesh.translate", args);
  }
  else if (kind == 'R') {
    args["angle"] = num_arg(angle * 180.0f / PI);
    operator_call(C, "mesh.rotate", args);
  }
  else {
    args["scale_x"] = num_arg(scale.x);
    args["scale_y"] = num_arg(scale.y);
    operator_call(C, "mesh.resize", args);
  }
}

static std::string modal_update(Context &C, const ViewMapping &view)
{
  ImGuiIO &io = ImGui::GetIO();
  ModalLayers layers;
  if (!modal_layers(C, g_modal, layers)) {
    modal_end(C, false);
    return {};
  }
  ToolSettings &ts = C.wm->tool_settings;
  /* Page Up and Page Down size proportional editing's reach (the wheel does too). */
  if (g_modal.proportional) {
    while (modal_key_take(C, "PAGE_UP", true)) {
      ts.proportional_size = std::min(ts.proportional_size * 1.1f, 1e6f);
    }
    while (modal_key_take(C, "PAGE_DOWN", true)) {
      ts.proportional_size = std::max(ts.proportional_size / 1.1f, 1e-4f);
    }
  }

  /* Keys while transforming, as in Blender. G again slides the points along their curves (and
   * back). */
  if (g_modal.kind == 'G' && !g_modal.extrude && !g_modal.tweak && !g_modal.about_points && modal_key_take(C, "G")) {
    g_modal.slide = !g_modal.slide;
    g_modal.num = NumInput();
    g_modal.axis = 0;
    modal_slide_restore(C, layers);
  }
  if (!g_modal.slide && modal_key_take(C, "X")) {
    g_modal.axis = g_modal.axis == 1 ? 0 : 1;
  }
  if (!g_modal.slide && modal_key_take_y(C)) {
    g_modal.axis = g_modal.axis == 2 ? 0 : 2;
  }
  num_input_keys(C, g_modal.num, g_modal.kind == 'R' || g_modal.axis != 0 || g_modal.slide ? 1 : 2);
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
  const float2 mouse = g_modal.precise.update(view.to_document(io.MousePos), io.KeyShift);
  const bool snap = io.KeyCtrl || C.wm->tool_settings.snap_grid;
  const NumInput &num = g_modal.num;
  if (g_modal.axis != 0) {
    /* Along one axis there's one field. */
    g_modal.num.field = 0;
  }
  const char *axis_label = g_modal.axis == 1 ? "  along X" : (g_modal.axis == 2 ? "  along Y" : "");

  std::string info;
  char text[128];
  float2 delta, scale(1.0f, 1.0f);
  float angle = 0.0f;
  if (g_modal.slide) {
    float factor = modal_slide_factor(g_modal, layers, mouse - g_modal.start_mouse);
    if (snap) {
      factor = std::round(factor * 10.0f) / 10.0f;
    }
    if (num.any()) {
      factor = num.value(0, 0.0f);
    }
    factor = std::clamp(factor, -1.0f, 1.0f);
    g_modal.slide_factor = factor;
    modal_slide_apply(C, layers, factor);
    modal_mirror_partners(C, g_modal, layers);
    std::snprintf(text, sizeof(text), "Slide  %s", num.shown(0, factor, "%.3f").c_str());
  }
  else if (g_modal.kind == 'G') {
    delta = mouse - g_modal.start_mouse;
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
    angle = degrees * PI / 180.0f;
    std::snprintf(text, sizeof(text), "Rotate  %s\xC2\xB0", num.shown(0, degrees, "%.1f").c_str());
  }
  else {
    const float start = std::max(distance(g_modal.start_mouse, g_modal.pivot), 1e-6f);
    float factor = distance(mouse, g_modal.pivot) / start;
    if (snap) {
      factor = std::round(factor * 10.0f) / 10.0f;
    }
    scale = float2(g_modal.axis == 2 ? 1.0f : factor, g_modal.axis == 1 ? 1.0f : factor);
    if (num.any()) {
      /* Typed: one number scales every way (or along the axis); with Y typed too, each its own. */
      const float x = num.value(0, num.has(1) ? 1.0f : 1.0f);
      factor = x;
      scale = g_modal.axis == 1 ? float2(x, 1.0f)
              : g_modal.axis == 2 ? float2(1.0f, x)
              : num.has(1) || num.negative[1] ? float2(num.has(0) ? x : 1.0f, num.value(1, 1.0f))
                                              : float2(x, x);
    }
    if (g_modal.axis == 0 && (num.field == 1 || num.has(1))) {
      std::snprintf(text, sizeof(text), "Scale  X: %s  Y: %s", num.shown(0, scale.x, "%.3f").c_str(),
                    num.shown(1, scale.y, "%.3f").c_str());
    }
    else {
      std::snprintf(text, sizeof(text), "Scale  %s%s", num.shown(0, factor, "%.3f").c_str(), axis_label);
    }
  }
  if (!g_modal.slide) {
    modal_apply(C, g_modal, layers, g_modal.kind, delta, angle, scale);
    for (const auto &[id, added] : g_modal.extruded) {
      if (Layer *layer = g_modal.extrude ? layer_find(*C.document, id) : nullptr) {
        for (const int point : added) {
          drawing_extrude_align(layer->drawing, point);
        }
      }
    }
  }
  modal_touch(C, layers);
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
    modal_confirm(C, delta, angle, scale);
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
  const bool boxlike = kind == PrimitiveKind::Box || kind == PrimitiveKind::Circle || kind == PrimitiveKind::Grid;
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

/* The Shape tool's drag ends as mesh.primitive_box_add, _circle_add or _grid_add, so the ViewportActionPanel
 * can change it (its place, size, cells or fill) until anything else is done, as Blender's Add Mesh does. */
static void shape_commit(Context &C, VectorDrawing &drawing, PrimitiveKind kind, const std::vector<float2> &points)
{
  (void)drawing;
  if (points.size() < 2) {
    return;
  }
  const ToolSettings &ts = C.wm->tool_settings;
  const float2 a = points.front(), b = points.back();
  OpArgs args = {{"x", num_arg((a.x + b.x) * 0.5f)},
                 {"y", num_arg((a.y + b.y) * 0.5f)},
                 {"width", num_arg(std::abs(b.x - a.x))},
                 {"height", num_arg(std::abs(b.y - a.y))}};
  if (kind == PrimitiveKind::Grid) {
    args["columns"] = std::to_string(ts.grid_columns);
    args["rows"] = std::to_string(ts.grid_rows);
  }
  else {
    args["fill"] = ts.draw_fill ? "1" : "0";
  }
  operator_call(C, kind == PrimitiveKind::Grid     ? "mesh.primitive_grid_add"
                   : kind == PrimitiveKind::Circle ? "mesh.primitive_circle_add"
                                                   : "mesh.primitive_box_add",
                args);
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
      candidate.point = drawing_pick_point(drawing, mouse, pick_line_pixels() * pixel);
      if (candidate.point >= 0) {
        d = distance(drawing.points[candidate.point].pos, mouse) / pixel;
      }
    }
    else if (drawing_pick_segment(drawing, mouse, pick_line_pixels() * pixel, &candidate.edge, &candidate.t)) {
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
/* A click on points where the last one was (the mouse within a couple of pixels): the next point under
 * it, round to the nearest again, so points on top of each other can each be picked. */
static struct {
  ImVec2 pos{-1e9f, -1e9f};
  uint64_t layer = 0, point = 0;
} g_point_cycle;

static void select_click(Context &C, const ViewMapping &view, float2 mouse_document, HandleDisplay handles)
{
  /* Shift adds, Ctrl takes away (none in a background run, which has no ImGui: the Motion Path tool's test hook). */
  const bool has_io = ImGui::GetCurrentContext() != nullptr;
  const bool shift = has_io && ImGui::GetIO().KeyShift, ctrl = has_io && ImGui::GetIO().KeyCtrl;
  const std::vector<Layer *> layers = editable_layers(C);
  Layer *hit_layer = nullptr;
  std::vector<int> picked;
  ShapeEdge hit_edge{-1, 0};
  float best = FLT_MAX;
  const ImVec2 mouse_screen = view.to_screen(mouse_document);
  const HandleHit handle = pick_handle(C, view, mouse_screen, handles);
  Layer &camera = document_camera(*C.document);
  /* Every shown point within reach, nearest first. */
  struct PointHit {
    Layer *layer;
    int point;
    float d;
  };
  std::vector<PointHit> hits;
  for (Layer *layer : layers) {
    if (C.wm->tool_settings.select_mode == SelectMode::Edge) {
      /* Points don't show in Edges mode, so clicks pick lines (below). */
      continue;
    }
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
    const float2 mouse = to_document.inverted().apply(mouse_document);
    const float reach = pick_pixels(C.document->overlays.point_size);
    const Posed shown = posed(*C.document, *layer);
    drawing_ensure_ids(layer->drawing);
    for (int p = 0; p < int(shown.drawing->points.size()); p++) {
      if (shown.drawing->points[size_t(p)].detail.hidden) {
        continue;
      }
      const float d = distance(shown.drawing->points[size_t(p)].pos, mouse) / pixel;
      if (d <= reach) {
        hits.push_back({layer, p, d});
      }
    }
  }
  /* Nearest first; a picked point counts a pixel nearer, so of two about as near the picked one is taken. */
  const auto how_near = [](const PointHit &h) {
    return h.d - (h.layer->drawing.points[size_t(h.point)].selected ? 1.0f : 0.0f);
  };
  std::stable_sort(hits.begin(), hits.end(), [&](const PointHit &a, const PointHit &b) { return how_near(a) < how_near(b); });
  size_t choice = 0;
  bool cycled = false;
  const ImVec2 moved = mouse_screen - g_point_cycle.pos;
  if (!hits.empty() && moved.x * moved.x + moved.y * moved.y <= 16.0f) {
    for (size_t k = 0; k < hits.size(); k++) {
      if (hits[k].layer->id == g_point_cycle.layer &&
          hits[k].layer->drawing.points[size_t(hits[k].point)].id == g_point_cycle.point)
      {
        choice = (k + 1) % hits.size();
        cycled = true;
        break;
      }
    }
  }
  if (!hits.empty()) {
    best = cycled ? 0.0f : hits[choice].d;
    hit_layer = hits[choice].layer;
    picked = {hits[choice].point};
    g_point_cycle = {mouse_screen, hit_layer->id, hit_layer->drawing.points[size_t(picked.front())].id};
  }
  else {
    g_point_cycle = {};
  }
  /* A handle nearer than any point takes the click: just that handle is selected (not while clicking
   * again goes through the points there). */
  if (handle.layer && handle.distance <= best) {
    if (!shift && !ctrl) {
      for (Layer *layer : layers) {
        deselect_all(layer->drawing);
      }
    }
    CurvePoint &cp = handle.layer->drawing.curves[handle.curve].points[handle.index];
    bool &flag = handle.pre ? cp.select_in : cp.select_out;
    flag = ctrl ? false : (shift ? !flag : true);
    camera.selected &= shift || ctrl;
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
      const VectorDrawing *wires = posed_wires(shown);
      const VectorDrawing lines = wires ? *wires : drawing_detail_shown_curves(*shown.drawing);
      if (drawing_pick_segment(lines, to_document.inverted().apply(mouse_document), pick_line_pixels() * pixel, &edge, &t)) {
        if (wires) {
          /* A piece of a segment Resample split: the segment, by the drawing's own points before it. */
          int own = -1;
          for (int i = 0; i <= edge.segment; i++) {
            own += wires->points[wires->curves[edge.curve].points[i].point].made_by < 0 ? 1 : 0;
          }
          edge.segment = std::max(own, 0);
        }
        int a, b;
        curve_segment_points((wires ? *shown.drawing : lines).curves[edge.curve], edge.segment, &a, &b);
        hit_layer = layer;
        picked = C.wm->tool_settings.select_mode == SelectMode::Edge ? std::vector<int>{a, b} : std::vector<int>{a};
        hit_edge = edge;
        break;
      }
    }
  }
  if (!shift && !ctrl) {
    for (Layer *layer : layers) {
      deselect_all(layer->drawing);
    }
  }
  if (hit_layer) {
    for (int p : picked) {
      MeshPoint &point = hit_layer->drawing.points[p];
      point.selected = ctrl ? false : (shift && picked.size() == 1 ? !point.selected : true);
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
      camera_border_distance(*C.document, view, view.to_screen(mouse_document)) <= pick_line_pixels())
  {
    camera.selected = ctrl ? false : (shift ? !camera.selected : true);
  }
  else if (!shift && !ctrl) {
    camera.selected = false;
  }
}

/* What pressing at `mouse` grabs to drag: the nearest shown point within reach (as a click picks it), or in Edges mode
 * the segment under it, its two points. Shift and Ctrl presses grab nothing: they box-select. */
static PressGrab select_press_grab(Context &C, const ViewMapping &view, ImVec2 mouse, bool shift_or_ctrl)
{
  PressGrab grab;
  if (shift_or_ctrl) {
    return grab;
  }
  grab.threshold = tablet_state().touching ? 10.0f : 3.0f;
  const float2 mouse_document = view.to_document(mouse);
  const bool edges = C.wm->tool_settings.select_mode == SelectMode::Edge;
  float best = FLT_MAX;
  for (Layer *layer : editable_layers(C)) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
    const float2 at = to_document.inverted().apply(mouse_document);
    const Posed shown = posed(*C.document, *layer);
    if (!edges) {
      const float reach = pick_pixels(C.document->overlays.point_size);
      for (int p = 0; p < int(shown.drawing->points.size()); p++) {
        if (shown.drawing->points[size_t(p)].detail.hidden) {
          continue;
        }
        const float d = distance(shown.drawing->points[size_t(p)].pos, at) / pixel;
        if (d <= reach && d < best) {
          best = d;
          grab.layer = layer->id;
          grab.points = {p};
          grab.distance = d;
        }
      }
      continue;
    }
    /* The segment under it, where its line shows (as a click in Edges mode picks it). */
    const VectorDrawing *wires = posed_wires(shown);
    const VectorDrawing lines = wires ? *wires : drawing_detail_shown_curves(*shown.drawing);
    ShapeEdge edge;
    float t;
    if (drawing_pick_segment(lines, at, pick_line_pixels() * pixel, &edge, &t)) {
      if (wires) {
        int own = -1;
        for (int i = 0; i <= edge.segment; i++) {
          own += wires->points[wires->curves[edge.curve].points[i].point].made_by < 0 ? 1 : 0;
        }
        edge.segment = std::max(own, 0);
      }
      int a, b;
      curve_segment_points((wires ? *shown.drawing : lines).curves[edge.curve], edge.segment, &a, &b);
      grab.layer = layer->id;
      grab.points = {a, b};
      grab.edge = edge;
      break;
    }
  }
  return grab;
}

/* A grab being dragged: what it holds becomes the selection unless it is all picked already (then the whole selection
 * moves with it). Its layer, or null when it has gone. */
static Layer *select_press_grab_pick(Context &C, const PressGrab &grab)
{
  Layer *layer = layer_find(*C.document, grab.layer);
  if (!layer) {
    return nullptr;
  }
  VectorDrawing &drawing = layer->drawing;
  bool picked = true;
  for (int p : grab.points) {
    picked = picked && p < int(drawing.points.size()) && drawing.points[size_t(p)].selected;
  }
  if (!picked) {
    for (Layer *editing : editable_layers(C)) {
      deselect_all(editing->drawing);
    }
    for (int p : grab.points) {
      if (p < int(drawing.points.size())) {
        drawing.points[size_t(p)].selected = true;
      }
    }
    drawing_expand_selection_by_mode(C, drawing);
    document_set_active_shape(*C.document, *layer, grab.points.front(), grab.edge);
    document_camera(*C.document).selected = false;
  }
  return layer;
}

/* Dragging a grab past its threshold: picked as above, then moving as G moves it until the mouse comes up. */
static bool select_press_grab_move(Context &C, Area &area, const ViewMapping &view, const PressGrab &grab, ImVec2 start)
{
  if (!select_press_grab_pick(C, grab)) {
    return false;
  }
  if (!modal_start(C, area, 'G', view.to_document(start))) {
    return false;
  }
  g_modal.tweak = true;
  g_modal.tweak_points = true;
  return true;
}

bool motion_path_vector_under_test(Context &C, float2 at, float zoom)
{
  reach_update(C);
  const ViewMapping view{{0.0f, 0.0f}, {1000.0f, 1000.0f}, {0.0f, 0.0f}, zoom};
  return vector_under(C, view, view.to_screen(at));
}

bool motion_path_pick_test(Context &C, float2 at, float zoom, bool extend, bool apply)
{
  reach_update(C);
  const ViewMapping view{{0.0f, 0.0f}, {1000.0f, 1000.0f}, {0.0f, 0.0f}, zoom};
  return motion_path_pick_click(C, view, view.to_screen(at), extend, apply);
}

std::vector<std::pair<uint64_t, std::vector<int>>> select_press_grab_test(Context &C, float2 at, float zoom,
                                                                          bool shift_or_ctrl, bool drag)
{
  reach_update(C);
  const ViewMapping view{{0.0f, 0.0f}, {1000.0f, 1000.0f}, {0.0f, 0.0f}, zoom};
  const PressGrab grab = select_press_grab(C, view, view.to_screen(at), shift_or_ctrl);
  std::vector<std::pair<uint64_t, std::vector<int>>> out;
  if (!grab.layer) {
    return out;
  }
  if (!drag) {
    out.push_back({grab.layer, grab.points});
    return out;
  }
  select_press_grab_pick(C, grab);
  for (Layer *layer : editable_layers(C)) {
    std::vector<int> picked;
    for (int p = 0; p < int(layer->drawing.points.size()); p++) {
      if (layer->drawing.points[size_t(p)].selected) {
        picked.push_back(p);
      }
    }
    if (!picked.empty()) {
      out.push_back({layer->id, picked});
    }
  }
  return out;
}

static void select_box(Context &C, const ViewMapping &view, ImVec2 a, ImVec2 b, HandleDisplay handles)
{
  const ImGuiIO &io = ImGui::GetIO();
  const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
  /* Each layer's place and pose found by an index (selecting changes no layers). */
  const LayerIndexScope layer_index(*C.document);
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
    const ShownHandleFactors factors = drawing_detail_shown_factors(*shown.drawing);
    std::vector<std::pair<CurvePoint *, int>> boxed;
    for (size_t c = 0; c < drawing.curves.size(); c++) {
      Curve &curve = drawing.curves[c];
      for (int i = 0; i < int(curve.points.size()); i++) {
        CurvePoint &cp = curve.points[i];
        if (handle_visible(drawing, cp, handles)) {
          const float2 here = shown.drawing->points[cp.point].pos;
          for (const bool pre : {true, false}) {
            const float2 h = handle_end(*shown.drawing, factors, int(c), i, pre);
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

/* The pen's touch a press is from, if it is the pen's: the one under way, or one that began and ended
 * in the moment before ImGui passed the press on (a quick tap); 0 for the mouse. A touch is followed
 * once. */
static uint64_t pen_touch_pressed()
{
  static uint64_t last_used = 0;
  const uint64_t id = pen_stroke_id();
  if (id == 0 || id == last_used || pen_stroke().empty()) {
    return 0;
  }
  const double now = pen_now();
  const bool recent = tablet_state().touching ? now - pen_stroke_began() < 0.5 : now - pen_stroke_ended() < 0.25;
  if (!recent) {
    return 0;
  }
  last_used = id;
  return id;
}

/* Adds the pen's samples that came in since the last frame to the stroke, every one (a tablet sends far
 * more than a frame's mouse position shows), at the sub-pixel place the tablet gives. */
static void draw_take_pen(const ViewMapping &view, const Transform2D &to_layer)
{
  if (g_drag.pen_stroke == 0 || pen_stroke_id() != g_drag.pen_stroke) {
    return;
  }
  const std::vector<PenSample> &samples = pen_stroke();
  for (; g_drag.pen_taken < samples.size(); g_drag.pen_taken++) {
    const PenSample &p = samples[g_drag.pen_taken];
    g_drag.ink.add({to_layer.apply(view.to_document(ImVec2(p.x, p.y))), p.pressure, {p.tilt_x, p.tilt_y}, p.time});
  }
}

/* The stroke as it stands, to `pen` (layer space): the same line the stroke will end as, at its real width
 * and through the real stroke tessellator, so lifting the pen changes nothing. The soft edge is a pixel,
 * as the renderer's. */
static void draw_tool_preview(ImDrawList *dl, const ViewMapping &view, const Transform2D &to_document, Color color,
                              float2 pen)
{
  std::vector<InkPoint> line = g_drag.ink.points();
  const std::vector<InkPoint> tail = g_drag.ink.tail(pen);
  line.insert(line.end(), tail.begin(), tail.end());
  ink_taper(line, g_drag.ink.settings(), g_drag.ink.view());
  const float half = g_drag.ink.view().half_width * view.zoom * to_document.scale();
  std::vector<float2> points;
  std::vector<float> widths;
  for (const InkPoint &p : line) {
    const ImVec2 at = view.to_screen(to_document.apply(p.pos));
    points.push_back({at.x, at.y});
    widths.push_back(std::max(p.radius * half, 0.0f));
  }
  if (points.size() >= 2) {
    ink_line_draw(dl, points, widths, color);
  }
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

/* Hashing for the overlays' fingerprint. */
static uint64_t overlay_mix(uint64_t h, uint64_t v)
{
  return (h ^ v) * 1099511628211ull;
}
static uint64_t overlay_mix_float(uint64_t h, float f)
{
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return overlay_mix(h, bits);
}

/* Everything a drawing's overlays are made from. With `unit`, points and handles to that size, the points
 * relative to the first (the same for the drawing moved whole); without, exactly. */
static uint64_t overlay_fingerprint(const VectorDrawing &d, float unit)
{
  uint64_t h = overlay_mix(1469598103934665603ull, d.points.size());
  const float2 origin = unit > 0.0f && !d.points.empty() ? d.points[0].pos : float2();
  const auto place = [&](float v, float o) {
    h = unit > 0.0f ? overlay_mix(h, uint64_t(std::llround((v - o) / unit))) : overlay_mix_float(h, v);
  };
  for (const MeshPoint &p : d.points) {
    place(p.pos.x, origin.x);
    place(p.pos.y, origin.y);
    h = overlay_mix(h, uint64_t(p.selected) | uint64_t(p.detail.hidden) << 1 | uint64_t(uint8_t(p.detail.level)) << 2);
  }
  for (const Curve &c : d.curves) {
    h = overlay_mix(h, c.points.size() << 1 | (c.closed ? 1 : 0));
    for (const CurvePoint &cp : c.points) {
      h = overlay_mix(h, uint64_t(cp.point));
      place(cp.handle_in.x, 0.0f);
      place(cp.handle_in.y, 0.0f);
      place(cp.handle_out.x, 0.0f);
      place(cp.handle_out.y, 0.0f);
      for (const float v : {cp.curvature, cp.weight_in, cp.weight_out, cp.offset_in, cp.offset_out}) {
        h = overlay_mix_float(h, v);
      }
      h = overlay_mix(h, uint64_t(cp.edge_hidden) | uint64_t(cp.handles_set) << 1 | uint64_t(cp.aligned) << 2 |
                             uint64_t(cp.select_in) << 3 | uint64_t(cp.select_out) << 4);
    }
  }
  for (const Shape &shape : d.shapes) {
    h = overlay_mix(h, shape.edges.size() << 8 | uint64_t(uint8_t(shape.detail_level)));
    for (const ShapeEdge &e : shape.edges) {
      h = overlay_mix(h, uint64_t(uint32_t(e.curve)) << 32 | uint32_t(e.segment));
    }
  }
  return h;
}

/* What is picked and shown in a drawing (points, handles, detail hidden): looked at every frame, as a pick can change
 * without the layer's revision (an operator that records no undo step), and Shape handles show by what is picked. */
static uint64_t overlay_selection_hash(const VectorDrawing &d)
{
  uint64_t h = overlay_mix(1469598103934665603ull, d.points.size());
  uint64_t bits = 0;
  int n = 0;
  const auto add = [&](bool b) {
    bits = bits << 1 | uint64_t(b);
    if (++n == 64) {
      h = overlay_mix(h, bits);
      bits = 0;
      n = 0;
    }
  };
  for (const MeshPoint &p : d.points) {
    add(p.selected);
    add(p.detail.hidden);
  }
  for (const Curve &c : d.curves) {
    for (const CurvePoint &cp : c.points) {
      add(cp.select_in);
      add(cp.select_out);
    }
  }
  return overlay_mix(h, bits ^ uint64_t(n) << 56);
}

/* The layer's own drawing as the overlays key it: its fingerprint (kept while its revision is) and what is picked. */
static uint64_t overlay_drawing_identity(uint64_t kept_fingerprint, const VectorDrawing &drawing)
{
  return overlay_mix(kept_fingerprint, overlay_selection_hash(drawing));
}

uint64_t overlay_drawing_identity_test(const Layer &layer)
{
  /* As draw_drawing_overlay keys the layer's own drawing: its kept fingerprint, and what is picked. */
  static uint64_t kept_revision = ~uint64_t(0), kept = 0;
  if (kept_revision != layer.revision.value) {
    kept_revision = layer.revision.value;
    kept = overlay_fingerprint(layer.drawing, 1.0f / 16.0f);
  }
  return overlay_drawing_identity(kept, layer.drawing);
}

/* The wires, handles and points of a drawing being edited (layer space): what the GPU overlays draw. */
static OverlayGeometry overlay_geometry(const VectorDrawing &drawing, const VectorDrawing *wires, const Transform2D &to_document,
                                        bool show_wires, bool show_points, int quality, HandleDisplay handles, bool point_squares,
                                        const LineCutter *cutter, float point_size, float handle_size, float pixel)
{
  OverlayGeometry g;
  /* Wires show each curve as it is edited: at its shape's detail level, through the points shown
   * (data/detail.hh); the stroke under them draws all of its detail. */
  VectorDrawing shown_curves;
  const VectorDrawing *lines = wires ? wires : &drawing;
  if (!wires && drawing_has_detail(drawing)) {
    shown_curves = drawing_detail_shown_curves(drawing);
    lines = &shown_curves;
  }
  const bool cuts = cutter && !cutter->empty();
  std::vector<float2> line;
  std::vector<char> shows;
  /* The colours that read on what is drawn under them (a layer-space place looked up in the canvas). */
  const ContrastColors wire = wire_colors();
  const CanvasSample *under = g_canvas_now;
  const std::function<bool(float2, Color &)> under_at = [&](float2 p, Color &c) {
    return under && under->at(to_document.apply(p), c);
  };
  std::vector<ContrastPiece> pieces;
  for (const Curve &curve : lines->curves) {
    if (!show_wires) {
      break;
    }
    for (int s = 0; s < curve.segment_count(); s++) {
      float2 ctrl[4];
      curve_segment_bezier(*lines, curve, s, ctrl);
      /* As many pieces as look smooth at this zoom (a quarter pixel off at most); dashed ones (hidden, cut
       * away) keep the viewport quality's pieces for their dashes. */
      const bool dashed = curve.points[s].edge_hidden || cuts;
      const int steps = bezier_steps(ctrl, dashed ? quality : 1, pixel * 0.25f);
      int a, b;
      curve_segment_points(curve, s, &a, &b);
      line.clear();
      for (int i = 0; i < steps; i++) {
        line.push_back(bezier_eval(ctrl, float(i) / float(steps)));
      }
      line.push_back(lines->points[b].pos);
      /* A piece of a segment that Resample split is picked with that segment: by the drawing's own points around it. */
      const int n = int(curve.points.size());
      const auto own_selected = [&](int i, int dir) {
        for (int step = 0; step < n; step++) {
          const MeshPoint &p = lines->points[curve.points[((i + dir * step) % n + n) % n].point];
          if (p.made_by < 0) {
            return p.selected;
          }
        }
        return false;
      };
      const bool picked_a = own_selected(s, -1), picked_b = own_selected(s + 1, 1);
      const bool selected = picked_a && picked_b;
      /* One end picked (Blender's edit mode): orange at that end, fading along the segment to the other's colour. */
      const bool fades = picked_a != picked_b;
      const ImU32 col = selected ? COL_WIRE_SELECTED : contrast_u32(wire.base, 0.92f);
      const ImU32 faint = (col & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 110);
      const float thickness = selected ? 1.6f : 1.5f;
      if (curve.points[s].edge_hidden) {
        /* A hidden edge (Moho's Hide Edge) draws dashed and faint, so it can still be found. */
        for (size_t i = 0; i + 1 < line.size(); i += 2) {
          g.lines.push_back({line[i], line[i + 1], faint, thickness});
        }
      }
      else if (cuts) {
        /* End Lines at Edge: solid where the line shows, dashed and faint where it's cut away. */
        shows.assign(line.size(), 0);
        for (size_t i = 0; i < line.size(); i++) {
          shows[i] = line_cutter_shows(*cutter, to_document.apply(line[i]));
        }
        for (size_t i = 0; i + 1 < line.size(); i++) {
          if (shows[i] && shows[i + 1]) {
            g.lines.push_back({line[i], line[i + 1], col, thickness});
          }
          else if (i % 2 == 0) {
            g.lines.push_back({line[i], line[i + 1], faint, thickness});
          }
        }
      }
      else if (!selected && (under || fades)) {
        /* Each piece its colour by what is drawn under it: the usual one, or where that is hard to see a lighter or
         * darker one, that piece alone. */
        pieces.clear();
        contrast_pieces(line, pixel * 6.0f, under_at, wire.base, pieces);
        float total = 0.0f;
        for (const ContrastPiece &p : pieces) {
          total += length(p.b - p.a);
        }
        float along = 0.0f;
        for (const ContrastPiece &p : pieces) {
          const ImU32 own = contrast_u32(wire.of(p.pick), 0.92f);
          if (!fades || total <= 0.0f) {
            g.lines.push_back({p.a, p.b, own, thickness});
            continue;
          }
          /* How far from the picked end each end of the piece is, 0 to 1. */
          const float t0 = along / total;
          along += length(p.b - p.a);
          const float t1 = along / total;
          const float f0 = picked_a ? t0 : 1.0f - t0, f1 = picked_a ? t1 : 1.0f - t1;
          g.lines.push_back({p.a, p.b, u32_mix(COL_WIRE_SELECTED, own, f0), thickness,
                             u32_mix(COL_WIRE_SELECTED, own, f1)});
        }
      }
      else {
        for (size_t i = 0; i + 1 < line.size(); i++) {
          g.lines.push_back({line[i], line[i + 1], col, thickness});
        }
      }
    }
  }
  if (!show_points) {
    return g;
  }
  /* Handles under the points: a line out to a dot, coloured by type, bright when selected. */
  const ContrastColors handle_own = contrast_colors_of(color_from_rgb(preferences().handle_color));
  const ContrastColors handle_picked = contrast_colors_of(color_from_rgb(preferences().handle_selected));
  const ContrastColors handle_tip_picked = contrast_colors_of(Color{1.0f, float(0x8c) / 255.0f, 0.0f, 1.0f});
  const ShownHandleFactors factors = drawing_detail_shown_factors(drawing);
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    const Curve &curve = drawing.curves[c];
    for (int i = 0; i < int(curve.points.size()); i++) {
      const CurvePoint &cp = curve.points[i];
      if (!handle_visible(drawing, cp, handles)) {
        continue;
      }
      const float2 here = drawing.points[cp.point].pos;
      const bool point_selected = drawing.points[cp.point].selected;
      for (const bool pre : {true, false}) {
        const float2 h = handle_end(drawing, factors, c, i, pre);
        if (distance(h, here) < 1e-9f) {
          continue;
        }
        /* A picked point lights its handles all along (as Blender's picks its handles); a picked tip alone lights its
         * end, fading to the point's. */
        const bool tip_picked = pre ? cp.select_in : cp.select_out;
        const bool at_point = point_selected, at_tip = point_selected || tip_picked;
        /* Light red, a stronger red where picked; no outline: in pieces, as wires, each taking a lighter or darker
         * shade only where what is drawn under it makes the red hard to see. */
        const ContrastColors &near_point = at_point ? handle_picked : handle_own;
        const ContrastColors &near_tip = at_tip ? handle_picked : handle_own;
        const int steps = std::clamp(int(std::ceil(distance(here, h) / (pixel * 6.0f))), 1, 64);
        ContrastPick pick_point = ContrastPick::Base, pick_tip = ContrastPick::Base;
        for (int s = 0; s < steps; s++) {
          const float t0 = float(s) / float(steps), t1 = float(s + 1) / float(steps);
          const float2 a = here + (h - here) * t0, b = here + (h - here) * t1;
          Color seen;
          if (under_at((a + b) * 0.5f, seen)) {
            pick_point = contrast_pick(seen, near_point.base, pick_point);
            pick_tip = contrast_pick(seen, near_tip.base, pick_tip);
          }
          const ImU32 from = contrast_u32(near_point.of(pick_point), 1.0f);
          const ImU32 to = contrast_u32(near_tip.of(pick_tip), 1.0f);
          if (from == to) {
            g.lines.push_back({a, b, from, 1.5f});
          }
          else {
            g.lines.push_back({a, b, u32_mix(from, to, t0), 1.5f, u32_mix(from, to, t1)});
          }
        }
        /* The tip, round, no outline: orange when picked; a lighter or darker shade where it is hard to see. */
        const ContrastColors &tip_colors = at_tip ? handle_tip_picked : handle_own;
        Color seen;
        const ImU32 tip = contrast_u32(
            under_at(h, seen) ? tip_colors.of(contrast_pick(seen, tip_colors.base)) : tip_colors.base, 1.0f);
        g.marks.push_back({h, tip, 0u, handle_size * 0.5f, 1.0f});
      }
    }
  }
  if (!point_squares) {
    /* Edges mode shows lines only, as Blender's edge select mode. */
    return g;
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
      /* Coarser points a third bigger. */
      const float r = point_size * (coarse[i] ? 0.667f : 0.5f);
      /* The wire's colour, or the one that reads on what is under the point. */
      Color c;
      const ImU32 fill = p.selected      ? COL_POINT_SELECTED
                         : under_at(p.pos, c) ? contrast_u32(wire.of(contrast_pick(c, wire.base)), 1.0f)
                                              : contrast_u32(wire.base, 1.0f);
      g.marks.push_back({p.pos, fill, p.selected ? 0u : IM_COL32(0x10, 0x10, 0x10, 170), r, 1.0f});
    }
  }
  return g;
}

/* Show Points: the points `layer`'s modifiers and its groups' add, for each modifier showing them, as hollow teal dots where they
 * end up after the whole stack (on the line as it shows). Drawn only: nothing picks them. */
static void draw_modifier_points(ImDrawList *dl, Context &C, const Layer &layer, const Transform2D &to_document,
                                 const ViewMapping &view, float point_size, bool show_points)
{
  Document &doc = *C.document;
  const WindowManager &wm = *C.wm;
  /* The modifier's points show: Show Points, or its Pieces or Spacing being dragged. */
  const auto on = [&](const Layer &owner, size_t index) {
    const Modifier &m = owner.modifiers[index];
    const bool scrubbed = wm.scrub_layer == owner.id && wm.scrub_modifier == int(index) &&
                          wm.scrub_frame >= ImGui::GetFrameCount() - 1;
    return m.show_viewport && modifier_makes_points(m) && (scrubbed || (show_points && m.show_points));
  };
  /* Its own modifiers', and those of the groups around it. */
  const auto any_on = [&](const Layer &owner) {
    for (size_t i = 0; i < owner.modifiers.size(); i++) {
      if (on(owner, i)) {
        return true;
      }
    }
    return false;
  };
  bool any = any_on(layer);
  for (const Layer *group = layer_parent(doc, layer.id); group && !any; group = layer_parent(doc, group->id)) {
    any = any_on(*group);
  }
  if (!any) {
    return;
  }
  const VectorDrawing shown = layer_evaluated_drawing(doc, layer, false);
  const ImU32 teal = IM_COL32(0x2e, 0xc4, 0xb6, 255);
  const float r = std::max(point_size * 0.5f, 2.5f);
  for (const MeshPoint &p : shown.points) {
    const Layer *owner = p.made_by_layer != 0 ? layer_find(doc, p.made_by_layer) : &layer;
    if (p.made_by < 0 || !owner || size_t(p.made_by) >= owner->modifiers.size() || !on(*owner, size_t(p.made_by))) {
      continue;
    }
    const ImVec2 at = view.to_screen(to_document.apply(p.pos));
    /* A dark ring under the teal one, so it shows on light and dark alike. */
    dl->AddCircle(at, r, IM_COL32(0, 0, 0, 120), 0, 3.2f);
    dl->AddCircle(at, r, teal, 0, 1.6f);
  }
}

OverlayColorsTest overlay_colors_test(const Document &doc, const Layer &layer, const CanvasSample &under, float pixel,
                                      bool handles)
{
  const CanvasSample *was = g_canvas_now;
  g_canvas_now = &under;
  const Posed shown = posed(doc, layer);
  const OverlayGeometry g = overlay_geometry(*shown.drawing, posed_wires(shown), layer_world_transform(doc, layer.id),
                                             true, true, 4, handles ? HandleDisplay::All : HandleDisplay::None, true, nullptr,
                                             6.0f, 6.5f, pixel);
  g_canvas_now = was;
  OverlayColorsTest out;
  const Transform2D to_document = layer_world_transform(doc, layer.id);
  for (const OverlayLine &l : g.lines) {
    out.wires.push_back({to_document.apply(l.a), to_document.apply(l.b), l.color, l.width, l.color_b});
  }
  for (const OverlayMark &m : g.marks) {
    out.points.push_back({to_document.apply(m.at), m.fill, m.outline, m.round});
  }
  return out;
}

/* The wires, handles and points of a drawing being edited, drawn by the GPU (gpu/overlay_renderer.hh) at the
 * next overlay_flush: made again only when what they show changes, kept per `slot` (the layer, in its
 * viewport). `cutter` (optional, End Lines at Edge): the stretches of the wires whose line it cuts away draw
 * dashed and faint, as hidden edges do. */
static void draw_drawing_overlay(uint64_t slot, const Layer &owner, const Posed &shown,
                                 const Transform2D &to_document, const ViewMapping &view, bool show_wires,
                                 bool show_points, int quality, HandleDisplay handles, bool point_squares = true,
                                 const LineCutter *cutter = nullptr, float point_size = 6.0f, float handle_size = 6.5f)
{
  const VectorDrawing &drawing = *shown.drawing;
  const VectorDrawing *wires = posed_wires(shown);
  /* A pixel in layer space, in steps of 2^(1/8) as the scene renderer's: zooming makes them again every few
   * wheel notches. */
  const float pixel =
      std::exp2(std::round(std::log2(1.0f / std::max(view.zoom * to_document.scale(), 1e-9f)) * 8.0f) / 8.0f);
  const bool cuts = cutter && !cutter->empty();
  uint64_t key = overlay_mix(1469598103934665603ull, uint64_t(show_wires) | uint64_t(show_points) << 1 |
                                                         uint64_t(point_squares) << 2 | uint64_t(handles) << 3);
  key = overlay_mix(key, uint64_t(uint32_t(quality)));
  if (wires) {
    key = overlay_mix(key, overlay_fingerprint(*wires, pixel / 16.0f));
  }
  for (const float v : {point_size, handle_size, pixel}) {
    key = overlay_mix_float(key, v);
  }
  /* Colours picked by what is under them depend on that, where the drawing is, and the colours set. */
  if (g_canvas_now) {
    key = overlay_mix(key, g_canvas_now->serial);
    for (const float v : {to_document.a, to_document.b, to_document.c, to_document.d, to_document.tx, to_document.ty}) {
      key = overlay_mix_float(key, v);
    }
  }
  {
    const Preferences &prefs = preferences();
    key = overlay_mix(key, uint64_t(prefs.wire_color) | uint64_t(prefs.wire_lighter) << 24);
    key = overlay_mix(key, uint64_t(prefs.wire_darker) | uint64_t(prefs.handle_color) << 24);
    key = overlay_mix(key, uint64_t(prefs.handle_selected));
  }
  /* Lines ended at masks' edges depend on where the drawing is: then exactly, with the cut and the place. */
  if (cuts) {
    key = overlay_mix(key, overlay_fingerprint(drawing, 0.0f));
    for (const float v : {to_document.a, to_document.b, to_document.c, to_document.d, to_document.tx, to_document.ty}) {
      key = overlay_mix_float(key, v);
    }
    for (const auto &rows : cutter->levels) {
      for (const LineCutter::Row &row : rows) {
        key = overlay_mix(key, uint64_t(row.mode) | uint64_t(row.invert) << 8 | uint64_t(row.fills_only) << 9 |
                                   uint64_t(row.strokes_only) << 10 | uint64_t(row.inside_stroke) << 11 |
                                   uint64_t(row.combine) << 12 | uint64_t(row.start_shown) << 16);
        for (const LineCutter::Piece &piece : row.pieces) {
          key = overlay_mix_float(overlay_mix(key, uint64_t(piece.closed) | uint64_t(piece.area) << 1 |
                                                       uint64_t(piece.line) << 2 | uint64_t(piece.source) << 3),
                                  piece.half);
          for (const float2 &q : piece.outline) {
            key = overlay_mix_float(overlay_mix_float(key, q.x), q.y);
          }
        }
      }
    }
  }
  else {
    /* A 16th of a pixel apart is the same. The layer's own drawing (not posed by bones, not a reference built
     * from another) keeps its fingerprint while its revision does (see Revision). */
    struct KeptFingerprint {
      uint64_t revision = 0;
      float pixel = 0.0f;
      uint64_t fingerprint = 0;
    };
    static std::unordered_map<uint64_t, KeptFingerprint> kept;
    if (&drawing == &owner.drawing && !owner.is_reference()) {
      KeptFingerprint &k = kept[slot];
      if (k.revision != owner.revision.value || k.pixel != pixel || k.fingerprint == 0) {
        k = {owner.revision.value, pixel, overlay_fingerprint(drawing, pixel / 16.0f)};
      }
      key = overlay_mix(key, overlay_drawing_identity(k.fingerprint, drawing));
    }
    else {
      key = overlay_mix(key, overlay_fingerprint(drawing, pixel / 16.0f));
    }
  }
  const float2 first = drawing.points.empty() ? float2() : drawing.points[0].pos;
  if (!overlay_kept(slot, key)) {
    overlay_keep(slot, key,
                 overlay_geometry(drawing, wires, to_document, show_wires, show_points, quality, handles, point_squares,
                                  cutter, point_size, handle_size, pixel),
                 first);
  }
  /* Document to screen, as ViewMapping::to_screen; a drawing moved whole since they were made goes the
   * difference further. */
  const Transform2D doc_to_screen{view.zoom,
                                  0.0f,
                                  0.0f,
                                  -view.zoom,
                                  view.origin.x + view.size.x * 0.5f - view.center.x * view.zoom,
                                  view.origin.y + view.size.y * 0.5f + view.center.y * view.zoom};
  const Transform2D moved = cuts ? Transform2D() : Transform2D::translate(first - overlay_kept_origin(slot));
  overlay_queue(slot, doc_to_screen * to_document * moved);
}

/* Which overlays a layer's are, in a viewport: each viewport keeps its own (their zooms differ). */
static uint64_t overlay_slot(const Area &area, uint64_t layer)
{
  return layer ^ (uint64_t(uint32_t(area.id)) << 48);
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
  if (active && active->is_bone_layer()) {
    return active;
  }
  const Layer *armature = active ? layer_armature(*C.document, active->id) : nullptr;
  return armature ? layer_find(*C.document, armature->id) : nullptr;
}

static bool bone_picked(const Bone &bone)
{
  return bone.selected || bone.select_head || bone.select_tail;
}

/* Ctrl-click in the weight tools: that bone alone. With Shift it joins the selected ones, or leaves them if it was
 * one, for Automatic: Selected Bones; the last one added is the one painted with. */
static void weight_bone_pick(Layer &armature, uint64_t id, bool add)
{
  Bone *bone = bone_find(armature, id);
  if (!add) {
    armature.active_bone = id;
    for (Bone &b : armature.bones) {
      bone_select(b, b.id == id);
    }
    return;
  }
  const bool leaves = bone_picked(*bone);
  bone_select(*bone, !leaves);
  if (!leaves) {
    armature.active_bone = id;
  }
  else if (armature.active_bone == id) {
    armature.active_bone = 0;
    for (const Bone &b : armature.bones) {
      armature.active_bone = bone_picked(b) ? b.id : armature.active_bone;
    }
  }
}

/* The deform modifier Weight Paint paints on the active layer: the one the tool settings name, or with
 * no bones to paint, the first. Null: the bones. */
static Modifier *weight_deform(Context &C)
{
  Layer *layer = document_active_layer(*C.document);
  /* A group's deforms have no weights: Automatic, every point fully. */
  if (!layer || layer->is_group()) {
    return nullptr;
  }
  Modifier *first = nullptr;
  for (Modifier &m : layer->modifiers) {
    if (!modifier_bends(m.type)) {
      continue;
    }
    if (m.name == C.wm->tool_settings.weight_target) {
      return &m;
    }
    first = first ? first : &m;
  }
  return layer_armature(*C.document, layer->id) ? nullptr : first;
}

/* Weight Paint's dropdown at the top of the viewport: the bones (Armature), or one of the layer's Vector
 * Deform modifiers. */
static void weight_target_combo(Context &C)
{
  Layer *layer = document_active_layer(*C.document);
  if (!layer || layer->is_group()) {
    return;
  }
  const bool bones = layer_armature(*C.document, layer->id) != nullptr;
  const Modifier *picked = weight_deform(C);
  int deforms = 0;
  for (const Modifier &m : layer->modifiers) {
    deforms += modifier_bends(m.type) ? 1 : 0;
  }
  if (deforms == 0) {
    return;
  }
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 6.0f);
  if (ImGui::BeginCombo("##weight_target", picked ? picked->name.c_str() : "Armature")) {
    if (bones && ImGui::Selectable("Armature", picked == nullptr)) {
      C.wm->tool_settings.weight_target.clear();
    }
    for (const Modifier &m : layer->modifiers) {
      if (modifier_bends(m.type) && ImGui::Selectable(m.name.c_str(), &m == picked)) {
        C.wm->tool_settings.weight_target = m.name;
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("What to paint: the bones' weights, or a deform modifier's (Curve, Cage or Mesh Deform)");
  }
  ImGui::SameLine();
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

/* Keep Animation in the weight tools: a weight change on the setup frame leaves the drawings where they
 * show on every keyed frame (their points keyed back there). Kept while Open Animate is open. */
static bool g_weight_keep_animation = false;

static void weight_keep_checkbox()
{
  ImGui::SameLine();
  ImGui::Checkbox("Keep Animation##weights", &g_weight_keep_animation);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Changing weights keeps the drawing where it shows on every keyed frame: the points the "
                      "change bends differently are keyed back there.\nOff, the poses follow the new weights");
  }
}

static OpArgs weight_args()
{
  return {{"keep_animation", g_weight_keep_animation ? "1" : "0"}};
}

/* Weight Paint shows no point selection without its Selection Mask, so a selection left by Weight Select doesn't count. */
static OpArgs weight_automatic_args(Context &C, bool selected_bones)
{
  OpArgs args = weight_args();
  args["bones"] = selected_bones ? "SELECTED" : "ALL";
  const ToolSettings &ts = C.wm->tool_settings;
  if (ts.tool == Tool::WeightPaint && !ts.weight_mask) {
    args["points"] = "ALL";
  }
  return args;
}

static void weight_automatic_selected_button(Context &C, const Layer &armature)
{
  const bool any = std::any_of(armature.bones.begin(), armature.bones.end(), bone_picked);
  ImGui::BeginDisabled(!any);
  if (ImGui::Button("Automatic: Selected Bones")) {
    operator_call(C, "weight.automatic", weight_automatic_args(C, true));
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Bind the points to the selected bones only: nearer bones pull harder, and what they followed "
                      "before is replaced.\nCtrl-click a bone to pick it, Ctrl+Shift-click to add more");
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
    operator_call(C, "weight.automatic", weight_automatic_args(C, false));
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Bind the selected points (all when none are selected) to every bone: nearer bones pull "
                      "harder, as Blender's Automatic Weights");
  }
  ImGui::SameLine();
  weight_automatic_selected_button(C, *armature);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 4.0f);
  ui_slider_float("##assign_weight", &ts.weight_value, 0.0f, 1.0f, "Weight %.2f");
  ImGui::SameLine();
  if (ImGui::Button("Assign")) {
    operator_call(C, "weight.assign", weight_args());
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Give the selected points this weight for the active bone (1.0: they follow it alone)");
  }
  ImGui::SameLine(0, 1);
  if (ImGui::Button("Remove")) {
    operator_call(C, "weight.remove", weight_args());
  }
  ImGui::SameLine(0, 1);
  if (ImGui::Button("Normalize")) {
    operator_call(C, "weight.normalize", weight_args());
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto Normalize", &ts.weight_normalize);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Keep each point's weights adding up to 1 when assigning or painting");
  }
  weight_keep_checkbox();
}

/* The Sculpt tool's header: the brush (its preset's name), Radius and Strength (each with pen
 * pressure), Falloff and Selected Only; Surface Smooth's Lump Sensitivity while it is the brush. */
static void sculpt_header(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  const float h = ImGui::GetFrameHeight();
  ImGui::SetNextItemWidth(h * 5.6f);
  if (ImGui::BeginCombo("##sculpt_brush", ts.sculpt_preset.empty() ? sculpt_brush_name(ts.sculpt_brush)
                                                                   : ts.sculpt_preset.c_str()))
  {
    for (int i = 0; i < SCULPT_BRUSH_COUNT; i++) {
      const SculptBrush brush = SculptBrush(i);
      if (ImGui::Selectable(sculpt_brush_name(brush), ts.sculpt_brush == brush)) {
        operator_call(C, "sculpt.brush_set", {{"brush", sculpt_brush_id(brush)}});
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("The brush (the shelf below has its presets)");
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(h * 6.0f);
  pressure_field("##sculpt_radius", &ts.sculpt_radius, 0.5f, 2.0f, 1000.0f, "Radius %.0f px", &ts.sculpt_pressure_radius,
                 "Pen pressure sets the radius");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How big the brush is, in screen pixels (F)");
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(h * 6.0f);
  /* Blender's sculpt Strength: a bar filled across 0-1, dragged within it, typed as high as wanted. */
  pressure_field("##sculpt_strength", &ts.sculpt_strength, 0.005f, 0.0f, 1.0f, "Strength %.3f",
                 &ts.sculpt_pressure_strength, "Pen pressure sets the strength", true, 0.0f, FLT_MAX);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How hard the brush works (Shift F)");
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(h * 3.6f);
  static const char *const falloffs[] = {"Smooth", "Sphere", "Constant"};
  int falloff = int(ts.sculpt_falloff);
  if (ImGui::Combo("##sculpt_falloff", &falloff, falloffs, 3)) {
    ts.sculpt_falloff = SculptFalloff(falloff);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How the brush fades toward its edge: Smooth eases out, Sphere stays strong almost to the "
                      "edge, Constant is the same all over");
  }
  ImGui::SameLine();
  if (header_toggle("Selected Only", ts.sculpt_selected_only,
                    "Sculpt only the selected points, as Blender's selection mask")) {
    ts.sculpt_selected_only = !ts.sculpt_selected_only;
  }
  if (ts.sculpt_brush == SculptBrush::SurfaceSmooth) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(h * 7.0f);
    /* A bar filled across 0.001-1, dragged within it, typed as high as wanted or down to 0 (none). */
    ui_slider_float("##surface_sensitivity", &ts.sculpt_surface_sensitivity, 0.001f, 1.0f, "Lump Sensitivity %.3f", 0,
                    0.0f, FLT_MAX);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How big a lump Surface Smooth takes out: 0 none, higher bigger ones (the ghost shows the "
                        "result). The same on any canvas, whatever its resolution, the brush's size or the zoom");
    }
  }
}

/* How far the mouse is along an F or Shift F drag. */
static float adjust_reach(float &base_x, bool &slow, float &slow_x, float &slow_reach)
{
  const ImGuiIO &io = ImGui::GetIO();
  const float now = slow ? slow_reach + 0.1f * (io.MousePos.x - slow_x) : io.MousePos.x - base_x;
  if (!slow && (ImGui::IsKeyPressed(ImGuiKey_LeftShift, false) || ImGui::IsKeyPressed(ImGuiKey_RightShift, false))) {
    slow = true;
    slow_x = io.MousePos.x;
    slow_reach = now;
  }
  else if (slow && !io.KeyShift) {
    /* Leaving Precision Mode carries on from where it got to. */
    slow = false;
    base_x = io.MousePos.x - now;
  }
  return std::max(now, 0.0f);
}

/* F or Shift F with the Draw tool, as the Sculpt tool's: the line's width ('R'), the circle showing it, or its
 * opacity ('S'). True while it has the mouse. */
struct DrawAdjust {
  char kind = 0;
  int area = 0;
  ImVec2 anchor;
  float base_x = 0.0f;
  bool slow = false;
  float slow_x = 0.0f, slow_reach = 0.0f;
  float was = 0.0f;
};
static DrawAdjust g_draw_adjust;
/* Shift F: this many pixels from the anchor is fully solid. */
static constexpr float DRAW_OPACITY_PX = 200.0f;

static bool draw_adjust_input(Context &C, Area &area, float pixel, bool hovered, ImDrawList *dl)
{
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  DrawAdjust &adjust = g_draw_adjust;
  float &value = wm.draw_adjust_request == 'R' || adjust.kind == 'R' ? draw_width(ts, ts.draw_mode)
                 : ts.draw_mode == DrawMode::FillPen                  ? ts.style.fill.a
                                                                      : ts.style.line.a;
  if (wm.draw_adjust_request && hovered && !g_drag.active) {
    adjust = {};
    adjust.kind = wm.draw_adjust_request;
    adjust.area = area.id;
    adjust.was = value;
    adjust.anchor = io.MousePos;
    adjust.base_x = io.MousePos.x - (adjust.kind == 'R' ? value * 0.5f / pixel : value * DRAW_OPACITY_PX);
  }
  wm.draw_adjust_request = 0;
  if (!adjust.kind || adjust.area != area.id) {
    return false;
  }
  const float reach = adjust_reach(adjust.base_x, adjust.slow, adjust.slow_x, adjust.slow_reach);
  char text[64];
  if (adjust.kind == 'R') {
    value = reach * 2.0f * pixel;
    dl->AddCircle(adjust.anchor, reach, IM_COL32(0xff, 0xff, 0xff, 230), 64, 1.5f);
    dl->AddCircle(adjust.anchor, reach + 1.5f, IM_COL32(0x00, 0x00, 0x00, 160), 64, 1.0f);
    std::snprintf(text, sizeof(text), "Width %.1f px", double(value) * C.document->height * 0.5);
  }
  else {
    value = std::clamp(reach / DRAW_OPACITY_PX, 0.0f, 1.0f);
    dl->AddCircle(adjust.anchor, DRAW_OPACITY_PX, IM_COL32(0xff, 0xff, 0xff, 120), 64, 1.0f);
    dl->AddCircleFilled(adjust.anchor, value * DRAW_OPACITY_PX, IM_COL32(0x47, 0x72, 0xb3, 90), 64);
    std::snprintf(text, sizeof(text), "Opacity %.2f", double(value));
  }
  /* On a plate: the drawing under it can be any colour. */
  const ImVec2 text_size = ImGui::CalcTextSize(text), text_at = adjust.anchor - text_size * 0.5f;
  dl->AddRectFilled(text_at - ImVec2(6, 3), text_at + text_size + ImVec2(6, 3), IM_COL32(0x18, 0x18, 0x18, 220), 4.0f);
  dl->AddText(text_at, IM_COL32(0xff, 0xff, 0xff, 255), text);
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    adjust.kind = 0;
  }
  else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    value = adjust.was;
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    adjust.kind = 0;
  }
  return true;
}

/* Surface Smooth's ghost: the fit its next stroke would move the line toward. Worked out again only when the
 * drawing is edited (a stroke, another edit, undo or redo: the undo history moves), or the frame, the layers or
 * the tolerance change, and then (the tolerance the same) only for the curves that changed; never for moving the
 * view (the app counts everything as maybe changed while a mouse button is down). A stroke starts from it. */
struct SurfaceGhost {
  size_t undo_size = 0, undo_active = 0;
  bool valid = false;
  /* A curve scrolled into view has no fit yet: fit the ones on view (the others keep theirs). */
  bool missing = false;
  /* Fitted on view only (while a value was being scrubbed): every curve is fitted once it stops. */
  bool partial = false;
  int frame = 0;
  float tolerance = -1.0f;
  std::vector<uint64_t> ids;
  SculptStroke stroke;
};
static SurfaceGhost g_surface_ghost;

/* What the view shows, in document space, with a margin (so a curve just scrolling in has its fit already). */
static SculptBox view_box(const ViewMapping &view)
{
  const float2 a = view.to_document(view.origin), b = view.to_document(view.origin + view.size);
  const float2 margin{(std::fabs(b.x - a.x)) * 0.15f, (std::fabs(b.y - a.y)) * 0.15f};
  return {float2{std::min(a.x, b.x), std::min(a.y, b.y)} - margin, float2{std::max(a.x, b.x), std::max(a.y, b.y)} + margin};
}

/* Draws where Surface Smooth takes the line: each segment whose fit differs from the line, as a pale curve. Returns
 * whether a curve the view shows has no fit yet (it was off the view when the fit was worked out). */
static bool draw_surface_ghost(ImDrawList *dl, const ViewMapping &view, const SculptStroke &stroke)
{
  const ImU32 col = IM_COL32(0x7f, 0xe6, 0xff, 170);
  const SculptBox shown = view_box(view);
  bool missing = false;
  for (const SculptStroke::Layer &sl : stroke.layers) {
    for (const SculptStroke::Curve &c : sl.curves) {
      const int n = int(c.points.size());
      if (c.target_pos.size() != size_t(n)) {
        if (!missing) {
          for (const int k : c.anchor) {
            const float2 p = c.pos[size_t(k)];
            missing |= p.x >= shown.lo.x && p.x <= shown.hi.x && p.y >= shown.lo.y && p.y <= shown.hi.y;
          }
        }
        continue;
      }
      for (int s = 0; s < (c.closed ? n : n - 1); s++) {
        const int a = s, b = (s + 1) % n;
        const float2 pa = c.pos[size_t(c.anchor[size_t(a)])], pb = c.pos[size_t(c.anchor[size_t(b)])];
        const float2 now[4] = {pa, pa + c.handle_out[size_t(a)], pb + c.handle_in[size_t(b)], pb};
        const float2 fit[4] = {c.target_pos[size_t(a)], c.target_pos[size_t(a)] + c.target_out[size_t(a)],
                               c.target_pos[size_t(b)] + c.target_in[size_t(b)], c.target_pos[size_t(b)]};
        float apart = 0.0f;
        for (int k = 0; k < 4; k++) {
          apart = std::max(apart, distance(now[k], fit[k]) * view.zoom);
        }
        /* Not where the fit is the line to within a pixel and a half (a circle nudged by a hair). */
        if (apart < 1.5f) {
          continue;
        }
        dl->AddBezierCubic(view.to_screen(fit[0]), view.to_screen(fit[1]), view.to_screen(fit[2]),
                           view.to_screen(fit[3]), col, 1.5f);
      }
    }
  }
  return missing;
}

/* A sculpt stroke while the left button is down, or F / Shift F sizing the brush. */
struct SculptState {
  bool active = false;
  int area = 0;
  float2 last;
  SculptStroke stroke;
  SetupSnapshot kept;
  bool changed = false;
  /* Started with Shift held: the whole stroke smooths, as in Blender's sculpt modes. */
  bool smoothing = false;
  /* F ('R') or Shift F ('S'), as Blender's radial control: the circle stays centred on `anchor` (where
   * the brush was) and the pointer where it is; moving right from where F was pressed grows it, left
   * shrinks it (`base_x`: where the mouse would be at 0). Shift while sizing is Precision Mode, a
   * tenth as fast from where it was pressed. `was` comes back on Esc. */
  char adjust = 0;
  int adjust_area = 0;
  ImVec2 anchor;
  float base_x = 0.0f;
  bool slow = false;
  float slow_x = 0.0f, slow_reach = 0.0f;
  float was = 0.0f;
};
static SculptState g_sculpt;
/* Shift F: this many pixels from the anchor is full strength. */
static constexpr float SCULPT_STRENGTH_PX = 200.0f;

static void sculpt_input(Context &C, Area &area, const ViewMapping &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  if (wm.sculpt_adjust_request && hovered && !g_sculpt.active) {
    g_sculpt.adjust = wm.sculpt_adjust_request;
    g_sculpt.adjust_area = area.id;
    g_sculpt.was = g_sculpt.adjust == 'R' ? ts.sculpt_radius : ts.sculpt_strength;
    const float reach = g_sculpt.adjust == 'R' ? ts.sculpt_radius : ts.sculpt_strength * SCULPT_STRENGTH_PX;
    g_sculpt.anchor = io.MousePos;
    g_sculpt.base_x = io.MousePos.x - reach;
    g_sculpt.slow = false;
  }
  wm.sculpt_adjust_request = 0;
  if (g_sculpt.adjust && g_sculpt.adjust_area == area.id) {
    const float reach = adjust_reach(g_sculpt.base_x, g_sculpt.slow, g_sculpt.slow_x, g_sculpt.slow_reach);
    char text[64];
    if (g_sculpt.adjust == 'R') {
      ts.sculpt_radius = std::clamp(reach, 2.0f, 1000.0f);
      dl->AddCircle(g_sculpt.anchor, ts.sculpt_radius, IM_COL32(0xff, 0xff, 0xff, 230), 64, 1.5f);
      std::snprintf(text, sizeof(text), "Radius %.0f px", double(ts.sculpt_radius));
    }
    else {
      ts.sculpt_strength = std::clamp(reach / SCULPT_STRENGTH_PX, 0.0f, 1.0f);
      dl->AddCircle(g_sculpt.anchor, SCULPT_STRENGTH_PX, IM_COL32(0xff, 0xff, 0xff, 120), 64, 1.0f);
      dl->AddCircleFilled(g_sculpt.anchor, ts.sculpt_strength * SCULPT_STRENGTH_PX, IM_COL32(0x47, 0x72, 0xb3, 90), 64);
      std::snprintf(text, sizeof(text), "Strength %.2f", double(ts.sculpt_strength));
    }
    dl->AddText(g_sculpt.anchor + ImVec2(-ImGui::CalcTextSize(text).x * 0.5f, -ImGui::GetFontSize() * 0.5f),
                IM_COL32(0xff, 0xff, 0xff, 255), text);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      g_sculpt.adjust = 0;
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      (g_sculpt.adjust == 'R' ? ts.sculpt_radius : ts.sculpt_strength) = g_sculpt.was;
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      g_sculpt.adjust = 0;
    }
    return;
  }

  const float pressure = std::clamp(wm.pen_pressure, 0.0f, 1.0f);
  const float radius_px = std::max(ts.sculpt_radius * (ts.sculpt_pressure_radius ? pressure : 1.0f), 1.0f);
  const float2 mouse = view.to_document(io.MousePos);
  /* Shift held: Smooth, with the built-in Smooth's strength and falloff and this brush's size, as
   * Blender's sculpt modes smooth with Shift whatever the brush. A stroke keeps what it began with. */
  const bool smoothing = g_sculpt.active ? g_sculpt.smoothing : io.KeyShift;
  const SculptPreset &smooth = sculpt_preset_default(SculptBrush::Smooth);
  SculptDab dab;
  dab.brush = smoothing ? SculptBrush::Smooth : ts.sculpt_brush;
  dab.center = mouse;
  dab.radius = radius_px / std::max(view.zoom, 1e-9f);
  dab.strength = (smoothing ? smooth.strength : ts.sculpt_strength) * (ts.sculpt_pressure_strength ? pressure : 1.0f);
  dab.falloff = smoothing ? smooth.falloff : ts.sculpt_falloff;
  dab.invert = io.KeyCtrl && !smoothing;
  dab.selected_only = ts.sculpt_selected_only;
  dab.mirror_x = ts.mirror_x;
  dab.surface_sensitivity = ts.sculpt_surface_sensitivity;
  const std::vector<uint64_t> ids = editable_layer_ids(C);

  if (hovered || g_sculpt.active) {
    /* The brush (it works on the line, so no points are lit). */
    dl->AddCircle(io.MousePos, radius_px,
                  dab.invert ? IM_COL32(0x80, 0xb0, 0xff, 220) : IM_COL32(0xff, 0xff, 0xff, 220), 64, 1.5f);
    if (smoothing) {
      dl->AddText(io.MousePos + ImVec2(radius_px * 0.72f + 4.0f, radius_px * 0.72f), IM_COL32(0xff, 0xff, 0xff, 230),
                  "Smooth");
    }
  }
  /* Surface Smooth shows the fit it moves the line toward: during a stroke the stroke's own, else the fit a
   * stroke would start with now. Whether or not the mouse is over the view, so it follows Lump Sensitivity as it
   * is scrubbed in the header. */
  /* By the brush picked, not the one Shift lends for the moment (Shift also scrubs a field finely). With the overlays
   * off (Shift Z) nothing shows, and nothing is fitted for it. */
  if (ts.sculpt_brush == SculptBrush::SurfaceSmooth && doc.overlays.show) {
    SculptDab ghost_dab = dab;
    ghost_dab.brush = SculptBrush::SurfaceSmooth;
    if (g_sculpt.active && g_sculpt.area == area.id && !g_sculpt.smoothing) {
      draw_surface_ghost(dl, view, g_sculpt.stroke);
    }
    else {
      SurfaceGhost &ghost = g_surface_ghost;
      const float tolerance = sculpt_surface_tolerance(ghost_dab);
      const size_t undo_size = C.undo ? C.undo->size() : 0, undo_active = C.undo ? C.undo->active() : 0;
      /* A field being dragged or typed in (Lump Sensitivity scrubbed): fit only what the view shows, so it keeps
       * up; once it stops, every editable curve, so moving or zooming the view later finds them all fitted. */
      const bool scrubbing = ImGui::IsAnyItemActive();
      if (!ghost.valid || ghost.missing || (ghost.partial && !scrubbing) || ghost.undo_size != undo_size ||
          ghost.undo_active != undo_active || ghost.frame != doc.frame_current || ghost.ids != ids ||
          ghost.tolerance != tolerance)
      {
        /* Curves just as they were keep their fit (the same tolerance): only the changed ones are fitted. */
        const SculptBox shown = view_box(view);
        SculptStroke refreshed = sculpt_stroke_begin(
            doc, ids, ghost_dab, ghost.valid && ghost.tolerance == tolerance ? &ghost.stroke : nullptr,
            scrubbing ? &shown : nullptr);
        ghost.partial = false;
        for (const SculptStroke::Layer &sl : refreshed.layers) {
          for (const SculptStroke::Curve &c : sl.curves) {
            ghost.partial |= c.target_pos.size() != c.points.size();
          }
        }
        ghost.stroke = std::move(refreshed);
        ghost.valid = true;
        ghost.undo_size = undo_size;
        ghost.undo_active = undo_active;
        ghost.frame = doc.frame_current;
        ghost.ids = ids;
        ghost.tolerance = tolerance;
      }
      ghost.missing = draw_surface_ghost(dl, view, ghost.stroke);
    }
  }

  if (hovered && !g_sculpt.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
    g_sculpt.active = true;
    g_sculpt.area = area.id;
    g_sculpt.last = mouse;
    g_sculpt.changed = false;
    g_sculpt.smoothing = smoothing;
    g_sculpt.kept = doc.frame_current == 0 && keep_poses_on(doc) ? setup_snapshot(doc, ids) : SetupSnapshot();
    /* Surface Smooth starts from the ghost's fit when it is of the drawing as it is (no fitting again). */
    const SurfaceGhost &ghost = g_surface_ghost;
    const bool ghost_fresh = dab.brush == SculptBrush::SurfaceSmooth && ghost.valid && C.undo &&
                             ghost.undo_size == C.undo->size() && ghost.undo_active == C.undo->active() &&
                             ghost.frame == doc.frame_current && ghost.ids == ids &&
                             ghost.tolerance == sculpt_surface_tolerance(dab);
    g_sculpt.stroke = sculpt_stroke_begin(doc, ids, dab, ghost_fresh ? &ghost.stroke : nullptr);
  }
  if (!g_sculpt.active || g_sculpt.area != area.id) {
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (g_sculpt.changed) {
      if (!g_sculpt.kept.empty()) {
        animation_record(doc);
        keep_poses(doc, g_sculpt.kept);
      }
      const std::string name = std::string("Sculpt ") + sculpt_brush_name(dab.brush);
      push_undo(C, name.c_str());
      /* A Surface Smooth stroke moves the line toward where it is going, not where that is (smoothing the line
       * again lands in the same place, the smoother being linear): the ghost keeps the stroke's targets, the line
       * as the stroke left it with them, instead of working them out again. */
      if (dab.brush == SculptBrush::SurfaceSmooth && C.undo) {
        SurfaceGhost &ghost = g_surface_ghost;
        ghost.stroke = std::move(g_sculpt.stroke);
        ghost.valid = true;
        ghost.missing = false;
        ghost.partial = false;
        ghost.undo_size = C.undo->size();
        ghost.undo_active = C.undo->active();
        ghost.frame = doc.frame_current;
        ghost.ids = ids;
        ghost.tolerance = sculpt_surface_tolerance(dab);
      }
    }
    g_sculpt = SculptState();
    return;
  }
  dab.move = mouse - g_sculpt.last;
  /* The brushes that work on while held: a tenth of a second is a full step. */
  dab.amount = std::min(1.0f, io.DeltaTime * 10.0f);
  const bool moving_brush = dab.brush == SculptBrush::Grab || dab.brush == SculptBrush::Push;
  if (!moving_brush || dab.move.x != 0.0f || dab.move.y != 0.0f) {
    g_sculpt.changed |= sculpt_stroke_dab(doc, g_sculpt.stroke, dab) > 0;
  }
  g_sculpt.last = mouse;
}

static void weight_paint_header(Context &C)
{
  ToolSettings &ts = C.wm->tool_settings;
  weight_target_combo(C);
  Layer *armature = weight_armature(C);
  Modifier *deform = weight_deform(C);
  if (deform && !deform->deform_custom) {
    /* Automatic weights aren't painted: shown, with the way to paint them. */
    ImGui::TextDisabled("Automatic weights (shown):");
    ImGui::SameLine();
    if (ImGui::Button("Switch to Custom")) {
      Layer &layer = *document_active_layer(*C.document);
      deform->deform_custom = true;
      vector_deform_fill_group(*C.document, layer, *deform);
      push_undo(C, "Deform Weights: Custom");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Paint this modifier's weights: they start from the automatic ones, in a point group of "
                        "their own");
    }
    return;
  }
  if (!deform && !armature) {
    return;
  }
  if (!deform) {
    weight_bone_combo(armature);
    ImGui::SameLine();
    weight_automatic_selected_button(C, *armature);
  }
  const float w = ImGui::GetFrameHeight() * 4.0f;
  ImGui::SameLine();
  const float wp = ImGui::GetFrameHeight() * 6.0f;
  ImGui::SetNextItemWidth(wp);
  pressure_field("##weight", &ts.weight_value, 0.005f, 0.0f, 1.0f, "Weight %.2f", &ts.weight_pressure_weight,
                 "Pen pressure sets the weight", true);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(wp);
  pressure_field("##radius", &ts.weight_radius, 0.5f, 2.0f, 500.0f, "Radius %.0f px", &ts.weight_pressure_radius,
                 "Pen pressure sets the radius");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(wp);
  pressure_field("##strength", &ts.weight_strength, 0.005f, 0.0f, 1.0f, "Strength %.2f", &ts.weight_pressure_strength,
                 "Pen pressure sets the strength", true);
  (void)w;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 3.5f);
  static const char *const modes[] = {"Draw", "Add", "Subtract"};
  ImGui::Combo("##weight_mode", &ts.weight_mode, modes, 3);
  if (!deform) {
    ImGui::SameLine();
    ImGui::Checkbox("Normalize", &ts.weight_normalize);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Keep each point's weights adding up to 1, as Blender's Auto Normalize");
    }
  }
  ImGui::SameLine();
  ImGui::Checkbox("Selection Mask", &ts.weight_mask);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Paint only the selected points (select them with Weight Select), as Blender's selection "
                      "masking");
  }
  weight_keep_checkbox();
}

/* The active drawing coloured by how strongly the active bone (or the deform modifier being painted) moves it. */
static void draw_weight_overlay(ImDrawList *dl, Context &C, const Layer &layer, const ViewMapping &view, int quality)
{
  const Document &doc = *C.document;
  const Layer *armature = layer_armature(doc, layer.id);
  const Modifier *deform = C.wm->tool_settings.tool == Tool::WeightPaint ? weight_deform(C) : nullptr;
  if (!armature && !deform) {
    return;
  }
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  const std::vector<float> weights = deform ? vector_deform_weights(doc, layer, *deform)
                                            : layer_point_bone_weights(doc, layer, armature->active_bone);
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
/* Keep Animation: where the drawing showed on the keyed frames of every action when the stroke began. */
static DrawingsShownAll g_weight_paint_kept;

/* What the brush paints: under a Parent Bone, only a drawing's own deform weights. */
static std::vector<Layer *> weight_tool_layers(Context &C)
{
  std::vector<Layer *> layers = document_weight_layers(*C.document);
  if (!weight_deform(C)) {
    std::erase_if(layers, [&](const Layer *l) { return layer_binding_group(*C.document, l->id) != nullptr; });
  }
  return layers;
}

/* Paints while the left button is down; Ctrl-click picks the bone, Ctrl+Shift-click adds one. */
static void weight_paint_input(Context &C, const ViewMapping &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  const ToolSettings &ts = C.wm->tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  Layer *armature = weight_armature(C);
  const Modifier *deform = weight_deform(C);
  if (!armature && !deform) {
    return;
  }
  if (hovered) {
    dl->AddCircle(io.MousePos, ts.weight_radius, IM_COL32(0xff, 0xff, 0xff, 200), 48, 1.5f);
  }
  if (deform && !deform->deform_custom) {
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      report_banner(ReportType::Warning, "These weights are Automatic: Switch to Custom (at the top) to paint them");
    }
    return;
  }
  if (armature && !deform && hovered && io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (const uint64_t bone = pick_bone(doc, *armature, view, io.MousePos)) {
      weight_bone_pick(*armature, bone, io.KeyShift);
      push_undo(C, "Select Bone");
      return;
    }
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g_weight_painting = true;
    std::vector<uint64_t> ids;
    for (const Layer *l : weight_tool_layers(C)) {
      ids.push_back(l->id);
    }
    g_weight_paint_kept = g_weight_keep_animation ? drawings_shown_capture_all(doc, ids) : DrawingsShownAll();
  }
  if (!g_weight_painting) {
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    g_weight_painting = false;
    drawings_shown_keep_all(doc, g_weight_paint_kept);
    g_weight_paint_kept = DrawingsShownAll();
    push_undo(C, "Weight Paint");
    return;
  }
  if (!deform && !bone_find(*armature, armature->active_bone)) {
    report_banner(ReportType::Warning, "Pick a bone to paint its weights (Ctrl-click a bone)");
    g_weight_painting = false;
    return;
  }
  for (Layer *weighed : weight_tool_layers(C)) {
    Layer &layer = *weighed;
    /* The brush works where points show (posed), in the layer's space. */
    const Transform2D to_document = layer_world_transform(doc, layer.id);
    const Transform2D to_layer = to_document.inverted();
    const std::vector<Transform2D> skins = layer_point_skins(doc, layer);
    const float2 mouse = to_layer.apply(view.to_document(io.MousePos));
    const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
    WeightPaintBrush brush;
    /* Pen pressure scales what its toggles say (a mouse presses fully). */
    const float pen = std::clamp(C.wm->pen_pressure, 0.0f, 1.0f);
    brush.radius = ts.weight_radius * (ts.weight_pressure_radius ? pen : 1.0f) * pixel;
    brush.bone = armature ? armature->active_bone : 0;
    brush.weight = ts.weight_value * (ts.weight_pressure_weight ? pen : 1.0f);
    brush.strength = ts.weight_strength * (ts.weight_pressure_strength ? pen : 1.0f) * std::min(1.0f, io.DeltaTime * 10.0f);
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
    if (deform) {
      vector_deform_paint(doc, layer, *deform, brush);
    }
    else {
      layer_weight_paint(doc, layer, brush);
    }
  }
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
        weight_bone_pick(*armature, bone, io.KeyShift);
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

/* Move Only Bones (the ViewportActionPanel's checkbox for moving bones while posing; kept until the
 * frame or action changes, g_move_only_bones): the drawings the bones bend, and layers following a bone as a whole, are put back
 * where they showed before the bones moved, by keying the change that undoes the bend on this frame.
 * Captured before the move; applied after each (from the captured state, so a drag can redo it). */
struct BoneCounter {
  struct LayerState {
    uint64_t id = 0;
    VectorDrawing drawing;
    /* Where each point showed (layer space), how it was bent, and its handles as shown. */
    std::vector<float2> shown;
    std::vector<Transform2D> skins;
    std::vector<std::array<float2, 2>> handles;
    /* Following a bone as a whole: its world transform then, and its own transform. */
    bool follows = false;
    Transform2D world;
    float2 position, scale;
    float rotation = 0.0f;
  };
  std::vector<LayerState> layers;
  /* The bone layer whose bones move. */
  uint64_t armature = 0;
};

static bool same_transform(const Transform2D &a, const Transform2D &b)
{
  const float eps = 1e-6f;
  return std::abs(a.a - b.a) < eps && std::abs(a.b - b.b) < eps && std::abs(a.c - b.c) < eps &&
         std::abs(a.d - b.d) < eps && std::abs(a.tx - b.tx) < eps && std::abs(a.ty - b.ty) < eps;
}

static BoneCounter bone_counter_capture(Document &doc, uint64_t armature)
{
  BoneCounter counter;
  counter.armature = armature;
  for (const Layer *l : document_layers_flat(doc)) {
    const Layer *owner = layer_armature(doc, l->id);
    if (!owner || owner->id != armature) {
      continue;
    }
    BoneCounter::LayerState st;
    st.id = l->id;
    st.follows = l->followed_bone() != 0;
    st.world = layer_world_transform(doc, l->id);
    st.position = l->position;
    st.rotation = l->rotation;
    st.scale = l->scale;
    if (l->has_drawing() && !l->is_reference()) {
      st.drawing = l->drawing;
      st.skins = layer_point_skins(doc, *l);
      for (size_t i = 0; i < l->drawing.points.size() && i < st.skins.size(); i++) {
        st.shown.push_back(st.skins[i].apply(l->drawing.points[i].pos));
      }
      for (const Curve &curve : l->drawing.curves) {
        for (const CurvePoint &cp : curve.points) {
          const Transform2D &skin = size_t(cp.point) < st.skins.size() ? st.skins[size_t(cp.point)] : Transform2D();
          st.handles.push_back({skin.apply_vector(cp.handle_in), skin.apply_vector(cp.handle_out)});
        }
      }
    }
    counter.layers.push_back(std::move(st));
  }
  return counter;
}

static void bone_counter_apply(Document &doc, const BoneCounter &counter)
{
  /* Dynamic bones swing to somewhere else once the bones they hang from have moved: the bend to undo
   * is the one they have now, worked out for the new pose as it will be keyed (recorded first: an
   * unkeyed bone's new value would otherwise count for every frame before), and no live swing starting
   * from it. */
  animation_record(doc);
  dynamics_update(doc);
  dynamics_live_quiet(doc, counter.armature);
  /* Parents before children (the flat order), so a group put back carries what's inside it. */
  for (const BoneCounter::LayerState &st : counter.layers) {
    Layer *l = layer_find(doc, st.id);
    if (!l) {
      continue;
    }
    if (st.follows) {
      l->position = st.position;
      l->rotation = st.rotation;
      l->scale = st.scale;
      const Transform2D after = layer_world_transform(doc, st.id);
      if (!same_transform(after, st.world)) {
        /* world = parents * local: the local that gives the world it had. */
        const Transform2D local = l->local_transform() * after.inverted() * st.world;
        const float sx = std::sqrt(local.a * local.a + local.b * local.b);
        if (sx > 1e-9f) {
          const float sign = st.scale.x < 0.0f ? -1.0f : 1.0f;
          l->position = {local.tx, local.ty};
          l->rotation = std::atan2(local.b * sign, local.a * sign) * 180.0f / PI;
          l->scale = {sx * sign, (local.a * local.d - local.b * local.c) / (sx * sign)};
        }
      }
    }
    if (st.skins.empty()) {
      continue;
    }
    l->drawing = st.drawing;
    const std::vector<Transform2D> skins = layer_point_skins(doc, *l);
    for (size_t i = 0; i < l->drawing.points.size() && i < skins.size() && i < st.skins.size(); i++) {
      MeshPoint &p = l->drawing.points[i];
      /* Detail rides its curve; points the move doesn't bend stay as they are (no keys). */
      if (p.detail.level > 0 || same_transform(skins[i], st.skins[i])) {
        continue;
      }
      p.pos = skins[i].inverted().apply(st.shown[i]);
    }
    size_t h = 0;
    for (Curve &curve : l->drawing.curves) {
      for (CurvePoint &cp : curve.points) {
        const size_t pi = size_t(cp.point);
        if (cp.handles_set && pi < skins.size() && pi < st.skins.size() && h < st.handles.size() &&
            l->drawing.points[pi].detail.level == 0 && !same_transform(skins[pi], st.skins[pi]))
        {
          const Transform2D undo = skins[pi].inverted();
          cp.handle_in = undo.apply_vector(st.handles[h][0]);
          cp.handle_out = undo.apply_vector(st.handles[h][1]);
        }
        h++;
      }
    }
  }
}

/* Ctrl while turning bones (R, and the Transform Bones tool's turn): the turn lands on the nearest of the
 * reference bone (the active one if picked, else the first picked) pointing a multiple of 45 degrees on
 * screen, its angle when the turn began, and posing its setup angle, each of those with whole turns more or
 * less (so a 360 action bone's full turn is one Ctrl away). */
struct BoneTurnSnap {
  bool valid = false;
  /* The reference bone's direction on screen when the turn began (radians), and which way a turn of its
   * angle turns it there (-1 where the bone layer or its parent is mirrored). */
  float dir0 = 0.0f;
  float sign = 1.0f;
  /* Posing: the turn that brings it back to its setup angle. */
  bool posing = false;
  float to_setup = 0.0f;
};

struct BoneModal {
  /* The mouse with Shift's precision (G, R and S follow this). */
  PreciseMouse precise;
  bool active = false;
  /* Started by pressing on a controller's knob: it ends as the mouse comes up (Blender's tweak). */
  bool tweak = false;
  /* Ctrl on a controller's knob, as it stood when the drag ended. */
  bool knob_snap = false;
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
  /* Ctrl's targets for R (see BoneTurnSnap). */
  BoneTurnSnap turn_snap;
  /* X or Y (0 none, 1 X, 2 Y): G along the document's axis; S along each bone's own axis while
   * posing (frame 0 only resizes, the same every way). */
  int axis = 0;
  /* S S: squash and stretch, longer and thinner (posing). */
  bool squash = false;
  /* Numbers typed while transforming. */
  NumInput num;
  /* Move Only Bones: what the bones bent, to put back (empty: off). */
  BoneCounter counter;
  /* Each update says which layers it changed (`touch`), as a point drag does, instead of every layer of the
   * document being taken as changed (on a big rig, the viewport then worked every drawing out again each frame).
   * Off where it can't know: bone dynamics simulating. */
  bool edits_known = false;
  std::vector<Layer *> touch;
  /* The other bone layers moving with it (sub bone groups inside the active one, bones_moving): their own `armature`,
   * `bones`, `to_armature` and `counter`. */
  struct Also {
    uint64_t armature = 0;
    std::vector<Bone> bones;
    Transform2D to_armature;
    BoneCounter counter;
  };
  std::vector<Also> also;
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
  /* Each bone's head, tail and a step along it, through a group's deform: bones show where what they carry shows. */
  std::vector<float2> ends;
  for (const BonePose &p : poses) {
    /* Along the bone's own axis, so a bone with no length still points somewhere. */
    const float2 ahead = p.head + normalize(p.pose.apply_vector({1.0f, 0.0f})) * 0.01f;
    ends.insert(ends.end(), {p.head, p.tail, ahead});
  }
  ends = layer_group_deform_points(doc, armature, std::move(ends));
  std::vector<ScreenBone> r(armature.bones.size());
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const float along = length(poses[i].pose.apply_vector({1.0f, 0.0f}));
    const float across = length(poses[i].pose.apply_vector({0.0f, 1.0f}));
    const float2 head = ends[i * 3], tail = ends[i * 3 + 1], ahead = ends[i * 3 + 2];
    r[i] = {view.to_screen(to_document.apply(head)), view.to_screen(to_document.apply(tail)),
            along > 1e-9f ? std::clamp(across / along, 0.1f, 10.0f) : 1.0f};
    const ImVec2 d = view.to_screen(to_document.apply(ahead)) - r[i].head;
    const float n = std::sqrt(d.x * d.x + d.y * d.y);
    if (n > 1e-6f) {
      r[i].dir = d / n;
    }
    r[i].moved = Transform2D::translate(to_document.apply_vector(head - poses[i].head)) * to_document * poses[i].skin *
                 from_document;
  }
  return r;
}

static float bone_joint_radius(const ScreenBone &sb)
{
  const ImVec2 d = sb.tail - sb.head;
  /* In proportion to the bone, as in Blender, so a small bone looks small; capped for long ones,
   * with a floor big enough that a short bone's ends still show their colour inside the outline
   * (selected or not). */
  return std::clamp(std::sqrt(d.x * d.x + d.y * d.y) * 0.08f, 3.0f, 6.5f);
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

/* Ctrl lands a picked controller's knob where the poses are: its spots, fix poses, rest, or where its keys cross. */
static void armature_snap_knobs(Layer &armature)
{
  for (Bone &b : armature.bones) {
    if (bone_is_controller(b) && b.selected) {
      armature_knob_snap(armature, b);
    }
  }
}

/* Frames of a controller's box to the screen, through what screen_bones did to its rest point. */
static ImVec2 controller_screen(const Document &doc, const Layer &armature, const ViewMapping &view, const Bone &knob,
                                const BonePose &pose, const ScreenBone &sb, float2 frames)
{
  const Transform2D to_document = layer_world_transform(doc, armature.id);
  return sb.head + view.to_screen(to_document.apply(controller_box_point(knob, pose, frames))) -
         view.to_screen(to_document.apply(pose.head));
}

static constexpr float CONTROLLER_KNOB_RADIUS = 7.0f;
/* Half a spot's square, and how near the mouse picks one. */
static constexpr float CONTROLLER_SPOT_HALF = 3.5f, CONTROLLER_SPOT_PICK = 6.0f;

/* A 2 Axis Controller: its box (the frames of its actions either way from the rest point), a tick on the edge for each
 * of their keys, a dot at each pose a fix made, a square at each spot (named under the mouse), a ring where its
 * actions really are if not at the knob, and the knob. */
static void draw_controller(ImDrawList *dl, const Document &doc, const Layer &armature, const Bone &bone,
                            const BonePose &pose, const ScreenBone &sb, const ViewMapping &view, bool picked, bool active,
                            bool faint, ImU32 fill, ImU32 edge)
{
  float2 lo, hi;
  controller_box(bone, lo, hi);
  const auto at = [&](float2 frames) { return controller_screen(doc, armature, view, bone, pose, sb, frames); };
  const ImVec2 corners[4] = {at({lo.x, lo.y}), at({hi.x, lo.y}), at({hi.x, hi.y}), at({lo.x, hi.y})};
  if (!faint) {
    dl->AddConvexPolyFilled(corners, 4, IM_COL32(0x10, 0x10, 0x10, 60));
  }
  dl->AddPolyline(corners, 4, edge, ImDrawFlags_Closed, active ? 1.6f : 1.2f);
  if (faint) {
    return;
  }
  const ImU32 guide = IM_COL32(0xff, 0xff, 0xff, 70);
  dl->AddLine(at({lo.x, 0.0f}), at({hi.x, 0.0f}), guide);
  dl->AddLine(at({0.0f, lo.y}), at({0.0f, hi.y}), guide);
  const auto tick = [&](ImVec2 from, ImVec2 toward) {
    const ImVec2 d = toward - from;
    const float n = std::sqrt(d.x * d.x + d.y * d.y);
    dl->AddLine(from, n > 1e-3f ? from + d * (5.0f / n) : from, IM_COL32(0xff, 0xff, 0xff, 170), 1.0f);
  };
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    const float sign = controller_side_sign(side);
    for (const int frame : bone.controller_keys[side]) {
      const float v = float(frame) * sign;
      if (controller_side_axis(side) == 0) {
        tick(at({v, lo.y}), at({v, hi.y}));
      }
      else {
        tick(at({lo.x, v}), at({hi.x, v}));
      }
    }
  }
  for (const float2 &p : controller_fix_poses(armature, bone)) {
    dl->AddCircleFilled(at(p), 3.0f, UI_FIX_COLOR);
  }
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  for (const ControllerSpot &spot : bone.controller_spots) {
    const ImVec2 s = at(spot.at);
    const ImVec2 half(CONTROLLER_SPOT_HALF, CONTROLLER_SPOT_HALF);
    dl->AddRect(s - half, s + half, IM_COL32(0xff, 0xff, 0xff, 210), 0.0f, 0, 1.2f);
    if (std::abs(mouse.x - s.x) <= CONTROLLER_SPOT_PICK && std::abs(mouse.y - s.y) <= CONTROLLER_SPOT_PICK) {
      const float font = ImGui::GetFontSize();
      const ImVec2 text = s + ImVec2(CONTROLLER_SPOT_PICK + 2.0f, -font - 2.0f);
      const ImVec2 size = ImGui::CalcTextSize(spot.name.c_str());
      dl->AddRectFilled(text - ImVec2(3.0f, 1.0f), text + size + ImVec2(3.0f, 1.0f), IM_COL32(0x18, 0x18, 0x18, 200), 3.0f);
      dl->AddText(text, IM_COL32(0xff, 0xff, 0xff, 230), spot.name.c_str());
    }
  }
  const float2 knob = controller_knob_shown(doc, armature, bone);
  const ImVec2 knob_at = at(knob);
  if (doc.frame_current != 0 && doc.action.empty()) {
    const ImVec2 playing = at(controller_knob_playing(armature, bone));
    const ImVec2 d = playing - knob_at;
    if (d.x * d.x + d.y * d.y > 9.0f) {
      dl->AddCircle(playing, CONTROLLER_KNOB_RADIUS, IM_COL32(0xff, 0xff, 0xff, 200), 0, 1.5f);
    }
  }
  dl->AddCircleFilled(knob_at, CONTROLLER_KNOB_RADIUS, fill == 0 ? edge : fill);
  dl->AddCircle(knob_at, CONTROLLER_KNOB_RADIUS, edge, 0, active ? 1.6f : 1.2f);
  /* Frame 0 sizes it: its corners are handles. */
  if (doc.frame_current == 0 && picked && (hi.x > lo.x || hi.y > lo.y)) {
    for (const ImVec2 &c : corners) {
      dl->AddRectFilled(c - ImVec2(3.5f, 3.5f), c + ImVec2(3.5f, 3.5f), edge);
    }
  }
  dl->AddText(corners[3] + ImVec2(2.0f, -ImGui::GetFontSize() - 2.0f), IM_COL32(0xff, 0xff, 0xff, 170), bone.name.c_str());
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
    /* A parent in a bone layer around this one, as it is here. */
    const Layer *outer = bone.outer_bone != 0 ? layer_find(doc, bone.outer_layer) : nullptr;
    const Bone *outer_parent = outer ? bone_find(*outer, bone.outer_bone) : nullptr;
    if (!g_relationship_lines || bone.connected || (parent == armature.bones.end() && !outer_parent) ||
        !bone_shown(armature, bone) || (!outer_parent && !bone_shown(armature, *parent)))
    {
      continue;
    }
    const ImVec2 from = outer_parent ? view.to_screen(layer_world_transform(doc, armature.id)
                                                          .apply(bone_outer_frame(bone, doc.frame_current == 0).apply({outer_parent->length, 0.0f})))
                                     : sbs[parent - armature.bones.begin()].tail;
    const ImVec2 to = sbs[i].head;
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
        /* Look At: a faint line from the eye to its target, and its Eye Shape outlined in red, as Limit
         * to Shape's. */
        if (c.type == BoneConstraintType::LookAt && c.enabled) {
          const BonePose &p = poses[i];
          if (const BonePose *tp = bone_pose_find(poses, c.target)) {
            dl->AddLine(view.to_screen(to_document.apply(p.head)), view.to_screen(to_document.apply(tp->head)),
                        IM_COL32(0x60, 0xc0, 0xff, 140), 1.0f);
          }
          if (c.shape_region.size() >= 3) {
            std::vector<ImVec2> line;
            for (const float2 q : c.shape_region) {
              line.push_back(view.to_screen(to_document.apply(q)));
            }
            dl->AddPolyline(line.data(), int(line.size()), IM_COL32(0xe8, 0x20, 0x20, 200), ImDrawFlags_Closed, 1.5f);
          }
        }
        /* Limit to Shape: its shape outlined in red, as Limit Angle's lines. */
        if (c.type == BoneConstraintType::LimitShape && c.enabled && c.shape_region.size() >= 3) {
          std::vector<ImVec2> line;
          for (const float2 p : c.shape_region) {
            line.push_back(view.to_screen(to_document.apply(p)));
          }
          dl->AddPolyline(line.data(), int(line.size()), IM_COL32(0xe8, 0x20, 0x20, 200), ImDrawFlags_Closed, 1.5f);
        }
        if (c.type != BoneConstraintType::LimitAngle || !c.enabled) {
          continue;
        }
        const BonePose *parent = bone_pose_find(poses, bone.parent);
        BonePose outer;
        outer.setup = bone.outer_setup;
        outer.pose = bone.outer_pose;
        parent = !parent && bone.outer_bone != 0 ? &outer : parent;
        /* From the axes its angle is measured in: its parent's, as they rest if it doesn't inherit
         * their turn (Relations). */
        const float base = bone_parent_axes_angle(bone, parent, doc.frame_current == 0);
        const float reach = std::max(distance(poses[i].head, poses[i].tail), 1e-4f);
        for (const float limit : {c.min_angle, c.max_angle}) {
          const float a = base + bone.setup_angle + limit;
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
      if (bone_is_controller(bone)) {
        const std::vector<BonePose> poses = g_keyed_bones ? armature_keyed_pose(armature, doc.frame_current == 0)
                                                          : armature_pose(armature, doc.frame_current == 0);
        draw_controller(dl, doc, armature, bone, poses[i], sbs[i], view, picked, active, faint,
                        active ? col_active : picked ? col_select : col_regular,
                        faint ? col_ghost : active ? col_active : COL_BONE_EDGE);
        continue;
      }
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
  /* Editing a fix: its action bones are held where it was made (turning them changes nothing it plays), so
   * they're outlined in the fix's violet and say so. */
  if (const FixAction *fix = doc.action_layer == armature.id && !doc.action.empty() ? fix_action_find(armature, doc.action)
                                                                                    : nullptr)
  {
    for (const std::string &driver : fix->drivers) {
      const Bone *held = action_bone_of(armature, driver);
      if (!held || !bone_shown(armature, *held)) {
        continue;
      }
      const size_t i = size_t(held - armature.bones.data());
      const ImVec2 head = sbs[i].head, tail = sbs[i].tail;
      const ImVec2 d = tail - head;
      const float len = std::sqrt(d.x * d.x + d.y * d.y);
      const float w = std::clamp(len * 0.1f, 4.0f, 12.0f) + 3.0f;
      const ImVec2 n = len > 0.0f ? ImVec2(-d.y / len, d.x / len) * w : ImVec2(0, 0);
      const ImVec2 mid = head + d * 0.18f;
      const ImVec2 back = len > 0.0f ? d * (-3.0f / len) : ImVec2(0, 0);
      const ImVec2 quad[4] = {head + back, mid + n, tail - back, mid - n};
      dl->AddPolyline(quad, 4, UI_FIX_COLOR, ImDrawFlags_Closed, 2.0f);
      const char *text = "held";
      const ImVec2 at = tail + ImVec2(6.0f, -ImGui::GetFontSize() * 0.5f);
      dl->AddText(at + ImVec2(1, 1), IM_COL32(0, 0, 0, 200), text);
      dl->AddText(at, UI_FIX_COLOR, text);
    }
  }
  /* A dial held at rest while a controller's knob is on the other side's. */
  if (!ghost && doc.frame_current != 0 && doc.action.empty()) {
    for (size_t i = 0; i < armature.bones.size(); i++) {
      const Bone &bone = armature.bones[i];
      if (bone_shown(armature, bone) && controller_dial_held(armature, bone)) {
        const char *text = "held";
        const ImVec2 at = sbs[i].tail + ImVec2(6.0f, -ImGui::GetFontSize() * 0.5f);
        dl->AddText(at + ImVec2(1, 1), IM_COL32(0, 0, 0, 200), text);
        dl->AddText(at, IM_COL32(0x60, 0xd0, 0xd0, 255), text);
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

/* Everything a click at `mouse` hits, the best first: heads and tails within reach, pins and custom
 * shapes, nearest first (a tail before a head at the same spot, so a shared joint is the parent's
 * tip); then bodies near the line, nearest first. A connected bone's head is its parent's tail, so
 * it isn't listed again. Clicking again in the same spot goes down this list. Posing (away from the
 * setup frame, as Blender's pose mode) a bone is picked whole: its ends pick the bone. */
static std::vector<BoneHit> pick_bone_parts(const Document &doc, const Layer &armature, const ViewMapping &view,
                                            ImVec2 mouse, std::vector<std::pair<int, float>> *r_ranks = nullptr)
{
  const std::vector<ScreenBone> sbs = screen_bones(doc, armature, view);
  const float2 m(mouse.x, mouse.y);
  struct Candidate {
    BoneHit hit;
    int phase;
    float d;
    int order;
  };
  std::vector<Candidate> found;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const Bone &bone = armature.bones[i];
    if (!bone_shown(armature, bone)) {
      continue;
    }
    /* A controller answers at its knob, so the box can sit over anything. On the setup frame, where the box is moved,
     * its edges too. */
    if (bone_is_controller(bone)) {
      const std::vector<BonePose> poses = g_keyed_bones ? armature_keyed_pose(armature, doc.frame_current == 0)
                                                        : armature_pose(armature, doc.frame_current == 0);
      const auto at = [&](float2 frames) {
        const ImVec2 s = controller_screen(doc, armature, view, bone, poses[i], sbs[i], frames);
        return float2(s.x, s.y);
      };
      const float d = distance(m, at(controller_knob_shown(doc, armature, bone)));
      if (d <= CONTROLLER_KNOB_RADIUS + 3.0f) {
        found.push_back({{bone.id, BonePart::Body}, 0, d, 0});
      }
      else if (doc.frame_current == 0) {
        float2 lo, hi;
        controller_box(bone, lo, hi);
        const float2 c[4] = {at({lo.x, lo.y}), at({hi.x, lo.y}), at({hi.x, hi.y}), at({lo.x, hi.y})};
        float edge = 1e30f;
        for (int k = 0; k < 4; k++) {
          edge = std::min(edge, distance_to_segment(m, c[k], c[(k + 1) % 4]));
        }
        if (edge < pick_line_pixels() * 1.5f) {
          found.push_back({{bone.id, BonePart::Body}, 1, edge, 0});
        }
      }
      continue;
    }
    /* A pin is one round thing: clicking it picks the whole bone. */
    if (shows_pin(bone)) {
      const float d = distance(m, float2(sbs[i].head.x, sbs[i].head.y));
      if (d <= pin_radius(bone) + 3.0f) {
        found.push_back({{bone.id, BonePart::Body}, 0, d, 0});
      }
      continue;
    }
    /* So is a custom shape: anywhere inside its fills, or near its lines. */
    if (const std::vector<ShapeLoop> loops = custom_shape_loops(doc, bone, sbs[i], view); !loops.empty()) {
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
      if (d <= 4.0f) {
        found.push_back({{bone.id, BonePart::Body}, 0, d, 0});
      }
      continue;
    }
    const float reach = bone_joint_radius(sbs[i]) + 3.0f;
    const bool posing = doc.frame_current != 0;
    const float tail_d = distance(m, float2(sbs[i].tail.x, sbs[i].tail.y));
    if (tail_d <= reach) {
      found.push_back({{bone.id, posing ? BonePart::Body : BonePart::Tail}, 0, tail_d, 0});
    }
    const float head_d = distance(m, float2(sbs[i].head.x, sbs[i].head.y));
    if (head_d <= reach) {
      found.push_back({{bone.id, posing ? BonePart::Body : BonePart::Head}, 0, head_d, 1});
    }
    const float body_d = distance_to_segment(m, {sbs[i].head.x, sbs[i].head.y}, {sbs[i].tail.x, sbs[i].tail.y});
    if (body_d < pick_line_pixels() * 1.5f) {
      found.push_back({{bone.id, BonePart::Body}, 1, body_d, 0});
    }
  }
  std::stable_sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &b) {
    if (a.phase != b.phase) {
      return a.phase < b.phase;
    }
    if (std::abs(a.d - b.d) > 0.01f) {
      return a.d < b.d;
    }
    return a.order < b.order;
  });
  std::vector<BoneHit> hits;
  for (const Candidate &c : found) {
    const Bone *bone = bone_find(armature, c.hit.bone);
    if (c.hit.part == BonePart::Head && bone && bone->connected &&
        std::any_of(hits.begin(), hits.end(), [&](const BoneHit &h) {
          return h.bone == bone->parent && h.part == BonePart::Tail;
        }))
    {
      continue;
    }
    if (std::any_of(hits.begin(), hits.end(),
                    [&](const BoneHit &h) { return h.bone == c.hit.bone && h.part == c.hit.part; }))
    {
      continue;
    }
    hits.push_back(c.hit);
    if (r_ranks) {
      r_ranks->push_back({c.phase, c.d});
    }
  }
  return hits;
}

static BoneHit pick_bone_part(const Document &doc, const Layer &armature, const ViewMapping &view, ImVec2 mouse)
{
  const std::vector<BoneHit> hits = pick_bone_parts(doc, armature, view, mouse);
  return hits.empty() ? BoneHit() : hits.front();
}

/* A click on bones where the last one was (the mouse within a couple of pixels): the next thing
 * under it, round to the first again, as clicking again picks the layer below. */
static struct {
  ImVec2 pos{-1e9f, -1e9f};
  uint64_t layer = 0;
  BoneHit last;
} g_bone_cycle;

/* A hit among a bone layer and the rest of its family (anim/bone_family.hh). */
struct FamilyHit {
  Layer *layer = nullptr;
  BoneHit hit;
};

/* Everything a click at `mouse` hits in `armature` and the rest of its family, the best first, ranked together: a sub
 * bone group's small bone isn't hidden by a long bone of the layer around it running past. At the same rank, `armature`'s
 * own. */
static std::vector<FamilyHit> pick_family_parts(Document &doc, Layer &armature, const ViewMapping &view, ImVec2 mouse)
{
  struct Ranked {
    FamilyHit at;
    std::pair<int, float> rank;
  };
  std::vector<Ranked> found;
  const auto gather = [&](Layer &l) {
    std::vector<std::pair<int, float>> ranks;
    const std::vector<BoneHit> hits = pick_bone_parts(doc, l, view, mouse, &ranks);
    for (size_t i = 0; i < hits.size(); i++) {
      found.push_back({{&l, hits[i]}, ranks[i]});
    }
  };
  gather(armature);
  for (Layer *l : armature_family(doc, armature.id)) {
    if (l->id != armature.id) {
      gather(*l);
    }
  }
  std::stable_sort(found.begin(), found.end(), [](const Ranked &a, const Ranked &b) {
    if (a.rank.first != b.rank.first) {
      return a.rank.first < b.rank.first;
    }
    return std::abs(a.rank.second - b.rank.second) > 0.01f && a.rank.second < b.rank.second;
  });
  std::vector<FamilyHit> hits;
  for (const Ranked &r : found) {
    hits.push_back(r.at);
  }
  return hits;
}

static FamilyHit pick_family_cycling(Document &doc, Layer &armature, const ViewMapping &view, ImVec2 mouse)
{
  const std::vector<FamilyHit> hits = pick_family_parts(doc, armature, view, mouse);
  FamilyHit hit = hits.empty() ? FamilyHit() : hits.front();
  const ImVec2 d = mouse - g_bone_cycle.pos;
  if (!hits.empty() && d.x * d.x + d.y * d.y <= 16.0f) {
    const auto at = std::find_if(hits.begin(), hits.end(), [&](const FamilyHit &h) {
      return h.layer->id == g_bone_cycle.layer && h.hit.bone == g_bone_cycle.last.bone && h.hit.part == g_bone_cycle.last.part;
    });
    if (at != hits.end()) {
      hit = std::next(at) != hits.end() ? *std::next(at) : hits.front();
    }
  }
  g_bone_cycle = {mouse, hit.layer ? hit.layer->id : 0, hit.hit};
  return hit;
}

/* A click that starts again: nothing stays picked in `armature`'s family. */
static void family_deselect_all(Document &doc, const Layer &armature)
{
  for (Layer *l : armature_family(doc, armature.id)) {
    armature_deselect_all(*l);
  }
}

/* A box dragged on `armature`: `select` picks in a bone layer what the box holds. It takes in the bone layers inside
 * `armature` too; unless `extending`, when that's no bones at all, the first of the family around it with bones in the
 * box is worked in instead, so a character's bones all answer boxes as they do clicks. */
static void box_select_family(Document &doc, Layer &armature, bool extending, const std::function<void(Layer &)> &select)
{
  const auto any_picked = [](const Layer &l) { 
    return std::any_of(l.bones.begin(), l.bones.end(), [](const Bone &b) { return b.selected || b.select_head || b.select_tail; });
  };
  bool caught = false;
  for (Layer *l : armature_and_inside(doc, armature.id)) {
    select(*l);
    caught = caught || any_picked(*l);
  }
  if (caught || extending) {
    return;
  }
  for (Layer *l : armature_family(doc, armature.id)) {
    if (l->id == armature.id || layer_inside(doc, l->id, armature.id)) {
      continue;
    }
    select(*l);
    if (any_picked(*l)) {
      doc.active_layer = l->id;
      return;
    }
  }
}

/* The bone under the mouse (screen pixels), or 0. */
static uint64_t pick_bone(const Document &doc, const Layer &armature, const ViewMapping &view, ImVec2 mouse)
{
  return pick_bone_part(doc, armature, view, mouse).bone;
}

/* G, R or S on the selected bones, on any frame. G moves a bone picked by its body or base and
 * aims (and on the setup frame stretches) one picked by its tip; R turns bones about their bases;
 * S scales them. Children come along, as in Moho. */
/* Transform Layer's pivot: where it turns, scales (Alt) and shears (Alt) the active layer about, in the
 * layer's own space. Not saved: it is the layer's origin until dragged, and again for another layer. */
struct LayerPivot {
  uint64_t layer = 0;
  bool set = false;
  float2 local;
};
static LayerPivot g_layer_pivot;

static float2 layer_pivot_local(uint64_t layer)
{
  return g_layer_pivot.set && g_layer_pivot.layer == layer ? g_layer_pivot.local : float2();
}

/* Moves `layer` (by its Location) so its own point `local` is at `target` (document space); `parent` is
 * its parent's space (to the document). */
static void layer_keep_point(Layer &layer, const Transform2D &parent, float2 local, float2 target)
{
  const Transform2D now = parent * layer.local_transform();
  layer.position = layer.position + parent.inverted().apply_vector(target - now.apply(local));
}

/* Sets `layer`'s Location, Scale and Shear to make its local transform `t`, its Rotation kept as it is (so a
 * shear drag leans it without turning it). False, and nothing changed, past 89 degrees of shear. */
static bool layer_set_keeping_rotation(Layer &layer, const Transform2D &t)
{
  const float turn = layer.rotation * PI / 180.0f;
  const float co = std::cos(-turn), si = std::sin(-turn);
  float2 x(co * t.a - si * t.b, si * t.a + co * t.b);
  float2 y(co * t.c - si * t.d, si * t.c + co * t.d);
  float sx = length(x), sy = length(y);
  if (sx < 1e-12f || sy < 1e-12f) {
    return false;
  }
  /* A flipped axis stays flipped: its scale negative, its direction the other way. */
  if (layer.scale.x < 0.0f) {
    x = -x;
    sx = -sx;
  }
  if (layer.scale.y < 0.0f) {
    y = -y;
    sy = -sy;
  }
  const float ax = std::atan2(x.y, x.x) * 180.0f / PI, ay = std::atan2(-y.x, y.y) * 180.0f / PI;
  if (std::fabs(ax) > 89.0f || std::fabs(ay) > 89.0f) {
    return false;
  }
  layer.position = {t.tx, t.ty};
  layer.scale = {sx, sy};
  layer.shear = {ax, ay};
  return true;
}

/* G, R and S on an active Media or text layer move, turn and scale the layer itself (its Location,
 * Rotation and Scale; keyed away from frame 0, as any edit), as Blender does an object: turning and
 * scaling about the layer's origin (a picture's middle, a text's first line's top). X or Y keeps to one axis (G: the document's, S:
 * the layer's own), Ctrl snaps (the grid, 5°, tenths), a typed number sets it. */
struct LayerModal {
  /* The mouse with Shift's precision (G, R and S follow this). */
  PreciseMouse precise;
  bool active = false;
  int area = 0;
  char kind = 'G';
  uint64_t layer = 0;
  Document snapshot;
  float2 position, scale;
  float rotation = 0.0f;
  /* Document -> the layer's parent space (for G). */
  Transform2D to_parent;
  /* What R and S turn and scale about: Transform Layer's pivot (the layer's origin unless moved), in the
   * document and in the layer's own space. */
  float2 pivot, pivot_local;
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
  /* With the Transform Layer tool, G, R and S are on the layer itself, whatever it holds. */
  const bool whole = C.wm->tool_settings.tool == Tool::TransformLayer;
  if (!layer || !(whole || layer->is_media() || layer->is_text()) || layer->locked || kind == 'E') {
    return false;
  }
  LayerModal modal;
  modal.area = area.id;
  modal.kind = kind;
  modal.layer = layer->id;
  const Transform2D world = layer_world_transform(doc, layer->id);
  modal.to_parent = (world * layer->local_transform().inverted()).inverted();
  modal.pivot_local = layer_pivot_local(layer->id);
  modal.pivot = world.apply(modal.pivot_local);
  modal.position = layer->position;
  modal.rotation = layer->rotation;
  modal.scale = layer->scale;
  modal.snapshot = doc;
  modal.start_mouse = mouse_document;
  modal.precise.reset(mouse_document);
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
    *C.document = std::move(g_layer_modal.snapshot);
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
  if (modal_key_take_y(C)) {
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
  const float2 mouse = lm.precise.update(view.to_document(io.MousePos), io.KeyShift);
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
    const float len = C.document->overlays.show ? std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)) : 0.0f;
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
      layer->position = lm.position;
      layer_keep_point(*layer, lm.to_parent.inverted(), lm.pivot_local, lm.pivot);
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
      layer->position = lm.position;
      layer_keep_point(*layer, lm.to_parent.inverted(), lm.pivot_local, lm.pivot);
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
  /* The mouse with Shift's precision (G, R and S follow this). */
  PreciseMouse precise;
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
  float factor = 1.0f;
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
    /* Where the points show (posed by the bones on this frame), as G, R and S measure from. */
    const Posed shown = posed(*C.document, *layer);
    for (int i = 0; i < int(layer->drawing.points.size()); i++) {
      const MeshPoint &p = layer->drawing.points[size_t(i)];
      if (p.selected) {
        modal.items.push_back({layer->id, i, p.radius});
        sum += to_document.apply(shown.drawing->points[size_t(i)].pos);
        count++;
      }
    }
  }
  if (modal.items.empty()) {
    return false;
  }
  /* X mirror editing: the selected points' images widen and thin with them, while dragging too (in the
   * same layer, or a sided layer's partner). They don't count toward the middle. */
  if (C.wm->tool_settings.mirror_x) {
    const size_t picked = modal.items.size();
    const auto add = [&](const Layer &layer, int point) {
      for (const RadiusModal::Item &item : modal.items) {
        if (item.layer == layer.id && item.point == point) {
          return;
        }
      }
      if (point >= 0 && !layer.drawing.points[size_t(point)].detail.hidden) {
        modal.items.push_back({layer.id, point, layer.drawing.points[size_t(point)].radius});
      }
    };
    for (size_t k = 0; k < picked; k++) {
      const RadiusModal::Item item = modal.items[k];
      Layer *layer = layer_find(*C.document, item.layer);
      if (!layer) {
        continue;
      }
      if (Layer *partner = layer_mirror_partner(*C.document, layer->id)) {
        if (partner->type == LayerType::Vector && !partner->locked &&
            !(partner->is_reference() && !partner->reference.edit_points))
        {
          add(*partner, layer_mirror_images(*C.document, *layer, *partner, {item.point})[0]);
        }
      }
      else {
        add(*layer, drawing_find_mirror_point(layer->drawing, item.point, true, false));
      }
    }
  }
  modal.snapshot = *C.document;
  modal.area = area.id;
  modal.pivot = sum / float(count);
  modal.start_mouse = mouse_document;
  modal.precise.reset(mouse_document);
  modal.active = true;
  g_radius_modal = std::move(modal);
  C.wm->modal_running = true;
  C.wm->modal_keys.clear();
  return true;
}

static void radius_modal_end(Context &C, bool confirm)
{
  /* Confirmed, it runs again as mesh.radius_scale from the start, so the ViewportActionPanel can
   * adjust it. */
  const int frame = C.document->frame_current;
  *C.document = std::move(g_radius_modal.snapshot);
  C.document->frame_current = frame;
  const float factor = g_radius_modal.factor;
  g_radius_modal = {};
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
  if (confirm) {
    operator_call(C, "mesh.radius_scale", {{"factor", num_arg(factor)}});
  }
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
  const float2 mouse = rm.precise.update(view.to_document(io.MousePos), io.KeyShift);
  /* Blender's dashed line from the middle to the mouse. */
  const ImVec2 a = view.to_screen(rm.pivot), b = io.MousePos;
  const float len = C.document->overlays.show ? std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)) : 0.0f;
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
  rm.factor = factor;
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

/* Puts back what bone_counter_apply changed, as it was captured. */
static void bone_counter_restore(Document &doc, const BoneCounter &counter)
{
  for (const BoneCounter::LayerState &st : counter.layers) {
    if (Layer *l = layer_find(doc, st.id)) {
      l->position = st.position;
      l->rotation = st.rotation;
      l->scale = st.scale;
      if (!st.skins.empty()) {
        l->drawing = st.drawing;
      }
    }
  }
}

/* The layers a bone drag can change, for one that knows its edits (BoneModal::touch): bone layers (their bones),
 * what action bone actions play into, what bones bend point by point (and every other worked-out drawing: references,
 * look-at warps), driven layers, switches, and what Move Only Bones puts back. Layers moving whole with a bone
 * change nothing of their own: the viewport finds where they are each frame. */
static void bone_modal_find_touched(Document &doc)
{
  BoneModal &bm = g_bone_modal;
  bm.touch.clear();
  bm.edits_known = !dynamics_live_running();
  const LayerIndexScope layer_index(doc);
  const ArmaturePoseCacheScope poses;
  std::set<uint64_t> wanted;
  for (const Driver &d : doc.drivers) {
    wanted.insert(d.target.layer);
  }
  for (const BoneCounter::LayerState &st : bm.counter.layers) {
    wanted.insert(st.id);
  }
  for (const BoneModal::Also &also : bm.also) {
    for (const BoneCounter::LayerState &st : also.counter.layers) {
      wanted.insert(st.id);
    }
  }
  const std::unordered_set<uint64_t> action_bone_layers = animation_action_bone_drag_layers();
  const auto walk = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &l : layers) {
      for (const Bone &b : l.bones) {
        for (const BoneConstraint &c : b.constraints) {
          bm.edits_known = bm.edits_known && c.type != BoneConstraintType::Dynamics;
        }
      }
      if (l.is_bone_layer() || l.is_switch() || wanted.contains(l.id) || action_bone_layers.contains(l.id) ||
          (l.has_drawing() && layer_has_modifier_effect(doc, l, false)))
      {
        bm.touch.push_back(&l);
      }
      self(self, l.children);
    }
  };
  walk(walk, doc.layers);
}

/* `angle_frame`: true when the turn adds to bone angles (R; the parent's frame counts), false when it turns
 * the bone layer's space about the pivot (the Transform Bones tool). */
static BoneTurnSnap bone_turn_snap_begin(const Layer &armature, bool setup_frame, const Transform2D &to_document,
                                         bool angle_frame)
{
  BoneTurnSnap s;
  int ref = -1;
  for (int i = 0; i < int(armature.bones.size()); i++) {
    const Bone &b = armature.bones[size_t(i)];
    if (!b.selected && !b.select_head && !b.select_tail) {
      continue;
    }
    if (ref < 0 || b.id == armature.active_bone) {
      ref = i;
    }
  }
  if (ref < 0) {
    return s;
  }
  const std::vector<BonePose> pose = armature_pose(armature, setup_frame);
  const BonePose &bp = pose[size_t(ref)];
  const float2 dir = to_document.apply_vector(bp.tail - bp.head);
  if (length(dir) < 1e-9f) {
    return s;
  }
  const auto det = [](const Transform2D &t) { return t.a * t.d - t.b * t.c; };
  float sign = det(to_document) < 0.0f ? -1.0f : 1.0f;
  const Bone &bone = armature.bones[size_t(ref)];
  if (angle_frame && bone.parent) {
    for (size_t i = 0; i < armature.bones.size(); i++) {
      if (armature.bones[i].id == bone.parent && det(pose[i].pose) < 0.0f) {
        sign = -sign;
      }
    }
  }
  s.valid = true;
  s.dir0 = std::atan2(dir.y, dir.x);
  s.sign = sign;
  s.posing = !setup_frame;
  s.to_setup = bone.setup_angle - bone.angle;
  return s;
}

/* The snapped turn for `turned` (radians), and what it snapped to (for the header). */
static float bone_turn_snapped(const BoneTurnSnap &s, float turned, const char **r_what)
{
  const float full = 2.0f * PI, step = PI / 4.0f;
  const auto whole_turns = [&](float base) { return base + std::round((turned - base) / full) * full; };
  float best = turned;
  float best_off = std::numeric_limits<float>::max();
  const auto offer = [&](float t, const char *what) {
    if (std::fabs(t - turned) < best_off - 1e-5f) {
      best_off = std::fabs(t - turned);
      best = t;
      *r_what = what;
    }
  };
  /* The starting and setup angles first: on a tie with a 45 degree direction they win. */
  offer(whole_turns(0.0f), "start");
  if (s.posing) {
    offer(whole_turns(s.to_setup), "setup");
  }
  if (s.valid) {
    const float dir = s.dir0 + s.sign * turned;
    offer((std::round(dir / step) * step - s.dir0) * s.sign, "45\xC2\xB0");
  }
  else {
    offer(std::round(turned / step) * step, "45\xC2\xB0");
  }
  return best;
}

float viewport_bone_turn_snap(Document &doc, float degrees, bool rotate_key, std::string *r_what)
{
  const Layer *armature = document_active_layer(doc);
  if (!armature || !armature->is_bone_layer()) {
    return degrees;
  }
  const BoneTurnSnap s = bone_turn_snap_begin(*armature, doc.frame_current == 0,
                                              layer_world_transform(doc, armature->id), rotate_key);
  const char *what = "";
  const float snapped = bone_turn_snapped(s, degrees * PI / 180.0f, &what) * 180.0f / PI;
  *r_what = what;
  return snapped;
}

/* The bone layer's space to the document's right at the picked bones, as they show: through a group's deform, so a drag
 * moves them as far as they show moved. `r_middle`: the middle of their heads. */
static Transform2D picked_bones_shown(const Document &doc, const Layer &armature, bool setup_frame,
                                      float2 *r_middle = nullptr)
{
  float2 sum;
  const std::vector<float2> heads = armature_picked_heads(armature, setup_frame);
  for (const float2 &h : heads) {
    sum += h;
  }
  const float2 middle = sum / float(std::max<size_t>(heads.size(), 1));
  if (r_middle) {
    *r_middle = middle;
  }
  return layer_world_transform(doc, armature.id) * layer_group_deform_at(doc, armature, middle);
}

/* Whether something picked carries bone layer `inner` whole, in the bone layers up to `outer`: it comes along then, as a
 * picked bone's child does, and moving its own bones as well would count twice. */
static bool carried_by_picked(Document &doc, const Layer &inner, uint64_t outer)
{
  for (const Layer *l = &inner; l && l->id != outer; l = layer_parent(doc, l->id)) {
    const Layer *rig = l->followed_bone() != 0 ? layer_armature(doc, l->id) : nullptr;
    for (const Bone *b = rig ? bone_find(*rig, l->followed_bone()) : nullptr; b; b = bone_find(*rig, b->parent)) {
      if (b->selected || b->select_head || b->select_tail) {
        return true;
      }
    }
  }
  return false;
}

/* The bone layers G, R and S move working in `armature`: it and the bone layers of its family inside it that have bones
 * picked (armature_and_inside), its own first. */
static std::vector<Layer *> bones_moving(Document &doc, const Layer &armature)
{
  std::vector<Layer *> moving;
  for (Layer *l : armature_and_inside(doc, armature.id)) {
    if (armature_any_picked(*l) && !carried_by_picked(doc, *l, armature.id)) {
      moving.push_back(l);
    }
  }
  bones_mark_carried(doc, moving);
  return moving;
}

static bool bone_modal_start(Context &C, Area &area, char kind, float2 mouse_document)
{
  Document &doc = *C.document;
  Layer *active = document_active_layer(doc);
  if (!active || !active->is_bone_layer() || kind == 'E') {
    return false;
  }
  const std::vector<Layer *> moving = bones_moving(doc, *active);
  if (moving.empty()) {
    return false;
  }
  Layer *armature = moving.front();
  BoneModal modal;
  modal.area = area.id;
  modal.kind = kind;
  modal.armature = armature->id;
  modal.setup_frame = doc.frame_current == 0;
  const Transform2D to_document = layer_world_transform(doc, armature->id);
  float2 middle;
  const Transform2D shown = picked_bones_shown(doc, *armature, modal.setup_frame, &middle);
  modal.to_armature = shown.inverted();
  modal.bones = armature->bones;
  /* The middle of everything moving. */
  float2 heads_sum = shown.apply(middle) * float(armature_picked_heads(*armature, modal.setup_frame).size());
  size_t heads = armature_picked_heads(*armature, modal.setup_frame).size();
  for (size_t i = 1; i < moving.size(); i++) {
    BoneModal::Also also;
    also.armature = moving[i]->id;
    also.bones = moving[i]->bones;
    float2 also_middle;
    const Transform2D also_shown = picked_bones_shown(doc, *moving[i], modal.setup_frame, &also_middle);
    also.to_armature = also_shown.inverted();
    const size_t count = armature_picked_heads(*moving[i], modal.setup_frame).size();
    heads_sum += also_shown.apply(also_middle) * float(count);
    heads += count;
    if (!modal.setup_frame && move_only_bones_on(doc)) {
      also.counter = bone_counter_capture(doc, moving[i]->id);
    }
    modal.also.push_back(std::move(also));
  }
  modal.pivot = heads_sum / float(std::max<size_t>(heads, 1));
  if (kind == 'R') {
    modal.turn_snap = bone_turn_snap_begin(*armature, modal.setup_frame, to_document, true);
  }
  modal.snapshot = doc;
  if (!modal.setup_frame && move_only_bones_on(doc)) {
    modal.counter = bone_counter_capture(doc, armature->id);
  }
  modal.start_mouse = mouse_document;
  modal.precise.reset(mouse_document);
  modal.last_mouse = mouse_document;
  modal.active = true;
  g_bone_modal = std::move(modal);
  /* Action bone actions follow every update: the layers they play into found once, now. */
  animation_action_bone_drag_begin(doc);
  bone_modal_find_touched(doc);
  C.wm->modal_running = true;
  C.wm->modal_keys.clear();
  return true;
}

static void bone_modal_end(Context &C, bool confirm)
{
  animation_action_bone_drag_end();
  if (confirm) {
    push_undo(C, g_bone_modal.kind == 'R' ? "Rotate Bones" : g_bone_modal.kind == 'S' ? "Scale Bones" : "Move Bones");
  }
  else {
    const int frame = C.document->frame_current;
    *C.document = std::move(g_bone_modal.snapshot);
    C.document->frame_current = frame;
  }
  g_bone_modal.active = false;
  g_bone_modal.snapshot = Document();
  g_bone_modal.bones.clear();
  g_bone_modal.counter = {};
  g_bone_modal.also.clear();
  g_bone_modal.touch.clear();
  g_bone_modal.edits_known = false;
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
}

/* A confirmed G, R or S on bones: the document as it was, then Move, Rotate or Scale Bones with the
 * amounts the drag ended on (and Move Only Bones as last set), as one undo step the
 * ViewportActionPanel can adjust. */
static void bone_modal_confirm(Context &C, float2 delta, float degrees, float factor)
{
  /* Taken, not copied: the snapshot is a whole document (on a big rig, copying it twice was most of the pause as
   * the drag ended). */
  animation_action_bone_drag_end();
  BoneModal bm = std::move(g_bone_modal);
  const int frame = C.document->frame_current;
  *C.document = std::move(bm.snapshot);
  C.document->frame_current = frame;
  g_bone_modal.active = false;
  g_bone_modal.snapshot = Document();
  g_bone_modal.bones.clear();
  g_bone_modal.counter = {};
  g_bone_modal.also.clear();
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
  OpArgs args{{"only_bones", move_only_bones_on(*C.document) ? "1" : "0"}, {"posing", bm.setup_frame ? "0" : "1"},
              {"keep_poses", keep_poses_on(*C.document) ? "1" : "0"},
              {"without_children", move_without_children_on(*C.document) ? "1" : "0"}};
  if (bm.kind == 'G') {
    args["move_x"] = num_arg(delta.x);
    args["move_y"] = num_arg(delta.y);
    args["knob_snap"] = bm.knob_snap ? "1" : "0";
    operator_call(C, "armature.translate", args);
  }
  else if (bm.kind == 'R') {
    args["angle"] = num_arg(degrees);
    operator_call(C, "armature.rotate", args);
  }
  else {
    args["scale"] = num_arg(factor);
    args["axis"] = std::to_string(bm.axis);
    args["squash"] = bm.squash ? "1" : "0";
    operator_call(C, "armature.resize", args);
  }
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
                       modal_key_take(C, "SPACE", true) ||
                       (g_bone_modal.tweak ? !ImGui::IsMouseDown(ImGuiMouseButton_Left)
                                           : ImGui::IsMouseClicked(ImGuiMouseButton_Left));
  /* Keys while transforming, as in Blender: X or Y for an axis (again: none), S again (while
   * scaling) for squash and stretch, and a typed number. */
  BoneModal &bm = g_bone_modal;
  if (modal_key_take(C, "X")) {
    bm.axis = bm.axis == 1 ? 0 : 1;
    bm.squash = false;
  }
  if (modal_key_take_y(C)) {
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
  const float2 mouse = bm.precise.update(view.to_document(io.MousePos), io.KeyShift);
  const bool snap = io.KeyCtrl;
  char text[160];
  float2 delta;
  float degrees = 0.0f, factor = 1.0f;
  /* The same for the other bone layers moving with it. */
  const auto for_also = [&](auto &&fn) {
    for (BoneModal::Also &also : bm.also) {
      if (Layer *l = layer_find(*C.document, also.armature)) {
        fn(*l, also);
      }
    }
  };
  if (g_bone_modal.kind == 'G') {
    delta = mouse - g_bone_modal.start_mouse;
    /* A controller's knob snaps to its actions' keys instead (below), which aren't on any grid. */
    const bool knobs = !bm.setup_frame && std::any_of(armature->bones.begin(), armature->bones.end(), [](const Bone &b) {
      return bone_is_controller(b) && b.selected;
    });
    if (snap && !knobs) {
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
    for_also([&](Layer &l, BoneModal::Also &also) {
      armature_grab_selected(l, also.bones, also.to_armature.apply_vector(delta), bm.setup_frame, C.wm->tool_settings.auto_ik);
    });
    bm.knob_snap = snap && !bm.setup_frame;
    if (bm.knob_snap) {
      armature_snap_knobs(*armature);
    }
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
    const char *snapped = nullptr;
    degrees = (snap ? bone_turn_snapped(g_bone_modal.turn_snap, g_bone_modal.turned, &snapped) : g_bone_modal.turned) *
              180.0f / PI;
    if (num.any()) {
      degrees = num.value(0, 0.0f);
      snapped = nullptr;
    }
    armature_rotate_selected(*armature, g_bone_modal.bones, degrees * PI / 180.0f, g_bone_modal.setup_frame);
    for_also([&](Layer &l, BoneModal::Also &also) {
      armature_rotate_selected(l, also.bones, degrees * PI / 180.0f, bm.setup_frame);
    });
    std::snprintf(text, sizeof(text), "Rotate Bones  %s\xC2\xB0%s%s", num.shown(0, degrees, "%.1f").c_str(),
                  snapped ? "  snapped to " : "", snapped ? snapped : "");
  }
  else {
    const float start = std::max(distance(g_bone_modal.start_mouse, g_bone_modal.pivot), 1e-6f);
    factor = distance(mouse, g_bone_modal.pivot) / start;
    if (snap) {
      factor = std::round(factor * 10.0f) / 10.0f;
    }
    if (num.any()) {
      factor = num.value(0, 1.0f);
    }
    armature_scale_selected(*armature, g_bone_modal.bones, factor, bm.setup_frame, bm.axis, bm.squash);
    for_also([&](Layer &l, BoneModal::Also &also) {
      armature_scale_selected(l, also.bones, factor, bm.setup_frame, bm.axis, bm.squash);
    });
    std::snprintf(text, sizeof(text), "%s  %s%s",
                  bm.setup_frame ? "Resize Bones" : bm.squash ? "Squash & Stretch" : "Scale Bones",
                  num.shown(0, factor, "%.3f").c_str(),
                  bm.axis == 1   ? "  along the bones' X (thickness)"
                  : bm.axis == 2 ? "  along the bones' Y (length)"
                                 : "");
  }
  if (!g_bone_modal.setup_frame && move_without_children_on(*C.document)) {
    armature_keep_children(*armature, g_bone_modal.bones, false);
    for_also([](Layer &l, BoneModal::Also &also) { armature_keep_children(l, also.bones, false); });
  }
  if (C.wm->tool_settings.mirror_x) {
    armature_mirror_edits(*armature, g_bone_modal.bones, g_bone_modal.setup_frame);
    for_also([&](Layer &l, BoneModal::Also &also) { armature_mirror_edits(l, also.bones, bm.setup_frame); });
  }
  /* Each update starts the bones again from how they were, which undoes what action bone actions did to
   * the bones they drive: play them again from the bones as they are now, so an action bone being
   * turned moves its bones (and their points) as it turns. */
  animation_refresh_action_bones(*C.document);
  if (!g_bone_modal.counter.layers.empty()) {
    bone_counter_apply(*C.document, g_bone_modal.counter);
  }
  for_also([&](Layer &, BoneModal::Also &also) {
    if (!also.counter.layers.empty()) {
      bone_counter_apply(*C.document, also.counter);
    }
  });
  if (g_bone_modal.edits_known) {
    for (Layer *l : g_bone_modal.touch) {
      layer_touch(*l);
    }
  }
  if (confirm) {
    bone_modal_confirm(C, delta, degrees, factor);
  }
  else {
    C.wm->modal_keys.clear();
  }
  return text;
}

/* A corner of a controller being dragged on the setup frame (its signs along X and Y). */
static struct {
  bool active = false;
  uint64_t layer = 0;
  uint64_t bone = 0;
  float2 corner;
} g_controller_resize;

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
 * The box gizmos' shared parts (Transform Points, Transform Bones and Transform Layer), after Affinity's,
 * Animate's and Harmony's: one look (a line in the mode's colour over a dark edge, white square handles that
 * light up under the mouse, the pivot a crosshair with a grey dot where it began), hit areas about twice what
 * is drawn, a readout by the cursor while dragging, and the cage Distort and Warp bend.
 */

/* Half a handle's side as drawn, and how near (pixels) counts as on a handle, an edge or the pivot. */
static constexpr float GIZMO_HANDLE = 4.5f;
static constexpr float GIZMO_HIT = 10.0f;
static constexpr float GIZMO_EDGE_HIT = 6.0f;
static constexpr float GIZMO_PIVOT_HIT = 9.0f;

/* The box's colour says its mode: Transform blue, Distort orange, Warp purple. */
static ImU32 gizmo_accent(int mode)
{
  switch (mode) {
    case 1:
      return IM_COL32(0xff, 0x9a, 0x3c, 255);
    case 2:
      return IM_COL32(0xb4, 0x7c, 0xff, 255);
    default:
      return IM_COL32(0x4f, 0xa3, 0xff, 255);
  }
}

static const char *gizmo_mode_name(int mode)
{
  return mode == 1 ? "Distort" : mode == 2 ? "Warp" : "Transform";
}

/* A line in the accent over a dark edge, so it shows on light and dark drawings alike. */
static void gizmo_draw_lines(ImDrawList *dl, const ImVec2 *pts, int n, bool closed, ImU32 accent, float width = 1.5f)
{
  dl->AddPolyline(pts, n, IM_COL32(0, 0, 0, 130), closed ? ImDrawFlags_Closed : ImDrawFlags_None, width + 2.0f);
  dl->AddPolyline(pts, n, accent, closed ? ImDrawFlags_Closed : ImDrawFlags_None, width);
}

static void gizmo_draw_handle(ImDrawList *dl, ImVec2 p, ImU32 accent, bool lit, float half = GIZMO_HANDLE)
{
  const ImVec2 r(half, half), edge(half + 1.5f, half + 1.5f);
  dl->AddRectFilled(p - edge, p + edge, IM_COL32(0, 0, 0, 140), 2.0f);
  dl->AddRectFilled(p - r, p + r, lit ? accent : IM_COL32(0xff, 0xff, 0xff, 255), 1.0f);
  dl->AddRect(p - r, p + r, accent, 1.0f, 0, 1.2f);
}

/* The pivot: a ring with a crosshair; while dragging, a grey dot where it was. */
static void gizmo_draw_pivot(ImDrawList *dl, ImVec2 p, ImU32 accent, bool lit, const ImVec2 *was)
{
  if (was && (std::fabs(was->x - p.x) > 1.0f || std::fabs(was->y - p.y) > 1.0f)) {
    dl->AddCircleFilled(*was, 4.0f, IM_COL32(0, 0, 0, 120), 12);
    dl->AddCircleFilled(*was, 3.0f, IM_COL32(0xa0, 0xa0, 0xa0, 230), 12);
  }
  const float r = 6.0f;
  if (lit) {
    dl->AddCircleFilled(p, r, (accent & 0x00ffffff) | 0x60000000, 20);
  }
  for (const bool dark : {true, false}) {
    const ImU32 col = dark ? IM_COL32(0, 0, 0, 150) : accent;
    const float w = dark ? 3.0f : 1.5f;
    dl->AddCircle(p, r, col, 20, w);
    dl->AddLine(p - ImVec2(r + 5.0f, 0), p - ImVec2(2.0f, 0), col, w);
    dl->AddLine(p + ImVec2(2.0f, 0), p + ImVec2(r + 5.0f, 0), col, w);
    dl->AddLine(p - ImVec2(0, r + 5.0f), p - ImVec2(0, 2.0f), col, w);
    dl->AddLine(p + ImVec2(0, 2.0f), p + ImVec2(0, r + 5.0f), col, w);
  }
}

/* A short arc just outside a corner, `out` pointing away from the box: where a drag turns it. */
static void gizmo_draw_turn_mark(ImDrawList *dl, ImVec2 corner, ImVec2 out, ImU32 accent)
{
  const float len = std::sqrt(out.x * out.x + out.y * out.y);
  if (len < 1e-6f) {
    return;
  }
  const float a = std::atan2(out.y, out.x);
  for (const bool dark : {true, false}) {
    dl->PathArcTo(corner, 13.0f, a - 0.55f, a + 0.55f, 10);
    dl->PathStroke(dark ? IM_COL32(0, 0, 0, 120) : ((accent & 0x00ffffff) | 0xb0000000), ImDrawFlags_None,
                   dark ? 3.5f : 1.5f);
  }
}

/* A small sign beside the cursor for what a press does where ImGui has no cursor of its own: 'R' turns, 'K'
 * shears (`along_x`: sideways). */
static void gizmo_cursor_sign(ImDrawList *dl, ImVec2 mouse, char kind, bool along_x = true)
{
  const ImVec2 c = mouse + ImVec2(16.0f, -14.0f);
  for (const bool dark : {true, false}) {
    const ImU32 col = dark ? IM_COL32(0, 0, 0, 200) : IM_COL32(0xff, 0xff, 0xff, 255);
    const float w = dark ? 3.5f : 1.5f;
    if (kind == 'R') {
      dl->PathArcTo(c, 6.0f, -PI * 0.9f, PI * 0.4f, 12);
      dl->PathStroke(col, ImDrawFlags_None, w);
      const ImVec2 tip = c + ImVec2(6.0f * std::cos(PI * 0.4f), 6.0f * std::sin(PI * 0.4f));
      dl->AddLine(tip, tip + ImVec2(-4.0f, 0.5f), col, w);
      dl->AddLine(tip, tip + ImVec2(0.5f, -4.0f), col, w);
    }
    else {
      /* Two arrows pointing opposite ways, one over the other (or side by side). */
      const ImVec2 u = along_x ? ImVec2(1, 0) : ImVec2(0, 1), v = along_x ? ImVec2(0, 1) : ImVec2(1, 0);
      for (const float side : {-1.0f, 1.0f}) {
        const ImVec2 from = c + v * (3.5f * side) - u * (6.0f * side), to = c + v * (3.5f * side) + u * (6.0f * side);
        dl->AddLine(from, to, col, w);
        dl->AddLine(to, to - u * (3.0f * side) + v * 2.5f, col, w);
        dl->AddLine(to, to - u * (3.0f * side) - v * 2.5f, col, w);
      }
    }
  }
}

/* The numbers a drag is making, by the cursor. */
static void gizmo_readout(ImDrawList *dl, ImVec2 mouse, const char *text)
{
  if (!text || !*text) {
    return;
  }
  const ImVec2 size = ImGui::CalcTextSize(text);
  const ImVec2 at = mouse + ImVec2(18.0f, 18.0f);
  dl->AddRectFilled(at - ImVec2(5, 3), at + size + ImVec2(5, 3), IM_COL32(0x18, 0x18, 0x18, 215), 4.0f);
  dl->AddText(at, IM_COL32(0xf0, 0xf0, 0xf0, 255), text);
}

/* The edge (from q[k] to q[k + 1]) the mouse is on, away from the handles at its ends and middle; -1 for none. */
static int gizmo_edge_hit(const ImVec2 q[4], ImVec2 mouse)
{
  const auto close_to = [&](ImVec2 p) { return std::fabs(p.x - mouse.x) < GIZMO_HIT && std::fabs(p.y - mouse.y) < GIZMO_HIT; };
  int best = -1;
  float best_d = GIZMO_EDGE_HIT;
  for (int k = 0; k < 4; k++) {
    const ImVec2 a = q[k], b = q[(k + 1) % 4];
    if (close_to(a) || close_to(b) || close_to((a + b) * 0.5f)) {
      continue;
    }
    const float d = distance_to_segment({mouse.x, mouse.y}, {a.x, a.y}, {b.x, b.y});
    if (d <= best_d) {
      best_d = d;
      best = k;
    }
  }
  return best;
}

static bool point_in_polygon(const std::vector<ImVec2> &poly, ImVec2 p)
{
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const ImVec2 a = poly[i], b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      in = !in;
    }
  }
  return in;
}

/* The Distort and Warp cage: a grid of `cells` each way laid over the box lo..hi, its nodes moved. A place
 * in the box goes where the grid takes it, by Catmull-Rom curves through the nodes (each node is on the
 * bent surface, and an even grid changes nothing; a one-cell grid is a plain four-corner distort). Past the
 * edges it carries on straight. */
struct GizmoCage {
  float2 lo, hi;
  int cells = 1;
  /* (cells + 1)^2 nodes, row by row from the bottom, each row left to right. */
  std::vector<float2> nodes;

  int side() const { return cells + 1; }
  float2 rest(int i, int j) const
  {
    return {lo.x + (hi.x - lo.x) * float(i) / float(cells), lo.y + (hi.y - lo.y) * float(j) / float(cells)};
  }
  void reset(float2 box_lo, float2 box_hi, int n)
  {
    lo = box_lo;
    hi = box_hi;
    cells = std::clamp(n, 1, 8);
    nodes.clear();
    for (int j = 0; j <= cells; j++) {
      for (int i = 0; i <= cells; i++) {
        nodes.push_back(rest(i, j));
      }
    }
  }
  bool moved() const
  {
    for (int j = 0; j <= cells; j++) {
      for (int i = 0; i <= cells; i++) {
        if (distance(nodes[size_t(j * side() + i)], rest(i, j)) > 1e-7f) {
          return true;
        }
      }
    }
    return false;
  }
  /* The grid's place for a box place: (0..cells, 0..cells) across the box. */
  float2 param(float2 p) const
  {
    const float w = std::max(hi.x - lo.x, 1e-12f), h = std::max(hi.y - lo.y, 1e-12f);
    return {(p.x - lo.x) / w * float(cells), (p.y - lo.y) / h * float(cells)};
  }
  static float2 curve(const float2 *v, int n, float u)
  {
    if (u <= 0.0f) {
      return v[0] + (v[1] - v[0]) * u;
    }
    if (u >= float(n)) {
      return v[n] + (v[n] - v[n - 1]) * (u - float(n));
    }
    const int k = std::min(int(std::floor(u)), n - 1);
    const float t = u - float(k);
    const float2 p1 = v[k], p2 = v[k + 1];
    const float2 p0 = k > 0 ? v[k - 1] : p1 * 2.0f - p2;
    const float2 p3 = k + 2 <= n ? v[k + 2] : p2 * 2.0f - p1;
    const float t2 = t * t, t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
           0.5f;
  }
  float2 at_param(float2 uv) const
  {
    std::vector<float2> column(static_cast<size_t>(side()));
    for (int j = 0; j <= cells; j++) {
      column[size_t(j)] = curve(&nodes[size_t(j * side())], cells, uv.x);
    }
    return curve(column.data(), cells, uv.y);
  }
  float2 map(float2 p) const { return at_param(param(p)); }
  /* The box place the cage takes to `p` (Newton's method from where it would be unbent). */
  float2 unmap(float2 p) const
  {
    float2 q = p;
    for (int it = 0; it < 24; it++) {
      const float2 f = map(q) - p;
      if (length(f) < 1e-7f) {
        break;
      }
      const float e = std::max(hi.x - lo.x, hi.y - lo.y) * 1e-3f + 1e-9f;
      const float2 dx = (map(q + float2(e, 0)) - map(q - float2(e, 0))) / (2.0f * e);
      const float2 dy = (map(q + float2(0, e)) - map(q - float2(0, e))) / (2.0f * e);
      const float det = dx.x * dy.y - dy.x * dx.y;
      if (std::fabs(det) < 1e-12f) {
        break;
      }
      q = q - float2((dy.y * f.x - dy.x * f.y) / det, (-dx.y * f.x + dx.x * f.y) / det);
    }
    return q;
  }
  /* How much node k moves map(p): the map is linear in its nodes, so this is one number per node. */
  float weight(int k, float2 p) const
  {
    GizmoCage bumped = *this;
    bumped.nodes[size_t(k)].x += 1.0f;
    return bumped.map(p).x - map(p).x;
  }
  /* Drags the place `grab` (box space) by `d`: the four nodes round it move, each by its share, so the
   * place goes exactly with the mouse. */
  void drag_place(const GizmoCage &start, float2 grab, float2 d)
  {
    nodes = start.nodes;
    const float2 uv = param(grab);
    const int i0 = std::clamp(int(std::floor(uv.x)), 0, cells - 1), j0 = std::clamp(int(std::floor(uv.y)), 0, cells - 1);
    const int ks[4] = {j0 * side() + i0, j0 * side() + i0 + 1, (j0 + 1) * side() + i0, (j0 + 1) * side() + i0 + 1};
    float w[4], sum = 0.0f;
    for (int n = 0; n < 4; n++) {
      w[n] = start.weight(ks[n], grab);
      sum += w[n] * w[n];
    }
    if (sum < 1e-12f) {
      return;
    }
    for (int n = 0; n < 4; n++) {
      nodes[size_t(ks[n])] = start.nodes[size_t(ks[n])] + d * (w[n] / sum);
    }
  }
  /* Its outline, on screen, for drawing and for what is inside. */
  template<typename F> std::vector<ImVec2> outline(F &&screen, int steps = 8) const
  {
    std::vector<ImVec2> pts;
    const float n = float(cells);
    const float2 ends[4] = {{0, 0}, {n, 0}, {n, n}, {0, n}};
    for (int k = 0; k < 4; k++) {
      for (int s = 0; s < cells * steps; s++) {
        const float t = float(s) / float(cells * steps);
        pts.push_back(screen(at_param(ends[k] + (ends[(k + 1) % 4] - ends[k]) * t)));
      }
    }
    return pts;
  }
};

/* The cage as an operator's arguments, and back. */
static void cage_args(const GizmoCage &cage, OpArgs &args, float box_angle = 0.0f)
{
  if (box_angle != 0.0f) {
    args["box_angle"] = num_arg(box_angle * 180.0f / PI);
  }
  args["lo_x"] = num_arg(cage.lo.x);
  args["lo_y"] = num_arg(cage.lo.y);
  args["hi_x"] = num_arg(cage.hi.x);
  args["hi_y"] = num_arg(cage.hi.y);
  args["cells"] = std::to_string(cage.cells);
  std::string nodes;
  char buf[40];
  for (const float2 &p : cage.nodes) {
    std::snprintf(buf, sizeof(buf), "%s%.9g %.9g", nodes.empty() ? "" : " ", p.x, p.y);
    nodes += buf;
  }
  args["nodes"] = nodes;
}

static bool cage_from_args(Context &C, GizmoCage &cage)
{
  cage.reset({op_arg_float(C, "lo_x", 0.0f), op_arg_float(C, "lo_y", 0.0f)},
             {op_arg_float(C, "hi_x", 1.0f), op_arg_float(C, "hi_y", 1.0f)}, op_arg_int(C, "cells", 1));
  if (!(cage.hi.x > cage.lo.x) || !(cage.hi.y > cage.lo.y)) {
    report(ReportType::Error, "The cage's box needs hi_x > lo_x and hi_y > lo_y");
    return false;
  }
  const std::string text = op_arg(C, "nodes", "");
  if (text.empty()) {
    return true;
  }
  std::vector<float> v;
  const char *s = text.c_str();
  char *end = nullptr;
  for (float f = std::strtof(s, &end); end != s; f = std::strtof(s, &end)) {
    v.push_back(f);
    s = end;
  }
  if (v.size() != cage.nodes.size() * 2) {
    report(ReportType::Error, "nodes= needs (cells + 1)^2 places, x y each, row by row from the bottom");
    return false;
  }
  for (size_t k = 0; k < cage.nodes.size(); k++) {
    cage.nodes[k] = {v[k * 2], v[k * 2 + 1]};
  }
  return true;
}

/* A Distort or Warp kept between drags (as Photoshop's Warp is, until it is applied): the drags each make
 * an undo step that bends the drawing from how it was before the first, so the cage keeps its shape. Gone
 * once anything else changes the document, the selection, the frame, or the mode. */
struct CageSession {
  bool active = false;
  GizmoCage cage;
  Document base;
  uint64_t changes = 0, layer = 0, selection = 0;
  int frame = 0, mode = 0, cells = 0;

  bool still(const Context &C, uint64_t at_layer, uint64_t at_selection) const
  {
    const ToolSettings &ts = C.wm->tool_settings;
    return active && layer == at_layer && selection == at_selection && frame == C.document->frame_current &&
           mode == ts.gizmo_mode && cells == (mode == 2 ? ts.warp_cells : 1) && C.undo &&
           changes == C.undo->change_count();
  }
  void keep(const Context &C, uint64_t at_layer, uint64_t at_selection, const GizmoCage &bent)
  {
    cage = bent;
    changes = C.undo ? C.undo->change_count() : 0;
    layer = at_layer;
    selection = at_selection;
    frame = C.document->frame_current;
    mode = C.wm->tool_settings.gizmo_mode;
    cells = mode == 2 ? C.wm->tool_settings.warp_cells : 1;
    active = true;
  }
};

/* A kept cage's selection key: the selection and the box's turn (a turned box is another cage). */
static uint64_t gizmo_key(uint64_t selection, float box_angle)
{
  uint32_t bits;
  std::memcpy(&bits, &box_angle, sizeof(bits));
  return selection ^ (uint64_t(bits) * 0x9e3779b97f4a7c15ull);
}

/* The box's turning stem: from the middle of its top side (its own +y) out to a square knob, as Harmony's and
 * Affinity's have. Dragging the knob turns the box's axes, not what is in it, so scaling, shearing and the
 * cage then work along them. */
template<typename F> static void gizmo_orient_knob(F &&screen, float2 lo, float2 hi, ImVec2 &r_base, ImVec2 &r_knob)
{
  const float2 c = (lo + hi) * 0.5f;
  const ImVec2 top = screen(float2(c.x, hi.y)), further = screen(float2(c.x, hi.y + std::max(hi.y - lo.y, 1e-6f)));
  ImVec2 dir = further - top;
  const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
  dir = len > 1e-6f ? dir / len : ImVec2(0.0f, -1.0f);
  r_base = top + dir * (GIZMO_HANDLE + 2.0f);
  r_knob = top + dir * 28.0f;
}

/* The mode switch, as the header shows it and the right-click menu: Transform, Distort, Warp (and Warp's grid). */
static void gizmo_mode_buttons(Context &C, const char *id)
{
  ToolSettings &ts = C.wm->tool_settings;
  ImGui::PushID(id);
  for (int m = 0; m < 3; m++) {
    if (m > 0) {
      ImGui::SameLine(0, 1);
    }
    const bool on = ts.gizmo_mode == m;
    if (on) {
      const ImVec4 c = ImGui::ColorConvertU32ToFloat4(gizmo_accent(m));
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x * 0.55f, c.y * 0.55f, c.z * 0.55f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 0.65f, c.y * 0.65f, c.z * 0.65f, 1.0f));
    }
    if (ImGui::Button(gizmo_mode_name(m))) {
      ts.gizmo_mode = m;
    }
    if (on) {
      ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", m == 0   ? "Move, turn, scale and shear (drag an edge); Ctrl-drag a corner to distort it"
                              : m == 1 ? "Drag each corner on its own (a side moves its two corners)"
                                       : "A grid over the selection: drag a node or anywhere inside to bend it");
    }
  }
  if (ts.gizmo_mode == 2) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 3.0f);
    ui_slider_int("##cells", &ts.warp_cells, 1, 8, "Grid %d");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How many cells the Warp grid has each way");
    }
  }
  ImGui::PopID();
}

/* Draws the cage (Distort's quad or Warp's grid) and its nodes; `hot` the node lit, -1 none. */
template<typename F>
static void gizmo_draw_cage(ImDrawList *dl, const GizmoCage &cage, F &&screen, ImU32 accent, int hot, bool sides)
{
  const int n = cage.cells, steps = 10;
  std::vector<ImVec2> line;
  for (int j = 1; j < n; j++) {
    for (const bool across : {true, false}) {
      line.clear();
      for (int s = 0; s <= n * steps; s++) {
        const float t = float(s) / float(steps);
        line.push_back(screen(cage.at_param(across ? float2(t, float(j)) : float2(float(j), t))));
      }
      dl->AddPolyline(line.data(), int(line.size()), (accent & 0x00ffffff) | 0xa0000000, ImDrawFlags_None, 1.0f);
    }
  }
  const std::vector<ImVec2> rim = cage.outline(screen, steps);
  gizmo_draw_lines(dl, rim.data(), int(rim.size()), true, accent);
  for (int k = 0; k < int(cage.nodes.size()); k++) {
    gizmo_draw_handle(dl, screen(cage.nodes[size_t(k)]), accent, k == hot, n > 1 ? 3.5f : GIZMO_HANDLE);
  }
  if (sides) {
    /* Distort's side handles: the middles of its edges (each moves its two corners). */
    const float2 mids[4] = {{0, 0.5f}, {1, 0.5f}, {0.5f, 1}, {0.5f, 0}};
    for (int k = 0; k < 4; k++) {
      gizmo_draw_handle(dl, screen(cage.at_param(mids[k])), accent, hot == 100 + k, 3.5f);
    }
  }
}

/* What a press on the cage takes: node k (>= 0), a Distort side 100-103 (left, right, top, bottom), -2 the
 * inside, -1 nothing. */
template<typename F> static int gizmo_cage_hit(const GizmoCage &cage, F &&screen, ImVec2 mouse, bool sides)
{
  const auto on = [&](float2 p) {
    const ImVec2 s = screen(p);
    return std::fabs(s.x - mouse.x) < GIZMO_HIT && std::fabs(s.y - mouse.y) < GIZMO_HIT;
  };
  for (int k = 0; k < int(cage.nodes.size()); k++) {
    if (on(cage.nodes[size_t(k)])) {
      return k;
    }
  }
  if (sides) {
    const float2 mids[4] = {{0, 0.5f}, {1, 0.5f}, {0.5f, 1}, {0.5f, 0}};
    for (int k = 0; k < 4; k++) {
      if (on(cage.at_param(mids[k]))) {
        return 100 + k;
      }
    }
  }
  return point_in_polygon(cage.outline(screen, 6), mouse) ? -2 : -1;
}

/* The cage after dragging what `hit` took by `d` (box space) from `start`; `grab` is where the inside was
 * pressed (box space, unbent). */
static GizmoCage gizmo_cage_dragged(const GizmoCage &start, int hit, float2 grab, float2 d)
{
  GizmoCage cage = start;
  if (hit >= 100) {
    /* A Distort side: its two corners (0 bottom left, 1 bottom right, 2 top left, 3 top right). */
    static const int ends[4][2] = {{0, 2}, {1, 3}, {2, 3}, {0, 1}};
    for (const int k : ends[hit - 100]) {
      cage.nodes[size_t(k)] = start.nodes[size_t(k)] + d;
    }
  }
  else if (hit >= 0) {
    cage.nodes[size_t(hit)] = start.nodes[size_t(hit)] + d;
  }
  else if (hit == -2) {
    cage.drag_place(start, grab, d);
  }
  return cage;
}

/* ---------------------------------------------------------------------------------
 * Transform Bones tool: a box gizmo around the selected bones, after Moho's
 * sz_transform_bones_gizmo. Inside the box moves, the band around it turns about the pivot, the
 * eight handles scale (from the opposite side; Alt from the pivot; Shift keeps a corner's
 * proportions), and the pivot can be dragged (Ctrl snaps it to a bone end). Outside, a click picks a
 * bone and a drag box-selects (Shift adds, Alt removes). Unselected children stay where they are.
 */

enum class GizmoMode { None, Move, Rotate, Scale, Shear, Cage, Pivot, Orient, Select };

/* Whether a box drag is changing what it holds (not picking, nor placing the pivot). */
static bool gizmo_moving(GizmoMode mode)
{
  return mode == GizmoMode::Move || mode == GizmoMode::Rotate || mode == GizmoMode::Scale || mode == GizmoMode::Shear ||
         mode == GizmoMode::Cage;
}

/* What the drag is doing, as the header's fields show it while it goes. */
struct GizmoLive {
  float2 offset;
  float2 scale = {1.0f, 1.0f};
  float degrees = 0.0f;
  float shear = 0.0f;
};

struct BoneGizmo {
  GizmoMode mode = GizmoMode::None;
  /* Move Only Bones while dragging (empty: off). */
  BoneCounter counter;
  /* Scale handle: 0-3 corners (bottom-left, top-left, top-right, bottom-right), 4-7 sides (left,
   * right, top, bottom). For a Cage drag, what of the cage (see gizmo_cage_hit). */
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
  /* Ctrl's targets while turning (see BoneTurnSnap). */
  BoneTurnSnap turn_snap;
  bool changed = false;
  /* The pivot while dragging (bone layer space). */
  float2 live_pivot;
  GizmoLive live;
  /* The drag's map so far (gizmo space), which the box is drawn through while it goes. */
  Transform2D live_map;
  /* How far the box is turned from the bone layer's axes (radians; see gizmo_orient_knob): the gizmo's space
   * is the bone layer's turned that much. For turning it: where it was, the mouse's angle round the box's
   * middle and that middle (bone layer space), and the pivot (document space) and its offset at the press. */
  float box_angle = 0.0f;
  /* The part of box_angle Rotate drags gave it (the box turns with what it turns), kept only while the
   * document is as this gizmo's last drag left it (`turned_changes`): an undo squares it again. */
  float box_turned = 0.0f;
  uint64_t turned_changes = 0;
  float orient_start = 0.0f, orient_ref = 0.0f;
  float2 orient_centre, orient_pivot, orient_offset;
  /* Keep Poses for a Distort or Warp on the setup frame (empty: off). */
  SetupSnapshot kept;
  /* The document as the press found it (a cancel puts it back), and a Cage drag's cage then and now, and
   * where its inside was pressed (box space, unbent). The press puts back a kept cage's starting document,
   * so the bones are taken from it a frame later (`capture`). */
  Document before;
  GizmoCage cage_start, cage_now;
  float2 grab;
  bool capture = false;
  CageSession session;
};
static BoneGizmo g_gizmo;


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

/* A corner handle's node in a one-cell cage (0 bottom left, 1 bottom right, 2 top left, 3 top right). */
static int gizmo_corner_node(int handle)
{
  static const int node[4] = {0, 2, 3, 1};
  return node[handle];
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

static Transform2D map_about(float2 pivot, float2 offset, float2 scale, float angle)
{
  return Transform2D::translate(pivot + offset) * Transform2D::rotate(angle) * Transform2D::scaling(scale) *
         Transform2D::translate(-pivot);
}

/* The header's fields and buttons: Transform Bones (armature.transform) about `pivot`, one undo step. */
static void bone_gizmo_apply(Context &C, float2 pivot, float2 offset, float2 scale, float degrees)
{
  operator_call(C, "armature.transform",
                {{"origin_x", num_arg(pivot.x)}, {"origin_y", num_arg(pivot.y)}, {"move_x", num_arg(offset.x)},
                 {"move_y", num_arg(offset.y)}, {"scale_x", num_arg(scale.x)}, {"scale_y", num_arg(scale.y)},
                 {"angle", num_arg(degrees)}, {"only_bones", move_only_bones_on(*C.document) ? "1" : "0"},
                 {"posing", C.document->frame_current != 0 ? "1" : "0"},
                 {"keep_poses", keep_poses_on(*C.document) ? "1" : "0"}});
}

/* The picked bones' ends' bounds in the bone layer's space turned by `turn` (radians), at least 0.01 across, as
 * armature_picked_bounds. */
static bool bone_gizmo_bounds(const Layer &armature, bool setup, float turn, float2 &lo, float2 &hi)
{
  if (turn == 0.0f) {
    return armature_picked_bounds(armature, setup, lo, hi);
  }
  const Transform2D unturn = Transform2D::rotate(-turn);
  const std::vector<BonePose> poses = armature_keyed_pose(armature, setup);
  bool any = false;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const Bone &b = armature.bones[i];
    if (!(b.selected || b.select_head || b.select_tail)) {
      continue;
    }
    for (const float2 e : {poses[i].head, poses[i].tail}) {
      const float2 q = unturn.apply(e);
      lo = any ? float2(std::min(lo.x, q.x), std::min(lo.y, q.y)) : q;
      hi = any ? float2(std::max(hi.x, q.x), std::max(hi.y, q.y)) : q;
      any = true;
    }
  }
  if (any) {
    const float2 c = (lo + hi) * 0.5f;
    if (hi.x - lo.x < 0.01f) {
      lo.x = c.x - 0.005f;
      hi.x = c.x + 0.005f;
    }
    if (hi.y - lo.y < 0.01f) {
      lo.y = c.y - 0.005f;
      hi.y = c.y + 0.005f;
    }
  }
  return any;
}

/* A cage (bone layer space, turned by box_angle) on the picked bones: armature.cage_transform, one undo step
 * named `name`. */
static void bone_gizmo_cage(Context &C, const GizmoCage &cage, const char *name, float box_angle = 0.0f)
{
  OpArgs args = {{"only_bones", move_only_bones_on(*C.document) ? "1" : "0"},
                 {"posing", C.document->frame_current != 0 ? "1" : "0"},
                 {"keep_poses", keep_poses_on(*C.document) ? "1" : "0"},
                 {"name", name}};
  cage_args(cage, args, box_angle);
  operator_call(C, "armature.cage_transform", args);
}

/* The header's typed fields, shared by Transform Points and Transform Bones: Position (the pivot's place),
 * Scale, Angle and Shear (degrees, x slants sideways, y up and down), with what a drag is doing shown in
 * them while it goes. Calls `apply` with what was typed. */
static void gizmo_header_fields(const char *id, bool any, float2 pivot, bool dragging, const GizmoLive &live,
                                const std::function<void(float2 move, float2 scale, float degrees)> &apply,
                                const std::function<void(float2 shear)> &shear)
{
  const float w = ImGui::GetFrameHeight() * 3.2f;
  ImGui::PushID(id);
  ImGui::BeginDisabled(!any);
  ImGui::TextUnformatted("Position");
  ImGui::SameLine();
  const float2 at = dragging ? pivot + live.offset : pivot;
  float pos[2] = {any ? at.x : 0.0f, any ? at.y : 0.0f};
  ImGui::SetNextItemWidth(w * 2.0f);
  if (ImGui::InputFloat2("##pos", pos, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) && any) {
    apply(float2(pos[0], pos[1]) - pivot, {1.0f, 1.0f}, 0.0f);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Where the pivot is: type a place to move the selection there");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Scale");
  ImGui::SameLine();
  float scale[2] = {dragging ? live.scale.x : 1.0f, dragging ? live.scale.y : 1.0f};
  ImGui::SetNextItemWidth(w * 2.0f);
  if (ImGui::InputFloat2("##scale", scale, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue) && any) {
    apply({}, {scale[0], scale[1]}, 0.0f);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Angle");
  ImGui::SameLine();
  float angle = dragging ? live.degrees : 0.0f;
  ImGui::SetNextItemWidth(w);
  if (ImGui::InputFloat("##angle", &angle, 0.0f, 0.0f, "%.1f\xC2\xB0", ImGuiInputTextFlags_EnterReturnsTrue) && any) {
    apply({}, {1.0f, 1.0f}, angle);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Shear");
  ImGui::SameLine();
  float slant[2] = {dragging ? live.shear : 0.0f, 0.0f};
  ImGui::SetNextItemWidth(w * 1.6f);
  if (ImGui::InputFloat2("##shear", slant, "%.1f\xC2\xB0", ImGuiInputTextFlags_EnterReturnsTrue) && any &&
      (slant[0] != 0.0f || slant[1] != 0.0f))
  {
    shear({std::clamp(slant[0], -89.0f, 89.0f), std::clamp(slant[1], -89.0f, 89.0f)});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Slant the selection about the pivot: x leans it sideways, y up and down (degrees)");
  }
  ImGui::SameLine();
  if (ImGui::Button("Flip H")) {
    apply({}, {-1.0f, 1.0f}, 0.0f);
  }
  ImGui::SameLine(0, 1);
  if (ImGui::Button("Flip V")) {
    apply({}, {1.0f, -1.0f}, 0.0f);
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

/* A typed shear about `pivot` as a one-cell cage round lo..hi: x leans the box sideways, y up and down. */
static GizmoCage cage_typed_shear(float2 lo, float2 hi, float2 pivot, float2 degrees)
{
  GizmoCage cage;
  cage.reset(lo, hi, 1);
  const float kx = std::tan(degrees.x * PI / 180.0f), ky = std::tan(degrees.y * PI / 180.0f);
  for (float2 &p : cage.nodes) {
    const float2 d = p - pivot;
    p = pivot + float2(d.x + kx * d.y, d.y + ky * d.x);
  }
  return cage;
}

static void transform_bones_header(Context &C)
{
  Document &doc = *C.document;
  Layer *armature = document_active_layer(doc);
  if (!armature || !armature->is_bone_layer()) {
    return;
  }
  gizmo_mode_buttons(C, "bones_mode");
  ImGui::SameLine();
  const BoneGizmo &g = g_gizmo;
  const bool setup = doc.frame_current == 0;
  const Transform2D turn = Transform2D::rotate(g.box_angle);
  float2 lo, hi, rlo, rhi;
  const bool any = armature_picked_bounds(*armature, setup, lo, hi) &&
                   bone_gizmo_bounds(*armature, setup, g.box_angle, rlo, rhi);
  /* The pivot is kept in the (maybe turned) box's space; the fields are in the bone layer's. */
  const float2 pivot = any ? turn.apply((rlo + rhi) * 0.5f + g.pivot_offset) : float2();
  const bool dragging = gizmo_moving(g.mode);
  GizmoLive live = g.live;
  live.offset = turn.apply_vector(live.offset);
  gizmo_header_fields(
      "bones", any, dragging ? turn.apply(g.pivot) : pivot, dragging, live,
      [&](float2 move, float2 scale, float degrees) { bone_gizmo_apply(C, pivot, move, scale, degrees); },
      [&](float2 shear) { bone_gizmo_cage(C, cage_typed_shear(lo, hi, pivot, shear), "Shear Bones"); });
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

/* What a press on the box would do. Transform: the pivot, a handle (scale; Ctrl: a corner distorts, a side
 * shears), an edge between handles (shear), inside (move), the band round it (turn), else select. Distort
 * and Warp: the cage's nodes and inside (Distort's sides too, and its inside moves). `handle` gets the
 * handle (0-7) or, for a Cage drag, the part of the cage. */
static GizmoMode gizmo_hit(int mode, const GizmoCage &cage, const Transform2D &to_document, const ViewMapping &view,
                           ImVec2 mouse, float2 mouse_gizmo, float2 lo, float2 hi, float2 pivot, bool ctrl, int *handle)
{
  const auto screen = [&](float2 p) { return view.to_screen(to_document.apply(p)); };
  {
    ImVec2 base, knob;
    gizmo_orient_knob(screen, mode != 0 ? cage.lo : lo, mode != 0 ? cage.hi : hi, base, knob);
    if (std::fabs(knob.x - mouse.x) < GIZMO_HIT && std::fabs(knob.y - mouse.y) < GIZMO_HIT) {
      return GizmoMode::Orient;
    }
  }
  if (mode != 0) {
    const int hit = gizmo_cage_hit(cage, screen, mouse, mode == 1);
    *handle = hit;
    if (hit == -1) {
      return GizmoMode::Select;
    }
    return mode == 1 && hit == -2 ? GizmoMode::Move : GizmoMode::Cage;
  }
  const float band = gizmo_box(to_document, view, lo, hi);
  const auto over = [&](float2 p, float reach) {
    const ImVec2 s = screen(p);
    return std::abs(s.x - mouse.x) < reach && std::abs(s.y - mouse.y) < reach;
  };
  if (over(pivot, GIZMO_PIVOT_HIT)) {
    return GizmoMode::Pivot;
  }
  const std::array<float2, 8> handles = gizmo_handles(lo, hi);
  for (int i = 0; i < 8; i++) {
    if (over(handles[i], GIZMO_HIT)) {
      if (ctrl && i < 4) {
        *handle = gizmo_corner_node(i);
        return GizmoMode::Cage;
      }
      *handle = i;
      return ctrl ? GizmoMode::Shear : GizmoMode::Scale;
    }
  }
  /* The edges, in the order gizmo_edge_hit counts them: left, top, right, bottom. */
  const ImVec2 q[4] = {screen(lo), screen({lo.x, hi.y}), screen(hi), screen({hi.x, lo.y})};
  static const int side[4] = {4, 6, 5, 7};
  if (const int e = gizmo_edge_hit(q, mouse); e >= 0) {
    *handle = side[e];
    return GizmoMode::Shear;
  }
  const float2 v = mouse_gizmo;
  if (v.x < lo.x - band || v.x > hi.x + band || v.y < lo.y - band || v.y > hi.y + band) {
    return GizmoMode::Select;
  }
  if (v.x < lo.x || v.x > hi.x || v.y < lo.y || v.y > hi.y) {
    return GizmoMode::Rotate;
  }
  return GizmoMode::Move;
}

/* A box drag's map (gizmo space) for Move, Rotate, Scale and Shear, from where it was pressed (`start`, the
 * box lo0..hi0 and `pivot`) to `mouse`; `snap` is Ctrl's snapping target for a move (or null). Sets
 * `live` (what the header and the readout show), `live_pivot`, and for Scale and Shear `origin`; true when
 * it is a shear (`r_map` then has it). */
static Transform2D gizmo_box_map(GizmoMode mode, int handle, float2 lo0, float2 hi0, float2 pivot, float2 start,
                                 float2 mouse, float turned, const float2 *snap, float2 &live_pivot, GizmoLive &live,
                                 float2 &origin)
{
  const ImGuiIO &io = ImGui::GetIO();
  live = {};
  origin = pivot;
  if (mode == GizmoMode::Move) {
    float2 offset = mouse - start;
    if (io.KeyShift) {
      (std::abs(offset.x) > std::abs(offset.y) ? offset.y : offset.x) = 0.0f;
    }
    if (snap) {
      offset = *snap - pivot;
    }
    live.offset = offset;
    live_pivot = pivot + offset;
    return map_about(pivot, offset, {1, 1}, 0.0f);
  }
  if (mode == GizmoMode::Rotate) {
    float angle = turned;
    if (io.KeyShift) {
      const float step = PI / 4.0f;
      angle = std::round(angle / step) * step;
    }
    live.degrees = angle * 180.0f / PI;
    live_pivot = pivot;
    return map_about(pivot, {}, {1, 1}, angle);
  }
  const float2 c0 = (lo0 + hi0) * 0.5f;
  static const float2 opposite[8] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}};
  /* The anchor is the opposite corner or side (x/y: 0 = low, 1 = high, -1 = the middle); Alt the pivot. */
  const float2 o = opposite[handle];
  float2 anchor(o.x < 0 ? c0.x : (o.x > 0 ? hi0.x : lo0.x), o.y < 0 ? c0.y : (o.y > 0 ? hi0.y : lo0.y));
  if (io.KeyAlt) {
    anchor = pivot;
  }
  origin = anchor;
  const float2 p0 = start - anchor, p1 = mouse - anchor;
  if (mode == GizmoMode::Shear) {
    /* Top or bottom: slides along x, by how far from the anchor's line; left or right: along y. Shift keeps
     * to steps of 15 degrees. */
    const bool along_x = handle >= 6;
    float k = along_x ? (std::abs(p0.y) > 1e-9f ? (mouse.x - start.x) / p0.y : 0.0f)
                      : (std::abs(p0.x) > 1e-9f ? (mouse.y - start.y) / p0.x : 0.0f);
    float degrees = std::atan(k) * 180.0f / PI;
    if (io.KeyShift) {
      degrees = std::round(degrees / 15.0f) * 15.0f;
    }
    degrees = std::clamp(degrees, -85.0f, 85.0f);
    k = std::tan(degrees * PI / 180.0f);
    live.shear = degrees;
    const Transform2D shear = along_x ? Transform2D{1.0f, 0.0f, k, 1.0f, -k * anchor.y, 0.0f}
                                      : Transform2D{1.0f, k, 0.0f, 1.0f, 0.0f, -k * anchor.x};
    live_pivot = shear.apply(pivot);
    return shear;
  }
  float2 scale(1.0f, 1.0f);
  if (std::abs(p0.x) > 1e-9f) {
    scale.x = p1.x / p0.x;
  }
  if (std::abs(p0.y) > 1e-9f) {
    scale.y = p1.y / p0.y;
  }
  if (handle == 4 || handle == 5) {
    scale.y = 1.0f;
  }
  if (handle == 6 || handle == 7) {
    scale.x = 1.0f;
  }
  if (io.KeyShift && handle < 4) {
    scale.x = scale.y = (scale.x + scale.y) * 0.5f;
  }
  live.scale = scale;
  const float2 rel = pivot - anchor;
  live_pivot = anchor + float2(rel.x * scale.x, rel.y * scale.y);
  return map_about(anchor, {}, scale, 0.0f);
}

/* The readout by the cursor for a drag. */
static std::string gizmo_live_text(GizmoMode mode, int box_mode, const GizmoLive &live)
{
  char text[96] = "";
  switch (mode) {
    case GizmoMode::Move:
      std::snprintf(text, sizeof(text), "X %.3f  Y %.3f", live.offset.x, live.offset.y);
      break;
    case GizmoMode::Rotate:
      std::snprintf(text, sizeof(text), "%.1f\xC2\xB0", live.degrees);
      break;
    case GizmoMode::Scale:
      std::snprintf(text, sizeof(text), "%.0f%% \xC3\x97 %.0f%%", live.scale.x * 100.0f, live.scale.y * 100.0f);
      break;
    case GizmoMode::Shear:
      std::snprintf(text, sizeof(text), "Shear %.1f\xC2\xB0", live.shear);
      break;
    case GizmoMode::Cage:
      std::snprintf(text, sizeof(text), "%s", box_mode == 2 ? "Warp" : "Distort");
      break;
    case GizmoMode::Orient:
      std::snprintf(text, sizeof(text), "Box %.1f°", live.degrees);
      break;
    default:
      break;
  }
  return text;
}

/* The cursor and its sign for what a press would do. */
static void gizmo_hover_cursor(ImDrawList *dl, GizmoMode mode, int handle)
{
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  switch (mode) {
    case GizmoMode::Move:
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      break;
    case GizmoMode::Rotate:
    case GizmoMode::Orient:
      gizmo_cursor_sign(dl, mouse, 'R');
      break;
    case GizmoMode::Shear:
      gizmo_cursor_sign(dl, mouse, 'K', handle >= 6);
      break;
    case GizmoMode::Cage:
    case GizmoMode::Pivot:
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

/* Draws the box gizmo (gizmo space through `to_document`): Transform's box with its handles, turn marks
 * and pivot, or Distort's and Warp's cage; `hot`/`hot_handle` what the mouse is on (or is dragging). */
static void gizmo_draw_box(ImDrawList *dl, const ViewMapping &view, const Transform2D &to_document, int mode,
                           const GizmoCage &cage, float2 lo, float2 hi, float2 pivot, const ImVec2 *pivot_was,
                           GizmoMode hot, int hot_handle, bool dragging)
{
  const auto screen = [&](float2 p) { return view.to_screen(to_document.apply(p)); };
  const ImU32 accent = gizmo_accent(mode);
  if (!dragging || hot == GizmoMode::Orient) {
    ImVec2 base, knob;
    gizmo_orient_knob(screen, mode != 0 ? cage.lo : lo, mode != 0 ? cage.hi : hi, base, knob);
    const ImVec2 stem[2] = {base, knob};
    gizmo_draw_lines(dl, stem, 2, false, accent, 1.2f);
    gizmo_draw_handle(dl, knob, accent, hot == GizmoMode::Orient);
  }
  if (mode != 0) {
    gizmo_draw_cage(dl, cage, screen, accent, hot == GizmoMode::Cage ? hot_handle : -1, mode == 1);
    return;
  }
  const ImVec2 q[4] = {screen(lo), screen({lo.x, hi.y}), screen(hi), screen({hi.x, lo.y})};
  gizmo_draw_lines(dl, q, 4, true, accent);
  if (hot == GizmoMode::Shear && hot_handle >= 4) {
    /* The edge a shear slides, brighter. */
    static const int edge[4][2] = {{0, 1}, {3, 2}, {1, 2}, {0, 3}};
    const ImVec2 e[2] = {q[edge[hot_handle - 4][0]], q[edge[hot_handle - 4][1]]};
    gizmo_draw_lines(dl, e, 2, false, accent, 3.0f);
  }
  if (!dragging) {
    const ImVec2 mid = (q[0] + q[2]) * 0.5f;
    for (const ImVec2 &corner : q) {
      gizmo_draw_turn_mark(dl, corner, corner - mid, accent);
    }
  }
  const std::array<float2, 8> handles = gizmo_handles(lo, hi);
  for (int i = 0; i < 8; i++) {
    const bool lit = (hot == GizmoMode::Scale || hot == GizmoMode::Shear) && hot_handle == i;
    gizmo_draw_handle(dl, screen(handles[i]), accent, lit);
  }
  gizmo_draw_pivot(dl, screen(pivot), accent, hot == GizmoMode::Pivot, pivot_was);
}

/* The right-click menu on a box gizmo: its mode, Warp's grid, and putting the pivot back. True when the
 * pivot is to go back. */
static bool gizmo_context_menu(Context &C, const char *id, bool open, bool pivot_moved)
{
  if (open) {
    ImGui::OpenPopup(id);
  }
  bool reset = false;
  if (ImGui::BeginPopup(id)) {
    ToolSettings &ts = C.wm->tool_settings;
    for (int m = 0; m < 3; m++) {
      if (ImGui::MenuItem(gizmo_mode_name(m), nullptr, ts.gizmo_mode == m)) {
        ts.gizmo_mode = m;
      }
    }
    if (ts.gizmo_mode == 2) {
      ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 5.0f);
      ui_slider_int("Grid", &ts.warp_cells, 1, 8);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Pivot and Box", nullptr, false, pivot_moved)) {
      reset = true;
    }
    ImGui::EndPopup();
  }
  return reset;
}

static void transform_bones_input(Context &C, Area &area, Layer &armature, const ViewMapping &view, bool hovered,
                                  ImDrawList *dl)
{
  Document &doc = *C.document;
  const ImGuiIO &io = ImGui::GetIO();
  ToolSettings &ts = C.wm->tool_settings;
  BoneGizmo &g = g_gizmo;
  const bool setup = doc.frame_current == 0;
  const int count = picked_bone_count(armature);
  const uint64_t selection = document_selection_hash(doc);
  if (g.armature != armature.id || g.last_count != count) {
    if (g.mode == GizmoMode::None) {
      g.pivot_offset = {};
      g.box_angle = 0.0f;
      g.box_turned = 0.0f;
    }
    g.armature = armature.id;
    g.last_count = count;
  }
  if (g.box_turned != 0.0f && g.mode == GizmoMode::None && C.undo && C.undo->change_count() != g.turned_changes) {
    g.box_angle -= g.box_turned;
    g.box_turned = 0.0f;
    g.pivot_offset = {};
  }
  /* The gizmo's space: the bone layer's, turned as far as the box is (`turn`: gizmo -> bone layer space). */
  const Transform2D base_space = layer_world_transform(doc, armature.id);
  const Transform2D turn = Transform2D::rotate(g.box_angle), unturn = turn.inverted();
  const Transform2D to_document = base_space * turn;
  const float2 mouse = to_document.inverted().apply(view.to_document(io.MousePos));
  /* Ctrl's snapping onto bone ends, in the gizmo's space. */
  const auto nearest_end = [&](float2 p, bool skip_picked, float2 &out) {
    float2 found;
    if (!nearest_bone_end(armature, setup, turn.apply(p), skip_picked, found)) {
      return false;
    }
    out = unturn.apply(found);
    return true;
  };
  float2 lo, hi;
  /* With the overlays off (Shift Z) the box is gone: not drawn, and clicks select. */
  const bool any = doc.overlays.show && bone_gizmo_bounds(armature, setup, g.box_angle, lo, hi);
  const float2 pivot = (lo + hi) * 0.5f + g.pivot_offset;
  if (any) {
    gizmo_box(to_document, view, lo, hi);
  }
  /* The cage Distort and Warp show: the one kept from the last drag, or a fresh one on the box. */
  GizmoCage cage;
  const bool kept_cage = g.session.still(C, armature.id, gizmo_key(selection, g.box_angle));
  if (g.mode == GizmoMode::Cage) {
    cage = g.cage_now;
  }
  else if (kept_cage) {
    cage = g.session.cage;
  }
  else {
    g.session.active = false;
    cage.reset(lo, hi, ts.gizmo_mode == 2 ? ts.warp_cells : 1);
  }

  /* Press: what the mouse is on decides the drag. A double-click on the pivot puts it back. */
  if (hovered && g.mode == GizmoMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    int handle = 0;
    const GizmoMode mode = any ? gizmo_hit(ts.gizmo_mode, cage, to_document, view, io.MousePos, mouse, lo, hi, pivot,
                                           io.KeyCtrl, &handle)
                               : GizmoMode::Select;
    const bool twice = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (mode == GizmoMode::Pivot && twice) {
      g.pivot_offset = {};
    }
    else if (mode == GizmoMode::Orient && twice) {
      /* Square with the bone layer again. */
      g.box_angle = 0.0f;
      g.box_turned = 0.0f;
      g.pivot_offset = {};
    }
    else {
      if (mode == GizmoMode::Orient) {
        const float2 mb = base_space.inverted().apply(view.to_document(io.MousePos));
        g.orient_centre = turn.apply((lo + hi) * 0.5f);
        g.orient_ref = std::atan2(mb.y - g.orient_centre.y, mb.x - g.orient_centre.x);
        g.orient_start = g.box_angle;
        g.orient_pivot = to_document.apply(pivot);
        g.orient_offset = g.pivot_offset;
      }
      if (gizmo_moving(mode)) {
        g.before = doc;
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
      g.live = {};
      g.live_map = Transform2D();
      g.capture = false;
      if (mode == GizmoMode::Cage) {
        /* A kept cage goes on bending the bones from where they were before its first drag. */
        g.cage_start = cage;
        g.cage_now = cage;
        g.grab = handle == -2 ? cage.unmap(mouse) : mouse;
        if (kept_cage && ts.gizmo_mode != 0) {
          const int frame = doc.frame_current;
          doc = g.session.base;
          doc.frame_current = frame;
          g.capture = true;
          /* The bones are taken from it next frame (`armature` is the old document's). */
          return;
        }
      }
      g.bones = armature.bones;
      g.turn_snap = mode == GizmoMode::Rotate ? bone_turn_snap_begin(armature, setup, base_space, false) : BoneTurnSnap();
      g.counter = {};
      if (!setup && move_only_bones_on(doc) && mode != GizmoMode::Select && mode != GizmoMode::Pivot) {
        g.counter = bone_counter_capture(doc, armature.id);
      }
    }
  }
  if (g.mode != GizmoMode::None && g.area == area.id) {
    if (g.capture) {
      g.capture = false;
      g.bones = armature.bones;
      g.counter = {};
      if (!setup && move_only_bones_on(doc)) {
        g.counter = bone_counter_capture(doc, armature.id);
      }
    }
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
            const ImVec2 a = g.start_screen, b = io.MousePos;
            const ImVec2 lo_s(std::min(a.x, b.x), std::min(a.y, b.y)), hi_s(std::max(a.x, b.x), std::max(a.y, b.y));
            const auto box = [&](Layer &l) {
              const Transform2D l_to_document = layer_world_transform(doc, l.id);
              armature_select_bones_in(
                  l, setup,
                  [&](float2 p) {
                    const ImVec2 s = view.to_screen(l_to_document.apply(p));
                    return s.x >= lo_s.x && s.x <= hi_s.x && s.y >= lo_s.y && s.y <= hi_s.y;
                  },
                  io.KeyShift, io.KeyAlt);
            };
            box_select_family(doc, armature, io.KeyShift || io.KeyAlt, box);
          }
          else if (io.KeyAlt) {
            /* Alt-drag takes bones out of the selection; an Alt-click picks the layer. */
            g.mode = GizmoMode::None;
            layer_pick_click(C, view, g.start_screen, io.KeyShift, false);
            break;
          }
          else {
            /* The best under the click: one of its family's (the bone layer around it, a shown sub bone
             * group) is worked in instead, so a character's bones all answer clicks. */
            const std::vector<FamilyHit> hits = pick_family_parts(doc, armature, view, io.MousePos);
            const FamilyHit best = hits.empty() ? FamilyHit() : hits.front();
            Layer *other = best.layer && best.layer->id != armature.id ? best.layer : nullptr;
            const uint64_t picked = other ? 0 : best.hit.bone;
            const uint64_t other_bone = other ? best.hit.bone : 0;
            if (!io.KeyShift) {
              family_deselect_all(doc, armature);
            }
            if (other) {
              if (Bone *b = bone_find(*other, other_bone)) {
                bone_select(*b, true);
                other->active_bone = other_bone;
              }
              /* Inside the bone layer worked in, it stays the one worked in. */
              if (!io.KeyShift && !layer_inside(doc, other->id, armature.id)) {
                doc.active_layer = other->id;
              }
            }
            else if (Bone *b = bone_find(armature, picked)) {
              bone_select(*b, true);
              armature.active_bone = picked;
            }
          }
          g.pivot_offset = {};
          push_undo(C, "Select Bones");
          g.mode = GizmoMode::None;
        }
        break;
      }
      case GizmoMode::Pivot: {
        float2 target = mouse;
        if (io.KeyCtrl) {
          nearest_end(mouse, false, target);
        }
        if (released && !dragged && io.KeyAlt) {
          /* An Alt-click on the pivot picks the layer there; the pivot stays. */
          g.mode = GizmoMode::None;
          layer_pick_click(C, view, g.start_screen, io.KeyShift, false);
          break;
        }
        g.pivot_offset = target - (g.lo + g.hi) * 0.5f;
        g.live_pivot = target;
        if (released || cancel) {
          g.mode = GizmoMode::None;
        }
        break;
      }
      case GizmoMode::Orient: {
        /* The box turns about its middle as the knob is swung round it (Shift: steps of 15 degrees); the bones
         * stay, and so does the pivot. */
        const float2 mb = base_space.inverted().apply(view.to_document(io.MousePos));
        float a = std::remainder(
            g.orient_start + std::atan2(mb.y - g.orient_centre.y, mb.x - g.orient_centre.x) - g.orient_ref, 2.0f * PI);
        if (io.KeyShift) {
          a = std::round(a / (PI / 12.0f)) * (PI / 12.0f);
        }
        if (cancel) {
          a = g.orient_start;
        }
        g.box_angle = a;
        g.live.degrees = a * 180.0f / PI;
        float2 nlo, nhi;
        if (cancel) {
          g.pivot_offset = g.orient_offset;
        }
        else if (bone_gizmo_bounds(armature, setup, a, nlo, nhi)) {
          const Transform2D now = base_space * Transform2D::rotate(a);
          g.pivot_offset = now.inverted().apply(g.orient_pivot) - (nlo + nhi) * 0.5f;
        }
        if (released || cancel) {
          g.mode = GizmoMode::None;
        }
        break;
      }
      default: {
        if (cancel) {
          const int frame = doc.frame_current;
          doc = std::move(g.before);
          doc.frame_current = frame;
          g.before = Document();
          g.mode = GizmoMode::None;
          return;
        }
        if (g.mode == GizmoMode::Cage) {
          g.cage_now = gizmo_cage_dragged(g.cage_start, g.handle, g.grab, mouse - g.start);
          const GizmoCage &bent = g.cage_now;
          armature_map_selected(
              armature, g.bones,
              std::function<float2(float2)>([&](float2 p) { return turn.apply(bent.map(unturn.apply(p))); }), setup);
        }
        else {
          if (g.mode == GizmoMode::Rotate) {
            g.turned += turn_between(g.last, mouse, g.pivot);
            g.last = mouse;
          }
          float2 snap, origin;
          const bool snapping = g.mode == GizmoMode::Move && io.KeyCtrl && nearest_end(mouse, true, snap);
          /* Ctrl: the turn lands on 45 degree directions, the start or the setup angle (BoneTurnSnap). */
          const char *snapped = nullptr;
          const float turned = g.mode == GizmoMode::Rotate && io.KeyCtrl ? bone_turn_snapped(g.turn_snap, g.turned, &snapped)
                                                                         : g.turned;
          const Transform2D map = gizmo_box_map(g.mode, g.handle, g.lo, g.hi, g.pivot, g.start, mouse, turned,
                                                snapping ? &snap : nullptr, g.live_pivot, g.live, origin);
          g.live_map = map;
          armature_map_selected(armature, g.bones, turn * map * unturn, setup);
        }
        if (ts.mirror_x) {
          armature_mirror_edits(armature, g.bones, setup);
        }
        if (!g.counter.layers.empty()) {
          animation_refresh_action_bones(doc);
          bone_counter_apply(doc, g.counter);
        }
        g.changed |= dragged;
        if (released) {
          const GizmoMode mode = g.mode;
          const uint64_t armature_id = armature.id;
          const GizmoLive live = g.live;
          const float2 lo0 = g.lo, hi0 = g.hi, start = g.start, pivot0 = g.pivot;
          (void)live;
          g.mode = GizmoMode::None;
          if (!g.changed) {
            const int frame = doc.frame_current;
            doc = std::move(g.before);
            doc.frame_current = frame;
            g.before = Document();
            g.counter = {};
            /* An Alt-click on the box (no drag) picks the layer there, as Alt-click does anywhere; Alt-drags
             * keep their meaning. */
            if (io.KeyAlt) {
              layer_pick_click(C, view, g.start_screen, io.KeyShift, false);
            }
            return;
          }
          if (mode == GizmoMode::Cage || mode == GizmoMode::Shear) {
            /* As one step from where the bones were before (a kept cage: before its first drag). */
            const int frame = doc.frame_current;
            const bool kept_on = mode == GizmoMode::Cage && ts.gizmo_mode != 0;
            /* While a kept cage is bent, `doc` is its starting document already (taken at the press). */
            Document base = kept_on && g.session.still(C, armature_id, selection) ? g.session.base : g.before;
            doc = base;
            doc.frame_current = frame;
            GizmoCage bent = g.cage_now;
            if (mode == GizmoMode::Shear) {
              /* The shear exactly: the box's corners where the drag's map took them. */
              bent.reset(lo0, hi0, 1);
              for (float2 &p : bent.nodes) {
                p = g.live_map.apply(p);
              }
            }
            bone_gizmo_cage(C, bent,
                            mode == GizmoMode::Shear ? "Shear Bones"
                            : ts.gizmo_mode == 2    ? "Warp Bones"
                                                    : "Distort Bones",
                            g.box_angle);
            if (kept_on) {
              g.session.base = std::move(base);
              g.session.keep(C, armature_id, gizmo_key(document_selection_hash(*C.document), g.box_angle), bent);
            }
          }
          else if (mode == GizmoMode::Scale && g.box_angle != 0.0f) {
            /* Scaled along a turned box's axes: no Transform Bones scale says that, so as the box's corners
             * where the drag took them (exactly: the map is affine). */
            armature.bones = g.bones;
            bone_counter_restore(doc, g.counter);
            GizmoCage bent;
            bent.reset(lo0, hi0, 1);
            for (float2 &p : bent.nodes) {
              p = g.live_map.apply(p);
            }
            bone_gizmo_cage(C, bent, "Scale Bones", g.box_angle);
          }
          else {
            armature.bones = g.bones;
            bone_counter_restore(doc, g.counter);
            float2 origin, live_pivot;
            GizmoLive again;
            gizmo_box_map(mode, g.handle, lo0, hi0, pivot0, start, mouse, g.turned, nullptr, live_pivot, again, origin);
            /* As one Transform Bones step the ViewportActionPanel can adjust (in the bone layer's space). */
            origin = turn.apply(origin);
            const float2 offset = turn.apply_vector(live.offset);
            operator_call(C, "armature.transform",
                          {{"origin_x", num_arg(origin.x)}, {"origin_y", num_arg(origin.y)},
                           {"move_x", num_arg(offset.x)}, {"move_y", num_arg(offset.y)},
                           {"scale_x", num_arg(live.scale.x)}, {"scale_y", num_arg(live.scale.y)},
                           {"angle", num_arg(live.degrees)},
                           {"only_bones", move_only_bones_on(*C.document) ? "1" : "0"},
                           {"posing", setup ? "0" : "1"}, {"keep_poses", keep_poses_on(*C.document) ? "1" : "0"}});
          }
          g.counter = {};
          g.before = Document();
          /* The pivot stays where the drag left it; a turn turns the box too, so it stays round the bones. */
          const float2 pivot_now = to_document.apply(g.live_pivot);
          if (mode == GizmoMode::Rotate) {
            g.box_angle = std::remainder(g.box_angle + live.degrees * PI / 180.0f, 2.0f * PI);
            g.box_turned += live.degrees * PI / 180.0f;
          }
          g.turned_changes = C.undo ? C.undo->change_count() : 0;
          float2 nlo, nhi;
          if (const Layer *moved = layer_find(*C.document, armature_id);
              moved && bone_gizmo_bounds(*moved, setup, g.box_angle, nlo, nhi))
          {
            const Transform2D now = layer_world_transform(*C.document, armature_id) * Transform2D::rotate(g.box_angle);
            g.pivot_offset = now.inverted().apply(pivot_now) - (nlo + nhi) * 0.5f;
          }
          return;
        }
        break;
      }
    }
  }

  /* The cursor says what a press would do; a right-click on the box opens its menu. */
  GizmoMode hot = g.mode;
  int hot_handle = g.handle;
  if (hovered && g.mode == GizmoMode::None && any) {
    hot = gizmo_hit(ts.gizmo_mode, cage, to_document, view, io.MousePos, mouse, lo, hi, pivot, io.KeyCtrl, &hot_handle);
    gizmo_hover_cursor(dl, hot, hot_handle);
  }
  const bool menu = hovered && g.mode == GizmoMode::None && any && hot != GizmoMode::Select &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  g_right_click_spent |= menu;
  if (gizmo_context_menu(C, "##bones_gizmo_menu", menu, g.pivot_offset != float2() || g.box_angle != 0.0f)) {
    g.pivot_offset = {};
    g.box_angle = 0.0f;
    g.box_turned = 0.0f;
  }

  /* The gizmo. */
  if (g.mode == GizmoMode::Select || !doc.overlays.show || !any) {
    return;
  }
  /* While dragging, the box as it was pressed, carried by the drag (so it turns, stretches and slants with
   * what it moves), and a grey dot where the pivot was. */
  const bool dragging = g.mode != GizmoMode::None && g.mode != GizmoMode::Orient;
  const ImVec2 was = view.to_screen(to_document.apply(g.pivot));
  gizmo_draw_box(dl, view, dragging ? to_document * g.live_map : to_document, ts.gizmo_mode, cage,
                 dragging ? g.lo : lo, dragging ? g.hi : hi,
                 !dragging ? pivot : g.mode == GizmoMode::Pivot ? g.live_pivot : g.pivot, dragging ? &was : nullptr,
                 hot, hot_handle, dragging);
  if (g.mode != GizmoMode::None && g.mode != GizmoMode::Pivot) {
    gizmo_readout(dl, io.MousePos, gizmo_live_text(g.mode, ts.gizmo_mode, g.live).c_str());
  }
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
  /* Keep Poses while dragging on the setup frame (empty: off). */
  SetupSnapshot kept;
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
  GizmoLive live;
  Transform2D live_map;
  /* How far the box is turned from the layer's axes (radians; see gizmo_orient_knob), and for turning it:
   * where it was, the mouse's angle round the box's middle and that middle (the layer's space), and the
   * pivot (document space) and its offset as the press found them. */
  float box_angle = 0.0f;
  /* The part of box_angle Rotate drags gave it (the box turns with what it turns), kept only while the
   * document is as this gizmo's last drag left it (`turned_changes`): an undo squares it again. */
  float box_turned = 0.0f;
  uint64_t turned_changes = 0;
  float orient_start = 0.0f, orient_ref = 0.0f;
  float2 orient_centre, orient_pivot, orient_offset;
  /* A Cage drag: the document as the press found it (a cancel puts it back), the cage then and now, and
   * where its inside was pressed (box space, unbent). */
  Document before;
  GizmoCage cage_start, cage_now;
  float2 grab;
  CageSession session;
};
static PointGizmo g_point_gizmo;

/* While the Transform Points box is moving, turning, scaling, shearing or bending points: they and their
 * handles aren't drawn, so the shape itself is what's seen. */
static bool point_gizmo_busy()
{
  const GizmoMode m = g_point_gizmo.mode;
  return m == GizmoMode::Move || m == GizmoMode::Rotate || m == GizmoMode::Scale || m == GizmoMode::Shear ||
         m == GizmoMode::Cage;
}

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
    const ShownHandleFactors factors = drawing_detail_shown_factors(d);
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
            places.push_back(to.apply(handle_end(d, factors, c, i, pre)));
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
/* Whether `item`, a handle of a selected point, leads to a point that isn't selected (the curve's next point
 * shown for an out handle, its last for an in handle; none past an open end). */
static bool handle_leads_out(const Document &doc, const ModalTransform::Item &item)
{
  const Layer *layer = layer_find(doc, item.layer);
  if (!layer || item.curve < 0 || item.curve >= int(layer->drawing.curves.size())) {
    return false;
  }
  const VectorDrawing &drawing = layer->drawing;
  const Curve &curve = drawing.curves[size_t(item.curve)];
  const int n = int(curve.points.size());
  /* The next point shown: detail hidden above the shape's level rides the curve, picked or not. */
  int k = item.index;
  for (int step = 0; step < n; step++) {
    k += item.pre ? -1 : 1;
    if (k < 0 || k >= n) {
      if (!curve.closed) {
        return false;
      }
      k = (k + n) % n;
    }
    if (k == item.index) {
      return false;
    }
    const MeshPoint &neighbour = drawing.points[size_t(curve.points[size_t(k)].point)];
    if (!neighbour.detail.hidden) {
      return !neighbour.selected;
    }
  }
  return false;
}

/* The selection mapped by the Transform Points box (document space -> document space). A selected point's handle
 * that leads to a point left out keeps its length and only rides along with its point, so the part not picked
 * keeps its shape (scaling a circle's right half from the right leaves the left half round); a smooth point's
 * such handle stays in line with its other one, which the map turns and stretches, so no corner appears where
 * the selection ends. With proportional editing the points left out come along, and every handle maps. */
static void point_gizmo_place_map(Context &C, ModalTransform &moving, const std::function<float2(float2)> &map)
{
  ModalLayers layers;
  if (!modal_layers(C, moving, layers)) {
    return;
  }
  const Document &doc = *C.document;
  const bool keep_outward = !C.wm->tool_settings.proportional;
  std::map<std::tuple<uint64_t, int, int, bool>, const ModalTransform::Item *> handles;
  for (const ModalTransform::Item &item : moving.items) {
    if (item.curve >= 0 && !item.direct) {
      handles[{item.layer, item.curve, item.index, item.pre}] = &item;
    }
  }
  for (const ModalTransform::Item &item : moving.items) {
    const float w = modal_weight(C.wm->tool_settings, item);
    if (w <= 0.0f) {
      modal_rest(layers, item);
      continue;
    }
    float2 to = map(item.start);
    if (keep_outward && item.curve >= 0 && !item.direct && !item.proportional && item.mirror_source < 0 &&
        handle_leads_out(doc, item))
    {
      const float2 offset = item.start - item.origin;
      const float2 at = map(item.origin);
      float2 out = offset;
      const auto other = handles.find({item.layer, item.curve, item.index, !item.pre});
      if (other != handles.end() && length(offset) > 1e-9f) {
        const float2 other0 = other->second->start - item.origin;
        const float2 other1 = map(other->second->start) - at;
        if (length(other0) > 1e-9f && length(other1) > 1e-9f &&
            dot(normalize(other0), normalize(offset)) < -0.9998f)
        {
          out = -normalize(other1) * length(offset);
        }
      }
      to = at + out;
    }
    modal_place(layers, item, item.start + (to - item.start) * w);
  }
  modal_mirror(C, moving, layers);
}

static void point_gizmo_place(Context &C, ModalTransform &moving, const Transform2D &to_document,
                              const Transform2D &map)
{
  const Transform2D in_document = to_document * map * to_document.inverted();
  point_gizmo_place_map(C, moving, [&](float2 p) { return in_document.apply(p); });
}

/* The same through any map (gizmo space): a Distort or Warp cage's. */
static void point_gizmo_place(Context &C, ModalTransform &moving, const Transform2D &to_document,
                              const std::function<float2(float2)> &map)
{
  const Transform2D to_gizmo = to_document.inverted();
  point_gizmo_place_map(C, moving, [&](float2 p) { return to_document.apply(map(to_gizmo.apply(p))); });
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
                 {"scale_x", num(scale.x)}, {"scale_y", num(scale.y)}, {"angle", num(degrees)},
                 {"setup", C.document->frame_current == 0 ? "1" : "0"}, {"keep_poses", keep_poses_on(*C.document) ? "1" : "0"}});
}

/* An option of the ViewportActionPanel. */
static OpProperty panel_float(const char *name, const char *label, const char *description, const char *fallback,
                              float min, float max, float step)
{
  OpProperty p;
  p.name = name;
  p.label = label;
  p.description = description;
  p.kind = OpProperty::Kind::Float;
  p.fallback = fallback;
  p.min = min;
  p.max = max;
  p.step = step;
  return p;
}

/* Keep Poses: shown for a move made on the setup frame (args[key] == value). */
static OpProperty keep_poses_option(const char *key, const char *value)
{
  OpProperty p;
  p.name = "keep_poses";
  p.label = "Keep Poses";
  p.description = "Other frames' keyed poses stay where they are: the move changes only the setup pose, its change "
                  "taken off every key on another frame, in every action. Kept for the next moves until the frame "
                  "changes";
  p.kind = OpProperty::Kind::Bool;
  p.fallback = "0";
  const std::string k = key, v = value;
  p.visible = [k, v](const OpArgs &args) {
    const auto it = args.find(k);
    return it != args.end() && it->second == v;
  };
  return p;
}

/* Keep Poses around a move: the setup of the layers it may change, taken before it when the move is
 * on the setup frame with the option on (the choice kept for the next moves). */
static SetupSnapshot keep_poses_begin(Context &C, const std::vector<uint64_t> &layers)
{
  if (op_has_arg(C, "keep_poses")) {
    sticky_set(g_keep_poses, *C.document, op_arg_bool(C, "keep_poses"));
  }
  if (C.document->frame_current != 0 || !op_arg_bool(C, "keep_poses")) {
    return {};
  }
  return setup_snapshot(*C.document, layers);
}

/* ...and after it: the move recorded as the setup, then taken off the other frames' keys. */
static void keep_poses_end(Context &C, const SetupSnapshot &before)
{
  if (before.empty()) {
    return;
  }
  animation_record(*C.document);
  keep_poses(*C.document, before);
}

static std::vector<uint64_t> editable_layer_ids(Context &C)
{
  std::vector<uint64_t> ids;
  for (const Layer *l : editable_layers(C)) {
    ids.push_back(l->id);
  }
  return ids;
}

/* Move Only Bones: shown for a move made while posing (bones don't bend drawings on frame 0). */
static OpProperty only_bones_option()
{
  OpProperty p;
  p.name = "only_bones";
  p.label = "Move Only Bones";
  p.description = "The drawings stay where they are: the points (and layers following a bone) the bones bend "
                  "get keyed on this frame to undo the bend. Kept for later moves until the frame or action "
                  "changes";
  p.kind = OpProperty::Kind::Bool;
  p.fallback = "0";
  p.visible = [](const OpArgs &args) {
    const auto it = args.find("posing");
    return it != args.end() && it->second == "1";
  };
  return p;
}

/* Move Without Children: posing, G, R and S move only the picked bones; their unpicked children stay
 * (on the setup frame they do anyway, as in Blender's edit mode). */
static OpProperty without_children_option()
{
  OpProperty p;
  p.name = "without_children";
  p.label = "Move Without Children";
  p.description = "Only the picked bones move: their children that aren't picked stay where they are (a "
                  "connected child keeps its direction, its base on its parent's tip). Kept for later moves until "
                  "the frame or action changes";
  p.kind = OpProperty::Kind::Bool;
  p.fallback = "0";
  p.visible = [](const OpArgs &args) {
    const auto it = args.find("posing");
    return it != args.end() && it->second == "1";
  };
  return p;
}

/* Move, Rotate, Scale and Transform Bones on the active bone layer's selected bones, as the drag ends
 * and again from the ViewportActionPanel: `kind` G, R, S or T (the Transform Bones tool's map). */
static OpStatus bones_transform_exec(Context &C, char kind)
{
  Document &doc = *C.document;
  Layer *active = document_active_layer(doc);
  if (!active || !active->is_bone_layer()) {
    return OpStatus::Cancelled;
  }
  /* G, R and S take in the sub bone groups inside it; the Transform Bones tool's box is around the active one's. */
  std::vector<Layer *> moving = bones_moving(doc, *active);
  if (kind == 'T' || kind == 'C') {
    moving.clear();
    if (armature_any_picked(*active)) {
      moving.push_back(active);
    }
  }
  if (moving.empty()) {
    return OpStatus::Cancelled;
  }
  const bool setup = doc.frame_current == 0;
  GizmoCage cage;
  if (kind == 'C' && !cage_from_args(C, cage)) {
    return OpStatus::Cancelled;
  }
  /* The choices are kept for the moves after this one, until the frame or action changes. */
  if (op_has_arg(C, "only_bones")) {
    sticky_set(g_move_only_bones, doc, op_arg_bool(C, "only_bones"));
  }
  if (op_has_arg(C, "without_children")) {
    sticky_set(g_move_without_children, doc, op_arg_bool(C, "without_children"));
  }
  /* On the setup frame bones move without their children anyway, as in Blender's edit mode. */
  const bool without_children = kind != 'T' && kind != 'C' && !setup && op_arg_bool(C, "without_children");
  struct Moved {
    Layer *armature;
    std::vector<Bone> start;
    Transform2D to_armature;
    BoneCounter counter;
  };
  std::vector<Moved> moved;
  std::vector<uint64_t> moved_ids;
  for (Layer *l : moving) {
    Moved m{l, l->bones, picked_bones_shown(doc, *l, setup).inverted(), {}};
    if (!setup && op_arg_bool(C, "only_bones")) {
      m.counter = bone_counter_capture(doc, l->id);
    }
    moved.push_back(std::move(m));
    moved_ids.push_back(l->id);
  }
  const SetupSnapshot kept = keep_poses_begin(C, moved_ids);
  const float2 move(op_arg_float(C, "move_x", 0.0f), op_arg_float(C, "move_y", 0.0f));
  const float degrees = op_arg_float(C, "angle", 0.0f);
  for (const Moved &m : moved) {
    Layer *armature = m.armature;
    const std::vector<Bone> &start = m.start;
    if (kind == 'G') {
      armature_grab_selected(*armature, start, m.to_armature.apply_vector(move), setup, C.wm->tool_settings.auto_ik);
      if (op_arg_bool(C, "knob_snap")) {
        armature_snap_knobs(*armature);
      }
    }
    else if (kind == 'R') {
      armature_rotate_selected(*armature, start, degrees * PI / 180.0f, setup);
    }
    else if (kind == 'S') {
      const int axis = std::clamp(op_arg_int(C, "axis", 0), 0, 2);
      const bool squash = op_arg_bool(C, "squash") && !setup;
      armature_scale_selected(*armature, start, op_arg_float(C, "scale", 1.0f), setup, axis, squash);
      C.undo_name = setup ? "Resize Bones" : squash ? "Squash & Stretch" : "Scale Bones";
    }
    else if (kind == 'C') {
      const Transform2D turn = Transform2D::rotate(op_arg_float(C, "box_angle", 0.0f) * PI / 180.0f);
      const Transform2D unturn = turn.inverted();
      armature_map_selected(*armature, start,
                            std::function<float2(float2)>([&](float2 p) { return turn.apply(cage.map(unturn.apply(p))); }),
                            setup);
      C.undo_name = op_arg(C, "name", "Distort Bones");
    }
    else {
      const float2 origin(op_arg_float(C, "origin_x", 0.0f), op_arg_float(C, "origin_y", 0.0f));
      const float2 scale(op_arg_float(C, "scale_x", 1.0f), op_arg_float(C, "scale_y", 1.0f));
      armature_map_selected(*armature, start, map_about(origin, move, scale, degrees * PI / 180.0f), setup);
    }
    if (without_children) {
      armature_keep_children(*armature, start, setup);
    }
    if (C.wm->tool_settings.mirror_x) {
      armature_mirror_edits(*armature, start, setup);
    }
  }
  animation_refresh_action_bones(doc);
  for (const Moved &m : moved) {
    if (!m.counter.layers.empty()) {
      bone_counter_apply(doc, m.counter);
    }
  }
  keep_poses_end(C, kept);
  return OpStatus::Finished;
}

/* Move, Rotate and Scale on the selected points (G, R, S), as the drag ends and again from the
 * ViewportActionPanel: proportional editing, mirror and the pivot as the drag had them. */
static OpStatus points_transform_exec(Context &C, char kind)
{
  static Area area;
  ModalTransform moving;
  if (!modal_collect(C, area, kind, {}, moving, false)) {
    return OpStatus::Cancelled;
  }
  ModalLayers layers;
  if (!modal_layers(C, moving, layers)) {
    return OpStatus::Cancelled;
  }
  const SetupSnapshot kept = keep_poses_begin(C, editable_layer_ids(C));
  const float2 delta(op_arg_float(C, "move_x", 0.0f), op_arg_float(C, "move_y", 0.0f));
  const float2 scale(op_arg_float(C, "scale_x", 1.0f), op_arg_float(C, "scale_y", 1.0f));
  modal_apply(C, moving, layers, kind, delta, op_arg_float(C, "angle", 0.0f) * PI / 180.0f, scale);
  keep_poses_end(C, kept);
  return OpStatus::Finished;
}

/* Alt G, R, S on bones: the pose cleared, `amount` of the way, with Move Only Bones and Move Without
 * Children as the moves have them (kept for later moves, until the frame or action changes). */
static OpStatus bones_clear_exec(Context &C, int kind)
{
  Document &doc = *C.document;
  Layer *armature = document_active_layer(doc);
  if (!armature || !armature->is_bone_layer() || !armature_any_picked(*armature)) {
    return OpStatus::Cancelled;
  }
  const auto option = [&](const char *name, StickyOption &sticky) {
    if (op_has_arg(C, name)) {
      sticky_set(sticky, doc, op_arg_bool(C, name));
    }
    const bool on = sticky_on(sticky, doc);
    op_arg_used(C, name, on ? "1" : "0");
    return on;
  };
  const bool only_bones = option("only_bones", g_move_only_bones);
  const bool without_children = option("without_children", g_move_without_children);
  const float amount = std::clamp(op_arg_float(C, "amount", 1.0f), 0.0f, 1.0f);
  const bool along_x = op_arg(C, "x", "1") == "1", along_y = op_arg(C, "y", "1") == "1";
  BoneCounter counter;
  if (only_bones) {
    counter = bone_counter_capture(doc, armature->id);
  }
  const std::vector<Bone> start = armature->bones;
  if (!armature_clear_pose(*armature, kind == 0, kind == 1, kind == 2)) {
    return OpStatus::Cancelled;
  }
  /* Part of the way, and only along the axes asked for: the location along the bone's rest axes (its
   * Transform), as the Bone panel shows it. */
  for (size_t i = 0; i < armature->bones.size(); i++) {
    Bone &b = armature->bones[i];
    const Bone &was = start[i];
    if (kind == 0) {
      const float2 from = bone_transform_from_position(was, was.position);
      const float2 to = bone_transform_from_position(b, b.position);
      const float2 t(along_x ? from.x + (to.x - from.x) * amount : from.x,
                     along_y ? from.y + (to.y - from.y) * amount : from.y);
      b.position = bone_position_from_transform(b, t);
    }
    else if (kind == 1) {
      b.angle = was.angle + (b.angle - was.angle) * amount;
    }
    else {
      b.scale = was.scale + (b.scale - was.scale) * amount;
    }
  }
  if (without_children) {
    armature_keep_children(*armature, start, false);
  }
  animation_refresh_action_bones(doc);
  if (!counter.layers.empty()) {
    bone_counter_apply(doc, counter);
  }
  return OpStatus::Finished;
}

/* Smooth Points on what shows: a posed drawing is smoothed as it is posed, then each point (and its
 * handles) goes back through its bones to where it is stored, as a move does. */
static OpStatus smooth_points_exec(Context &C)
{
  SmoothPointsSettings s;
  s.factor = op_arg_float(C, "factor", 0.5f);
  s.repeat = op_arg_int(C, "repeat", 1);
  s.position = op_arg(C, "position", "1") == "1";
  s.handles = op_arg(C, "handles", "1") == "1";
  s.radius = op_arg(C, "radius", "0") == "1";
  s.keep_shape = op_arg(C, "keep_shape", "0") == "1";
  s.ends = op_arg(C, "ends", "0") == "1";
  const ToolSettings &ts = C.wm->tool_settings;
  const SetupSnapshot kept = keep_poses_begin(C, editable_layer_ids(C));
  const PartnerMirror partners = layer_mirror_partners_begin(*C.document, editable_layer_ids(C), ts.mirror_x);
  int smoothed = 0;
  for (Layer *layer : editable_layers(C)) {
    if (layer->is_reference() && !layer->reference.edit_points) {
      continue;
    }
    VectorDrawing &d = layer->drawing;
    drawing_init_handles(d);
    const Posed shown = posed(*C.document, *layer);
    if (!shown.moved) {
      smoothed += drawing_smooth_points(d, s);
    }
    else {
      VectorDrawing work = shown.storage;
      const int n = drawing_smooth_points(work, s);
      smoothed += n;
      for (size_t i = 0; n > 0 && i < d.points.size(); i++) {
        if (d.points[i].selected) {
          d.points[i].pos = shown.skins[i].inverted().apply(work.points[i].pos);
          d.points[i].radius = work.points[i].radius;
        }
      }
      for (size_t c = 0; n > 0 && c < d.curves.size(); c++) {
        for (size_t i = 0; i < d.curves[c].points.size(); i++) {
          CurvePoint &cp = d.curves[c].points[i];
          const CurvePoint &w = work.curves[c].points[i];
          if (d.points[cp.point].selected) {
            const Transform2D un = shown.skins[cp.point].inverted();
            cp.handle_in = un.apply_vector(w.handle_in);
            cp.handle_out = un.apply_vector(w.handle_out);
            cp.handles_set = true;
            cp.aligned = w.aligned;
          }
        }
      }
    }
    const bool self_x = layer_mirror_x_self(*C.document, layer->id, ts.mirror_x);
    if (self_x || ts.mirror_y) {
      drawing_apply_mirror(d, drawing_mirror_pairs(d, drawing_selected_points(d), self_x, ts.mirror_y));
      drawing_mirror_selected_handles(d, self_x, ts.mirror_y);
    }
  }
  layer_mirror_partners_end(*C.document, partners);
  keep_poses_end(C, kept);
  if (smoothed == 0) {
    report(ReportType::Warning, "Select points along a curve to smooth them (an open curve's ends need Smooth Endpoints)");
    return OpStatus::Cancelled;
  }
  return OpStatus::Finished;
}

/* ---------------------------------------------------------------------------------
 * Snap (Shift S): Blender's cursor and selection snapping, for the 2D cursor
 */

/* What the selection is: picked bones (a bone layer), the active Media layer, or points. */
enum class SnapSelection { None, Points, Bones, Media };

static SnapSelection snap_selection(Context &C)
{
  if (!C.document) {
    return SnapSelection::None;
  }
  Layer *active = document_active_layer(*C.document);
  const bool whole = C.wm->tool_settings.tool == Tool::TransformLayer;
  if (active && active->is_bone_layer() && armature_any_picked(*active) && !whole) {
    return SnapSelection::Bones;
  }
  /* A Media or text layer (or any, with Transform Layer): the layer itself. */
  if (active && (active->is_media() || active->is_text() || whole) && !active->locked) {
    return SnapSelection::Media;
  }
  for (Layer *layer : editable_layers(C)) {
    for (const MeshPoint &p : layer->drawing.points) {
      if (p.selected && !p.detail.hidden) {
        return SnapSelection::Points;
      }
    }
  }
  return SnapSelection::None;
}

/* The middle of the selection where it shows (document space). */
static bool snap_selection_middle(Context &C, float2 *r_middle)
{
  Document &doc = *C.document;
  Layer *active = document_active_layer(doc);
  float2 sum;
  int count = 0;
  switch (snap_selection(C)) {
    case SnapSelection::Bones: {
      const Transform2D to_document = layer_world_transform(doc, active->id);
      for (const float2 head : armature_picked_heads(*active, doc.frame_current == 0)) {
        sum += to_document.apply(head);
        count++;
      }
      break;
    }
    case SnapSelection::Media:
      *r_middle = layer_world_transform(doc, active->id).apply(float2());
      return true;
    case SnapSelection::Points:
      for (Layer *layer : editable_layers(C)) {
        const Transform2D to_document = layer_world_transform(doc, layer->id);
        const Posed shown = posed(doc, *layer);
        for (int i = 0; i < int(layer->drawing.points.size()); i++) {
          const MeshPoint &p = layer->drawing.points[size_t(i)];
          if (p.selected && !p.detail.hidden) {
            sum += to_document.apply(shown.drawing->points[size_t(i)].pos);
            count++;
          }
        }
      }
      break;
    case SnapSelection::None:
      break;
  }
  if (count == 0) {
    return false;
  }
  *r_middle = sum / float(count);
  return true;
}

/* The active element where it shows: the active bone's base, the active shape's middle (the shape
 * last clicked), or the Media layer's centre. */
static bool snap_active(Context &C, float2 *r_at)
{
  Document &doc = *C.document;
  Layer *active = document_active_layer(doc);
  switch (snap_selection(C)) {
    case SnapSelection::Bones: {
      const std::vector<BonePose> poses = armature_keyed_pose(*active, doc.frame_current == 0);
      const BonePose *pose = bone_pose_find(poses, active->active_bone);
      if (!pose) {
        return false;
      }
      *r_at = layer_world_transform(doc, active->id).apply(pose->head);
      return true;
    }
    case SnapSelection::Media:
      *r_at = layer_world_transform(doc, active->id).apply(float2());
      return true;
    default:
      break;
  }
  for (Layer *layer : editable_layers(C)) {
    const int s = drawing_shape_index(layer->drawing, doc.active_shape);
    if (s < 0) {
      continue;
    }
    const Transform2D to_document = layer_world_transform(doc, layer->id);
    const Posed shown = posed(doc, *layer);
    float2 sum;
    int count = 0;
    for (const int p : shape_points(layer->drawing, layer->drawing.shapes[size_t(s)])) {
      sum += to_document.apply(shown.drawing->points[size_t(p)].pos);
      count++;
    }
    if (count > 0) {
      *r_at = sum / float(count);
      return true;
    }
  }
  return false;
}

/* Each selected point to `target(where it shows)`, through its bones, its handles coming along, with
 * mirror editing and Keep Poses as a move has them. */
static bool snap_points_to(Context &C, const std::function<float2(float2)> &target)
{
  static Area area;
  ModalTransform moving;
  if (!modal_collect(C, area, 'G', {}, moving, false)) {
    return false;
  }
  ModalLayers layers;
  if (!modal_layers(C, moving, layers)) {
    return false;
  }
  const SetupSnapshot kept = keep_poses_begin(C, editable_layer_ids(C));
  std::map<std::pair<uint64_t, int>, float2> moved;
  for (const ModalTransform::Item &item : moving.items) {
    if (item.curve >= 0 || item.mirror_source >= 0 || item.proportional) {
      continue;
    }
    const float2 to = target(item.start);
    moved[{item.layer, item.point}] = to - item.start;
    modal_place(layers, item, to);
  }
  for (const ModalTransform::Item &item : moving.items) {
    if (item.mirror_source >= 0) {
      continue;
    }
    if (item.proportional) {
      modal_rest(layers, item);
      continue;
    }
    if (item.curve < 0) {
      continue;
    }
    if (const auto it = moved.find({item.layer, item.point}); it != moved.end()) {
      modal_place(layers, item, item.start + it->second);
    }
  }
  modal_mirror(C, moving, layers);
  keep_poses_end(C, kept);
  return !moved.empty();
}

/* The selection moved so that it lands on `to`: each point (or, with `keep_offset`, the selection's
 * middle; bones always move together, by their middle); a Media layer's centre. */
static bool snap_selection_to(Context &C, float2 to, bool keep_offset)
{
  Document &doc = *C.document;
  float2 middle;
  if (!snap_selection_middle(C, &middle)) {
    return false;
  }
  switch (snap_selection(C)) {
    case SnapSelection::Points:
      if (keep_offset) {
        return snap_points_to(C, [&](float2 p) { return p + (to - middle); });
      }
      return snap_points_to(C, [&](float2) { return to; });
    case SnapSelection::Bones: {
      /* Move Only Bones and Move Without Children as the bone moves have them (kept until the frame or action
       * changes); posing only, so the panel shows them then. */
      const bool posing = doc.frame_current != 0;
      op_arg_used(C, "posing", posing ? "1" : "0");
      const auto option = [&](const char *name, StickyOption &sticky) {
        if (op_has_arg(C, name)) {
          sticky_set(sticky, doc, op_arg_bool(C, name));
        }
        const bool on = sticky_on(sticky, doc);
        op_arg_used(C, name, on ? "1" : "0");
        return on ? "1" : "0";
      };
      const char *only_bones = option("only_bones", g_move_only_bones);
      const char *without_children = option("without_children", g_move_without_children);
      const float2 d = to - middle;
      return operator_call(C, "armature.translate",
                           {{"move_x", num_arg(d.x)}, {"move_y", num_arg(d.y)}, {"only_bones", only_bones},
                            {"without_children", without_children}}) == OpStatus::Finished;
    }
    case SnapSelection::Media: {
      Layer &layer = *document_active_layer(doc);
      const Transform2D parent = layer_world_transform(doc, layer.id) * layer.local_transform().inverted();
      layer.position = parent.inverted().apply(to);
      return true;
    }
    case SnapSelection::None:
      break;
  }
  return false;
}

static void snap_operators_register()
{
  const auto cursor_op = [](const char *idname, const char *name, const char *description,
                            std::function<bool(Context &, float2 *)> where) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    /* The cursor is a tool setting, as Blender's 3D cursor is the scene's: no undo step of its own. */
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document && C.wm; };
    ot.exec = [where](Context &C) {
      float2 at;
      if (!where(C, &at)) {
        report(ReportType::Warning, "Nothing to put the cursor on: select points, bones or a Media layer");
        return OpStatus::Cancelled;
      }
      C.wm->tool_settings.cursor = at;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  };
  cursor_op("view.snap_cursor_to_center", "Cursor to World Origin", "Put the 2D cursor at the origin (0, 0)",
            [](Context &, float2 *r) {
              *r = float2();
              return true;
            });
  cursor_op("view.snap_cursor_to_selected", "Cursor to Selected",
            "Put the 2D cursor at the middle of the selection (points, bones or the Media layer)", snap_selection_middle);
  cursor_op("view.snap_cursor_to_active", "Cursor to Active",
            "Put the 2D cursor on the active element: the active bone's base, the active shape's middle or the Media "
            "layer's centre",
            snap_active);
  cursor_op("view.snap_cursor_to_grid", "Cursor to Grid", "Put the 2D cursor on the nearest grid point",
            [](Context &C, float2 *r) {
              *r = snap_to_grid(C.wm->tool_settings.cursor, C.wm->tool_settings.grid_size);
              return true;
            });

  const auto selection_op = [](const char *idname, const char *name, const char *description,
                               std::vector<OpProperty> props, std::function<bool(Context &)> run) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.props = std::move(props);
    ot.poll = [](Context &C) { return C.document && C.wm && snap_selection(C) != SnapSelection::None; };
    ot.exec = [run](Context &C) { return run(C) ? OpStatus::Finished : OpStatus::Cancelled; };
    operators().add(std::move(ot));
  };
  selection_op("view.snap_selected_to_grid", "Selection to Grid",
               "Move each selected point onto its nearest grid point (bones and a Media layer: their middle); "
               "only_bones= and without_children= as Move Bones",
               {only_bones_option(), without_children_option()},
               [](Context &C) {
                 const float grid = C.wm->tool_settings.grid_size;
                 if (snap_selection(C) == SnapSelection::Points) {
                   return snap_points_to(C, [&](float2 p) { return snap_to_grid(p, grid); });
                 }
                 float2 middle;
                 return snap_selection_middle(C, &middle) && snap_selection_to(C, snap_to_grid(middle, grid), true);
               });
  OpProperty offset;
  offset.name = "use_offset";
  offset.label = "Offset";
  offset.description = "Keep the selection's shape: its middle goes to the cursor (off: every point goes there)";
  offset.kind = OpProperty::Kind::Bool;
  offset.fallback = "0";
  selection_op("view.snap_selected_to_cursor", "Selection to Cursor",
               "Move the selection to the 2D cursor: each point onto it, or with use_offset=1 the selection's middle "
               "(bones always move together); only_bones= and without_children= as Move Bones",
               {offset, only_bones_option(), without_children_option()}, [](Context &C) {
                 return snap_selection_to(C, C.wm->tool_settings.cursor, op_arg_bool(C, "use_offset"));
               });
  selection_op("view.snap_selected_to_active", "Selection to Active",
               "Move the selection onto the active element (each point; bones together); only_bones= and "
               "without_children= as Move Bones",
               {only_bones_option(), without_children_option()}, [](Context &C) {
                 float2 at;
                 return snap_active(C, &at) && snap_selection_to(C, at, false);
               });

  OperatorType pie;
  pie.idname = "view.snap_pie";
  pie.name = "Snap";
  pie.description = "Blender's Snap pie menu (Shift S): the 2D cursor to the origin, selection, active or grid, and "
                    "the selection to the cursor, active or grid";
  pie.poll = [](Context &C) { return C.document && C.wm && C.wm->mouse_in_viewport; };
  pie.exec = [](Context &C) {
    C.wm->menu_request = "snap_pie";
    return OpStatus::Finished;
  };
  operators().add(std::move(pie));
}

/* Shift S: Blender's Snap pie. Its items sit round the mouse as on the numpad (4 left, 6 right, 8 up, 2
 * down, the diagonals between); pointing the mouse their way lights one, and a click or its number picks
 * it; Esc, right-click or a click in the middle closes it. As Blender's: letting go of S picks the lit
 * one if the mouse went out of the middle while S was held, however quickly (a drag), or if S was held a
 * while; a quick tap leaves it open to click. */
static struct {
  bool open = false;
  int area = 0;
  ImVec2 centre;
  double opened = 0.0;
  /* S still held since it opened, and whether the mouse left the middle meanwhile. */
  bool held = true;
  bool dragged = false;
} g_snap_pie;

static void snap_pie_close(Context &C)
{
  g_snap_pie.open = false;
  C.wm->modal_running = false;
  C.wm->modal_keys.clear();
}

static void snap_pie(Context &C, bool hovered, int area)
{
  WindowManager &wm = *C.wm;
  ImGuiIO &io = ImGui::GetIO();
  if (hovered && wm.menu_request == "snap_pie") {
    wm.menu_request.clear();
    g_snap_pie = {true, area, io.MousePos, ImGui::GetTime(), true, false};
    wm.modal_running = true;
    wm.modal_keys.clear();
  }
  if (!g_snap_pie.open || g_snap_pie.area != area) {
    return;
  }
  struct Item {
    int number;
    float dx, dy;
    const char *label;
    const char *idname;
    OpArgs args;
  };
  static const Item items[] = {
      {4, -1.0f, 0.0f, "Cursor to Grid", "view.snap_cursor_to_grid", {}},
      {6, 1.0f, 0.0f, "Selection to Grid", "view.snap_selected_to_grid", {}},
      {2, 0.0f, 1.0f, "Cursor to Selected", "view.snap_cursor_to_selected", {}},
      {8, 0.0f, -1.0f, "Selection to Cursor", "view.snap_selected_to_cursor", {}},
      {7, -0.7071f, -0.7071f, "Selection to Cursor (Keep Offset)", "view.snap_selected_to_cursor", {{"use_offset", "1"}}},
      {9, 0.7071f, -0.7071f, "Selection to Active", "view.snap_selected_to_active", {}},
      {1, -0.7071f, 0.7071f, "Cursor to World Origin", "view.snap_cursor_to_center", {}},
      {3, 0.7071f, 0.7071f, "Cursor to Active", "view.snap_cursor_to_active", {}},
  };
  /* A clear window over everything takes the clicks, so nothing underneath reacts. */
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::SetNextWindowFocus();
  ImGui::Begin("##snap_pie", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::InvisibleButton("##catch", viewport->Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  const bool cancel = ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::End();

  /* Blender's pie, in its units (UI_UNIT, 20 pixels at scale 1): items 100 out, 1.5 units tall; the
   * left ones end at the radius, the right ones start there, the top and bottom ones sit centred above
   * and below it, the diagonals beside it; a ring of 12 to 19 pixels in the middle. */
  const ImVec2 c = g_snap_pie.centre;
  const float u = ImGui::GetFrameHeight(), fs = ImGui::GetFontSize();
  const float scale = u / 20.0f;
  const float radius = 100.0f * scale, row_h = 1.5f * u, pad = 0.35f * u, icon = 0.8f * u;
  const float ring_in = 12.0f * scale, ring_out = 19.0f * scale;
  /* Which way the mouse points (none inside the ring's threshold). */
  const ImVec2 m = io.MousePos;
  const float mx = m.x - c.x, my = m.y - c.y;
  int lit = -1;
  if (mx * mx + my * my > ring_in * ring_in) {
    float best = -2.0f;
    const float len = std::sqrt(mx * mx + my * my);
    for (int i = 0; i < 8; i++) {
      const float d = (mx * items[i].dx + my * items[i].dy) / len;
      if (d > best) {
        best = d;
        lit = i;
      }
    }
  }
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  constexpr ImU32 inner = IM_COL32(0x18, 0x18, 0x18, 255), inner_lit = IM_COL32(0x54, 0x54, 0x54, 255),
                  outline = IM_COL32(0x24, 0x24, 0x24, 255), blue = IM_COL32(0x47, 0x72, 0xb3, 255),
                  text = IM_COL32(0xd9, 0xd9, 0xd9, 255), text_lit = IM_COL32(0xff, 0xff, 0xff, 255),
                  dim = IM_COL32(0x8c, 0x8c, 0x8c, 255);
  /* The middle: a dark ring, a quarter of it blue the way the mouse points (axes and diagonals both). */
  const float ring_mid = (ring_in + ring_out) * 0.5f, ring_w = ring_out - ring_in;
  dl->AddCircle(c, ring_mid, inner, 48, ring_w);
  if (lit >= 0) {
    const float a = std::atan2(items[lit].dy, items[lit].dx);
    dl->PathArcTo(c, ring_mid, a - PI / 8.0f, a + PI / 8.0f, 12);
    dl->PathStroke(blue, 0, ring_w);
  }
  dl->AddCircle(c, ring_in, outline, 48, 1.0f);
  dl->AddCircle(c, ring_out, outline, 48, 1.0f);
  const ImVec2 title = ImGui::CalcTextSize("Snap");
  dl->AddText(ImVec2(c.x - title.x * 0.5f, c.y - ring_out - fs * 1.6f), dim, "Snap");
  /* Blender's icons: the pointer (Selection to ...) and the cursor's ring and ticks (Cursor to ...). */
  const auto draw_icon = [&](ImVec2 at, bool cursor, ImU32 col) {
    const float r = icon * 0.5f;
    const ImVec2 o(at.x + r, at.y);
    if (cursor) {
      dl->AddCircle(o, r * 0.55f, col, 16, 1.2f * scale + 0.3f);
      for (const auto &[x, y] : {std::pair{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}}) {
        dl->AddLine(ImVec2(o.x + x * r * 0.55f, o.y + y * r * 0.55f), ImVec2(o.x + x * r, o.y + y * r), col,
                    1.2f * scale + 0.3f);
      }
    }
    else {
      const ImVec2 tip(o.x - r * 0.45f, o.y - r * 0.8f);
      const ImVec2 pts[] = {tip, ImVec2(tip.x, tip.y + r * 1.45f), ImVec2(tip.x + r * 0.38f, tip.y + r * 1.1f),
                            ImVec2(tip.x + r * 0.62f, tip.y + r * 1.6f), ImVec2(tip.x + r * 0.82f, tip.y + r * 1.5f),
                            ImVec2(tip.x + r * 0.58f, tip.y + r * 1.0f), ImVec2(tip.x + r * 1.05f, tip.y + r * 1.0f)};
      dl->AddConcavePolyFilled(pts, 7, col);
    }
  };
  int picked = -1;
  for (int i = 0; i < 8; i++) {
    const Item &it = items[i];
    char number[4];
    std::snprintf(number, sizeof(number), "%d", it.number);
    const float number_w = ImGui::CalcTextSize(number).x;
    const float w = pad + icon + pad * 0.8f + ImGui::CalcTextSize(it.label).x + 1.5f * u + number_w;
    const float px = c.x + it.dx * radius, py = c.y + it.dy * radius;
    const float fx = it.dx > 0.01f ? 0.0f : it.dx < -0.01f ? -1.0f : -0.5f;
    /* Up: sits above the radius; down: below it; the rest centred on it. */
    const float fy = it.dy < -0.99f ? -1.0f : it.dy > 0.99f ? 0.0f : -0.5f;
    const ImVec2 lo(px + fx * w, py + fy * row_h), hi(lo.x + w, lo.y + row_h);
    const bool on = i == lit;
    const float round = 0.2f * u;
    dl->AddRectFilled(lo, hi, on ? inner_lit : inner, round);
    dl->AddRect(lo, hi, outline, round, 0, 1.0f);
    const float mid = (lo.y + hi.y) * 0.5f;
    draw_icon(ImVec2(lo.x + pad, mid), std::strncmp(it.idname, "view.snap_cursor", 16) == 0, on ? text_lit : text);
    dl->AddText(ImVec2(lo.x + pad + icon + pad * 0.8f, mid - fs * 0.5f), on ? text_lit : text, it.label);
    dl->AddText(ImVec2(hi.x - pad - number_w, mid - fs * 0.5f), dim, number);
    /* Its number, on the keyboard's top row or the keypad. */
    char keypad[16];
    std::snprintf(keypad, sizeof(keypad), "Keypad %d", it.number);
    if (modal_key_take(C, number, true) || modal_key_take(C, keypad, true)) {
      picked = i;
    }
  }
  if (clicked && lit >= 0) {
    picked = lit;
  }
  /* Shift S held, then let go with an item lit: that one, as Blender's pies. */
  /* Let go of S (seen as it no longer being down, so a release between frames isn't missed). */
  if (g_snap_pie.held) {
    if (lit >= 0) {
      g_snap_pie.dragged = true;
    }
    if (!ImGui::IsKeyDown(ImGuiKey_S)) {
      g_snap_pie.held = false;
      if (picked < 0 && lit >= 0 && (g_snap_pie.dragged || ImGui::GetTime() - g_snap_pie.opened > 0.25)) {
        picked = lit;
      }
    }
  }
  if (picked >= 0) {
    snap_pie_close(C);
    operator_call(C, items[picked].idname, items[picked].args);
    return;
  }
  if (cancel || (clicked && lit < 0)) {
    snap_pie_close(C);
    return;
  }
  /* Other keys while it is open do nothing. */
  wm.modal_keys.clear();
}

static void transform_operators_register()
{
  const auto add = [](const char *idname, const char *name, const char *description, std::vector<OpProperty> props,
                      std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec,
                      unsigned extra_flags = 0) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | extra_flags;
    ot.props = std::move(props);
    ot.poll = std::move(poll);
    ot.exec = std::move(exec);
    operators().add(std::move(ot));
  };
  const auto points_poll = [](Context &C) { return C.document && C.wm && !editable_layers(C).empty(); };
  const auto bones_poll = [](Context &C) {
    const Layer *a = C.document ? document_active_layer(*C.document) : nullptr;
    return C.wm && a && a->is_bone_layer() && !bones_moving(*C.document, *a).empty();
  };
  const OpProperty move_x = panel_float("move_x", "Move X", "How far across", "0", -1e6f, 1e6f, 0.01f);
  const OpProperty move_y = panel_float("move_y", "Move Y", "How far up", "0", -1e6f, 1e6f, 0.01f);
  const OpProperty angle = panel_float("angle", "Angle", "Degrees, counter-clockwise", "0", -36000.0f, 36000.0f, 0.5f);
  const OpProperty scale_x = panel_float("scale_x", "Scale X", "Across", "1", -1e4f, 1e4f, 0.01f);
  const OpProperty scale_y = panel_float("scale_y", "Scale Y", "Up", "1", -1e4f, 1e4f, 0.01f);
  const OpProperty keep_points = keep_poses_option("setup", "1");
  const OpProperty keep_bones = keep_poses_option("posing", "0");
  add("mesh.translate", "Move",
      "Move the selected points by move_x, move_y (as G does). On the setup frame keep_poses=1 leaves the other "
      "frames' keyed poses where they are (setup=1 shows it in the panel)",
      {move_x, move_y, keep_points}, points_poll, [](Context &C) { return points_transform_exec(C, 'G'); });
  add("mesh.radius_scale", "Scale Radius",
      "Scale the line's width at the selected points by factor= (as Alt S does: Blender's Shrink/Fatten)",
      {panel_float("factor", "Factor", "How much wider (below 1: thinner)", "1", 0.0f, 100.0f, 0.01f)}, points_poll,
      [](Context &C) {
        const float factor = std::max(op_arg_float(C, "factor", 1.0f), 0.0f);
        int changed = 0;
        for (Layer *layer : editable_layers(C)) {
          if (layer->is_reference() && !layer->reference.edit_points) {
            continue;
          }
          for (MeshPoint &p : layer->drawing.points) {
            if (p.selected) {
              p.radius = std::clamp(p.radius * factor, 0.0f, POINT_RADIUS_MAX);
              changed++;
            }
          }
        }
        C.undo_name = "Point Radius";
        return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
      },
      OPTYPE_MIRROR);
  {
    OpProperty repeat;
    repeat.name = "repeat";
    repeat.label = "Repeat";
    repeat.description = "How many times to smooth";
    repeat.kind = OpProperty::Kind::Int;
    repeat.fallback = "1";
    repeat.min = 1.0f;
    repeat.max = 100.0f;
    repeat.step = 1.0f;
    add("mesh.smooth_points", "Smooth Points",
        "Smooth the selected points (right-click; Blender's Smooth Vertices, for curves): each goes factor= of the "
        "way to its neighbours' middle along its curves, repeat= times, and its handles as far toward smooth ones; "
        "position=, handles=, radius= say what is smoothed, keep_shape=1 keeps shapes from shrinking, ends=1 smooths "
        "open curves' ends too; keep_poses= as Move",
        {panel_float("factor", "Smoothing", "How far toward the neighbours' middle and smooth handles", "0.5", 0.0f,
                     1.0f, 0.01f),
         repeat, op_bool_property("position", "Position", "Move the points", "1"),
         op_bool_property("handles", "Handles", "Turn and size the handles toward smooth ones", "1"),
         op_bool_property("radius", "Radius", "Even out the line's width", "0"),
         op_bool_property("keep_shape", "Keep Shape", "Push back out after each step, so shapes don't shrink", "0"),
         op_bool_property("ends", "Smooth Endpoints", "Smooth open curves' ends too", "0"), keep_points},
        points_poll, [](Context &C) { return smooth_points_exec(C); });
  }
  add("mesh.slide", "Slide",
      "Slide the selected points along their curves (G G): factor= of the way to the next point (negative: toward "
      "the one before), the curve keeping its shape; keep_poses= as Move",
      {panel_float("factor", "Factor", "How far toward the next point (negative: the one before)", "0", -1.0f, 1.0f,
                   0.01f),
       keep_points},
      points_poll, [](Context &C) {
        const SetupSnapshot kept = keep_poses_begin(C, editable_layer_ids(C));
        const PartnerMirror partners =
            layer_mirror_partners_begin(*C.document, editable_layer_ids(C), C.wm->tool_settings.mirror_x);
        const float factor = op_arg_float(C, "factor", 0.0f);
        int moved = 0;
        for (Layer *layer : editable_layers(C)) {
          moved += drawing_slide_points(layer->drawing, drawing_selected_points(layer->drawing), factor);
        }
        layer_mirror_partners_end(*C.document, partners);
        keep_poses_end(C, kept);
        return moved > 0 ? OpStatus::Finished : OpStatus::Cancelled;
      });
  add("mesh.rotate", "Rotate",
      "Turn the selected points by angle= degrees about the pivot (as R does); keep_poses= as Move",
      {angle, keep_points}, points_poll, [](Context &C) { return points_transform_exec(C, 'R'); });
  add("mesh.resize", "Scale",
      "Scale the selected points by scale_x, scale_y about the pivot (as S does); keep_poses= as Move",
      {scale_x, scale_y, keep_points}, points_poll, [](Context &C) { return points_transform_exec(C, 'S'); });
  add("armature.translate", "Move Bones",
      "Move the selected bones by move_x, move_y (document space; as G does). Posing, only_bones=1 keeps the "
      "drawings where they are (posing=1 shows it in the panel)",
      {move_x, move_y, only_bones_option(), without_children_option(), keep_bones}, bones_poll,
      [](Context &C) { return bones_transform_exec(C, 'G'); });
  add("armature.rotate", "Rotate Bones", "Turn the selected bones by angle= degrees (as R does); only_bones= as Move Bones",
      {angle, only_bones_option(), without_children_option(), keep_bones}, bones_poll,
      [](Context &C) { return bones_transform_exec(C, 'R'); });
  add("armature.resize", "Scale Bones",
      "Scale the selected bones by scale= (as S does; axis=1 or 2 along the bones' X or Y, squash=1 squash and "
      "stretch); only_bones= as Move Bones",
      {panel_float("scale", "Scale", "How much bigger", "1", -1e4f, 1e4f, 0.01f), only_bones_option(),
       without_children_option(), keep_bones},
      bones_poll,
      [](Context &C) { return bones_transform_exec(C, 'S'); });
  add("armature.cage_transform", "Distort Bones",
      "Bend the selected bones (their bases and tips) by a cage in the bone layer's space, as the Transform Bones "
      "tool's Distort, Warp and edge shear do: the box lo_x, lo_y, hi_x, hi_y split into cells= each way (1-8), "
      "nodes= its (cells + 1)^2 corners where they go (x y each, row by row from the bottom, left to right), "
      "box_angle= degrees the box is turned (its space the layer's turned that much); name= the undo step's "
      "name; only_bones= as Move Bones",
      {only_bones_option(), keep_bones}, bones_poll, [](Context &C) { return bones_transform_exec(C, 'C'); });
  add("armature.transform", "Transform Bones",
      "The Transform Bones tool's move: the selected bones moved by move_x, move_y, scaled by scale_x, scale_y and "
      "turned by angle= degrees about origin_x, origin_y (bone layer space); only_bones= as Move Bones",
      {move_x, move_y, scale_x, scale_y, angle, only_bones_option(), keep_bones}, bones_poll,
      [](Context &C) { return bones_transform_exec(C, 'T'); });

  /* Alt G / Alt R / Alt S: the selected bones' pose cleared (away from frame 0), with the bone moves'
   * options (shown always: these never run on the setup frame) and how much of it goes. */
  const OpProperty amount = op_float_property("amount", "Amount",
                                              "How much of the change from the setup pose goes: 1 all of it", "1",
                                              0.0f, 1.0f, 0.01f);
  const OpProperty clear_x = op_bool_property("x", "X", "Clear across (x)", "1");
  const OpProperty clear_y = op_bool_property("y", "Y", "Clear up (y)", "1");
  OpProperty only = only_bones_option(), without = without_children_option();
  only.visible = nullptr;
  without.visible = nullptr;
  for (const auto &[idname, name, what] :
       {std::tuple{"armature.clear_location", "Clear Location", 0}, std::tuple{"armature.clear_rotation", "Clear Rotation", 1},
        std::tuple{"armature.clear_scale", "Clear Scale", 2}})
  {
    const int kind = what;
    std::vector<OpProperty> props = {amount};
    if (kind == 0) {
      props.push_back(clear_x);
      props.push_back(clear_y);
    }
    props.push_back(only);
    props.push_back(without);
    add(idname, name,
        kind == 0   ? "Put the selected bones back where they are in the setup pose (Alt G, away from frame 0): amount= "
                      "how much, x= / y= which ways (along the bone's rest axes); only_bones= and without_children= "
                      "as Move Bones"
        : kind == 1 ? "Turn the selected bones back to their setup angle (Alt R, away from frame 0); amount=, "
                      "only_bones=, without_children= as Clear Location"
                    : "Give the selected bones back their setup scale (Alt S, away from frame 0); amount=, only_bones=, "
                      "without_children= as Clear Location",
        props,
        [](Context &C) {
          const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
          return l && l->is_bone_layer() && C.document->frame_current != 0 && armature_any_picked(*l);
        },
        [kind](Context &C) { return bones_clear_exec(C, kind); });
  }
}

/* Add Box, Add Circle and Add Grid: what the Shape tool's drag makes, as operators the ViewportActionPanel can
 * run again with other options (Blender's Add Mesh panel). */
/* The Add menu's (Shift A) can be added on frame 0 to a vector layer that isn't a reference. */
static bool primitive_add_poll(Context &C)
{
  Layer *layer = nullptr;
  return C.document && C.wm && editable_drawing(C, &layer) && layer && !layer->is_reference() &&
         C.document->frame_current == 0;
}

static void primitive_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "mesh.primitive_point_add";
    ot.name = "Add Point";
    ot.description = "Add a single point to the active vector layer at x, y (the layer's space), to draw on from "
                     "with Extrude; it becomes the selection";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.props = {panel_float("x", "Location X", "Where, across (the layer's space)", "0", -1e4f, 1e4f, 0.01f),
                panel_float("y", "Y", "Where, up (the layer's space)", "0", -1e4f, 1e4f, 0.01f)};
    ot.poll = primitive_add_poll;
    ot.exec = [](Context &C) {
      VectorDrawing *drawing = editable_drawing(C);
      if (!drawing) {
        return OpStatus::Cancelled;
      }
      VectorDrawing point;
      point.points.push_back({float2(op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f))});
      drawing_append_selected(*drawing, point);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mesh.primitive_line_add";
    ot.name = "Add Line";
    ot.description = "Add a straight line to the active vector layer: two points, its middle at x, y and `length` "
                     "long, turned by `angle` degrees (the layer's space), drawn in the Draw tool's look; it becomes "
                     "the selection";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.props = {panel_float("x", "Location X", "Its middle, across (the layer's space)", "0", -1e4f, 1e4f, 0.01f),
                panel_float("y", "Y", "Its middle, up (the layer's space)", "0", -1e4f, 1e4f, 0.01f),
                panel_float("length", "Length", "How long", "1", 1e-4f, 1e4f, 0.01f),
                panel_float("angle", "Angle", "Turned this many degrees from level", "0", -360.0f, 360.0f, 1.0f)};
    ot.poll = primitive_add_poll;
    ot.exec = [](Context &C) {
      VectorDrawing *drawing = editable_drawing(C);
      if (!drawing) {
        return OpStatus::Cancelled;
      }
      const float2 middle(op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f));
      const float half = std::abs(op_arg_float(C, "length", 1.0f)) * 0.5f;
      const float angle = op_arg_float(C, "angle", 0.0f) * PI / 180.0f;
      if (half < 1e-5f) {
        report(ReportType::Warning, "Too small to draw");
        return OpStatus::Cancelled;
      }
      const float2 along(std::cos(angle) * half, std::sin(angle) * half);
      VectorDrawing line;
      line.points.push_back({middle - along});
      line.points.push_back({middle + along});
      Curve curve;
      curve.points.push_back({0});
      curve.points.push_back({1});
      line.curves.push_back(std::move(curve));
      Shape shape;
      shape.name = "Line";
      shape.style = C.wm->tool_settings.style;
      shape.has_fill = false;
      shape.edges.push_back({0, 0});
      line.shapes.push_back(std::move(shape));
      drawing_append_selected(*drawing, line);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* Shift A: Blender's Add menu, at the mouse; what it adds goes at the 2D cursor. */
    OperatorType ot;
    ot.idname = "mesh.add_menu";
    ot.name = "Add";
    ot.description = "Blender's Add menu (Shift A): a Point, Line, Square or Circle at the 2D cursor";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) {
      Layer *layer = nullptr;
      return C.document && C.wm && editable_drawing(C, &layer) && layer && !layer->is_reference();
    };
    ot.exec = [](Context &C) {
      C.wm->menu_request = "mesh_add";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  for (const PrimitiveKind kind : PRIMITIVE_KINDS) {
    OperatorType ot;
    const std::string name = primitive_name(kind);
    ot.idname = kind == PrimitiveKind::Grid     ? "mesh.primitive_grid_add"
                : kind == PrimitiveKind::Circle ? "mesh.primitive_circle_add"
                                                : "mesh.primitive_box_add";
    ot.name = "Add " + name;
    ot.description = "Add a " + name + " to the active vector layer as the Shape tool draws one: its middle at x, y "
                     "and its width and height (the layer's space)" +
                     (kind == PrimitiveKind::Grid ? std::string(", columns= by rows= cells (1 to 32)")
                                                  : std::string(", fill=1 to fill it")) +
                     "; the new points become the selection";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    OpProperty columns, rows;
    for (OpProperty *p : {&columns, &rows}) {
      p->kind = OpProperty::Kind::Int;
      p->fallback = "3";
      p->min = 1.0f;
      p->max = float(GRID_CELLS_MAX);
      p->step = 1.0f;
    }
    columns.name = "columns";
    columns.label = "Columns";
    columns.description = "How many cells across";
    rows.name = "rows";
    rows.label = "Rows";
    rows.description = "How many cells up";
    std::vector<OpProperty> props;
    if (kind == PrimitiveKind::Grid) {
      props = {columns, rows};
    }
    props.push_back(panel_float("x", "Location X", "Its middle, across (the layer's space)", "0", -1e4f, 1e4f, 0.01f));
    props.push_back(panel_float("y", "Y", "Its middle, up (the layer's space)", "0", -1e4f, 1e4f, 0.01f));
    props.push_back(panel_float("width", "Width", "How wide", "1", 1e-4f, 1e4f, 0.01f));
    props.push_back(panel_float("height", "Height", "How tall", "1", 1e-4f, 1e4f, 0.01f));
    if (kind != PrimitiveKind::Grid) {
      props.push_back(op_bool_property("fill", "Fill", "Fill it as well as outlining it", "0"));
    }
    ot.props = std::move(props);
    ot.poll = [](Context &C) {
      Layer *layer = nullptr;
      return C.document && C.wm && editable_drawing(C, &layer) && layer && !layer->is_reference() &&
             C.document->frame_current == 0;
    };
    ot.exec = [kind](Context &C) {
      Layer *layer = nullptr;
      VectorDrawing *drawing = editable_drawing(C, &layer);
      if (!drawing) {
        return OpStatus::Cancelled;
      }
      ToolSettings &ts = C.wm->tool_settings;
      const float2 middle(op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f));
      const float2 half(std::abs(op_arg_float(C, "width", 1.0f)) * 0.5f, std::abs(op_arg_float(C, "height", 1.0f)) * 0.5f);
      const int columns = std::clamp(op_arg_int(C, "columns", ts.grid_columns), 1, GRID_CELLS_MAX);
      const int rows = std::clamp(op_arg_int(C, "rows", ts.grid_rows), 1, GRID_CELLS_MAX);
      const bool fill = op_arg_bool(C, "fill");
      const VectorDrawing shape = primitive_drawing(kind, {middle - half, middle + half}, ts.style, fill, columns, rows);
      if (shape.curves.empty()) {
        report(ReportType::Warning, "Too small to draw");
        return OpStatus::Cancelled;
      }
      drawing_append_selected(*drawing, shape);
      /* The next one starts from these, as Blender's Add Mesh options are kept. */
      if (kind == PrimitiveKind::Grid) {
        ts.grid_columns = columns;
        ts.grid_rows = rows;
      }
      else if (op_has_arg(C, "fill")) {
        ts.draw_fill = fill;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

void viewport_operators_register()
{
  transform_operators_register();
  snap_operators_register();
  primitive_operators_register();
  OperatorType ot;
  ot.idname = "mesh.transform_selected";
  ot.name = "Transform Points";
  ot.description = "Move, scale and turn the selected points (and handles picked on their own) about a pivot, in the "
                   "active layer's space: move_x/move_y, scale_x/scale_y, angle (degrees), pivot_x/pivot_y (default: "
                   "the middle of the selection), as the Transform Points tool's header does";
  ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot.props = {keep_poses_option("setup", "1")};
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
    if (!modal_collect(C, area, 'G', {}, moving, false)) {
      return OpStatus::Cancelled;
    }
    const SetupSnapshot kept = keep_poses_begin(C, editable_layer_ids(C));
    point_gizmo_place(C, moving, to_document, map_about(pivot, move, scale, angle));
    keep_poses_end(C, kept);
    return OpStatus::Finished;
  };
  operators().add(std::move(ot));

  OperatorType cage_op;
  cage_op.idname = "mesh.cage_transform";
  cage_op.name = "Distort Points";
  cage_op.description = "Bend the selected points (and handles) by a cage in the active layer's space, as the "
                        "Transform Points tool's Distort, Warp and edge shear do: the box lo_x, lo_y, hi_x, hi_y split "
                        "into cells= each way (1-8), nodes= its (cells + 1)^2 corners where they go (x y each, row by "
                        "row from the bottom, left to right), box_angle= degrees the box is turned (its space the layer's "
                        "turned that much); name= the undo step's name";
  cage_op.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  cage_op.props = {keep_poses_option("setup", "1")};
  cage_op.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr && !editable_layers(C).empty(); };
  cage_op.exec = [](Context &C) {
    GizmoCage cage;
    if (!cage_from_args(C, cage)) {
      return OpStatus::Cancelled;
    }
    static Area area;
    ModalTransform moving;
    if (!modal_collect(C, area, 'G', {}, moving, false)) {
      return OpStatus::Cancelled;
    }
    const SetupSnapshot kept = keep_poses_begin(C, editable_layer_ids(C));
    /* The cage's box may be turned (box_angle, degrees): its space is the layer's turned that much. */
    const Transform2D space = point_gizmo_space(*C.document) *
                              Transform2D::rotate(op_arg_float(C, "box_angle", 0.0f) * PI / 180.0f);
    point_gizmo_place(C, moving, space, [&](float2 p) { return cage.map(p); });
    keep_poses_end(C, kept);
    C.undo_name = op_arg(C, "name", "Distort Points");
    return OpStatus::Finished;
  };
  operators().add(std::move(cage_op));

  /* Mirror (right-click on points, Blender's): flip the selection across X or Y, along the document's
   * axes through each layer's origin (Global), or the active layer's own axes through the selection's
   * middle (Local). */
  OperatorType mirror;
  mirror.idname = "transform.mirror";
  mirror.name = "Mirror";
  mirror.description = "Flip the selected points (and handles picked on their own): axis=X (left and right swap) "
                       "or Y (up and down), orientation=GLOBAL (the document's axes, through each point's layer's "
                       "origin) or LOCAL (the active layer's own axes, turning with it, through the selection's "
                       "middle)";
  mirror.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  mirror.poll = ot.poll;
  OpProperty axis;
  axis.name = "axis";
  axis.label = "Axis";
  axis.description = "X swaps left and right, Y up and down";
  axis.items = {{"X", "X"}, {"Y", "Y"}};
  axis.fallback = "X";
  OpProperty orientation;
  orientation.name = "orientation";
  orientation.label = "Orientation";
  orientation.description = "The document's axes, or the active layer's own (turning with it)";
  orientation.items = {{"GLOBAL", "Global"}, {"LOCAL", "Local"}};
  orientation.fallback = "GLOBAL";
  mirror.props = {axis, orientation};
  mirror.exec = [](Context &C) {
    const std::string axis = op_arg(C, "axis", "X"), orientation = op_arg(C, "orientation", "GLOBAL");
    if ((axis != "X" && axis != "Y") || (orientation != "GLOBAL" && orientation != "LOCAL")) {
      report(ReportType::Error, "axis is X or Y; orientation is GLOBAL or LOCAL");
      return OpStatus::Cancelled;
    }
    const Transform2D to_document = orientation == "LOCAL" ? point_gizmo_space(*C.document) : Transform2D();
    float2 lo, hi;
    if (!point_gizmo_bounds(C, to_document.inverted(), lo, hi)) {
      return OpStatus::Cancelled;
    }
    static Area area;
    ModalTransform moving;
    if (!modal_collect(C, area, 'G', {}, moving, false)) {
      return OpStatus::Cancelled;
    }
    const float2 flip = axis == "X" ? float2(-1.0f, 1.0f) : float2(1.0f, -1.0f);
    if (orientation == "LOCAL") {
      point_gizmo_place(C, moving, to_document, map_about((lo + hi) * 0.5f, {}, flip, 0.0f));
    }
    else {
      /* Global: across the document's axes through each point's layer's origin. */
      ModalLayers layers;
      if (!modal_layers(C, moving, layers)) {
        return OpStatus::Cancelled;
      }
      for (const ModalTransform::Item &item : moving.items) {
        const float w = modal_weight(C.wm->tool_settings, item);
        if (w <= 0.0f) {
          modal_rest(layers, item);
          continue;
        }
        const float2 origin = layer_world_transform(*C.document, item.layer).apply(float2(0.0f, 0.0f));
        const float2 d = item.start - origin;
        const float2 to = origin + float2(d.x * flip.x, d.y * flip.y);
        modal_place(layers, item, item.start + (to - item.start) * w);
      }
      modal_mirror(C, moving, layers);
    }
    C.undo_name = "Mirror " + axis + (orientation == "LOCAL" ? " Local" : " Global");
    return OpStatus::Finished;
  };
  operators().add(std::move(mirror));
}

/* A cage (the active layer's space) on the selected points: mesh.cage_transform, one undo step named `name`. */
static void point_gizmo_cage(Context &C, const GizmoCage &cage, const char *name, float box_angle = 0.0f)
{
  OpArgs args = {{"setup", C.document->frame_current == 0 ? "1" : "0"},
                 {"keep_poses", keep_poses_on(*C.document) ? "1" : "0"},
                 {"name", name}};
  cage_args(cage, args, box_angle);
  operator_call(C, "mesh.cage_transform", args);
}

static void transform_points_header(Context &C)
{
  Document &doc = *C.document;
  gizmo_mode_buttons(C, "points_mode");
  ImGui::SameLine();
  const PointGizmo &g = g_point_gizmo;
  const Transform2D to_document = point_gizmo_space(doc);
  const Transform2D turn = Transform2D::rotate(g.box_angle);
  float2 lo, hi, rlo, rhi;
  const bool any = point_gizmo_bounds(C, to_document.inverted(), lo, hi) &&
                   point_gizmo_bounds(C, (to_document * turn).inverted(), rlo, rhi);
  /* The pivot is kept in the (maybe turned) box's space; the fields are in the layer's. */
  const float2 pivot = any ? turn.apply((rlo + rhi) * 0.5f + g.pivot_offset) : float2();
  const bool dragging = gizmo_moving(g.mode);
  GizmoLive live = g.live;
  live.offset = turn.apply_vector(live.offset);
  gizmo_header_fields(
      "points", any, dragging ? turn.apply(g.pivot) : pivot, dragging, live,
      [&](float2 move, float2 scale, float degrees) { point_gizmo_apply(C, pivot, move, scale, degrees); },
      [&](float2 shear) { point_gizmo_cage(C, cage_typed_shear(lo, hi, pivot, shear), "Shear Points"); });
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
  PointGizmo &g = g_point_gizmo;
  const uint64_t selection = document_selection_hash(doc);
  if ((g.layer != doc.active_layer || g.selection != selection) && g.mode == GizmoMode::None) {
    g.pivot_offset = {};
    g.box_angle = 0.0f;
    g.box_turned = 0.0f;
    g.layer = doc.active_layer;
    g.selection = selection;
  }
  if (g.box_turned != 0.0f && g.mode == GizmoMode::None && C.undo && C.undo->change_count() != g.turned_changes) {
    g.box_angle -= g.box_turned;
    g.box_turned = 0.0f;
    g.pivot_offset = {};
  }
  /* The gizmo's space: the active layer's, turned as far as the box is. */
  const Transform2D base_space = point_gizmo_space(doc);
  const Transform2D to_document = base_space * Transform2D::rotate(g.box_angle);
  const Transform2D to_gizmo = to_document.inverted();
  const float2 mouse = to_gizmo.apply(view.to_document(io.MousePos));
  float2 lo, hi;
  /* With the overlays off (Shift Z) the box is gone: not drawn, and clicks select. */
  const bool any = doc.overlays.show && point_gizmo_bounds(C, to_gizmo, lo, hi);
  const float2 pivot = (lo + hi) * 0.5f + g.pivot_offset;
  if (any) {
    gizmo_box(to_document, view, lo, hi);
  }
  /* The cage Distort and Warp show: the one kept from the last drag, or a fresh one on the box. */
  GizmoCage cage;
  const bool kept_cage = g.session.still(C, doc.active_layer, gizmo_key(selection, g.box_angle));
  if (g.mode == GizmoMode::Cage) {
    cage = g.cage_now;
  }
  else if (kept_cage) {
    cage = g.session.cage;
  }
  else {
    g.session.active = false;
    cage.reset(lo, hi, ts.gizmo_mode == 2 ? ts.warp_cells : 1);
  }

  /* Press: what the mouse is on decides the drag. A double-click on the pivot puts it back. */
  if (hovered && g.mode == GizmoMode::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    int handle = 0;
    GizmoMode mode = any ? gizmo_hit(ts.gizmo_mode, cage, to_document, view, io.MousePos, mouse, lo, hi, pivot,
                                     io.KeyCtrl, &handle)
                         : GizmoMode::Select;
    if (mode == GizmoMode::Pivot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      g.pivot_offset = {};
      mode = GizmoMode::None;
    }
    if (mode == GizmoMode::Orient) {
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        /* Square with the layer again. */
        g.box_angle = 0.0f;
        g.box_turned = 0.0f;
        g.pivot_offset = {};
        mode = GizmoMode::None;
      }
      else {
        const float2 mb = base_space.inverted().apply(view.to_document(io.MousePos));
        g.orient_centre = Transform2D::rotate(g.box_angle).apply((lo + hi) * 0.5f);
        g.orient_ref = std::atan2(mb.y - g.orient_centre.y, mb.x - g.orient_centre.x);
        g.orient_start = g.box_angle;
        g.orient_pivot = to_document.apply(pivot);
        g.orient_offset = g.pivot_offset;
      }
    }
    if (gizmo_moving(mode)) {
      g.before = doc;
      if (mode == GizmoMode::Cage) {
        g.cage_start = cage;
        g.cage_now = cage;
        g.grab = handle == -2 ? cage.unmap(mouse) : mouse;
        /* A kept cage goes on bending the points from where they were before its first drag. */
        if (kept_cage && ts.gizmo_mode != 0) {
          const int frame = doc.frame_current;
          doc = g.session.base;
          doc.frame_current = frame;
        }
      }
      g.moving = ModalTransform();
      if (!modal_collect(C, area, 'G', view.to_document(io.MousePos), g.moving)) {
        mode = GizmoMode::None;
        const int frame = doc.frame_current;
        doc = std::move(g.before);
        doc.frame_current = frame;
        g.before = Document();
      }
      g.kept = doc.frame_current == 0 && keep_poses_on(*C.document) ? setup_snapshot(doc, editable_layer_ids(C)) : SetupSnapshot();
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
    g.live = {};
    g.live_map = Transform2D();
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
        if (released && !dragged && io.KeyAlt) {
          /* An Alt-click on the pivot picks the layer there; the pivot stays. */
          g.mode = GizmoMode::None;
          layer_pick_click(C, view, g.start_screen, io.KeyShift, false);
          break;
        }
        g.pivot_offset = target - (g.lo + g.hi) * 0.5f;
        g.live_pivot = target;
        if (released || cancel) {
          g.mode = GizmoMode::None;
        }
        break;
      }
      case GizmoMode::Orient: {
        /* The box turns about its middle as the knob is swung round it (Shift: steps of 15 degrees); what it
         * holds stays, and so does the pivot. */
        const float2 mb = base_space.inverted().apply(view.to_document(io.MousePos));
        float a = std::remainder(
            g.orient_start + std::atan2(mb.y - g.orient_centre.y, mb.x - g.orient_centre.x) - g.orient_ref, 2.0f * PI);
        if (io.KeyShift) {
          a = std::round(a / (PI / 12.0f)) * (PI / 12.0f);
        }
        if (cancel) {
          a = g.orient_start;
        }
        g.box_angle = a;
        g.live.degrees = a * 180.0f / PI;
        const Transform2D now = base_space * Transform2D::rotate(a);
        float2 nlo, nhi;
        if (cancel) {
          g.pivot_offset = g.orient_offset;
        }
        else if (point_gizmo_bounds(C, now.inverted(), nlo, nhi)) {
          g.pivot_offset = now.inverted().apply(g.orient_pivot) - (nlo + nhi) * 0.5f;
        }
        if (released || cancel) {
          g.mode = GizmoMode::None;
        }
        break;
      }
      default: {
        const auto put_back = [&]() {
          const int frame = doc.frame_current;
          doc = std::move(g.before);
          doc.frame_current = frame;
          g.before = Document();
          g.moving = ModalTransform();
          g.mode = GizmoMode::None;
        };
        if (cancel) {
          put_back();
          break;
        }
        if (g.mode == GizmoMode::Cage) {
          g.cage_now = gizmo_cage_dragged(g.cage_start, g.handle, g.grab, mouse - g.start);
          const GizmoCage &bent = g.cage_now;
          point_gizmo_place(C, g.moving, to_document, [&](float2 p) { return bent.map(p); });
        }
        else {
          if (g.mode == GizmoMode::Rotate) {
            g.turned += turn_between(g.last, mouse, g.pivot);
            g.last = mouse;
          }
          float2 snap, origin;
          const bool snapping = g.mode == GizmoMode::Move && io.KeyCtrl && nearest_point(C, to_gizmo, mouse, snap);
          g.live_map = gizmo_box_map(g.mode, g.handle, g.lo, g.hi, g.pivot, g.start, mouse, g.turned,
                                     snapping ? &snap : nullptr, g.live_pivot, g.live, origin);
          point_gizmo_place(C, g.moving, to_document, g.live_map);
        }
        g.changed |= dragged;
        if (released) {
          if (!g.changed) {
            put_back();
            /* An Alt-click on the box (no drag) picks the layer there, as Alt-click does anywhere; Alt-drags
             * keep their meaning. */
            if (io.KeyAlt) {
              layer_pick_click(C, view, g.start_screen, io.KeyShift, false);
            }
            break;
          }
          const GizmoMode mode = g.mode;
          if (mode == GizmoMode::Cage || mode == GizmoMode::Shear) {
            /* As one step from where the points were before (a kept cage: before its first drag), which
             * the ViewportActionPanel can't take apart but Python can make again. */
            GizmoCage bent = g.cage_now;
            if (mode == GizmoMode::Shear) {
              bent.reset(g.lo, g.hi, 1);
              for (float2 &p : bent.nodes) {
                p = g.live_map.apply(p);
              }
            }
            const bool kept_on = mode == GizmoMode::Cage && ts.gizmo_mode != 0;
            Document base = g.moving.snapshot;
            const int frame = doc.frame_current;
            doc = base;
            doc.frame_current = frame;
            g.moving = ModalTransform();
            g.mode = GizmoMode::None;
            point_gizmo_cage(C, bent,
                             mode == GizmoMode::Shear ? "Shear Points"
                             : ts.gizmo_mode == 2    ? "Warp Points"
                                                     : "Distort Points",
                             g.box_angle);
            if (kept_on) {
              g.session.base = std::move(base);
              g.session.keep(C, C.document->active_layer,
                             gizmo_key(document_selection_hash(*C.document), g.box_angle), bent);
            }
          }
          else {
            if (!g.kept.empty()) {
              animation_record(doc);
              keep_poses(doc, g.kept);
            }
            push_undo(C, "Transform Points");
          }
          g.before = Document();
          /* The pivot stays where the drag left it; a turn turns the box too (as Illustrator's and Affinity's
           * do), so it stays round what it holds. */
          const float2 pivot_now = to_document.apply(g.live_pivot);
          if (mode == GizmoMode::Rotate) {
            g.box_angle = std::remainder(g.box_angle + g.live.degrees * PI / 180.0f, 2.0f * PI);
            g.box_turned += g.live.degrees * PI / 180.0f;
          }
          g.turned_changes = C.undo ? C.undo->change_count() : 0;
          const Transform2D now = base_space * Transform2D::rotate(g.box_angle);
          float2 nlo, nhi;
          if (point_gizmo_bounds(C, now.inverted(), nlo, nhi)) {
            g.pivot_offset = now.inverted().apply(pivot_now) - (nlo + nhi) * 0.5f;
          }
          g.selection = document_selection_hash(*C.document);
          g.moving = ModalTransform();
          g.mode = GizmoMode::None;
        }
        break;
      }
    }
  }

  /* The cursor says what a press would do; a right-click on the box opens its menu. */
  GizmoMode hot = g.mode;
  int hot_handle = g.handle;
  if (hovered && g.mode == GizmoMode::None && any) {
    hot = gizmo_hit(ts.gizmo_mode, cage, to_document, view, io.MousePos, mouse, lo, hi, pivot, io.KeyCtrl, &hot_handle);
    gizmo_hover_cursor(dl, hot, hot_handle);
  }
  const bool menu = hovered && g.mode == GizmoMode::None && any && hot != GizmoMode::Select &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  g_right_click_spent |= menu;
  if (gizmo_context_menu(C, "##points_gizmo_menu", menu, g.pivot_offset != float2() || g.box_angle != 0.0f)) {
    g.pivot_offset = {};
    g.box_angle = 0.0f;
    g.box_turned = 0.0f;
  }

  /* The gizmo: while dragging, the box as it was pressed carried by the drag, and a grey dot where the
   * pivot was. */
  if (g.mode == GizmoMode::Select || !doc.overlays.show || !any) {
    return;
  }
  const bool dragging = g.mode != GizmoMode::None && g.mode != GizmoMode::Orient;
  const ImVec2 was = view.to_screen(to_document.apply(g.pivot));
  gizmo_draw_box(dl, view, dragging ? to_document * g.live_map : to_document, ts.gizmo_mode,
                 g.mode == GizmoMode::Cage ? g.cage_now : cage, dragging ? g.lo : lo, dragging ? g.hi : hi,
                 !dragging ? pivot : g.mode == GizmoMode::Pivot ? g.live_pivot : g.pivot, dragging ? &was : nullptr,
                 hot, hot_handle, dragging);
  if (g.mode != GizmoMode::None && g.mode != GizmoMode::Pivot) {
    gizmo_readout(dl, io.MousePos, gizmo_live_text(g.mode, ts.gizmo_mode, g.live).c_str());
  }
}

/* Whether a shown point of an edited layer is under the mouse (screen pixels). */
static bool point_under(Context &C, const ViewMapping &view, ImVec2 mouse)
{
  for (Layer *layer : editable_layers(C)) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const Posed shown = posed(*C.document, *layer);
    for (const MeshPoint &p : shown.drawing->points) {
      const ImVec2 s = view.to_screen(to_document.apply(p.pos));
      if ((s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y) <= pick_line_pixels() * pick_line_pixels()) {
        return true;
      }
    }
  }
  return false;
}

/* Whether a line of an edited layer is under the mouse, where a click picks lines (Edges and Strokes modes). */
static bool line_under(Context &C, const ViewMapping &view, ImVec2 mouse)
{
  if (C.wm->tool_settings.select_mode == SelectMode::Point) {
    return false;
  }
  const float2 mouse_document = view.to_document(mouse);
  for (Layer *layer : editable_layers(C)) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
    const Posed shown = posed(*C.document, *layer);
    const VectorDrawing *wires = posed_wires(shown);
    const VectorDrawing lines = wires ? *wires : drawing_detail_shown_curves(*shown.drawing);
    ShapeEdge edge;
    float t;
    if (drawing_pick_segment(lines, to_document.inverted().apply(mouse_document), pick_line_pixels() * pixel, &edge,
                             &t))
    {
      return true;
    }
  }
  return false;
}

/* A point, a shown handle or (outside Points mode) a line of the drawings being edited, under the mouse. */
static bool vector_under(Context &C, const ViewMapping &view, ImVec2 mouse)
{
  return point_under(C, view, mouse) ||
         pick_handle(C, view, mouse, handles_shown(C.document->overlays, C.wm->tool_settings)).layer ||
         line_under(C, view, mouse);
}

/* Alt click in the Motion Path tool (Alt Shift: `extend`): the bone (of the active bone layer and its family) or the
 * point (of the drawings being edited) under the mouse, picked as the Select tool's click picks it, so its path shows.
 * A point wins over a bone's body, a bone's end over a point, as there. False, nothing done, when there is neither. */
static bool motion_path_pick_click(Context &C, const ViewMapping &view, ImVec2 at, bool extend, bool apply)
{
  Document &doc = *C.document;
  if (C.wm->tool_settings.path_edit_layer) {
    /* Edit Layer: the layer itself, never its bones or points. */
    return false;
  }
  if (apply) {
    /* A bone or point picked: no path handles with it. */
    C.wm->path_handle_picks.clear();
  }
  Layer *armature = document_active_layer(doc);
  if (armature && !armature->is_bone_layer()) {
    armature = nullptr;
  }
  const std::vector<Layer *> point_layers = editable_layers(C);
  FamilyHit under;
  if (armature) {
    /* Only asking (apply off): the first hit, the cycle through bones on top of each other left as it is. */
    if (apply) {
      under = pick_family_cycling(doc, *armature, view, at);
    }
    else if (const std::vector<FamilyHit> hits = pick_family_parts(doc, *armature, view, at); !hits.empty()) {
      under = hits.front();
    }
  }
  const bool on_point = !point_layers.empty() && (!under.hit.bone || under.hit.part == BonePart::Body) &&
                        vector_under(C, view, at);
  if (!on_point && !under.hit.bone) {
    return false;
  }
  if (!apply) {
    return true;
  }
  if (on_point) {
    if (!extend && armature) {
      family_deselect_all(doc, *armature);
    }
    /* (It reads Shift itself: held, the point joins the selection.) */
    select_click(C, view, view.to_document(at), handles_shown(doc.overlays, C.wm->tool_settings));
  }
  else {
    Layer *other = under.layer && under.layer->id != armature->id ? under.layer : nullptr;
    if (!extend) {
      family_deselect_all(doc, *armature);
      for (Layer *l : point_layers) {
        deselect_all(l->drawing);
      }
    }
    bone_click_select(other ? *other : *armature, under.hit, extend);
    if (other && !extend && !layer_inside(doc, other->id, armature->id)) {
      doc.active_layer = other->id;
    }
  }
  push_undo(C, on_point ? "Select" : "Select Bone");
  return true;
}

/* The Motion Path tool's Edit brush off the paths: the drawings being edited, as with the Select tool. */
static struct {
  bool active = false;
  int area = 0;
  ImVec2 start;
  /* A point pressed on: a drag moves it, a click picks. None: a click picks a line or clears, a drag is a box. */
  PressGrab grab;
  bool box = false;
} g_path_vector;

static bool motion_path_vector_active()
{
  return g_path_vector.active;
}

/* A press off the paths. A handle nearer than any point is dragged at once (as Blender's tweak); a point, a line or
 * empty space waits to see a click or a drag. False (nothing taken) on a bone's end (it wins over a point), or with
 * no drawing to edit. */
static bool motion_path_vector_press(Context &C, Area &area, const ViewMapping &view, ImVec2 at)
{
  const ImGuiIO &io = ImGui::GetIO();
  if (editable_layers(C).empty() || C.wm->tool_settings.path_edit_layer) {
    return false;
  }
  bool bone = false;
  if (motion_path_pick_click(C, view, at, false, false)) {
    /* Something there: a bone unless the drawing is (motion_path_pick_click's choice, asked again without bones). */
    bone = !vector_under(C, view, at);
    if (!bone) {
      Layer *armature = document_active_layer(*C.document);
      if (armature && armature->is_bone_layer()) {
        const std::vector<FamilyHit> hits = pick_family_parts(*C.document, *armature, view, at);
        bone = !hits.empty() && hits.front().hit.bone && hits.front().hit.part != BonePart::Body;
      }
    }
  }
  if (bone) {
    return false;
  }
  const bool modifier = io.KeyShift || io.KeyCtrl;
  const HandleHit handle = pick_handle(C, view, at, handles_shown(C.document->overlays, C.wm->tool_settings));
  const PressGrab grab = select_press_grab(C, view, at, modifier);
  if (handle.layer && !modifier && grab.distance >= handle.distance) {
    CurvePoint &cp = handle.layer->drawing.curves[handle.curve].points[handle.index];
    if (!(handle.pre ? cp.select_in : cp.select_out)) {
      for (Layer *editing : editable_layers(C)) {
        deselect_all(editing->drawing);
      }
      (handle.pre ? cp.select_in : cp.select_out) = true;
    }
    if (modal_start(C, area, 'G', view.to_document(at))) {
      g_modal.tweak = true;
    }
    return true;
  }
  g_path_vector = {};
  g_path_vector.active = true;
  g_path_vector.area = area.id;
  g_path_vector.start = at;
  g_path_vector.grab = grab;
  return true;
}

/* Until the mouse comes up: a point dragged moves (as G), empty space drags a box; let go without moving, a click. */
static void motion_path_vector_update(Context &C, Area &area, const ViewMapping &view, ImDrawList *dl)
{
  auto &g = g_path_vector;
  const ImGuiIO &io = ImGui::GetIO();
  if (g.area != area.id) {
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    g = {};
    return;
  }
  if (g.grab.layer) {
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, g.grab.threshold)) {
      const PressGrab grab = g.grab;
      const ImVec2 start = g.start;
      g = {};
      select_press_grab_move(C, area, view, grab, start);
      return;
    }
  }
  else {
    g.box |= ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
    if (g.box) {
      dl->AddRectFilled(g.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
      dl->AddRect(g.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
    }
  }
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    return;
  }
  if (g.box) {
    select_box(C, view, g.start, io.MousePos, handles_shown(C.document->overlays, C.wm->tool_settings));
    push_undo(C, "Select");
  }
  else if (vector_under(C, view, g.start)) {
    /* (The click reads Shift and Ctrl itself: adding, taking away.) */
    motion_path_pick_click(C, view, g.start, io.KeyShift || io.KeyCtrl);
  }
  else if (!io.KeyShift && !io.KeyCtrl) {
    /* A click on nothing: no path keys or dots selected; the points picked stay, so their paths do. */
    operator_call(C, "anim.motion_path_select", {{"none", "1"}});
  }
  g = {};
}

/* The Select tool on a bone layer: a click picks a bone or one of its ends; a drag box-selects
 * the ends inside (Shift adds, Ctrl removes), as Blender's Select Box tool does. */
struct BoneBox {
  bool active = false;
  int area = 0;
  ImVec2 start;
  /* A point (of a drawing inside) pressed on, not under a bone's end: dragging it moves it. */
  PressGrab grab;
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
  /* Dragging a controller's corner on the setup frame: how far a frame is, so the corner stays under the mouse. */
  if (g_controller_resize.active) {
    Layer *owner = layer_find(doc, g_controller_resize.layer);
    Bone *knob = owner ? bone_find(*owner, g_controller_resize.bone) : nullptr;
    if (!knob || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      g_controller_resize.active = false;
      push_undo(C, "Resize Controller");
      return;
    }
    const std::vector<BonePose> poses = armature_pose(*owner, true);
    const BonePose *pose = bone_pose_find(poses, knob->id);
    const float2 d = layer_world_transform(doc, owner->id).inverted().apply(view.to_document(io.MousePos)) - pose->head;
    float2 lo, hi;
    controller_box(*knob, lo, hi);
    /* One size for both ways, so the box keeps its shape: the corner goes with whichever way the mouse is further. */
    const float2 frames(g_controller_resize.corner.x > 0.0f ? hi.x : -lo.x, g_controller_resize.corner.y > 0.0f ? hi.y : -lo.y);
    float size = 1e-5f;
    if (frames.x > 0.0f) {
      size = std::max(size, std::abs(d.x) / frames.x);
    }
    if (frames.y > 0.0f) {
      size = std::max(size, std::abs(d.y) / frames.y);
    }
    knob->controller_frame_size = size;
    knob->controller_size_auto = false;
    return;
  }
  if (hovered && C.wm->tool_settings.tool == Tool::Select && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (doc.frame_current == 0 && !io.KeyShift && !io.KeyCtrl) {
      const std::vector<ScreenBone> sbs = screen_bones(doc, armature, view);
      const std::vector<BonePose> poses = armature_pose(armature, true);
      for (size_t i = 0; i < armature.bones.size(); i++) {
        const Bone &knob = armature.bones[i];
        if (!bone_is_controller(knob) || !knob.selected) {
          continue;
        }
        float2 lo, hi;
        controller_box(knob, lo, hi);
        for (const float2 corner : {float2(1, 1), float2(-1, 1), float2(1, -1), float2(-1, -1)}) {
          const ImVec2 at = controller_screen(doc, armature, view, knob, poses[i], sbs[i],
                                              {corner.x > 0.0f ? hi.x : lo.x, corner.y > 0.0f ? hi.y : lo.y});
          if (hi.x > lo.x + 1e-3f && hi.y > lo.y + 1e-3f && std::abs(at.x - io.MousePos.x) <= 6.0f &&
              std::abs(at.y - io.MousePos.y) <= 6.0f)
          {
            g_controller_resize = {true, armature.id, knob.id, corner};
            return;
          }
        }
      }
    }
    /* Clicking a spot of a controller (not under its knob) puts the knob there; Shift keys it held there. */
    if (doc.frame_current != 0 && !io.KeyCtrl) {
      const std::vector<ScreenBone> sbs = screen_bones(doc, armature, view);
      const std::vector<BonePose> poses = armature_pose(armature, false);
      std::string knob_name, spot_name;
      float best = CONTROLLER_SPOT_PICK + 1.0f;
      for (size_t i = 0; i < armature.bones.size() && i < poses.size() && i < sbs.size(); i++) {
        const Bone &knob = armature.bones[i];
        if (!bone_is_controller(knob) || !bone_shown(armature, knob) || bone_held_by_fix(doc, armature, knob)) {
          continue;
        }
        const ImVec2 k = controller_screen(doc, armature, view, knob, poses[i], sbs[i], controller_knob_shown(doc, armature, knob));
        if (std::hypot(k.x - io.MousePos.x, k.y - io.MousePos.y) <= CONTROLLER_KNOB_RADIUS) {
          continue;
        }
        for (const ControllerSpot &spot : knob.controller_spots) {
          const ImVec2 s = controller_screen(doc, armature, view, knob, poses[i], sbs[i], spot.at);
          const float d = std::max(std::abs(s.x - io.MousePos.x), std::abs(s.y - io.MousePos.y));
          if (d <= CONTROLLER_SPOT_PICK && d < best) {
            best = d;
            knob_name = knob.name;
            spot_name = spot.name;
          }
        }
      }
      if (!spot_name.empty()) {
        operator_call(C, "armature.controller_spot_go",
                      {{"bone", knob_name}, {"name", spot_name}, {"hold", io.KeyShift ? "1" : "0"}});
        return;
      }
    }
    /* Pressing on a controller's knob, or on the setup frame its box, drags it at once (Ctrl: onto its keys). */
    if (!io.KeyShift) {
      const std::vector<FamilyHit> under = pick_family_parts(doc, armature, view, io.MousePos);
      const Bone *knob = !under.empty() && under.front().layer->id == armature.id ? bone_find(armature, under.front().hit.bone) : nullptr;
      if (knob && bone_is_controller(*knob) && !bone_held_by_fix(doc, armature, *knob)) {
        if (!knob->selected) {
          family_deselect_all(doc, armature);
          bone_click_select(armature, under.front().hit, false);
        }
        if (bone_modal_start(C, area, 'G', view.to_document(io.MousePos))) {
          g_bone_modal.tweak = true;
        }
        return;
      }
    }
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
    if (!editable_layers(C).empty()) {
      const std::vector<FamilyHit> parts = pick_family_parts(doc, armature, view, io.MousePos);
      /* A bone's end wins over a point, a point over a bone's body (as a click picks). */
      if (parts.empty() || !parts.front().hit.bone || parts.front().hit.part == BonePart::Body) {
        g_bone_box.grab = select_press_grab(C, view, io.MousePos, io.KeyShift || io.KeyCtrl);
      }
    }
  }
  if (g_bone_box.active && g_bone_box.area == area.id && g_bone_box.grab.layer &&
      ImGui::IsMouseDragging(ImGuiMouseButton_Left, g_bone_box.grab.threshold))
  {
    const PressGrab grab = g_bone_box.grab;
    const ImVec2 start = g_bone_box.start;
    g_bone_box = {};
    Layer *layer = layer_find(doc, grab.layer);
    if (layer && !layer->drawing.points[size_t(grab.points.front())].selected) {
      family_deselect_all(doc, armature);
    }
    select_press_grab_move(C, area, view, grab, start);
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
        const ImVec2 a = g_bone_box.start, b = io.MousePos;
        const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        const auto box = [&](Layer &l) {
          const Transform2D to_document = layer_world_transform(doc, l.id);
          armature_select_joints_in(
              l, doc.frame_current == 0,
              [&](float2 p) {
                const ImVec2 s = view.to_screen(to_document.apply(p));
                return s.x >= lo.x && s.x <= hi.x && s.y >= lo.y && s.y <= hi.y;
              },
              io.KeyShift, io.KeyCtrl);
        };
        box_select_family(doc, armature, io.KeyShift || io.KeyCtrl, box);
        if (!point_layers.empty()) {
          select_box(C, view, a, b, handles);
        }
        push_undo(C, "Box Select");
      }
      else {
        /* A point under the mouse wins over a bone's body (points are the smaller targets); a
         * bone's ball wins over a point. */
        const FamilyHit under = pick_family_cycling(doc, armature, view, g_bone_box.start);
        const BoneHit hit = under.hit;
        /* A handle counts as a point here (Shift-click picks it as on the layer itself). */
        const bool on_point = !point_layers.empty() && (hit.part == BonePart::Body || !hit.bone) &&
                              (point_under(C, view, g_bone_box.start) ||
                               pick_handle(C, view, g_bone_box.start, handles).layer);
        if (on_point) {
          if (!io.KeyShift) {
            family_deselect_all(doc, armature);
          }
          select_click(C, view, view.to_document(g_bone_box.start), handles);
          push_undo(C, "Select");
        }
        else {
          /* One of its family's under the click (the bone layer around it, a shown sub bone group,
           * anim/bone_family.hh) is worked in instead; with Shift it joins what's picked. */
          Layer *other = under.layer && under.layer->id != armature.id ? under.layer : nullptr;
          if (!io.KeyShift) {
            family_deselect_all(doc, armature);
          }
          if (other) {
            bone_click_select(*other, hit, io.KeyShift);
            /* Inside the bone layer worked in, it stays the one worked in. */
            if (!io.KeyShift && !layer_inside(doc, other->id, armature.id)) {
              doc.active_layer = other->id;
            }
          }
          else {
            bone_click_select(armature, hit, io.KeyShift);
          }
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
    if (!bones_moving(doc, armature).empty()) {
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

/* Ctrl P and Alt P on bones: Blender's parent menus; Ctrl A its Apply menu. */
static void bone_parent_menus(Context &C, bool hovered)
{
  WindowManager &wm = *C.wm;
  if (hovered && (wm.menu_request == "bone_parent_set" || wm.menu_request == "bone_parent_clear" ||
                  wm.menu_request == "bone_apply"))
  {
    ImGui::OpenPopup(wm.menu_request == "bone_parent_set"   ? "##bone_parent_set"
                     : wm.menu_request == "bone_apply" ? "##bone_apply"
                                                       : "##bone_parent_clear");
    wm.menu_request.clear();
  }
  if (ui_menu_begin("##bone_apply")) {
    ImGui::TextDisabled("Apply");
    ImGui::Separator();
    bone_apply_items(C);
    ui_menu_end();
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
    menu_operator(C, "Symmetrize", "armature.symmetrize");
    ImGui::Separator();
    if (ImGui::BeginMenu("Convert To", C.document->frame_current == 0)) {
      const Layer *armature = document_active_layer(*C.document);
      const Bone *active = armature ? bone_find(*armature, armature->active_bone) : nullptr;
      const bool controller = active && bone_is_controller(*active);
      if (ImGui::MenuItem("Regular", nullptr, active && !controller)) {
        operator_call(C, "armature.bone_convert", {{"type", "REGULAR"}});
      }
      if (ImGui::MenuItem("2 Axis Controller", nullptr, controller)) {
        operator_call(C, "armature.bone_convert", {{"type", "CONTROLLER_2D"}});
      }
      ImGui::EndMenu();
    }
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
                                  const std::vector<uint64_t> &local_view, int *r_shape = nullptr,
                                  uint64_t skip = 0)
{
  /* Local View hides the rest, so a click can't pick them. */
  const std::set<uint64_t> local = local_view_layers(doc, local_view);
  const float2 at = view.to_document(mouse);
  const float px = std::max(distance(view.to_document(mouse + ImVec2(1.0f, 0.0f)), at), 1e-9f);
  /* Top first, as drawn (Draw Order included). */
  const std::vector<const Layer *> order = draw_order_layers(
      doc, [&](const Layer &l) { return l.shown() && (local.empty() || local.contains(l.id)); });
  const auto find = [&]() -> uint64_t {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const Layer &layer = **it;
      if (!layer.has_shapes() || layer.id == skip) {
        continue;
      }
      const VectorDrawing drawing = layer_has_modifier_effect(doc, layer, false)
                                        ? layer_evaluated_drawing(doc, layer, false)
                                        : layer_drawing(doc, layer);
      const Transform2D to_document = layer_world_transform(doc, layer.id);
      const float2 p = to_document.inverted().apply(at);
      const float scale = std::max(length(to_document.apply_vector({1.0f, 0.0f})), 1e-9f);
      /* Picking a stroke (the brush eyedropper): anywhere on it as drawn, its width included. */
      float reach = 4.0f * px / scale;
      if (r_shape) {
        float half = 0.0f;
        for (const Shape &s : drawing.shapes) {
          half = std::max(half, s.has_outline ? s.style.line_width * 0.5f : 0.0f);
        }
        reach += half;
      }
      if (const int shape = drawing_pick_shape(drawing, p, reach); shape >= 0) {
        if (r_shape) {
          *r_shape = shape;
        }
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
    if (!l.has_shapes()) {
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
    if (!l.shown()) {
      return;
    }
    for (const Layer &child : l.children) {
      self(self, child);
    }
    if (!l.has_shapes()) {
      return;
    }
    /* The drawing as it shows (bones, modifiers, a reference's original), its curves followed between
     * the points (a curve bulges past them), and as wide as its lines. */
    const Transform2D to_document = layer_world_transform(doc, l.id);
    const VectorDrawing shown = layer_evaluated_drawing(doc, l, false);
    float half_line = 0.0f;
    for (const Shape &shape : shown.shapes) {
      if (shape.has_outline && !shape.hidden) {
        half_line = std::max(half_line, shape.style.line_width * 0.5f);
      }
    }
    const float pad = half_line * to_document.scale() * view.zoom;
    const auto take = [&](float2 p) {
      const ImVec2 s = view.to_screen(to_document.apply(p));
      lo = ImVec2(std::min(lo.x, s.x - pad), std::min(lo.y, s.y - pad));
      hi = ImVec2(std::max(hi.x, s.x + pad), std::max(hi.y, s.y + pad));
    };
    std::vector<float2> samples;
    for (const Curve &curve : shown.curves) {
      for (int seg = 0; seg < curve.segment_count(); seg++) {
        samples.clear();
        curve_segment_sample(shown, curve, seg, 12, samples);
        for (const float2 &p : samples) {
          take(p);
        }
      }
    }
    for (const MeshPoint &p : shown.points) {
      take(p.pos);
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
 * Transform Layer (the tool): a box round the active layer's content, in its own space, as Transform
 * Points has round points (Moho's Transform Layer tool). Drag inside to move, the band round it to turn
 * about the layer's origin, a handle to scale (Shift keeps proportions, Alt from the origin). It changes
 * the layer's Location, Rotation and Scale, keyed as any change is.
 */

/* What the active layer shows, in its own space (before its own transform): its drawing or letters, a
 * picture's corners, a group's layers, a bone layer's bones. False with nothing to show. */
static bool layer_content_bounds(const Document &doc, const Layer &layer, float2 &r_lo, float2 &r_hi)
{
  const Transform2D to_layer = layer_world_transform(doc, layer.id).inverted();
  r_lo = float2(FLT_MAX, FLT_MAX);
  r_hi = float2(-FLT_MAX, -FLT_MAX);
  const auto take = [&](float2 document_point) {
    const float2 p = to_layer.apply(document_point);
    r_lo = float2(std::min(r_lo.x, p.x), std::min(r_lo.y, p.y));
    r_hi = float2(std::max(r_hi.x, p.x), std::max(r_hi.y, p.y));
  };
  const auto add = [&](auto &&self, const Layer &l) -> void {
    if (!l.shown()) {
      return;
    }
    for (const Layer &child : l.children) {
      self(self, child);
    }
    const Transform2D to_document = layer_world_transform(doc, l.id);
    if (l.is_bone_layer()) {
      for (const BonePose &pose : armature_pose(l, doc.frame_current == 0)) {
        take(to_document.apply(pose.head));
        take(to_document.apply(pose.tail));
      }
    }
    if (l.is_media() && !l.media.clips.empty()) {
      const MediaInfo info = media_clip_info(l.media.clips.front());
      if (info.source_width > 0 && info.source_height > 0) {
        float2 q[4];
        media_quad(doc, l, info.source_width, info.source_height, q);
        for (const float2 &p : q) {
          take(to_document.apply(p));
        }
      }
    }
    if (l.is_text()) {
      const TextBox box = text_bounds(l.text);
      take(to_document.apply(box.min));
      take(to_document.apply(box.max));
      take(to_document.apply({box.min.x, box.max.y}));
      take(to_document.apply({box.max.x, box.min.y}));
      return;
    }
    if (!l.has_shapes()) {
      return;
    }
    const VectorDrawing shown = layer_evaluated_drawing(doc, l, false);
    std::vector<float2> samples;
    for (const Curve &curve : shown.curves) {
      for (int seg = 0; seg < curve.segment_count(); seg++) {
        samples.clear();
        curve_segment_sample(shown, curve, seg, 8, samples);
        for (const float2 &p : samples) {
          take(to_document.apply(p));
        }
      }
    }
    for (const MeshPoint &p : shown.points) {
      take(to_document.apply(p.pos));
    }
  };
  add(add, layer);
  return r_lo.x <= r_hi.x;
}

struct LayerGizmo {
  bool active = false;
  int area = 0;
  /* 'G' move, 'R' turn, 'S' scale (by `handle`: 0-3 corners from bottom left, anticlockwise; 4-7 the
   * sides' middles, bottom, right, top, left), 'K' shear (by the edge `handle` - 4), 'P' the pivot. */
  char mode = 0;
  int handle = -1;
  uint64_t layer = 0;
  Document snapshot;
  /* The layer as the drag began: its transform's parts, its parent's space (to the document) and its
   * own, its content's box (its own space), and its pivot (its own space). */
  float2 position, scale, shear;
  float rotation = 0.0f;
  Transform2D parent, world;
  float2 lo, hi, pivot;
  float2 start_mouse;
  /* What it has done, for the readout. */
  char readout[64] = "";
};
static LayerGizmo g_layer_gizmo;

/* A box handle's place (0-7, see LayerGizmo) in the layer's own space. */
static float2 layer_gizmo_handle(float2 lo, float2 hi, int k)
{
  static const float2 at[8] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5f, 0}, {1, 0.5f}, {0.5f, 1}, {0, 0.5f}};
  return {lo.x + (hi.x - lo.x) * at[k].x, lo.y + (hi.y - lo.y) * at[k].y};
}

/* The active layer's own grid across the viewport, for Transform Layer: lines at even steps of its own
 * units, through everything that places it (its transform, its groups', a bone it follows), so a layer
 * squashed across has tall thin cells and a turned one a turned grid. One step for both ways (from the
 * layer's average scale, in powers of 2, about 24 to 48 pixels apart), so a squash shows rather than
 * being evened out; every 4th line a little stronger, and its own axes through its origin in Blender's
 * colours (X red, Y green). */
static void draw_layer_grid(ImDrawList *dl, const ViewMapping &view, const Transform2D &world)
{
  const float pixels = world.scale() * view.zoom;
  if (!(pixels > 1e-6f) || !std::isfinite(pixels)) {
    return;
  }
  float step = 1.0f;
  while (step * pixels < 24.0f) {
    step *= 2.0f;
  }
  while (step * pixels > 48.0f) {
    step *= 0.5f;
  }
  /* The part of its space the viewport shows: the screen's corners in it. */
  const Transform2D to_local = world.inverted();
  float2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
  for (const ImVec2 corner : {view.origin, ImVec2(view.origin.x + view.size.x, view.origin.y),
                              ImVec2(view.origin.x + view.size.x, view.origin.y + view.size.y),
                              ImVec2(view.origin.x, view.origin.y + view.size.y)})
  {
    const float2 p = to_local.apply(view.to_document(corner));
    lo = float2(std::min(lo.x, p.x), std::min(lo.y, p.y));
    hi = float2(std::max(hi.x, p.x), std::max(hi.y, p.y));
  }
  const auto at = [&](float2 local) { return view.to_screen(world.apply(local)); };
  dl->PushClipRect(view.origin, ImVec2(view.origin.x + view.size.x, view.origin.y + view.size.y), true);
  for (const bool across : {true, false}) {
    const float from = across ? lo.x : lo.y, to = across ? hi.x : hi.y;
    const long first = long(std::floor(from / step)), last = long(std::ceil(to / step));
    if (last - first > 2000) {
      continue;
    }
    for (long k = first; k <= last; k++) {
      const float v = float(k) * step;
      const ImVec2 a = across ? at({v, lo.y}) : at({lo.x, v});
      const ImVec2 b = across ? at({v, hi.y}) : at({hi.x, v});
      ImU32 col = k % 4 == 0 ? IM_COL32(0, 0, 0, 46) : IM_COL32(0, 0, 0, 20);
      float width = 1.0f;
      if (k == 0) {
        /* Its own axes: the line across (y = 0) is its X axis, red; the line up (x = 0) its Y axis, green. */
        col = across ? IM_COL32(0x5a, 0xb4, 0x3c, 170) : IM_COL32(0xd8, 0x4a, 0x4a, 170);
        width = 1.5f;
      }
      dl->AddLine(a, b, col, width);
    }
  }
  dl->PopClipRect();
}

/* The box's drag done: its undo step, and with the Motion Path tool (Edit Layer) keyed whatever Auto Keying says, as
 * the paths' own edits are. */
void layer_gizmo_finish(Context &C, const char *name)
{
  if (C.wm->tool_settings.tool == Tool::MotionPath) {
    Document &doc = *C.document;
    const bool auto_key = doc.auto_key;
    doc.auto_key = true;
    animation_record(doc);
    doc.auto_key = auto_key;
  }
  push_undo(C, name);
}

static void layer_gizmo_input(Context &C, Area &area, const ViewMapping &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  ImGuiIO &io = ImGui::GetIO();
  LayerGizmo &g = g_layer_gizmo;
  Layer *layer = document_active_layer(doc);
  if (g.active && (!layer || layer->id != g.layer)) {
    g = {};
  }
  if (!layer || layer->locked) {
    return;
  }
  if (g_layer_pivot.layer != layer->id) {
    g_layer_pivot = {layer->id, false, {}};
  }
  const auto type_in_layer = [&]() {
    C.wm->tool_settings.tool = Tool::EditText;
    const TextToolView text_view{[&](float2 p) { return view.to_screen(p); },
                                 [&](ImVec2 p) { return view.to_document(p); }, view.zoom};
    text_editing_begin_at(C, text_view, io.MousePos);
  };
  /* With the overlays off (Shift Z) the box is gone: not drawn, and nothing to grab. A drag it began goes on. */
  if (!doc.overlays.show && !g.active) {
    if (hovered && layer->is_text() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      type_in_layer();
    }
    return;
  }
  float2 lo, hi;
  if (g.active) {
    lo = g.lo;
    hi = g.hi;
  }
  else if (!layer_content_bounds(doc, *layer, lo, hi)) {
    /* Nothing shown yet: a small box round its origin. */
    lo = float2(-0.05f, -0.05f);
    hi = float2(0.05f, 0.05f);
  }
  const Transform2D world = g.active ? g.world : layer_world_transform(doc, layer->id);
  const auto screen = [&](float2 local) { return view.to_screen(world.apply(local)); };
  const ImVec2 corners[4] = {screen(lo), screen({hi.x, lo.y}), screen(hi), screen({lo.x, hi.y})};
  const float2 mouse = view.to_document(io.MousePos);
  const float2 pivot = g.active ? g.pivot : layer_pivot_local(layer->id);

  /* What the mouse is over: the pivot, a handle, an edge between handles (shear), the inside, or the band
   * round it (turn). */
  const auto on = [&](float2 local, float reach) {
    const ImVec2 p = screen(local);
    return std::fabs(io.MousePos.x - p.x) <= reach && std::fabs(io.MousePos.y - p.y) <= reach;
  };
  const bool over_pivot = !g.active && on(pivot, GIZMO_PIVOT_HIT);
  int over_handle = -1;
  for (int k = 0; k < 8 && !g.active && !over_pivot; k++) {
    if (on(layer_gizmo_handle(lo, hi, k), GIZMO_HIT)) {
      over_handle = k;
    }
  }
  const int over_edge = g.active || over_pivot || over_handle >= 0 ? -1 : gizmo_edge_hit(corners, io.MousePos);
  const float2 local_mouse = world.inverted().apply(mouse);
  const bool inside = local_mouse.x >= lo.x && local_mouse.x <= hi.x && local_mouse.y >= lo.y && local_mouse.y <= hi.y;
  float edge = FLT_MAX;
  for (int k = 0; k < 4; k++) {
    edge = std::min(edge, distance_to_segment({io.MousePos.x, io.MousePos.y}, {corners[k].x, corners[k].y},
                                              {corners[(k + 1) % 4].x, corners[(k + 1) % 4].y}));
  }
  const bool band = !inside && !over_pivot && over_handle < 0 && over_edge < 0 && edge <= 22.0f;

  /* A text layer double-clicked: typing in it (the Edit Text tool, the caret where it was clicked). The
   * pivot double-clicked goes back to the layer's origin. */
  if (!g.active && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    if (over_pivot) {
      g_layer_pivot.set = false;
      return;
    }
    if (layer->is_text()) {
      type_in_layer();
      return;
    }
  }
  if (hovered && !g.active) {
    if (over_pivot) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    else if (over_handle >= 0) {
      ImGui::SetMouseCursor(over_handle >= 4 ? (over_handle % 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS)
                                             : (over_handle % 2 ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE));
    }
    else if (over_edge >= 0) {
      gizmo_cursor_sign(dl, io.MousePos, 'K', over_edge % 2 == 0);
    }
    else if (inside) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    else if (band) {
      gizmo_cursor_sign(dl, io.MousePos, 'R');
    }
  }
  if (hovered && !g.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      (over_pivot || over_handle >= 0 || over_edge >= 0 || inside || band))
  {
    g.active = true;
    g.area = area.id;
    g.mode = over_pivot ? 'P' : over_handle >= 0 ? 'S' : over_edge >= 0 ? 'K' : inside ? 'G' : 'R';
    g.handle = over_edge >= 0 ? 4 + over_edge : over_handle;
    g.layer = layer->id;
    g.snapshot = doc;
    g.position = layer->position;
    g.rotation = layer->rotation;
    g.scale = layer->scale;
    g.shear = layer->shear;
    g.world = world;
    g.parent = world * layer->local_transform().inverted();
    g.lo = lo;
    g.hi = hi;
    g.pivot = pivot;
    g.start_mouse = mouse;
    g.readout[0] = 0;
  }

  if (g.active && g.area == area.id) {
    const float2 pivot_world = g.world.apply(g.pivot);
    const float2 m = g.world.inverted().apply(mouse), m0 = g.world.inverted().apply(g.start_mouse);
    /* Back to how it was pressed, then the drag's change on that. */
    layer->position = g.position;
    layer->rotation = g.rotation;
    layer->scale = g.scale;
    layer->shear = g.shear;
    if (g.mode == 'P') {
      /* The pivot: anywhere, Ctrl onto the box's corners, sides' middles or middle. */
      float2 at = m;
      if (io.KeyCtrl) {
        float best = FLT_MAX;
        for (int k = 0; k < 9; k++) {
          const float2 p = k < 8 ? layer_gizmo_handle(g.lo, g.hi, k) : (g.lo + g.hi) * 0.5f;
          if (distance(p, m) < best) {
            best = distance(p, m);
            at = p;
          }
        }
      }
      g_layer_pivot = {layer->id, true, at};
    }
    else if (g.mode == 'G') {
      float2 d = mouse - g.start_mouse;
      if (io.KeyShift) {
        (std::abs(d.x) > std::abs(d.y) ? d.y : d.x) = 0.0f;
      }
      layer->position = g.position + g.parent.inverted().apply_vector(d);
      std::snprintf(g.readout, sizeof(g.readout), "X %.3f  Y %.3f", d.x, d.y);
    }
    else if (g.mode == 'R') {
      const float a0 = std::atan2(g.start_mouse.y - pivot_world.y, g.start_mouse.x - pivot_world.x);
      const float a1 = std::atan2(mouse.y - pivot_world.y, mouse.x - pivot_world.x);
      float turn = (a1 - a0) * 180.0f / PI;
      while (turn > 180.0f) {
        turn -= 360.0f;
      }
      while (turn < -180.0f) {
        turn += 360.0f;
      }
      float rotation = g.rotation + turn;
      if (io.KeyShift) {
        rotation = std::round(rotation / 45.0f) * 45.0f;
      }
      layer->rotation = rotation;
      /* About the pivot: moved back so it stays where it was. */
      layer_keep_point(*layer, g.parent, g.pivot, pivot_world);
      std::snprintf(g.readout, sizeof(g.readout), "%.1f\xC2\xB0", rotation - g.rotation);
    }
    else if (g.mode == 'S') {
      /* The handle follows the mouse, the opposite one (or, with Alt, the pivot) staying put. */
      const float2 h = layer_gizmo_handle(g.lo, g.hi, g.handle);
      const float2 anchor =
          io.KeyAlt ? g.pivot
                    : layer_gizmo_handle(g.lo, g.hi, g.handle < 4 ? (g.handle + 2) % 4 : 4 + (g.handle - 4 + 2) % 4);
      const bool across = g.handle < 4 || g.handle == 5 || g.handle == 7;
      const bool up = g.handle < 4 || g.handle == 4 || g.handle == 6;
      /* Kept the grab: the handle moves as the mouse does from where it was pressed. */
      const float2 grabbed = h + (m - m0);
      float fx = across && std::fabs(h.x - anchor.x) > 1e-6f ? (grabbed.x - anchor.x) / (h.x - anchor.x) : 1.0f;
      float fy = up && std::fabs(h.y - anchor.y) > 1e-6f ? (grabbed.y - anchor.y) / (h.y - anchor.y) : 1.0f;
      if (io.KeyShift) {
        /* Proportions kept: the corners by the larger change, a side's both ways alike. */
        const float f = g.handle < 4 ? (std::fabs(fx) > std::fabs(fy) ? fx : fy) : (across ? fx : fy);
        fx = fy = f;
      }
      layer->scale = {g.scale.x * fx, g.scale.y * fy};
      /* Scaled about its origin: moved back so the anchor stays where it was. */
      layer_keep_point(*layer, g.parent, anchor, g.world.apply(anchor));
      std::snprintf(g.readout, sizeof(g.readout), "%.0f%% \xC3\x97 %.0f%%", fx * 100.0f, fy * 100.0f);
    }
    else if (g.mode == 'K') {
      /* An edge slid along itself: the bottom and top (edges 0 and 2) sideways, the sides up and down, about
       * the opposite edge (Alt: the pivot), in the layer's own space; its Rotation stays, its Scale and Shear
       * take the lean (Shift: steps of 15 degrees). */
      const int e = g.handle - 4;
      const bool along_x = e % 2 == 0;
      const float2 anchor = io.KeyAlt ? g.pivot
                            : e == 0  ? float2(g.lo.x, g.hi.y)
                            : e == 1  ? float2(g.lo.x, g.lo.y)
                            : e == 2  ? float2(g.lo.x, g.lo.y)
                                      : float2(g.hi.x, g.lo.y);
      const float reach = along_x ? m0.y - anchor.y : m0.x - anchor.x;
      float k = std::fabs(reach) > 1e-9f ? (along_x ? m.x - m0.x : m.y - m0.y) / reach : 0.0f;
      float degrees = std::atan(k) * 180.0f / PI;
      if (io.KeyShift) {
        degrees = std::round(degrees / 15.0f) * 15.0f;
      }
      degrees = std::clamp(degrees, -85.0f, 85.0f);
      k = std::tan(degrees * PI / 180.0f);
      const Transform2D lean = along_x ? Transform2D{1.0f, 0.0f, k, 1.0f, -k * anchor.y, 0.0f}
                                       : Transform2D{1.0f, k, 0.0f, 1.0f, 0.0f, -k * anchor.x};
      const Transform2D was = layer->local_transform();
      layer_set_keeping_rotation(*layer, was * lean);
      /* As Properties names it: a slid top or bottom tilts the layer's Y axis, a slid side its X axis. */
      std::snprintf(g.readout, sizeof(g.readout), "Shear %s %.1f\xC2\xB0", along_x ? "Y" : "X",
                    along_x ? layer->shear.y : layer->shear.x);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      const int frame = doc.frame_current;
      const char mode = g.mode;
      const float2 pivot_was = g.pivot;
      doc = std::move(g.snapshot);
      doc.frame_current = frame;
      if (mode == 'P') {
        g_layer_pivot.local = pivot_was;
      }
      g = {};
      return;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const char mode = g.mode;
      const bool moved = layer->position != g.position || layer->rotation != g.rotation || layer->scale != g.scale ||
                         layer->shear != g.shear;
      g = {};
      if (mode != 'P' && moved) {
        layer_gizmo_finish(C, mode == 'R'   ? "Rotate Layer"
                              : mode == 'S' ? "Scale Layer"
                              : mode == 'K' ? "Shear Layer"
                                            : "Move Layer");
      }
      return;
    }
  }

  if (!doc.overlays.show) {
    return;
  }
  /* The box, its handles, the turn marks at its corners and the pivot, over the layer's grid. */
  const Transform2D shown = g.active ? g.parent * layer->local_transform() : world;
  if (C.wm->tool_settings.layer_grid) {
    draw_layer_grid(dl, view, shown);
  }
  const auto at = [&](float2 local) { return view.to_screen(shown.apply(local)); };
  const ImVec2 box[4] = {at(lo), at({hi.x, lo.y}), at(hi), at({lo.x, hi.y})};
  const ImU32 accent = gizmo_accent(0);
  gizmo_draw_lines(dl, box, 4, true, accent);
  const int lit_edge = g.active && g.mode == 'K' ? g.handle - 4 : over_edge;
  if (lit_edge >= 0) {
    const ImVec2 e[2] = {box[lit_edge], box[(lit_edge + 1) % 4]};
    gizmo_draw_lines(dl, e, 2, false, accent, 3.0f);
  }
  if (!g.active) {
    const ImVec2 mid = (box[0] + box[2]) * 0.5f;
    for (const ImVec2 &corner : box) {
      gizmo_draw_turn_mark(dl, corner, corner - mid, accent);
    }
  }
  for (int k = 0; k < 8; k++) {
    const bool lit = k == over_handle || (g.active && g.mode == 'S' && k == g.handle);
    gizmo_draw_handle(dl, at(layer_gizmo_handle(lo, hi, k)), accent, lit);
  }
  const float2 shown_pivot = g.active && g.mode == 'P' ? g_layer_pivot.local : pivot;
  const ImVec2 was = view.to_screen(g.world.apply(g.pivot));
  gizmo_draw_pivot(dl, g.active && g.mode == 'P' ? view.to_screen(g.world.apply(shown_pivot)) : at(shown_pivot), accent,
                   over_pivot || (g.active && g.mode == 'P'), g.active ? &was : nullptr);
  if (g.active) {
    gizmo_readout(dl, io.MousePos, g.readout);
  }
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
        if (!l.shown() || (!local.empty() && !local.contains(l.id))) {
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
  /* The Motion Path tool's Alt click on a bone or point picks that instead (motion_path_pick_click). */
  if (ts.tool == Tool::MotionPath && C.document->motion_paths &&
      motion_path_pick_click(C, view, ImGui::GetIO().MousePos, false, false))
  {
    return false;
  }
  /* Alt-click on a guide line deletes it. */
  if (ts.tool == Tool::Annotate && annotate_mode_is_guide(ts.annotate_mode) && C.document->guide_settings.show) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (pick_guide_measure(*C.document, view, mouse).guide >= 0 || pick_guide_turn(*C.document, view, mouse).guide >= 0) {
      return false;
    }
  }
  return true;
}

/* --- Follow Point's light blue points (anim/follow_point.hh) --- */

static constexpr float FOLLOW_POINT_RADIUS = 5.0f;
static constexpr float FOLLOW_POINT_PICK = 9.0f;

/* Every Show Follow Point constraint's. */
template<typename Fn> static void for_each_shown_follow_point(Document &doc, Fn &&fn)
{
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer()) {
      continue;
    }
    Layer &rig = *layer_find(doc, flat->id);
    for (Bone &bone : rig.bones) {
      for (BoneConstraint &c : bone.constraints) {
        if (c.type == BoneConstraintType::FollowPoint && c.follow_show && c.follow_layer != 0) {
          fn(rig, bone, c);
        }
      }
    }
  }
}

/* Where a Follow Deform's points show now (document space). False when the layer has nothing to go by. */
static bool follow_deform_shown(const Document &doc, const Layer &layer, const Modifier &m, float2 &r_first, float2 &r_second)
{
  if (layer.drawing.points.empty()) {
    return false;
  }
  const Transform2D to_document = layer_world_transform(doc, layer.id);
  const float2 first = layer_places_shown(doc, layer, {{0, vector_deform_follow_rest(doc, layer, m, false)}})[0];
  const float2 above = first - vector_deform_follow_now(doc, layer, m, false);
  r_first = to_document.apply(first);
  r_second = to_document.apply(vector_deform_follow_now(doc, layer, m, true) + above);
  return true;
}

template<typename Fn> static void for_each_shown_follow_deform(Document &doc, Fn &&fn)
{
  for (const Layer *flat : document_layers_flat(doc)) {
    for (const Modifier &m : flat->modifiers) {
      if (m.type == ModifierType::FollowDeform && m.deform_show_point && m.show_viewport) {
        fn(*layer_find(doc, flat->id), *const_cast<Modifier *>(&m));
      }
    }
  }
}

/* The point eyedropper's points, where they show: the nearest within reach of the mouse is what a click picks (its id, its
 * number in `r_number`). Drawn when `dl` is given. */
static uint64_t follow_pick_points(const Document &doc, const ViewMapping &view, ImDrawList *dl, int *r_number)
{
  uint64_t shape_id = 0;
  const Layer *layer = layer_find(doc, ui_point_eyedropper_layer(&shape_id));
  const int shape = layer ? drawing_shape_index(layer->drawing, shape_id) : -1;
  if (shape < 0) {
    return 0;
  }
  const VectorDrawing shown = layer_evaluated_drawing(doc, *layer, false);
  const Transform2D to_document = layer_world_transform(doc, layer->id);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  float best = FOLLOW_POINT_PICK * FOLLOW_POINT_PICK;
  uint64_t under = 0;
  ImVec2 under_at;
  for (const int p : shape_points(layer->drawing, layer->drawing.shapes[size_t(shape)])) {
    const uint64_t id = layer->drawing.points[size_t(p)].id;
    const int at = drawing_point_index(shown, id);
    if (at < 0) {
      continue;
    }
    const ImVec2 place = view.to_screen(to_document.apply(shown.points[size_t(at)].pos));
    if (dl) {
      dl->AddCircleFilled(place, 4.5f, IM_COL32(0x10, 0x10, 0x10, 255));
      dl->AddCircleFilled(place, 3.0f, IM_COL32(255, 255, 255, 255));
    }
    const float distance = ImLengthSqr(place - mouse);
    if (distance < best) {
      best = distance;
      under = id;
      under_at = place;
      if (r_number) {
        *r_number = p;
      }
    }
  }
  if (dl && under != 0) {
    dl->AddCircleFilled(under_at, FOLLOW_POINT_RADIUS + 1.5f, IM_COL32(0x10, 0x10, 0x10, 255));
    dl->AddCircleFilled(under_at, FOLLOW_POINT_RADIUS, IM_COL32(0x6f, 0xc8, 0xff, 255));
  }
  return under;
}

/* Envelope's Range slider, while dragged: a tint over what it reaches, strongest at the cage's outline, and a thin line
 * at the limit. Its Blend slider: a violet tint where the sides share, so where Blend changes anything shows even on
 * the setup frame, where nothing bends. A Points mesh's Falloff and Blend: violet where its lines share. */
static void draw_deform_range(ImDrawList *dl, Context &C, const ViewMapping &view)
{
  const WindowManager::DeformRangePreview &preview = C.wm->deform_range_preview;
  if (preview.frame < ImGui::GetFrameCount() - 1) {
    return;
  }
  const Document &doc = *C.document;
  const Layer *layer = layer_find(doc, preview.layer);
  if (!layer || preview.modifier < 0 || preview.modifier >= int(layer->modifiers.size())) {
    return;
  }
  using What = WindowManager::DeformRangePreview::What;
  const Modifier &m = layer->modifiers[size_t(preview.modifier)];
  DeformRangeView range;
  /* Blend on a Points mesh shows where its lines share, as Falloff does. */
  const bool lines = preview.what == What::Falloff ||
                     (preview.what == What::Blend && m.type == ModifierType::MeshDeform);
  if (!(lines                         ? vector_deform_falloff_view(doc, *layer, m, range) :
        preview.what == What::Blend ? vector_deform_blend_view(doc, *layer, m, range) :
                                      vector_deform_range_view(doc, *layer, m, range)))
  {
    return;
  }
  const bool range_tint = preview.what == What::Range;
  const ImU32 tint = range_tint ? IM_COL32(0xff, 0x8a, 0x20, 0) : IM_COL32(0xb0, 0x70, 0xff, 0);
  const float strongest = range_tint ? 110.0f : 150.0f;
  const Transform2D to_document = layer_world_transform(doc, layer->id);
  const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve(int(range.quads.size()) * 6, int(range.quads.size()) * 4);
  for (const std::array<int, 4> &quad : range.quads) {
    const ImDrawIdx first = ImDrawIdx(dl->_VtxCurrentIdx);
    for (const int k : quad) {
      const int alpha = int(std::clamp(range.say[size_t(k)], 0.0f, 1.0f) * strongest);
      dl->PrimWriteVtx(view.to_screen(to_document.apply(range.at[size_t(k)])), white,
                       tint | (ImU32(alpha) << IM_COL32_A_SHIFT));
    }
    dl->PrimWriteIdx(first);
    dl->PrimWriteIdx(ImDrawIdx(first + 1));
    dl->PrimWriteIdx(ImDrawIdx(first + 2));
    dl->PrimWriteIdx(first);
    dl->PrimWriteIdx(ImDrawIdx(first + 2));
    dl->PrimWriteIdx(ImDrawIdx(first + 3));
  }
  for (const std::array<int, 2> &line : range.limit) {
    dl->AddLine(view.to_screen(to_document.apply(range.at[size_t(line[0])])),
                view.to_screen(to_document.apply(range.at[size_t(line[1])])), IM_COL32(0xff, 0xb0, 0x60, 220), 1.0f);
  }
}

/* Show Mesh: each shown Curve, Cage or Mesh Deform with it on draws an even grid over what it reaches, bent as it bends
 * that space now, over the drawing. A grey grid on the setup frame, where nothing bends. */
static void draw_deform_grids(ImDrawList *dl, Context &C, const ViewMapping &view)
{
  const Document &doc = *C.document;
  if (!doc.overlays.show) {
    return;
  }
  std::vector<ImVec2> screen;
  for (const Layer *layer : document_layers_flat(doc)) {
    if (!layer_shown_in_document(doc, layer->id)) {
      continue;
    }
    for (const Modifier &m : layer->modifiers) {
      if (!m.show_mesh || !m.show_viewport || !modifier_bends(m.type)) {
        continue;
      }
      DeformGridView grid;
      if (!vector_deform_grid_view(doc, *layer, m, grid)) {
        continue;
      }
      const Transform2D to_document = layer_world_transform(doc, layer->id);
      const ImU32 colour = grid.bent ? IM_COL32(0xe8, 0x9a, 0x3c, 170) : IM_COL32(0xb0, 0xb0, 0xb0, 110);
      for (const std::vector<float2> &line : grid.lines) {
        screen.clear();
        for (const float2 p : line) {
          screen.push_back(view.to_screen(to_document.apply(p)));
        }
        dl->AddPolyline(screen.data(), int(screen.size()), colour, ImDrawFlags_None, 1.0f);
      }
    }
  }
}

/* A point dragged: on frame 0 that places its rest; on other frames it shifts it from there, keyed like any change. */
struct FollowPointDrag {
  bool active = false;
  int area = -1;
  uint64_t rig = 0, bone = 0;
  int index = 0;
  /* A Follow Deform's second point; a constraint's points go by `which` (0 to 2). */
  bool second = false;
  float2 start;
  float2 pressed;
  bool moved = false;
  /* A Follow Deform's: its layer and modifier (`index`), in place of a bone's constraint. */
  uint64_t layer = 0;
  int which = 0;
  /* The shape's point it was picked as, put back if the drag is cancelled. */
  uint64_t start_id = 0;
  /* With Ctrl on frame 0: the shape's point it snaps to, picked on release, and where that shows. */
  uint64_t snap = 0;
  float2 snap_at;
};
static FollowPointDrag g_follow_drag;

static void draw_follow_points(ImDrawList *dl, Context &C, const ViewMapping &view)
{
  Document &doc = *C.document;
  follow_pick_points(doc, view, dl, nullptr);
  if (!doc.overlays.show) {
    return;
  }
  for_each_shown_follow_deform(doc, [&](Layer &layer, Modifier &m) {
    float2 first, second;
    if (!follow_deform_shown(doc, layer, m, first, second)) {
      return;
    }
    const ImU32 blue = IM_COL32(0x6f, 0xc8, 0xff, 255), dark = IM_COL32(0x10, 0x10, 0x10, 255);
    const ImVec2 at = view.to_screen(first);
    if (m.deform_two) {
      const ImVec2 other = view.to_screen(second);
      dl->AddLine(at, other, blue, 1.0f);
      dl->AddCircleFilled(other, FOLLOW_POINT_RADIUS + 1.5f, dark);
      dl->AddCircleFilled(other, FOLLOW_POINT_RADIUS, blue);
    }
    dl->AddCircleFilled(at, FOLLOW_POINT_RADIUS + 1.5f, dark);
    dl->AddCircleFilled(at, FOLLOW_POINT_RADIUS, blue);
  });
  for_each_shown_follow_point(doc, [&](Layer &, Bone &, BoneConstraint &c) {
    const ImU32 blue = IM_COL32(0x6f, 0xc8, 0xff, 255), dark = IM_COL32(0x10, 0x10, 0x10, 255);
    const int count = c.follow_count();
    for (int i = 1; i < count; i++) {
      dl->AddLine(view.to_screen(c.follow_now), view.to_screen(c.follow_now_at(i)), blue, 1.0f);
    }
    for (int i = count - 1; i >= 0; i--) {
      const ImVec2 at = view.to_screen(c.follow_now_at(i));
      dl->AddCircleFilled(at, FOLLOW_POINT_RADIUS + 1.5f, dark);
      dl->AddCircleFilled(at, FOLLOW_POINT_RADIUS, blue);
    }
  });
  /* The shape's point a Ctrl drag snaps to. */
  if (g_follow_drag.active && g_follow_drag.snap != 0) {
    const ImVec2 at = view.to_screen(g_follow_drag.snap_at);
    dl->AddCircle(at, FOLLOW_POINT_RADIUS + 4.0f, IM_COL32(0x10, 0x10, 0x10, 255), 0, 3.5f);
    dl->AddCircle(at, FOLLOW_POINT_RADIUS + 4.0f, IM_COL32(255, 255, 255, 255), 0, 1.5f);
  }
}

/* The point of the followed shape nearest the mouse, within reach, as it rests (frame 0): its id, and where it shows in
 * `r_at` (document space). 0 when none is. */
static uint64_t follow_snap_point(const Document &doc, const Layer &owner, const Modifier &m, const ViewMapping &view,
                                  float2 &r_at)
{
  VectorDrawing rest, bent;
  if (!vector_deform_shape_read(doc, owner, m, rest, bent)) {
    return 0;
  }
  const Transform2D to_document = layer_world_transform(doc, owner.id);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  float best = FOLLOW_POINT_PICK * FOLLOW_POINT_PICK;
  uint64_t under = 0;
  for (const MeshPoint &point : rest.points) {
    const float2 at = to_document.apply(point.pos);
    const float distance = ImLengthSqr(view.to_screen(at) - mouse);
    if (distance < best) {
      best = distance;
      under = point.id;
      r_at = at;
    }
  }
  return under;
}

/* Whether the mouse is on a point, or dragging one. */
static bool follow_point_input(Context &C, const Area &area, const ViewMapping &view, bool hovered)
{
  Document &doc = *C.document;
  FollowPointDrag &d = g_follow_drag;
  if (d.active && d.layer != 0) {
    if (d.area != area.id) {
      return false;
    }
    Layer *layer = layer_find(doc, d.layer);
    Modifier *m = layer && d.index < int(layer->modifiers.size()) ? &layer->modifiers[size_t(d.index)] : nullptr;
    if (!m || m->type != ModifierType::FollowDeform) {
      d = {};
      return false;
    }
    const bool rest = doc.frame_current == 0;
    float2 &value = rest ? (d.second ? m->deform_point2 : m->deform_point) : (d.second ? m->deform_shift2 : m->deform_shift);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      value = d.start;
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      d = {};
      return true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (d.moved) {
        push_undo(C, rest ? "Move Follow Point" : "Shift Follow Point");
      }
      d = {};
      return true;
    }
    const float2 to = view.to_document(ImGui::GetIO().MousePos);
    if (length(to - d.pressed) > 1e-6f) {
      const float2 here = layer_world_transform(doc, layer->id).inverted().apply(to);
      value = rest ? here : vector_deform_follow_shift(doc, *layer, *m, d.second, here);
      d.moved = d.moved || length(value - d.start) > 1e-6f;
    }
    return true;
  }
  if (d.active) {
    if (d.area != area.id) {
      return false;
    }
    Layer *rig = layer_find(doc, d.rig);
    Bone *bone = rig ? bone_find(*rig, d.bone) : nullptr;
    BoneConstraint *c = bone && d.index < int(bone->constraints.size()) ? &bone->constraints[size_t(d.index)] : nullptr;
    Layer *owner = nullptr;
    const Modifier *m = c ? follow_point_modifier(doc, *c, &owner) : nullptr;
    if (!m || d.which >= c->follow_count()) {
      d = {};
      return false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (doc.frame_current == 0) {
        follow_point_place(doc, *rig, *c, *owner, d.start, d.which);
        c->follow_id_at(d.which) = d.start_id;
        c->follow_now_at(d.which) = layer_world_transform(doc, owner->id).apply(d.start);
      }
      else {
        c->follow_shift_at(d.which) = d.start;
      }
      g_right_click_spent |= ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      d = {};
      return true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      /* Let go on a point of the shape with Ctrl: it becomes that point. */
      if (d.snap != 0) {
        follow_point_pick(doc, *rig, *c, d.which, d.snap);
      }
      if (d.moved || d.snap != d.start_id) {
        push_undo(C, doc.frame_current == 0 ? "Move Follow Point" : "Shift Follow Point");
      }
      d = {};
      return true;
    }
    const float2 to = view.to_document(ImGui::GetIO().MousePos);
    if (doc.frame_current != 0) {
      if (length(to - d.pressed) > 1e-6f) {
        const float2 shift = follow_point_shift_to(doc, *rig, *bone, *c, d.which, to);
        d.moved = d.moved || length(shift - d.start) > 1e-6f;
        c->follow_shift_at(d.which) = shift;
      }
      return true;
    }
    if (!d.moved && length(to - d.pressed) <= 1e-6f) {
      return true;
    }
    /* Moved off the shape's point it was, it has a place of its own (until let go on one with Ctrl). */
    c->follow_id_at(d.which) = 0;
    float2 at = to;
    d.snap = ImGui::GetIO().KeyCtrl ? follow_snap_point(doc, *owner, *m, view, at) : 0;
    d.snap_at = at;
    const float2 point = layer_world_transform(doc, owner->id).inverted().apply(at);
    d.moved = d.moved || length(point - d.start) > 1e-6f;
    follow_point_place(doc, *rig, *c, *owner, point, d.which);
    c->follow_now_at(d.which) = at;
    return true;
  }
  if (!hovered || !doc.overlays.show || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    return false;
  }
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  float best = FOLLOW_POINT_PICK * FOLLOW_POINT_PICK;
  for_each_shown_follow_point(doc, [&](Layer &rig, Bone &bone, BoneConstraint &c) {
    const bool rest = doc.frame_current == 0;
    for (int which = 0; which < c.follow_count(); which++) {
      /* A point of the shape goes as it does: there's nothing to shift. On frame 0 dragging it gives it a place of its
       * own. */
      if (!rest && c.follow_id_at(which) != 0) {
        continue;
      }
      const float distance = ImLengthSqr(view.to_screen(c.follow_now_at(which)) - mouse);
      if (distance < best) {
        best = distance;
        d = {};
        d.active = true;
        d.area = area.id;
        d.rig = rig.id;
        d.bone = bone.id;
        d.index = int(&c - bone.constraints.data());
        d.which = which;
        d.start = rest ? c.follow_point_at(which) : c.follow_shift_at(which);
        d.start_id = rest ? c.follow_id_at(which) : 0;
        d.pressed = view.to_document(mouse);
      }
    }
  });
  for_each_shown_follow_deform(doc, [&](Layer &layer, Modifier &m) {
    float2 first, second;
    if (!follow_deform_shown(doc, layer, m, first, second)) {
      return;
    }
    for (const bool is_second : {false, true}) {
      /* A point of the shape goes as it does: there's nothing to drag. */
      if ((is_second && !m.deform_two) || (is_second ? m.deform_point2_id : m.deform_point_id) != 0) {
        continue;
      }
      const float distance = ImLengthSqr(view.to_screen(is_second ? second : first) - mouse);
      if (distance < best) {
        best = distance;
        const bool rest = doc.frame_current == 0;
        d = {};
        d.active = true;
        d.area = area.id;
        d.layer = layer.id;
        d.index = int(&m - layer.modifiers.data());
        d.second = is_second;
        d.start = rest ? (is_second ? m.deform_point2 : m.deform_point) : (is_second ? m.deform_shift2 : m.deform_shift);
        d.pressed = view.to_document(mouse);
      }
    }
  });
  return d.active;
}

/* Runs a pick click: shows what it would pick, and picks on release if the mouse stayed put. */
static void layer_pick_update(Context &C, const ViewMapping &view, ImDrawList *dl)
{
  const ImGuiIO &io = ImGui::GetIO();
  if (const Layer *target = layer_find(*C.document, layer_pick_target(C, view, g_layer_click.start));
      target && C.document->overlays.show)
  {
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

/* The ViewportActionPanel (editors/ui.hh, ui_action_panel_draw), kept by the project as other panels are. */
static constexpr const char *ACTION_PANEL_KEY = "Viewport/Adjust Last Operation";

/* Points proportional editing could drag along rest where `modal.snapshot` has them: after a frame
 * change while playing, where the new frame has them, so they play on rather than being keyed where
 * the drag began. */
static void modal_rest_follow(ModalTransform &modal)
{
  for (ModalTransform::Item &item : modal.items) {
    const Layer *layer = item.proportional ? layer_find(modal.snapshot, item.layer) : nullptr;
    if (!layer) {
      continue;
    }
    const VectorDrawing &drawing = layer->drawing;
    if (item.curve < 0) {
      if (item.point >= 0 && item.point < int(drawing.points.size())) {
        item.rest = drawing.points[item.point].pos;
      }
    }
    else if (item.curve < int(drawing.curves.size()) && item.index < int(drawing.curves[item.curve].points.size())) {
      const CurvePoint &cp = drawing.curves[item.curve].points[item.index];
      item.rest = item.pre ? cp.handle_in : cp.handle_out;
    }
  }
}

bool viewport_precise_edit()
{
  return g_modal.active || (g_bone_modal.active && g_bone_modal.edits_known);
}

/* Set by the viewport being middle-dragged, each frame (viewport_navigating). */
static bool g_navigating = false;

bool viewport_navigating()
{
  return g_navigating;
}

bool viewport_drag_active()
{
  return g_modal.active || g_bone_modal.active || g_layer_modal.active || g_radius_modal.active ||
         gizmo_moving(g_gizmo.mode) || gizmo_moving(g_point_gizmo.mode);
}

/* What the drag going on has held (animation_follow_playback); forgotten when it ends. */
static std::vector<char> g_follow_held;

void viewport_drag_follow_forget()
{
  g_follow_held.clear();
}

void viewport_drag_follow_playback(Document &dragged, Document &now)
{
  std::vector<char> &held = g_follow_held;
  if (g_modal.active && animation_follow_playback(g_modal.snapshot, dragged, now, held)) {
    modal_rest_follow(g_modal);
  }
  if (gizmo_moving(g_point_gizmo.mode) && animation_follow_playback(g_point_gizmo.moving.snapshot, dragged, now, held)) {
    modal_rest_follow(g_point_gizmo.moving);
  }
  if (g_layer_modal.active) {
    animation_follow_playback(g_layer_modal.snapshot, dragged, now, held);
  }
  if (g_radius_modal.active) {
    animation_follow_playback(g_radius_modal.snapshot, dragged, now, held);
  }
  if (g_bone_modal.active && animation_follow_playback(g_bone_modal.snapshot, dragged, now, held)) {
    /* Each update starts the bones from these. */
    if (const Layer *armature = layer_find(g_bone_modal.snapshot, g_bone_modal.armature)) {
      g_bone_modal.bones = armature->bones;
    }
    for (BoneModal::Also &also : g_bone_modal.also) {
      if (const Layer *armature = layer_find(g_bone_modal.snapshot, also.armature)) {
        also.bones = armature->bones;
      }
    }
  }
  if (gizmo_moving(g_gizmo.mode)) {
    Layer *was = layer_find(dragged, g_gizmo.armature);
    Layer *is = layer_find(now, g_gizmo.armature);
    if (was && is) {
      animation_follow_playback(g_gizmo.bones, was->bones, is->bones, held);
    }
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
  /* Media View draws with the overlays off, leaving the project's own setting as it was. */
  const bool overlays_shown = doc.overlays.show;
  if (C.wm->media_view) {
    doc.overlays.show = false;
  }
  viewport_draw_contents(area, C);
  if (C.wm->media_view) {
    doc.overlays.show = overlays_shown;
  }
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
  reach_update(C);
  Document &doc = *C.document;
  /* Detail rides its curves as they are edited (data/detail.hh). It changes drawings, never which layers
   * there are: found by an index. */
  {
    /* A layer whose revision is the one looked at last frame hasn't changed since: nothing to sync (a big document
     * has thousands, and looking through every drawing each frame costs milliseconds). */
    static std::unordered_map<uint64_t, uint64_t> synced;
    static uint64_t synced_latest = 0;
    if (revision_latest() != synced_latest) {
      synced_latest = revision_latest();
      const LayerIndexScope layer_index(doc);
      for (const Layer *flat : document_layers_flat(doc)) {
        uint64_t &seen = synced[flat->id];
        if (seen == flat->revision.value && seen != 0) {
          continue;
        }
        Layer *l = layer_find(doc, flat->id);
        if (l && l->type == LayerType::Vector && !l->is_reference() && drawing_has_detail(l->drawing)) {
          drawing_detail_sync(l->drawing, true);
        }
        seen = l ? l->revision.value : 0;
      }
    }
  }
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  ImGuiIO &io = ImGui::GetIO();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  /* The Sculpt and Draw tools' shelves take a strip along the bottom: the view is what's above it. */
  const float shelf_h = ts.tool == Tool::Draw         ? draw_shelf_height(C, area)
                        : ts.tool == Tool::MotionPath ? path_shelf_height(C, area)
                                                      : sculpt_shelf_height(C, area);
  const ImVec2 size = ImGui::GetContentRegionAvail() - ImVec2(0.0f, shelf_h);
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
  /* Alt right-click: up the layer tree, as = (the group holding the active layer). The click is spent,
   * so no context menu opens with it. */
  if (hovered && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !wm.modal_running) {
    operator_call(C, "layer.select_parent");
    g_right_click_spent = true;
  }
  /* In Front Of... / Behind...: the layer pointed at is the one it would go in front of or behind. */
  ui_draw_order_pick_update(C);
  ui_mask_pick_update(C);
  if (mouse_here && wm.draw_order_pick.mode != 0) {
    /* The group beside the layers being moved; Ctrl: the drawing itself. */
    const uint64_t hit = layer_under_mouse(doc, view, io.MousePos, wm.local_view);
    wm.draw_order_pick.hover = hit && !io.KeyCtrl ? draw_order_pick_target(doc, hit, wm.draw_order_pick.layers) : hit;
  }
  /* The bone eyedropper names the bone under the mouse. */
  if (const Layer *picking = layer_find(doc, ui_bone_eyedropper_armature()); picking && mouse_here) {
    if (const Bone *under = bone_find(*picking, pick_bone(doc, *picking, view, io.MousePos))) {
      ui_bone_eyedropper_hover(under->name);
    }
  }
  /* The point eyedropper names the point under the mouse. */
  if (mouse_here && ui_point_eyedropper_layer() != 0) {
    int number = 0;
    if (follow_pick_points(doc, view, nullptr, &number) != 0) {
      ui_bone_eyedropper_hover("Point " + std::to_string(number));
    }
  }
  /* The shape eyedropper names the shape under the mouse, and its layer. */
  if (mouse_here && ui_shape_eyedropper_active()) {
    int shape = -1;
    if (const Layer *under = layer_find(doc, layer_under_mouse(doc, view, io.MousePos, wm.local_view, &shape,
                                                                ui_shape_eyedropper_skip())))
    {
      const VectorDrawing shown = layer_has_modifier_effect(doc, *under, false) ? layer_evaluated_drawing(doc, *under, false)
                                                                                : layer_drawing(doc, *under);
      if (shape >= 0 && shape < int(shown.shapes.size())) {
        ui_shape_eyedropper_hover(under->name, shown.shapes[size_t(shape)].name);
      }
    }
  }
  if (mouse_here && ui_layer_eyedropper_active() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (const Layer *picking = layer_find(doc, ui_bone_eyedropper_armature())) {
      ui_bone_eyedropper_pick(C, pick_bone(doc, *picking, view, io.MousePos));
    }
    else if (ui_point_eyedropper_layer() != 0) {
      ui_point_eyedropper_pick(C, follow_pick_points(doc, view, nullptr, nullptr));
    }
    else if (ui_shape_eyedropper_active()) {
      int shape = -1;
      const uint64_t layer = layer_under_mouse(doc, view, io.MousePos, wm.local_view, &shape,
                                               ui_shape_eyedropper_skip());
      ui_shape_eyedropper_pick(C, layer, shape);
    }
    else {
      uint64_t hit = layer_under_mouse(doc, view, io.MousePos, wm.local_view);
      if (hit && wm.draw_order_pick.mode != 0 && !io.KeyCtrl) {
        hit = draw_order_pick_target(doc, hit, wm.draw_order_pick.layers);
      }
      ui_layer_eyedropper_pick(C, hit);
    }
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
  else if ((g_path_slide.active && ts.proportional || g_path_scale.active) && io.MouseWheel != 0.0f) {
    /* Edit's dot reach, in whole frames (up for more). */
    ts.path_proportional_size =
        std::clamp(ts.path_proportional_size + (io.MouseWheel > 0.0f ? 1.0f : -1.0f), 1.0f, 500.0f);
    g_path_slide.again = true;
    g_path_scale.again = true;
  }
  else if (hovered && io.MouseWheel != 0.0f) {
    const float2 under_mouse = view.to_document(io.MousePos);
    const float factor = std::pow(1.15f, io.MouseWheel);
    view.zoom = std::clamp(view.zoom * factor, VIEW_ZOOM_MIN, VIEW_ZOOM_MAX);
    view.center = under_mouse - (under_mouse - view.center) * (area.view_zoom / view.zoom);
    area.view_navigated = true;
    navigated = true;
  }
  g_navigating = region_active && ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !through_camera;
  if (region_active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) &&
      (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
  {
    if (io.KeyCtrl) {
      /* Ctrl + middle drag zooms about the middle of the view, as in Blender: up zooms in, down out,
       * about twice as big per 175 pixels dragged. */
      const float factor = std::pow(1.004f, -io.MouseDelta.y);
      view.zoom = std::clamp(view.zoom * factor, VIEW_ZOOM_MIN, VIEW_ZOOM_MAX);
    }
    else {
      view.center -= float2(io.MouseDelta.x, -io.MouseDelta.y) / view.zoom;
    }
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
  /* New renders of this area's canvas, counted for reading it back. */
  static std::map<int, uint64_t> render_serials;
  /* While pointing for In Front Of... / Behind..., the moving layers show where they would go. */
  std::vector<std::pair<Layer *, float>> order_preview;
  const WindowManager::DrawOrderPick &pick = wm.draw_order_pick;
  ui_draw_order_preview_begin(doc, order_preview, pick.hover, pick.mode, pick.layers);
  /* Guides (layers not rendered) are overlays: with them off, the viewport shows what renders. */
  renderer->set_show_guides(doc.overlays.show && doc.overlays.guides);
  renderer->set_media_view(wm.media_view);
  renderer->set_interactive(true);
  renderer->set_overscan(doc.shading.layer_caching);
  renderer->set_layer_runs(doc.shading.layer_caching);
  renderer->set_local_view(doc, wm.local_view);
  /* The Masking tab's Fills Mask / Lines Mask, while held. */
  const Layer *mask_previewed = wm.mask_preview.frame >= ImGui::GetFrameCount() - 1
                                    ? layer_find(doc, wm.mask_preview.layer)
                                    : nullptr;
  const uint64_t mask_preview_key =
      mask_previewed ? ((mask_previewed->id * 2 + (wm.mask_preview.lines ? 1 : 0)) * 1024 +
                        uint64_t(wm.mask_preview.upto + 1)) * 65536 + uint64_t(wm.mask_preview.step + 1)
                     : 0;
  renderer->set_mask_preview(mask_previewed ? mask_previewed->id : 0, wm.mask_preview.lines, wm.mask_preview.upto,
                             wm.mask_preview.step);
  /* The last render is used again while nothing it shows has changed: no layer (each change gives a new revision,
   * and anything not sure to have said so, a Python run, a widget being used, a drag, touches every layer), no undo
   * step, no frame, view, size or setting below, no video frame decoded, and it drew everything (a zoom's drawings
   * still being made don't count). A big drawing takes far longer to render than an idle frame or a panel's click
   * has. */
  struct LastRender {
    unsigned int texture = 0;
    int width = 0, height = 0;
    Transform2D to_pixels;
    int frame = 0, quality = 0, samples = 0;
    uint64_t changes = 0;
    /* Media decoded since (a video's frame arriving shows without anything else changing). */
    uint64_t media = 0;
    bool guides = false;
    std::vector<uint64_t> local_view;
    uint64_t mask_preview = 0;
    uint64_t revision = 0;
    bool media_view = false;
    bool complete = false;
    /* A knob shown somewhere by the Spots panel (anim/controller_preview.hh). */
    uint64_t preview = 0;
  };
  static std::map<int, LastRender> last_renders;
  LastRender &last = last_renders[area.id];
  const bool show_guides = doc.overlays.show && doc.overlays.guides;
  const bool same_view = std::memcmp(&last.to_pixels, &document_to_pixels, sizeof(Transform2D)) == 0;
  /* Everything but the view as it was. */
  const bool same_content = last.texture != 0 && last.width == width_px && last.height == height_px &&
                    last.frame == doc.frame_current && last.quality == doc.viewport_quality &&
                    last.samples == doc.viewport_samples && last.changes == C.undo->change_count() &&
                    last.media == media_generation() && last.guides == show_guides &&
                    last.local_view == wm.local_view && last.mask_preview == mask_preview_key &&
                    last.revision == revision_latest() && last.media_view == wm.media_view && last.complete &&
                    last.preview == controller_preview_stamp();
  /* Live bone physics moves bones without new revisions; a text typed on the canvas is safest drawn anew. */
  const bool still = same_content && !wm.playing && order_preview.empty() && !dynamics_live_running() &&
                     !wm.text_editing;
  /* Zooming: while the zoom keeps changing (a wheel turning, a pinch), the last image is shown scaled to where it
   * now goes, and drawn anew, sharp, once it has stopped for a moment. */
  struct Zooming {
    float zoom = 0.0f;
    double moved = -10.0;
  };
  static std::map<int, Zooming> zooming;
  Zooming &zoom_state = zooming[area.id];
  const float zoom_now = std::hypot(document_to_pixels.a, document_to_pixels.b);
  if (zoom_state.zoom != 0.0f && zoom_now != zoom_state.zoom) {
    zoom_state.moved = ImGui::GetTime();
  }
  zoom_state.zoom = zoom_now;
  constexpr double ZOOM_SETTLE = 0.15;
  const bool zoom_preview = doc.shading.zoom_preview && last.texture != 0 &&
                            ImGui::GetTime() - zoom_state.moved < ZOOM_SETTLE && !wm.playing &&
                            std::fabs(std::hypot(last.to_pixels.a, last.to_pixels.b) - zoom_now) > 1e-6f * zoom_now;
  /* Backdrops for zooming out (as browsers' and maps' lower-resolution levels): the scene around the view at less
   * detail, drawn on a frame with nothing else to do and kept while nothing it shows changes. While a zoom is shown
   * scaled they fill what the scaled image no longer covers, blurry, under it (never over anything sharper), until
   * the view is drawn sharp. Level 0, the overview: four times as wide and tall as the view at an eighth of its
   * resolution, there before any zoom shows. Level 1: twice as wide and tall at half the resolution, sharper for a
   * zoom out of up to two times; shown only while up to date. */
  struct Backdrop {
    std::unique_ptr<SceneRenderer> renderer;
    unsigned int texture = 0;
    Transform2D to_pixels;
    int width = 0, height = 0, frame = 0;
    uint64_t revision = ~uint64_t(0), changes = 0, media = 0;
    float zoom = 0.0f, scale = 0.0f;
    float2 center;
    bool complete = false, guides = false, media_view = false;
    std::vector<uint64_t> local_view;
  };
  /* Each level's resolution (a fraction of the view's) and how much more than the view it covers. */
  static constexpr int BACKDROP_LEVELS = 2;
  static constexpr int BACKDROP_DIVIDE[BACKDROP_LEVELS] = {8, 2};
  static constexpr float BACKDROP_COVER[BACKDROP_LEVELS] = {4.0f, 2.0f};
  static std::map<int, std::array<Backdrop, BACKDROP_LEVELS>> backdrops;
  std::array<Backdrop, BACKDROP_LEVELS> &levels = backdrops[area.id];
  const auto level_size = [&](int k) {
    return std::pair{std::max(width_px / BACKDROP_DIVIDE[k], 1), std::max(height_px / BACKDROP_DIVIDE[k], 1)};
  };
  /* A level's scale for the view at `at_zoom`: `cover` times the view's width in its pixels, rounded to a power of
   * two (as browsers' lower-resolution tilings snap to a few scales), so its drawings, worked out for that scale, are
   * drawn again as they were when it's drawn anew after a change or a small zoom. */
  const auto level_scale = [&](int k, float at_zoom) {
    const float ideal = at_zoom * float(level_size(k).first) / (BACKDROP_COVER[k] * float(width_px));
    return std::exp2(std::round(std::log2(std::max(ideal, 1e-12f))));
  };
  /* Up to date: what it shows is what the view shows now, about here and at about this zoom. */
  const auto level_fresh = [&](int k) {
    const Backdrop &b = levels[size_t(k)];
    const auto [bw, bh] = level_size(k);
    return b.texture != 0 && b.complete && b.revision == revision_latest() && b.changes == C.undo->change_count() &&
           b.media == media_generation() && b.frame == doc.frame_current && b.width == bw && b.height == bh &&
           b.scale == level_scale(k, zoom_now) &&
           length(view.center - b.center) * zoom_now < 0.25f * float(std::min(width_px, height_px)) &&
           b.guides == show_guides && b.media_view == wm.media_view && b.local_view == wm.local_view;
  };
  /* Drawn where the last sharp image was drawn (its zoom and place, not the zoom being moved to). */
  const float drawn_zoom = std::hypot(last.to_pixels.a, last.to_pixels.b);
  const float2 drawn_center = drawn_zoom > 0.0f
                                  ? float2((float(width_px) * 0.5f - last.to_pixels.tx) / drawn_zoom,
                                           (float(height_px) * 0.5f - last.to_pixels.ty) / drawn_zoom)
                                  : view.center;
  const auto draw_level = [&](int k, float at_zoom, float2 at_center) {
    Backdrop &b = levels[size_t(k)];
    if (!b.renderer) {
      b.renderer = std::make_unique<SceneRenderer>();
    }
    SceneRenderer &br = *b.renderer;
    br.set_show_guides(show_guides);
    br.set_media_view(wm.media_view);
    br.set_interactive(true);
    br.set_local_view(doc, wm.local_view);
    const auto [bw, bh] = level_size(k);
    const float bz = level_scale(k, at_zoom);
    const Transform2D to{bz, 0, 0, bz, float(bw) * 0.5f - at_center.x * bz, float(bh) * 0.5f - at_center.y * bz};
    b.texture = br.render(doc, bw, bh, to, std::max(doc.viewport_quality / (k == 0 ? 3 : 2), 2), k == 0 ? 2 : 4);
    b.to_pixels = to;
    b.width = bw;
    b.height = bh;
    b.frame = doc.frame_current;
    b.revision = revision_latest();
    b.changes = C.undo->change_count();
    b.media = media_generation();
    b.zoom = at_zoom;
    b.scale = bz;
    b.center = at_center;
    b.complete = br.complete();
    b.guides = show_guides;
    b.media_view = wm.media_view;
    b.local_view = wm.local_view;
  };
  unsigned int rendered = 0;
  bool zoom_scaled = false;
  if (still && same_view) {
    rendered = last.texture;
    /* Nothing else to do this frame, and nothing has changed for a moment (a burst of edits draws them once,
     * after): one stale level drawn anew (the overview first). */
    static uint64_t seen_revision = 0;
    static double seen_at = 0.0;
    if (revision_latest() != seen_revision) {
      seen_revision = revision_latest();
      seen_at = ImGui::GetTime();
    }
    if (doc.shading.zoom_preview && !wm.playing && ImGui::GetTime() - seen_at > 0.3) {
      for (int k = 0; k < BACKDROP_LEVELS; k++) {
        if (!level_fresh(k)) {
          draw_level(k, zoom_now, view.center);
          break;
        }
      }
    }
  }
  else if (still && renderer->pan_to(width_px, height_px, document_to_pixels)) {
    /* Panned within the last image's margin: the same image, another part of it shown. */
    rendered = last.texture;
    last.to_pixels = document_to_pixels;
  }
  else if (zoom_preview) {
    rendered = last.texture;
    zoom_scaled = true;
    /* An overview there before the zoom shows: one a little out of date (an edit just before) stands in, as only its
     * edges show; none (or another frame's) is drawn first. */
    const Backdrop &overview = levels[0];
    if (overview.texture == 0 || overview.frame != doc.frame_current || overview.width != level_size(0).first) {
      draw_level(0, drawn_zoom, drawn_center);
    }
  }
  else {
    renderer->set_zooming(!doc.shading.zoom_preview && ImGui::GetTime() - zoom_state.moved < ZOOM_SETTLE);
    /* Everything it draws from but the view (as same_content above): the same as the last render's, only the view has
     * moved, and the renderer keeps what it worked out of each drawing. Not while bones move without new revisions
     * (live physics) or a text is typed on the canvas. */
    uint64_t content = 0;
    if (!dynamics_live_running() && !wm.text_editing && order_preview.empty()) {
      const auto mix = [&content](uint64_t v) { content = content * 0x100000001b3ull ^ (v + 0x9e3779b97f4a7c15ull); };
      mix(uint64_t(uint32_t(doc.frame_current)));
      mix(uint64_t(doc.viewport_quality) | uint64_t(doc.viewport_samples) << 16);
      mix(C.undo->change_count());
      mix(media_generation());
      mix(revision_latest());
      mix(mask_preview_key);
      mix(uint64_t(wm.media_view));
      for (const uint64_t id : wm.local_view) {
        mix(id);
      }
      mix(controller_preview_stamp());
      content |= 1;
    }
    renderer->set_content(content);
    /* Hovering a spot in the Spots panel: the drawing posed there, from a copy (nothing in the document moves). */
    const Document *previewed = controller_preview_document(doc);
    rendered = renderer->render(previewed ? *previewed : doc, width_px, height_px, document_to_pixels, doc.viewport_quality,
                                doc.viewport_samples);
    render_serials[area.id]++;
    last = {rendered,         width_px,          height_px,          document_to_pixels,
            doc.frame_current, doc.viewport_quality, doc.viewport_samples, C.undo->change_count(),
            media_generation(), show_guides,       wm.local_view,      mask_preview_key,
            revision_latest(), wm.media_view,     renderer->complete(), controller_preview_stamp()};
  }
  if (const unsigned int texture = rendered) {
    dl->AddCallback(premultiplied_blend, nullptr);
    if (zoom_scaled) {
      /* The last image's corners where they go in the view now. */
      const Transform2D image_to_now = document_to_pixels * renderer->image_to_document();
      const float w = float(renderer->image_width()), h = float(renderer->image_height());
      const auto at = [&](float x, float y) {
        const float2 p = image_to_now.apply({x, y});
        return origin + ImVec2(p.x / fb_scale, size.y - p.y / fb_scale);
      };
      dl->PushClipRect(origin, origin + size, true);
      /* The backdrops first, blurriest at the bottom, where the scaled image doesn't reach (zooming out). The
       * middle level only while it shows what the view does now. */
      for (int k = 0; k < BACKDROP_LEVELS; k++) {
        const Backdrop &b = levels[size_t(k)];
        const bool current = b.revision == revision_latest() && b.changes == C.undo->change_count();
        if (b.texture == 0 || b.frame != doc.frame_current || (k > 0 && !current)) {
          continue;
        }
        const Transform2D level_to_now = document_to_pixels * b.to_pixels.inverted();
        const float bw = float(b.width), bh = float(b.height);
        const auto bat = [&](float x, float y) {
          const float2 q = level_to_now.apply({x, y});
          return origin + ImVec2(q.x / fb_scale, size.y - q.y / fb_scale);
        };
        dl->AddImageQuad(ImTextureRef(ImTextureID(intptr_t(b.texture))), bat(0, bh), bat(bw, bh), bat(bw, 0),
                         bat(0, 0), ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0), ImVec2(0, 0));
      }
      dl->AddImageQuad(ImTextureRef(ImTextureID(intptr_t(texture))), at(0, h), at(w, h), at(w, 0), at(0, 0),
                       ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0), ImVec2(0, 0));
      dl->PopClipRect();
    }
    else {
      float uv[4];
      renderer->shown_uv(uv);
      dl->AddImage(ImTextureRef(ImTextureID(intptr_t(texture))), origin, origin + size, ImVec2(uv[0], uv[3]),
                   ImVec2(uv[2], uv[1]));
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  }
  /* The canvas read back for the overlays to look under themselves (overlay_contrast.hh): begun after a render (not
   * while playing, at most a few times a second) and taken a frame or so later, so nothing waits for the GPU; the
   * overlays keep the colours they have meanwhile. Only a picture that really changed (not a click that picked
   * something, which draws the same one again) makes them work their colours out again. */
  {
    struct Readback {
      CanvasSample sample;
      std::vector<uint8_t> incoming;
      Transform2D incoming_to_pixels;
      uint64_t hash = 0;
      uint64_t read = 0;
      bool pending = false;
      double at = -10.0;
    };
    static std::map<int, Readback> readbacks;
    Readback &rb = readbacks[area.id];
    const uint64_t drawn = render_serials[area.id];
    int w = 0, h = 0;
    if (rb.pending && renderer->read_pixels_take(rb.incoming, &w, &h)) {
      rb.pending = false;
      const uint64_t hash = canvas_hash(rb.incoming, w, h, rb.incoming_to_pixels, doc.background);
      if (hash != rb.hash) {
        rb.hash = hash;
        rb.sample.rgba.swap(rb.incoming);
        rb.sample.width = w;
        rb.sample.height = h;
        rb.sample.document_to_pixels = rb.incoming_to_pixels;
        rb.sample.background = doc.background;
        rb.sample.serial++;
      }
    }
    if (rendered && drawn != rb.read && !wm.playing && ImGui::GetTime() - rb.at >= 0.15 &&
        renderer->read_pixels_begin())
    {
      rb.pending = true;
      rb.incoming_to_pixels = renderer->image_to_document().inverted();
      rb.read = drawn;
      rb.at = ImGui::GetTime();
    }
    g_canvas_now = rb.sample.rgba.empty() ? nullptr : &rb.sample;
  }
  if (mask_previewed) {
    /* Says what's shown, over the black and white. */
    const int upto = wm.mask_preview.upto;
    std::string row;
    if (upto >= 0 && upto < int(mask_previewed->masks.size())) {
      const LayerMask &card = mask_previewed->masks[size_t(upto)];
      row = " " + (card.name.empty() ? std::to_string(upto + 1) + " (" + mask_label(doc, card) + ")"
                                     : "\"" + card.name + "\"");
    }
    const std::string what = wm.mask_preview.step >= 0
                                 ? "As far as this masking area, of" + row + ":  white shows, black is hidden"
                                 : std::string(wm.mask_preview.lines ? "Lines" : "Fills") + " mask" + row + " of " +
                                       mask_previewed->name + ":  white shows, black is hidden";
    const ImVec2 at = origin + ImVec2(ImGui::GetFrameHeight() * 2.0f, ImGui::GetFrameHeight() * 1.6f);
    const ImVec2 text = ImGui::CalcTextSize(what.c_str());
    dl->AddRectFilled(at - ImVec2(6, 3), at + text + ImVec2(6, 3), IM_COL32(0xe0, 0x70, 0x10, 230), 4.0f);
    dl->AddText(at, IM_COL32_WHITE, what.c_str());
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
  /* The camera's frame, the canvas outside it darkened, and its name above it (orange while picked).
   * A picked camera shows it even with the overlays off: it is what's being worked on. */
  if ((ov.show && ov.camera_frame) || through_camera) {
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
  /* Tools that work on every edited layer's points, not the active layer's drawing: they work with a
   * group active too (Edit Descendants brings the drawings inside it). */
  const bool point_tool = ts.tool == Tool::Select || ts.tool == Tool::SplitCurve || ts.tool == Tool::Cursor ||
                          ts.tool == Tool::TransformPoints || ts.tool == Tool::Sculpt;
  const auto points_to_edit = [&]() { return drawing || (point_tool && !editable_layers(C).empty()); };
  bool points_editable = points_to_edit();
  /* The weight tools, on the active drawing or the drawings in the active group or bone layer. */
  const bool weighing = (ts.tool == Tool::WeightPaint || ts.tool == Tool::WeightSelect) &&
                        (drawing || (document_active_layer(doc) && document_active_layer(doc)->is_group()));
  const Transform2D to_document = layer ? layer_world_transform(doc, layer->id) : Transform2D();
  const Transform2D to_layer = to_document.inverted();
  const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
  const float radius = pick_line_pixels() * pixel;
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
  /* The guide under the mouse with a guide tool, drawn brighter next frame. */
  static int annotate_hovered_guide = -1;
  if (ts.tool != Tool::Annotate) {
    annotate_hovered_guide = -1;
  }
  draw_annotation_overlays(dl, C, view, annotate_hovered_guide);
  draw_deform_grids(dl, C, view);
  draw_deform_range(dl, C, view);
  draw_follow_points(dl, C, view);
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
  else if (!g_modal.active && follow_point_input(C, area, view, hovered)) {
    /* The light blue point has the mouse. */
  }
  else if (hovered && !g_modal.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && layer_pick_wanted(C, view)) {
    g_layer_click = {true, area.id, io.MousePos};
    layer_pick_update(C, view, dl);
  }
  else if (ts.tool == Tool::SelectLayer && !g_modal.active) {
    /* The tool: the layer a click would pick, outlined. */
    if (hovered && ov.show) {
      if (const Layer *target = layer_find(doc, layer_pick_target(C, view, io.MousePos))) {
        draw_layer_box(dl, doc, *target, view);
      }
    }
  }
  else if (ts.tool == Tool::Annotate && !g_modal.active) {
    annotate_hovered_guide = annotate_tool_input(C, area, view, hovered, dl);
  }
  else if (const TextToolView text_view{[&](float2 p) { return view.to_screen(p); },
                                        [&](ImVec2 p) { return view.to_document(p); }, view.zoom};
           !g_modal.active && text_editing_wanted(C, text_view, hovered))
  {
    /* Typing, and the Edit Text tool on a text layer. */
    text_editing_input(C, text_view, hovered, dl);
  }
  else if (ts.tool == Tool::TransformLayer && !g_modal.active) {
    layer_gizmo_input(C, area, view, hovered, dl);
  }
  else if (ts.tool == Tool::MotionPath && !g_modal.active) {
    /* draw_motion_paths, below, takes its clicks and drags. */
  }
  else if (tool_setup_only(ts.tool) && doc.frame_current != 0 && !g_modal.active) {
    /* A setup-only tool kept picked away from frame 0: a click says what makes it work. */
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      report_banner(ReportType::Warning, "Setup frame only: press Tab to go to frame 0");
    }
  }
  else if (ts.tool == Tool::TransformPoints && points_editable && !g_modal.active) {
    transform_points_input(C, area, view, hovered, dl);
  }
  else if (ts.tool == Tool::Sculpt && points_editable && !g_modal.active) {
    sculpt_input(C, area, view, hovered, dl);
  }
  else if (ts.tool == Tool::Draw && points_editable && !g_modal.active && draw_adjust_input(C, area, pixel, hovered, dl)) {
    /* F or Shift F: the line's width or opacity, until a click. */
  }
  else if (ts.tool == Tool::WeightPaint && weighing && !g_modal.active) {
    weight_paint_input(C, view, hovered, dl);
  }
  else if (ts.tool == Tool::WeightSelect && weighing && !g_modal.active) {
    weight_select_input(C, area, view, hovered, dl);
  }
  else if (armature_active && !g_modal.active) {
    bone_tool_input(C, area, *armature_active, view, hovered, dl);
  }
  else if (g_modal.active && g_modal.area == area.id) {
    modal_info = modal_update(C, view);
    /* Proportional editing's reach, a circle about the middle of what moves, as Blender's. */
    if (ov.show && g_modal.active && g_modal.proportional && ts.proportional) {
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
    points_editable = points_to_edit();
  }
  else if (hovered && points_editable) {
    /* Shift right-click places the cursor from any tool, as in Blender. */
    if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      ts.cursor = view.to_document(io.MousePos);
    }
    if (ts.tool == Tool::Draw && ts.draw_mode != DrawMode::Pen && ts.draw_cursor_circle && !g_drag.active) {
      /* The line's width round the mouse, in black and white to show on any drawing. */
      const float half = draw_width(ts, ts.draw_mode) * 0.5f / pixel;
      if (half >= 1.5f) {
        dl->AddCircle(io.MousePos, half, IM_COL32(0x00, 0x00, 0x00, 170), 0, 1.0f);
        dl->AddCircle(io.MousePos, half + 1.0f, IM_COL32(0xff, 0xff, 0xff, 170), 0, 1.0f);
      }
    }
    /* New artwork is drawn on the setup frame, so it exists on every frame (as bones are added
     * there); other frames animate what is drawn. */
    const bool draws = ts.tool == Tool::Draw || ts.tool == Tool::Shape;
    if (draws && io.MousePos.y < origin.y + ImGui::GetFrameHeight() + 8.0f) {
      /* A click between the header's buttons reaches the view, but isn't on the drawing. */
    }
    else if (draws && doc.frame_current != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
            /* Ctrl or the pen's eraser end erases with its matching eraser, as Grease Pencil's Ctrl does. */
            const bool erase = io.KeyCtrl || wm.pen_eraser;
            g_drag.ink_mode = erase && ts.draw_mode == DrawMode::FillPen    ? DrawMode::FillEraser
                              : erase && ts.draw_mode == DrawMode::Freehand ? DrawMode::StrokeEraser
                                                                            : ts.draw_mode;
            g_drag.ink.begin(ts.ink, {pixel, draw_width(ts, g_drag.ink_mode) * 0.5f,
                                      2.0f / std::max(float(doc.height), 1.0f)});
            if ((g_drag.pen_stroke = pen_touch_pressed())) {
              draw_take_pen(view, to_layer);
            }
          }
          if (g_drag.active && g_drag.ink.empty() && ts.draw_mode != DrawMode::Pen) {
            g_drag.ink.add({mouse_layer, wm.pen_pressure, {}, ImGui::GetTime()});
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
          /* On a point (an edge in Edges mode): dragging it moves it, a click still just picks. A handle takes the
           * press only if no point is nearer (as a click picks: the nearest within reach). */
          const PressGrab grab = select_press_grab(C, view, io.MousePos, io.KeyShift || io.KeyCtrl);
          if (!handle.layer || io.KeyShift || io.KeyCtrl || grab.distance < handle.distance) {
            g_drag.grab = grab;
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
  else if (hovered && !points_editable && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ts.tool != Tool::Cursor) {
    const Layer *active = document_active_layer(doc);
    report(ReportType::Info, !active                 ? "Add a layer to draw on (Layers editor, +)"
                             : active->is_media()    ? "This is a Media layer: G, R and S move, turn and scale it, right-click for Fit to Camera Frame; add a vector layer above it to draw over it"
                             : active->is_text()     ? "This is a text layer: click its text to type in it; G, R and S move, turn and scale it"
                             : active->is_group()    ? "Select a vector layer to draw on"
                             : active->is_reference() ? "This is a reference layer: Break Reference to draw on it"
                                                      : "This layer is locked");
  }

  /* Dragging and releasing. */
  if (g_drag.active && g_drag.area == area.id && points_editable) {
    const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
    switch (ts.tool) {
      case Tool::Select:
        if (g_drag.grab.layer) {
          if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, g_drag.grab.threshold)) {
            const PressGrab grab = g_drag.grab;
            const ImVec2 start = g_drag.start;
            g_drag = {};
            /* The frame carries on drawing (a return here skipped the rest of the view for a frame: a flicker). */
            select_press_grab_move(C, area, view, grab, start);
          }
          break;
        }
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
        if (g_drag.pen_stroke) {
          draw_take_pen(view, to_layer);
        }
        else if (distance(mouse_layer, g_drag.ink.last_sample()) > pixel * 0.25f) {
          g_drag.ink.add({mouse_layer, wm.pen_pressure, {}, ImGui::GetTime()});
        }
        {
          /* With a pen, a moment more of the line, the way it is going: the frame shown is a little
           * behind the hand, and this keeps the line at the pen's tip. The next frame draws the real
           * samples in its place. */
          float2 tip = g_drag.ink.last_sample();
          if (g_drag.pen_stroke && g_drag.pen_stroke == pen_stroke_id() && ts.draw_prediction > 0.0f) {
            const PenSample ahead = pen_predict(pen_stroke(), ts.draw_prediction * 1e-3, 40.0f);
            if (!pen_stroke().empty()) {
              tip = to_layer.apply(view.to_document(ImVec2(ahead.x, ahead.y)));
            }
          }
          /* An eraser shows where it has been; it cuts on lifting. */
          draw_tool_preview(dl, view, to_document,
                            draw_mode_cuts(g_drag.ink_mode)        ? Color{0.9f, 0.3f, 0.3f, 0.4f}
                            : g_drag.ink_mode == DrawMode::FillPen ? ts.style.fill
                                                                   : ts.style.line,
                            tip);
        }
        break;
      case Tool::Cursor:
        ts.cursor = view.to_document(io.MousePos);
        break;
      case Tool::Shape:
        if (dragging) {
          const VectorDrawing preview = primitive_drawing(
              ts.shape, shape_drag_points(ts.shape, g_drag.shape_start, shape_snap(C, mouse_layer)), ts.style,
              ts.draw_fill, ts.grid_columns, ts.grid_rows);
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
            draw_take_pen(view, to_layer);
            if (draw_stroke_commit(C, *layer, g_drag.ink, g_drag.ink_mode)) {
              push_undo(C, g_drag.ink_mode == DrawMode::FillEraser     ? "Erase Fill"
                           : g_drag.ink_mode == DrawMode::StrokeEraser ? "Erase Line"
                           : g_drag.ink_mode == DrawMode::Trimmer      ? "Trim Line"
                           : g_drag.ink_mode == DrawMode::FillPen      ? "Paint Fill"
                                                                       : "Draw");
            }
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
  /* A gizmo may have put back an earlier document: look the layer up again. */
  drawing = editable_drawing(C, &layer);
  if (!pen_active(ts) && g_pen.layer) {
    pen_finish(C);
  }

  /* Onion skins, under the wires of what is being edited (not while playing: every frame would have
   * them worked out again). */
  if (ov.show && doc.onion_skin && !wm.playing) {
    draw_onion_skins(C, view, dl);
  }

  /* Overlays for the layer being edited. They only draw: layers looked up by an index (each edited
   * drawing finds its place, masks and bones), not by searching the tree each time. */
  std::optional<LayerIndexScope> overlay_index;
  overlay_index.emplace(doc);
  if (weighing) {
    for (const Layer *weighed : document_weight_layers(doc)) {
      draw_weight_overlay(dl, C, *weighed, view, std::clamp(doc.viewport_quality, 1, 64));
    }
  }
  else if (!drawing && ov.show && (ov.wireframe || ov.points)) {
    /* A bone layer being worked in: the wires and points of the drawings in its shown bone
     * collections. */
    for (Layer *editing : editable_layers(C)) {
      const LineCutter cut = line_cutter(doc, *editing, false);
      const Posed shown = posed(doc, *editing);
      draw_drawing_overlay(overlay_slot(area, editing->id), *editing, shown,
                           layer_world_transform(doc, editing->id), view, ov.wireframe, points_overlay_shown(ov, ts),
                           std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts),
                           ts.select_mode != SelectMode::Edge, &cut, ov.point_size, ov.handle_size);
    }
    overlay_flush(dl);
  }
  if (drawing) {
    if (ov.show && (ov.wireframe || ov.points)) {
      /* Every layer being edited shows its wires and points. */
      for (Layer *editing : editable_layers(C)) {
        if (editing->id == layer->id) {
          continue;
        }
        const LineCutter cut = line_cutter(doc, *editing, false);
        const Posed shown = posed(doc, *editing);
        draw_drawing_overlay(overlay_slot(area, editing->id), *editing, shown,
                             layer_world_transform(doc, editing->id), view, ov.wireframe, points_overlay_shown(ov, ts),
                             std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts),
                             ts.select_mode != SelectMode::Edge, &cut, ov.point_size, ov.handle_size);
      }
      const Posed shown = posed(doc, *layer);
      const LineCutter cut = line_cutter(doc, *layer, false);
      draw_drawing_overlay(overlay_slot(area, layer->id), *layer, shown, to_document, view, ov.wireframe,
                           points_overlay_shown(ov, ts), std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts),
                           ts.select_mode != SelectMode::Edge, &cut, ov.point_size, ov.handle_size);
      overlay_flush(dl);
    }
    overlay_index.reset();
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
  overlay_index.reset();
  /* Bones, as the Armature overlay says: every visible bone layer's, or only selected ones'. Unless
   * it's Off, the weight tools show only the bones they work with, the selected ones solid: no others can be used. */
  if (ov.show) {
    const Layer *painting = (ts.tool == Tool::WeightPaint || ts.tool == Tool::WeightSelect) &&
                                    ov.armature != ArmatureDisplay::Off
                                ? weight_armature(C)
                                : nullptr;
    /* With the active bone layer, the rest of its family: the bone layer around it and the sub bone
     * groups shown inside (anim/bone_family.hh), drawn as other bone layers are. */
    std::set<uint64_t> family, worked_in;
    if (const Layer *active = document_active_layer(doc); active && active->is_bone_layer()) {
      for (const Layer *l : armature_family(doc, active->id)) {
        family.insert(l->id);
      }
      for (const Layer *l : armature_and_inside(doc, active->id)) {
        worked_in.insert(l->id);
      }
    }
    const auto visit = [&](auto &&self, const std::vector<Layer> &list, bool shown) -> void {
      for (const Layer &l : list) {
        const bool visible = shown && l.shown();
        if (l.is_bone_layer() && visible &&
            (painting ? &l == painting
                      : ov.armature == ArmatureDisplay::On ||
                            (ov.armature == ArmatureDisplay::Selected &&
                             (l.selected || l.id == doc.active_layer || family.contains(l.id)))))
        {
          /* The bone layer being worked in and the sub bone groups inside it, worked with it, are drawn solid. */
          g_plain_armature = doc.frame_current != 0 ? 0
                             : g_bone_modal.active               ? g_bone_modal.armature
                             : g_gizmo.mode != GizmoMode::None  ? g_gizmo.armature
                                                                 : 0;
          /* Those around it are faint, but what's picked in them shows. */
          draw_bones(dl, doc, l, view, !worked_in.contains(l.id) && l.id != doc.active_layer, &l == painting || family.contains(l.id));
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
    for (const LayerMask &m : layer_mask_cards(doc, *masked)) {
      if (!m.enabled) {
        continue;
      }
      for (const MaskStep &s : m.steps) {
        const auto outline = [&](const MaskStep &area) {
          for (const uint64_t id : area.layers) {
            const bool hovered_mask = std::find(wm.mask_hover.begin(), wm.mask_hover.end(), id) != wm.mask_hover.end();
            if (const Layer *source = layer_find(doc, id); source && !hovered_mask) {
              draw_mask_outline(dl, doc, *source, view, m.start_shown && mask_step_cuts(area), false);
            }
          }
        };
        outline(s);
        for (const MaskStep &child : s.children) {
          outline(child);
        }
      }
    }
  }
  /* A material under the mouse in the Shape panel's list: every shape using it, outlined. */
  if (wm.material_hover != 0 && ImGui::GetFrameCount() - wm.material_hover_frame <= 1) {
    for (const Layer *l : document_layers_flat(doc)) {
      if (!l->has_shapes() || !l->shown()) {
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
        for (const LayerMask &m : layer_mask_cards(doc, *masked)) {
          for (const MaskStep &s : m.steps) {
            outside |= m.start_shown && mask_step_cuts(s) &&
                       std::find(s.layers.begin(), s.layers.end(), id) != s.layers.end();
          }
        }
      }
      draw_mask_outline(dl, doc, *source, view, outside, true);
    }
  }
  if (ov.show) {
    draw_motion_paths(C, area, view, dl, hovered);
    /* Edit Layer: the Transform Layer box round the layer, taking the presses the paths left (Edit's; the other
     * brushes paint wherever pressed). */
    if (ts.tool == Tool::MotionPath && ts.path_edit_layer && ts.path_brush == PathBrush::Edit && !g_modal.active &&
        !g_path_drag.active && !g_path_slide.active && !g_path_brush.active && !g_path_brush.pending)
    {
      layer_gizmo_input(C, area, view, hovered, dl);
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
  /* The Motion Path tool too, while no path keys or dots are picked (then the path's menu is the one). */
  const bool path_tool_points = ts.tool == Tool::MotionPath && !g_path_drag.active && path_edit_selection(C).empty() &&
                                wm.path_handle_picks.empty();
  if (hovered && (ts.tool == Tool::Select || path_tool_points) && !io.KeyShift && !g_modal.active && drawing &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right) &&
      !g_right_click_spent)
  {
    bool any = false;
    for (Layer *editing : editable_layers(C)) {
      any |= drawing_selected_alignment(editing->drawing) != -1;
    }
    /* With nothing selected: the active layer's Draw Order (the Select tool's). */
    if (any || ts.tool == Tool::Select) {
      ImGui::OpenPopup(any ? "##point_menu" : "##draw_order");
    }
  }
  if (hovered && wm.menu_request == "draw_order") {
    ImGui::OpenPopup("##draw_order");
    wm.menu_request.clear();
  }
  motion_path_menus(C, hovered);
  /* Right-click on an active Media layer: its menu. */
  if (const Layer *active = document_active_layer(doc);
      hovered && active && active->is_media() && !g_modal.active && !g_layer_modal.active &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right) &&
      !g_right_click_spent)
  {
    ImGui::OpenPopup("##media_menu");
  }
  if (ui_menu_begin("##media_menu")) {
    const Layer *menu_layer = document_active_layer(doc);
    ImGui::TextDisabled("%s", menu_layer ? menu_layer->name.c_str() : "Media");
    ImGui::Separator();
    menu_operator(C, "Fit to Camera Frame", "media.fit_to_camera");
    ImGui::Separator();
    menu_operator(C, "Clear Location", "layer.clear_location");
    menu_operator(C, "Clear Rotation", "layer.clear_rotation");
    menu_operator(C, "Clear Scale", "layer.clear_scale");
    ui_menu_end();
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
    if (const Layer *sw = switch_of(doc, doc.active_layer)) {
      if (ui_submenu_begin(("Switch: " + sw->name).c_str())) {
        ui_switch_menu_items(C, sw->id);
        ui_submenu_end();
      }
    }
    ui_menu_end();
  }
  bone_parent_menus(C, hovered);
  ui_bone_collection_menus(C, hovered);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    g_right_click_spent = false;
  }
  snap_pie(C, hovered, area.id);
  /* Ctrl L: Blender's Link/Transfer Data menu, for bones or for shapes. */
  if (hovered && wm.menu_request == "link_data") {
    ImGui::OpenPopup("##link_data");
    wm.menu_request.clear();
  }
  if (ui_menu_begin("##link_data")) {
    ImGui::TextDisabled("Link/Transfer Data");
    ImGui::Separator();
    const Layer *active = document_active_layer(doc);
    if (active && active->is_bone_layer()) {
      menu_operator(C, "Copy Constraints", "armature.constraints_copy");
      menu_operator(C, "Copy Constraints with Drivers", "armature.constraints_copy", {{"drivers", "1"}});
    }
    else {
      menu_operator(C, "Link Materials", "material.link_selected");
    }
    /* Layers: from the active one to the others picked in the Layers editor. */
    ImGui::Separator();
    menu_operator(C, "Copy Masks", "layer.masks_copy");
    ui_menu_end();
  }
  /* Shift A on a drawing: Blender's Add menu. */
  if (hovered && wm.menu_request == "mesh_add") {
    ImGui::OpenPopup("##mesh_add");
    wm.menu_request.clear();
  }
  if (ui_menu_begin("##mesh_add")) {
    add_menu_items(C);
    ui_menu_end();
  }
  /* M on points: Blender's Merge menu. */
  if (hovered && wm.menu_request == "mesh_merge") {
    ImGui::OpenPopup("##mesh_merge");
    wm.menu_request.clear();
  }
  if (ui_menu_begin("##mesh_merge")) {
    ImGui::TextDisabled("Merge");
    ImGui::Separator();
    menu_operator(C, "Merge", "mesh.merge");
    menu_operator(C, "Merge by Distance", "mesh.merge_by_distance");
    ui_menu_end();
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
    menu_operator(C, "Smooth Points", "mesh.smooth_points");
    menu_operator(C, "Subdivide", "mesh.subdivide");
    ImGui::Separator();
    menu_operator(C, "Join Fills", "mesh.join_fills");
    menu_operator(C, "Separate Fills", "mesh.separate_fills");
    ImGui::Separator();
    /* Mirror, as Blender's: across the document's axes or the layer's own. */
    if (ui_submenu_begin("Mirror")) {
      menu_operator(C, "X Global", "transform.mirror", {{"axis", "X"}, {"orientation", "GLOBAL"}});
      menu_operator(C, "Y Global", "transform.mirror", {{"axis", "Y"}, {"orientation", "GLOBAL"}});
      ImGui::Separator();
      menu_operator(C, "X Local", "transform.mirror", {{"axis", "X"}, {"orientation", "LOCAL"}});
      menu_operator(C, "Y Local", "transform.mirror", {{"axis", "Y"}, {"orientation", "LOCAL"}});
      ui_submenu_end();
    }
    menu_operator(C, "Symmetrize", "mesh.symmetrize");
    ImGui::Separator();
    /* Detail levels: up a level; detail points picked, down a level, or back onto the curve of the
     * level below. */
    bool detail = false;
    for (const Layer *editing : editable_layers(C)) {
      detail |= !editing->is_reference() && drawing_detail_selected(editing->drawing);
    }
    menu_operator(C, "Move to Higher Level", "mesh.detail_raise");
    if (detail) {
      menu_operator(C, "Move to Lower Level", "mesh.detail_lower");
      menu_operator(C, "Smooth to Lower Level", "mesh.detail_smooth");
    }
    ImGui::Separator();
    menu_operator(C, "Delete", "mesh.delete");
    ImGui::Separator();
    if (ui_submenu_begin("Draw Order")) {
      ui_draw_order_menu_items(C, 0);
      ui_submenu_end();
    }
    if (const Layer *sw = switch_of(doc, doc.active_layer)) {
      if (ui_submenu_begin(("Switch: " + sw->name).c_str())) {
        ui_switch_menu_items(C, sw->id);
        ui_submenu_end();
      }
    }
    ui_menu_end();
  }

  /* Show Points' dots, on the layer being edited; on every drawing in a group when the group is active. */
  if (ov.show) {
    const Layer *active_owner = document_active_layer(doc);
    if (active_owner && active_owner->is_group()) {
      for (const Layer *inside : document_layers_flat(doc)) {
        if (inside->type == LayerType::Vector && inside->shown() && layer_inside(doc, inside->id, active_owner->id)) {
          draw_modifier_points(dl, C, *inside, layer_world_transform(doc, inside->id), view, ov.point_size,
                               points_overlay_shown(ov, ts));
        }
      }
    }
    else if (const Layer *dotted = active_owner && active_owner->type == LayerType::Vector ? active_owner : nullptr) {
      draw_modifier_points(dl, C, *dotted, layer_world_transform(doc, dotted->id), view, ov.point_size,
                           points_overlay_shown(ov, ts));
    }
  }

  /* Header, tool settings, tool bar, and the info line in the corner. */
  header_draw(area, C, origin, size.x);
  if (preferences().viewport_tool_settings) {
    tool_settings_draw(area, C, origin + ImVec2(0.0f, ImGui::GetFrameHeight() + 8.0f), size.x);
  }
  ui_status_hint_set(tool_hint(ts));
  const float header_h = header_rows_height();
  toolbar_draw(C, origin, header_h + 6.0f, size.y - header_h - 16.0f);

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
  ui_action_panel_draw(C, "Viewport", ACTION_PANEL_KEY, origin, size, bar_w + 4.0f);
  if (ov.show && ov.info) {
    dl->AddText(origin + ImVec2(bar_w + 10.0f, header_h + 8.0f), IM_COL32(0xe6, 0xe6, 0xe6, 255), info);
  }
  /* Editing a fix: say so where the work is, overlays or not (keys made now go in the fix, not the
   * mainline), with how much of it this frame is. */
  if (const Layer *fix_owner = doc.action.empty() || wm.media_view ? nullptr : layer_find(doc, doc.action_layer)) {
    if (const FixAction *fix = fix_action_find(*fix_owner, doc.action)) {
      /* On one of its poses, which; between them, that keys here don't play. */
      const int pose = fix_pose_on_frame(*fix, doc.frame_current);
      std::string label = "Editing fix: " + fix->name + "  |  ";
      if (pose >= 0) {
        label += "pose " + std::to_string(pose + 1) + " of " + std::to_string(fix->poses.size()) + ": " +
                 fix_pose_label(*fix_owner, *fix, fix->poses[size_t(pose)]);
      }
      else {
        std::string frames;
        for (const FixPose &p : fix->poses) {
          frames += (frames.empty() ? "" : ", ") + std::to_string(p.frame);
        }
        label += "frame " + std::to_string(doc.frame_current) + " isn't a pose: keys here don't play (poses on " +
                 (fix->poses.size() == 1 ? "frame " : "frames ") + frames + ")";
      }
      const float line = ov.show && ov.info ? ImGui::GetTextLineHeightWithSpacing() + 4.0f : 0.0f;
      const ImVec2 at = origin + ImVec2(bar_w + 10.0f, header_h + 8.0f + line);
      dl->AddRectFilled(at - ImVec2(6, 3), at + ImGui::CalcTextSize(label.c_str()) + ImVec2(6, 3),
                        pose >= 0 ? UI_FIX_ACTIVE_COLOR & 0xd8ffffff : IM_COL32(0x8a, 0x3a, 0x3a, 0xd8), 4.0f);
      dl->AddText(at, IM_COL32(0xf2, 0xee, 0xff, 255), label.c_str());
    }
  }
  /* Media View hides the overlays, the info line with them: a word says why, and how to leave it. */
  if (wm.media_view) {
    const char *label = "Media View  (` to leave)";
    const ImVec2 at = origin + ImVec2(bar_w + 10.0f, header_h + 8.0f);
    dl->AddRectFilled(at - ImVec2(6, 3), at + ImGui::CalcTextSize(label) + ImVec2(6, 3), IM_COL32(0x18, 0x18, 0x18, 200), 4.0f);
    dl->AddText(at, IM_COL32(0xe6, 0xe6, 0xe6, 255), label);
  }
  if (!modal_info.empty()) {
    const ImVec2 text = ImGui::CalcTextSize(modal_info.c_str());
    const ImVec2 at = origin + ImVec2((size.x - text.x) * 0.5f, header_h + 8.0f);
    dl->AddRectFilled(at - ImVec2(8, 4), at + text + ImVec2(8, 4), IM_COL32(0x18, 0x18, 0x18, 220), 4.0f);
    dl->AddText(at, IM_COL32(0xff, 0xff, 0xff, 255), modal_info.c_str());
  }

  if (shelf_h > 0.0f) {
    (ts.tool == Tool::Draw         ? draw_shelf_draw
     : ts.tool == Tool::MotionPath ? path_shelf_draw
                                   : sculpt_shelf_draw)(C, area, origin + ImVec2(0.0f, size.y), ImVec2(size.x, shelf_h));
  }
  if (wm.draw_popup_request) {
    wm.draw_popup_request = false;
    ImGui::OpenPopup("##draw_presets");
  }
  draw_presets_popup(C);
  /* GPU overlays of layers no longer edited go after a while. */
  overlay_collect();
  /* Claim the whole region so the window doesn't scroll. */
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(1, 1));
}

}  // namespace oanim

