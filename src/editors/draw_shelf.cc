/* The Draw tool's shelf: a strip along the bottom of the viewport while Draw is the tool, as Blender's asset
 * shelf shows brushes, and the same tiles in a popup (Shift Space, or the header's pen button) for anyone
 * who folds it. Each tile is a preset (assets/draw_presets.hh), its picture a sample line made by the engine
 * itself from the preset's settings, so a tile shows how the pen really draws. Clicking a tile picks it;
 * Mine's + tile saves the pen as it is now; right-clicking one puts it back or deletes it. Tabs with nothing
 * in them don't show. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "assets/draw_presets.hh"
#include "data/ink_stroke.hh"
#include "editors/editors.hh"
#include "gpu/tessellate.hh"
#include "imgui_stdlib.h"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

static constexpr ImU32 COL_INK = IM_COL32(0x1e, 0x1e, 0x1e, 255);
static constexpr ImU32 COL_CHANGED = IM_COL32(0x47, 0x72, 0xb3, 255);
/* The sample line: this many document pixels across, in this long. */
static constexpr float SAMPLE_PIXELS = 400.0f;
static constexpr float SAMPLE_SECONDS = 0.7f;
/* Its line drawn this much wider than true, and a document this many pixels tall: a tile is small. */
static constexpr float SAMPLE_WIDEN = 3.0f;
static constexpr float SAMPLE_DOCUMENT_HEIGHT = 1080.0f;

enum { TAB_ALL, TAB_PENS, TAB_ERASERS, TAB_TRIM, TAB_MINE };

void ink_line_draw(ImDrawList *dl, const std::vector<float2> &points, const std::vector<float> &half_widths, Color color)
{
  std::vector<Vertex> vertices;
  tessellate_stroke(points, half_widths, false, color, 1.0f, vertices);
  /* Triangles, a few at a time (a draw list's indices may be 16-bit). The tessellator's colours are
   * premultiplied, ImGui's not. */
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  static constexpr size_t CHUNK = 3 * 10000;
  for (size_t from = 0; from < vertices.size(); from += CHUNK) {
    const size_t count = std::min(CHUNK, vertices.size() - from);
    dl->PrimReserve(int(count), int(count));
    for (size_t i = 0; i < count; i++) {
      const Vertex &v = vertices[from + i];
      const uint32_t a = v.rgba >> 24;
      const auto straight = [a](uint32_t c) { return a > 0 ? std::min<uint32_t>(c * 255 / a, 255) : 0; };
      const ImU32 col = IM_COL32(straight(v.rgba & 0xff), straight(v.rgba >> 8 & 0xff), straight(v.rgba >> 16 & 0xff), a);
      dl->PrimWriteIdx(ImDrawIdx(dl->_VtxCurrentIdx));
      dl->PrimWriteVtx(ImVec2(v.x, v.y), uv, col);
    }
  }
}

/* What a preset's tile shows: its settings as they are now (the active one as the header has them, another
 * as it was last changed to). */
static void preset_now(const ToolSettings &ts, const DrawPreset &p, InkSettings *r_ink, float *r_line_width)
{
  *r_ink = p.ink;
  *r_line_width = p.line_width;
  if (p.mode == DrawMode::Pen) {
    return;
  }
  if (ts.draw_preset == p.name) {
    *r_ink = ts.ink;
    *r_line_width = draw_mode_paints(p.mode) ? ts.fill_size : ts.style.line_width;
  }
  else if (const auto it = ts.draw_changes.find(p.name); it != ts.draw_changes.end()) {
    *r_ink = it->second.ink;
    *r_line_width = it->second.line_width;
  }
}

/* The sample line a pen with these settings makes: an S, pressing into its middle, leaning as it goes. Made
 * again only when the settings change. */
struct SampleLine {
  InkSettings ink;
  float line_width = -1.0f;
  std::vector<InkPoint> points;
};

