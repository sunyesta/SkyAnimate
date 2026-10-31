#include "editors/text_tool.hh"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "data/document.hh"
#include "data/text.hh"
#include "ops/undo.hh"
#include "text/text_layer.hh"
#include "text/text_outline.hh"
#include "wm/context.hh"

namespace oanim {

namespace {

struct Editing {
  uint64_t layer = 0;
  /* The caret, and where the selection began (the same: nothing selected). */
  int cursor = 0;
  int anchor = 0;
  /* The text when typing began, and each change since (Ctrl Z steps back through them). */
  TextSettings before;
  std::vector<TextSettings> history;
  bool focus = false;
  int focus_wait = 0;
  bool selecting = false;
  double blink_from = 0.0;
};

Editing g_edit;

}  // namespace

bool text_tool_editing(uint64_t *r_layer, int *r_from, int *r_to)
{
  if (g_edit.layer == 0) {
    return false;
  }
  if (r_layer) {
    *r_layer = g_edit.layer;
  }
  if (r_from) {
    *r_from = std::min(g_edit.cursor, g_edit.anchor);
  }
  if (r_to) {
    *r_to = std::max(g_edit.cursor, g_edit.anchor);
  }
  return true;
}

void text_tool_finish(Context &C)
{
  if (g_edit.layer == 0) {
    return;
  }
  Document &doc = *C.document;
  const uint64_t id = g_edit.layer;
  const TextSettings before = g_edit.before;
  g_edit = {};
  C.wm->text_editing = false;
  const Layer *layer = layer_find(doc, id);
  if (layer && layer->is_text() && !(layer->text == before)) {
    document_sync_text(doc);
    C.undo->push("Edit Text", doc);
  }
}

static bool is_space(char16_t c)
{
  return c == u' ' || c == u'\n' || c == u'\t';
}

/* Positions a step left or right: a letter (a surrogate pair whole), or with `word` to the start or end
 * of a word. */
static int step(const std::u16string &s, int pos, int dir, bool word)
{
  const int n = int(s.size());
  if (!word) {
    pos += dir;
    if (pos > 0 && pos < n && s[size_t(pos)] >= 0xDC00 && s[size_t(pos)] <= 0xDFFF) {
      pos += dir;
    }
    return std::clamp(pos, 0, n);
  }
  if (dir < 0) {
    while (pos > 0 && is_space(s[size_t(pos - 1)])) {
      pos--;
    }
    while (pos > 0 && !is_space(s[size_t(pos - 1)])) {
      pos--;
    }
  }
  else {
    while (pos < n && !is_space(s[size_t(pos)])) {
      pos++;
    }
    while (pos < n && is_space(s[size_t(pos)])) {
      pos++;
    }
  }
  return pos;
}

static void begin(Context &C, Layer &layer, int pos)
{
  if (g_edit.layer != layer.id) {
    text_tool_finish(C);
    g_edit = {};
    g_edit.layer = layer.id;
    g_edit.before = layer.text;
  }
  g_edit.cursor = g_edit.anchor = pos;
  g_edit.focus = true;
  g_edit.blink_from = ImGui::GetTime();
  C.wm->text_editing = true;
}

/* The active layer when it is text the mouse is over (its lines' box, a few pixels wider), and the
 * mouse in its space. */
static Layer *active_text_under(Document &doc, float2 p, float slack, float2 *r_local)
{
  Layer *layer = document_active_layer(doc);
  if (!layer || !layer->is_text() || layer->locked || !layer->shown()) {
    return nullptr;
  }
  const Transform2D to_layer = layer_world_transform(doc, layer->id).inverted();
  const float2 q = to_layer.apply(p);
  const float s = slack * to_layer.scale();
  const TextBox box = text_bounds(layer->text);
  if (q.x >= box.min.x - s && q.x <= box.max.x + s && q.y >= box.min.y - s && q.y <= box.max.y + s) {
    *r_local = q;
    return layer;
  }
  return nullptr;
}

bool text_editing_wanted(Context &C, const TextToolView &, bool)
{
  /* Typing goes on until it's finished; the Edit Text tool has the viewport while a text layer is active. */
  const Layer *active = document_active_layer(*C.document);
  return g_edit.layer != 0 || (C.wm->tool_settings.tool == Tool::EditText && active && active->is_text());
}

void text_editing_begin_at(Context &C, const TextToolView &view, ImVec2 mouse)
{
  Layer *layer = document_active_layer(*C.document);
  if (!layer || !layer->is_text() || layer->locked) {
    return;
  }
  const float2 local = layer_world_transform(*C.document, layer->id).inverted().apply(view.to_document(mouse));
  begin(C, *layer, text_hit(layer->text, local));
}

namespace {

/* A side handle of the Edit Text tool being dragged: the text's lines wrap where it is. */
struct WrapDrag {
  bool active = false;
  int side = 0;
  uint64_t layer = 0;
  float2 position;
  Transform2D parent, world;
  float2 lo, hi;
  /* Where its edges are now (the layer's space as the drag began). */
  float left = 0.0f, right = 0.0f;
};
WrapDrag g_wrap;

}  // namespace

/* The Edit Text tool's handles: one each side of the text, halfway up. Dragging one wraps the lines at
 * that width (point text becomes a text box, its left edge where the text's was), double-clicking one
 * makes it point text again. True while one has the mouse. */
static bool wrap_handles(Context &C, Layer &layer, const TextToolView &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  ImGuiIO &io = ImGui::GetIO();
  const Transform2D world = g_wrap.active ? g_wrap.world : layer_world_transform(doc, layer.id);
  const TextBox box = text_bounds(layer.text);
  const float2 lo = g_wrap.active ? g_wrap.lo : box.min, hi = g_wrap.active ? g_wrap.hi : box.max;
  const float mid = (lo.y + hi.y) * 0.5f;
  int over = 0;
  for (const int side : {-1, 1}) {
    const ImVec2 p = view.to_screen(world.apply({side < 0 ? lo.x : hi.x, mid}));
    if (std::fabs(io.MousePos.x - p.x) <= 7.0f && std::fabs(io.MousePos.y - p.y) <= 9.0f) {
      over = side;
    }
  }
  if (hovered && over != 0 && !g_wrap.active) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      /* Point text again: no wrapping, its left edge kept where it is. */
      if (layer.text.box_width > 0.0f) {
        const Transform2D parent = world * layer.local_transform().inverted();
        layer.text.box_width = 0.0f;
        const float new_left = text_bounds(layer.text).min.x;
        layer.position = parent.inverted().apply(world.apply({lo.x - new_left, 0.0f}));
        document_sync_text(doc);
        C.undo->push("Point Text", doc);
      }
      return true;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      g_wrap = {true, over, layer.id, layer.position, world * layer.local_transform().inverted(), world, lo, hi,
                lo.x, hi.x};
    }
  }
  if (g_wrap.active && g_wrap.layer == layer.id) {
    /* The edge dragged follows the mouse, the other stays; the box starts at its left edge. */
    const float2 m = g_wrap.world.inverted().apply(view.to_document(io.MousePos));
    const float least = 0.02f;
    const float left = g_wrap.side < 0 ? std::min(m.x, g_wrap.hi.x - least) : g_wrap.lo.x;
    const float right = g_wrap.side > 0 ? std::max(m.x, g_wrap.lo.x + least) : g_wrap.hi.x;
    layer.text.box_width = right - left;
    g_wrap.left = left;
    g_wrap.right = right;
    layer.position = g_wrap.parent.inverted().apply(g_wrap.world.apply({left, 0.0f}));
    document_sync_text(doc);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      g_wrap = {};
      C.undo->push("Text Box Width", doc);
    }
  }
  /* The handles: bars a text box's edges are, dots on point text's ends. */
  for (const int side : {-1, 1}) {
    if (!doc.overlays.show) {
      break;
    }
    const float x = g_wrap.active ? (side < 0 ? g_wrap.left : g_wrap.right) : (side < 0 ? lo.x : hi.x);
    const ImVec2 p = view.to_screen(world.apply({x, mid}));
    const bool lit = over == side || (g_wrap.active && g_wrap.side == side);
    const ImU32 fill = lit ? IM_COL32(0xff, 0xa5, 0x30, 255) : IM_COL32(0xff, 0xff, 0xff, 255);
    dl->AddRectFilled(ImVec2(p.x - 3.5f, p.y - 8.5f), ImVec2(p.x + 3.5f, p.y + 8.5f), IM_COL32(0, 0, 0, 160), 2.0f);
    dl->AddRectFilled(ImVec2(p.x - 2.5f, p.y - 7.5f), ImVec2(p.x + 2.5f, p.y + 7.5f), fill, 1.5f);
  }
  return g_wrap.active || (hovered && over != 0);
}

