#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/* Shared widgets for editors, like Blender's UI layout: panels of properties drawn from their
 * RNA descriptions. */

#include "rna/rna.hh"

namespace oanim {

struct Context;
struct Document;
class UndoStack;
struct Layer;
struct WindowManager;

/* A collapsible panel with a two-column (label, value) table. Call ui_panel_end() only when
 * this returns true. */
bool ui_panel_begin(const char *title, bool default_open = true);
/* The same with what the header says instead of `title` (which stays the panel's name, so it keeps its
 * fold), in `color` (0: the usual). */
bool ui_panel_begin(const char *title, const std::string &header, unsigned int color);
/* Panels open or folded as the project keeps them (Document::ui_open, saved with it): the editors
 * get the project each frame (ui_bind_project). `key` is "<editor>/<panel>"; ui_fold_scope() is the
 * editor of the window being drawn ("Properties"). ui_fold_next() opens or folds the next
 * collapsing header or tree node as kept; ui_fold_record() keeps what it shows (changing it is
 * unsaved). */
void ui_bind_project(Document *doc, UndoStack *undo);
std::string ui_fold_scope();
bool ui_fold_state(const std::string &key, bool default_open);
void ui_fold_record(const std::string &key, bool default_open, bool open);
void ui_fold_next(const std::string &key, bool default_open);
/* ImGui::CollapsingHeader, open or folded as the project keeps it (keyed by this window's editor
 * and `label`). */
bool ui_collapsing_header(const char *label, bool default_open);
/* Blender's panel boxes: between these, every panel and collapsing header is a rounded box with a
 * grip at its header's right that drags it to another place; `scope` keeps their order ("Properties/
 * Shape": Document::ui_panel_order, once one was dragged). A box runs to the next one. */
void ui_boxes_begin(const std::string &scope);
void ui_boxes_end();
/* A sub-panel inside a panel (Blender's): a folding row (a table row, inside a panel's table) whose
 * rows, while open, sit on a darker band. `key` keeps its fold; call ui_subpanel_end() when it's open. */
bool ui_subpanel_begin(const std::string &key, const std::string &label, bool default_open);
void ui_subpanel_end();

/* Drag-and-drop reordering, as the Layers editor shows it: no yellow box, a blue line where the
 * dragged item will go. */
inline constexpr unsigned int UI_DROP_LINE_COLOR = 0xFFFFB86Fu; /* IM_COL32(0x6f, 0xb8, 0xff, 255) */
void ui_panel_end();

/* One property row inside a panel. Edits push an undo step when the widget is released.
 * `python_path` is shown in the tooltip, e.g. "oanim.data.document". */
void ui_prop(Context &C, const rna::PointerRNA &ptr, const char *identifier, const char *python_path);
/* Blender's keyframe button: state 0 not animated (a dot), 1 animated (a hollow diamond), 2 keyed
 * on this frame (a filled one). Called before ui_prop / ui_prop_pair, the next row's fields get
 * Blender's tint for the state (green animated, yellow keyed here) and the button at their right;
 * `on_click` runs when it is pressed. `pin` adds the pin button after it (-1 none, 0 not pinned,
 * 1 pinned, which also tints the row red); `on_pin` runs when that is pressed. */
/* `flash_layer`, `flash_path`: the channel the row shows, so a flash asked for it finds it. */
void ui_key_next(int state, std::function<void()> on_click, int pin = -1, std::function<void()> on_pin = {},
                 uint64_t flash_layer = 0, const std::string &flash_path = std::string());

/* Showing a property in the Properties editor (WindowManager::properties_flash, asked for from the
 * timeline's Keyed Here list): a Properties editor binds it while it draws (null after), its
 * panels open for it, and the row of a keyed field that matches flashes. */
void ui_flash_bind(WindowManager *wm);
/* Where a keyed field drew its key button (the last item): flashes the row when it is the property
 * asked for, scrolling to it the first time. */
void ui_flash_row(uint64_t layer, const std::string &path);
/* Once a frame, after the editors drew: ends a flash after its second, or, with no Properties
 * editor to show it in, says so at the mouse. */
void ui_flash_update(WindowManager &wm);
/* For rows drawn by hand: the tint (pop with ImGui::PopStyleColor(ui_key_tint_push(...))) and
 * the button itself, after the field (returns true when pressed). */
int ui_key_tint_push(int state);
bool ui_key_button(const char *id, int state);
/* The pin button after a key button: pin -1 leaves its place empty, 0 grey, 1 red (pinned to the
 * timeline). Returns true when pressed. */
bool ui_pin_button(const char *id, int pin);
/* A pinned property's row, tinted red (call after drawing the row). */
void ui_pinned_row();
/* The pin, drawn into a square at `min` of size `size` (also used by the timeline). */
void ui_pin_draw(ImDrawList *dl, ImVec2 min, float size, unsigned int color);
/* The width a key button and the pin's place take, with their spacing. */
float ui_key_button_width();
/* A row drawn by hand: its label, then `body` (given the width it has, the key button's room left
 * out), then the key button if ui_key_next asked for one. */
void ui_row(const char *label, const std::function<void(float width)> &body);
/* A Bool property as Blender's checkbox, its name to the right of the box, in the value column;
 * `heading` in the label column names the group (empty for the rest of it): "Audio  [x] Scrubbing". */
void ui_prop_check(Context &C, const rna::PointerRNA &ptr, const char *identifier, const char *python_path,
                   const char *heading = "");
/* Two properties side by side under one label, as Blender shows Location X and Y:
 * "Location  [X 0.000] [Y 0.000]". `between` (optional) draws a square button between them (the
 * resolution's link). */
void ui_prop_pair(Context &C, const rna::PointerRNA &ptr, const char *label, const char *first,
                  const char *first_prefix, const char *second, const char *second_prefix, const char *python_path,
                  const std::function<void()> &between = {});

/* A colour row inside a panel; returns true when the colour changed. */
/* The status bar along the bottom of the window (Blender's), with its reports banner: the latest
 * report, coloured by type, for a few seconds; hovering shows all of it, clicking lists the recent
 * ones. Drawn before the docking space, which then sits above it. */
void ui_status_bar_draw();
/* A square revert button (a curved arrow turning back), `tooltip` on hover. True when clicked. */
bool ui_revert_button(const char *id, const char *tooltip);
/* How much room a revert button takes after a field (its width and the gap before it). */
float ui_revert_button_space();
/* `r_revert` (optional): also a revert button at the right end of the row, true when clicked. */
/* `r_pick` (optional): an eyedropper button at the row's very end (lit while `pick_active`), true when
 * clicked. */
bool ui_color(Context &C, const char *label, float rgba[4], const char *undo_name, bool *r_revert = nullptr,
              bool *r_pick = nullptr, bool pick_active = false);

/* A colour swatch that opens Open Animate's colour picker, which shows #RRGGBBAA and has an
 * eyedropper; with `hex`, a #RRGGBBAA field beside it too. Returns true while the colour
 * changes; `r_finished` (optional) is set when an edit ends, for an undo step. */
/* `right_space`: room left free at the right of the hex field (for a button after it). */
/* `swatch_width`: the swatch's width (0: the usual). */
bool ui_color_edit(const char *id, float rgba[4], bool hex, bool *r_finished = nullptr, float right_space = 0.0f,
                   float swatch_width = 0.0f);

/* The eyedropper picks a colour from anywhere in the window. Draw the overlay with the rest of
 * the UI; take the sample after the frame is drawn, before it is shown (it reads the pixel). */
void ui_eyedropper_overlay();
void ui_eyedropper_sample(int framebuffer_height);

/* The layer eyedropper, for a bone's custom shape: while it is on, clicking a layer in the viewport
 * or a row of the Layers editor gives that layer to the bone; Esc, right-click or a click anywhere
 * else stops it. */
void ui_layer_eyedropper_start(uint64_t armature, uint64_t bone);
bool ui_layer_eyedropper_active();
/* The same eyedropper for something else: `pick` gets the layer clicked and returns true when it's
 * done (false keeps it on); `cue` is what the mouse says. */
void ui_layer_eyedropper_start_with(std::function<bool(Context &, uint64_t)> pick, std::function<std::string()> cue);
/* In Front Of... / Behind... (layer.draw_order_pick): starts the eyedropper for it, and ends it once
 * the eyedropper is done. Called by the editors that take its click, each frame. */
void ui_draw_order_pick_update(Context &C);
/* While pointing at `target` during it: the moving layers drawn as they would be, until _end. */
bool ui_draw_order_preview_begin(Document &doc, std::vector<std::pair<Layer *, float>> &r_saved, uint64_t target,
                                 int mode, const std::vector<uint64_t> &layers);
void ui_draw_order_preview_end(std::vector<std::pair<Layer *, float>> &saved);
/* Inside... / Outside... (mask.pick): starts the layer eyedropper for it, and ends it once done. */
void ui_mask_pick_update(Context &C);
/* The Mask menu's items, for `layer` (0: the active and picked layers). */
void ui_mask_menu_items(Context &C, uint64_t layer);
/* The Draw Order menu's items, for `layer` (0: the active and picked layers). */
void ui_draw_order_menu_items(Context &C, uint64_t layer);
/* A click on `layer` while it is on (0: on no layer, which keeps it on). */
void ui_layer_eyedropper_pick(Context &C, uint64_t layer);
/* The shape eyedropper: the same, for a shape clicked in the viewport (the brush eyedropper): `pick`
 * gets its layer and its index in the drawing as shown, and returns true when it's done. Clicks in
 * the Layers editor don't count. */
void ui_shape_eyedropper_start(std::function<bool(Context &, uint64_t, int)> pick, std::function<std::string()> cue,
                               const char *tag = "");
bool ui_shape_eyedropper_active();
/* Whether it's on for `tag` (which button started it: "BRUSH", "FILL"...). */
bool ui_shape_eyedropper_active(const char *tag);
/* Takes a look setting of a shape clicked in the viewport (mesh.style_copy_from: FILL, LINE, WIDTH or
 * BRUSH) onto the selected shapes; `what_label` names it in the cue ("fill colour"). */
void ui_style_eyedropper_start(const char *what, const char *what_label);
/* An eyedropper button (a square, the pipette on it), lit while `active`. True when clicked. */
bool ui_pick_button(const char *id, const char *tooltip, bool active);
/* A click on shape `shape` of `layer` while it is on (layer 0: on no shape, which keeps it on). */
void ui_shape_eyedropper_pick(Context &C, uint64_t layer, int shape);
/* The bone eyedropper: the same, for a bone of bone layer `armature` clicked in the viewport (Parent
 * Bone's): `pick` gets it and returns true when it's done. */
void ui_bone_eyedropper_start(uint64_t armature, std::function<bool(Context &, uint64_t)> pick,
                              std::function<std::string()> cue);
/* The bone layer it picks from while it's on (0: it isn't on). */
uint64_t ui_bone_eyedropper_armature();
/* A click on `bone` while it is on (0: on no bone, which keeps it on). */
void ui_bone_eyedropper_pick(Context &C, uint64_t bone);
/* The bone under the mouse this frame, named at the mouse (the viewport says, each frame). */
void ui_bone_eyedropper_hover(const std::string &bone_name);
/* Its cue at the mouse, and stopping it; once a frame, after the editors drew. */
void ui_layer_eyedropper_overlay();
/* A pipette icon, as on Blender's eyedropper buttons. */
void ui_eyedropper_icon(ImDrawList *dl, ImVec2 center, float size, ImU32 color);

/* M and Shift M: Blender's Move to Collection and Assign to Collection menus, opened by the editor
 * under the mouse (the viewport or the Layers editor) when one of them asked. */
void ui_bone_collection_menus(Context &C, bool hovered);

/* Menus that open at the mouse (right-click menus, X, Ctrl P...), as Blender's: each remembers the
 * item last chosen and opens with it under the mouse (the first item, before any choice). In place
 * of ImGui::BeginPopup / MenuItem / BeginMenu / EndMenu / EndPopup. A menu is remembered by its id,
 * so the same menu opened from different rows shares one memory. */
bool ui_menu_begin(const char *id);
bool ui_menu_item(const char *label, const char *shortcut = nullptr, bool selected = false, bool enabled = true);
bool ui_submenu_begin(const char *label, bool enabled = true);
void ui_submenu_end();
void ui_menu_end();

/* Drivers (src/editors/driver_ui.cc). A keyable field or colour is wrapped in ui_driver_field_begin /
 * _end: purple while driven, red while its driver doesn't work, and right-clicking it offers Add,
 * Edit, Copy, Paste and Delete Driver. `component` -1 is the whole field (every part of a colour or a
 * position), 0 / 1 one of a pair's X / Y. Begin returns what End pops. */
int ui_driver_field_begin(Context &C, uint64_t layer, const std::string &path, int component);
void ui_driver_field_end(Context &C, uint64_t layer, const std::string &path, int component, int pushed);
/* After a number field (before ui_driver_field_end): "#expression" typed into it drives it. */
void ui_driver_typed(Context &C, uint64_t layer, const std::string &path, int component);
/* The colour ui_color draws next ("shapes[<id>].fill"), so it can be driven. */
void ui_driver_next(uint64_t layer, const std::string &path);
bool ui_driver_next_take(uint64_t *r_layer, std::string *r_path);
/* True when a field's driver menu opened this frame (a row's own right-click menu then stays shut). */
bool ui_driver_menu_opened_this_frame();
/* The Driver window, drawn once a frame with the other floating windows. */
void ui_driver_window_draw(Context &C);

}  // namespace oanim
