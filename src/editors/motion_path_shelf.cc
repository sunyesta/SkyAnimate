/* The Motion Path tool's brush shelf (editors/tool_shelf.hh), while it is the tool: tabs All, Shape and Speed; a tile
 * per brush, its picture a path with what the brush does to it, its name beneath. Clicking a tile picks the brush. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "editors/editors.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

static constexpr ImU32 COL_INK = IM_COL32(0x1e, 0x1e, 0x1e, 255);
static constexpr ImU32 COL_FAINT = IM_COL32(0xa8, 0xa8, 0xa8, 255);
static constexpr ImU32 COL_ARROW = IM_COL32(0xe0, 0xa0, 0x20, 255);
static constexpr ImU32 COL_FAST = IM_COL32(0xd8, 0x40, 0x30, 255);
static constexpr ImU32 COL_SLOW = IM_COL32(0x30, 0x60, 0xd8, 255);

float path_shelf_height(const Context &C, const Area &area)
{
  return C.wm->tool_settings.tool == Tool::MotionPath ? shelf_height(area.path_shelf) : 0.0f;
}

/* A row of dots along an arc from left to right, `spacing(i)` (0..1) where dot i of n sits. */
template<typename F>
static void dots(ImDrawList *dl, ImVec2 c, float s, int n, F &&spacing, ImU32 col, float bump = 0.45f)
{
  for (int i = 0; i < n; i++) {
    const float u = spacing(i);
    const ImVec2 p = c + ImVec2((u - 0.5f) * 1.6f * s, -std::sin(u * 3.14159f) * bump * s + 0.25f * s);
    dl->AddCircleFilled(p, std::max(1.5f, s * 0.07f), col);
  }
}

static void arc_line(ImDrawList *dl, ImVec2 c, float s, ImU32 col, float bump, float t)
{
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 16; i++) {
    const float u = float(i) / 16.0f;
    pts.push_back(c + ImVec2((u - 0.5f) * 1.6f * s, -std::sin(u * 3.14159f) * bump * s + 0.25f * s));
  }
  dl->AddPolyline(pts.data(), int(pts.size()), col, ImDrawFlags_None, t);
}

