/* Number fields, as Blender's number buttons (interface_handlers.cc's do_but_NUM and numedit_but_NUM, and
 * interface_widgets.cc's widget_numbut and widget_numslider), drawn and handled here for every number in the
 * app (ui_drag_float and its kin in ui.hh):
 *
 * - The look: a rounded field, its label on the left and its value on the right (centred without a label),
 *   no arrows; a factor (a 0-1 strength, weight, influence) filled to the value's share of its soft range.
 * - A drag (past a few pixels) changes it, the cursor hidden and kept where the press was so it never runs
 *   into the screen's edge, and shown again there (a factor's at the value's place on its bar). Shift is a
 *   tenth as fast, Ctrl snaps (0.1, 1 or 10 by the range; whole numbers by tens), Esc or right-click puts the
 *   value back. A drag stops at the soft limits (coming back changes it at once); one started past them can
 *   go as far back as it began. A factor's bar is its soft range across.
 * - A click without a drag types into it (a sum, as 2*3 or 90/4, units left out); Enter keeps it, Esc puts
 *   it back, Tab and Shift Tab go on to the next field and back. Typed values stop at the hard limits.
 * - Over it: Ctrl and the wheel step it, Shift a tenth as far; minus flips its sign; Ctrl C
 *   and Ctrl V copy and paste its value; Backspace puts back its default, where it has one.
 * - Pressed and dragged up or down over other number fields first, they all change together by the drag.
 * - A pen pressure toggle can be joined to its right end, for values pen pressure scales. */

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"

#include "editors/ui.hh"

namespace oanim {

/* Blender's slider fill and active toggle blue. */
static constexpr ImU32 COL_FILL = IM_COL32(0x47, 0x72, 0xb3, 255);

/* --- The format: "Radius %.0f px" -> label "Radius", value "%.0f px" ------------------------------ */

struct NumberFormat {
  std::string label;
  std::string value;
};

static NumberFormat split_format(const char *format, bool is_int)
{
  NumberFormat f;
  const std::string text = format ? format : "";
  size_t at = std::string::npos;
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '%') {
      if (i + 1 < text.size() && text[i + 1] == '%') {
        i++;
        continue;
      }
      at = i;
      break;
    }
  }
  if (at == std::string::npos) {
    f.label = text;
    f.value = is_int ? "%d" : "%.3f";
    return f;
  }
  f.label = text.substr(0, at);
  while (!f.label.empty() && std::isspace(static_cast<unsigned char>(f.label.back()))) {
    f.label.pop_back();
  }
  f.value = text.substr(at);
  return f;
}

static std::string format_value(const NumberFormat &f, double v, bool is_int)
{
  char buf[96];
  if (is_int) {
    std::snprintf(buf, sizeof(buf), f.value.c_str(), int(std::lround(v)));
  }
  else {
    std::snprintf(buf, sizeof(buf), f.value.c_str(), v);
  }
  return buf;
}

/* The number alone, for typing into (its own format's precision, no decorations). */
static std::string format_plain(const NumberFormat &f, double v, bool is_int)
{
  if (is_int) {
    return std::to_string(int(std::lround(v)));
  }
  int precision = 3;
  if (const char *dot = std::strchr(f.value.c_str(), '.')) {
    precision = std::clamp(std::atoi(dot + 1), 0, 9);
  }
  /* Enough places to keep what's there (a value finer than the field shows isn't cut by typing). */
  char buf[64];
  for (int p = precision; p <= 9; p++) {
    std::snprintf(buf, sizeof(buf), "%.*f", p, v);
    if (std::fabs(std::strtod(buf, nullptr) - v) <= 1e-7 * std::max(1.0, std::fabs(v))) {
      break;
    }
  }
  return buf;
}

/* --- Typed sums: + - * / ^, brackets, a leading sign; units (px, °, %, letters) left out ---------------- */

