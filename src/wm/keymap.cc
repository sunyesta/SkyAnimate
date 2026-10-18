#include "wm/keymap.hh"

namespace sky {

KeyMaps &keymaps()
{
  static KeyMaps maps;
  return maps;
}

void KeyMaps::add(const std::string &keymap, KeyMapItem item)
{
  std::vector<KeyMapItem> &items = maps_[keymap];
  /* Re-adding the same event replaces its binding. */
  std::erase_if(items, [&](const KeyMapItem &existing) { return existing.event == item.event; });
  items.push_back(std::move(item));
}

const KeyMapItem *KeyMaps::match(const std::string &keymap, const KeyEvent &event) const
{
  auto it = maps_.find(keymap);
  if (it == maps_.end()) {
    return nullptr;
  }
  for (const KeyMapItem &item : it->second) {
    if (item.event == event) {
      return &item;
    }
  }
  return nullptr;
}

std::string KeyMaps::shortcut_for(const std::string &idname) const
{
  for (const auto &[name, items] : maps_) {
    for (const KeyMapItem &item : items) {
      if (item.idname == idname) {
        return key_event_label(item.event);
      }
    }
  }
  return {};
}

std::string key_event_label(const KeyEvent &event)
{
  std::string label;
  if (event.ctrl) {
    label += "Ctrl ";
  }
  if (event.shift) {
    label += "Shift ";
  }
  if (event.alt) {
    label += "Alt ";
  }
  return label + event.type;
}

}  // namespace sky
