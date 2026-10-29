/* The Sculpt tool's brush shelf: a strip along the bottom of the viewport while Sculpt is the tool,
 * as Blender's asset shelf shows brushes. Tabs (All, Move, Shape, Mine), a search field and the
 * tiles' size along its top; under them a tile per preset, its picture a line before and after the
 * brush, its name beneath. Clicking a tile picks the preset; Mine's + tile saves the brush as it is
 * now, and right-clicking a saved one deletes it. Dragging the top edge sizes the shelf; the arrow
 * folds it to its tabs. The viewport's view sits above it, so it never covers the drawing. */

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

static const char *const SHELF_TABS[] = {"All", "Move", "Shape", "Mine"};
static constexpr ImU32 COL_SHELF = IM_COL32(0x2b, 0x2b, 0x2b, 255);
static constexpr ImU32 COL_SHELF_HEADER = IM_COL32(0x23, 0x23, 0x23, 255);
static constexpr ImU32 COL_TILE = IM_COL32(0xf2, 0xf2, 0xf2, 255);
static constexpr ImU32 COL_TILE_ACTIVE = IM_COL32(0x47, 0x72, 0xb3, 255);
static constexpr ImU32 COL_INK = IM_COL32(0x1e, 0x1e, 0x1e, 255);
static constexpr ImU32 COL_INK_BEFORE = IM_COL32(0xa8, 0xa8, 0xa8, 255);
static constexpr ImU32 COL_ARROW = IM_COL32(0xe0, 0xa0, 0x20, 255);

static float header_height()
{
  return std::round(ImGui::GetFrameHeight() * 1.3f);
}

static float tile_size(const Area &area)
{
  return area.sculpt_shelf_tile > 0.0f ? area.sculpt_shelf_tile : std::round(ImGui::GetFrameHeight() * 2.6f);
}

/* A tile and the name under it. */
static float tile_height(const Area &area)
{
  return tile_size(area) + ImGui::GetFontSize() + 6.0f;
}

