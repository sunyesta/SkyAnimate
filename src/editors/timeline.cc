/* Timeline editor, laid out like Blender's: a header with menus, playback buttons and the
 * frame fields, then a ruler with frame numbers, the playhead and the frame range. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

#include "anim/animation.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "editors/editors.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/keymap.hh"

namespace sky {

/* Colours from Blender's default timeline (dope sheet) theme. */
static constexpr ImU32 COL_ICON = IM_COL32(0xe6, 0xe6, 0xe6, 255);
static constexpr ImU32 COL_BUTTON_HOVER = IM_COL32(0x54, 0x54, 0x54, 255);
static constexpr ImU32 COL_REGION = IM_COL32(0x2b, 0x2b, 0x2b, 255);
static constexpr ImU32 COL_OUTSIDE_RANGE = IM_COL32(0x00, 0x00, 0x00, 0x33);
static constexpr ImU32 COL_GRID = IM_COL32(0x3a, 0x3a, 0x3a, 255);
static constexpr ImU32 COL_GRID_MINOR = IM_COL32(0x31, 0x31, 0x31, 255);
static constexpr ImU32 COL_RULER_TEXT = IM_COL32(0x9a, 0x9a, 0x9a, 255);
static constexpr ImU32 COL_PLAYHEAD = IM_COL32(0x47, 0x72, 0xb3, 255);
/* Blender's keyframe diamonds: light grey, orange when selected. */
static constexpr ImU32 COL_KEY = IM_COL32(0xdd, 0xdd, 0xdd, 255);
static constexpr ImU32 COL_KEY_SELECTED = IM_COL32(0xff, 0xbe, 0x33, 255);
static constexpr ImU32 COL_RECORD = IM_COL32(0xd6, 0x3c, 0x3c, 255);
/* Editing an action instead of the mainline: Blender's orange for "not the main data". */
static constexpr ImU32 COL_ACTION_ACTIVE = IM_COL32(0x8a, 0x5a, 0x1e, 255);
static constexpr ImU32 COL_ACTION_TINT = IM_COL32(0xd0, 0x8a, 0x2e, 0x14);

enum class Icon { Record, JumpStart, KeyPrev, PlayReverse, Play, Pause, KeyNext, JumpEnd };

/* Triangles are wound clockwise (screen y points down) so ImGui anti-aliases their edges. */
static void tri_right(ImDrawList *dl, float x0, float x1, float cy, float half_h, ImU32 col)
{
  dl->AddTriangleFilled(ImVec2(x0, cy - half_h), ImVec2(x1, cy), ImVec2(x0, cy + half_h), col);
}

static void tri_left(ImDrawList *dl, float x0, float x1, float cy, float half_h, ImU32 col)
{
  dl->AddTriangleFilled(ImVec2(x0, cy - half_h), ImVec2(x0, cy + half_h), ImVec2(x1, cy), col);
}

static void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 col)
{
  dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
}

static void icon_draw(ImDrawList *dl, Icon icon, ImVec2 c, float s, bool on)
{
  const ImU32 col = COL_ICON;
  switch (icon) {
    case Icon::Record:
      if (on) {
        dl->AddCircleFilled(c, s * 0.8f, COL_RECORD);
      }
      else {
        dl->AddCircle(c, s * 0.75f, col, 0, 1.5f);
      }
      break;
    case Icon::Play:
      tri_right(dl, c.x - s * 0.7f, c.x + s * 0.9f, c.y, s, col);
      break;
    case Icon::PlayReverse:
      tri_left(dl, c.x + s * 0.7f, c.x - s * 0.9f, c.y, s, col);
      break;
    case Icon::Pause:
      dl->AddRectFilled(ImVec2(c.x - s * 0.7f, c.y - s), ImVec2(c.x - s * 0.2f, c.y + s), col);
      dl->AddRectFilled(ImVec2(c.x + s * 0.2f, c.y - s), ImVec2(c.x + s * 0.7f, c.y + s), col);
      break;
    case Icon::JumpStart:
      dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x - s * 0.65f, c.y + s), col);
      tri_left(dl, c.x + s * 0.9f, c.x - s * 0.55f, c.y, s, col);
      break;
    case Icon::JumpEnd:
      dl->AddRectFilled(ImVec2(c.x + s * 0.65f, c.y - s), ImVec2(c.x + s, c.y + s), col);
      tri_right(dl, c.x - s * 0.9f, c.x + s * 0.55f, c.y, s, col);
      break;
    case Icon::KeyPrev:
      tri_left(dl, c.x - s * 0.15f, c.x - s, c.y, s * 0.7f, col);
      diamond(dl, ImVec2(c.x + s * 0.45f, c.y), s * 0.6f, col);
      break;
    case Icon::KeyNext:
      tri_right(dl, c.x + s * 0.15f, c.x + s, c.y, s * 0.7f, col);
      diamond(dl, ImVec2(c.x - s * 0.45f, c.y), s * 0.6f, col);
      break;
  }
}

static bool icon_button(const char *id, Icon icon, bool on, const char *tooltip, const std::string &shortcut)
{
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (ImGui::IsItemHovered()) {
    dl->AddRectFilled(p, p + ImVec2(h, h), COL_BUTTON_HOVER, ImGui::GetStyle().FrameRounding);
  }
  icon_draw(dl, icon, p + ImVec2(h * 0.5f, h * 0.5f), h * 0.3f, on);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    if (shortcut.empty()) {
      ImGui::SetTooltip("%s", tooltip);
    }
    else {
      ImGui::SetTooltip("%s\nShortcut: %s", tooltip, shortcut.c_str());
    }
  }
  return pressed;
}

