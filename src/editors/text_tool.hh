#pragma once

/* Typing on the canvas: the Edit Text tool (a text layer's) clicks into its text to type, with a caret
 * and selection as in Blender's text edit mode, and its side handles set where the lines wrap; Esc, Tab or
 * a click away finishes, as one undo step (Esc back to Transform Layer, as a double-click with that began
 * typing). */

#include <cstdint>
#include <functional>

#include "core/math.hh"
#include "imgui.h"

namespace oanim {

struct Context;

/* The viewport's mapping, document space to the screen and back. */
struct TextToolView {
  std::function<ImVec2(float2)> to_screen;
  std::function<float2(ImVec2)> to_document;
  float zoom = 1.0f;
};

/* Whether the viewport's mouse goes to the text: typing, or over the active text layer's text. */
bool text_editing_wanted(Context &C, const TextToolView &view, bool hovered);
void text_editing_input(Context &C, const TextToolView &view, bool hovered, ImDrawList *dl);
/* Typing in the active text layer, the caret where `mouse` (screen) is: a double-click with Transform
 * Layer. */
void text_editing_begin_at(Context &C, const TextToolView &view, ImVec2 mouse);
/* Whether text is being typed (the layer, and the selection: from == to for just the caret). */
bool text_tool_editing(uint64_t *r_layer = nullptr, int *r_from = nullptr, int *r_to = nullptr);
/* Finishes typing (Esc, a click away, another layer made active): one undo step for what was typed. */
void text_tool_finish(Context &C);

}  // namespace oanim
