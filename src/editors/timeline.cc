/* Timeline editor, laid out like Blender's: a header with menus, playback buttons and the
 * frame fields, then a ruler with frame numbers, the playhead and the frame range. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "anim/animation.hh"
#include "anim/controller.hh"
#include "anim/draw_order.hh"
#include "anim/switch.hh"
#include "anim/motion_path.hh"
#include "anim/onion.hh"
#include "assets/asset_library.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "editors/editors.hh"
#include "editors/timeline_media.hh"
#include "editors/timeline_path.hh"
#include "editors/ui.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/file_drop.hh"
#include "wm/keymap.hh"

namespace oanim {

/* Colours from Blender's default timeline (dope sheet) theme. */
static constexpr ImU32 COL_ICON = IM_COL32(0xe6, 0xe6, 0xe6, 255);
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
/* Blender's header buttons (the default theme's Regular widgets): grey, lighter under the mouse, blue
 * when on; a menu's arrow half darker; outlined, with a darker line under each group. */
static constexpr ImU32 COL_WIDGET = IM_COL32(0x54, 0x54, 0x54, 255);
static constexpr ImU32 COL_WIDGET_HOVER = IM_COL32(0x65, 0x65, 0x65, 255);
static constexpr ImU32 COL_WIDGET_PRESS = IM_COL32(0x47, 0x72, 0xb3, 255);
static constexpr ImU32 COL_WIDGET_MENU = IM_COL32(0x28, 0x28, 0x28, 255);
static constexpr ImU32 COL_WIDGET_MENU_HOVER = IM_COL32(0x33, 0x33, 0x33, 255);
static constexpr ImU32 COL_WIDGET_OUTLINE = IM_COL32(0x3d, 0x3d, 0x3d, 255);
static constexpr ImU32 COL_WIDGET_SHADOW = IM_COL32(0x29, 0x29, 0x29, 255);
/* Editing an action instead of the mainline: Blender's orange for "not the main data". */
static constexpr ImU32 COL_ACTION_ACTIVE = IM_COL32(0x8a, 0x5a, 0x1e, 255);
static constexpr ImU32 COL_FIX_ACTIVE = UI_FIX_ACTIVE_COLOR;
static constexpr ImU32 COL_FIX_TEXT = UI_FIX_COLOR;
static constexpr ImU32 COL_BROKEN = UI_BROKEN_COLOR;
static constexpr ImU32 COL_ACTION_TINT = IM_COL32(0xd0, 0x8a, 0x2e, 0x14);

enum class Icon { Record, JumpStart, KeyPrev, PlayReverse, Play, Pause, KeyNext, JumpEnd, Stepped, Menu, Onion };

/* Where a button sits in a row of joined buttons (Blender's aligned buttons): only the row's outer
 * corners are round. */
enum class Joined { Alone, First, Middle, Last };

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
      /* Blender's: a red dot in a light ring, on or off (the button turns blue when on). */
      (void)on;
      dl->AddCircleFilled(c, s * 0.78f, IM_COL32(0xd8, 0xdc, 0xe4, 255));
      dl->AddCircleFilled(c, s * 0.58f, COL_RECORD);
      break;
    case Icon::Menu: {
      /* The small down arrow of a menu button. */
      const float w = s * 0.55f;
      const ImVec2 pts[3] = {c + ImVec2(-w, -w * 0.45f), c + ImVec2(0.0f, w * 0.55f), c + ImVec2(w, -w * 0.45f)};
      dl->AddPolyline(pts, 3, col, 0, std::max(1.2f, s * 0.22f));
      break;
    }
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
    case Icon::Onion: {
      /* Two drawings, the one behind faint: a frame seen through the one before it. */
      (void)on;
      dl->AddCircle(c + ImVec2(-s * 0.38f, 0.0f), s * 0.62f, (col & 0x00ffffffu) | 0x80000000u, 0, 1.4f);
      dl->AddCircleFilled(c + ImVec2(s * 0.32f, 0.0f), s * 0.62f, col);
      break;
    }
    case Icon::Stepped: {
      /* Stairs: the motion held from key to key. */
      (void)on;
      const ImVec2 pts[7] = {c + ImVec2(-s, s),         c + ImVec2(-s * 0.33f, s),  c + ImVec2(-s * 0.33f, 0.0f),
                             c + ImVec2(s * 0.33f, 0.0f), c + ImVec2(s * 0.33f, -s), c + ImVec2(s, -s),
                             c + ImVec2(s, -s)};
      dl->AddPolyline(pts, 7, col, 0, 1.6f);
      break;
    }
  }
}

/* A Blender header button: its fill (grey, blue when on; a menu's arrow half darker), its outline, and
 * the darker line under it, round only at the outer ends of the row it is `joined` in. */
static void widget_draw(ImDrawList *dl, ImVec2 a, ImVec2 b, Joined joined, bool on, bool hovered, bool held,
                        bool menu = false)
{
  const float r = ImGui::GetStyle().FrameRounding;
  const ImDrawFlags corners = joined == Joined::Alone   ? ImDrawFlags_RoundCornersAll
                              : joined == Joined::First ? ImDrawFlags_RoundCornersLeft
                              : joined == Joined::Last  ? ImDrawFlags_RoundCornersRight
                                                        : ImDrawFlags_RoundCornersNone;
  ImU32 fill = menu ? COL_WIDGET_MENU : COL_WIDGET;
  if (on || held) {
    fill = COL_WIDGET_PRESS;
  }
  else if (hovered) {
    fill = menu ? COL_WIDGET_MENU_HOVER : COL_WIDGET_HOVER;
  }
  dl->AddRectFilled(a + ImVec2(0.0f, 1.0f), b + ImVec2(0.0f, 1.0f), COL_WIDGET_SHADOW, r, corners);
  dl->AddRectFilled(a, b, fill, r, corners);
  dl->AddRect(a, b, COL_WIDGET_OUTLINE, r, corners, 1.0f);
}

static bool icon_button(const char *id, Icon icon, bool on, const char *tooltip, const std::string &shortcut,
                        Joined joined = Joined::Alone, float width = 0.0f)
{
  const float h = ImGui::GetFrameHeight();
  const float w = width > 0.0f ? width : h;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  widget_draw(dl, p, p + ImVec2(w, h), joined, on && icon != Icon::Menu, ImGui::IsItemHovered(),
              ImGui::IsItemActive(), icon == Icon::Menu);
  icon_draw(dl, icon, p + ImVec2(w * 0.5f, h * 0.5f), h * 0.3f, on);
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
static void operator_icon_button(Context &C, const char *id, Icon icon, const char *idname,
                                 Joined joined = Joined::Alone)
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return;
  }
  if (icon_button(id, icon, false, ot->name.c_str(), keymaps().shortcut_for(idname), joined)) {
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
  ui_drag_int(id, value, 0.25f, 0, 1048574, format, ImGuiSliderFlags_AlwaysClamp);
  if (undo_name && ImGui::IsItemDeactivatedAfterEdit() && C.undo) {
    C.undo->push(undo_name, *C.document);
  }
}

/* Which timeline is edited: the mainline, or an action of the bone layer the active layer is in or of
 * one around it, like Moho's switch between the mainline and an action. The + next to it adds an action.
 *
 * The list, top to bottom: what the moment calls for first (the selected bone's action bone actions; the action bone
 * actions playing together now, with the fix for them or a new one), then Mainline, Actions, Action Bone
 * Actions and Fix Actions. Right-click an action to rename or delete it. */