namespace {
struct Expr {
  const char *s;
  bool ok = true;
  void space()
  {
    while (*s && (std::isspace(static_cast<unsigned char>(*s)) || std::isalpha(static_cast<unsigned char>(*s)) ||
                  *s == '%' || static_cast<unsigned char>(*s) >= 0x80))
    {
      s++;
    }
  }
  double primary()
  {
    space();
    if (*s == '(') {
      s++;
      const double v = sum();
      space();
      if (*s == ')') {
        s++;
      }
      else {
        ok = false;
      }
      return v;
    }
    if (*s == '-' || *s == '+') {
      const bool neg = *s == '-';
      s++;
      const double v = power();
      return neg ? -v : v;
    }
    char *end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) {
      ok = false;
      return 0.0;
    }
    s = end;
    return v;
  }
  double power()
  {
    double v = primary();
    space();
    if (*s == '^') {
      s++;
      v = std::pow(v, power());
    }
    return v;
  }
  double product()
  {
    double v = power();
    for (;;) {
      space();
      if (*s == '*') {
        s++;
        v *= power();
      }
      else if (*s == '/') {
        s++;
        const double d = power();
        if (d == 0.0) {
          ok = false;
          return 0.0;
        }
        v /= d;
      }
      else {
        return v;
      }
    }
  }
  double sum()
  {
    double v = product();
    for (;;) {
      space();
      if (*s == '+') {
        s++;
        v += product();
      }
      else if (*s == '-') {
        s++;
        v -= product();
      }
      else {
        return v;
      }
    }
  }
};
}  // namespace

bool ui_number_evaluate(const char *text, double *r_value)
{
  Expr e{text};
  const double v = e.sum();
  e.space();
  if (!e.ok || *e.s != 0 || !std::isfinite(v)) {
    return false;
  }
  *r_value = v;
  return true;
}

/* --- State ----------------------------------------------------------------------------------------- */

namespace {
struct Drag {
  ImGuiID id = 0;
  double start = 0.0;
  /* The value as dragged before snapping and limits (so Ctrl and the limits don't lose motion). */
  double raw = 0.0;
  /* Where the press was (the cursor comes back there) and where the mouse was last frame. */
  ImVec2 press;
  ImVec2 last;
  bool dragging = false;
  /* Soft limits as this drag goes: widened to take in where it began. */
  double lo = 0.0, hi = 0.0;
};
struct Multi {
  /* Picking fields up and down (before the drag across), then changing them all. */
  bool picking = false, editing = false;
  ImGuiID origin = 0;
  float column = 0.0f;
  std::map<ImGuiID, double> starts;
  /* The change in the origin's units (each field takes as much, within its own limits). */
  double delta = 0.0;
  int frame = 0;
};
/* A scrub hides the cursor and reads only how far the mouse moves (the window's relative mouse mode, as
 * Blender grabs the cursor), so the drag never runs into a screen edge. The app turns it on while
 * `want` is fresh and gives the motion here; when let go, the cursor comes back at `restore`. */
struct Grab {
  bool want = false;
  bool active = false;
  int frame = 0;
  float dx = 0.0f;
  ImVec2 restore;
};
Drag g_drag;
Multi g_multi;
Grab g_grab;
int g_hover_frame = -100;
}  // namespace

bool ui_number_field_hovered()
{
  return ImGui::GetCurrentContext() && g_hover_frame >= ImGui::GetFrameCount() - 1;
}

bool ui_number_grab_wanted(float *r_x, float *r_y)
{
  *r_x = g_grab.restore.x;
  *r_y = g_grab.restore.y;
  return ImGui::GetCurrentContext() && g_grab.want && g_grab.frame >= ImGui::GetFrameCount() - 1;
}

void ui_number_grab_set_active(bool active)
{
  g_grab.active = active;
  g_grab.dx = 0.0f;
}

void ui_number_grab_motion(float dx, float /*dy*/)
{
  g_grab.dx += dx;
}

/* Asks for the grab this frame; the cursor comes back at `restore` once nothing asks. */
static void grab_cursor(ImVec2 restore)
{
  g_grab.want = true;
  g_grab.frame = ImGui::GetFrameCount();
  g_grab.restore = restore;
}

static void release_cursor(ImVec2 restore)
{
  g_grab.want = false;
  g_grab.restore = restore;
}