void path_brush_icon(ImDrawList *dl, PathBrush brush, ImVec2 c, float s, unsigned int col)
{
  const float t = std::max(1.5f, s * 0.07f);
  const auto even = [](int i) { return float(i) / 6.0f; };
  switch (brush) {
    case PathBrush::Edit: {
      arc_line(dl, c, s, col, 0.45f, t * 0.6f);
      dots(dl, c, s, 7, even, col);
      /* A key and its handles. */
      const ImVec2 k = c + ImVec2(0.0f, -0.2f * s);
      dl->AddLine(k - ImVec2(0.45f * s, 0), k + ImVec2(0.45f * s, 0), COL_ARROW, t * 0.8f);
      dl->AddCircleFilled(k - ImVec2(0.45f * s, 0), t * 1.4f, COL_ARROW);
      dl->AddCircleFilled(k + ImVec2(0.45f * s, 0), t * 1.4f, COL_ARROW);
      const float r = s * 0.13f;
      const ImVec2 d[4] = {{k.x, k.y - r}, {k.x + r, k.y}, {k.x, k.y + r}, {k.x - r, k.y}};
      dl->AddConvexPolyFilled(d, 4, col);
      break;
    }
    case PathBrush::Grab:
      arc_line(dl, c, s, COL_FAINT, 0.2f, t);
      arc_line(dl, c, s, col, 0.75f, t);
      dl->AddLine(c + ImVec2(0, 0.05f * s), c + ImVec2(0, -0.35f * s), COL_ARROW, t);
      dl->AddTriangleFilled(c + ImVec2(0, -0.5f * s), c + ImVec2(-0.12f * s, -0.33f * s),
                            c + ImVec2(0.12f * s, -0.33f * s), COL_ARROW);
      break;
    case PathBrush::Smooth: {
      std::vector<ImVec2> lumpy;
      for (int i = 0; i <= 16; i++) {
        const float u = float(i) / 16.0f;
        lumpy.push_back(c + ImVec2((u - 0.5f) * 1.6f * s, -std::sin(u * 3.14159f) * 0.45f * s + 0.25f * s +
                                                               std::sin(u * 25.0f) * 0.09f * s));
      }
      dl->AddPolyline(lumpy.data(), int(lumpy.size()), COL_FAINT, ImDrawFlags_None, t);
      arc_line(dl, c, s, col, 0.45f, t);
      break;
    }
    case PathBrush::EditSpeed: {
      /* Dots along the arc, one picked out and an arrow along the path from it. */
      arc_line(dl, c, s, COL_FAINT, 0.45f, t * 0.6f);
      dots(dl, c, s, 9, [](int i) { return float(i) / 8.0f; }, col);
      const float u = 0.375f;
      const ImVec2 p = c + ImVec2((u - 0.5f) * 1.6f * s, -std::sin(u * 3.14159f) * 0.45f * s + 0.25f * s);
      dl->AddCircleFilled(p, std::max(2.5f, s * 0.12f), COL_ARROW);
      std::vector<ImVec2> along;
      for (int i = 0; i <= 8; i++) {
        const float v = u + 0.06f + float(i) * 0.03f;
        along.push_back(c + ImVec2((v - 0.5f) * 1.6f * s, -std::sin(v * 3.14159f) * 0.45f * s + 0.25f * s - 0.2f * s));
      }
      dl->AddPolyline(along.data(), int(along.size()), COL_ARROW, ImDrawFlags_None, t);
      const ImVec2 tip = along.back(), back = along[along.size() - 3];
      const ImVec2 dir = (tip - back) * (1.0f / std::max(1e-3f, std::sqrt((tip.x - back.x) * (tip.x - back.x) + (tip.y - back.y) * (tip.y - back.y))));
      const ImVec2 side(-dir.y, dir.x);
      dl->AddTriangleFilled(tip + dir * (0.12f * s), tip + side * (0.09f * s), tip - side * (0.09f * s), COL_ARROW);
      break;
    }
    case PathBrush::PaintSpeed:
    case PathBrush::Faster:
    case PathBrush::Slower:
    case PathBrush::BlendSpeed: {
      /* The dots' spacing: wide (fast, red) or close (slow, blue) where the brush has been. */
      const int n = 9;
      std::vector<float> step(n - 1, 1.0f);
      if (brush == PathBrush::PaintSpeed || brush == PathBrush::Faster) {
        step = {0.6f, 0.6f, 1.0f, 2.0f, 2.0f, 1.0f, 0.6f, 0.6f};
      }
      else if (brush == PathBrush::Slower) {
        step = {1.4f, 1.4f, 1.0f, 0.4f, 0.4f, 1.0f, 1.4f, 1.4f};
      }
      float total = 0.0f;
      for (const float v : step) {
        total += v;
      }
      std::vector<float> at(n, 0.0f);
      for (int i = 1; i < n; i++) {
        at[size_t(i)] = at[size_t(i - 1)] + step[size_t(i - 1)] / total;
      }
      arc_line(dl, c, s, COL_FAINT, 0.45f, t * 0.6f);
      for (int i = 0; i < n; i++) {
        const float v = i > 0 ? step[size_t(i - 1)] : step[0];
        const ImU32 dot = brush == PathBrush::BlendSpeed ? col : v > 1.2f ? COL_FAST : v < 0.8f ? COL_SLOW : col;
        dots(dl, c, s, 1, [&](int) { return at[size_t(i)]; }, dot);
      }
      if (brush == PathBrush::Faster || brush == PathBrush::Slower) {
        /* A plus or a minus, top right. */
        const ImVec2 m = c + ImVec2(0.55f * s, -0.55f * s);
        dl->AddLine(m - ImVec2(0.15f * s, 0), m + ImVec2(0.15f * s, 0), COL_ARROW, t * 1.3f);
        if (brush == PathBrush::Faster) {
          dl->AddLine(m - ImVec2(0, 0.15f * s), m + ImVec2(0, 0.15f * s), COL_ARROW, t * 1.3f);
        }
      }
      else if (brush == PathBrush::PaintSpeed) {
        /* A brush tip. */
        const ImVec2 m = c + ImVec2(0.5f * s, -0.55f * s);
        dl->AddLine(m + ImVec2(0.15f * s, -0.15f * s), m - ImVec2(0.05f * s, -0.05f * s), COL_ARROW, t * 1.6f);
        dl->AddCircleFilled(m - ImVec2(0.08f * s, -0.08f * s), t * 1.4f, COL_ARROW);
      }
      break;
    }
  }
}

static bool in_tab(PathBrush brush, int tab)
{
  switch (tab) {
    case 1:
      return !path_brush_is_speed(brush) && brush != PathBrush::EditSpeed;
    case 2:
      return path_brush_is_speed(brush) || brush == PathBrush::EditSpeed;
    default:
      return true;
  }
}