/* A button that runs an operator, with its name and shortcut as the tooltip. */
static void operator_icon_button(Context &C, const char *id, Icon icon, const char *idname)
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return;
  }
  if (icon_button(id, icon, false, ot->name.c_str(), keymaps().shortcut_for(idname))) {
    operator_call(C, idname);
  }
}

static void view_frame_all(Area &area, const Document &doc)
{
  const float span = float(std::max(doc.frame_end - doc.frame_start, 10));
  const float margin = span * 0.04f + 1.0f;
  area.view_start = float(doc.frame_start) - margin;
  area.view_end = float(doc.frame_end) + margin;
}

static void view_center_current(Area &area, const Document &doc)
{
  const float half = (area.view_end - area.view_start) * 0.5f;
  area.view_start = float(doc.frame_current) - half;
  area.view_end = float(doc.frame_current) + half;
}

/* An integer field that pushes an undo step when the user lets go of it. */
static void frame_field(Context &C, const char *id, int *value, const char *format, float width,
                        const char *undo_name)
{
  ImGui::SetNextItemWidth(width);
  ImGui::DragInt(id, value, 0.25f, 0, 1048574, format, ImGuiSliderFlags_AlwaysClamp);
  if (undo_name && ImGui::IsItemDeactivatedAfterEdit() && C.undo) {
    C.undo->push(undo_name, *C.document);
  }
}

/* Which timeline is edited: the mainline, or an action of the bone layer the active layer is in,
 * like Moho's switch between the mainline and an action. The + next to it adds an action. */
