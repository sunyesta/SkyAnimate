/* The Motion Path tool's brush shelf (editors/tool_shelf.hh), while it is the tool: tabs All and Shape; a tile
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
    case PathBrush::BlendSpeed: {
      /* The dots evenly spaced where the brush has been. */
      const int n = 9;
      const std::vector<float> step(n - 1, 1.0f);
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
        dots(dl, c, s, 1, [&](int) { return at[size_t(i)]; }, col);
      }
      break;
    }
    case PathBrush::SurfaceSmooth: {
      /* A wiggly arc and the clean arc it becomes. */
      std::vector<ImVec2> wiggly;
      for (int i = 0; i <= 24; i++) {
        const float u = float(i) / 24.0f;
        wiggly.push_back(c + ImVec2((u - 0.5f) * 1.6f * s, -std::sin(u * 3.14159f) * 0.45f * s + 0.2f * s +
                                                                std::sin(u * 40.0f) * 0.07f * s));
      }
      dl->AddPolyline(wiggly.data(), int(wiggly.size()), COL_FAINT, ImDrawFlags_None, t * 0.8f);
      arc_line(dl, c, s, col, 0.45f, t);
      break;
    }
    case PathBrush::Pinch: {
      /* The dots crowding toward the middle. */
      arc_line(dl, c, s, COL_FAINT, 0.45f, t * 0.6f);
      const auto pinched = [](int i) {
        const float u = float(i) / 8.0f - 0.5f;
        return 0.5f + u * std::abs(u) * 2.0f;
      };
      dots(dl, c, s, 9, pinched, col);
      break;
    }
    case PathBrush::Slide: {
      /* Dots along the arc, an arrow along it. */
      arc_line(dl, c, s, COL_FAINT, 0.45f, t * 0.6f);
      dots(dl, c, s, 7, even, col);
      const ImVec2 a = c + ImVec2(-0.3f * s, 0.45f * s), b = c + ImVec2(0.3f * s, 0.45f * s);
      dl->AddLine(a, b, COL_ARROW, t);
      dl->AddTriangleFilled(b + ImVec2(0.14f * s, 0), b + ImVec2(-0.02f * s, -0.1f * s),
                            b + ImVec2(-0.02f * s, 0.1f * s), COL_ARROW);
      break;
    }
  }
}

/* 0 All, 1 Shape (Grab, Smooth), 2 Timing (Blend Speed, Pinch, Slide). */
static bool in_tab(PathBrush brush, int tab)
{
  if (tab == 1) {
    return path_brush_is_sculpt(brush);
  }
  if (tab == 2) {
    return path_brush_is_timing(brush);
  }
  return true;
}

static const char *brush_tip(PathBrush brush)
{
  switch (brush) {
    case PathBrush::Edit:
      return "Click a key or a dot to select it (Shift: add or take out; double click: go to its frame). Drag a key "
             "to move it, a dot along its path (proportional editing: O), a selected key's handles to bend the path "
             "(Alt breaks them). Alt click the path: a control point there, shaping it only (Alt drag: and move it; "
             "Alt Shift: a breakdown on the arc). G R S on the selected keys, V their handle types, X deletes them; "
             "Alt S scales the timing around the selection. Shift drag: Blend Speed";
    case PathBrush::Grab:
      return "The Sculpt tool's Grab on the path: the path under the brush follows the mouse, the middle most, its "
             "keys, control points and handles fitted to it; each stretch keeps its timing. Shift: Smooth";
    case PathBrush::Smooth:
      return "The Sculpt tool's Smooth on the path: brush over a lumpy stretch to even it out; keys stay on their "
             "frames, each stretch keeping its timing";
    case PathBrush::SurfaceSmooth:
      return "The Sculpt tool's Surface Smooth on the path: its lumps and wiggles go, its arcs stay (Lump "
             "Sensitivity: how big a lump); each stretch keeps its timing";
    case PathBrush::BlendSpeed:
      return "Even out the spacing where you brush. Ctrl sharpens it: slow parts slower, fast parts faster";
    case PathBrush::Pinch:
      return "Crowd the dots together where you brush, so the move slows there. Ctrl spreads them: faster. Shift: "
             "Blend Speed. The keys and the path's shape stay";
    case PathBrush::Slide:
      return "Drag the dots inside the circle along the path, the ones nearer its edge going less. The keys and the "
             "path's shape stay. Shift: Blend Speed";
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
  shelf.tab = std::min(shelf.tab, 2);
  if (!shelf_frame(shelf, {{0, "All"}, {1, "Shape"}, {2, "Timing"}}, min, size)) {
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
    static constexpr PathBrush ORDER[PATH_BRUSH_COUNT] = {PathBrush::Edit,          PathBrush::Grab,
                                                          PathBrush::Smooth,        PathBrush::SurfaceSmooth,
                                                          PathBrush::BlendSpeed,    PathBrush::Pinch,
                                                          PathBrush::Slide};
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
