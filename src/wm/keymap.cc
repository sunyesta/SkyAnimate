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
  /* Re-adding an operator's key replaces it; other operators can share the key, as in Blender
   * (the first that can run takes it: X deletes points in a drawing and bones in a bone layer). */
  std::erase_if(items, [&](const KeyMapItem &existing) {
    return existing.event == item.event && existing.idname == item.idname;
  });
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

std::vector<const KeyMapItem *> KeyMaps::match_all(const std::string &keymap, const KeyEvent &event) const
{
  std::vector<const KeyMapItem *> found;
  if (auto it = maps_.find(keymap); it != maps_.end()) {
    for (const KeyMapItem &item : it->second) {
      if (item.event == event) {
        found.push_back(&item);
      }
    }
  }
  return found;
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