static void action_menu(Context &C, float width)
{
  Document &doc = *C.document;
  const bool in_action = !doc.action.empty();
  const std::string preview = in_action ? doc.action : "Mainline";
  Layer *owner = in_action ? layer_find(doc, doc.action_layer) : action_owner(doc, doc.active_layer);
  if (in_action && !owner) {
    owner = action_owner(doc, doc.active_layer);
  }

  if (in_action) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, COL_ACTION_ACTIVE);
  }
  const float plus_w = owner ? ImGui::GetFrameHeight() : 0.0f;
  ImGui::SetNextItemWidth(width - plus_w - (owner ? 2.0f : 0.0f));
  const bool open = ImGui::BeginCombo("##action", preview.c_str());
  if (in_action) {
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Timeline being edited: the mainline, or an action of the bone layer you're in (a separate "
                      "animation for it and everything inside)\n\nPython: sky.data.document.action");
  }
  if (open) {
    std::string doomed;
    const auto action_item = [&](const std::string &name) {
      if (ImGui::Selectable(name.c_str(), in_action && doc.action == name && owner && doc.action_layer == owner->id)) {
        operator_call(C, "anim.action_set", {{"name", name}});
      }
      /* Right-click: the action's own menu. */
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete")) {
          doomed = name;
        }
        ImGui::EndPopup();
      }
    };
    /* At the top, with one bone selected: its smart actions, and a new one while it has fewer
     * than two (the second turns the other way). */
    const Bone *single = nullptr;
    if (owner) {
      int picked = 0;
      for (const Bone &b : owner->bones) {
        if (b.selected || b.select_head || b.select_tail) {
          picked++;
          single = &b;
        }
      }
      if (picked != 1) {
        single = nullptr;
      }
    }
    if (single) {
      ImGui::PushID("selected");
      ImGui::SeparatorText(("Selected: " + single->name).c_str());
      int count = 0;
      for (const std::string &name : owner->actions) {
        if (smart_action_bone(*owner, name) == single) {
          action_item(name);
          count++;
        }
      }
      if (count < 2) {
        if (ImGui::Selectable(count == 0 ? "+ New Smart Action" : "+ New Smart Action (other way)")) {
          operator_call(C, "anim.action_new", {{"name", single->name}, {"smart", "1"}});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("An action named after the bone: key the bone turning, and what it should move. Then "
                            "turning the bone anywhere else plays the action that far (Moho's smart bones). A "
                            "second one covers turning the other way");
        }
      }
      ImGui::PopID();
      ImGui::Separator();
    }
    if (ImGui::Selectable("Mainline", !in_action)) {
      operator_call(C, "anim.action_set", {{"name", ""}});
    }
    if (owner && !owner->actions.empty()) {
      for (const bool smart : {false, true}) {
        bool any = false;
        for (const std::string &name : owner->actions) {
          if ((smart_action_bone(*owner, name) != nullptr) != smart) {
            continue;
          }
          if (!any) {
            ImGui::SeparatorText(smart ? "Smart Actions" : "Actions");
            any = true;
          }
          action_item(name);
        }
      }
    }
    if (owner && !single) {
      ImGui::Separator();
      ImGui::TextDisabled("Select one bone for its smart actions");
    }
    else if (!owner) {
      ImGui::Separator();
      ImGui::TextDisabled("Select a bone layer, or a layer in one,\nto add and edit its actions.");
    }
    /* After drawing the list, which the deletion changes. */
    if (!doomed.empty()) {
      operator_call(C, "anim.action_delete", {{"name", doomed}});
    }
    ImGui::EndCombo();
  }
  if (!owner) {
    return;
  }
  ImGui::SameLine(0, 2.0f);
  static char new_name[64] = "Action";
  if (ImGui::Button("+##new_action", ImVec2(plus_w, 0))) {
    std::snprintf(new_name, sizeof(new_name), "%s", "Action");
    ImGui::OpenPopup("##new_action_popup");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("New action for \"%s\"", owner->name.c_str());
  }
  if (ImGui::BeginPopup("##new_action_popup")) {
    ImGui::TextUnformatted("New Action");
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    const bool entered = ImGui::InputText("##action_name", new_name, sizeof(new_name),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if ((entered || ImGui::Button("Create")) && new_name[0] != '\0') {
      operator_call(C, "anim.action_new", {{"name", new_name}});
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

static void timeline_header(Area &area, Context &C)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float pad = style.ItemSpacing.x;
  const float top = 4.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;

  /* Left: editor type and the View, Marker and Playback menus. */
  ImGui::SetCursorScreenPos(origin + ImVec2(pad, top));
  area_editor_type_menu(area);
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
  for (const char *menu : {"View", "Marker", "Playback"}) {
    ImGui::SameLine();
    if (ImGui::Button(menu)) {
      ImGui::OpenPopup(menu);
    }
  }
  ImGui::PopStyleColor();
  ImGui::SameLine(0.0f, pad * 2.0f);
  action_menu(C, h * 6.0f);
  const float menus_end = ImGui::GetItemRectMax().x;

  if (ImGui::BeginPopup("View")) {
    if (ImGui::MenuItem("Frame All", "Home")) {
      view_frame_all(area, doc);
    }
    if (ImGui::MenuItem("Go to Current Frame")) {
      view_center_current(area, doc);
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("Marker")) {
    ImGui::MenuItem("Add Marker", "M", false, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Arrives in milestone 3");
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("Playback")) {
    ImGui::MenuItem("Loop Playback", nullptr, &wm.play_loop);
    ImGui::MenuItem("Auto Keying", nullptr, &doc.auto_key);
    ImGui::EndPopup();
  }

  /* Centre: auto-keying, then the playback buttons. */
  const float spacing = std::max(1.0f, style.ItemSpacing.x * 0.35f);
  const float record_gap = h * 0.6f;
  const float transport_w = 7.0f * h + 5.0f * spacing + record_gap;
  const float transport_x = std::max(menus_end + pad * 2.0f, origin.x + (width - transport_w) * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(transport_x, origin.y + top));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, style.ItemSpacing.y));
  if (icon_button("##auto_key", Icon::Record, doc.auto_key,
                  "Auto Keying\nEditing at a frame other than 0 keys what you change there", "")) {
    doc.auto_key = !doc.auto_key;
  }
  ImGui::SameLine(0.0f, record_gap);
  operator_icon_button(C, "##jump_start", Icon::JumpStart, "screen.frame_jump_start");
  ImGui::SameLine();
  operator_icon_button(C, "##key_prev", Icon::KeyPrev, "screen.keyframe_jump_prev");
  ImGui::SameLine();
  operator_icon_button(C, "##play_reverse", (wm.playing && wm.play_reverse) ? Icon::Pause : Icon::PlayReverse,
                       "screen.animation_play_reverse");
  ImGui::SameLine();
  operator_icon_button(C, "##play", (wm.playing && !wm.play_reverse) ? Icon::Pause : Icon::Play,
                       "screen.animation_play");
  ImGui::SameLine();
  operator_icon_button(C, "##key_next", Icon::KeyNext, "screen.keyframe_jump_next");
  ImGui::SameLine();
  operator_icon_button(C, "##jump_end", Icon::JumpEnd, "screen.frame_jump_end");
  ImGui::PopStyleVar();
  const float transport_end = ImGui::GetItemRectMax().x;

  /* Right: current frame, then the Start and End of the range. */
  const float current_w = h * 2.6f;
  const float range_w = h * 4.0f;
  const float group_gap = h * 0.6f;
  const float fields_w = current_w + group_gap + range_w * 2.0f + style.ItemSpacing.x;
  const float fields_x = std::max(transport_end + pad * 2.0f, origin.x + width - fields_w - pad);
  ImGui::SetCursorScreenPos(ImVec2(fields_x, origin.y + top));
  frame_field(C, "##frame_current", &doc.frame_current, "%d", current_w, nullptr);
  ImGui::SameLine(0.0f, group_gap);
  frame_field(C, "##frame_start", &doc.frame_start, "Start %d", range_w, "Change Start");
  ImGui::SameLine();
  frame_field(C, "##frame_end", &doc.frame_end, "End %d", range_w, "Change End");
  doc.frame_end = std::max(doc.frame_end, doc.frame_start);

  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top + h + top));
}

/* Frames between labelled ruler ticks, chosen so labels are at least min_px apart.
 * Like Blender, steps follow the frame rate: at 24 fps that is 6, 12, 24, 48, ... */
static float ruler_step(float span, float width, float min_px, float fps)
{
  const float second = std::max(1.0f, std::round(fps));
  const float candidates[] = {1.0f, 2.0f, second / 4.0f, second / 2.0f, second};
  for (float step : candidates) {
    if (step >= 1.0f && std::floor(step) == step && width * step / span >= min_px) {
      return step;
    }
  }
  float step = second;
  while (width * step / span < min_px) {
    step *= 2.0f;
  }
  return step;
}