/* Blender's Ctrl snap (numedit_apply_snapf, linear): by the soft range, small with Shift. */
static double snap_value(double v, double lo, double hi, bool is_int, bool small)
{
  if (is_int) {
    const double step = small ? 100.0 : 10.0;
    return std::round(v / step) * step;
  }
  double range = hi - lo;
  if (!(range > 0.0) || !std::isfinite(range)) {
    range = 10.0;
  }
  range = std::min(range, 20.0);
  const double fac = small ? 0.1 : 1.0;
  if (range < 2.1) {
    return std::round(v * 10.0 / fac) * 0.1 * fac;
  }
  return std::round(v / fac) * fac;
}

/* --- Drawing ----------------------------------------------------------------------------------------- */

/* A stylus: a slanted pen, its nib down and to the left (Blender's pressure icon). */
static void draw_pen(ImDrawList *dl, ImVec2 c, float s, ImU32 col)
{
  const ImVec2 tip(c.x - s * 0.6f, c.y + s * 0.6f), top(c.x + s * 0.55f, c.y - s * 0.55f);
  const ImVec2 nib(c.x - s * 0.25f, c.y + s * 0.25f);
  dl->AddLine(nib, top, col, s * 0.42f);
  dl->AddTriangleFilled(tip, ImVec2(nib.x - s * 0.15f, nib.y - s * 0.15f + s * -0.05f), ImVec2(nib.x + s * 0.2f, nib.y + s * 0.2f),
                        col);
  dl->AddTriangleFilled(tip, ImVec2(nib.x - s * 0.22f, nib.y - s * 0.02f), ImVec2(nib.x + s * 0.02f, nib.y + s * 0.22f), col);
}

/* --- The field ------------------------------------------------------------------------------------------- */

