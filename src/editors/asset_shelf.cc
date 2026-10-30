/* The Asset Shelf: a view of its own (as Properties or the Timeline) showing what fits what you are
 * working on (Blender's asset shelf): for a layer in a switch, that switch's children as thumbnails,
 * the switches it sits in as rows above; for a bone, the switches its counterparts are in and those it
 * drives through smart actions. A click picks and keys (Alt: the whole hold); hovering previews in the
 * viewport, nothing keyed; dragging across the tiles scrubs and picks where it lets go (Esc cancels);
 * arrows preview, Enter picks, Ctrl F searches, F2 renames. Pin keeps the rows; All Switches lists every
 * switch. A tile's right-click menu has Duplicate, Duplicate with Keys, Rename, Set as Default, Replace
 * Keys With, Delete, Show in Layers Editor and Pin; the + tile adds an empty drawing. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "anim/bone_family.hh"
#include "anim/draw_order.hh"
#include "anim/switch.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "editors/editors.hh"
#include "editors/switch_thumbnails.hh"
#include "editors/ui.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

namespace {

/* The tile whose preview the shelf set (so it can end it), the tile a drag started on, and the tile the
 * keys move through. */
struct ShelfState {
  bool previewing = false;
  bool dragging = false;
  uint64_t drag_switch = 0;
  uint64_t focus_switch = 0, focus_child = 0;
  /* The tile the right-click menu is for. */
  uint64_t menu_switch = 0, menu_child = 0;
  uint64_t renaming = 0;
  bool focus_rename = false;
  bool rename_here = false;
  std::string rename_text;
  double hover_since = 0.0;
  uint64_t hover_child = 0;
};
ShelfState g_shelf;

bool matches(const std::string &name, const std::string &search)
{
  if (search.empty()) {
    return true;
  }
  const auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
  };
  return lower(name).find(lower(search)) != std::string::npos;
}

void all_switches(std::vector<Layer> &layers, std::vector<uint64_t> &r_ids)
{
  for (Layer &l : layers) {
    if (l.is_switch()) {
      r_ids.push_back(l.id);
    }
    all_switches(l.children, r_ids);
  }
}

void add_once(std::vector<uint64_t> &ids, uint64_t id)
{
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
    ids.push_back(id);
  }
}

/* The switches that fit what is being worked on, outermost first. */
std::vector<uint64_t> context_switches(Document &doc)
{
  std::vector<uint64_t> rows;
  Layer *active = document_active_layer(doc);
  if (!active) {
    return rows;
  }
  for (const Layer *l = active; l; l = layer_parent(doc, l->id)) {
    if (l->is_switch()) {
      rows.insert(rows.begin(), l->id);
    }
  }
  const Bone *bone = active->is_bone_layer() ? bone_find(*active, active->active_bone) : nullptr;
  if (bone && bone->selected) {
    std::vector<uint64_t> all;
    all_switches(doc.layers, all);
    for (const uint64_t id : all) {
      Layer &sw = *layer_find(doc, id);
      for (Layer &child : sw.children) {
        Layer *armature = switch_child_armature(child);
        if (armature && armature->id != active->id && bone_counterpart(doc, active->id, bone->id, *armature)) {
          add_once(rows, id);
        }
      }
      /* Driven by its smart action: the switch keyed in the action named after the bone. */
      const auto it = sw.switch_keys.actions.find(bone->name);
      if (it != sw.switch_keys.actions.end() && !it->second.empty() && layer_inside(doc, id, active->id)) {
        add_once(rows, id);
      }
    }
  }
  return rows;
}

void pick(Context &C, const Layer &sw, const std::string &child, bool hold)
{
  operator_call(C, "layer.switch_set", {{"layer", std::to_string(sw.id)}, {"child", child}, {"hold", hold ? "1" : "0"}});
}