static const std::vector<InkPoint> &sample_line(const std::string &name, const InkSettings &ink, float line_width)
{
  static std::map<std::string, SampleLine> lines;
  SampleLine &line = lines[name];
  if (line.line_width == line_width && line.ink == ink) {
    return line.points;
  }
  line.ink = ink;
  line.line_width = line_width;
  InkStroke stroke;
  stroke.begin(ink, {1.0f, line_width * SAMPLE_DOCUMENT_HEIGHT * 0.25f, 1.0f});
  static constexpr int STEPS = 84;
  for (int i = 0; i <= STEPS; i++) {
    const float u = float(i) / float(STEPS);
    const float y = std::sin(u * 2.0f * PI) * SAMPLE_PIXELS * 0.2f;
    stroke.add({{u * SAMPLE_PIXELS, y}, 0.2f + 0.8f * std::sin(u * PI), {u * 50.0f, 0.0f}, double(u * SAMPLE_SECONDS)});
  }
  stroke.finish();
  std::vector<InkPoint> points = stroke.points();
  ink_taper(points, ink, stroke.view());
  /* A tile has a few dozen pixels for it. */
  line.points.clear();
  for (const InkPoint &p : points) {
    if (line.points.empty() || distance(p.pos, line.points.back().pos) >= 6.0f) {
      line.points.push_back(p);
    }
  }
  if (distance(points.back().pos, line.points.back().pos) > 0.0f) {
    line.points.push_back(points.back());
  }
  return line.points;
}

static void tile_picture(ImDrawList *dl, const ToolSettings &ts, const DrawPreset &p, ImVec2 a, ImVec2 b)
{
  const float size = b.x - a.x;
  const ImVec2 c = (a + b) * 0.5f;
  if (p.mode == DrawMode::Pen) {
    /* A curve with its points and handles. */
    const float s = size * 0.32f, t = std::max(1.0f, size * 0.03f);
    const ImVec2 p0 = c + ImVec2(-s, s * 0.6f), p1 = c + ImVec2(s, -s * 0.6f);
    const ImVec2 h0 = c + ImVec2(-s * 0.3f, -s * 0.9f), h1 = c + ImVec2(s * 0.3f, s * 0.9f);
    dl->AddBezierCubic(p0, h0, h1, p1, COL_INK, t, 24);
    dl->AddLine(p0, h0, IM_COL32(0xa8, 0xa8, 0xa8, 255), 1.0f);
    dl->AddLine(p1, h1, IM_COL32(0xa8, 0xa8, 0xa8, 255), 1.0f);
    const float r = size * 0.035f;
    dl->AddCircleFilled(h0, r, IM_COL32(0x47, 0x72, 0xb3, 255));
    dl->AddCircleFilled(h1, r, IM_COL32(0x47, 0x72, 0xb3, 255));
    dl->AddRectFilled(p0 - ImVec2(r, r), p0 + ImVec2(r, r), COL_INK);
    dl->AddRectFilled(p1 - ImVec2(r, r), p1 + ImVec2(r, r), COL_INK);
    return;
  }
  InkSettings ink;
  float line_width = 0.0f;
  preset_now(ts, p, &ink, &line_width);
  const float scale = size * 0.8f / SAMPLE_PIXELS;
  const float half = line_width * SAMPLE_DOCUMENT_HEIGHT * 0.25f * scale * SAMPLE_WIDEN;
  std::vector<float2> points;
  std::vector<float> widths;
  for (const InkPoint &point : sample_line(p.name, ink, line_width)) {
    const ImVec2 at = c + ImVec2(point.pos.x - SAMPLE_PIXELS * 0.5f, -point.pos.y) * scale;
    points.push_back({at.x, at.y});
    widths.push_back(std::min(point.radius * half, size * 0.1f));
  }
  if (points.size() < 2) {
    return;
  }
  if (p.mode == DrawMode::FillEraser) {
    /* A patch of fill, with the eraser's path cut out of it. */
    dl->AddRectFilled(a + ImVec2(size * 0.1f, size * 0.2f), b - ImVec2(size * 0.1f, size * 0.2f), IM_COL32(0xd8, 0x9a, 0x5c, 255), 3.0f);
    ink_line_draw(dl, points, widths, Color{0.95f, 0.95f, 0.95f, 1.0f});
    return;
  }
  ink_line_draw(dl, points, widths, p.mode == DrawMode::FillPen ? Color{0.85f, 0.6f, 0.36f, 1.0f} : Color{0.12f, 0.12f, 0.12f, 1.0f});
}