bool ui_number(const char *id_text, double *value, const NumberField &f)
{
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) {
    return false;
  }
  ImGuiContext &g = *GImGui;
  ImGuiIO &io = g.IO;
  const ImGuiStyle &style = g.Style;
  const ImGuiID id = window->GetID(id_text);
  const NumberFormat fmt = split_format(f.format, f.is_int);
  const float h = ImGui::GetFrameHeight();
  const float w_total = std::max(ImGui::CalcItemWidth(), h);
  const float toggle_w = f.pressure ? h : 0.0f;
  const ImVec2 at = window->DC.CursorPos;
  const ImRect frame_bb(at, at + ImVec2(w_total - toggle_w, h));
  const ImRect total_bb(at, at + ImVec2(w_total, h));
  const bool limited = !(f.soft_min == 0.0 && f.soft_max == 0.0) && f.soft_max > f.soft_min;
  const bool factor = f.factor && limited;
  ImDrawList *dl = window->DrawList;

  /* The pressure toggle first (its own item), so the field is the last item (tooltips and undo after the
   * call see the field). */
  if (f.pressure) {
    const ImRect toggle_bb(ImVec2(frame_bb.Max.x, at.y), total_bb.Max);
    const ImVec2 back = window->DC.CursorPos;
    ImGui::SetCursorScreenPos(toggle_bb.Min);
    ImGui::PushID(id_text);
    const bool clicked = ImGui::InvisibleButton("##pressure", toggle_bb.GetSize());
    const bool hovered_toggle = ImGui::IsItemHovered();
    ImGui::PopID();
    if (clicked) {
      *f.pressure = !*f.pressure;
    }
    if (hovered_toggle && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", f.pressure_tip ? f.pressure_tip : "Pen pressure scales this");
    }
    const ImU32 bg = *f.pressure ? COL_FILL
                                 : ImGui::GetColorU32(hovered_toggle ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    dl->AddRectFilled(toggle_bb.Min, toggle_bb.Max, bg, style.FrameRounding, ImDrawFlags_RoundCornersRight);
    dl->AddLine(ImVec2(toggle_bb.Min.x, toggle_bb.Min.y + 2.0f), ImVec2(toggle_bb.Min.x, toggle_bb.Max.y - 2.0f),
                IM_COL32(0x2a, 0x2a, 0x2a, 255));
    draw_pen(dl, toggle_bb.GetCenter(), h * 0.38f, ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::SetCursorScreenPos(back);
  }

  ImGui::ItemSize(total_bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(frame_bb, id, &frame_bb, ImGuiItemFlags_Inputable)) {
    return false;
  }
  const bool hovered = ImGui::ItemHoverable(frame_bb, id, g.LastItemData.ItemFlags);
  if (hovered) {
    g_hover_frame = ImGui::GetFrameCount();
  }
  const double hard_lo = f.hard_min, hard_hi = f.hard_max;
  const auto hold = [&](double v) {
    v = std::clamp(v, hard_lo, hard_hi);
    return f.is_int ? std::round(v) : v;
  };
  bool changed = false;
  const auto set = [&](double v) {
    v = hold(v);
    if (v != *value) {
      *value = v;
      changed = true;
      ImGui::MarkItemEdited(id);
    }
  };
  /* A step: Ctrl and the wheel. */
  const double step = f.is_int ? 1.0 : (f.step > 0.0 ? f.step : 0.1);
  const auto step_by = [&](int dir) {
    const double s = step * (io.KeyShift && !f.is_int ? 0.1 : 1.0);
    double v = *value + double(dir) * s;
    if (limited) {
      v = dir < 0 ? std::max(v, std::min(f.soft_min, *value)) : std::min(v, std::max(f.soft_max, *value));
    }
    set(v);
  };

  bool temp_input = ImGui::TempInputIsActive(id);
  if (!temp_input) {
    const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
    const bool tabbed_in = g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput);
    if (clicked) {
      ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
      g_drag = {};
      g_drag.id = id;
      g_drag.start = g_drag.raw = *value;
      g_drag.press = io.MousePos;
      g_drag.lo = limited ? std::min(f.soft_min, *value) : -DBL_MAX;
      g_drag.hi = limited ? std::max(f.soft_max, *value) : DBL_MAX;
      ImGui::SetActiveID(id, window);
      ImGui::SetFocusID(id, window);
      ImGui::FocusWindow(window);
    }
    else if (tabbed_in) {
      temp_input = true;
    }
  }

  /* Dragging, picking fields for a drag together, or a click becoming typing. */
  if (!temp_input && g.ActiveId == id && g_drag.id == id) {
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (cancel) {
      if (g_drag.dragging) {
        set(g_drag.start);
        release_cursor(g_drag.press);
      }
      /* The others go back too (they read the change until the button is let go). */
      g_multi.delta = 0.0;
      g_multi.frame = ImGui::GetFrameCount();
      g_multi.picking = false;
      g_drag = {};
      ImGui::ClearActiveID();
    }
    else if (io.MouseDown[0]) {
      const ImVec2 d = io.MousePos - g_drag.press;
      const float threshold = 3.0f;
      if (!g_drag.dragging && !g_multi.picking && (std::fabs(d.x) > threshold || std::fabs(d.y) > threshold)) {
        if (std::fabs(d.y) > std::fabs(d.x)) {
          /* Up or down first: pick the fields passed over, then drag across to change them all. */
          g_multi = {};
          g_multi.picking = true;
          g_multi.origin = id;
          g_multi.column = g_drag.press.x;
          g_multi.starts[id] = *value;
        }
        else {
          g_drag.dragging = true;
          g_drag.last = io.MousePos;
          grab_cursor(g_drag.press);
        }
      }
      else if (g_multi.picking && g_multi.origin == id) {
        /* Across after picking: the drag starts, from where the mouse is now. */
        if (std::fabs(io.MousePos.x - g_multi.column) > threshold) {
          g_multi.picking = false;
          g_multi.editing = g_multi.starts.size() > 1;
          g_drag.dragging = true;
          g_drag.press = ImVec2(g_multi.column, io.MousePos.y);
          g_drag.last = io.MousePos;
          grab_cursor(g_drag.press);
        }
      }
      else if (g_drag.dragging) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        /* Grabbed: the motion the window reports; until then (or a script's simulated mouse) the mouse's
         * own moves. */
        float dx = io.MousePos.x - g_drag.last.x;
        if (g_grab.active) {
          dx = g_grab.dx;
          g_grab.dx = 0.0f;
        }
        g_drag.last = io.MousePos;
        grab_cursor(g_drag.press);
        const double precise = io.KeyShift ? 0.1 : 1.0;
        double per_pixel;
        if (factor) {
          per_pixel = (f.soft_max - f.soft_min) / double(std::max(frame_bb.GetWidth(), 1.0f));
        }
        else if (f.is_int && limited) {
          const double range = f.soft_max - f.soft_min;
          per_pixel = range > 256.0 ? 1.0 : range > 32.0 ? 0.5 : 1.0 / 16.0;
        }
        else {
          per_pixel = f.speed > 0.0 ? f.speed : 0.01;
        }
        g_drag.raw += double(dx) * per_pixel * precise;
        g_drag.raw = std::clamp(g_drag.raw, g_drag.lo, g_drag.hi);
        double v = g_drag.raw;
        if (io.KeyCtrl) {
          v = snap_value(v, f.soft_min, f.soft_max, f.is_int, io.KeyShift);
          v = std::clamp(v, g_drag.lo, g_drag.hi);
        }
        if (g_multi.editing && g_multi.origin == id) {
          g_multi.delta = v - g_drag.start;
          g_multi.frame = ImGui::GetFrameCount();
        }
        set(v);
      }
    }
    else {
      /* Let go. */
      if (g_drag.dragging) {
        /* The cursor shows again where it was (a factor's, at the value's place on its bar). */
        ImVec2 back = g_drag.press;
        if (factor) {
          const float t = float(std::clamp((*value - f.soft_min) / (f.soft_max - f.soft_min), 0.0, 1.0));
          back = ImVec2(frame_bb.Min.x + t * frame_bb.GetWidth(), frame_bb.GetCenter().y);
        }
        release_cursor(back);
      }
      else if (hovered && !g_multi.picking) {
        /* A click: typing into it. */
        g.NavActivateId = id;
        g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
        temp_input = true;
      }
      g_multi.picking = g_multi.editing = false;
      g_multi.starts.clear();
      g_drag = {};
      if (!temp_input) {
        ImGui::ClearActiveID();
      }
    }
  }

  /* Another field being dragged: picked up as the mouse passes down over it, then changed with it. */
  if (g_multi.origin != 0 && g_multi.origin != id) {
    const bool in_column = g_multi.column >= frame_bb.Min.x && g_multi.column <= frame_bb.Max.x;
    if (g_multi.picking && in_column && io.MousePos.y >= frame_bb.Min.y && io.MousePos.y <= frame_bb.Max.y &&
        !g_multi.starts.count(id))
    {
      g_multi.starts[id] = *value;
    }
    if (g_multi.editing && g_multi.starts.count(id) && g_multi.frame >= ImGui::GetFrameCount() - 1) {
      double v = g_multi.starts[id] + g_multi.delta;
      if (limited) {
        v = std::clamp(v, std::min(f.soft_min, g_multi.starts[id]), std::max(f.soft_max, g_multi.starts[id]));
      }
      set(v);
    }
  }
  if (g_multi.origin != 0 && g.ActiveId != g_multi.origin && !io.MouseDown[0]) {
    g_multi = {};
  }

  /* Typing. */
  if (temp_input) {
    std::string text = format_plain(fmt, *value, f.is_int);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", text.c_str());
    g.LastItemData.ItemFlags |= ImGuiItemFlags_NoMarkEdited;
    g.LastItemData.ItemFlags &= ~ImGuiItemFlags_LiveEditOnInputText;
    const bool done = ImGui::TempInputText(frame_bb, id, id_text, buf, sizeof(buf), ImGuiInputTextFlags_None);
    g.LastItemData.ItemFlags &= ~ImGuiItemFlags_NoMarkEdited;
    if (done) {
      double v;
      if (ui_number_evaluate(buf, &v)) {
        set(v);
      }
    }
    return changed;
  }

  /* Over it: keys and the wheel. */
  if (hovered && g.ActiveId == 0 && !io.WantTextInput) {
    if (io.KeyCtrl && io.MouseWheel != 0.0f && !factor) {
      step_by(io.MouseWheel > 0.0f ? 1 : -1);
      io.MouseWheel = 0.0f;
    }
    if (!io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))) {
      set(-*value);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
      ImGui::SetClipboardText(format_plain(fmt, *value, f.is_int).c_str());
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
      double v;
      if (const char *clip = ImGui::GetClipboardText(); clip && ui_number_evaluate(clip, &v)) {
        set(v);
      }
    }
    if (!std::isnan(f.default_value) && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
      set(f.default_value);
    }
  }

  /* The field. */
  const bool active = g.ActiveId == id;
  const bool picked = g_multi.starts.count(id) && (g_multi.picking || g_multi.editing);
  const ImU32 frame_col = ImGui::GetColorU32(active || picked ? ImGuiCol_FrameBgActive
                                             : hovered           ? ImGuiCol_FrameBgHovered
                                                                 : ImGuiCol_FrameBg);
  const ImDrawFlags corners = f.pressure ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersAll;
  dl->AddRectFilled(frame_bb.Min, frame_bb.Max, frame_col, style.FrameRounding, corners);
  if (factor) {
    const float t = float(std::clamp((*value - f.soft_min) / (f.soft_max - f.soft_min), 0.0, 1.0));
    if (t > 0.0f) {
      const float x = frame_bb.Min.x + t * frame_bb.GetWidth();
      const bool full = x >= frame_bb.Max.x - style.FrameRounding;
      dl->AddRectFilled(frame_bb.Min, ImVec2(std::max(x, frame_bb.Min.x + 2.0f), frame_bb.Max.y), COL_FILL,
                        style.FrameRounding,
                        full ? corners : ImDrawFlags(ImDrawFlags_RoundCornersLeft));
    }
  }
  if (style.FrameBorderSize > 0.0f) {
    dl->AddRect(frame_bb.Min, frame_bb.Max, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding, corners,
                style.FrameBorderSize);
  }
  const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
  const std::string shown = format_value(fmt, *value, f.is_int);
  const float pad = std::max(style.FramePadding.x, h * 0.35f);
  const ImVec2 value_size = ImGui::CalcTextSize(shown.c_str());
  ImGui::PushClipRect(frame_bb.Min, frame_bb.Max, true);
  const float text_y = frame_bb.GetCenter().y - value_size.y * 0.5f;
  if (!fmt.label.empty()) {
    /* The value always shows whole; a label that doesn't fit beside it loses its end, as Blender's does. */
    const float gap = ImGui::CalcTextSize(" ").x * 2.0f;
    const float room = frame_bb.GetWidth() - pad * 2.0f - value_size.x - gap;
    std::string label = fmt.label;
    if (ImGui::CalcTextSize(label.c_str()).x > room) {
      while (!label.empty() && ImGui::CalcTextSize((label + "..").c_str()).x > room) {
        label.pop_back();
      }
      label = label.empty() ? std::string() : label + "..";
    }
    dl->AddText(ImVec2(frame_bb.Min.x + pad, text_y), text_col, label.c_str());
    dl->AddText(ImVec2(frame_bb.Max.x - pad - value_size.x, text_y), text_col, shown.c_str());
  }
  else {
    dl->AddText(ImVec2(frame_bb.GetCenter().x - value_size.x * 0.5f, text_y), text_col, shown.c_str());
  }
  ImGui::PopClipRect();
  return changed;
}

