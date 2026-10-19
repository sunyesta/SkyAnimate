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
#include <cfloat>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

#include "core/log.hh"
#include "data/document.hh"
#include "editors/editors.hh"
#include "editors/ui.hh"
#include "gpu/scene_renderer.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/event_system.hh"
#include "wm/keymap.hh"

namespace sky {

/* Blender's edit-mode colours. */
static constexpr ImU32 COL_VIEWPORT_BACK = IM_COL32(0x3d, 0x3d, 0x3d, 255);
static constexpr ImU32 COL_WIRE = IM_COL32(0x20, 0x20, 0x20, 170);
static constexpr ImU32 COL_WIRE_SELECTED = IM_COL32(0xff, 0x8c, 0x00, 220);
static constexpr ImU32 COL_POINT = IM_COL32(0x10, 0x10, 0x10, 230);
static constexpr ImU32 COL_POINT_SELECTED = IM_COL32(0xff, 0x8c, 0x00, 255);
static constexpr ImU32 COL_HIGHLIGHT = IM_COL32(0x47, 0x72, 0xb3, 255);
/* Handle colours by type, unselected and selected, from Blender's default theme. */
static constexpr ImU32 COL_HANDLE[4][2] = {
    {IM_COL32(0x90, 0x90, 0x00, 255), IM_COL32(0xf0, 0xff, 0x40, 255)}, /* Automatic */
    {IM_COL32(0x40, 0x90, 0x30, 255), IM_COL32(0x40, 0xc0, 0x30, 255)}, /* Vector */
    {IM_COL32(0x80, 0x30, 0x60, 255), IM_COL32(0xf0, 0x90, 0xa0, 255)}, /* Aligned */
    {IM_COL32(0x10, 0x10, 0x10, 255), IM_COL32(0xd0, 0x30, 0x30, 255)}, /* Free */
};
static constexpr ImU32 COL_HEADER = IM_COL32(0x30, 0x30, 0x30, 235);
static constexpr ImU32 COL_TOOLBAR = IM_COL32(0x28, 0x28, 0x28, 235);

/* Pick radius in screen pixels. */
static constexpr float PICK_PIXELS = 9.0f;

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

static void view_fit(Area &area, const Document &doc, ImVec2 size)
{
  const float aspect = float(doc.width) / float(std::max(doc.height, 1));
  area.view_center_x = 0.0f;
  area.view_center_y = 0.0f;
  area.view_zoom = std::max(1.0f, std::min(size.x / (2.0f * aspect * 1.15f), size.y / (2.0f * 1.15f)));
  area.view_fit_width = size.x;
  area.view_fit_height = size.y;
}

/* ---------------------------------------------------------------------------------
 * Interaction state. One tool or modal transform runs at a time, so these are shared.
 */

/* A running G R S or E. It works in document space, so points on several layers (each with its
 * own transform) move together. */
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
  };
  std::vector<Item> items;
  float2 pivot;
  float2 start_mouse;
  /* 0 free, 1 X only, 2 Y only. */
  int axis = 0;
  std::string typed;
  /* Started by dragging a handle: ends when the mouse button is let go. */
  bool tweak = false;
  /* Only handles move: S lengthens and R turns each handle about its own point. */
  bool about_points = false;
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
  if (!ov.show || !ov.points || (ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Freehand)) {
    return HandleDisplay::None;
  }
  return ov.handles;
}