static bool in_tab(const DrawPreset &p, int tab)
{
  switch (tab) {
    case TAB_PENS:
      return p.category == DRAW_CATEGORY_PENS;
    case TAB_ERASERS:
      return p.category == DRAW_CATEGORY_ERASERS;
    case TAB_TRIM:
      return p.category == DRAW_CATEGORY_TRIM;
    case TAB_MINE:
      return p.category == DRAW_CATEGORY_MINE;
    default:
      return true;
  }
}

/* The tabs with something in them (Mine has its + tile), as Blender hides empty catalogs. */
static std::vector<ShelfTab> shown_tabs()
{
  std::vector<ShelfTab> tabs = {{TAB_ALL, "All"}, {TAB_PENS, "Pens"}};
  const auto any = [](const char *category) {
    const std::vector<DrawPreset> &all = draw_presets();
    return std::any_of(all.begin(), all.end(), [&](const DrawPreset &p) { return p.category == category; });
  };
  if (any(DRAW_CATEGORY_ERASERS)) {
    tabs.push_back({TAB_ERASERS, "Erasers"});
  }
  if (any(DRAW_CATEGORY_TRIM)) {
    tabs.push_back({TAB_TRIM, "Trim"});
  }
  tabs.push_back({TAB_MINE, "Mine"});
  return tabs;
}

static std::string tile_tooltip(const ToolSettings &ts, const DrawPreset &p)
{
  if (p.mode == DrawMode::Pen) {
    return p.name + "\nClick to add points along a curve, drag to place each";
  }
  if (p.mode == DrawMode::FillEraser) {
    return p.name + "\nCuts the brushed area out of fills; lines stay";
  }
  InkSettings ink;
  float line_width = 0.0f;
  preset_now(ts, p, &ink, &line_width);
  std::string text = p.name + "\n";
  text += p.mode == DrawMode::FillPen ? "Paints a fill, merging with one of the same colour it overlaps. " : "";
  text += ink.stabilizer == InkStabilizer::Off     ? "No smoothing"
          : ink.stabilizer == InkStabilizer::Smart ? "Smart smoothing"
                                                   : "Pulled String";
  text += ink.use_pressure ? ", width from pressure" : "";
  text += ink.speed_width > 0.0f ? ", width from speed" : "";
  text += ink.tilt_width > 0.0f ? ", width from tilt" : "";
  text += ink.taper_start > 0.0f || ink.taper_end > 0.0f ? ", tapered" : "";
  if (!(ink == p.ink) || line_width != p.line_width) {
    text += "\nChanged from its saved settings (right-click to put it back)";
  }
  return text;
}