static const char *brush_tip(PathBrush brush)
{
  switch (brush) {
    case PathBrush::Edit:
      return "Click a dot to select its key and show its handles (Alt click: go to its frame too); drag a key to move it, a dot between keys to add "
             "one (Ctrl: onto the line between the keys, Shift: a breakdown), a selected key's handles to bend the "
             "path (Alt breaks them)";
    case PathBrush::Grab:
      return "Drag a soft area of the path: the keys under the brush follow the mouse, the nearest most";
    case PathBrush::Smooth:
      return "Brush over a lumpy stretch: each key under it moves toward the curve through the keys either side, "
             "and its dragged handles back toward Auto";
    case PathBrush::PaintSpeed:
      return "Paint a speed on the dots between keys (Speed in the header; 1 is the stretch's even speed). Ctrl "
             "paints 1 / Speed. Each key still arrives on its frame, the rest of the stretch making up for it";
    case PathBrush::Faster:
      return "Speed up where you brush, the rest of the stretch slowing to make up for it. Ctrl: Slower";
    case PathBrush::Slower:
      return "Slow down where you brush, the rest of the stretch speeding up to make up for it. Ctrl: Faster";
    case PathBrush::BlendSpeed:
      return "Even out the spacing where you brush. Ctrl sharpens it: slow parts slower, fast parts faster";
    case PathBrush::EditSpeed:
      return "Drag a dot between keys along its path: the keys stay on their frames, the rest of the stretch making "
             "up for it. Proportional editing (O; Shift O: falloff; the wheel while dragging: its size in frames) "
             "slides the dots around it too. Ctrl click: even spacing. Alt click: go to its frame";
  }
  return "";
}

void path_shelf_draw(Context &C, Area &area, ImVec2 min, ImVec2 size)
{
  if (size.y <= 0.0f) {
    return;
  }
  ToolSettings &ts = C.wm->tool_settings;
  ShelfState &shelf = area.path_shelf;
  ImGui::PushID("##path_shelf");
  if (!shelf_frame(shelf, {{0, "All"}, {1, "Shape"}, {2, "Speed"}}, min, size)) {
    ImGui::PopID();
    return;
  }
  const float tile = shelf_tile_size(shelf);
  const float th = shelf_tile_height(shelf);
  const float gap = 8.0f;
  const ImVec2 body_min = ImGui::GetCursorScreenPos();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  if (ImGui::BeginChild("##tiles", ImVec2(size.x, min.y + size.y - body_min.y), ImGuiChildFlags_None)) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos() + ImVec2(gap, 6.0f);
    const int per_row = std::max(1, int((size.x - gap) / (tile + gap)));
    const std::string search = shelf_lower(shelf.search);
    std::vector<PathBrush> shown;
    /* Edit Speed first among the speed brushes: it takes dots, as Edit does. */
    static constexpr PathBrush ORDER[PATH_BRUSH_COUNT] = {
        PathBrush::Edit,       PathBrush::Grab,   PathBrush::Smooth, PathBrush::EditSpeed,
        PathBrush::PaintSpeed, PathBrush::Faster, PathBrush::Slower, PathBrush::BlendSpeed};
    for (const PathBrush brush : ORDER) {
      if (in_tab(brush, shelf.tab) &&
          (search.empty() || shelf_lower(path_brush_name(brush)).find(search) != std::string::npos))
      {
        shown.push_back(brush);
      }
    }
    for (int k = 0; k < int(shown.size()); k++) {
      const PathBrush brush = shown[size_t(k)];
      const ImVec2 at = start + ImVec2(float(k % per_row) * (tile + gap), float(k / per_row) * (th + gap));
      ImGui::SetCursorScreenPos(at);
      ImGui::PushID(k);
      const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(tile, th));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 a = at, b = at + ImVec2(tile, tile);
      const bool active = ts.path_brush == brush;
      shelf_tile_box(dl, a, b, active, hovered);
      path_brush_icon(dl, brush, (a + b) * 0.5f, tile * 0.38f, COL_INK);
      shelf_tile_name(dl, a, b, path_brush_name(brush), active);
      if (hovered) {
        ImGui::SetTooltip("%s\n%s", path_brush_name(brush), brush_tip(brush));
      }
      if (clicked) {
        operator_call(C, "anim.motion_path_brush_set", {{"brush", path_brush_id(brush)}});
      }
      ImGui::PopID();
    }
    if (shown.empty()) {
      ImGui::SetCursorScreenPos(start);
      ImGui::TextDisabled("No brushes match");
    }
    const int rows = (int(shown.size()) + per_row - 1) / per_row;
    ImGui::SetCursorScreenPos(start + ImVec2(0, float(std::max(rows, 1)) * (th + gap)));
    ImGui::Dummy(ImVec2(1, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::PopID();
}

}  // namespace oanim