static bool handle_visible(const VectorDrawing &drawing, const CurvePoint &cp, HandleDisplay display)
{
  return display == HandleDisplay::All || (display == HandleDisplay::Selected && curve_point_selected(drawing, cp));
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
    const VectorDrawing &drawing = layer->drawing;
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
};

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
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 panel_min = origin + ImVec2(pad, top);
  const ImVec2 panel_max = panel_min + ImVec2(button + pad * 2, (button + 2) * float(std::size(TOOLS)) + pad * 2);
  dl->AddRectFilled(panel_min, panel_max, COL_TOOLBAR, 6.0f);

  for (size_t i = 0; i < std::size(TOOLS); i++) {
    const ToolInfo &info = TOOLS[i];
    const ImVec2 p = panel_min + ImVec2(pad, pad + float(i) * (button + 2));
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(int(i));
    if (ImGui::InvisibleButton("##tool", ImVec2(button, button))) {
      ts.tool = info.tool;
    }
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ts.tool == info.tool;
    if (active || hovered) {
      dl->AddRectFilled(p, p + ImVec2(button, button), active ? COL_HIGHLIGHT : IM_COL32(0x45, 0x45, 0x45, 255),
                        4.0f);
    }
    const ImU32 icon_col = IM_COL32(0xe6, 0xe6, 0xe6, 255);
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
      const std::string shortcut = keymaps().shortcut_for(info.op_idname);
      ImGui::SetTooltip("%s\n%s%s%s", info.name, info.description, shortcut.empty() ? "" : "\nShortcut: ",
                        shortcut.c_str());
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
  return ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                      ImGuiTreeNodeFlags_SpanAvailWidth);
}

/* The overlays toggle and the arrow that opens their settings, like Blender's. */
static void overlays_buttons(Area &area, ToolSettings &ts, float h, float arrow_w)
{
  ViewportOverlays &ov = area.overlays;
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
    static const char *const handle_modes[] = {"None", "Selected", "All"};
    int mode = int(ov.handles);
    if (ImGui::Combo("##handles", &mode, handle_modes, 3)) {
      ov.handles = HandleDisplay(mode);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Which curve handles to show: none, the selected points' handles, or all of them");
    }
  }
  if (overlay_section("Armature")) {
    overlay_checkbox_pair("Bones", &ov.bones, nullptr, nullptr, column);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Bones arrive in milestone 4");
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
  if (ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, operator_poll(C, *ot))) {
    operator_call(C, idname);
    return true;
  }
  return false;
}

/* The header: editor menu, Select and Drawing menus, the active tool's settings, snapping. */
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
  ImGui::SameLine();
  if (header_menu("Drawing")) {
    menu_operator(C, "Create Shape", "mesh.create_shape");
    menu_operator(C, "Create Line", "mesh.create_line");
    menu_operator(C, "Delete Shape", "mesh.delete_shape");
    if (ImGui::BeginMenu("Arrange")) {
      menu_operator(C, "Bring to Front", "mesh.shape_raise_to_front");
      menu_operator(C, "Bring Forward", "mesh.shape_raise");
      menu_operator(C, "Send Backward", "mesh.shape_lower");
      menu_operator(C, "Send to Back", "mesh.shape_lower_to_back");
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Set Handle Type")) {
      for (const HandleType type : {HandleType::Auto, HandleType::Vector, HandleType::Aligned, HandleType::Free}) {
        if (ImGui::MenuItem(handle_type_name(type))) {
          operator_call(C, "mesh.handle_type_set", {{"type", handle_type_identifier(type)}});
        }
      }
      ImGui::EndMenu();
    }
    menu_operator(C, "Peak", "mesh.peak");
    menu_operator(C, "Smooth", "mesh.smooth");
    ImGui::Separator();
    menu_operator(C, "Extrude", "mesh.extrude");
    menu_operator(C, "Duplicate", "mesh.duplicate_move");
    menu_operator(C, "Delete Points", "mesh.delete");
    ImGui::EndPopup();
  }

  ImGui::SameLine(0.0f, h);
  switch (ts.tool) {
    case Tool::Select:
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
  }

  /* On the right, as in Blender: snapping, then the overlays toggle and its dropdown. */
  const ImGuiStyle &style = ImGui::GetStyle();
  const float snap_w = ImGui::CalcTextSize("Snap").x + style.FramePadding.x * 2.0f;
  const float arrow_w = h * 0.75f;
  const float right_w = snap_w + h * 0.6f + h + 1.0f + arrow_w;
  ImGui::SameLine();
  const float right = origin.x + width - right_w - 6.0f;
  if (ImGui::GetCursorScreenPos().x < right) {
    ImGui::SetCursorScreenPos(ImVec2(right, ImGui::GetCursorScreenPos().y));
  }
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
  ImGui::SameLine(0.0f, h * 0.6f);
  overlays_buttons(area, ts, h, arrow_w);
}

/* ---------------------------------------------------------------------------------
 * Modal transform (G, R, S, E)
 */