float sculpt_shelf_height(const Context &C, const Area &area)
{
  if (C.wm->tool_settings.tool != Tool::Sculpt) {
    return 0.0f;
  }
  if (!area.sculpt_shelf_open) {
    return header_height();
  }
  const float one_row = header_height() + tile_height(area) + 12.0f;
  return area.sculpt_shelf_height > 0.0f ? std::max(area.sculpt_shelf_height, one_row) : one_row;
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

static std::string lower(std::string s)
{
  for (char &c : s) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
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
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 max = min + size;
  const float hh = header_height();
  const float unit = ImGui::GetFrameHeight();
  dl->AddRectFilled(min, max, COL_SHELF);
  dl->AddRectFilled(min, ImVec2(max.x, min.y + hh), COL_SHELF_HEADER);
  dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(0x18, 0x18, 0x18, 255), 1.0f);
  ImGui::PushID("##sculpt_shelf");

  /* The top edge sizes it (open). */
  if (area.sculpt_shelf_open) {
    ImGui::SetCursorScreenPos(min - ImVec2(0, 3));
    ImGui::InvisibleButton("##edge", ImVec2(size.x, 6));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
      area.sculpt_shelf_height = std::max(size.y - ImGui::GetIO().MouseDelta.y, hh + tile_height(area) + 12.0f);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      area.sculpt_shelf_height = 0.0f;
    }
  }

  /* Header: the fold arrow, the tabs; on the right the tiles' size and the search. */
  ImGui::SetCursorScreenPos(ImVec2(min.x + 6.0f, min.y + (hh - unit) * 0.5f));
  if (ImGui::ArrowButton("##fold", area.sculpt_shelf_open ? ImGuiDir_Down : ImGuiDir_Up)) {
    area.sculpt_shelf_open = !area.sculpt_shelf_open;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(area.sculpt_shelf_open ? "Fold the brush shelf" : "Open the brush shelf");
  }
  for (int i = 0; i < 4; i++) {
    ImGui::SameLine(0, i == 0 ? 10.0f : 2.0f);
    const bool on = area.sculpt_shelf_tab == i;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(SHELF_TABS[i])) {
      area.sculpt_shelf_tab = i;
      area.sculpt_shelf_open = true;
    }
    if (on) {
      ImGui::PopStyleColor();
    }
  }
  const float search_w = unit * 7.0f, size_w = unit * 3.2f;
  ImGui::SetCursorScreenPos(ImVec2(max.x - search_w - size_w - 14.0f, min.y + (hh - unit) * 0.5f));
  ImGui::SetNextItemWidth(size_w);
  static const char *const SIZES[] = {"Small", "Medium", "Large"};
  const float base = std::round(unit * 2.6f);
  int size_index = area.sculpt_shelf_tile <= 0.0f ? 1 : area.sculpt_shelf_tile < base ? 0 : area.sculpt_shelf_tile > base ? 2 : 1;
  if (ImGui::Combo("##tile_size", &size_index, SIZES, 3)) {
    area.sculpt_shelf_tile = size_index == 1 ? 0.0f : size_index == 0 ? std::round(base * 0.7f) : std::round(base * 1.45f);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How big the brush pictures are");
  }
  ImGui::SameLine(0, 6.0f);
  ImGui::SetNextItemWidth(search_w);
  ImGui::InputTextWithHint("##search", "Search", &area.sculpt_shelf_search);

  if (!area.sculpt_shelf_open) {
    ImGui::PopID();
    return;
  }

  /* The tiles, in rows, scrolling when they don't fit. */
  const float ts_px = tile_size(area);
  const float th = tile_height(area);
  const float gap = 8.0f;
  const ImVec2 body_min(min.x, min.y + hh);
  ImGui::SetCursorScreenPos(body_min);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  if (ImGui::BeginChild("##tiles", ImVec2(size.x, size.y - hh), ImGuiChildFlags_None)) {
    ImDrawList *tdl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos() + ImVec2(gap, 6.0f);
    const int per_row = std::max(1, int((size.x - gap) / (ts_px + gap)));
    const std::string search = lower(area.sculpt_shelf_search);
    std::vector<const SculptPreset *> shown;
    for (const SculptPreset &p : sculpt_presets()) {
      if (in_tab(p, area.sculpt_shelf_tab) && (search.empty() || lower(p.name).find(search) != std::string::npos)) {
        shown.push_back(&p);
      }
    }
    /* Mine (and All) end with a + tile: save the brush as it is now. */
    const bool plus = area.sculpt_shelf_tab == 0 || area.sculpt_shelf_tab == 3;
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
        /* The + tile. */
        tdl->AddRectFilled(a, b, hovered ? IM_COL32(0x4a, 0x4a, 0x4a, 255) : IM_COL32(0x3a, 0x3a, 0x3a, 255), 4.0f);
        tdl->AddRect(a, b, IM_COL32(0x60, 0x60, 0x60, 255), 4.0f, 0, 1.0f);
        const ImVec2 c = (a + b) * 0.5f;
        const float r = ts_px * 0.22f;
        tdl->AddLine(c - ImVec2(r, 0), c + ImVec2(r, 0), IM_COL32(0xe6, 0xe6, 0xe6, 255), 2.0f);
        tdl->AddLine(c - ImVec2(0, r), c + ImVec2(0, r), IM_COL32(0xe6, 0xe6, 0xe6, 255), 2.0f);
        const char *label = "Save";
        tdl->AddText(ImVec2(a.x + (ts_px - ImGui::CalcTextSize(label).x) * 0.5f, b.y + 3.0f),
                     IM_COL32(0xc8, 0xc8, 0xc8, 255), label);
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
            area.sculpt_shelf_tab = 3;
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
      tdl->AddRectFilled(a, b, COL_TILE, 4.0f);
      if (active || hovered) {
        tdl->AddRect(a - ImVec2(1, 1), b + ImVec2(1, 1), active ? COL_TILE_ACTIVE : IM_COL32(0x90, 0x90, 0x90, 255),
                     5.0f, 0, active ? 2.5f : 1.5f);
      }
      sculpt_brush_icon(tdl, p.brush, (a + b) * 0.5f - ImVec2(0, ts_px * 0.03f), ts_px * 0.42f, COL_INK, COL_INK_BEFORE);
      falloff_mark(tdl, p, b - ImVec2(ts_px * 0.13f, ts_px * 0.13f), ts_px * 0.09f);
      /* Its name, cut to the tile's width. */
      std::string name = p.name;
      while (name.size() > 1 && ImGui::CalcTextSize(name.c_str()).x > ts_px) {
        name.pop_back();
      }
      if (name.size() < p.name.size() && name.size() > 2) {
        name.replace(name.size() - 2, 2, "..");
      }
      tdl->AddText(ImVec2(a.x + (ts_px - ImGui::CalcTextSize(name.c_str()).x) * 0.5f, b.y + 3.0f),
                   active ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xc8, 0xc8, 0xc8, 255), name.c_str());
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
      ImGui::TextDisabled(area.sculpt_shelf_tab == 3 ? "No saved brushes yet" : "No brushes match");
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
