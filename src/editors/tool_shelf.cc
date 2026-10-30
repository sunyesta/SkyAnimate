#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include "editors/tool_shelf.hh"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "imgui_stdlib.h"

namespace oanim {

static constexpr ImU32 COL_SHELF = IM_COL32(0x2b, 0x2b, 0x2b, 255);
static constexpr ImU32 COL_SHELF_HEADER = IM_COL32(0x23, 0x23, 0x23, 255);
static constexpr ImU32 COL_TILE = IM_COL32(0xf2, 0xf2, 0xf2, 255);
static constexpr ImU32 COL_TILE_ACTIVE = IM_COL32(0x47, 0x72, 0xb3, 255);

static float header_height()
{
  return std::round(ImGui::GetFrameHeight() * 1.3f);
}

float shelf_tile_size(const ShelfState &shelf)
{
  return shelf.tile > 0.0f ? shelf.tile : std::round(ImGui::GetFrameHeight() * 2.6f);
}

float shelf_tile_height(const ShelfState &shelf)
{
  return shelf_tile_size(shelf) + ImGui::GetFontSize() + 6.0f;
}

float shelf_height(const ShelfState &shelf)
{
  if (!shelf.open) {
    return header_height();
  }
  const float one_row = header_height() + shelf_tile_height(shelf) + 12.0f;
  return shelf.height > 0.0f ? std::max(shelf.height, one_row) : one_row;
}

std::string shelf_lower(std::string s)
{
  for (char &c : s) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

static void tab_buttons(ShelfState &shelf, const std::vector<ShelfTab> &tabs, float first_gap)
{
  for (size_t i = 0; i < tabs.size(); i++) {
    ImGui::SameLine(0, i == 0 ? first_gap : 2.0f);
    const bool on = shelf.tab == tabs[i].tab;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(tabs[i].name)) {
      shelf.tab = tabs[i].tab;
      shelf.open = true;
    }
    if (on) {
      ImGui::PopStyleColor();
    }
  }
}

/* The tiles' size and the search. */
static void size_and_search(ShelfState &shelf)
{
  const float unit = ImGui::GetFrameHeight();
  ImGui::SetNextItemWidth(unit * 3.2f);
  static const char *const SIZES[] = {"Small", "Medium", "Large"};
  const float base = std::round(unit * 2.6f);
  int size_index = shelf.tile <= 0.0f ? 1 : shelf.tile < base ? 0 : shelf.tile > base ? 2 : 1;
  if (ImGui::Combo("##tile_size", &size_index, SIZES, 3)) {
    shelf.tile = size_index == 1 ? 0.0f : size_index == 0 ? std::round(base * 0.7f) : std::round(base * 1.45f);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How big the brush pictures are");
  }
  ImGui::SameLine(0, 6.0f);
  ImGui::SetNextItemWidth(unit * 7.0f);
  ImGui::InputTextWithHint("##search", "Search", &shelf.search);
}

bool shelf_frame(ShelfState &shelf, const std::vector<ShelfTab> &tabs, ImVec2 min, ImVec2 size)
{
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 max = min + size;
  const float hh = header_height();
  const float unit = ImGui::GetFrameHeight();
  dl->AddRectFilled(min, max, COL_SHELF);
  dl->AddRectFilled(min, ImVec2(max.x, min.y + hh), COL_SHELF_HEADER);
  dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(0x18, 0x18, 0x18, 255), 1.0f);
  ImGui::PushID("##shelf");

  /* The top edge sizes it (open). */
  if (shelf.open) {
    ImGui::SetCursorScreenPos(min - ImVec2(0, 3));
    ImGui::InvisibleButton("##edge", ImVec2(size.x, 6));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
      shelf.height = std::max(size.y - ImGui::GetIO().MouseDelta.y, hh + shelf_tile_height(shelf) + 12.0f);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      shelf.height = 0.0f;
    }
  }

  /* Header: the fold arrow, the tabs; on the right the tiles' size and the search. */
  ImGui::SetCursorScreenPos(ImVec2(min.x + 6.0f, min.y + (hh - unit) * 0.5f));
  if (ImGui::ArrowButton("##fold", shelf.open ? ImGuiDir_Down : ImGuiDir_Up)) {
    shelf.open = !shelf.open;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(shelf.open ? "Fold the brush shelf" : "Open the brush shelf");
  }
  tab_buttons(shelf, tabs, 10.0f);
  ImGui::SetCursorScreenPos(ImVec2(max.x - unit * (7.0f + 3.2f) - 14.0f, min.y + (hh - unit) * 0.5f));
  size_and_search(shelf);
  ImGui::PopID();
  /* The tiles start under the header. */
  ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + hh));
  return shelf.open;
}

void shelf_tabs(ShelfState &shelf, const std::vector<ShelfTab> &tabs)
{
  ImGui::PushID("##shelf");
  ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
  tab_buttons(shelf, tabs, 0.0f);
  ImGui::SameLine(0, 12.0f);
  size_and_search(shelf);
  ImGui::PopID();
}

void shelf_tile_box(ImDrawList *dl, ImVec2 a, ImVec2 b, bool active, bool hovered)
{
  dl->AddRectFilled(a, b, COL_TILE, 4.0f);
  if (active || hovered) {
    dl->AddRect(a - ImVec2(1, 1), b + ImVec2(1, 1), active ? COL_TILE_ACTIVE : IM_COL32(0x90, 0x90, 0x90, 255), 5.0f, 0,
                active ? 2.5f : 1.5f);
  }
}

void shelf_tile_name(ImDrawList *dl, ImVec2 a, ImVec2 b, const std::string &name, bool active)
{
  const float width = b.x - a.x;
  std::string shown = name;
  while (shown.size() > 1 && ImGui::CalcTextSize(shown.c_str()).x > width) {
    shown.pop_back();
  }
  if (shown.size() < name.size() && shown.size() > 2) {
    shown.replace(shown.size() - 2, 2, "..");
  }
  dl->AddText(ImVec2(a.x + (width - ImGui::CalcTextSize(shown.c_str()).x) * 0.5f, b.y + 3.0f),
              active ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xc8, 0xc8, 0xc8, 255), shown.c_str());
}

void shelf_plus_tile(ImDrawList *dl, ImVec2 a, ImVec2 b, bool hovered)
{
  dl->AddRectFilled(a, b, hovered ? IM_COL32(0x4a, 0x4a, 0x4a, 255) : IM_COL32(0x3a, 0x3a, 0x3a, 255), 4.0f);
  dl->AddRect(a, b, IM_COL32(0x60, 0x60, 0x60, 255), 4.0f, 0, 1.0f);
  const ImVec2 c = (a + b) * 0.5f;
  const float r = (b.x - a.x) * 0.22f;
  dl->AddLine(c - ImVec2(r, 0), c + ImVec2(r, 0), IM_COL32(0xe6, 0xe6, 0xe6, 255), 2.0f);
  dl->AddLine(c - ImVec2(0, r), c + ImVec2(0, r), IM_COL32(0xe6, 0xe6, 0xe6, 255), 2.0f);
  const char *label = "Save";
  dl->AddText(ImVec2(a.x + (b.x - a.x - ImGui::CalcTextSize(label).x) * 0.5f, b.y + 3.0f),
              IM_COL32(0xc8, 0xc8, 0xc8, 255), label);
}

}  // namespace oanim
