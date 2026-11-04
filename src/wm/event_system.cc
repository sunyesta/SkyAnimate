#include "wm/event_system.hh"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "ops/operator.hh"
#include "wm/context.hh"
#include "wm/keymap.hh"

namespace oanim {

static bool try_keymap(Context &C, const std::string &keymap, const KeyEvent &event)
{
  /* As in Blender: of the operators on this key, the first that can run here takes it (X deletes
   * points in a drawing and bones in a bone layer). */
  for (const KeyMapItem *item : keymaps().match_all(keymap, event)) {
    const OperatorType *ot = operators().find(item->idname);
    if (ot && operator_poll(C, *ot)) {
      return operator_call(C, item->idname) != OpStatus::PassThrough;
    }
  }
  return false;
}

/* Keys a text field handles itself: plain typing, editing shortcuts and caret movement. */
static bool key_used_by_text_editing(const KeyEvent &event)
{
  static const std::string_view keys[] = {"Z",          "Y",           "A",        "C",          "V",
                                          "X",          "LEFT_ARROW",  "RIGHT_ARROW", "UP_ARROW", "DOWN_ARROW",
                                          "BACK_SPACE", "DEL",         "HOME",     "END",        "RET",
                                          "TAB"};
  if (!event.ctrl && !event.alt) {
    return true;
  }
  return std::find(std::begin(keys), std::end(keys), event.type) != std::end(keys);
}

bool wm_handle_key_event(Context &C, const KeyEvent &event, bool area_only)
{
  /* Typing in a text field: the keys it uses are the field's, not the shortcuts of the editor
   * under the mouse (M while renaming a layer is a letter, not Move to Collection). The text
   * editor and console are text themselves: their keymaps still apply (Alt P). */
  const bool text_area = C.area_type == "Text Editor" || C.area_type == "Console";
  if (area_only && !text_area && key_used_by_text_editing(event)) {
    return false;
  }
  /* The Sculpt tool's keys (its brushes, F and Shift F) come before the viewport's own. */
  if (C.area_type == "Viewport" && C.wm->tool_settings.tool == Tool::Sculpt && try_keymap(C, "Sculpt", event)) {
    return true;
  }
  /* The Motion Path tool's (F and Shift F, for its brushes). */
  if (C.area_type == "Viewport" && C.wm->tool_settings.tool == Tool::MotionPath && try_keymap(C, "Motion Path", event)) {
    return true;
  }
  /* And the Draw tool's (F and Shift F, Shift Space). */
  if (C.area_type == "Viewport" && C.wm->tool_settings.tool == Tool::Draw && try_keymap(C, "Draw", event)) {
    return true;
  }
  if (C.area_type != "Window" && try_keymap(C, C.area_type, event)) {
    return true;
  }
  /* Global shortcuts such as Ctrl Q still work while typing, as in Blender. */
  if (area_only && key_used_by_text_editing(event)) {
    return false;
  }
  return try_keymap(C, "Window", event);
}

}  // namespace oanim
