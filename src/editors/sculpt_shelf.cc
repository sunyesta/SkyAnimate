/* The Sculpt tool's brush shelf (editors/tool_shelf.hh), while Sculpt is the tool: tabs All, Move, Shape and Mine; a
 * tile per preset, its picture a line before and after the brush, its name beneath. Clicking a tile picks the
 * preset; Mine's + tile saves the brush as it is now, and right-clicking a saved one deletes it. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "assets/sculpt_presets.hh"
#include "editors/editors.hh"
#include "imgui_stdlib.h"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

static constexpr ImU32 COL_INK = IM_COL32(0x1e, 0x1e, 0x1e, 255);
static constexpr ImU32 COL_INK_BEFORE = IM_COL32(0xa8, 0xa8, 0xa8, 255);
static constexpr ImU32 COL_ARROW = IM_COL32(0xe0, 0xa0, 0x20, 255);

float sculpt_shelf_height(const Context &C, const Area &area)
{
  return C.wm->tool_settings.tool == Tool::Sculpt ? shelf_height(area.sculpt_shelf) : 0.0f;
}

static void arrow(ImDrawList *dl, ImVec2 from, ImVec2 to, ImU32 col, float t)
{
  dl->AddLine(from, to, col, t);
  const ImVec2 d = to - from;
  const float len = std::max(std::sqrt(d.x * d.x + d.y * d.y), 1e-3f);
  const ImVec2 u(d.x / len, d.y / len), n(-u.y, u.x);
  const float h = t * 2.8f;
  dl->AddTriangleFilled(to + u * h, to - u * h + n * h, to - u * h - n * h, col);
}

/* A wavy line across the picture: y offset at x (-1 to 1), with `bump` lifting its middle. */
static std::vector<ImVec2> wave(ImVec2 c, float s, float bump, float shift = 0.0f)
{
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 24; i++) {
    const float x = -0.8f + 1.6f * float(i) / 24.0f;
    const float mid = std::exp(-((x - shift) * (x - shift)) / 0.08f);
    const float y = 0.12f * std::sin(x * 7.0f) - bump * mid;
    pts.push_back(c + ImVec2(x * s, (y + 0.15f) * s));
  }
  return pts;
}

void sculpt_brush_icon(ImDrawList *dl, SculptBrush brush, ImVec2 c, float s, ImU32 col, ImU32 before)
{
  /* The arrow fades with the icon (a tool greyed out). */
  const ImU32 arrow_col = (COL_ARROW & 0x00ffffffu) | ((((COL_ARROW >> 24) * (col >> 24)) / 255u) << 24);
  const float t = std::max(1.0f, s * 0.07f);
  switch (brush) {
    case SculptBrush::Grab: {
      dl->AddPolyline(wave(c, s, 0.0f).data(), 25, before, 0, t);
      dl->AddPolyline(wave(c, s, 0.55f).data(), 25, col, 0, t * 1.2f);
      arrow(dl, c + ImVec2(0, s * 0.05f), c + ImVec2(0, -s * 0.55f), arrow_col, t);
      break;
    }
    case SculptBrush::Push: {
      dl->AddPolyline(wave(c, s, 0.0f).data(), 25, before, 0, t);
      dl->AddPolyline(wave(c, s, 0.35f, 0.25f).data(), 25, col, 0, t * 1.2f);
      arrow(dl, c + ImVec2(-s * 0.45f, -s * 0.5f), c + ImVec2(s * 0.2f, -s * 0.5f), arrow_col, t);
      break;
    }
    case SculptBrush::Smooth: {
      std::vector<ImVec2> rough, smooth;
      for (int i = 0; i <= 12; i++) {
        const float x = -0.8f + 1.6f * float(i) / 12.0f;
        rough.push_back(c + ImVec2(x * s, (i % 2 ? -0.32f : 0.1f) * s));
      }
      for (int i = 0; i <= 24; i++) {
        const float x = -0.8f + 1.6f * float(i) / 24.0f;
        smooth.push_back(c + ImVec2(x * s, (0.25f + 0.08f * std::sin(x * 3.0f)) * s));
      }
      dl->AddPolyline(rough.data(), int(rough.size()), before, 0, t);
      dl->AddPolyline(smooth.data(), int(smooth.size()), col, 0, t * 1.2f);
      break;
    }
    case SculptBrush::SurfaceSmooth: {
      /* A lumpy arch and the same arch with its lumps gone. */
      std::vector<ImVec2> lumpy, even;
      for (int i = 0; i <= 32; i++) {
        const float x = -0.8f + 1.6f * float(i) / 32.0f;
        const float arch = 0.3f - 0.6f * (1.0f - x * x / 0.64f);
        lumpy.push_back(c + ImVec2(x * s, (arch + 0.07f * std::sin(x * 22.0f)) * s));
        even.push_back(c + ImVec2(x * s, (arch + 0.22f) * s));
      }
      dl->AddPolyline(lumpy.data(), int(lumpy.size()), before, 0, t);
      dl->AddPolyline(even.data(), int(even.size()), col, 0, t * 1.2f);
      break;
    }
    case SculptBrush::Thickness: {
      /* A line swelling to its middle. */
      std::vector<ImVec2> top, bottom;
      for (int i = 0; i <= 24; i++) {
        const float x = -0.8f + 1.6f * float(i) / 24.0f;
        const float half = 0.03f + 0.22f * std::exp(-(x * x) / 0.12f);
        const float y = 0.1f * std::sin(x * 4.0f);
        top.push_back(c + ImVec2(x * s, (y - half) * s));
        bottom.push_back(c + ImVec2(x * s, (y + half) * s));
      }
      std::vector<ImVec2> outline = top;
      outline.insert(outline.end(), bottom.rbegin(), bottom.rend());
      dl->AddConcavePolyFilled(outline.data(), int(outline.size()), col);
      break;
    }
    case SculptBrush::Pinch: {
      for (const float side : {-1.0f, 1.0f}) {
        std::vector<ImVec2> was, now;
        for (int i = 0; i <= 16; i++) {
          const float y = -0.7f + 1.4f * float(i) / 16.0f;
          was.push_back(c + ImVec2(side * 0.55f * s, y * s));
          now.push_back(c + ImVec2(side * (0.55f - 0.35f * std::exp(-(y * y) / 0.1f)) * s, y * s));
        }
        dl->AddPolyline(was.data(), int(was.size()), before, 0, t);
        dl->AddPolyline(now.data(), int(now.size()), col, 0, t * 1.2f);
      }
      arrow(dl, c + ImVec2(-s * 0.75f, 0), c + ImVec2(-s * 0.35f, 0), arrow_col, t * 0.8f);
      arrow(dl, c + ImVec2(s * 0.75f, 0), c + ImVec2(s * 0.35f, 0), arrow_col, t * 0.8f);
      break;
    }
    case SculptBrush::Twist: {
      std::vector<ImVec2> spiral;
      for (int i = 0; i <= 48; i++) {
        const float a = float(i) / 48.0f * 3.6f * PI;
        const float r = 0.08f + 0.6f * float(i) / 48.0f;
        spiral.push_back(c + ImVec2(std::cos(a) * r * s, std::sin(a) * r * s));
      }
      dl->AddPolyline(spiral.data(), int(spiral.size()), col, 0, t * 1.2f);
      dl->PathArcTo(c, s * 0.82f, -PI * 0.9f, -PI * 0.25f, 12);
      dl->PathStroke(arrow_col, 0, t);
      const ImVec2 tip = c + ImVec2(std::cos(-PI * 0.25f), std::sin(-PI * 0.25f)) * (s * 0.82f);
      dl->AddCircleFilled(tip, t * 1.6f, arrow_col);
      break;
    }
  }
}