/* --- The ImGui-shaped helpers the editors call ------------------------------------------------------------ */

void ui_number_hard_limits(float soft_min, float soft_max, const char *format, float *r_hard_min, float *r_hard_max)
{
  const bool auto_min = std::isnan(*r_hard_min), auto_max = std::isnan(*r_hard_max);
  const bool limited = !(soft_min == 0.0f && soft_max == 0.0f);
  if (auto_min) {
    *r_hard_min = limited && soft_min >= 0.0f ? soft_min : -FLT_MAX;
  }
  if (auto_max) {
    const bool percent = format && std::strstr(format, "%%");
    const bool fraction = limited && soft_min >= 0.0f && (soft_max == 1.0f || (percent && soft_max == 100.0f));
    *r_hard_max = fraction ? soft_max : FLT_MAX;
  }
}

/* Width for each of `n` fields side by side, as ImGui's DragFloat2 and DragFloat3. */
template<typename T>
static bool number_row(const char *id, T *v, int n, const NumberField &base)
{
  ImGui::BeginGroup();
  ImGui::PushID(id);
  ImGui::PushMultiItemsWidths(n, ImGui::CalcItemWidth());
  bool changed = false;
  for (int k = 0; k < n; k++) {
    ImGui::PushID(k);
    if (k > 0) {
      ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    }
    double d = double(v[k]);
    if (ui_number("##v", &d, base)) {
      v[k] = T(d);
      changed = true;
    }
    ImGui::PopID();
    ImGui::PopItemWidth();
  }
  ImGui::PopID();
  ImGui::EndGroup();
  return changed;
}