static int typed_filter(ImGuiInputTextCallbackData *data)
{
  auto *typed = static_cast<std::u16string *>(data->UserData);
  const ImWchar c = data->EventChar;
  if (c == '\r' || c == '\n') {
    typed->push_back(u'\n');
  }
  else if (c >= 32 && c != 127) {
    typed->push_back(char16_t(c));
  }
  /* Taken here: the hidden field only collects what is typed. */
  return 1;
}

void text_editing_input(Context &C, const TextToolView &view, bool hovered, ImDrawList *dl)
{
  Document &doc = *C.document;
  ImGuiIO &io = ImGui::GetIO();
  const float2 mouse = view.to_document(io.MousePos);
  const float slack = 6.0f / std::max(view.zoom, 1e-6f);

  /* Typing ends when its layer goes or stops being the active one. */
  Layer *editing = g_edit.layer ? layer_find(doc, g_edit.layer) : nullptr;
  if (g_edit.layer && (!editing || !editing->is_text() || editing->id != doc.active_layer)) {
    text_tool_finish(C);
    editing = nullptr;
  }

  /* --- The mouse. --- */
  /* The wrap handles first (the Edit Text tool's own). */
  if (C.wm->tool_settings.tool == Tool::EditText) {
    if (Layer *active = document_active_layer(doc);
        active && active->is_text() && !active->locked && (doc.overlays.show || g_wrap.active) &&
        wrap_handles(C, *active, view, hovered, dl))
    {
      return;
    }
  }
  float2 local;
  Layer *under = hovered ? active_text_under(doc, mouse, slack, &local) : nullptr;
  if (under) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (editing && under) {
      const int pos = text_hit(editing->text, local);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const std::u16string s = utf8_to_utf16(text_string(editing->text));
        g_edit.anchor = step(s, std::min(pos + 1, int(s.size())), -1, true);
        g_edit.cursor = step(s, g_edit.anchor, 1, true);
        while (g_edit.cursor > g_edit.anchor && is_space(s[size_t(g_edit.cursor - 1)])) {
          g_edit.cursor--;
        }
      }
      else {
        g_edit.cursor = pos;
        if (!io.KeyShift) {
          g_edit.anchor = pos;
        }
        g_edit.selecting = true;
      }
      g_edit.focus = true;
      g_edit.blink_from = ImGui::GetTime();
    }
    else if (under) {
      begin(C, *under, text_hit(under->text, local));
      g_edit.selecting = true;
      editing = under;
    }
    else {
      /* A click away: typing is done. */
      text_tool_finish(C);
      return;
    }
  }
  if (g_edit.selecting && editing) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const Transform2D to_layer = layer_world_transform(doc, editing->id).inverted();
      g_edit.cursor = text_hit(editing->text, to_layer.apply(mouse));
    }
    else {
      g_edit.selecting = false;
    }
  }
  if (!editing) {
    if (under && doc.overlays.show) {
      /* What a click would type in: its lines' box, faint. */
      const Transform2D to_document = layer_world_transform(doc, under->id);
      const TextBox b = text_bounds(under->text);
      const ImVec2 q[4] = {view.to_screen(to_document.apply(b.min)),
                           view.to_screen(to_document.apply({b.max.x, b.min.y})),
                           view.to_screen(to_document.apply(b.max)),
                           view.to_screen(to_document.apply({b.min.x, b.max.y}))};
      dl->AddPolyline(q, 4, IM_COL32(0xff, 0xff, 0xff, 90), ImDrawFlags_Closed, 1.0f);
    }
    return;
  }

  /* --- The keys, through a hidden field that has the keyboard while typing (so the viewport's
   * shortcuts are letters, and an input method can show its candidates at the caret). --- */
  Layer &layer = *editing;
  const Transform2D to_document = layer_world_transform(doc, layer.id);
  const ImVec2 caret_top = view.to_screen(to_document.apply(text_caret(layer.text, g_edit.cursor).top));

  std::u16string typed;
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
  const ImVec2 keep = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(caret_top);
  if (g_edit.focus) {
    ImGui::SetKeyboardFocusHere();
    g_edit.focus = false;
    /* The field has the keyboard from the next frame: until then it isn't lost. */
    g_edit.focus_wait = 3;
  }
  else if (g_edit.focus_wait > 0) {
    g_edit.focus_wait--;
  }
  static char sink[8] = {};
  sink[0] = '\0';
  ImGui::InputTextMultiline("##text_tool", sink, sizeof(sink), ImVec2(1.0f, 1.0f),
                            ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_NoHorizontalScroll,
                            typed_filter, &typed);
  const bool has_keys = ImGui::IsItemActive();
  ImGui::SetCursorScreenPos(keep);
  ImGui::PopStyleVar();

  const std::u16string s = utf8_to_utf16(text_string(layer.text));
  const int n = int(s.size());
  g_edit.cursor = std::clamp(g_edit.cursor, 0, n);
  g_edit.anchor = std::clamp(g_edit.anchor, 0, n);
  const int from = std::min(g_edit.cursor, g_edit.anchor), to = std::max(g_edit.cursor, g_edit.anchor);
  const bool shift = io.KeyShift, ctrl = io.KeyCtrl;
  const auto changed = [&]() {
    g_edit.history.push_back(layer.text);
    if (g_edit.history.size() > 200) {
      g_edit.history.erase(g_edit.history.begin());
    }
  };
  const auto replace = [&](int a, int b, const std::u16string &with) {
    changed();
    text_replace(layer.text, a, b, utf16_to_utf8(with));
    g_edit.cursor = g_edit.anchor = a + int(with.size());
    g_edit.blink_from = ImGui::GetTime();
  };
  const auto move = [&](int pos) {
    g_edit.cursor = std::clamp(pos, 0, n);
    if (!shift) {
      g_edit.anchor = g_edit.cursor;
    }
    g_edit.blink_from = ImGui::GetTime();
  };
  const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };

  if (!typed.empty()) {
    replace(from, to, typed);
  }
  else if (pressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
    /* Esc: done typing, back to moving the layer (as double-clicking it began typing). */
    const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    text_tool_finish(C);
    if (esc && C.wm->tool_settings.tool == Tool::EditText) {
      C.wm->tool_settings.tool = Tool::TransformLayer;
    }
    return;
  }
  else if (pressed(ImGuiKey_Backspace)) {
    if (from != to) {
      replace(from, to, u"");
    }
    else if (from > 0) {
      replace(step(s, from, -1, ctrl), from, u"");
    }
  }
  else if (pressed(ImGuiKey_Delete)) {
    if (from != to) {
      replace(from, to, u"");
    }
    else if (from < n) {
      replace(from, step(s, from, 1, ctrl), u"");
    }
  }
  else if (pressed(ImGuiKey_LeftArrow)) {
    move(from != to && !shift ? from : step(s, g_edit.cursor, -1, ctrl));
  }
  else if (pressed(ImGuiKey_RightArrow)) {
    move(from != to && !shift ? to : step(s, g_edit.cursor, 1, ctrl));
  }
  else if (pressed(ImGuiKey_UpArrow)) {
    move(text_line_step(layer.text, g_edit.cursor, -1));
  }
  else if (pressed(ImGuiKey_DownArrow)) {
    move(text_line_step(layer.text, g_edit.cursor, 1));
  }
  else if (pressed(ImGuiKey_Home)) {
    move(ctrl ? 0 : text_line_start(layer.text, g_edit.cursor));
  }
  else if (pressed(ImGuiKey_End)) {
    move(ctrl ? n : text_line_end(layer.text, g_edit.cursor));
  }
  else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
    g_edit.anchor = 0;
    g_edit.cursor = n;
  }
  else if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_X, false))) {
    /* After the hidden field's own copy (of nothing), so this is what the clipboard holds. */
    if (from != to) {
      ImGui::SetClipboardText(utf16_to_utf8(s.substr(size_t(from), size_t(to - from))).c_str());
      if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        replace(from, to, u"");
      }
    }
  }
  else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, true)) {
    if (!g_edit.history.empty()) {
      layer.text = g_edit.history.back();
      g_edit.history.pop_back();
      g_edit.cursor = g_edit.anchor = std::min(g_edit.cursor, text_length(layer.text));
    }
  }
  else if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_B, false) || ImGui::IsKeyPressed(ImGuiKey_I, false))) {
    /* Bold or italic, on the selection (or all of it with nothing selected), as the first letter is not. */
    const bool bold = ImGui::IsKeyPressed(ImGuiKey_B, false);
    changed();
    const int a = from != to ? from : 0, b = from != to ? to : n;
    std::vector<TextRun *> runs = text_runs_between(layer.text, a, b);
    const bool on = !runs.empty() && (bold ? runs.front()->weight >= 600 : runs.front()->italic);
    for (TextRun *run : runs) {
      if (bold) {
        run->weight = on ? 400 : 700;
      }
      else {
        run->italic = !on;
      }
    }
    text_tidy(layer.text);
  }
  /* Letters it was given are its letters at once, for the caret and selection drawn below. */
  document_sync_text(doc);
  if (!has_keys && g_edit.focus_wait == 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    /* The keyboard went elsewhere (a field in another editor): typing here is done. */
    text_tool_finish(C);
    return;
  }

  /* --- What it looks like: the selection, the caret, a text box's edges. --- */
  for (const TextBox &b : text_selection(layer.text, std::min(g_edit.cursor, g_edit.anchor),
                                         std::max(g_edit.cursor, g_edit.anchor)))
  {
    const ImVec2 q[4] = {view.to_screen(to_document.apply(b.min)), view.to_screen(to_document.apply({b.max.x, b.min.y})),
                         view.to_screen(to_document.apply(b.max)), view.to_screen(to_document.apply({b.min.x, b.max.y}))};
    dl->AddConvexPolyFilled(q, 4, IM_COL32(0x47, 0x72, 0xb3, 110));
  }
  const TextCaret now = text_caret(layer.text, g_edit.cursor);
  const ImVec2 top = view.to_screen(to_document.apply(now.top)), bottom = view.to_screen(to_document.apply(now.bottom));
  if (std::fmod(ImGui::GetTime() - g_edit.blink_from, 1.06) < 0.53) {
    dl->AddLine(top, bottom, IM_COL32(0xff, 0xff, 0xff, 255), 3.0f);
    dl->AddLine(top, bottom, IM_COL32(0x10, 0x10, 0x10, 255), 1.5f);
  }
  if (!doc.overlays.show) {
    return;
  }
  const TextBox b = text_bounds(layer.text);
  const ImVec2 q[4] = {view.to_screen(to_document.apply(b.min)), view.to_screen(to_document.apply({b.max.x, b.min.y})),
                       view.to_screen(to_document.apply(b.max)), view.to_screen(to_document.apply({b.min.x, b.max.y}))};
  dl->AddPolyline(q, 4, layer.text.box_width > 0.0f ? IM_COL32(0xff, 0xff, 0xff, 150) : IM_COL32(0xff, 0xff, 0xff, 60),
                  ImDrawFlags_Closed, 1.0f);
}

}  // namespace oanim