/* A press in the timeline: scrubbing (in the ruler) or picking keys (below it). */
struct TimelinePress {
  bool active = false;
  bool scrub = false;
  ImVec2 start;
};
static TimelinePress g_press;

/* G or S on keys: from a copy of the document, the keys move by the mouse until a click. */
struct KeyTransform {
  bool active = false;
  int area = 0;
  char kind = 'G';
  float start_x = 0.0f;
  Document snapshot;
};
static KeyTransform g_key_transform;

static bool modal_key_take(Context &C, const char *type)
{
  std::vector<ModalKey> &keys = C.wm->modal_keys;
  for (auto it = keys.begin(); it != keys.end(); ++it) {
    if (it->type == type) {
      keys.erase(it);
      return true;
    }
  }
  return false;
}

/* A row of the timeline. Rows fold, as Blender's channels do:

     All                     everything of the layers the timeline shows (white)
       Layers
         <layer>             that layer's own channels
           Angle & Transform
             Angle           rotation
             Transform       position and scale
           Opacity
           Visibility
       Bones                 every bone's angle and transform
         Bone Angle
         Bone Transform
       Points                every point's position, width and shape
     Selected                the same for the selected bones and points (red rows)

   A row with no keys under it isn't shown; a folding row shows the keys of all it holds. */
struct ChannelRow {
  std::string name;
  std::vector<Layer *> layers;
  KeyFilter filter;
  int depth = 0;
  /* Key of a folding row's open state, "" when it doesn't fold. */
  std::string fold;
  bool open = false;
  /* In the Selected section (drawn red). */
  bool selected = false;
  /* A layer's own row: clicking its name makes that layer active. */
  uint64_t layer = 0;
  /* Covers every layer the timeline shows. */
  bool all_layers = false;
};

static bool row_open(const Area &area, const std::string &key, bool fallback)
{
  const auto it = area.timeline_open.find(key);
  return it == area.timeline_open.end() ? fallback : it->second;
}

/* Something selected for the Selected section: a point, or a bone (by body or ball). */
static bool anything_selected(const std::vector<Layer *> &layers)
{
  for (const Layer *l : layers) {
    for (const MeshPoint &p : l->drawing.points) {
      if (p.selected) {
        return true;
      }
    }
    for (const Bone &b : l->bones) {
      if (b.selected || b.select_head || b.select_tail) {
        return true;
      }
    }
  }
  return false;
}

static std::vector<ChannelRow> channel_rows(const Area &area, const std::vector<Layer *> &layers)
{
  std::vector<ChannelRow> rows;
  const auto keyed = [](const std::vector<Layer *> &ls, const KeyFilter &f) { return !layers_key_summary(ls, f).empty(); };
  const auto add = [&](std::string name, std::vector<Layer *> ls, KeyFilter f, int depth, std::string fold,
                       bool fold_default, bool selected, uint64_t layer, bool all) -> bool {
    ChannelRow row{std::move(name), std::move(ls), f, depth, fold, false, selected, layer, all};
    row.open = !fold.empty() && row_open(area, fold, fold_default);
    rows.push_back(std::move(row));
    return rows.back().open;
  };
  if (layers.empty()) {
    return rows;
  }
  /* The bones and points part, for All and for Selected. */
  const auto bones_and_points = [&](bool selected, int depth, const char *prefix) {
    KeyFilter bones{KEY_BONES, selected}, angle{KEY_BONE_ANGLE, selected}, move{KEY_BONE_TRANSFORM, selected},
        points{KEY_POINTS, selected};
    if (keyed(layers, bones) &&
        add("Bones", layers, bones, depth, std::string(prefix) + "bones", false, selected, 0, true))
    {
      if (keyed(layers, angle)) {
        add("Bone Angle", layers, angle, depth + 1, "", false, selected, 0, true);
      }
      if (keyed(layers, move)) {
        add("Bone Transform", layers, move, depth + 1, "", false, selected, 0, true);
      }
    }
    if (keyed(layers, points)) {
      add("Points", layers, points, depth, "", false, selected, 0, true);
    }
  };
  if (add("All", layers, KeyFilter{}, 0, "all", true, false, 0, true)) {
    const KeyFilter layer_kinds{KEY_LAYER, false};
    if (keyed(layers, layer_kinds) && add("Layers", layers, layer_kinds, 1, "layers", true, false, 0, true)) {
      for (Layer *l : layers) {
        if (!keyed({l}, layer_kinds)) {
          continue;
        }
        const std::string id = std::to_string(l->id);
        if (!add(l->name, {l}, layer_kinds, 2, "layer:" + id, false, false, l->id, false)) {
          continue;
        }
        const KeyFilter at{KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM, false};
        if (keyed({l}, at) && add("Angle & Transform", {l}, at, 3, "lat:" + id, false, false, 0, false)) {
          if (keyed({l}, {KEY_LAYER_ANGLE, false})) {
            add("Angle", {l}, {KEY_LAYER_ANGLE, false}, 4, "", false, false, 0, false);
          }
          if (keyed({l}, {KEY_LAYER_TRANSFORM, false})) {
            add("Transform", {l}, {KEY_LAYER_TRANSFORM, false}, 4, "", false, false, 0, false);
          }
        }
        if (keyed({l}, {KEY_OPACITY, false})) {
          add("Opacity", {l}, {KEY_OPACITY, false}, 3, "", false, false, 0, false);
        }
        if (keyed({l}, {KEY_VISIBILITY, false})) {
          add("Visibility", {l}, {KEY_VISIBILITY, false}, 3, "", false, false, 0, false);
        }
      }
    }
    bones_and_points(false, 1, "");
  }
  if (anything_selected(layers) &&
      add("Selected", layers, KeyFilter{KEY_BONES | KEY_POINTS, true}, 0, "sel", true, true, 0, true))
  {
    bones_and_points(true, 1, "sel_");
  }
  return rows;
}