void tile_menu(Context &C, Area &area, uint64_t sw_id, uint64_t child_id)
{
  Document &doc = *C.document;
  const Layer *sw = layer_find(doc, sw_id);
  const Layer *child = layer_find(doc, child_id);
  if (!sw || !child) {
    return;
  }
  const std::string name = child->name;
  ImGui::TextDisabled("%s", name.c_str());
  ImGui::Separator();
  if (ui_menu_item("Duplicate")) {
    operator_call(C, "layer.switch_duplicate", {{"child", std::to_string(child_id)}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("A copy to change into another drawing, without its timeline keys, picked here.\n"
                      "The ViewportActionPanel has Start From (Setup or This Frame)");
  }
  if (ui_menu_item("Duplicate with Keys")) {
    operator_call(C, "layer.switch_duplicate", {{"child", std::to_string(child_id)}, {"keys", "1"}});
  }
  if (ui_menu_item("Rename", "F2")) {
    g_shelf.renaming = child_id;
    g_shelf.rename_text = name;
    g_shelf.focus_rename = true;
  }
  const bool is_default = switch_default_child(doc, *sw) == child_id;
  if (ui_menu_item("Set as Default", nullptr, is_default)) {
    operator_call(C, "layer.switch_set", {{"layer", std::to_string(sw_id)}, {"child", name}, {"default", "1"}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Shown on frame 0, and wherever no key or smart action picks another");
  }
  if (ui_submenu_begin("Aliases")) {
    /* The other names it answers to in a linked switch, separated by commas. */
    static std::string text;
    static uint64_t text_for = 0;
    if (text_for != child_id) {
      text.clear();
      for (const std::string &a : child->switch_aliases) {
        text += (text.empty() ? "" : ", ") + a;
      }
      text_for = child_id;
    }
    ImGui::TextDisabled("Other names it answers to (O, Oh):");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::InputText("##aliases", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
      operator_call(C, "layer.switch_aliases_set", {{"layer", std::to_string(child_id)}, {"aliases", text}});
      text_for = 0;
      ImGui::CloseCurrentPopup();
    }
    ui_submenu_end();
  }
  if (ui_menu_item("Rename Here Only")) {
    g_shelf.renaming = child_id;
    g_shelf.rename_text = name;
    g_shelf.focus_rename = true;
    g_shelf.rename_here = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("In a linked switch, renaming renames its partners too; this renames only this one");
  }
  if (ui_submenu_begin("Replace Keys With")) {
    for (const Layer &other : sw->children) {
      if (other.id != child_id && ui_menu_item(other.name.c_str())) {
        operator_call(C, "layer.switch_replace_keys",
                      {{"layer", std::to_string(sw_id)}, {"from", std::to_string(child_id)}, {"child", other.name}});
        break;
      }
    }
    if (ui_menu_item("None")) {
      operator_call(C, "layer.switch_replace_keys",
                    {{"layer", std::to_string(sw_id)}, {"from", std::to_string(child_id)}, {"child", ""}});
    }
    ui_submenu_end();
  }
  ImGui::Separator();
  if (ui_menu_item("Show in Layers Editor")) {
    doc.active_layer = child_id;
    if (C.wm) {
      C.wm->layers_show_active = child_id;
      C.wm->layers_show_active_serial++;
    }
  }
  if (ui_menu_item("Pin", nullptr, area.shelf_pinned)) {
    area.shelf_pinned = !area.shelf_pinned;
    area.shelf_pins = {sw_id};
  }
  ImGui::Separator();
  const int used = switch_keys_showing(doc, *sw, child_id);
  const std::string del = used > 0 ? "Delete (used by " + std::to_string(used) + (used == 1 ? " key)" : " keys)") : "Delete";
  if (ui_menu_item(del.c_str())) {
    doc.active_layer = child_id;
    operator_call(C, "layer.delete");
  }
  if (used > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Its keys then show the default and say (missing): Replace Keys With points them elsewhere");
  }
}

}  // namespace

void asset_shelf_draw(Area &area, Context &C)
{
  Document &doc = *C.document;
  const ImGuiIO &io = ImGui::GetIO();
  area_editor_type_menu(area);

  /* Which rows: pinned, every switch, or what fits the work. */
  std::vector<uint64_t> rows;
  if (area.shelf_tab == 1) {
    all_switches(doc.layers, rows);
  }
  else if (area.shelf_pinned) {
    rows = area.shelf_pins;
  }
  else {
    rows = context_switches(doc);
    if (rows.empty()) {
      /* Nothing fits now: what fitted last. */
      rows = area.shelf_last;
    }
    else {
      area.shelf_last = rows;
    }
  }
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [&](uint64_t id) {
                              const Layer *l = layer_find(doc, id);
                              return !l || !l->is_switch();
                            }),
             rows.end());

  /* Header: tabs, what it follows, pin, search, size, preview. */
  ImGui::SameLine();
  for (int t = 0; t < 2; t++) {
    ImGui::SameLine(0, t == 0 ? 8.0f : 2.0f);
    const bool on = area.shelf_tab == t;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(t == 0 ? "Context" : "All Switches")) {
      area.shelf_tab = t;
    }
    if (on) {
      ImGui::PopStyleColor();
    }
  }
  ImGui::SameLine(0, 8.0f);
  if (ImGui::Button(area.shelf_pinned ? "Pinned" : "Pin")) {
    area.shelf_pinned = !area.shelf_pinned;
    area.shelf_pins = rows;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(area.shelf_pinned ? "Pinned: these rows stay whatever you select. Click to follow the work again"
                                        : "Keep these rows whatever you select");
  }
  ImGui::SameLine();
  {
    std::string follows;
    if (!rows.empty() && area.shelf_tab == 0) {
      const Layer *inner = layer_find(doc, rows.back());
      follows = inner->name;
      if (rows.size() > 1) {
        follows += ", in " + layer_find(doc, rows.front())->name;
      }
    }
    ImGui::TextDisabled("%s", area.shelf_tab == 1 ? "Every switch"
                              : rows.empty()     ? "Select a layer in a switch, or a bone with swaps"
                                                 : follows.c_str());
  }
  /* Search, size and preview at the right, or on a line of their own when the shelf is narrow. */
  const float right_w = 330.0f;
  ImGui::SameLine();
  if (ImGui::GetContentRegionAvail().x < right_w) {
    ImGui::NewLine();
  }
  else {
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - right_w));
  }
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(130.0f);
  ImGui::InputTextWithHint("##shelf_search", "Search (Ctrl F)", &area.shelf_search);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  ImGui::SliderFloat("##shelf_tile", &area.shelf_tile, 40.0f, 160.0f, "%.0f px");
  ImGui::SameLine();
  ImGui::Checkbox("Preview", &area.shelf_hover_preview);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Hovering a tile shows it in the viewport a moment later, nothing keyed");
  }
  ImGui::Separator();

  ImGui::BeginChild("##shelf_rows", ImVec2(0, 0), ImGuiChildFlags_None);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float tile = std::clamp(area.shelf_tile, 40.0f, 160.0f);
  const float name_h = ImGui::GetTextLineHeight() + 4.0f;
  const int tex_size = int(tile * std::max(1.0f, io.DisplayFramebufferScale.x));
  const bool hovered_window = ImGui::IsWindowHovered();
  /* What the mouse is over this frame (preview and scrub), and the tiles in order (arrow keys). */
  uint64_t over_switch = 0, over_child = 0;
  std::vector<std::pair<uint64_t, uint64_t>> order;
  uint64_t menu_switch = 0, menu_child = 0;

  for (const uint64_t sw_id : rows) {
    Layer *sw = layer_find(doc, sw_id);
    if (!sw) {
      continue;
    }
    ImGui::PushID(int(sw_id));
    const SwitchSource source = switch_source(doc, *sw, doc.frame_current);
    if (const SwitchLink *link = switch_link_of(doc, *sw)) {
      const Layer *p = layer_parent(doc, sw->id);
      ImGui::Text("%s%s", p ? (p->name + " > ").c_str() : "", sw->name.c_str());
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.31f, 0.78f, 0.78f, 1.0f), "(linked: %d)", int(switch_link_members(doc, link->id).size()));
    }
    else {
      ImGui::Text("%s", sw->name.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", switch_value_label(*sw, float(sw->switch_shown)).c_str(),
                        source == SwitchSource::Hand  ? " (keyed)"
                        : source == SwitchSource::Rig ? " (from the rig)"
                                                      : " (the default)");
    /* Tiles in a row, wrapping. */
    const float avail = ImGui::GetContentRegionAvail().x;
    float x = 0.0f;
    const ImVec2 row_start = ImGui::GetCursorScreenPos();
    float y = row_start.y;
    const float gap = 6.0f;
    std::vector<uint64_t> ids;
    for (const Layer &c : sw->children) {
      if (matches(c.name, area.shelf_search)) {
        ids.push_back(c.id);
      }
    }
    ids.push_back(0); /* the + tile */
    for (const uint64_t child_id : ids) {
      if (x > 0.0f && x + tile > avail) {
        x = 0.0f;
        y += tile + name_h + gap;
      }
      const ImVec2 at(row_start.x + x, y);
      x += tile + gap;
      ImGui::SetCursorScreenPos(at);
      ImGui::PushID(int(child_id));
      ImGui::InvisibleButton("##tile", ImVec2(tile, tile + name_h));
      const bool over = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                        ImGui::IsMouseHoveringRect(at, at + ImVec2(tile, tile + name_h));
      const ImVec2 box_max = at + ImVec2(tile, tile);
      dl->AddRectFilled(at, box_max, IM_COL32(0x30, 0x30, 0x30, 255), 4.0f);
      if (child_id == 0) {
        /* The + tile: an empty drawing to draw in. */
        const ImVec2 c = at + ImVec2(tile * 0.5f, tile * 0.5f);
        const float r = tile * 0.18f;
        dl->AddLine(c - ImVec2(r, 0), c + ImVec2(r, 0), IM_COL32(0xc0, 0xc0, 0xc0, 255), 2.0f);
        dl->AddLine(c - ImVec2(0, r), c + ImVec2(0, r), IM_COL32(0xc0, 0xc0, 0xc0, 255), 2.0f);
        if (over) {
          dl->AddRect(at, box_max, IM_COL32(0xf0, 0xf0, 0xf0, 160), 4.0f);
          ImGui::SetTooltip("Add an empty drawing to %s, picked here", sw->name.c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          operator_call(C, "layer.switch_add_child", {{"layer", std::to_string(sw_id)}});
        }
        ImGui::PopID();
        continue;
      }
      const Layer *child = layer_find(doc, child_id);
      order.push_back({sw_id, child_id});
      if (const unsigned int tex = switch_thumbnail(doc, *sw, *child, tex_size)) {
        dl->AddImage(ImTextureRef(ImTextureID(intptr_t(tex))), at + ImVec2(2, 2), box_max - ImVec2(2, 2));
      }
      const bool picked = sw->switch_shown == child_id;
      uint64_t prev_sw = 0, prev_child = 0;
      const bool previewed = switch_preview_active(&prev_sw, &prev_child) && prev_sw == sw_id && prev_child == child_id;
      const bool focused = g_shelf.focus_switch == sw_id && g_shelf.focus_child == child_id;
      if (picked) {
        dl->AddRect(at, box_max, IM_COL32(0xf0, 0xf0, 0xf0, 255), 4.0f, 0, 2.0f);
      }
      else if (previewed || focused) {
        /* A preview, unlike a pick: dashed orange corners. */
        const ImU32 col = IM_COL32(0xff, 0x9a, 0x3c, 255);
        const float k = tile * 0.25f;
        for (const ImVec2 corner : {at, ImVec2(box_max.x, at.y), box_max, ImVec2(at.x, box_max.y)}) {
          const float sx = corner.x == at.x ? 1.0f : -1.0f, sy = corner.y == at.y ? 1.0f : -1.0f;
          dl->AddLine(corner, corner + ImVec2(sx * k, 0), col, 2.0f);
          dl->AddLine(corner, corner + ImVec2(0, sy * k), col, 2.0f);
        }
      }
      else if (over) {
        dl->AddRect(at, box_max, IM_COL32(0xf0, 0xf0, 0xf0, 90), 4.0f);
      }
      /* Its name under it, or a field while being renamed. */
      const ImVec2 name_at(at.x, box_max.y + 2.0f);
      if (C.wm && C.wm->shelf_rename == child_id) {
        /* A duplicate just made: its name opens for typing. */
        C.wm->shelf_rename = 0;
        g_shelf.renaming = child_id;
        g_shelf.rename_text = child->name;
        g_shelf.focus_rename = true;
      }
      if (g_shelf.renaming == child_id) {
        ImGui::SetCursorScreenPos(name_at);
        ImGui::SetNextItemWidth(tile);
        if (g_shelf.focus_rename) {
          ImGui::SetKeyboardFocusHere();
          g_shelf.focus_rename = false;
        }
        ImGui::InputText("##rename", &g_shelf.rename_text, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivated()) {
          if (!g_shelf.rename_text.empty() && g_shelf.rename_text != child->name) {
            if (g_shelf.rename_here) {
              operator_call(C, "layer.rename_here", {{"layer", std::to_string(child_id)}, {"name", g_shelf.rename_text}});
            }
            else {
              layer_find(doc, child_id)->name = g_shelf.rename_text;
              C.undo->push("Rename Layer", doc);
            }
          }
          g_shelf.renaming = 0;
          g_shelf.rename_here = false;
        }
      }
      else {
        dl->PushClipRect(name_at, name_at + ImVec2(tile, name_h), true);
        dl->AddText(name_at, picked ? IM_COL32(0xf0, 0xf0, 0xf0, 255) : IM_COL32(0xb0, 0xb0, 0xb0, 255), child->name.c_str());
        dl->PopClipRect();
      }
      if (over) {
        over_switch = sw_id;
        over_child = child_id;
        if (!g_shelf.dragging) {
          ImGui::SetItemTooltip("%s\nClick: show it here, keyed (Alt: the whole hold). Drag across the tiles to\n"
                                "scrub. Right-click for Duplicate, Rename, Set as Default and more",
                                child->name.c_str());
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          g_shelf.dragging = true;
          g_shelf.drag_switch = sw_id;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          menu_switch = sw_id;
          menu_child = child_id;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(name_at, name_at + ImVec2(tile, name_h))) {
          g_shelf.renaming = child_id;
          g_shelf.rename_text = child->name;
          g_shelf.focus_rename = true;
        }
      }
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(row_start.x, y + tile + name_h + gap));
    ImGui::Dummy(ImVec2(1, 4));
    ImGui::Separator();
    ImGui::PopID();
  }
  if (rows.empty()) {
    ImGui::TextDisabled("Nothing to show. Select a layer inside a switch (Layers editor, + > Switch, or Wrap in "
                        "Switch),\nor a bone whose counterparts are in a switch.");
  }

  /* The tile menu. */
  if (menu_child) {
    g_shelf.menu_switch = menu_switch;
    g_shelf.menu_child = menu_child;
    ImGui::OpenPopup("##shelf_tile_menu");
  }
  if (ui_menu_begin("##shelf_tile_menu")) {
    tile_menu(C, area, g_shelf.menu_switch, g_shelf.menu_child);
    ui_menu_end();
  }

  /* Keys while the mouse is over the shelf: arrows move a preview through the tiles, Enter picks it,
   * F2 renames, Esc ends a scrub or preview. */
  bool key_preview = false;
  if (hovered_window && !ImGui::GetIO().WantTextInput && !order.empty()) {
    int at = -1;
    for (int i = 0; i < int(order.size()); i++) {
      if (order[size_t(i)] == std::pair{g_shelf.focus_switch, g_shelf.focus_child}) {
        at = i;
      }
    }
    const int step = ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1
                     : ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1
                                                                                                        : 0;
    if (step) {
      at = at < 0 ? 0 : std::clamp(at + step, 0, int(order.size()) - 1);
      g_shelf.focus_switch = order[size_t(at)].first;
      g_shelf.focus_child = order[size_t(at)].second;
    }
    if (at >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
      if (const Layer *sw = layer_find(doc, g_shelf.focus_switch); sw && layer_find(doc, g_shelf.focus_child)) {
        pick(C, *sw, layer_find(doc, g_shelf.focus_child)->name, io.KeyAlt);
      }
      g_shelf.focus_child = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
      const uint64_t target = over_child ? over_child : g_shelf.focus_child;
      if (const Layer *l = target ? layer_find(doc, target) : nullptr) {
        g_shelf.renaming = target;
        g_shelf.rename_text = l->name;
        g_shelf.focus_rename = true;
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      g_shelf.focus_child = 0;
      g_shelf.dragging = false;
    }
    key_preview = g_shelf.focus_child != 0 && layer_find(doc, g_shelf.focus_child);
  }
  else if (!hovered_window) {
    g_shelf.focus_child = 0;
  }

  /* A drag across the tiles scrubs; letting go on one picks it (Esc cancels). */
  uint64_t want_sw = 0, want_child = 0;
  if (g_shelf.dragging) {
    if (over_switch == g_shelf.drag_switch) {
      want_sw = over_switch;
      want_child = over_child;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      g_shelf.dragging = false;
      if (want_child) {
        if (const Layer *sw = layer_find(doc, want_sw)) {
          pick(C, *sw, layer_find(doc, want_child)->name, io.KeyAlt);
        }
      }
      want_child = 0;
    }
  }
  else if (key_preview) {
    want_sw = g_shelf.focus_switch;
    want_child = g_shelf.focus_child;
  }
  else if (over_child && area.shelf_hover_preview) {
    /* Hover: after a moment. */
    if (g_shelf.hover_child != over_child) {
      g_shelf.hover_child = over_child;
      g_shelf.hover_since = ImGui::GetTime();
    }
    if (ImGui::GetTime() - g_shelf.hover_since > 0.35) {
      want_sw = over_switch;
      want_child = over_child;
    }
  }
  if (!over_child) {
    g_shelf.hover_child = 0;
  }
  uint64_t now_sw = 0, now_child = 0;
  const bool active_preview = switch_preview_active(&now_sw, &now_child);
  if (want_child && (!active_preview || now_sw != want_sw || now_child != want_child)) {
    switch_preview_set(want_sw, want_child);
    switches_resolve(doc, doc.frame_current);
    g_shelf.previewing = true;
  }
  else if (!want_child && g_shelf.previewing) {
    switch_preview_clear();
    switches_resolve(doc, doc.frame_current);
    g_shelf.previewing = false;
  }
  ImGui::EndChild();
}

}  // namespace oanim