static bool in_tab(const SculptPreset &p, int tab)
{
  switch (tab) {
    case 1:
      return p.category == SCULPT_CATEGORY_MOVE;
    case 2:
      return p.category == SCULPT_CATEGORY_SHAPE;
    case 3:
      return p.category == SCULPT_CATEGORY_MINE;
    default:
      return true;
  }
}

/* The key that picks a brush while Sculpt is the tool (its built-in preset). */
static const char *brush_key(SculptBrush brush)
{
  switch (brush) {
    case SculptBrush::Grab:
      return "G";
    case SculptBrush::Push:
      return "P";
    case SculptBrush::Smooth:
      return "S";
    case SculptBrush::Thickness:
      return "T";
    case SculptBrush::Pinch:
      return "I";
    case SculptBrush::Twist:
      return "W";
    case SculptBrush::SurfaceSmooth:
      return "";
  }
  return "";
}

static void falloff_mark(ImDrawList *dl, const SculptPreset &p, ImVec2 corner, float r)
{
  /* Bottom-right: the brush's size and strength at a glance (bigger, darker). */
  const float size = std::clamp(p.radius / 180.0f, 0.25f, 1.0f) * r;
  const int alpha = int(60 + 160 * std::clamp(p.strength, 0.0f, 1.0f));
  dl->AddCircleFilled(corner, size, IM_COL32(0x47, 0x72, 0xb3, alpha), 16);
  dl->AddCircle(corner, size, IM_COL32(0x47, 0x72, 0xb3, 255), 16, 1.0f);
}