/* The tiles, in rows of `width`, scrolling when they don't fit. True when one was picked. */
static bool preset_tiles(Context &C, ShelfState &shelf, float width)
{
  ToolSettings &ts = C.wm->tool_settings;
  bool picked = false;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float size = shelf_tile_size(shelf), th = shelf_tile_height(shelf), gap = 8.0f;
  const ImVec2 start = ImGui::GetCursorScreenPos() + ImVec2(gap, 6.0f);
  const int per_row = std::max(1, int((width - gap) / (size + gap)));
  const std::string search = shelf_lower(shelf.search);
  std::vector<const DrawPreset *> shown;
  for (const DrawPreset &p : draw_presets()) {
    if (in_tab(p, shelf.tab) && (search.empty() || shelf_lower(p.name).find(search) != std::string::npos)) {
      shown.push_back(&p);
    }
  }
  /* Mine (and All) end with a + tile: save the pen as it is now. */
  const bool plus = shelf.tab == TAB_ALL || shelf.tab == TAB_MINE;
  const int count = int(shown.size()) + (plus ? 1 : 0);
  static std::string save_name;
  static std::string menu_preset;
  for (int k = 0; k < count; k++) {
    const ImVec2 at = start + ImVec2(float(k % per_row) * (size + gap), float(k / per_row) * (th + gap));
    ImGui::SetCursorScreenPos(at);
    ImGui::PushID(k);
    const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(size, th));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a = at, b = at + ImVec2(size, size);
    if (k == int(shown.size())) {
      shelf_plus_tile(dl, a, b, hovered);
      if (hovered) {
        ImGui::SetTooltip("Save the pen as it is now to the asset library, under Mine");
      }
      if (clicked) {
        save_name = ts.draw_preset;
        ImGui::OpenPopup("##save_preset");
      }
      if (ImGui::BeginPopup("##save_preset")) {
        ImGui::TextUnformatted("Save Pen");
        if (ImGui::IsWindowAppearing()) {
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 9.0f);
        const bool enter = ImGui::InputText("##name", &save_name, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
          operator_call(C, "draw.preset_save", {{"name", save_name}});
          shelf.tab = TAB_MINE;
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
    const DrawPreset &p = *shown[size_t(k)];
    const bool active = ts.draw_mode == p.mode && (p.mode == DrawMode::Pen || ts.draw_preset == p.name);
    shelf_tile_box(dl, a, b, active, hovered);
    tile_picture(dl, ts, p, a, b);
    shelf_tile_name(dl, a, b, p.name, active);
    InkSettings ink;
    float line_width = 0.0f;
    preset_now(ts, p, &ink, &line_width);
    const bool changed = p.mode != DrawMode::Pen && (!(ink == p.ink) || line_width != p.line_width);
    if (changed) {
      dl->AddCircleFilled(ImVec2(b.x - size * 0.1f, a.y + size * 0.1f), size * 0.045f, COL_CHANGED);
    }
    if (hovered) {
      ImGui::SetTooltip("%s", tile_tooltip(ts, p).c_str());
    }
    if (clicked) {
      operator_call(C, "draw.preset_load", {{"name", p.name}});
      picked = true;
    }
    if ((changed || !p.builtin) && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      menu_preset = p.name;
      ImGui::OpenPopup("##tile_menu");
    }
    if (ImGui::BeginPopup("##tile_menu")) {
      const DrawPreset *menu = draw_preset_find(menu_preset);
      if (ImGui::MenuItem(("Put Back \"" + menu_preset + "\"").c_str())) {
        operator_call(C, "draw.preset_reset", {{"name", menu_preset}});
      }
      if (menu && !menu->builtin && ImGui::MenuItem(("Delete \"" + menu_preset + "\"").c_str())) {
        operator_call(C, "draw.preset_remove", {{"name", menu_preset}});
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (count == 0) {
    ImGui::SetCursorScreenPos(start);
    ImGui::TextDisabled("No pens match");
  }
  /* Room for the rows, so the shelf scrolls. */
  const int rows = (count + per_row - 1) / per_row;
  ImGui::SetCursorScreenPos(start + ImVec2(0, float(std::max(rows, 1)) * (th + gap)));
  ImGui::Dummy(ImVec2(1, 1));
  return picked;
}

float draw_shelf_height(const Context &C, const Area &area)
{
  return C.wm->tool_settings.tool == Tool::Draw ? shelf_height(area.draw_shelf) : 0.0f;
}

void draw_shelf_draw(Context &C, Area &area, ImVec2 min, ImVec2 size)
{
  if (size.y <= 0.0f) {
    return;
  }
  ImGui::PushID("##draw_shelf");
  if (shelf_frame(area.draw_shelf, shown_tabs(), min, size)) {
    const ImVec2 body = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    if (ImGui::BeginChild("##tiles", ImVec2(size.x, min.y + size.y - body.y), ImGuiChildFlags_None)) {
      preset_tiles(C, area.draw_shelf, size.x);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  }
  ImGui::PopID();
}

void draw_presets_popup(Context &C)
{
  static ShelfState popup;
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFrameHeight() * 24.0f, 0.0f));
  if (!ImGui::BeginPopup("##draw_presets")) {
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  shelf_tabs(popup, shown_tabs());
  const float rows = std::min(3.0f, std::ceil(float(draw_presets().size() + 1) /
                                              std::max(std::floor(ImGui::GetContentRegionAvail().x /
                                                                  (shelf_tile_size(popup) + 8.0f)),
                                                       1.0f)));
  const float height = rows * (shelf_tile_height(popup) + 8.0f) + 8.0f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x2b, 0x2b, 0x2b, 255));
  if (ImGui::BeginChild("##tiles", ImVec2(0.0f, height), ImGuiChildFlags_None)) {
    if (preset_tiles(C, popup, ImGui::GetContentRegionAvail().x)) {
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::EndPopup();
}

}  // namespace oanim
