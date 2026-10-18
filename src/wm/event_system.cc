#include "wm/event_system.hh"

#include "ops/operator.hh"
#include "wm/context.hh"
#include "wm/keymap.hh"

namespace sky {

static bool try_keymap(Context &C, const std::string &keymap, const KeyEvent &event)
{
  const KeyMapItem *item = keymaps().match(keymap, event);
  if (!item) {
    return false;
  }
  const OperatorType *ot = operators().find(item->idname);
  if (!ot || !operator_poll(C, *ot)) {
    return false;
  }
  return operator_call(C, item->idname) != OpStatus::PassThrough;
}

bool wm_handle_key_event(Context &C, const KeyEvent &event)
{
  if (C.area_type != "Window" && try_keymap(C, C.area_type, event)) {
    return true;
  }
  return try_keymap(C, "Window", event);
}

}  // namespace sky