static std::string layer_ids(const std::vector<Layer *> &layers)
{
  std::string ids;
  for (const Layer *l : layers) {
    ids += (ids.empty() ? "" : ",") + std::to_string(l->id);
  }
  return ids;
}

static void timeline_body(Area &area, Context &C)
{
  Document &doc = *C.document;
  ImGuiIO &io = ImGui::GetIO();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  if (size.x < 8.0f || size.y < 8.0f) {
    return;
  }
  const ImVec2 p1 = p0 + size;

  if (!area.view_initialized) {
    view_frame_all(area, doc);
    area.view_initialized = true;
  }

  ImGui::InvisibleButton("##timeline_region", size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();

  const std::vector<Layer *> layers = timeline_layers(C);
  /* Channel rows: Summary, then each animated (active or selected) layer, as Blender's Dope Sheet
   * lists the selected objects. The names sit in a column on the left. */
  std::vector<ChannelRow> rows = channel_rows(area, layers);
  const float font_size = ImGui::GetFontSize();
  const float names_w = std::min(font_size * 9.0f, size.x * 0.4f);
  /* The keys region: right of the names. */
  const ImVec2 k0(p0.x + names_w, p0.y);
  const float keys_w = std::max(size.x - names_w, 8.0f);
  const float ruler_h = ImGui::GetFrameHeight() + 2.0f;
  const float row_h = ImGui::GetFrameHeight();
  const float rows_top = p0.y + ruler_h + 2.0f;
  const float rows_h = std::max(p1.y - rows_top, 1.0f);
  area.channel_scroll = std::clamp(area.channel_scroll, 0.0f, std::max(0.0f, float(rows.size()) * row_h - rows_h));
  const auto row_top = [&](size_t r) { return rows_top + float(r) * row_h - area.channel_scroll; };
  const auto row_at = [&](float y) -> int {
    if (y < rows_top) {
      return -1;
    }
    const int r = int(std::floor((y - rows_top + area.channel_scroll) / row_h));
    return r >= 0 && r < int(rows.size()) ? r : -1;
  };
  const bool over_names = io.MousePos.x < k0.x;

  /* Navigation: wheel zooms around the mouse (over the names it scrolls the rows), Ctrl+wheel or
   * middle-drag pans, Home frames all. */
  float span = area.view_end - area.view_start;
  if (hovered) {
    if (over_names) {
      area.channel_scroll -= io.MouseWheel * row_h * 2.0f;
    }
    else {
      const float mouse_frame = area.view_start + (io.MousePos.x - k0.x) / keys_w * span;
      if (io.MouseWheel != 0.0f && !io.KeyCtrl) {
        const float factor = std::pow(0.85f, io.MouseWheel);
        const float new_span = std::clamp(span * factor, 10.0f, 200000.0f);
        const float t = (mouse_frame - area.view_start) / span;
        area.view_start = mouse_frame - t * new_span;
        area.view_end = area.view_start + new_span;
      }
      const float pan_wheel = io.KeyCtrl ? io.MouseWheel : io.MouseWheelH;
      if (pan_wheel != 0.0f) {
        const float delta = -pan_wheel * span * 0.1f;
        area.view_start += delta;
        area.view_end += delta;
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
      view_frame_all(area, doc);
    }
  }
  span = area.view_end - area.view_start;
  if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    const float delta = -io.MouseDelta.x / keys_w * span;
    area.view_start += delta;
    area.view_end += delta;
  }

  const auto frame_x = [&](float frame) { return k0.x + (frame - area.view_start) / span * keys_w; };
  const auto x_frame = [&](float x) { return area.view_start + (x - k0.x) / keys_w * span; };
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float key_r = std::max(4.0f, std::min(font_size * 0.36f, row_h * 0.4f));
  std::vector<std::map<int, KeyFrameSummary>> summaries(rows.size());
  const auto summarize = [&]() {
    for (size_t r = 0; r < rows.size(); r++) {
      summaries[r] = layers_key_summary(rows[r].layers, rows[r].filter);
    }
  };
  summarize();

  /* G and S on the selected keys follow the mouse, as in Blender. */
  if (C.wm->timeline_transform_request && hovered && !g_key_transform.active) {
    g_key_transform = {true, area.id, C.wm->timeline_transform_request, io.MousePos.x, doc};
    C.wm->modal_running = true;
    C.wm->modal_keys.clear();
  }
  C.wm->timeline_transform_request = 0;
  std::string transform_info;
  if (g_key_transform.active && g_key_transform.area == area.id) {
    const int frame = doc.frame_current;
    if (modal_key_take(C, "ESC") || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      doc = g_key_transform.snapshot;
      doc.frame_current = frame;
      g_key_transform.active = false;
      C.wm->modal_running = false;
    }
    else {
      doc = g_key_transform.snapshot;
      doc.frame_current = frame;
      const float frames_per_px = span / keys_w;
      if (g_key_transform.kind == 'D') {
        /* Shift D: copies of the selected keys follow the mouse; the originals stay. */
        const int offset = int(std::lround((io.MousePos.x - g_key_transform.start_x) * frames_per_px));
        layers_duplicate_selected_keys(timeline_layers(C), offset);
        transform_info = "Duplicate Keys  " + std::string(offset >= 0 ? "+" : "") + std::to_string(offset);
      }
      else if (g_key_transform.kind == 'G') {
        const int offset = int(std::lround((io.MousePos.x - g_key_transform.start_x) * frames_per_px));
        layers_move_selected_keys(timeline_layers(C), [offset](int f) { return f + offset; });
        transform_info = "Move Keys  " + std::string(offset >= 0 ? "+" : "") + std::to_string(offset);
      }
      else {
        /* Scale about the playhead by how far the mouse is from it. */
        const float cx = frame_x(float(frame));
        const float start = std::max(std::fabs(g_key_transform.start_x - cx), 1.0f);
        const float factor = std::fabs(io.MousePos.x - cx) / start;
        layers_move_selected_keys(timeline_layers(C), [&](int f) {
          return int(std::lround(float(frame) + float(f - frame) * factor));
        });
        char text[48];
        std::snprintf(text, sizeof(text), "Scale Keys  %.2f", factor);
        transform_info = text;
      }
      animation_evaluate(doc, frame);
      if (modal_key_take(C, "RET") || modal_key_take(C, "SPACE") || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_key_transform.active = false;
        g_key_transform.snapshot = Document();
        C.wm->modal_running = false;
        C.undo->push(g_key_transform.kind == 'G'   ? "Move Keys"
                     : g_key_transform.kind == 'S' ? "Scale Keys"
                                                   : "Duplicate Keys",
                     doc);
        /* The confirming click shouldn't also pick keys. */
        g_press = {};
      }
      C.wm->modal_keys.clear();
    }
    /* The document was replaced: look the layers up again. */
    rows = channel_rows(area, timeline_layers(C));
    summaries.resize(rows.size());
    summarize();
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    if (over_names) {
      /* The arrow folds or opens a row; a layer's name makes it the active layer (the selection
       * stays); elsewhere a folding row's name folds it too. */
      const int r = row_at(io.MousePos.y);
      if (r >= 0) {
        const ChannelRow &row = rows[r];
        const float arrow_x = p0.x + 6.0f + float(row.depth) * font_size;
        const bool on_arrow = io.MousePos.x < arrow_x + font_size;
        if (!row.fold.empty() && (on_arrow || !row.layer)) {
          area.timeline_open[row.fold] = !row.open;
        }
        else if (row.layer) {
          doc.active_layer = row.layer;
          C.undo->push("Active Layer", doc);
        }
      }
      g_press = {};
    }
    else {
      /* The ruler scrubs; below it a click picks keys of a row and a drag boxes them. */
      g_press = {true, io.MousePos.y < p0.y + ruler_h, io.MousePos};
      if (!g_press.scrub) {
        const int r = row_at(io.MousePos.y);
        int hit = -1;
        if (r >= 0) {
          const float ky = row_top(r) + row_h * 0.5f;
          for (const auto &[f, sum] : summaries[r]) {
            if (std::fabs(io.MousePos.x - frame_x(float(f))) <= key_r + 2.0f && std::fabs(io.MousePos.y - ky) <= key_r + 3.0f) {
              hit = f;
            }
          }
        }
        if (hit >= 0) {
          const KeyFrameSummary &sum = summaries[r][hit];
          operator_call(C, "anim.keys_select",
                        {{"frame_start", std::to_string(hit)},
                         {"layers", rows[r].all_layers ? std::string() : layer_ids(rows[r].layers)},
                         {"kinds", std::to_string(rows[r].filter.kinds)},
                         {"selected_only", rows[r].filter.selected_only ? "1" : "0"},
                         {"extend", io.KeyShift ? "1" : "0"},
                         {"deselect", io.KeyShift && sum.selected == sum.keys ? "1" : "0"}});
          g_press = {};
        }
      }
    }
  }
  if (g_press.active && g_press.scrub && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    doc.frame_current = std::max(0, int(std::lround(x_frame(io.MousePos.x))));
    C.wm->playing = false;
  }
  const bool boxing = g_press.active && !g_press.scrub &&
                      std::fabs(io.MousePos.x - g_press.start.x) + std::fabs(io.MousePos.y - g_press.start.y) > 4.0f;
  if (g_press.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (!g_press.scrub) {
      if (boxing) {
        /* Keys of the rows the box covers, each row's own kind: Shift adds, Ctrl takes away, as
         * Blender's box select. */
        const float lo_y = std::min(g_press.start.y, io.MousePos.y), hi_y = std::max(g_press.start.y, io.MousePos.y);
        std::vector<size_t> boxed;
        for (size_t r = 0; r < rows.size(); r++) {
          const float ky = row_top(r) + row_h * 0.5f;
          if (ky >= lo_y - key_r && ky <= hi_y + key_r) {
            boxed.push_back(r);
          }
        }
        if (!boxed.empty()) {
          const int first = int(std::ceil(x_frame(std::min(g_press.start.x, io.MousePos.x))));
          const int last = int(std::floor(x_frame(std::max(g_press.start.x, io.MousePos.x))));
          for (size_t i = 0; i < boxed.size(); i++) {
            const ChannelRow &row = rows[boxed[i]];
            operator_call(C, "anim.keys_select",
                          {{"frame_start", std::to_string(first)},
                           {"frame_end", std::to_string(last)},
                           {"layers", row.all_layers ? std::string() : layer_ids(row.layers)},
                           {"kinds", std::to_string(row.filter.kinds)},
                           {"selected_only", row.filter.selected_only ? "1" : "0"},
                           /* Rows after the first add to what the first picked. */
                           {"extend", io.KeyShift || i > 0 ? "1" : "0"},
                           {"deselect", io.KeyCtrl ? "1" : "0"}});
          }
        }
        else if (!io.KeyShift) {
          operator_call(C, "anim.keys_deselect_all");
        }
      }
      else if (!io.KeyShift) {
        operator_call(C, "anim.keys_deselect_all");
      }
      summarize();
    }
    g_press = {};
  }
  dl->AddRectFilled(p0, p1, COL_REGION);
  if (!doc.action.empty()) {
    /* A warm tint is a constant reminder that keys go into the action, not the mainline. */
    dl->AddRectFilled(p0, p1, COL_ACTION_TINT);
  }

  /* Darken everything outside the frame range. */
  const float x_start = frame_x(float(doc.frame_start));
  const float x_end = frame_x(float(doc.frame_end));
  if (x_start > k0.x) {
    dl->AddRectFilled(k0, ImVec2(std::min(x_start, p1.x), p1.y), COL_OUTSIDE_RANGE);
  }
  if (x_end < p1.x) {
    dl->AddRectFilled(ImVec2(std::max(x_end, k0.x), p0.y), p1, COL_OUTSIDE_RANGE);
  }

  /* Grid lines and ruler numbers. */
  dl->PushClipRect(k0, p1, true);
  const float step = ruler_step(span, keys_w, ImGui::GetFontSize() * 4.0f, doc.fps);
  for (float f = std::floor(area.view_start / step) * step; f <= area.view_end; f += step) {
    const float x = frame_x(f);
    if (step >= 2.0f) {
      const float xm = frame_x(f + step * 0.5f);
      dl->AddLine(ImVec2(xm, p0.y + ruler_h), ImVec2(xm, p1.y), COL_GRID_MINOR);
    }
    if (x < k0.x) {
      continue;
    }
    dl->AddLine(ImVec2(x, p0.y + ruler_h), ImVec2(x, p1.y), COL_GRID);
    const std::string label = std::to_string(int(f));
    const float tw = ImGui::CalcTextSize(label.c_str()).x;
    dl->AddText(ImVec2(x - tw * 0.5f, p0.y + (ruler_h - font_size) * 0.5f), COL_RULER_TEXT, label.c_str());
  }
  dl->PopClipRect();

  /* The rows: hold bars between neighbouring keys where nothing changes, and a diamond per keyed
   * frame, orange when selected. */
  dl->PushClipRect(ImVec2(p0.x, rows_top), p1, true);
  for (size_t r = 0; r < rows.size(); r++) {
    const float top = row_top(r);
    if (top + row_h < rows_top || top > p1.y) {
      continue;
    }
    const float ky = top + row_h * 0.5f;
    if (rows[r].selected) {
      /* The Selected section: red rows, as Moho marks the selected bones' and points' keys. */
      dl->AddRectFilled(ImVec2(k0.x, top), ImVec2(p1.x, top + row_h), IM_COL32(0xc0, 0x30, 0x30, 38));
      dl->AddLine(ImVec2(k0.x, ky), ImVec2(p1.x, ky), IM_COL32(0xd0, 0x40, 0x40, 110));
    }
    else if (r % 2 == 1) {
      dl->AddRectFilled(ImVec2(p0.x, top), ImVec2(p1.x, top + row_h), IM_COL32(0xff, 0xff, 0xff, 6));
    }
    dl->AddLine(ImVec2(p0.x, top + row_h), ImVec2(p1.x, top + row_h), IM_COL32(0x22, 0x22, 0x22, 255));
    std::map<int, KeyFrameSummary> &sum_row = summaries[r];
    dl->PushClipRect(ImVec2(k0.x, rows_top), p1, true);
    for (const auto &[a, b] : layers_hold_spans(rows[r].layers, rows[r].filter)) {
      const float xa = frame_x(float(a)), xb = frame_x(float(b));
      if (xb < k0.x || xa > p1.x) {
        continue;
      }
      const bool selected = sum_row[a].selected > 0 && sum_row[b].selected > 0;
      dl->AddRectFilled(ImVec2(xa, ky - key_r * 0.45f), ImVec2(xb, ky + key_r * 0.45f),
                        selected ? IM_COL32(0xc0, 0x90, 0x30, 200) : IM_COL32(0x80, 0x80, 0x80, 170));
    }
    const float kr = rows[r].depth == 0 ? key_r : key_r * 0.85f;
    for (const auto &[f, sum] : sum_row) {
      const float kx = frame_x(float(f));
      if (kx < k0.x - kr || kx > p1.x + kr) {
        continue;
      }
      const ImVec2 d[4] = {ImVec2(kx, ky - kr), ImVec2(kx + kr, ky), ImVec2(kx, ky + kr), ImVec2(kx - kr, ky)};
      dl->AddConvexPolyFilled(d, 4, sum.selected > 0 ? COL_KEY_SELECTED : COL_KEY);
      dl->AddPolyline(d, 4, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 1.0f);
    }
    dl->PopClipRect();
  }
  dl->PopClipRect();

  /* The names column. */
  dl->AddRectFilled(p0, ImVec2(k0.x, p1.y), IM_COL32(0x26, 0x26, 0x26, 255));
  dl->AddLine(ImVec2(k0.x, p0.y), ImVec2(k0.x, p1.y), IM_COL32(0x18, 0x18, 0x18, 255));
  dl->PushClipRect(ImVec2(p0.x, rows_top), ImVec2(k0.x - 4.0f, p1.y), true);
  for (size_t r = 0; r < rows.size(); r++) {
    const float top = row_top(r);
    if (top + row_h < rows_top || top > p1.y) {
      continue;
    }
    const ChannelRow &row = rows[r];
    const float ky = top + row_h * 0.5f;
    const bool is_active = row.layer != 0 && row.layer == doc.active_layer;
    if (is_active) {
      dl->AddRectFilled(ImVec2(p0.x, top), ImVec2(k0.x, top + row_h), IM_COL32(0x33, 0x4d, 0x80, 255));
    }
    else if (row.selected) {
      dl->AddRectFilled(ImVec2(p0.x, top), ImVec2(k0.x, top + row_h), IM_COL32(0xc0, 0x30, 0x30, 45));
    }
    const ImU32 col = row.depth == 0 ? (row.selected ? IM_COL32(0xff, 0x90, 0x90, 255) : IM_COL32(0xe6, 0xe6, 0xe6, 255))
                      : is_active    ? IM_COL32(0xff, 0xff, 0xff, 255)
                                     : IM_COL32(0xc8, 0xc8, 0xc8, 255);
    float x = p0.x + 6.0f + float(row.depth) * font_size;
    if (!row.fold.empty()) {
      /* The fold arrow: pointing down when open, right when folded. */
      const float s = font_size * 0.28f;
      const ImVec2 c(x + font_size * 0.4f, ky);
      if (row.open) {
        dl->AddTriangleFilled(c + ImVec2(-s, -s * 0.6f), c + ImVec2(s, -s * 0.6f), c + ImVec2(0, s * 0.8f), col);
      }
      else {
        dl->AddTriangleFilled(c + ImVec2(-s * 0.6f, -s), c + ImVec2(-s * 0.6f, s), c + ImVec2(s * 0.8f, 0), col);
      }
    }
    x += font_size;
    dl->AddText(ImVec2(x, top + (row_h - font_size) * 0.5f), col, row.name.c_str());
  }
  dl->PopClipRect();
  if (hovered && over_names && io.MousePos.y >= rows_top) {
    const int r = row_at(io.MousePos.y);
    if (r >= 0 && rows[r].layer && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s\nClick to make it the active layer; the arrow folds it", rows[r].name.c_str());
    }
  }

  if (boxing) {
    dl->AddRectFilled(g_press.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
    dl->AddRect(g_press.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
  }
  if (!transform_info.empty()) {
    dl->AddText(ImVec2(k0.x + 8.0f, p0.y + ruler_h + 4.0f), IM_COL32_WHITE, transform_info.c_str());
  }

  /* Playhead: a blue line with the frame number in a box on the ruler; red on the setup frame (0),
   * where the timeline is edged red as the viewport is. */
  const bool setup = doc.frame_current == 0;
  const ImU32 playhead = setup ? COL_RECORD : COL_PLAYHEAD;
  if (setup) {
    dl->AddRect(p0 + ImVec2(1, 1), p1 - ImVec2(1, 1), COL_RECORD, 0.0f, 0, 2.0f);
  }
  const float x = frame_x(float(doc.frame_current));
  if (x >= k0.x - 40.0f && x <= p1.x + 40.0f) {
    dl->PushClipRect(k0, p1, true);
    dl->AddLine(ImVec2(x, p0.y + ruler_h * 0.5f), ImVec2(x, p1.y), playhead, 2.0f);
    const std::string label = std::to_string(doc.frame_current);
    const float box_w = std::max(ImGui::CalcTextSize(label.c_str()).x + font_size * 0.8f, ruler_h);
    const ImVec2 box_min(x - box_w * 0.5f, p0.y + 2.0f);
    const ImVec2 box_max(x + box_w * 0.5f, p0.y + ruler_h - 2.0f);
    dl->AddRectFilled(box_min, box_max, playhead, ImGui::GetStyle().FrameRounding);
    const float tw = ImGui::CalcTextSize(label.c_str()).x;
    dl->AddText(ImVec2(x - tw * 0.5f, p0.y + (ruler_h - font_size) * 0.5f), IM_COL32_WHITE, label.c_str());
    dl->PopClipRect();
  }
}

void timeline_draw(Area &area, Context &C)
{
  timeline_header(area, C);
  timeline_body(area, C);
}

}  // namespace sky