void sculpt_shelf_draw(Context &C, Area &area, ImVec2 min, ImVec2 size)
{
  if (size.y <= 0.0f) {
    return;
  }
  ToolSettings &ts = C.wm->tool_settings;
  ShelfState &shelf = area.sculpt_shelf;
  const float unit = ImGui::GetFrameHeight();
  ImGui::PushID("##sculpt_shelf");
  if (!shelf_frame(shelf, {{0, "All"}, {1, "Move"}, {2, "Shape"}, {3, "Mine"}}, min, size)) {
    ImGui::PopID();
    return;
  }

  /* The tiles, in rows, scrolling when they don't fit. */
  const float ts_px = shelf_tile_size(shelf);
  const float th = shelf_tile_height(shelf);
  const float gap = 8.0f;
  const ImVec2 body_min = ImGui::GetCursorScreenPos();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  if (ImGui::BeginChild("##tiles", ImVec2(size.x, min.y + size.y - body_min.y), ImGuiChildFlags_None)) {
    ImDrawList *tdl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos() + ImVec2(gap, 6.0f);
    const int per_row = std::max(1, int((size.x - gap) / (ts_px + gap)));
    const std::string search = shelf_lower(shelf.search);
    std::vector<const SculptPreset *> shown;
    for (const SculptPreset &p : sculpt_presets()) {
      if (in_tab(p, shelf.tab) && (search.empty() || shelf_lower(p.name).find(search) != std::string::npos)) {
        shown.push_back(&p);
      }
    }
    /* Mine (and All) end with a + tile: save the brush as it is now. */
    const bool plus = shelf.tab == 0 || shelf.tab == 3;
    const int count = int(shown.size()) + (plus ? 1 : 0);
    static std::string save_name;
    static std::string menu_preset;
    for (int k = 0; k < count; k++) {
      const ImVec2 at = start + ImVec2(float(k % per_row) * (ts_px + gap), float(k / per_row) * (th + gap));
      ImGui::SetCursorScreenPos(at);
      ImGui::PushID(k);
      const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(ts_px, th));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 a = at, b = at + ImVec2(ts_px, ts_px);
      if (k == int(shown.size())) {
        shelf_plus_tile(tdl, a, b, hovered);
        if (hovered) {
          ImGui::SetTooltip("Save the brush as it is now (%s, radius %.0f px, strength %.2f) to the asset library, "
                            "under Mine",
                            sculpt_brush_name(ts.sculpt_brush), double(ts.sculpt_radius), double(ts.sculpt_strength));
        }
        if (clicked) {
          save_name = ts.sculpt_preset.empty() ? sculpt_brush_name(ts.sculpt_brush) : ts.sculpt_preset;
          ImGui::OpenPopup("##save_preset");
        }
        if (ImGui::BeginPopup("##save_preset")) {
          ImGui::TextUnformatted("Save Sculpt Brush");
          if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
          }
          ImGui::SetNextItemWidth(unit * 9.0f);
          const bool enter = ImGui::InputText("##name", &save_name, ImGuiInputTextFlags_EnterReturnsTrue);
          if (ImGui::Button("Save") || enter) {
            operator_call(C, "sculpt.preset_save", {{"name", save_name}});
            shelf.tab = 3;
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndPopup();
        }
        ImGui::PopID();
        continue;
      }
      const SculptPreset &p = *shown[size_t(k)];
      const bool active = ts.sculpt_preset == p.name;
      shelf_tile_box(tdl, a, b, active, hovered);
      sculpt_brush_icon(tdl, p.brush, (a + b) * 0.5f - ImVec2(0, ts_px * 0.03f), ts_px * 0.42f, COL_INK, COL_INK_BEFORE);
      falloff_mark(tdl, p, b - ImVec2(ts_px * 0.13f, ts_px * 0.13f), ts_px * 0.09f);
      shelf_tile_name(tdl, a, b, p.name, active);
      if (hovered) {
        static const char *const FALLOFFS[] = {"Smooth", "Sphere", "Constant"};
        /* As it is now: the active one as the header has it, another as it was last changed to. */
        float radius = p.radius, strength = p.strength;
        SculptFalloff falloff = p.falloff;
        if (active) {
          radius = ts.sculpt_radius;
          strength = ts.sculpt_strength;
          falloff = ts.sculpt_falloff;
        }
        else if (const auto it = ts.sculpt_changes.find(p.name); it != ts.sculpt_changes.end()) {
          radius = it->second.radius;
          strength = it->second.strength;
          falloff = it->second.falloff;
        }
        ImGui::SetTooltip("%s\n%s brush, radius %.0f px, strength %.2f, %s falloff%s%s", p.name.c_str(),
                          sculpt_brush_name(p.brush), double(radius), double(strength), FALLOFFS[int(falloff)],
                          p.builtin && &sculpt_preset_default(p.brush) == &p ? "\nShortcut: " : "",
                          p.builtin && &sculpt_preset_default(p.brush) == &p ? brush_key(p.brush) : "");
      }
      if (clicked) {
        operator_call(C, "sculpt.preset_load", {{"name", p.name}});
      }
      if (!p.builtin && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        menu_preset = p.name;
        ImGui::OpenPopup("##tile_menu");
      }
      if (ImGui::BeginPopup("##tile_menu")) {
        if (ImGui::MenuItem(("Delete \"" + menu_preset + "\"").c_str())) {
          operator_call(C, "sculpt.preset_remove", {{"name", menu_preset}});
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    if (count == 0) {
      ImGui::SetCursorScreenPos(start);
      ImGui::TextDisabled(shelf.tab == 3 ? "No saved brushes yet" : "No brushes match");
    }
    /* Room for the rows, so the shelf scrolls. */
    const int rows = (count + per_row - 1) / per_row;
    ImGui::SetCursorScreenPos(start + ImVec2(0, float(std::max(rows, 1)) * (th + gap)));
    ImGui::Dummy(ImVec2(1, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::PopID();
}

}  // namespace oanim