static void action_menu(Context &C, float width)
{
  Document &doc = *C.document;
  const bool in_action = !doc.action.empty();
  Layer *owner = in_action ? layer_find(doc, doc.action_layer) : action_owner(doc, doc.active_layer);
  if (in_action && !owner) {
    owner = action_owner(doc, doc.active_layer);
  }
  const FixAction *editing_fix = in_action && owner ? fix_action_find(*owner, doc.action) : nullptr;
  const std::string preview = !in_action ? "Mainline" : editing_fix ? "Fix: " + doc.action : doc.action;

  if (in_action) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, editing_fix ? COL_FIX_ACTIVE : COL_ACTION_ACTIVE);
  }
  /* A fix's name is its drivers': wider, to read whole. */
  if (editing_fix) {
    const float h = ImGui::GetFrameHeight();
    width = std::max(width, std::min(ImGui::CalcTextSize(preview.c_str()).x + h * 2.5f, h * 14.0f));
  }
  const float plus_w = owner ? ImGui::GetFrameHeight() : 0.0f;
  ImGui::SetNextItemWidth(width - plus_w - (owner ? 2.0f : 0.0f));
  const bool open = ImGui::BeginCombo("##action", preview.c_str(), ImGuiComboFlags_HeightLarge);
  if (in_action) {
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    if (editing_fix) {
      std::string drivers;
      for (const std::string &d : editing_fix->drivers) {
        drivers += (drivers.empty() ? "" : ", ") + d;
      }
      const int held = fix_pose_on_frame(*editing_fix, doc.frame_current);
      const std::string where = held >= 0 ? "Pose " + std::to_string(held + 1) + ": " +
                                                fix_pose_label(*owner, *editing_fix, editing_fix->poses[size_t(held)]) +
                                                ". They're held there: key what should change"
                                          : "This frame isn't one of its poses: keys here don't play. Its poses are "
                                            "marked on the ruler";
      ImGui::SetTooltip("Editing the fix for %s.\n%s.\n\nPython: oanim.data.document.action", drivers.c_str(),
                        where.c_str());
    }
    else {
      ImGui::SetTooltip("Timeline being edited: the mainline, or an action of the bone layer you're in or one around it "
                        "(a separate animation for it and everything inside)\n\nPython: oanim.data.document.action");
    }
  }
  /* Right-click Rename: the action, and the name being typed. */
  static std::string renaming, rename_to;
  static uint64_t renaming_layer = 0;
  if (open) {
    std::string doomed;
    uint64_t doomed_layer = 0;
    const auto action_item = [&](const Layer &rig, const std::string &name, const std::string &label, const std::string &right,
                                 ImU32 right_col) {
      ImGui::PushID(int(rig.id));
      ImGui::PushID(name.c_str());
      const bool here = in_action && doc.action == name && doc.action_layer == rig.id;
      if (ImGui::Selectable(label.c_str(), here)) {
        operator_call(C, "anim.action_set", {{"name", name}, {"layer", std::to_string(rig.id)}});
      }
      if (!right.empty()) {
        const float rw = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - rw));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(right_col), "%s", right.c_str());
      }
      /* Right-click: the action's own menu. */
      if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##action_menu");
      }
      if (ui_menu_begin("##action_menu")) {
        /* A controller's own actions aren't named after anything. */
        const bool is_action_bone_action = controller_dial_of_action(rig, name) != nullptr;
        if (ui_menu_item("Rename...", nullptr, false, !is_action_bone_action)) {
          renaming = name;
          renaming_layer = rig.id;
          rename_to = name;
        }
        if (is_action_bone_action && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip("An action bone action is named after its bone: rename the bone (its fixes follow)");
        }
        if (ui_menu_item("Delete")) {
          doomed = name;
          doomed_layer = rig.id;
        }
        ui_menu_end();
      }
      ImGui::PopID();
      ImGui::PopID();
    };
    const auto percent = [](float share) { return std::to_string(int(std::lround(share * 100.0f))) + "%"; };
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    /* At the top, with one bone selected: its action bone actions, and a new one while it has fewer
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
        if (action_bone_of(*owner, name) == single) {
          /* A 360 action bone action (its bone keyed a whole turn) goes round and round: say so. */
          action_item(*owner, name, name, action_bone_action_full_turn(*owner, name) ? "360Â°" : "", dim);
          count++;
        }
      }
      if (count < 2) {
        if (ImGui::Selectable(count == 0 ? "+ New Bone Action" : "+ New Bone Action (other way)")) {
          operator_call(C, "anim.action_new", {{"name", single->name}, {"action_bone", "1"}, {"layer", std::to_string(owner->id)}});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("An action named after the bone: key the bone turning, and what it should move. Then "
                            "turning the bone anywhere else plays the action that far (Moho's smart bones). A "
                            "second one covers turning the other way");
        }
      }
      ImGui::PopID();
    }

    /* Action bone actions playing together now (each turned at least 5%): fixing this pose, in the fix for all of
     * them or one for some of them. Not while one is edited (an action bone action shows alone; a fix holds its
     * drivers). */
    if (owner && (!in_action || !action_adds(*owner, doc.action)) && doc.frame_current != 0) {
      const std::vector<std::pair<std::string, float>> counting = action_bone_actions_counting(*owner);
      if (counting.size() >= 2) {
        ImGui::PushID("playing");
        ImGui::SeparatorText("Playing now");
        std::string line;
        for (const auto &[name, t] : counting) {
          const float length = action_bone_action_length(*owner, name);
          line += (line.empty() ? "" : "   \xC2\xB7   ") + name + " " + percent(length > 0.0f ? std::min(t / length, 1.0f) : 0.0f);
        }
        ImGui::Indent();
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::Unindent();
        std::vector<std::string> names;
        for (const auto &[name, t] : counting) {
          names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        /* One item per fix: for all of them first, then those for some of them. */
        const auto fix_item = [&](const FixAction *fix, const std::vector<std::string> &drivers, bool all) {
          std::string joined;
          for (const std::string &d : drivers) {
            joined += (joined.empty() ? "" : ",") + d;
          }
          const int near_pose = fix ? fix_pose_near(*owner, *fix, counting) : -1;
          const std::string only = all ? "" : "Only " + fix->name + ": ";
          const std::string label = !fix       ? "+ New Fix Action for This Pose"
                                    : near_pose >= 0 ? only + (all ? "Edit Fix: " + fix->name + "  \xC2\xB7  " : "edit ") + "pose " + std::to_string(near_pose + 1)
                                                : (all ? "+ Add This Pose to Fix: " + fix->name : "+ " + only + "add this pose");
          ImGui::PushID(joined.c_str());
          if (ImGui::Selectable(label.c_str())) {
            operator_call(C, "anim.fix_action_new", {{"drivers", joined}});
          }
          if (fix) {
            const float strength = fix_action_strength(*owner, *fix, counting);
            const std::string right = percent(strength) + " now";
            const float rw = ImGui::CalcTextSize(right.c_str()).x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - rw));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_FIX_TEXT), "%s", right.c_str());
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            if (!fix) {
              ImGui::SetTooltip("These action bones together don't look right? Make a fix for them: it opens with them "
                                "held right here, to key the correction, and then plays by itself whenever they play "
                                "together. Fix more poses of theirs in the same fix later");
            }
            else if (near_pose >= 0) {
              ImGui::SetTooltip("The fix has this pose already (pose %d): open it to change the correction", near_pose + 1);
            }
            else {
              ImGui::SetTooltip(all ? "Add this pose to the fix, starting from what it plays here now, and key the "
                                      "correction. Each pose is exact; between them they blend"
                                    : "Fix this pose in the fix for these action bones only, leaving the others out "
                                      "(when they aren't what looks wrong)");
            }
          }
          ImGui::PopID();
        };
        const FixAction *for_all = nullptr;
        for (const FixAction &f : owner->fix_actions) {
          std::vector<std::string> d = f.drivers;
          std::sort(d.begin(), d.end());
          for_all = d == names ? &f : for_all;
        }
        fix_item(for_all, names, true);
        for (const FixAction &f : owner->fix_actions) {
          const bool inside = &f != for_all && f.drivers.size() >= 2 && fix_action_broken(*owner, f).empty() &&
                              std::all_of(f.drivers.begin(), f.drivers.end(), [&](const std::string &d) {
                                return std::find(names.begin(), names.end(), d) != names.end();
                              });
          if (inside) {
            fix_item(&f, f.drivers, false);
          }
        }
        ImGui::PopID();
      }
    }

    ImGui::Separator();
    if (ImGui::Selectable("Mainline", !in_action)) {
      operator_call(C, "anim.action_set", {{"name", ""}});
    }
    /* The bone layer it's in, and those around it: their actions animate this one too. */
    std::vector<Layer *> rigs = action_owners(doc, doc.active_layer);
    if (owner && std::find(rigs.begin(), rigs.end(), owner) == rigs.end()) {
      rigs.insert(rigs.begin(), owner);
    }
    for (Layer *rig : rigs) {
      if (rig->actions.empty()) {
        continue;
      }
      const std::string above = rig == rigs.front() ? "" : rig->name + ": ";
      const auto section = [&](const char *title, auto &&wanted, auto &&item) {
        bool any = false;
        for (const std::string &name : rig->actions) {
          if (!wanted(name)) {
            continue;
          }
          if (!any) {
            ImGui::SeparatorText((above + title).c_str());
            any = true;
          }
          item(name);
        }
      };
      const auto plain = [&](const std::string &name) { action_item(*rig, name, name, "", 0); };
      section("Actions", [&](const std::string &n) { return !action_adds(*rig, n); }, plain);
      section("Action Bone Actions", [&](const std::string &n) { return action_bone_of(*rig, n) != nullptr; },
              [&](const std::string &name) {
                action_item(*rig, name, name, action_bone_action_full_turn(*rig, name) ? "360Â°" : "", dim);
              });
      const std::vector<std::pair<std::string, float>> playing = action_bone_actions_playing(*rig);
      section("Fix Actions", [&](const std::string &n) { return fix_action_find(*rig, n) != nullptr; },
              [&](const std::string &name) {
                const FixAction &f = *fix_action_find(*rig, name);
                const std::string broken = fix_action_broken(*rig, f);
                const float strength = broken.empty() ? fix_action_strength(*rig, f, playing) : 0.0f;
                action_item(*rig, name, name, !broken.empty() ? "broken" : strength > 0.0f ? percent(strength) : "",
                            !broken.empty() ? COL_BROKEN : strength > 0.0f ? COL_FIX_TEXT : dim);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                  std::string drivers;
                  for (const std::string &d : f.drivers) {
                    drivers += (drivers.empty() ? "" : " and ") + d;
                  }
                  ImGui::SetTooltip(broken.empty() ? "Fixes %s playing together (%s now)"
                                                   : "Can't play: %s. Its keys are kept",
                                    broken.empty() ? drivers.c_str() : broken.c_str(), percent(strength).c_str());
                }
              });
    }
    if (owner && !single && (in_action || action_bone_actions_playing(*owner).size() < 2)) {
      ImGui::Separator();
      ImGui::TextDisabled("Select one bone for its actions.\nTurn two action bones together to fix them.");
    }
    else if (!owner) {
      ImGui::Separator();
      ImGui::TextDisabled("Select a bone layer, or a layer in one,\nto add and edit its actions.");
    }
    /* After drawing the list, which the deletion changes. */
    if (!doomed.empty()) {
      operator_call(C, "anim.action_delete", {{"name", doomed}, {"layer", std::to_string(doomed_layer)}});
    }
    ImGui::EndCombo();
  }
  if (!renaming.empty()) {
    ImGui::OpenPopup("##rename_action");
  }
  if (ImGui::BeginPopup("##rename_action")) {
    ImGui::TextUnformatted("Rename Action");
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool entered = ImGui::InputText("##rename_to", &rename_to,
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if ((entered || ImGui::Button("Rename")) && !rename_to.empty()) {
      operator_call(C, "anim.action_rename", {{"name", renaming}, {"to", rename_to}, {"layer", std::to_string(renaming_layer)}});
      renaming.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  else {
    renaming.clear();
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
      operator_call(C, "anim.action_new", {{"name", new_name}, {"layer", std::to_string(owner->id)}});
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

/* --- Interpolation: the header's mode menu (also T, and right-click > Interpolation), its
 * settings, and the shape each key takes by its mode. ------------------------------------------ */

/* The modes the menus offer, in groups (Bezier comes from dragging handles, Pose from actions). */
struct InterpGroup {
  const char *heading;
  Interp modes[3];
  int count;
};
static constexpr InterpGroup INTERP_GROUPS[] = {
    {nullptr, {Interp::Smooth, Interp::Linear, Interp::Step}, 3},
    {"Ease", {Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut}, 3},
    {"Bouncy", {Interp::Back, Interp::Bounce, Interp::Elastic}, 3},
    {"Special", {Interp::Noisy, Interp::Cycle, Interp::Cycle}, 2},
};

static const char *interp_tooltip(Interp interp)
{
  switch (interp) {
    case Interp::Smooth:
      return "Eases through each key and never goes past it (the default)";
    case Interp::Linear:
      return "Steady speed, turning sharply at each key";
    case Interp::Step:
      return "Holds the key before, then jumps to this one";
    case Interp::EaseIn:
      return "Slows down into this key";
    case Interp::EaseOut:
      return "Starts slowly from the key before";
    case Interp::EaseInOut:
      return "Starts slowly and slows down into this key";
    case Interp::Back:
      return "Goes past this key, then settles back onto it";
    case Interp::Bounce:
      return "Bounces as it lands on this key";
    case Interp::Elastic:
      return "Wobbles about this key like a spring";
    case Interp::Noisy:
      return "Jitters about the straight path to this key";
    case Interp::Cycle:
      return "After this key, plays again from an earlier frame, looping until the next key";
    default:
      return "";
  }
}

/* A mode's curve in the box a..b: progress up, time across, with room for overshoot. */
static void curve_draw(ImDrawList *dl, ImVec2 a, ImVec2 b, const KeySettings &s, ImU32 col, float thickness,
                       float lo = -0.3f, float hi = 1.3f)
{
  constexpr int N = 48;
  ImVec2 pts[N + 1];
  for (int i = 0; i <= N; i++) {
    const float t = float(i) / float(N);
    const float v = std::clamp(interp_curve(s, t), lo, hi);
    pts[i] = ImVec2(a.x + t * (b.x - a.x), b.y - (v - lo) / (hi - lo) * (b.y - a.y));
  }
  dl->AddPolyline(pts, N + 1, col, 0, thickness);
}

/* Spaces before a name in a menu or list, leaving room for its curve. */
static std::string curve_room()
{
  const float h = ImGui::GetFrameHeight();
  const float space = std::max(1.0f, ImGui::CalcTextSize(" ").x);
  return std::string(size_t(std::ceil((h * 1.5f + ImGui::GetFontSize() * 0.6f) / space)), ' ');
}

/* The settings popover was asked for from a menu (T, right-click) or a script: it opens at the
 * mouse on the header's next turn. */
static bool g_settings_request = false;

/* A menu item (or, with `selectable`, a list entry) with a mode's curve before its name. */
static bool interp_item(const char *name, const KeySettings &shape, bool selected, bool enabled, const char *tooltip,
                        bool selectable = false)
{
  const std::string label = curve_room() + name;
  const bool chosen = selectable ? ImGui::Selectable(label.c_str(), selected)
                                 : ui_menu_item(label.c_str(), nullptr, selected, enabled);
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const float h = b.y - a.y;
  const float x = a.x + (selectable ? 2.0f : ImGui::GetStyle().ItemSpacing.x * 0.5f);
  curve_draw(ImGui::GetWindowDrawList(), ImVec2(x, a.y + 2.0f), ImVec2(x + h * 1.5f, b.y - 2.0f), shape,
             enabled ? IM_COL32(0x9c, 0xc6, 0xff, 255) : IM_COL32(0x80, 0x80, 0x80, 255), 1.5f);
  if (tooltip && tooltip[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return chosen;
}

/* What the menus and the header show: the selected keys' settings, or with none selected the
 * new-key settings. */
static KeySettings interp_shown(const SelectedKeys &sel)
{
  return sel.count > 0 ? sel.first : new_key_settings().settings;
}

/* The Interpolation menu's items. `prepare` (right-click on a key) selects the keys clicked
 * before anything is set. */
static void interp_menu_items(Context &C, const std::function<void()> &prepare)
{
  const SelectedKeys sel = layers_selected_keys(timeline_layers(C));
  const KeySettings current = interp_shown(sel);
  const bool for_keys = prepare || sel.count > 0;
  const auto run = [&](const char *idname, OpArgs args = {}) {
    if (prepare) {
      prepare();
    }
    operator_call(C, idname, args);
  };
  if (!prepare) {
    if (sel.count > 0) {
      ImGui::TextDisabled("%d selected key%s", sel.count, sel.count == 1 ? "" : "s");
    }
    else {
      ImGui::TextDisabled("New keys (no keys selected)");
    }
  }
  for (const InterpGroup &group : INTERP_GROUPS) {
    if (group.heading) {
      ImGui::SeparatorText(group.heading);
    }
    else if (!prepare) {
      ImGui::Separator();
    }
    for (int i = 0; i < group.count; i++) {
      const Interp mode = group.modes[i];
      KeySettings shape = current;
      shape.interp = mode;
      if (interp_item(interp_name(mode), shape, !sel.mixed && current.interp == mode, true, interp_tooltip(mode))) {
        run("anim.keys_interpolation_set", {{"interp", interp_identifier(mode)}});
      }
    }
    if (group.modes[0] == Interp::EaseIn) {
      const std::string label = curve_room() + "Strength";
      if (ui_submenu_begin(label.c_str())) {
        for (int e = 0; e < EASE_COUNT; e++) {
          KeySettings shape = current;
          if (shape.interp != Interp::EaseIn && shape.interp != Interp::EaseOut) {
            shape.interp = Interp::EaseInOut;
          }
          shape.ease = Ease(e);
          if (interp_item(ease_name(Ease(e)), shape, !sel.mixed_ease && current.ease == Ease(e), true,
                          "How strongly the Ease modes slow down"))
          {
            run("anim.keys_interpolation_set", {{"ease", ease_identifier(Ease(e))}});
          }
        }
        ui_submenu_end();
      }
    }
  }
  ImGui::Separator();
  if (ui_menu_item("Copy Previous Key", nullptr, new_key_settings().copy_previous)) {
    operator_call(C, "anim.interpolation_copy_previous");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("New keys copy the interpolation of the key before them, instead of the new-key mode");
  }
  ImGui::Separator();
  if (ui_menu_item("Copy Interpolation", nullptr, false, for_keys)) {
    run("anim.interpolation_copy");
  }
  if (ui_menu_item("Paste Interpolation", nullptr, false, for_keys)) {
    run("anim.interpolation_paste");
  }
  if (ui_menu_item("Use for New Keys", nullptr, false, for_keys)) {
    run("anim.interpolation_use_for_new_keys");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("New keys get this key's interpolation and settings");
  }
  ImGui::Separator();
  if (ui_menu_item("Settings...")) {
    if (prepare) {
      prepare();
    }
    g_settings_request = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("The curve and the mode's own settings: strength, overshoot, hold, animating on 2s");
  }
}

/* A sliders icon: the settings button beside the mode. */
static void settings_icon(ImDrawList *dl, ImVec2 c, float s, ImU32 col)
{
  for (int i = -1; i <= 1; i++) {
    const float y = c.y + float(i) * s * 0.7f;
    dl->AddLine(ImVec2(c.x - s, y), ImVec2(c.x + s, y), col, 1.2f);
    const float kx = c.x + (i == 0 ? s * 0.4f : -s * 0.35f);
    dl->AddCircleFilled(ImVec2(kx, y), s * 0.28f, col);
  }
}

/* The settings popover: the curve, the mode's own settings, hold and interval. Drags change the
 * keys (or the new-key settings) as they go; letting go makes one undo step. */
static void interp_settings_popup(Context &C)
{
  if (!ImGui::BeginPopup("##interp_settings")) {
    return;
  }
  Document &doc = *C.document;
  const std::vector<Layer *> layers = timeline_layers(C);
  const SelectedKeys sel = layers_selected_keys(layers);
  const bool keys = sel.count > 0;
  const KeySettings s = interp_shown(sel);
  const float fs = ImGui::GetFontSize();
  const float w = fs * 14.0f;

  if (keys) {
    ImGui::Text("Interpolation of %d key%s", sel.count, sel.count == 1 ? "" : "s");
    if (sel.mixed) {
      ImGui::TextDisabled("Mixed: showing the first key's; a change goes to all");
    }
  }
  else {
    ImGui::Text("Interpolation of New Keys");
    ImGui::TextDisabled("No keys selected");
  }

  /* The curve: progress (0 at the key before, 1 at this one) over the time between them. */
  {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 size(w, fs * 7.0f);
    ImGui::Dummy(size);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 b = a + size;
    dl->AddRectFilled(a, b, IM_COL32(0x1d, 0x1d, 0x1d, 255), 3.0f);
    /* Tall enough for the whole curve (Elastic and Back swing past this key). */
    float lo = -0.15f, hi = 1.15f;
    for (int i = 0; i <= 64; i++) {
      const float v = interp_curve(s, float(i) / 64.0f);
      lo = std::min(lo, v - 0.08f);
      hi = std::max(hi, v + 0.08f);
    }
    /* The lines of the key before's value (0) and this key's (1), labelled above, at the right. */
    for (const float v : {0.0f, 1.0f}) {
      const float y = b.y - (v - lo) / (hi - lo) * size.y;
      dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(0x44, 0x44, 0x44, 255));
      const char *label = v == 0.0f ? "key before" : "this key";
      const float tw = ImGui::CalcTextSize(label).x;
      dl->AddText(ImVec2(b.x - tw - 4.0f, y - fs - 1.0f), IM_COL32(0x80, 0x80, 0x80, 255),
                  label);
    }
    curve_draw(dl, a + ImVec2(6.0f, 0.0f), b - ImVec2(6.0f, 0.0f), s, IM_COL32(0x9c, 0xc6, 0xff, 255), 2.0f, lo, hi);
  }

  const auto set = [&](const char *name, const std::string &value) {
    operator_call(C, "anim.keys_interpolation_set", {{name, value}});
  };
  const auto live = [&](const std::function<void(KeySettings &)> &change) {
    animation_record(doc);
    if (keys) {
      layers_change_selected_keys(layers, change);
      animation_evaluate(doc, doc.frame_current);
    }
    else {
      change(new_key_settings().settings);
    }
  };
  const auto done = [&]() {
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      if (keys) {
        C.undo->push("Interpolation Settings", doc);
      }
      else {
        preferences_save();
      }
    }
  };
  const float label_w = fs * 5.5f;
  const auto row = [&](const char *label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(w - label_w + ImGui::GetStyle().WindowPadding.x * 0.0f);
  };

  row("Mode");
  if (ImGui::BeginCombo("##mode", keys && sel.mixed ? "Mixed" : interp_name(s.interp), ImGuiComboFlags_HeightLarge)) {
    for (const InterpGroup &group : INTERP_GROUPS) {
      if (group.heading) {
        ImGui::SeparatorText(group.heading);
      }
      for (int i = 0; i < group.count; i++) {
        KeySettings shape = s;
        shape.interp = group.modes[i];
        if (interp_item(interp_name(group.modes[i]), shape, !sel.mixed && s.interp == group.modes[i], true,
                        interp_tooltip(group.modes[i]), true))
        {
          set("interp", interp_identifier(group.modes[i]));
        }
      }
    }
    ImGui::EndCombo();
  }
  if (s.interp == Interp::EaseIn || s.interp == Interp::EaseOut || s.interp == Interp::EaseInOut) {
    row("Strength");
    if (ImGui::BeginCombo("##strength", keys && sel.mixed_ease ? "Mixed" : ease_name(s.ease))) {
      for (int e = 0; e < EASE_COUNT; e++) {
        KeySettings shape = s;
        shape.ease = Ease(e);
        if (interp_item(ease_name(Ease(e)), shape, s.ease == Ease(e), true, nullptr, true)) {
          set("ease", ease_identifier(Ease(e)));
        }
      }
      ImGui::EndCombo();
    }
  }
  else if (s.interp == Interp::Back) {
    row("Overshoot");
    float v = s.overshoot;
    if (ui_drag_float("##overshoot", &v, 0.02f, 0.0f, 10.0f, "%.2f")) {
      live([v](KeySettings &k) { k.overshoot = v; });
    }
    done();
  }
  else if (s.interp == Interp::Elastic) {
    row("Amplitude");
    float amp = s.elastic_amplitude;
    if (ui_drag_float("##amplitude", &amp, 0.02f, 1.0f, 5.0f, "%.2f")) {
      live([amp](KeySettings &k) { k.elastic_amplitude = amp; });
    }
    done();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How far the first swing goes past this key (1: as far as the move)");
    }
    row("Period");
    float period = s.elastic_period;
    if (ui_drag_float("##period", &period, 0.005f, 0.05f, 1.0f, "%.2f")) {
      live([period](KeySettings &k) { k.elastic_period = period; });
    }
    done();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How long each wobble lasts, as a share of the time between the keys");
    }
  }
  else if (s.interp == Interp::Noisy) {
    row("Amount");
    float amount = s.noise_amplitude;
    if (ui_drag_float("##noise_amount", &amount, 0.005f, 0.0f, 1000.0f, "%.3f")) {
      live([amount](KeySettings &k) { k.noise_amplitude = amount; });
    }
    done();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How far it jitters, in the property's own units");
    }
    row("Wiggles");
    float wiggles = s.noise_frequency;
    if (ui_drag_float("##noise_wiggles", &wiggles, 0.005f, 0.01f, 4.0f, "%.2f per frame")) {
      live([wiggles](KeySettings &k) { k.noise_frequency = wiggles; });
    }
    done();
  }
  else if (s.interp == Interp::Cycle) {
    row("Loop Back");
    int back = s.cycle_back;
    if (ui_drag_int("##cycle_back", &back, 0.2f, 0, 100000, back == 0 ? "to the first key" : "%d frames")) {
      live([back](KeySettings &k) { k.cycle_back = std::max(0, back); });
    }
    done();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How many frames back each loop starts (0: from the first key)");
    }
  }

  ImGui::Separator();
  row("Hold");
  int hold = s.hold;
  if (ui_drag_int("##hold", &hold, 0.2f, 0, 100000, hold == 1 ? "%d frame" : "%d frames")) {
    live([hold](KeySettings &k) { k.hold = std::max(0, hold); });
  }
  done();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Frames it waits at this key before moving on");
  }
  row("Animate On");
  {
    const float bw = (w - label_w - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    for (int on = 1; on <= 3; on++) {
      if (on > 1) {
        ImGui::SameLine();
      }
      const bool active = s.interval == on;
      if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      const std::string label = std::to_string(on) + "s";
      if (ImGui::Button(label.c_str(), ImVec2(bw, 0.0f))) {
        set("interval", std::to_string(on));
      }
      if (active) {
        ImGui::PopStyleColor();
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("On 2s or 3s the motion only moves every second or third frame");
    }
  }

  ImGui::Separator();
  ImGui::BeginDisabled(!keys);
  const float half = (w - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
  if (ImGui::Button("Copy", ImVec2(half, 0.0f))) {
    operator_call(C, "anim.interpolation_copy");
  }
  ImGui::SameLine();
  if (ImGui::Button("Paste", ImVec2(half, 0.0f))) {
    operator_call(C, "anim.interpolation_paste");
  }
  if (ImGui::Button("Use for New Keys", ImVec2(w, 0.0f))) {
    operator_call(C, "anim.interpolation_use_for_new_keys");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("New keys get this key's interpolation and settings");
  }
  ImGui::EndDisabled();
  bool copy_previous = new_key_settings().copy_previous;
  if (ImGui::Checkbox("Copy Previous Key", &copy_previous)) {
    operator_call(C, "anim.interpolation_copy_previous", {{"enable", copy_previous ? "1" : "0"}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("New keys copy the interpolation of the key before them, instead of the new-key mode");
  }
  ImGui::EndPopup();
}

/* The header's mode dropdown and settings button. */
static void interp_header(Context &C, float width)
{
  /* Visiting every channel of a big rig each frame costs tens of milliseconds, so the summary is kept until an
   * edit (key selection goes through operators, which push undo), a click or the shown layers change. */
  static SelectedKeys sel;
  static uint64_t sel_changes = ~uint64_t(0);
  static std::vector<uint64_t> sel_layers;
  const std::vector<Layer *> layers = timeline_layers(C);
  std::vector<uint64_t> ids;
  ids.reserve(layers.size());
  for (const Layer *layer : layers) {
    ids.push_back(layer->id);
  }
  const uint64_t changes = C.undo ? C.undo->change_count() : 0;
  bool clicked = false;
  for (int b = 0; b < ImGuiMouseButton_COUNT; b++) {
    clicked |= ImGui::IsMouseReleased(b);
  }
  if (!C.undo || changes != sel_changes || ids != sel_layers || clicked) {
    sel = layers_selected_keys(layers);
    sel_changes = changes;
    sel_layers = std::move(ids);
  }
  const KeySettings s = interp_shown(sel);
  const char *name = sel.count > 0 && sel.mixed ? "Mixed" : interp_name(s.interp);
  const float h = ImGui::GetFrameHeight();
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float drop_w = width - h - 2.0f;
  ImGui::InvisibleButton("##interp_mode", ImVec2(drop_w, h));
  const bool hovered = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked()) {
    ImGui::OpenPopup("##key_interpolation");
  }
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, p + ImVec2(drop_w, h), ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                    style.FrameRounding);
  KeySettings shape = s;
  if (sel.count > 0 && sel.mixed) {
    shape.interp = Interp::Smooth;
  }
  curve_draw(dl, p + ImVec2(style.FramePadding.x, 3.0f), p + ImVec2(style.FramePadding.x + h * 1.3f, h - 3.0f), shape,
             IM_COL32(0x9c, 0xc6, 0xff, 255), 1.5f);
  dl->PushClipRect(p, p + ImVec2(drop_w - h * 0.7f, h), true);
  dl->AddText(p + ImVec2(style.FramePadding.x * 2.0f + h * 1.3f, style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text),
              name);
  dl->PopClipRect();
  ImGui::RenderArrow(dl, p + ImVec2(drop_w - h * 0.75f, style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text),
                     ImGuiDir_Down, 0.8f);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    if (sel.count > 0) {
      ImGui::SetTooltip("Interpolation of the selected keys: how the motion arrives at each from the key before\nShortcut: T");
    }
    else {
      ImGui::SetTooltip("Interpolation new keys get (no keys selected)\nShortcut: T");
    }
  }
  ImGui::SameLine(0.0f, 2.0f);
  const ImVec2 q = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##interp_settings_button", ImVec2(h, h))) {
    ImGui::OpenPopup("##interp_settings");
  }
  dl->AddRectFilled(q, q + ImVec2(h, h), ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                    style.FrameRounding);
  settings_icon(dl, q + ImVec2(h * 0.5f, h * 0.5f), h * 0.28f, COL_ICON);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Interpolation settings: the curve, strength, overshoot, hold, animating on 2s");
  }
  /* T over the timeline opens the same menu at the mouse (asked from a script with the mouse
   * outside the window, the timeline drawn first opens it); likewise the settings. */
  const bool here =
      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
      !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
  if (C.wm->menu_request == "key_interpolation" && here) {
    ImGui::OpenPopup("##key_interpolation");
    C.wm->menu_request.clear();
  }
  if (C.wm->menu_request == "key_interpolation_settings" && here) {
    g_settings_request = true;
    C.wm->menu_request.clear();
  }
  if (g_settings_request) {
    ImGui::OpenPopup("##interp_settings");
    g_settings_request = false;
  }
  if (ui_menu_begin("##key_interpolation")) {
    interp_menu_items(C, nullptr);
    ui_menu_end();
  }
  interp_settings_popup(C);
}

