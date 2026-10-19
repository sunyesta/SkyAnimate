#pragma once

/* Context: what an operator or panel is working on right now, like Blender's bContext. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/math.hh"
#include "data/vector.hh"

namespace sky {

struct Document;
class UndoStack;

/* Arguments to an operator call by name, like the keyword arguments of a bpy.ops call:
 * sky.ops.wm.open_mainfile(filepath="walk.sky"). */
using OpArgs = std::map<std::string, std::string>;

/* Viewport tools, as on the toolbar. */
enum class Tool { Select, Cursor, Draw, Shape, SplitCurve, WeightPaint, TransformBones };

/* The Draw tool draws freehand strokes, or with the pen: click to add points along a curve. */
enum class DrawMode { Freehand, Pen };

/* Blender Grease Pencil's selection modes (1, 2, 3). */
enum class SelectMode { Point, Edge, Stroke };

/* Settings shared by the viewport tools, like Blender's ToolSettings. */
struct ToolSettings {
  Tool tool = Tool::Select;
  /* K switches to Split Curve for a single split; afterwards (or on Esc or right-click) the
   * tool it came from returns. */
  bool tool_temporary = false;
  Tool tool_return = Tool::Select;
  SelectMode select_mode = SelectMode::Point;
  /* Snap points to the grid while drawing and transforming. */
  bool snap_grid = false;
  /* Snap to existing points while drawing. */
  bool snap_points = true;
  /* Grid spacing for snapping and for the grid overlay. */
  float grid_size = 0.1f;
  /* Style for new shapes and strokes. */
  Style style;
  /* What the Draw and Shape tools draw, picked from their flyouts on the tool bar. */
  DrawMode draw_mode = DrawMode::Freehand;
  PrimitiveKind shape = PrimitiveKind::Box;
  /* Fill closed freehand strokes and closed shapes, Moho's "auto fill". */
  bool draw_fill = false;
  /* Tablet pressure sets line width while drawing. */
  bool use_pressure = true;
  /* The 2D cursor in document space. */
  float2 cursor;
  /* Weight Paint brush, as Blender's: radius in screen pixels, the weight it paints, how hard,
   * and how (0 Draw, 1 Add, 2 Subtract); Ctrl while painting subtracts. */
  /* Blender's mesh symmetry: moving a point moves the one mirrored across the layer's vertical
   * (X) and/or horizontal (Y) axis too. */
  bool mirror_x = false;
  bool mirror_y = false;
  float weight_radius = 50.0f;
  float weight_value = 1.0f;
  float weight_strength = 0.5f;
  int weight_mode = 0;
  bool weight_normalize = true;
};

/* A key press during a modal transform, by Blender event name ("E", "RET", "ESC"). */
struct ModalKey {
  std::string type;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
};

/* Window-manager state that operators may change. */
struct WindowManager {
  bool background = false;
  bool quit_requested = false;
  /* Interface scale, 1.0 = 100%. */
  float ui_scale = 1.0f;
  bool ui_scale_changed = true;
  /* Set by render.render; the window renders the frame (it owns the OpenGL context). */
  bool render_requested = false;
  /* G or S over the timeline: it starts moving or scaling the selected keys. */
  char timeline_transform_request = 0;

  /* Animation playback, advanced by the main loop in real time. */
  bool playing = false;
  bool play_reverse = false;
  bool play_loop = true;
  /* Frame to return to when playback is cancelled with Esc. */
  int play_start_frame = 1;
  uint64_t play_last_ns = 0;
  double play_frames_due = 0.0;

  /* Active workspace tab, and how many there are. */
  int workspace_index = 0;
  int workspace_count = 1;

  ToolSettings tool_settings;
  /* Set by transform.* operators; the viewport under the mouse runs the modal transform:
   * 'G' move, 'R' rotate, 'S' scale, 'E' extrude then move. */
  char transform_request = 0;
  /* A menu a key asked for, opened at the mouse by the editor under it ("bone_parent_set"). */
  std::string menu_request;
  /* Mouse position in document space while over a viewport, for operators that act at the
   * mouse (L select linked). */
  float2 mouse_document;
  bool mouse_in_viewport = false;
  /* A viewport is running a modal transform; key presses go to it instead of the keymaps. */
  bool modal_running = false;
  /* Keys pressed while it runs, in the order they arrived. The transform reads these rather
   * than ImGui's key state: ImGui spreads a click and a key that land in the same frame over
   * two frames, so a quick click then E would end the transform before ImGui showed it the E,
   * while the keymap had already passed the E over because the transform was running. */
  std::vector<ModalKey> modal_keys;
  /* Latest pen pressure, 0-1 (1 for a mouse). */
  float pen_pressure = 1.0f;

  /* The .sky file the document was opened from or saved to; empty for a new document. */
  std::string filepath;
  /* UndoStack::change_count() when the document was last saved or opened, and autosaved. */
  uint64_t saved_change = 0;
  uint64_t autosaved_change = 0;
  /* An operator that would lose unsaved changes (New, Open, Quit) waits here while the
   * window asks whether to save first; it then runs again with confirmed=1. */
  std::string confirm_operator;
  OpArgs confirm_args;
};

struct Context {
  Document *document = nullptr;
  UndoStack *undo = nullptr;
  WindowManager *wm = nullptr;
  /* Keymap name of the editor under the mouse, e.g. "Viewport". */
  std::string area_type = "Window";
  /* Text shown in the text editors; text.* operators act on it. */
  std::string text_name;
  /* Arguments of the operator running now. */
  const OpArgs *op_args = nullptr;
};

/* An argument of the running operator, or `fallback` when it wasn't given. */
std::string op_arg(const Context &C, const std::string &name, const std::string &fallback = "");
/* True for "1", "True" and "true". */
bool op_arg_bool(const Context &C, const std::string &name);
/* Number arguments (given as text, "0.5"), or `fallback` when missing or not a number. */
int op_arg_int(const Context &C, const std::string &name, int fallback);
float op_arg_float(const Context &C, const std::string &name, float fallback);

}  // namespace sky