static NumberField field_of(float speed, float soft_min, float soft_max, const char *format, float hard_min,
                            float hard_max, bool is_int, bool factor)
{
  ui_number_hard_limits(soft_min, soft_max, format, &hard_min, &hard_max);
  NumberField f;
  f.is_int = is_int;
  f.factor = factor;
  f.soft_min = soft_min;
  f.soft_max = soft_max;
  f.hard_min = hard_min;
  f.hard_max = hard_max;
  f.speed = speed;
  /* Ctrl and the wheel's step: ten pixels' worth of drag. */
  f.step = double(speed) * 10.0;
  f.format = format;
  return f;
}

bool ui_drag_float(const char *id, float *v, float speed, float soft_min, float soft_max, const char *format, int flags,
                   float hard_min, float hard_max)
{
  (void)flags;
  double d = double(*v);
  const bool changed = ui_number(id, &d, field_of(speed, soft_min, soft_max, format, hard_min, hard_max, false, false));
  if (changed) {
    *v = float(d);
  }
  return changed;
}

bool ui_drag_float2(const char *id, float v[2], float speed, float soft_min, float soft_max, const char *format,
                    int flags, float hard_min, float hard_max)
{
  (void)flags;
  return number_row(id, v, 2, field_of(speed, soft_min, soft_max, format, hard_min, hard_max, false, false));
}

