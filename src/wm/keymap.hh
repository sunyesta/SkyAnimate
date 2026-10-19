#pragma once

/* Keymaps: data that maps key presses to operators, grouped by editor like Blender's.
 * The default keymap is defined in Python (scripts/startup/sky_keymap_default.py). */

#include <map>
#include <string>
#include <vector>

namespace sky {

struct KeyEvent {
  /* Key name in Blender's style: "A", "F3", "SPACE", "ESC", "RET", ... */
  std::string type;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;

  bool operator==(const KeyEvent &other) const = default;
};

struct KeyMapItem {
  std::string idname;
  KeyEvent event;
};

class KeyMaps {
 public:
  /* keymap: "Window" for global shortcuts, or an editor's keymap name such as "Viewport". */
  void add(const std::string &keymap, KeyMapItem item);
  void clear() { maps_.clear(); }
  const KeyMapItem *match(const std::string &keymap, const KeyEvent &event) const;
  /* Every item for the key, in keymap order: the first whose operator can run takes it. */
  std::vector<const KeyMapItem *> match_all(const std::string &keymap, const KeyEvent &event) const;
  /* First shortcut bound to an operator, for showing in menus. Empty if none. */
  std::string shortcut_for(const std::string &idname) const;
  const std::map<std::string, std::vector<KeyMapItem>> &all() const { return maps_; }

 private:
  std::map<std::string, std::vector<KeyMapItem>> maps_;
};

KeyMaps &keymaps();

/* "Ctrl Shift Z" style label. */
std::string key_event_label(const KeyEvent &event);

}  // namespace sky