/* A key in the rows, shaped by its mode's family: diamond Smooth, thin diamond Linear, square
 * Step, circle Ease, star Back/Bounce/Elastic, hexagon Noisy/Cycle. */
static void key_draw(ImDrawList *dl, ImVec2 c, float r, KeyShape shape, ImU32 fill)
{
  const ImU32 edge = IM_COL32(0, 0, 0, 200);
  switch (shape) {
    case KEY_SHAPE_EASE:
      dl->AddCircleFilled(c, r * 0.85f, fill);
      dl->AddCircle(c, r * 0.85f, edge, 0, 1.0f);
      return;
    case KEY_SHAPE_STEP: {
      const float s = r * 0.72f;
      dl->AddRectFilled(c - ImVec2(s, s), c + ImVec2(s, s), fill);
      dl->AddRect(c - ImVec2(s, s), c + ImVec2(s, s), edge, 0.0f, 0, 1.0f);
      return;
    }
    case KEY_SHAPE_BOUNCY: {
      ImVec2 pts[8];
      for (int i = 0; i < 8; i++) {
        const float a = float(i) * PI * 0.25f - PI * 0.5f;
        const float rr = i % 2 == 0 ? r * 1.05f : r * 0.42f;
        pts[i] = c + ImVec2(std::cos(a) * rr, std::sin(a) * rr);
      }
      dl->AddConcavePolyFilled(pts, 8, fill);
      dl->AddPolyline(pts, 8, edge, ImDrawFlags_Closed, 1.0f);
      return;
    }
    case KEY_SHAPE_SPECIAL: {
      ImVec2 pts[6];
      for (int i = 0; i < 6; i++) {
        const float a = float(i) * PI / 3.0f;
        pts[i] = c + ImVec2(std::cos(a) * r * 0.9f, std::sin(a) * r * 0.9f);
      }
      dl->AddConvexPolyFilled(pts, 6, fill);
      dl->AddPolyline(pts, 6, edge, ImDrawFlags_Closed, 1.0f);
      return;
    }
    default:
      break;
  }
  const float wx = shape == KEY_SHAPE_LINEAR ? r * 0.6f : r;
  const ImVec2 d[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + wx, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - wx, c.y)};
  dl->AddConvexPolyFilled(d, 4, fill);
  dl->AddPolyline(d, 4, edge, ImDrawFlags_Closed, 1.0f);
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
  ImVec2 playback_at;
  for (const char *menu : {"View", "Marker", "Playback"}) {
    ImGui::SameLine();
    if (ImGui::Button(menu)) {
      ImGui::OpenPopup(menu);
    }
    playback_at = ImGui::GetItemRectMin();
  }
  ImGui::PopStyleColor();
  ImGui::SameLine(0.0f, pad * 2.0f);
  action_menu(C, h * 6.0f);
  ImGui::SameLine(0.0f, pad * 2.0f);
  interp_header(C, h * 6.5f);
  const float menus_end = ImGui::GetItemRectMax().x;

  if (ImGui::BeginPopup("View")) {
    if (ImGui::MenuItem("Frame All", "Home")) {
      view_frame_all(area, doc);
    }
    if (ImGui::MenuItem("Go to Current Frame")) {
      view_center_current(area, doc);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Group by Owner", nullptr, !area.timeline_by_kind)) {
      area.timeline_by_kind = false;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Rows by whose they are: each layer, its bones, each bone's properties");
    }
    if (ImGui::MenuItem("Group by Kind", nullptr, area.timeline_by_kind)) {
      area.timeline_by_kind = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Rows by what they are: layer transforms, bone angles, bone transforms, constraints");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear Pins")) {
      operator_call(C, "anim.pins_clear");
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
  /* Blender's Playback popover: headings on the left, the settings beside them. It opens against its
   * button, upwards (the timeline is usually at the bottom), and ImGui keeps it on screen. */
  if (ImGui::IsPopupOpen("Playback")) {
    ImGui::SetNextWindowPos(playback_at, ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
  }
  if (ImGui::BeginPopup("Playback")) {
    if (ImGui::BeginTable("##playback", 2, ImGuiTableFlags_SizingStretchProp, ImVec2(h * 11.0f, 0.0f))) {
      ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
      ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
      const rna::PointerRNA ptr{&doc, &Document::rna()};
      const char *path = "oanim.data.document";
      ui_prop(C, ptr, "sync_mode", path);
      ui_prop_check(C, ptr, "use_audio_scrub", path, "Audio");
      ui_prop_check(C, ptr, "play_audio", path);
      ui_row("Playback", [&](float) { ImGui::Checkbox("Loop Playback", &wm.play_loop); });
      ui_row("", [&](float) {
        bool stepped = stepped_preview();
        if (ImGui::Checkbox("Stepped Preview", &stepped)) {
          operator_call(C, "screen.stepped_preview");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("Play everything as Step, holding each key until the next, to judge the timing of the "
                            "poses. No key changes");
        }
      });
      ui_prop_check(C, ptr, "auto_key", path, "Keying");
      ImGui::EndTable();
    }
    ImGui::EndPopup();
  }

  /* Centre: auto-keying, then the playback buttons. */
  const float spacing = std::max(1.0f, style.ItemSpacing.x * 0.35f);
  const float record_gap = h * 0.6f;
  const float keying_arrow_w = h * 0.75f;
  const float onion_gap = h * 0.25f;
  const float transport_w = 9.0f * h + 2.0f * record_gap + 2.0f * keying_arrow_w + onion_gap;
  const float transport_x = std::max(menus_end + pad * 2.0f, origin.x + (width - transport_w) * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(transport_x, origin.y + top));
  (void)spacing;
  /* Buttons in a group touch, as Blender's aligned rows. */
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, style.ItemSpacing.y));
  if (icon_button("##auto_key", Icon::Record, doc.auto_key,
                  "Auto Keying\nEditing at a frame other than 0 keys what you change there", "", Joined::First)) {
    doc.auto_key = !doc.auto_key;
    C.undo->mark_changed();
  }
  /* Blender's auto-keying popover beside the record button: the mode, and Moho's Auto-Freeze Keys. */
  ImGui::SameLine(0.0f, 0.0f);
  const ImVec2 arrow_at = ImGui::GetCursorScreenPos();
  if (icon_button("##auto_key_options", Icon::Menu, false, "Auto Keying options", "", Joined::Last, keying_arrow_w)) {
    ImGui::OpenPopup("##auto_key_popover");
  }
  /* Opens above the timeline's header. */
  ImGui::SetNextWindowPos(ImVec2(arrow_at.x - h * 2.0f, arrow_at.y - 4.0f), ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
  if (ImGui::BeginPopup("##auto_key_popover")) {
    const float seg_w = h * 4.2f;
    for (const auto &[mode, label, tip] :
         {std::tuple{AutoKeyMode::AddReplace, "Add & Replace", "Key whatever changes, adding keys where there are none"},
          std::tuple{AutoKeyMode::Replace, "Replace", "Only change keys already on the frame: a change where there "
                                                       "is no key isn't keyed"}})
    {
      if (mode == AutoKeyMode::Replace) {
        ImGui::SameLine(0.0f, 0.0f);
      }
      const bool on = doc.auto_key_mode == mode;
      if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (ImGui::Button(label, ImVec2(seg_w, 0.0f)) && !on) {
        doc.auto_key_mode = mode;
        C.undo->mark_changed();
      }
      if (on) {
        ImGui::PopStyleColor();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tip);
      }
    }
    ImGui::BeginDisabled(doc.auto_key_mode == AutoKeyMode::Replace);
    if (ImGui::Checkbox("Auto-Freeze Keys", &doc.auto_freeze_keys)) {
      C.undo->mark_changed();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Keying a bone keys every other bone of its bone layer shown there on that frame too, and "
                        "keying a point every point of its drawing, so the whole pose and shape hold there (Moho's). "
                        "Not with Replace, which adds no keys");
    }
    ImGui::EndPopup();
  }
  /* Onion skins beside it, the same way: the toggle and its popover. */
  ImGui::SameLine(0.0f, onion_gap);
  if (icon_button("##onion_skin", Icon::Onion, doc.onion_skin,
                  "Onion Skins\nShow the drawings being edited as they are on the keys (or frames) before this "
                  "one, as coloured wireframes. Never rendered",
                  "", Joined::First)) {
    doc.onion_skin = !doc.onion_skin;
    C.undo->mark_changed();
  }
  ImGui::SameLine(0.0f, 0.0f);
  const ImVec2 onion_arrow_at = ImGui::GetCursorScreenPos();
  if (icon_button("##onion_options", Icon::Menu, false, "Onion Skin options", "", Joined::Last, keying_arrow_w)) {
    ImGui::OpenPopup("##onion_popover");
  }
  ImGui::SetNextWindowPos(ImVec2(onion_arrow_at.x - h * 3.0f, onion_arrow_at.y - 4.0f), ImGuiCond_Appearing,
                          ImVec2(0.0f, 1.0f));
  if (ImGui::BeginPopup("##onion_popover")) {
    ImGui::TextDisabled("Onion Skins");
    const float seg_w = h * 4.2f;
    for (const auto &[mode, label, tip] :
         {std::tuple{OnionMode::Keyframes, "Keyframes", "Step back and forward by the keys of the layers being edited "
                                                         "(and of the bones and groups moving them)"},
          std::tuple{OnionMode::Frames, "Frames", "Step back and forward by frames"}})
    {
      if (mode == OnionMode::Frames) {
        ImGui::SameLine(0.0f, 0.0f);
      }
      const bool on = doc.onion_mode == mode;
      if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (ImGui::Button(label, ImVec2(seg_w, 0.0f)) && !on) {
        doc.onion_mode = mode;
        C.undo->mark_changed();
      }
      if (on) {
        ImGui::PopStyleColor();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tip);
      }
    }
    if (ImGui::BeginTable("##onion", 2, ImGuiTableFlags_SizingStretchProp, ImVec2(seg_w * 2.0f, 0.0f))) {
      ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
      ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
      const rna::PointerRNA ptr{&doc, &Document::rna()};
      const char *path = "oanim.data.document";
      /* Each side's count with its colour. */
      for (const bool after : {false, true}) {
        Color &color = after ? doc.onion_color_after : doc.onion_color_before;
        ui_row(after ? "After" : "Before", [&](float width) {
          const float swatch = h * 1.3f;
          ImGui::SetNextItemWidth(std::max(h, width - swatch - ImGui::GetStyle().ItemInnerSpacing.x));
          int &count = after ? doc.onion_after : doc.onion_before;
          if (ui_drag_int(after ? "##onion_after" : "##onion_before", &count, 0.1f, 0, 20,
                             doc.onion_mode == OnionMode::Frames ? "%d frames" : "%d keys")) {
            count = std::clamp(count, 0, 20);
            C.undo->mark_changed();
          }
          ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
          float rgba[4] = {color.r, color.g, color.b, color.a};
          if (ui_color_edit(after ? "##onion_color_after" : "##onion_color_before", rgba, false, nullptr, 0.0f,
                            swatch)) {
            color = {rgba[0], rgba[1], rgba[2], rgba[3]};
            C.undo->mark_changed();
          }
        });
      }
      ImGui::BeginDisabled(doc.onion_mode != OnionMode::Frames);
      ui_prop(C, ptr, "onion_step", path);
      ImGui::EndDisabled();
      ui_prop_check(C, ptr, "onion_fade", path, "Display");
      ui_prop_check(C, ptr, "onion_all_layers", path);
      ImGui::EndTable();
    }
    /* Motion paths (anim/motion_path.hh): where the selected things go, under the ghosts they go with. */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Motion Paths");
    if (ImGui::BeginTable("##paths", 2, ImGuiTableFlags_SizingStretchProp, ImVec2(seg_w * 2.0f, 0.0f))) {
      ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
      ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
      const rna::PointerRNA ptr{&doc, &Document::rna()};
      const char *path = "oanim.data.document";
      ui_prop_check(C, ptr, "motion_paths", path, "Show");
      ImGui::BeginDisabled(!doc.motion_paths);
      ImGui::BeginDisabled(doc.path_all);
      ui_prop(C, ptr, "path_before", path);
      ui_prop(C, ptr, "path_after", path);
      ImGui::EndDisabled();
      ui_prop_check(C, ptr, "path_all", path, "Range");
      ui_prop(C, ptr, "path_step", path);
      ui_prop_check(C, ptr, "path_lines", path, "Display");
      ui_prop(C, ptr, "path_dot_colors", path);
      ui_prop(C, ptr, "path_relative", path);
      if (doc.path_relative == PathRelative::Parent) {
        ui_row("Parent", [&](float width) {
          const Layer *parent = layer_find(doc, doc.path_relative_layer);
          std::string name = parent ? parent->name : "(none)";
          if (parent && doc.path_relative_bone) {
            for (const Bone &b : parent->bones) {
              if (b.id == doc.path_relative_bone) {
                name += " / " + b.name;
              }
            }
          }
          if (ImGui::Button((name + "##path_parent").c_str(), ImVec2(width, 0.0f))) {
            operator_call(C, "anim.motion_path_relative", {{"to", "PARENT"}});
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Click to use the active layer (and its active bone)");
          }
        });
      }
      ImGui::EndDisabled();
      ImGui::EndTable();
    }
    if (doc.motion_paths) {
      /* Each path traced now: its colour, a pin, and for a bone which end. */
      const std::vector<MotionPathTarget> targets = motion_path_targets(doc, editable_layers(C));
      for (size_t i = 0; i < targets.size() && i < 12; i++) {
        const MotionPathTarget &t = targets[i];
        ImGui::PushID(int(i));
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
        const Color c = motion_path_color(doc, t, i);
        float rgba[4] = {c.r, c.g, c.b, 1.0f};
        bool finished = false;
        if (ui_color_edit("##path_color", rgba, false, &finished, 0.0f, h * 1.3f) && finished) {
          OpArgs set = args;
          set["color"] = std::to_string(rgba[0]) + "," + std::to_string(rgba[1]) + "," + std::to_string(rgba[2]);
          operator_call(C, "anim.motion_path_style", set);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(t.pinned ? "Pinned" : "Pin")) {
          OpArgs set = args;
          set["pinned"] = t.pinned ? "0" : "1";
          operator_call(C, "anim.motion_path_pin", set);
        }
        if (t.bone) {
          ImGui::SameLine();
          if (ImGui::SmallButton(t.head ? "Head" : "Tip")) {
            OpArgs set = args;
            set["head"] = t.head ? "0" : "1";
            operator_call(C, "anim.motion_path_style", set);
          }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(motion_path_label(doc, t).c_str());
        ImGui::PopID();
      }
      if (ImGui::Button(wm.path_snapshot.empty() ? "Snapshot" : "New Snapshot", ImVec2(seg_w, 0.0f))) {
        operator_call(C, "anim.motion_path_snapshot");
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Keep a faint copy of the paths as they are, to compare an edit against");
      }
      ImGui::SameLine(0.0f, 0.0f);
      ImGui::BeginDisabled(wm.path_snapshot.empty());
      if (ImGui::Button("Clear Snapshot", ImVec2(seg_w, 0.0f))) {
        operator_call(C, "anim.motion_path_snapshot", {{"clear", "1"}});
      }
      ImGui::EndDisabled();
      ImGui::TextDisabled("Alt Space plays just the paths' range");
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine(0.0f, record_gap);
  operator_icon_button(C, "##jump_start", Icon::JumpStart, "screen.frame_jump_start", Joined::First);
  ImGui::SameLine();
  operator_icon_button(C, "##key_prev", Icon::KeyPrev, "screen.keyframe_jump_prev", Joined::Middle);
  ImGui::SameLine();
  operator_icon_button(C, "##play_reverse", (wm.playing && wm.play_reverse) ? Icon::Pause : Icon::PlayReverse,
                       "screen.animation_play_reverse", Joined::Middle);
  ImGui::SameLine();
  operator_icon_button(C, "##play", (wm.playing && !wm.play_reverse) ? Icon::Pause : Icon::Play,
                       "screen.animation_play", Joined::Middle);
  ImGui::SameLine();
  operator_icon_button(C, "##key_next", Icon::KeyNext, "screen.keyframe_jump_next", Joined::Middle);
  ImGui::SameLine();
  operator_icon_button(C, "##jump_end", Icon::JumpEnd, "screen.frame_jump_end", Joined::Last);
  ImGui::SameLine(0.0f, record_gap);
  if (icon_button("##stepped_preview", Icon::Stepped, stepped_preview(),
                  "Stepped Preview\nPlay everything as Step, holding each key until the next, to judge the timing "
                  "of the poses. No key changes",
                  ""))
  {
    operator_call(C, "screen.stepped_preview");
  }
  ImGui::PopStyleVar();
  const float transport_end = ImGui::GetItemRectMax().x;

  /* Right: current frame, then the Start and End of the range. */
  const float current_w = h * 2.6f;
  const float range_w = h * 4.0f;
  const float group_gap = h * 0.6f;
  const float fields_w = current_w + group_gap + range_w * 2.0f;
  const float fields_x = std::max(transport_end + pad * 2.0f, origin.x + width - fields_w - pad);
  ImGui::SetCursorScreenPos(ImVec2(fields_x, origin.y + top));
  frame_field(C, "##frame_current", &doc.frame_current, "%d", current_w, nullptr);
  ImGui::SameLine(0.0f, group_gap);
  /* Start and End side by side as one row, as Blender's. */
  frame_field(C, "##frame_start", &doc.frame_start, "Start %d", range_w, "Change Start");
  ImGui::SameLine(0.0f, 0.0f);
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
/* The reference channel a right-click asked to sync (its menu's subject). */
static uint64_t g_sync_layer = 0;
static std::string g_sync_path;

/* The Keyed Here list (Ctrl-click or right-click a key): what a row has keyed on a frame. */
struct KeyedHere {
  std::vector<uint64_t> layers;
  KeyFilter filter;
  int frame = 0;
};
static KeyedHere g_keyed_here;

/* One line of the list: a property (or, for points, all the points keyed with that property, with
 * each one in `singles`), what to show it with, and the channels it stands for. */
struct KeyedEntry {
  uint64_t layer = 0;
  std::string label, value;
  std::vector<std::string> paths;
  std::vector<std::pair<std::string, std::string>> singles;
};

static Layer *keyed_layer(Document &doc, uint64_t id)
{
  Layer &simulation = document_simulation(doc);
  return id == simulation.id ? &simulation : layer_find(doc, id);
}

/* A key's value and interpolation, as the list shows it: "1.250  Smooth", "(0.12, 0.40)  Linear". */
/* The document the timeline shows (for naming Draw Order targets). */
static const Document *g_timeline_doc = nullptr;

static std::string keyed_value(Layer &layer, const std::string &path, int frame)
{
  ChannelRef ref;
  if (!layer_channel(layer, path, &ref)) {
    return "";
  }
  char text[96] = "";
  Interp interp = Interp::Smooth;
  if (ref.scalar) {
    if (const Key<float> *k = ref.scalar->key_at(frame)) {
      const bool turn = path.ends_with("angle") || path.ends_with("rotation") || path.ends_with("direction");
      if (path == "visible") {
        std::snprintf(text, sizeof(text), "%s", k->value > 0.5f ? "On" : "Off");
      }
      else if (path == "draw_order") {
        return g_timeline_doc ? draw_order_label(*g_timeline_doc, k->value) : std::string();
      }
      else if (path == "switch") {
        return g_timeline_doc ? switch_key_label(*g_timeline_doc, layer, k->value) : switch_value_label(layer, k->value);
      }
      else {
        std::snprintf(text, sizeof(text), turn ? "%.1f\xC2\xB0" : "%.3f", turn ? k->value * 180.0f / PI : k->value);
      }
      interp = k->interp;
    }
  }
  else if (const Key<float2> *k = ref.vector->key_at(frame)) {
    std::snprintf(text, sizeof(text), "(%.3f, %.3f)", k->value.x, k->value.y);
    interp = k->interp;
  }
  return std::string(text) + "  " + interp_name(interp);
}

/* The points' kinds of property a drawing keys, grouped in the list. */
static const char *point_kind(const std::string &path, std::string *r_single)
{
  const bool curve = path.rfind("curves[", 0) == 0;
  if (!curve && path.rfind("points[", 0) != 0) {
    return nullptr;
  }
  const size_t dot = path.rfind('.');
  const std::string what = dot == std::string::npos ? path : path.substr(dot + 1);
  const std::string where = dot == std::string::npos ? path : path.substr(0, dot);
  if (r_single) {
    /* "Point 3", "Curve 0, point 2", and which handle. */
    int a = -1, b = -1;
    const size_t open = where.find('['), open2 = where.find('[', open + 1);
    a = open != std::string::npos ? std::atoi(where.c_str() + open + 1) : -1;
    b = open2 != std::string::npos ? std::atoi(where.c_str() + open2 + 1) : -1;
    *r_single = curve ? "Curve " + std::to_string(a) + ", point " + std::to_string(b) : "Point " + std::to_string(a);
    if (what.rfind("handle_in", 0) == 0) {
      *r_single += ": in";
    }
    else if (what.rfind("handle_out", 0) == 0) {
      *r_single += ": out";
    }
  }
  if (what == "pos") {
    return "Points: Position";
  }
  if (what == "radius") {
    return "Points: Width";
  }
  if (what == "offset") {
    return "Points: Offset";
  }
  if (what == "curvature") {
    return "Points: Curvature";
  }
  return "Handles";
}

static std::vector<KeyedEntry> keyed_entries(Document &doc, const KeyedHere &k)
{
  std::vector<Layer *> layers;
  for (const uint64_t id : k.layers) {
    if (Layer *l = keyed_layer(doc, id)) {
      layers.push_back(l);
    }
  }
  std::vector<KeyedEntry> entries;
  std::map<std::pair<uint64_t, std::string>, size_t> groups;
  const auto keyed = layers_keyed_at(layers, k.filter, k.frame);
  std::set<uint64_t> owners;
  for (const auto &[layer, path] : keyed) {
    owners.insert(layer->id);
  }
  for (const auto &[layer, path] : keyed) {
    std::string single;
    if (const char *kind = point_kind(path, &single)) {
      const auto key = std::make_pair(layer->id, std::string(kind));
      auto it = groups.find(key);
      if (it == groups.end()) {
        it = groups.emplace(key, entries.size()).first;
        KeyedEntry e;
        e.layer = layer->id;
        e.label = layer->name + ": " + kind;
        entries.push_back(std::move(e));
      }
      KeyedEntry &e = entries[it->second];
      e.paths.push_back(path);
      e.singles.push_back({single, path});
      continue;
    }
    KeyedEntry e;
    e.layer = layer->id;
    e.label = channel_label(*layer, path, true);
    /* A bone's or the document's own owner name; with several layers, say whose layer too. */
    if (owners.size() > 1 && path.rfind("bones[", 0) == 0) {
      e.label = layer->name + " > " + e.label;
    }
    e.value = keyed_value(*layer, path, k.frame);
    e.paths = {path};
    entries.push_back(std::move(e));
  }
  for (KeyedEntry &e : entries) {
    if (e.singles.size() == 1) {
      e.value = keyed_value(*keyed_layer(doc, e.layer), e.paths.front(), k.frame);
      e.label += " (" + e.singles.front().first + ")";
      e.singles.clear();
    }
  }
  return entries;
}

static std::string joined_paths(const std::vector<std::string> &paths)
{
  std::string joined;
  for (const std::string &p : paths) {
    joined += (joined.empty() ? "" : "|") + p;
  }
  return joined;
}

/* G or S on keys: from a copy of the document, the keys move by the mouse until a click. */
struct KeyTransform {
  bool active = false;
  int area = 0;
  char kind = 'G';
  float start_x = 0.0f;
  Document snapshot;
  /* Shift D: how far the copies are from the keys now. */
  int offset = 0;
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

/* A row of the timeline. Rows fold, as Blender's channels do, down to one row per property, in
   four groups:

     Pinned                  properties pinned to the timeline, whatever is picked (red; with no
                             keys left too), each with its pin to unpin it
     Props                   the settings: a layer's own transform, opacity and visibility, the
                             bones' constraint settings, the document's Simulation settings
     All                     the motion: the bones' angle, position and scale, and the points
     Selected                the motion of the selected bones and points (red)

   Inside Props, All and Selected, View > Group by Kind (the default) lists by what they are (Layers, Constraints, Simulation; Bone Angle and Bone Transform, a row per
   bone: "arm: Angle"); Group by Owner, as Blender's Dope Sheet, lists each layer, its bones and
   each bone's rows ("Angle", "IK: Influence"). A row with no keys under it isn't shown (but a pinned one is, under
   Pinned); a folding row shows the keys of all it holds. The four start open, but the Bones
   inside Selected (and in All) start folded. */
struct ChannelRow {
  std::string name;
  std::vector<Layer *> layers;
  KeyFilter filter;
  int depth = 0;
  /* Key of a folding row's open state, "" when it doesn't fold. */
  std::string fold;
  bool open = false;
  /* Open unless folded (its default, when the project keeps nothing for it). */
  bool fold_default = false;
  /* A copy's (reference layer's) channel changed from its original (drawn teal). */
  bool changed = false;
  /* In the Pinned or Selected sections (drawn red). */
  bool selected = false;
  /* A layer's own row: clicking its name makes that layer active. */
  uint64_t layer = 0;
  /* One pinned property's row: its pin (by its layer and path) unpins it. */
  uint64_t pin_layer = 0;
  std::string pin_path;
  /* A Media layer's clips (its id): bars to move and trim instead of keys, two rows tall (the second
   * row, part 1, is the bars' lower half). */
  uint64_t media = 0;
  int media_part = 0;
};

/* Folding rows open or folded as the project keeps them (saved with it, as panels are). */
static bool row_open(const Area &area, const std::string &key, bool fallback)
{
  (void)area;
  return ui_fold_state("Timeline/" + key, fallback);
}

/* Something selected for the Selected section: a point, or a bone (by body or ball). */
static bool anything_selected(const std::vector<Layer *> &layers, const std::set<uint64_t> &edited)
{
  for (const Layer *l : layers) {
    if (l->id == CAMERA_LAYER_ID && l->selected) {
      return true;
    }
    for (const MeshPoint &p : l->drawing.points) {
      if (p.selected && edited.contains(l->id)) {
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

static bool bone_picked_any(const Bone &b)
{
  return b.selected || b.select_head || b.select_tail;
}

/* `layers`: what the timeline shows. `open_all`: every folding row open (for listing them). */
static std::vector<ChannelRow> channel_rows(const Area &area, const std::vector<Layer *> &layers, Document &doc,
                                            const std::vector<uint64_t> &pinned_layers, bool open_all = false)
{
  /* Lookups by an index while the rows are worked out (they change no layers). */
  const LayerIndexScope layer_index(doc);
  std::vector<ChannelRow> rows;
  /* Selected counts the points selected in the layers being edited: a selection left in another layer
   * isn't one now. */
  const std::shared_ptr<const std::set<uint64_t>> edited = editable_layer_ids(doc);
  const auto now = [&](KeyFilter f) {
    if (f.selected_only) {
      f.point_layers = edited;
    }
    return f;
  };
  const auto keyed = [&](const std::vector<Layer *> &ls, const KeyFilter &f) {
    return !layers_key_summary(ls, now(f)).empty();
  };
  const auto add = [&](std::string name, std::vector<Layer *> ls, KeyFilter f, int depth, std::string fold,
                       bool fold_default, bool red, uint64_t layer) -> bool {
    ChannelRow row;
    row.name = std::move(name);
    row.layers = std::move(ls);
    row.filter = now(std::move(f));
    row.depth = depth;
    row.fold = fold;
    row.open = !fold.empty() && (open_all || row_open(area, fold, fold_default));
    row.fold_default = fold_default;
    row.selected = red;
    row.layer = layer;
    rows.push_back(std::move(row));
    return rows.back().open;
  };
  /* A row per animated property of `l` that passes `base`, named with its owner or not. */
  const auto property_rows = [&](Layer *l, const KeyFilter &base, int depth, bool owner, bool red) {
    /* Points get no row of their own: not listing them spares working out every point's channel. */
    KeyFilter listed = base;
    listed.kinds &= ~uint32_t(KEY_POINTS);
    for (const ChannelRef &ref : layer_filtered_channels(*l, listed)) {
      const bool animated = ref.scalar ? ref.scalar->animated() : ref.vector->animated();
      if (!animated || channel_kind(ref.path) == KEY_POINTS) {
        continue;
      }
      KeyFilter f = base;
      f.path = ref.path;
      add(channel_label(*l, ref.path, owner), {l}, f, depth, "", false, red, 0);
      rows.back().changed = l->is_reference() && l->reference.own_channels.count(ref.path) > 0;
      if (ref.scalar ? ref.scalar->pinned : ref.vector->pinned) {
        rows.back().pin_layer = l->id;
        rows.back().pin_path = ref.path;
      }
    }
  };
  const auto bone_filter = [](const Bone &b, uint32_t kinds, bool selected) {
    return KeyFilter{kinds, selected, "bones[\"" + b.name + "\"].", false};
  };

  /* Pinned: every pinned property in the document, keys or not. */
  std::vector<std::pair<Layer *, std::string>> pinned;
  const KeyFilter pins{KEY_ALL, false, "", true};
  for (const uint64_t id : pinned_layers) {
    Layer *l = layer_find(doc, id);
    if (!l) {
      continue;
    }
    for (const ChannelRef &ref : layer_filtered_channels(*l, pins)) {
      pinned.push_back({l, ref.path});
    }
  }
  for (const ChannelRef &ref : layer_filtered_channels(document_simulation(doc), pins)) {
    pinned.push_back({&doc.simulation, ref.path});
  }
  if (!pinned.empty()) {
    std::vector<Layer *> owners;
    for (const auto &[l, path] : pinned) {
      if (std::find(owners.begin(), owners.end(), l) == owners.end()) {
        owners.push_back(l);
      }
    }
    if (add("Pinned", owners, pins, 0, "pinned", true, true, 0)) {
      for (const auto &[l, path] : pinned) {
        add(channel_label(*l, path, true), {l}, KeyFilter{KEY_ALL, false, path, false}, 1, "", false, true, 0);
        rows.back().pin_layer = l->id;
        rows.back().pin_path = path;
      }
    }
  }
  if (layers.empty()) {
    return rows;
  }
  Layer *sim = nullptr;
  std::vector<Layer *> owned;
  for (Layer *l : layers) {
    (l->id == SIMULATION_LAYER_ID ? sim : owned.emplace_back(l)) = l;
  }
  const KeyFilter sim_kind{KEY_SIMULATION, false};

  /* Props: the settings (a layer's own transform, opacity and visibility, constraints' settings,
   * the document's Simulation); All and Selected: the motion (bones' angle, position and scale,
   * and points). */
  const uint32_t props = KEY_LAYER | KEY_CONSTRAINTS | KEY_SIMULATION;
  const uint32_t bone_motion = KEY_BONE_ANGLE | KEY_BONE_TRANSFORM;
  const uint32_t motion = bone_motion | KEY_POINTS;
  /* Selected: the motion of the selected points and bones, and the picked camera's Position, Zoom and
   * Roll (a layer's own transform is a prop, so only the camera's pass as selected). */
  const KeyFilter selected_motion{motion | KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM, true};
  /* The active layer when it is a Media layer: under Selected, its clips (start and end) and its own
   * keyed channels (where it is, its opacity). */
  Layer *media = nullptr;
  for (Layer *l : owned) {
    if (l->is_media() && l->id == doc.active_layer) {
      media = l;
    }
  }
  const auto media_rows = [&](int depth) {
    if (media) {
      add(media->name + ": Clips", {media}, KeyFilter{0, false}, depth, "", false, true, 0);
      rows.back().media = media->id;
      add("", {media}, KeyFilter{0, false}, depth, "", false, true, 0);
      rows.back().media = media->id;
      rows.back().media_part = 1;
      property_rows(media, KeyFilter{KEY_LAYER, false}, depth, true, true);
    }
  };
  const auto camera_rows = [&](int depth) {
    const KeyFilter camera{KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM, true};
    for (Layer *l : owned) {
      if (l->id == CAMERA_LAYER_ID && keyed({l}, camera) && add("Camera", {l}, camera, depth, "sel:camera", true, true, 0))
      {
        property_rows(l, camera, depth + 1, false, true);
      }
    }
  };

  if (!area.timeline_by_kind) {
    /* Props, by owner: each layer (Transform, Opacity, Visibility, its bones' constraint settings),
     * then Simulation. */
    if (keyed(layers, KeyFilter{props}) && add("Props", layers, KeyFilter{props}, 0, "props", true, false, 0)) {
      for (Layer *l : owned) {
        const KeyFilter own{props & ~KEY_SIMULATION};
        if (!keyed({l}, own)) {
          continue;
        }
        const std::string id = std::to_string(l->id);
        if (!add(l->name, {l}, own, 1, "p:" + id, l->id == doc.active_layer, false, l->id)) {
          continue;
        }
        const KeyFilter transform{KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM, false};
        if (keyed({l}, transform) && add("Transform", {l}, transform, 2, "p:t:" + id, false, false, 0)) {
          property_rows(l, transform, 3, false, false);
        }
        property_rows(l, KeyFilter{KEY_OPACITY | KEY_VISIBILITY | KEY_DRAW_ORDER, false}, 2, false, false);
        const KeyFilter constraints{KEY_CONSTRAINTS, false};
        if (keyed({l}, constraints) && add("Bones", {l}, constraints, 2, "p:b:" + id, false, false, 0)) {
          for (const Bone &b : l->bones) {
            const KeyFilter one = bone_filter(b, KEY_CONSTRAINTS, false);
            if (keyed({l}, one) &&
                add(b.name, {l}, one, 3, "p:bone:" + id + ":" + std::to_string(b.id), false, false, 0))
            {
              property_rows(l, one, 4, false, false);
            }
          }
        }
      }
      if (sim && keyed({sim}, sim_kind) && add("Simulation", {sim}, sim_kind, 1, "p:sim", false, false, 0)) {
        property_rows(sim, sim_kind, 2, false, false);
      }
    }
    /* All, by owner: each layer's bones (their angle, position and scale) and points. */
    if (keyed(owned, KeyFilter{motion}) && add("All", owned, KeyFilter{motion}, 0, "all", true, false, 0)) {
      for (Layer *l : owned) {
        if (!keyed({l}, KeyFilter{motion})) {
          continue;
        }
        const std::string id = std::to_string(l->id);
        if (!add(l->name, {l}, KeyFilter{motion}, 1, "o:" + id, l->id == doc.active_layer, false, l->id)) {
          continue;
        }
        const KeyFilter bones{bone_motion, false};
        if (keyed({l}, bones) && add("Bones", {l}, bones, 2, "o:b:" + id, false, false, 0)) {
          for (const Bone &b : l->bones) {
            const KeyFilter one = bone_filter(b, bone_motion, false);
            if (keyed({l}, one) &&
                add(b.name, {l}, one, 3, "o:bone:" + id + ":" + std::to_string(b.id), false, false, 0))
            {
              property_rows(l, one, 4, false, false);
            }
          }
        }
        const KeyFilter points{KEY_POINTS, false};
        if (keyed({l}, points)) {
          add("Points", {l}, points, 2, "", false, false, 0);
        }
      }
    }
    if ((anything_selected(owned, *edited) || media) && add("Selected", owned, selected_motion, 0, "sel", true, true, 0)) {
      media_rows(1);
      camera_rows(1);
      for (Layer *l : owned) {
        for (const Bone &b : l->bones) {
          const KeyFilter one = bone_filter(b, bone_motion, true);
          if (bone_picked_any(b) && keyed({l}, one) &&
              add(b.name, {l}, one, 1, "sel:" + std::to_string(l->id) + ":" + std::to_string(b.id), false, true, 0))
          {
            property_rows(l, one, 2, false, true);
          }
        }
        const KeyFilter points{KEY_POINTS, true};
        if (keyed({l}, points)) {
          add(owned.size() > 1 ? l->name + ": Points" : std::string("Points"), {l}, points, 1, "", false, true, 0);
        }
      }
    }
    return rows;
  }

  /* Group by Kind. Props: Layers (each layer's Angle & Transform, Opacity, Visibility),
   * Constraints, Simulation. */
  if (keyed(layers, KeyFilter{props}) && add("Props", layers, KeyFilter{props}, 0, "props", true, false, 0)) {
    const KeyFilter layer_kinds{KEY_LAYER & ~KEY_DRAW_ORDER, false};
    if (keyed(owned, layer_kinds) && add("Layers", owned, layer_kinds, 1, "layers", true, false, 0)) {
      for (Layer *l : owned) {
        if (!keyed({l}, layer_kinds)) {
          continue;
        }
        const std::string id = std::to_string(l->id);
        if (!add(l->name, {l}, layer_kinds, 2, "layer:" + id, false, false, l->id)) {
          continue;
        }
        const KeyFilter at{KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM, false};
        if (keyed({l}, at) && add("Angle & Transform", {l}, at, 3, "lat:" + id, false, false, 0)) {
          property_rows(l, at, 4, false, false);
        }
        property_rows(l, KeyFilter{KEY_OPACITY | KEY_VISIBILITY, false}, 3, false, false);
      }
    }
    /* Every layer's Draw Order in one place, so a rule reaching across bone layers reads at a glance. */
    const KeyFilter order{KEY_DRAW_ORDER, false};
    if (keyed(owned, order) && add("Draw Order", owned, order, 1, "draw_order", true, false, 0)) {
      for (Layer *l : owned) {
        property_rows(l, order, 2, true, false);
      }
    }
    const KeyFilter constraints{KEY_CONSTRAINTS, false};
    if (keyed(owned, constraints) && add("Constraints", owned, constraints, 1, "constraints", false, false, 0)) {
      for (Layer *l : owned) {
        property_rows(l, constraints, 2, true, false);
      }
    }
    if (sim && keyed({sim}, sim_kind) && add("Simulation", {sim}, sim_kind, 1, "sim", false, false, 0)) {
      property_rows(sim, sim_kind, 2, false, false);
    }
  }
  /* All and Selected: Bones (Bone Angle, Bone Transform, a row per bone) and Points. */
  const auto bones_and_points = [&](bool selected, int depth, const std::string &prefix) {
    const KeyFilter bones{bone_motion, selected};
    /* Folded to start with, even in Selected (open by default), so a long rig doesn't fill it. */
    if (keyed(owned, bones) && add("Bones", owned, bones, depth, prefix + "bones", false, selected, 0)) {
      for (const auto &[name, kind] : {std::pair{"Bone Angle", KEY_BONE_ANGLE}, std::pair{"Bone Transform", KEY_BONE_TRANSFORM}}) {
        const KeyFilter group{uint32_t(kind), selected};
        if (keyed(owned, group) && add(name, owned, group, depth + 1, prefix + name, selected, selected, 0)) {
          for (Layer *l : owned) {
            property_rows(l, group, depth + 2, true, selected);
          }
        }
      }
    }
    const KeyFilter points{KEY_POINTS, selected};
    if (keyed(owned, points)) {
      add("Points", owned, points, depth, "", false, selected, 0);
    }
  };
  if (keyed(owned, KeyFilter{motion}) && add("All", owned, KeyFilter{motion}, 0, "all", true, false, 0)) {
    bones_and_points(false, 1, "");
  }
  if ((anything_selected(owned, *edited) || media) && add("Selected", owned, selected_motion, 0, "sel", true, true, 0)) {
    media_rows(1);
    camera_rows(1);
    bones_and_points(true, 1, "sel_");
  }
  return rows;
}

std::vector<TimelineRowInfo> timeline_rows_list(Context &C, bool by_kind)
{
  Area area;
  area.timeline_by_kind = by_kind;
  std::vector<TimelineRowInfo> list;
  for (const ChannelRow &row : channel_rows(area, timeline_shown_layers(C), *C.document, timeline_pinned_layer_ids(C), true)) {
    list.push_back({row.depth, row.name, row.selected, !row.pin_path.empty(), row.changed});
  }
  return list;
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
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  if (avail.x < 8.0f || avail.y < 8.0f) {
    return;
  }
  /* The Motion Path tool's timing chart takes the bottom (at most half), its clicks its own. */
  const float strip_h = std::min(timeline_path_strip_height(C, ImGui::GetFrameHeight()), avail.y * 0.5f);
  /* The sidebar (N) takes the right. */
  const float sidebar_w = timeline_sidebar_width(C, avail.x);
  const ImVec2 size(avail.x - sidebar_w, avail.y - strip_h);
  const ImVec2 p1 = p0 + size;

  if (!area.view_initialized) {
    view_frame_all(area, doc);
    area.view_initialized = true;
  }

  g_timeline_doc = &doc;
  /* The Adjust Last Operation panel, drawn over it later, takes the clicks over it. */
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##timeline_region", size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  /* Held still over a key: its Keyed Here list shows as a tooltip. */
  const bool hovered_still = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
  const bool active = ImGui::IsItemActive();
  C.wm->keyed_hover = {};

  /* Channel rows: Pinned, All, then each animated (active or selected) layer, as Blender's Dope
   * Sheet lists the selected objects. The names sit in a column on the left. */
  std::vector<ChannelRow> rows = channel_rows(area, timeline_shown_layers(C), doc, timeline_pinned_layer_ids(C));
  const float font_size = ImGui::GetFontSize();
  /* Wide enough for a property's name ("arm: Dynamics: Angle Bounce"); its edge drags. */
  const float names_w = std::clamp(area.timeline_names_w > 0.0f ? area.timeline_names_w : font_size * 13.0f,
                                   font_size * 5.0f, std::max(font_size * 5.0f, size.x * 0.6f));
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
  /* The names column's edge drags to make it wider or narrower. */
  static bool names_resizing = false;
  const bool on_edge = hovered && std::fabs(io.MousePos.x - k0.x) <= 4.0f && io.MousePos.y >= rows_top;
  if (on_edge || names_resizing) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  if (on_edge && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    names_resizing = true;
  }
  if (names_resizing) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      area.timeline_names_w = io.MousePos.x - p0.x;
    }
    else {
      names_resizing = false;
    }
  }

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
  const TimelineMediaView media_view{k0.x, p1.x, area.view_start, span, keys_w};
  /* A file dropped on the keys region starts on the frame under it. */
  C.wm->timeline_drop_areas.push_back({k0.x, p0.y, p1.x, p1.y, area.view_start, span / keys_w});
  const bool media_dragging = timeline_media_drag_update(C, media_view);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float key_r = std::max(4.0f, std::min(font_size * 0.36f, row_h * 0.4f));
  /* Each row's keys are kept between frames: with every point of a big rig keyed, working them out is
   * tens of milliseconds a frame. They are worked out again after a change (an edit, undo, keys picked or
   * moved: all push undo), while keys are dragged here (they move without one), or for other rows. */
  struct KeptRows {
    uint64_t signature = 0;
    std::vector<std::map<int, KeyFrameSummary>> summaries;
    std::vector<std::optional<std::vector<std::pair<int, int>>>> holds;
  };
  static std::map<int, KeptRows> kept_rows;
  const auto rows_signature = [&]() {
    uint64_t h = std::hash<const void *>()(&doc) ^ (C.undo ? C.undo->change_count() * 0x9e3779b97f4a7c15ull : 0);
    const auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(std::hash<std::string>()(doc.action));
    mix(doc.action_layer);
    mix(rows.size());
    for (const ChannelRow &row : rows) {
      mix(row.layers.size());
      for (const Layer *l : row.layers) {
        mix(l->id);
      }
      mix(row.filter.kinds | uint64_t(row.filter.selected_only) << 32 | uint64_t(row.filter.pinned_only) << 33);
      mix(std::hash<std::string>()(row.filter.path));
      if (row.filter.point_layers) {
        for (const uint64_t id : *row.filter.point_layers) {
          mix(id);
        }
      }
    }
    return h;
  };
  KeptRows &kept = kept_rows[area.id];
  std::vector<std::map<int, KeyFrameSummary>> &summaries = kept.summaries;
  const auto summarize = [&]() {
    summaries.assign(rows.size(), {});
    for (size_t r = 0; r < rows.size(); r++) {
      summaries[r] = layers_key_summary(rows[r].layers, rows[r].filter);
      /* A two-way link's driven side keeps no keys of its own: its row shows the ones it shares (the
       * other side's), where its motion changes. */
      if (rows[r].layers.size() == 1 && !rows[r].filter.path.empty()) {
        for (const Driver &d : doc.drivers) {
          if (!d.two_way || d.vars.size() != 1 || d.target.layer != rows[r].layers[0]->id || d.target.path != rows[r].filter.path) {
            continue;
          }
          Layer *source = layer_find(doc, d.vars.front().source.layer);
          ChannelRef ch;
          if (source && layer_channel(*source, d.vars.front().source.path, &ch)) {
            const auto add = [&](int frame) {
              KeyFrameSummary &sum = summaries[r][frame];
              sum.keys++;
            };
            if (ch.scalar) {
              for (const Key<float> &k : ch.scalar->keys) {
                add(k.frame);
              }
            }
            else if (ch.vector) {
              for (const Key<float2> &k : ch.vector->keys) {
                add(k.frame);
              }
            }
          }
        }
      }
    }
    kept.holds.assign(rows.size(), std::nullopt);
    kept.signature = C.undo && !g_key_transform.active ? rows_signature() : 0;
  };
  const auto hold_spans = [&](size_t r) -> const std::vector<std::pair<int, int>> & {
    if (kept.holds.size() != rows.size()) {
      kept.holds.assign(rows.size(), std::nullopt);
    }
    if (!kept.holds[r]) {
      kept.holds[r] = layers_hold_spans(rows[r].layers, rows[r].filter);
    }
    return *kept.holds[r];
  };
  if (kept.signature == 0 || g_key_transform.active || kept.signature != rows_signature() ||
      summaries.size() != rows.size())
  {
    summarize();
  }
  /* The key under `m` (its frame, -1 for none), and its row. */
  const auto key_under = [&](ImVec2 m, int *r_row) -> int {
    const int r = m.x >= k0.x ? row_at(m.y) : -1;
    *r_row = r;
    int hit = -1;
    if (r >= 0) {
      const float ky = row_top(size_t(r)) + row_h * 0.5f;
      for (const auto &[f, sum] : summaries[size_t(r)]) {
        if (std::fabs(m.x - frame_x(float(f))) <= key_r + 2.0f && std::fabs(m.y - ky) <= key_r + 3.0f) {
          hit = f;
        }
      }
    }
    return hit;
  };
  const auto keyed_open = [&](int r, int frame) {
    g_keyed_here = {};
    for (const Layer *l : rows[size_t(r)].layers) {
      g_keyed_here.layers.push_back(l->id);
    }
    g_keyed_here.filter = rows[size_t(r)].filter;
    g_keyed_here.frame = frame;
    ImGui::OpenPopup("##keyed_here");
  };

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
        g_key_transform.offset = offset;
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
        C.wm->modal_running = false;
        if (g_key_transform.kind == 'D') {
          /* Done again as Duplicate Keys by the offset the mouse gave (Ignore Bones as last set): its
           * options then show in the timeline's Adjust Last Operation panel. */
          doc = g_key_transform.snapshot;
          doc.frame_current = frame;
          operator_call(C, "anim.keys_duplicate", {{"frames", std::to_string(g_key_transform.offset)}});
        }
        else {
          C.undo->push(g_key_transform.kind == 'G' ? "Move Keys" : "Scale Keys", doc);
        }
        g_key_transform.snapshot = Document();
        /* The confirming click shouldn't also pick keys. */
        g_press = {};
      }
      C.wm->modal_keys.clear();
    }
    /* The document was replaced: look the layers up again. */
    rows = channel_rows(area, timeline_shown_layers(C), doc, timeline_pinned_layer_ids(C));
    summaries.resize(rows.size());
    summarize();
  }
  else if (!names_resizing && over_names && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    /* A reference's own layer channel: sync it (or all of them) to its original again. */
    const int r = row_at(io.MousePos.y);
    if (r >= 0 && rows[r].layers.size() == 1 && rows[r].layers[0]->is_reference() &&
        rows[r].layers[0]->reference.own_channels.count(rows[r].filter.path))
    {
      g_sync_layer = rows[r].layers[0]->id;
      g_sync_path = rows[r].filter.path;
      ImGui::OpenPopup("##reference_channel");
    }
  }
  else if (!names_resizing && !over_names && !media_dragging && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    /* Right-click a key: what it has keyed (as Ctrl-click); on a Media row, the clip menu. */
    int r = -1;
    const int row = row_at(io.MousePos.y);
    if (row >= 0 && rows[row].media) {
      timeline_media_menu_open(C, media_view, *rows[row].layers[0]);
    }
    else if (const int hit = key_under(io.MousePos, &r); hit >= 0) {
      keyed_open(r, hit);
    }
  }
  else if (!names_resizing && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    if (over_names) {
      /* The arrow folds or opens a row; a layer's name makes it the active layer (the selection
       * stays); elsewhere a folding row's name folds it too. */
      const int r = row_at(io.MousePos.y);
      if (r >= 0) {
        const ChannelRow &row = rows[r];
        const float arrow_x = p0.x + 6.0f + float(row.depth) * font_size;
        const bool on_arrow = io.MousePos.x < arrow_x + font_size;
        if (!row.pin_path.empty() && io.MousePos.x >= k0.x - 4.0f - row_h) {
          operator_call(C, "anim.channel_pin",
                        {{"layer", std::to_string(row.pin_layer)}, {"paths", row.pin_path}, {"pinned", "0"}});
        }
        else if (!row.fold.empty() && (on_arrow || !row.layer)) {
          ui_fold_record("Timeline/" + row.fold, row.fold_default, !row.open);
        }
        else if (row.layer) {
          doc.active_layer = row.layer;
          C.undo->push("Active Layer", doc);
        }
      }
      g_press = {};
    }
    else {
      /* The ruler scrubs; below it a click picks keys of a row and a drag boxes them (a Media row's
       * clips are picked and dragged by their own code). */
      g_press = {true, io.MousePos.y < p0.y + ruler_h, io.MousePos};
      /* A double-click in a row keys its channels on that frame (the playhead goes there). */
      if (!g_press.scrub && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const int r = row_at(io.MousePos.y);
        if (r >= 0 && !rows[size_t(r)].media) {
          const ChannelRow &row = rows[size_t(r)];
          operator_call(C, "anim.keys_insert",
                        {{"frame", std::to_string(std::max(0, int(std::lround(x_frame(io.MousePos.x)))))},
                         {"layers", layer_ids(row.layers)},
                         {"kinds", std::to_string(row.filter.kinds)},
                         {"selected_only", row.filter.selected_only ? "1" : "0"},
                         {"path", row.filter.path},
                         {"pinned", row.filter.pinned_only ? "1" : "0"}});
          g_press = {};
        }
      }
      const int media_row = g_press.scrub || !g_press.active ? -1 : row_at(io.MousePos.y);
      if (media_row >= 0 && rows[media_row].media &&
          timeline_media_row_press(C, media_view, *rows[media_row].layers[0],
                                   row_top(size_t(media_row - rows[media_row].media_part)), row_h * 2.0f))
      {
        g_press = {};
      }
      if (g_press.active && !g_press.scrub) {
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
        if (hit >= 0 && io.KeyCtrl) {
          /* Ctrl-click a key: the list of what it has keyed, each one a way to it. */
          keyed_open(r, hit);
          g_press = {};
        }
        else if (hit >= 0) {
          const KeyFrameSummary &sum = summaries[r][hit];
          operator_call(C, "anim.keys_select",
                        {{"frame_start", std::to_string(hit)},
                         {"layers", layer_ids(rows[r].layers)},
                         {"kinds", std::to_string(rows[r].filter.kinds)},
                         {"selected_only", rows[r].filter.selected_only ? "1" : "0"},
                         {"path", rows[r].filter.path},
                         {"pinned", rows[r].filter.pinned_only ? "1" : "0"},
                         {"extend", io.KeyShift ? "1" : "0"},
                         {"deselect", io.KeyShift && sum.selected == sum.keys ? "1" : "0"}});
          g_press = {};
        }
      }
    }
  }
  /* The Keyed Here list: each property keyed on that frame under the row, by owner, with its value;
   * choosing one jumps to the frame and selects its bone or points, or shows it in Properties
   * (Shift adds to the selection, Alt keeps the frame). */
  if (ui_menu_begin("##keyed_here")) {
    const std::vector<KeyedEntry> entries = keyed_entries(doc, g_keyed_here);
    ImGui::TextDisabled("Keyed on frame %d", g_keyed_here.frame);
    ImGui::Separator();
    const auto go = [&](uint64_t layer, const std::vector<std::string> &paths) {
      operator_call(C, "anim.keyed_go",
                    {{"layer", std::to_string(layer)},
                     {"paths", joined_paths(paths)},
                     {"frame", std::to_string(io.KeyAlt ? -1 : g_keyed_here.frame)},
                     {"extend", io.KeyShift ? "1" : "0"}});
    };
    const auto hover = [&](uint64_t layer, const std::vector<std::string> &paths) {
      if (ImGui::IsItemHovered()) {
        C.wm->keyed_hover = {layer, paths};
      }
    };
    if (entries.empty()) {
      ImGui::TextDisabled("Nothing keyed here");
    }
    size_t shown = 0;
    for (const KeyedEntry &e : entries) {
      if (++shown > 40) {
        ImGui::TextDisabled("...and %d more", int(entries.size() - 40));
        break;
      }
      if (!e.singles.empty()) {
        /* Many points: all of them at once, or one at a time. */
        const std::string label = e.label + " (" + std::to_string(e.singles.size()) + ")";
        if (ui_submenu_begin(label.c_str())) {
          const std::string all = "All " + std::to_string(e.singles.size());
          if (ui_menu_item(all.c_str())) {
            go(e.layer, e.paths);
          }
          hover(e.layer, e.paths);
          ImGui::Separator();
          for (const auto &[name, path] : e.singles) {
            const std::string value = keyed_value(*keyed_layer(doc, e.layer), path, g_keyed_here.frame);
            if (ui_menu_item(name.c_str(), value.c_str())) {
              go(e.layer, {path});
            }
            hover(e.layer, {path});
          }
          ui_submenu_end();
        }
        hover(e.layer, e.paths);
        continue;
      }
      if (ui_menu_item(e.label.c_str(), e.value.c_str())) {
        go(e.layer, e.paths);
      }
      hover(e.layer, e.paths);
    }
    ImGui::Separator();
    std::vector<Layer *> these;
    for (const uint64_t id : g_keyed_here.layers) {
      if (Layer *l = keyed_layer(doc, id)) {
        these.push_back(l);
      }
    }
    /* Interpolation for the selection; these keys join it first unless they're all in it. */
    if (ui_submenu_begin("Interpolation")) {
      interp_menu_items(C, [&]() {
        const KeyFrameSummary sum = layers_key_summary(these, g_keyed_here.filter)[g_keyed_here.frame];
        if (sum.selected < sum.keys) {
          operator_call(C, "anim.keys_select",
                        {{"frame_start", std::to_string(g_keyed_here.frame)},
                         {"layers", layer_ids(these)},
                         {"kinds", std::to_string(g_keyed_here.filter.kinds)},
                         {"selected_only", g_keyed_here.filter.selected_only ? "1" : "0"},
                         {"path", g_keyed_here.filter.path},
                         {"pinned", g_keyed_here.filter.pinned_only ? "1" : "0"}});
        }
      });
      ui_submenu_end();
    }
    if (ui_menu_item("Select These Keys")) {
      std::vector<Layer *> &layers = these;
      operator_call(C, "anim.keys_select",
                    {{"frame_start", std::to_string(g_keyed_here.frame)},
                     {"layers", layer_ids(layers)},
                     {"kinds", std::to_string(g_keyed_here.filter.kinds)},
                     {"selected_only", g_keyed_here.filter.selected_only ? "1" : "0"},
                     {"path", g_keyed_here.filter.path},
                     {"pinned", g_keyed_here.filter.pinned_only ? "1" : "0"},
                     {"extend", io.KeyShift ? "1" : "0"}});
    }
    ui_menu_end();
  }
  else if (hovered_still && !g_press.active && !ImGui::IsAnyMouseDown()) {
    /* Held still over a key: the same list, to look at. */
    int r = -1;
    if (const int hit = key_under(io.MousePos, &r); hit >= 0) {
      KeyedHere look;
      for (const Layer *l : rows[size_t(r)].layers) {
        look.layers.push_back(l->id);
      }
      look.filter = rows[size_t(r)].filter;
      look.frame = hit;
      const std::vector<KeyedEntry> entries = keyed_entries(doc, look);
      ImGui::BeginTooltip();
      ImGui::TextDisabled("Keyed on frame %d", hit);
      size_t shown = 0;
      for (const KeyedEntry &e : entries) {
        if (++shown > 15) {
          ImGui::TextDisabled("...and %d more", int(entries.size() - 15));
          break;
        }
        if (!e.singles.empty()) {
          ImGui::Text("%s (%d)", e.label.c_str(), int(e.singles.size()));
        }
        else {
          ImGui::Text("%s", e.label.c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("%s", e.value.c_str());
        }
      }
      ImGui::TextDisabled("Ctrl-click (or right-click) to go to one");
      ImGui::EndTooltip();
    }
  }
  timeline_media_menu(C);
  if (ui_menu_begin("##reference_channel")) {
    if (ui_menu_item("Reset Channel to Original")) {
      operator_call(C, "layer.reference_sync", {{"layer", std::to_string(g_sync_layer)}, {"path", g_sync_path}});
    }
    if (ui_menu_item("Reset Layer Animation to Original")) {
      operator_call(C, "layer.reference_sync", {{"layer", std::to_string(g_sync_layer)}});
    }
    ui_menu_end();
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
                           {"layers", layer_ids(row.layers)},
                           {"kinds", std::to_string(row.filter.kinds)},
                           {"selected_only", row.filter.selected_only ? "1" : "0"},
                           {"path", row.filter.path},
                           {"pinned", row.filter.pinned_only ? "1" : "0"},
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
  /* Editing a fix: each pose's frame marked (a line, its number on the ruler's foot, at the bottom the pose),
   * the one shown brighter. Keys on other frames don't play. */
  if (const Layer *fix_owner = doc.action.empty() ? nullptr : layer_find(doc, doc.action_layer)) {
    if (const FixAction *fix = fix_action_find(*fix_owner, doc.action)) {
      const int shown = fix_pose_on_frame(*fix, doc.frame_current);
      const float label_y = p1.y - font_size - 4.0f;
      for (size_t k = 0; k < fix->poses.size(); k++) {
        const float x = frame_x(float(fix->poses[k].frame));
        const bool here = int(k) == shown;
        const ImU32 col = here ? COL_FIX_TEXT : (COL_FIX_TEXT & 0x90ffffff);
        dl->AddLine(ImVec2(x, p0.y + ruler_h), ImVec2(x, p1.y), col, here ? 2.0f : 1.5f);
        dl->AddRectFilled(ImVec2(x - 1.0f, p0.y + ruler_h - 4.0f), ImVec2(x + 1.0f, p0.y + ruler_h), col);
        /* The pose shown named in full; each label kept short of the next pose's line. */
        std::string label = "Pose " + std::to_string(k + 1);
        if (here) {
          label += ": " + fix_pose_label(*fix_owner, *fix, fix->poses[k]);
        }
        float next_x = p1.x;
        for (const FixPose &other : fix->poses) {
          const float ox = frame_x(float(other.frame));
          next_x = ox > x + 1.0f ? std::min(next_x, ox - 3.0f) : next_x;
        }
        dl->PushClipRect(ImVec2(x, p0.y), ImVec2(std::max(x, next_x), p1.y), true);
        dl->AddText(ImVec2(x + 4.0f, label_y), col, label.c_str());
        dl->PopClipRect();
      }
    }
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
      if (!rows[r].media) {
        dl->AddLine(ImVec2(k0.x, ky), ImVec2(p1.x, ky), IM_COL32(0xd0, 0x40, 0x40, 110));
      }
    }
    else if (r % 2 == 1) {
      dl->AddRectFilled(ImVec2(p0.x, top), ImVec2(p1.x, top + row_h), IM_COL32(0xff, 0xff, 0xff, 6));
    }
    if (!(rows[r].media && rows[r].media_part == 0)) {
      dl->AddLine(ImVec2(p0.x, top + row_h), ImVec2(p1.x, top + row_h), IM_COL32(0x22, 0x22, 0x22, 255));
    }
    std::map<int, KeyFrameSummary> &sum_row = summaries[r];
    if (rows[r].media) {
      /* Two rows tall: drawn with its first row, or with the second when the first is scrolled off. */
      const bool first = rows[r].media_part == 0;
      if (first || row_top(r - 1) + row_h < rows_top) {
        const float bar_top = first ? top : top - row_h;
        C.wm->timeline_drop_areas.push_back({k0.x, std::max(bar_top, rows_top), p1.x, std::min(bar_top + row_h * 2.0f, p1.y),
                                             area.view_start, span / keys_w, rows[r].media});
        dl->PushClipRect(ImVec2(k0.x, rows_top), p1, true);
        timeline_media_row_draw(C, dl, media_view, *rows[r].layers[0], bar_top, row_h * 2.0f);
        dl->PopClipRect();
        if (hovered && !media_dragging && !g_press.active && io.MousePos.x >= k0.x) {
          timeline_media_row_hover(C, media_view, *rows[r].layers[0], bar_top, row_h * 2.0f);
        }
      }
      continue;
    }
    dl->PushClipRect(ImVec2(k0.x, rows_top), p1, true);
    const bool order_row = rows[r].filter.path == "draw_order" && rows[r].layers.size() == 1;
    if (order_row) {
      /* Draw Order: between keys, a bar naming the rule in effect (none while Normal). */
      const Layer &owner = *rows[r].layers[0];
      const std::vector<Key<float>> &keys = owner.draw_order_keys.keys;
      for (size_t i = 0; i < keys.size(); i++) {
        const DrawOrderRule rule = draw_order_rule(keys[i].value);
        if (rule.mode == DrawOrderMode::Normal) {
          continue;
        }
        const float xa = frame_x(float(keys[i].frame)) + key_r * 0.6f;
        const float xb = i + 1 < keys.size() ? frame_x(float(keys[i + 1].frame)) - key_r * 0.6f : p1.x;
        if (xb <= xa || xb < k0.x || xa > p1.x) {
          continue;
        }
        const bool front = rule.mode == DrawOrderMode::Front;
        const bool missing = layer_find(doc, rule.target) == nullptr;
        const ImU32 fill = missing ? IM_COL32(0x80, 0x30, 0x30, 200)
                           : front ? IM_COL32(0x1d, 0x6e, 0x5a, 220)
                                   : IM_COL32(0x4a, 0x42, 0x8c, 220);
        const ImVec2 bmin(xa, ky - row_h * 0.32f), bmax(xb, ky + row_h * 0.32f);
        dl->AddRectFilled(bmin, bmax, fill, 3.0f);
        const std::string label = draw_order_label(doc, keys[i].value);
        dl->PushClipRect(bmin, bmax, true);
        const float text_x = std::max(xa, k0.x) + 5.0f;
        if (ImGui::CalcTextSize(label.c_str()).x <= bmax.x - text_x - 3.0f) {
          dl->AddText(ImVec2(text_x, ky - font_size * 0.5f), IM_COL32(0xf0, 0xf0, 0xf0, 255), label.c_str());
        }
        else {
          /* Too short for the words: an arrow (up in front, down behind) and the layer's name. */
          const float s = font_size * 0.28f, d = front ? -1.0f : 1.0f, cx = text_x + s;
          dl->AddTriangleFilled(ImVec2(cx, ky + d * s), ImVec2(cx + s, ky - d * s * 0.6f), ImVec2(cx - s, ky - d * s * 0.6f),
                                IM_COL32(0xf0, 0xf0, 0xf0, 255));
          const Layer *target = layer_find(doc, rule.target);
          dl->AddText(ImVec2(cx + s + 4.0f, ky - font_size * 0.5f), IM_COL32(0xf0, 0xf0, 0xf0, 255),
                      target ? target->name.c_str() : "?");
        }
        dl->PopClipRect();
        if (hovered && io.MousePos.x >= bmin.x && io.MousePos.x < bmax.x && io.MousePos.y >= bmin.y &&
            io.MousePos.y < bmax.y && !g_press.active)
        {
          const int last = i + 1 < keys.size() ? keys[i + 1].frame - 1 : -1;
          ImGui::SetTooltip("%s %s, frame %d%s", owner.name.c_str(), label.c_str(), keys[i].frame,
                            last >= 0 ? (" to " + std::to_string(last)).c_str() : " on");
          C.wm->keyed_hover.boxes = {owner.id, rule.target};
        }
      }
    }
    const bool switch_row = rows[r].filter.path == "switch" && rows[r].layers.size() == 1 && rows[r].layers[0]->is_switch();
    if (switch_row) {
      /* Switch: between keys, a bar naming the child shown, coloured by who decides there: a key made by
       * hand (teal), the rig (orange: a Rig key, or the stretch from frame 0 when action bone actions key it),
       * the default (grey); red for a child that is gone. */
      const Layer &sw = *rows[r].layers[0];
      const std::vector<Key<float>> &keys = sw.switch_keys.keys;
      const bool rigged = switch_keyed_by_actions(sw);
      /* A linked group's keys (on its lead) read as names, for the member being worked in: one lacking a
       * name says what it keeps showing. */
      const SwitchLink *link = switch_link_of(doc, sw);
      const Layer *viewer = &sw;
      if (link) {
        for (const Layer *l = document_active_layer(doc); l; l = layer_parent(doc, l->id)) {
          if (l->is_switch() && l->switch_link == link->id) {
            viewer = l;
            break;
          }
        }
      }
      const std::string fallback = switch_key_label(doc, *viewer, switch_default(sw));
      for (size_t i = 0; i < keys.size(); i++) {
        const float xa = frame_x(float(keys[i].frame)) + key_r * 0.6f;
        const float xb = i + 1 < keys.size() ? frame_x(float(keys[i + 1].frame)) - key_r * 0.6f : p1.x;
        if (xb <= xa || xb < k0.x || xa > p1.x) {
          continue;
        }
        const float v = keys[i].value;
        const bool rig = v < -0.5f || (keys[i].frame == 0 && rigged);
        const bool by_hand = keys[i].frame > 0 && v > -0.5f;
        const bool missing = !link && v > 0.5f && !switch_child(sw, v);
        const std::string held = link && switch_key_missing(doc, *viewer, v) ? switch_key_held(doc, *viewer, v, keys[i].frame) : "";
        const ImU32 fill = missing   ? IM_COL32(0x80, 0x30, 0x30, 200)
                           : rig     ? IM_COL32(0x8c, 0x5a, 0x1e, 220)
                           : by_hand ? IM_COL32(0x1d, 0x6e, 0x6e, 220)
                                     : IM_COL32(0x4a, 0x4a, 0x4a, 220);
        const std::string name = switch_key_label(doc, *viewer, v);
        std::string label = v < -0.5f ? "Rig, else " + fallback
                            : rig     ? "Rig, else " + name
                            : by_hand ? name
                                      : name + " (default)";
        if (!held.empty()) {
          label += " (not here: keeps " + held + ")";
        }
        const ImVec2 bmin(xa, ky - row_h * 0.32f), bmax(xb, ky + row_h * 0.32f);
        dl->AddRectFilled(bmin, bmax, fill, 3.0f);
        dl->PushClipRect(bmin, bmax, true);
        dl->AddText(ImVec2(std::max(xa, k0.x) + 5.0f, ky - font_size * 0.5f), IM_COL32(0xf0, 0xf0, 0xf0, 255), label.c_str());
        dl->PopClipRect();
        if (hovered && io.MousePos.x >= bmin.x && io.MousePos.x < bmax.x && io.MousePos.y >= bmin.y &&
            io.MousePos.y < bmax.y && !g_press.active)
        {
          const int last = i + 1 < keys.size() ? keys[i + 1].frame - 1 : -1;
          const char *who = missing ? " (a child that is gone: the default shows)"
                            : rig   ? " (its action bones pick, else the default)"
                            : by_hand ? " (keyed by hand: wins over action bones)"
                                      : " (the default)";
          ImGui::SetTooltip("%s shows %s, frame %d%s%s\nDrag a key to retime; Replace Keys With... is in its "
                            "right-click Switch menu",
                            viewer->name.c_str(), label.c_str(), keys[i].frame,
                            last >= 0 ? (" to " + std::to_string(last)).c_str() : " on", who);
        }
      }
    }
    for (const auto &[a, b] : order_row || switch_row ? std::vector<std::pair<int, int>>() : hold_spans(r)) {
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
      key_draw(dl, ImVec2(kx, ky), kr, sum.shape, sum.selected > 0 ? COL_KEY_SELECTED : COL_KEY);
    }
    dl->PopClipRect();
  }
  dl->PopClipRect();

  /* The names column. */
  dl->AddRectFilled(p0, ImVec2(k0.x, p1.y), IM_COL32(0x26, 0x26, 0x26, 255));
  dl->AddLine(ImVec2(k0.x, p0.y), ImVec2(k0.x, p1.y), IM_COL32(0x18, 0x18, 0x18, 255));
  dl->PushClipRect(ImVec2(p0.x, rows_top), ImVec2(k0.x - 4.0f, p1.y), true);
  /* Rows whose name didn't fit (shown whole on hover). */
  std::vector<bool> cut(rows.size(), false);
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
    const ImU32 col = row.changed       ? IM_COL32(0x6b, 0xdb, 0xd1, 255)
                      : row.depth == 0 ? (row.selected ? IM_COL32(0xff, 0x90, 0x90, 255) : IM_COL32(0xe6, 0xe6, 0xe6, 255))
                      : is_active      ? IM_COL32(0xff, 0xff, 0xff, 255)
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
    /* The name, cut short with "..." where it would run into the pin or the keys. */
    const float text_right = k0.x - 6.0f - (row.pin_path.empty() ? 0.0f : row_h);
    cut[r] = ImGui::CalcTextSize(row.name.c_str()).x > text_right - x;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, ImVec2(x, top + (row_h - font_size) * 0.5f), ImVec2(text_right, top + row_h),
                              text_right, row.name.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
    if (!row.pin_path.empty()) {
      /* Its pin, red: click to unpin. */
      const ImVec2 at(k0.x - 4.0f - row_h, top);
      const bool over = io.MousePos.x >= at.x && io.MousePos.x < k0.x && io.MousePos.y >= top && io.MousePos.y < top + row_h;
      ui_pin_draw(dl, at, row_h, over ? IM_COL32(0xff, 0x70, 0x70, 255) : IM_COL32(0xe0, 0x40, 0x40, 255));
    }
  }
  dl->PopClipRect();
  if (hovered && over_names && io.MousePos.y >= rows_top) {
    const int r = row_at(io.MousePos.y);
    if (r >= 0 && !rows[r].pin_path.empty() && io.MousePos.x >= k0.x - 4.0f - row_h &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
      ImGui::SetTooltip("Unpin from the Timeline");
    }
    else if (r >= 0 && rows[r].layer && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s\nClick to make it the active layer; the arrow folds it", rows[r].name.c_str());
    }
    else if (r >= 0 && cut[r] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", rows[r].name.c_str());
    }
  }

  if (boxing) {
    dl->AddRectFilled(g_press.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 20));
    dl->AddRect(g_press.start, io.MousePos, IM_COL32(0xff, 0xff, 0xff, 160));
  }
  if (!transform_info.empty()) {
    dl->AddText(ImVec2(k0.x + 8.0f, p0.y + ruler_h + 4.0f), IM_COL32_WHITE, transform_info.c_str());
  }

  /* A file dragged in from Explorer: where it would land, live. On a Media layer's clips row it joins
   * that layer; anywhere else it makes a new Media layer (shown under the ruler). */
  if (const FileDrag &drag = file_drag();
      drag.active && drag.x >= k0.x && drag.x < p1.x && drag.y >= p0.y && drag.y < p1.y)
  {
    const int r = row_at(drag.y);
    if (r >= 0 && rows[r].media) {
      timeline_media_drop_preview(C, dl, media_view, rows[r].layers[0], row_top(size_t(r - rows[r].media_part)),
                                  row_h * 2.0f);
    }
    else {
      /* At the mouse's height, kept below the ruler. */
      const float band = row_h * 2.0f;
      timeline_media_drop_preview(C, dl, media_view, nullptr,
                                  std::clamp(drag.y - band * 0.5f, rows_top, std::max(rows_top, p1.y - band)), band);
    }
  }

  /* The frames onion skins show, tinted faintly in their side's colour. */
  if (doc.onion_skin && !C.wm->playing) {
    const float half = std::max(2.0f, (frame_x(1.0f) - frame_x(0.0f)) * 0.5f);
    dl->PushClipRect(ImVec2(k0.x, p0.y + ruler_h), p1, true);
    for (const OnionGhost &g : onion_ghosts(doc, onion_layers(doc, editable_layers(C)))) {
      const Color &c = g.after ? doc.onion_color_after : doc.onion_color_before;
      const float fx = frame_x(float(g.frame));
      dl->AddRectFilled(ImVec2(fx - half, p0.y + ruler_h), ImVec2(fx + half, p1.y),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 0.22f * onion_ghost_alpha(doc, g))));
    }
    dl->PopClipRect();
  }

  /* The frames motion paths cover (or Alt Space plays), shaded under the keys with edges at both ends. */
  if (doc.motion_paths || (C.wm->playing && C.wm->play_range_first > 0)) {
    int first = 0, last = 0;
    if ((C.wm->playing && C.wm->play_range_first > 0)) {
      first = C.wm->play_range_first;
      last = C.wm->play_range_last;
    }
    else {
      motion_path_range(doc, motion_path_targets(doc, editable_layers(C)), first, last);
    }
    if (last > first) {
      const float half = (frame_x(1.0f) - frame_x(0.0f)) * 0.5f;
      const float x0 = frame_x(float(first)) - half, x1 = frame_x(float(last)) + half;
      dl->PushClipRect(ImVec2(k0.x, p0.y), p1, true);
      dl->AddRectFilled(ImVec2(x0, p0.y + ruler_h), ImVec2(x1, p1.y), IM_COL32(255, 200, 80, 18));
      dl->AddRectFilled(ImVec2(x0, p0.y + ruler_h - 3.0f), ImVec2(x1, p0.y + ruler_h), IM_COL32(255, 200, 80, 150));
      dl->PopClipRect();
    }
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

  if (strip_h > 0.0f) {
    timeline_path_strip(C, ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y + strip_h), k0.x, area.view_start,
                        area.view_end - area.view_start, row_h);
  }

  if (sidebar_w > 0.0f) {
    timeline_sidebar(C, ImVec2(p1.x, p0.y), ImVec2(p1.x + sidebar_w, p0.y + avail.y));
  }

  /* Blender's Adjust Last Operation, over the keys' bottom-left corner (as in the viewport). */
  ui_action_panel_draw(C, "Timeline", "Timeline/Adjust Last Operation", ImVec2(k0.x, p0.y), p1 - ImVec2(k0.x, p0.y),
                       4.0f);
}

void timeline_draw(Area &area, Context &C)
{
  timeline_header(area, C);
  timeline_body(area, C);
}

}  // namespace oanim