static bool modal_start(Context &C, Area &area, char kind, float2 mouse_document)
{
  const std::vector<Layer *> layers = editable_layers(C);
  if (layers.empty()) {
    return false;
  }
  ModalTransform modal;
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
      report(ReportType::Warning, "Extrude works from the ends of open curves");
      return false;
    }
  }

  float2 sum;
  int counted = 0;
  for (Layer *layer : layers) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    VectorDrawing &drawing = layer->drawing;
    for (int i = 0; i < int(drawing.points.size()); i++) {
      if (drawing.points[i].selected) {
        const float2 p = to_document.apply(drawing.points[i].pos);
        modal.items.push_back({layer->id, i, p});
        sum += p;
        counted++;
      }
    }
    /* Handles after the points, so they're placed once their points have moved. */
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      Curve &curve = drawing.curves[c];
      for (int i = 0; i < int(curve.points.size()); i++) {
        CurvePoint &cp = curve.points[i];
        const bool point_moves = drawing.points[cp.point].selected;
        if (!point_moves && (cp.select_in || cp.select_out)) {
          /* Grabbing an Automatic or Vector handle makes it Aligned or Free, as in Blender. */
          if (cp.handle_type == HandleType::Auto) {
            curve_point_set_handle_type(drawing, curve, i, HandleType::Aligned);
          }
          else if (cp.handle_type == HandleType::Vector) {
            curve_point_set_handle_type(drawing, curve, i, HandleType::Free);
          }
        }
        const bool stored = cp.handle_type == HandleType::Aligned || cp.handle_type == HandleType::Free;
        for (const bool pre : {true, false}) {
          const bool direct = !point_moves && (pre ? cp.select_in : cp.select_out);
          if (!direct && !(point_moves && stored)) {
            continue;
          }
          const float2 h = to_document.apply(curve_handle(drawing, curve, i, pre));
          ModalTransform::Item item{layer->id, cp.point, h};
          item.curve = c;
          item.index = i;
          item.pre = pre;
          item.direct = direct;
          item.origin = to_document.apply(drawing.points[cp.point].pos);
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
  modal.active = true;
  g_modal = std::move(modal);
  C.wm->modal_running = true;
  C.wm->modal_keys.clear();
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
static bool modal_key_take(Context &C, const char *type, bool any_modifiers = false)
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

/* Runs the transform for this frame; returns a line describing it for the header. */
static std::string modal_update(Context &C, const ViewMapping &view)
{
  ImGuiIO &io = ImGui::GetIO();
  /* Each moved point's layer, and its way from document space into that layer. */
  std::map<uint64_t, std::pair<Layer *, Transform2D>> layers;
  for (const ModalTransform::Item &item : g_modal.items) {
    if (!layers.count(item.layer)) {
      Layer *layer = layer_find(*C.document, item.layer);
      if (!layer || item.point >= int(layer->drawing.points.size())) {
        modal_end(C, false);
        return {};
      }
      layers[item.layer] = {layer, layer_world_transform(*C.document, item.layer).inverted()};
    }
  }
  /* Places one moved point or handle at a document-space position. Points come first in the
   * list, so a handle is placed relative to where its point now is. */
  const auto place = [&](const ModalTransform::Item &item, float2 p) {
    const auto &[layer, to_layer] = layers[item.layer];
    VectorDrawing &drawing = layer->drawing;
    if (item.curve < 0) {
      drawing.points[item.point].pos = to_layer.apply(p);
      return;
    }
    if (item.curve >= int(drawing.curves.size()) || item.index >= int(drawing.curves[item.curve].points.size())) {
      return;
    }
    Curve &curve = drawing.curves[item.curve];
    CurvePoint &cp = curve.points[item.index];
    if (item.direct) {
      /* An Aligned point's partner handle turns to stay in line, unless it is moving too. */
      const bool partner_moves = item.pre ? cp.select_out : cp.select_in;
      curve_point_move_handle(drawing, curve, item.index, item.pre, to_layer.apply(p), partner_moves);
    }
    else {
      (item.pre ? cp.handle_in : cp.handle_out) = to_layer.apply(p) - drawing.points[cp.point].pos;
    }
  };

  /* Keys while transforming, as in Blender. */
  if (modal_key_take(C, "X")) {
    g_modal.axis = g_modal.axis == 1 ? 0 : 1;
  }
  if (modal_key_take(C, "Y")) {
    g_modal.axis = g_modal.axis == 2 ? 0 : 2;
  }
  for (const ImWchar ch : io.InputQueueCharacters) {
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
      g_modal.typed.push_back(char(ch));
    }
  }
  while (modal_key_take(C, "BACK_SPACE", true)) {
    if (!g_modal.typed.empty()) {
      g_modal.typed.pop_back();
    }
  }
  if (modal_key_take(C, "ESC", true) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
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
  char typed_buf[32];
  std::snprintf(typed_buf, sizeof(typed_buf), "%s", g_modal.typed.c_str());
  const bool has_typed = !g_modal.typed.empty() && g_modal.typed != "-" && g_modal.typed != ".";
  const float typed = has_typed ? std::strtof(typed_buf, nullptr) : 0.0f;
  const char *axis_label = g_modal.axis == 1 ? "  along X" : (g_modal.axis == 2 ? "  along Y" : "");

  std::string info;
  char text[128];
  if (g_modal.kind == 'G') {
    float2 delta = mouse - g_modal.start_mouse;
    if (snap) {
      delta = snap_to_grid(delta, C.wm->tool_settings.grid_size);
    }
    if (has_typed) {
      delta = g_modal.axis == 2 ? float2(0, typed) : float2(typed, g_modal.axis == 1 ? 0.0f : delta.y * 0.0f);
    }
    if (g_modal.axis == 1) {
      delta.y = 0.0f;
    }
    else if (g_modal.axis == 2) {
      delta.x = 0.0f;
    }
    for (const ModalTransform::Item &item : g_modal.items) {
      place(item, item.start + delta);
    }
    std::snprintf(text, sizeof(text), "%s  X: %.3f  Y: %.3f%s", g_modal.extrude ? "Extrude" : "Move", delta.x,
                  delta.y, axis_label);
  }
  else if (g_modal.kind == 'R') {
    const float2 a = g_modal.start_mouse - g_modal.pivot, b = mouse - g_modal.pivot;
    float angle = std::atan2(cross(a, b), dot(a, b));
    float degrees = angle * 180.0f / PI;
    if (snap) {
      degrees = std::round(degrees / 5.0f) * 5.0f;
    }
    if (has_typed) {
      degrees = typed;
    }
    angle = degrees * PI / 180.0f;
    for (const ModalTransform::Item &item : g_modal.items) {
      const float2 c = g_modal.about_points ? item.origin : g_modal.pivot;
      place(item, c + rotate(item.start - c, angle));
    }
    std::snprintf(text, sizeof(text), "Rotate  %.1f\xC2\xB0", degrees);
  }
  else {
    const float start = std::max(distance(g_modal.start_mouse, g_modal.pivot), 1e-6f);
    float factor = distance(mouse, g_modal.pivot) / start;
    if (snap) {
      factor = std::round(factor * 10.0f) / 10.0f;
    }
    if (has_typed) {
      factor = typed;
    }
    const float2 f(g_modal.axis == 2 ? 1.0f : factor, g_modal.axis == 1 ? 1.0f : factor);
    for (const ModalTransform::Item &item : g_modal.items) {
      const float2 c = g_modal.about_points ? item.origin : g_modal.pivot;
      const float2 d = item.start - c;
      place(item, c + float2(d.x * f.x, d.y * f.y));
    }
    std::snprintf(text, sizeof(text), "Scale  %.3f%s", factor, axis_label);
  }
  info = text;
  if (!g_modal.typed.empty()) {
    info += "   [" + g_modal.typed + "]";
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
    VectorDrawing &drawing = layer->drawing;
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
  float best = FLT_MAX;
  const HandleHit handle = pick_handle(C, view, view.to_screen(mouse_document), handles);
  for (Layer *layer : layers) {
    const Transform2D to_document = layer_world_transform(*C.document, layer->id);
    const float pixel = 1.0f / std::max(view.zoom * to_document.scale(), 1e-6f);
    const float2 mouse = to_document.inverted().apply(mouse_document);
    if (const int p = drawing_pick_point(layer->drawing, mouse, PICK_PIXELS * pixel); p >= 0) {
      const float d = distance(layer->drawing.points[p].pos, mouse) / pixel;
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
      if (drawing_pick_segment(layer->drawing, to_document.inverted().apply(mouse_document), PICK_PIXELS * pixel,
                               &edge, &t))
      {
        int a, b;
        curve_segment_points(layer->drawing.curves[edge.curve], edge.segment, &a, &b);
        hit_layer = layer;
        picked = C.wm->tool_settings.select_mode == SelectMode::Edge ? std::vector<int>{a, b} : std::vector<int>{a};
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
    /* Handles shown before the box (whose points were selected then) can be boxed too. */
    std::vector<std::pair<CurvePoint *, int>> boxed;
    for (Curve &curve : drawing.curves) {
      for (int i = 0; i < int(curve.points.size()); i++) {
        CurvePoint &cp = curve.points[i];
        if (handles == HandleDisplay::All || (handles == HandleDisplay::Selected && curve_point_selected(drawing, cp))) {
          const float2 here = drawing.points[cp.point].pos;
          for (const bool pre : {true, false}) {
            const float2 h = curve_handle(drawing, curve, i, pre);
            if (distance(h, here) > 1e-9f && inside(h)) {
              boxed.push_back({&cp, pre ? 0 : 1});
            }
          }
        }
      }
    }
    for (MeshPoint &p : drawing.points) {
      if (inside(p.pos)) {
        p.selected = !io.KeyCtrl;
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
      /* Clicking the first point closes the curve. */
      curve.closed = true;
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
                                 HandleDisplay handles)
{
  std::vector<float2> samples;
  for (const Curve &curve : drawing.curves) {
    if (!show_wires) {
      break;
    }
    for (int s = 0; s < curve.segment_count(); s++) {
      samples.clear();
      curve_segment_sample(drawing, curve, s, quality, samples,
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
        const ImU32 col = COL_HANDLE[int(cp.handle_type)][selected ? 1 : 0];
        const ImVec2 end = view.to_screen(to_document.apply(h));
        dl->AddLine(at, end, col, 1.5f);
        dl->AddCircleFilled(end, 3.2f, (pre ? cp.select_in : cp.select_out) ? IM_COL32(0xff, 0xff, 0xff, 255) : col);
        dl->AddCircle(end, 3.2f, IM_COL32(0, 0, 0, 160));
      }
    }
  }
  const float r = 3.0f;
  for (int pass = 0; pass < 2; pass++) {
    for (const MeshPoint &p : drawing.points) {
      if (p.selected != (pass == 1)) {
        continue;
      }
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

void viewport_draw(Area &area, Context &C)
{
  Document &doc = *C.document;
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
  const bool hovered = ImGui::IsItemHovered();
  const bool region_active = ImGui::IsItemActive();

  ViewMapping view{origin, size, {area.view_center_x, area.view_center_y}, area.view_zoom};

  /* --- Navigation: wheel zooms at the mouse, middle drag pans, Home frames the camera. --- */
  if (hovered && io.MouseWheel != 0.0f) {
    const float2 under_mouse = view.to_document(io.MousePos);
    const float factor = std::pow(1.15f, io.MouseWheel);
    view.zoom = std::clamp(view.zoom * factor, 2.0f, 200000.0f);
    view.center = under_mouse - (under_mouse - view.center) * (area.view_zoom / view.zoom);
    area.view_navigated = true;
  }
  if (region_active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    view.center -= float2(io.MouseDelta.x, -io.MouseDelta.y) / view.zoom;
    area.view_navigated = true;
  }
  if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
    view_fit(area, doc, size);
    area.view_navigated = false;
    view.center = {area.view_center_x, area.view_center_y};
    view.zoom = area.view_zoom;
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
  dl->AddRectFilled(origin, origin + size, COL_VIEWPORT_BACK);
  const float aspect = float(doc.width) / float(std::max(doc.height, 1));
  const ImVec2 frame_min = view.to_screen({-aspect, 1.0f}), frame_max = view.to_screen({aspect, -1.0f});
  dl->AddRectFilled(frame_min, frame_max,
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
  if (const unsigned int texture = renderer->render(doc, width_px, height_px, document_to_pixels, doc.viewport_quality,
                                                             doc.viewport_samples)) {
    dl->AddCallback(premultiplied_blend, nullptr);
    dl->AddImage(ImTextureRef(ImTextureID(intptr_t(texture))), origin, origin + size, ImVec2(0, 1), ImVec2(1, 0));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  }
  const ViewportOverlays &ov = area.overlays;
  if (ov.show && ov.camera_frame) {
    dl->AddRect(frame_min, frame_max, IM_COL32(0x1d, 0x1d, 0x1d, 255), 0.0f, 0, 1.0f);
  }
  if (ov.show && ov.grid) {
    draw_grid(dl, view, ts.grid_size);
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
    }
    ts.tool_temporary = false;
  }

  std::string modal_info;
  if (wm.transform_request && hovered && !g_modal.active) {
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
    modal_start(C, area, g_pending_transform, view.to_document(io.MousePos));
    g_pending_transform = 0;
  }
  if (g_modal.active && g_modal.area == area.id) {
    modal_info = modal_update(C, view);
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
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
            const int p = drawing_split_segment(cut, hit.edge, io.KeyShift ? 0.5f : hit.t);
            for (Layer *editing : editable_layers(C)) {
              deselect_all(editing->drawing);
            }
            cut.points[p].selected = true;
            push_undo(C, "Split Curve");
          }
          split_curve_done(ts);
          break;
        }
        case Tool::Select: {
          /* Pressing on a shown handle drags it at once, as Blender's tweak does. */
          const HandleHit handle = pick_handle(C, view, io.MousePos, handles_shown(area.overlays, ts));
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
                             : active->is_group()    ? "Select a vector layer to draw on"
                             : active->is_reference() ? "This is a reference layer: edit its original"
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
            select_box(C, view, g_drag.start, io.MousePos, handles_shown(area.overlays, ts));
          }
          else {
            select_click(C, view, view.to_document(io.MousePos), handles_shown(area.overlays, ts));
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
  if (drawing) {
    if (ov.show && (ov.wireframe || ov.points)) {
      /* Every layer being edited shows its wires and points. */
      for (Layer *editing : editable_layers(C)) {
        if (editing->id == layer->id) {
          continue;
        }
        draw_drawing_overlay(dl, editing->drawing, layer_world_transform(doc, editing->id), view, ov.wireframe,
                             ov.points && !(ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Freehand),
                             std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts));
      }
      draw_drawing_overlay(dl, *drawing, to_document, view, ov.wireframe, ov.points && !(ts.tool == Tool::Draw && ts.draw_mode == DrawMode::Freehand),
                           std::clamp(doc.viewport_quality, 1, 64), handles_shown(ov, ts));
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
  if (ov.show && ov.cursor) {
    draw_cursor(dl, view.to_screen(ts.cursor));
  }

  /* Blender's V menu: Set Handle Type. */
  if (wm.handle_menu_requested && hovered) {
    wm.handle_menu_requested = false;
    ImGui::OpenPopup("##handle_type_menu");
  }
  if (ImGui::BeginPopup("##handle_type_menu")) {
    ImGui::TextDisabled("Set Handle Type");
    ImGui::Separator();
    for (const HandleType type : {HandleType::Auto, HandleType::Vector, HandleType::Aligned, HandleType::Free}) {
      if (ImGui::MenuItem(handle_type_name(type))) {
        operator_call(C, "mesh.handle_type_set", {{"type", handle_type_identifier(type)}});
      }
    }
    ImGui::EndPopup();
  }

  /* Header, tool bar, and the info line in the corner. */
  header_draw(area, C, origin, size.x);
  const float header_h = ImGui::GetFrameHeight() + 8.0f;
  toolbar_draw(C, origin, header_h + 6.0f);

  const Layer *active = document_active_layer(doc);
  char info[256];
  std::snprintf(info, sizeof(info), "Camera  |  Frame %d  |  %s%s", doc.frame_current,
                active ? active->name.c_str() : "No layer", active && active->is_reference() ? "  (reference)" : "");
  const float bar_w = ImGui::GetFrameHeight() * 1.35f + 16.0f;
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

}  // namespace sky