bool ui_drag_float3(const char *id, float v[3], float speed, float soft_min, float soft_max, const char *format,
                    int flags, float hard_min, float hard_max)
{
  (void)flags;
  return number_row(id, v, 3, field_of(speed, soft_min, soft_max, format, hard_min, hard_max, false, false));
}

bool ui_drag_int(const char *id, int *v, float speed, int soft_min, int soft_max, const char *format, int flags,
                 float hard_min, float hard_max)
{
  (void)flags;
  NumberField f = field_of(speed, float(soft_min), float(soft_max), format, hard_min, hard_max, true, false);
  f.hard_min = std::max(f.hard_min, double(INT_MIN));
  f.hard_max = std::min(f.hard_max, double(INT_MAX));
  double d = double(*v);
  const bool changed = ui_number(id, &d, f);
  if (changed) {
    *v = int(std::lround(d));
  }
  return changed;
}

bool ui_slider_float(const char *id, float *v, float soft_min, float soft_max, const char *format, int flags,
                     float hard_min, float hard_max)
{
  (void)flags;
  double d = double(*v);
  const bool changed = ui_number(
      id, &d, field_of((soft_max - soft_min) / 200.0f, soft_min, soft_max, format, hard_min, hard_max, false, true));
  if (changed) {
    *v = float(d);
  }
  return changed;
}

bool ui_slider_int(const char *id, int *v, int soft_min, int soft_max, const char *format, int flags, float hard_min,
                   float hard_max)
{
  /* Whole numbers drag by the step, as Blender's integer sliders. */
  return ui_drag_int(id, v, 0.1f, soft_min, soft_max, format, flags, hard_min, hard_max);
}

}  // namespace oanim
