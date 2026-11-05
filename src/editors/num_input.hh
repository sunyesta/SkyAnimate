/* Numeric input for modal transforms (the viewport's G R S, the timeline's G and key drags). */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wm/context.hh"

namespace oanim {

/* Numbers typed while G, R or S runs, as Blender's numeric input: digits and a point type into the
 * field being typed (Move has X and Y: Tab goes to the other), minus flips its sign (before or after
 * the digits), Backspace takes the last character back, Ctrl Backspace clears it. Once anything is
 * typed the mouse stops moving the value; a field left empty counts as 0 (Move) or as the other
 * one (Scale). Read from the keys the transform takes, as text input is off while it runs. */
struct NumInput {
  std::string text[2];
  bool negative[2] = {false, false};
  int field = 0;

  bool any() const { return has(0) || has(1) || negative[0] || negative[1]; }
  bool has(int i) const
  {
    const std::string &t = text[i];
    return !t.empty() && t != ".";
  }
  float value(int i, float fallback) const
  {
    if (!has(i)) {
      return negative[i] ? -fallback : fallback;
    }
    const float v = std::strtof(text[i].c_str(), nullptr);
    return negative[i] ? -v : v;
  }
  /* The field as the header shows it: what's typed, with a caret in the field being typed. */
  std::string shown(int i, float live, const char *format) const
  {
    std::string s;
    if (has(i) || negative[i] || (i == field && any())) {
      s = (negative[i] ? "-" : "") + text[i];
      if (i == field) {
        s = "[" + s + "|]";
      }
      return s;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), format, double(live));
    return buf;
  }
};

/* Takes the typed keys of a running transform into `num`; `fields` is 2 for Move and Scale (Tab
 * switches), 1 otherwise. Returns whether anything changed. */
inline bool num_input_keys(Context &C, NumInput &num, int fields)
{
  bool changed = false;
  std::vector<ModalKey> &keys = C.wm->modal_keys;
  for (auto it = keys.begin(); it != keys.end();) {
    std::string t = it->type;
    if (t.rfind("KEYPAD_", 0) == 0) {
      t = t.substr(7);
    }
    std::string &text = num.text[num.field];
    bool used = true;
    if (it->ctrl && t == "BACK_SPACE") {
      text.clear();
      num.negative[num.field] = false;
    }
    else if (it->ctrl) {
      used = false;
    }
    else if (t.size() == 1 && t[0] >= '0' && t[0] <= '9') {
      text.push_back(t[0]);
    }
    else if ((t == "." || t == "PERIOD" || t == "NUMPAD_PERIOD" || t == ",") && text.find('.') == std::string::npos) {
      text.push_back('.');
    }
    else if (t == "-" || t == "MINUS") {
      num.negative[num.field] = !num.negative[num.field];
    }
    else if (t == "BACK_SPACE") {
      if (!text.empty()) {
        text.pop_back();
      }
      else {
        num.negative[num.field] = false;
      }
    }
    else if (t == "TAB" && fields > 1) {
      num.field = (num.field + 1) % fields;
    }
    else {
      used = false;
    }
    if (used) {
      changed = true;
      it = keys.erase(it);
    }
    else {
      ++it;
    }
  }
  return changed;
}

}  // namespace oanim
