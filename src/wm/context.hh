#pragma once

/* Context: what an operator or panel is working on right now, like Blender's bContext. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/math.hh"
#include "data/vector.hh"

namespace oanim {

struct Document;
class UndoStack;

/* Arguments to an operator call by name, like the keyword arguments of a bpy.ops call:
 * oanim.ops.wm.open_mainfile(filepath="walk.oanim"). */
using OpArgs = std::map<std::string, std::string>;

/* Viewport tools, as on the toolbar. */
enum class Tool { Select, SelectLayer, Cursor, Draw, Shape, SplitCurve, WeightSelect, WeightPaint, TransformBones, TransformPoints };
/* Tools that only work on the setup frame (0): drawing and rigging. */
bool tool_setup_only(Tool tool);

/* The Draw tool draws freehand strokes, or with the pen: click to add points along a curve. */
enum class DrawMode { Freehand, Pen };

/* Blender Grease Pencil's selection modes (1, 2, 3). */
enum class SelectMode { Point, Edge, Stroke };

/* Settings shared by the viewport tools, like Blender's ToolSettings. */
struct ToolSettings {
  Tool tool = Tool::Select;
  /* The Select Layer tool's Pick: the layer clicked, or its outermost group (Photoshop's Auto-Select
   * Layer / Group). Alt-click picks the same way from any tool. */
  bool layer_pick_group = false;
  /* K switches to Split Curve for a single split; afterwards (or on Esc or right-click) the
   * tool it came from returns. */
  bool tool_temporary = false;
  /* A setup-only tool (Draw, Weight Paint...) gives way to Select away from frame 0; this is the
   * tool to go back to on frame 0. */
  Tool tool_before_posing = Tool::Select;
  bool tool_switched_for_posing = false;
  /* Posing a bone by its tip turns its parents too (Moho's IK). */
  bool auto_ik = true;
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
  /* Blender's proportional editing (O), on points only: G R S drag the points around the
   * selection too, by the falloff, out to the size (document units; the wheel or Page Up and
   * Page Down change it while transforming). Connected Only: only along the curves. */
  bool proportional = false;
  ProportionalFalloff proportional_falloff = ProportionalFalloff::Smooth;
  bool proportional_connected = false;
  float proportional_size = 0.2f;
  float weight_radius = 50.0f;
  float weight_value = 1.0f;
  float weight_strength = 0.5f;
  int weight_mode = 0;
  bool weight_normalize = true;
  /* Selection Mask: paint only the selected points. */
  bool weight_mask = false;
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
  /* The frame Tab left for the setup frame (0), to go back to with Tab again. */
  int frame_before_setup = 0;
  uint64_t play_last_ns = 0;
  double play_frames_due = 0.0;

  /* Where the timelines' frames are on screen (set as they draw), so a file dropped on one starts on
   * the frame it lands on. */
  struct TimelineDropArea {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float first_frame = 0, frames_per_unit = 1;
    /* A Media layer's clips row: a file dropped on it becomes a clip of that layer (0: none). */
    uint64_t media_layer = 0;
  };
  std::vector<TimelineDropArea> timeline_drop_areas;

  /* Mouse events from oanim.wm.mouse_simulate (checking the UI from scripts), in the window's UI
   * units: one is given to ImGui each frame, so a press and its release land on different frames.
   * `button` -1 only moves the mouse. */
  struct SimulatedMouse {
    float x = 0.0f, y = 0.0f;
    int button = -1;
    bool down = false;
    /* Ctrl held with it (a magnet drag in the timeline, say). */
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
  };
  std::vector<SimulatedMouse> mouse_simulated;

  /* Active workspace tab, and how many there are. */
  int workspace_index = 0;
  int workspace_count = 1;

  ToolSettings tool_settings;
  /* Set by transform.* operators; the viewport under the mouse runs the modal transform:
   * 'G' move, 'R' rotate, 'S' scale, 'E' extrude then move. */
  char transform_request = 0;
  /* Set by view.overlays_toggle (Shift Z): the viewport under the mouse shows or hides its
   * overlays. */
  bool overlays_toggle_request = false;
  /* Local View (Blender's, / or Numpad /): the layers the viewports show alone, or empty for all.
   * The view stays where it is. Not saved and not undone, as a view. */
  std::vector<uint64_t> local_view;
  /* Set by screen.redo_last (F9): the ViewportActionPanel opens, or folds if open. */
  bool action_panel_toggle_request = false;
  /* A Properties tab to show (oanim.wm.properties_tab), taken by the next Properties editor drawn
   * (-1: none): 0 Document, 1 Layer, 2 Drawing or Armature, 3 Modifiers, 4 Bone Constraints, 5 Reference. */
  int properties_tab_request = -1;
  /* A property to show in the Properties editor (picked from a key in the timeline): the panel
   * holding it opens, and its row is scrolled to and flashes blue for a second. `layer` 0: none. */
  struct PropertiesFlash {
    uint64_t layer = 0;
    std::string path;
    /* The panel to open ("" for none). */
    std::string panel;
    /* Since it was found (ImGui time; below 0 until a Properties editor drew it). */
    double start = -1.0;
    int frames = 0;
    bool seen_properties = false;
  } properties_flash;
  /* The item of the timeline's Keyed Here list under the mouse: the viewport outlines its bone or
   * points (`layer` 0: none). Set by the timeline each frame. */
  struct KeyedHover {
    uint64_t layer = 0;
    std::vector<std::string> paths;
    /* Layers to outline whole: a Draw Order bar's layer and the one it's drawn in front of or behind. */
    std::vector<uint64_t> boxes;
  } keyed_hover;
  /* A menu a key asked for, opened at the mouse by the editor under it ("bone_parent_set"). */
  std::string menu_request;
  /* In Front Of... / Behind... (layer.draw_order_pick): waiting for a click on the layer to go in
   * front of (mode 1) or behind (-1); `layers` move. `hover`: the layer under the mouse, shown
   * as the result while pointing. */
  struct DrawOrderPick {
    int mode = 0;
    std::vector<uint64_t> layers;
    uint64_t hover = 0;
    /* The eyedropper is on for it. */
    bool started = false;
  } draw_order_pick;
  /* Inside... / Outside... (mask.pick): waiting for a click on the layer to mask `layers` with;
   * `index` >= 0 adds it to that mask of theirs instead of a new one. */
  struct MaskPick {
    bool active = false;
    /* The new row's mode (a MaskMode identifier), or empty: Add for a first row, Intersect after. */
    std::string mode;
    int index = -1;
    std::vector<uint64_t> layers;
    bool started = false;
  } mask_pick;
  /* A mask chip under the mouse in the Layers editor: its layers are outlined in the viewport. */
  std::vector<uint64_t> mask_hover;
  /* Layers a mask's field in Properties names, under the mouse: highlighted in the Layers editor.
   * Set with the ImGui frame it was set on, as editors draw in any order (shown for that frame and
   * the next). */
  std::vector<uint64_t> layers_hover;
  int layers_hover_frame = -10;
  /* A material under the mouse in the Shape panel's list: the viewport outlines its shapes (set with
   * the ImGui frame it was set on). */
  uint64_t material_hover = 0;
  int material_hover_frame = -10;
  /* Show Active (. over the Layers editor): the layer whose row the Layers editors scroll to (its
   * row flashes for a moment), and a count of the requests, so each is taken once. */
  uint64_t layers_show_active = 0;
  uint64_t layers_show_active_serial = 0;
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

  /* The .oanim file the document was opened from or saved to; empty for a new document. */
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
  /* Keymap name of the editor being drawn right now ("" between editors): where an operator picked
   * from its menus runs, whatever is under the mouse (the menu). */
  std::string drawing_area;
  /* Text shown in the text editors; text.* operators act on it. */
  std::string text_name;
  /* Arguments of the operator running now. */
  const OpArgs *op_args = nullptr;
  /* The undo step's name, set by an operator that says what it did ("Hand In Front Of Torso");
   * empty: the operator's name. */
  std::string undo_name;
};

/* An argument of the running operator, or `fallback` when it wasn't given. */
std::string op_arg(const Context &C, const std::string &name, const std::string &fallback = "");
/* Whether the running operator was given `name`. */
bool op_has_arg(const Context &C, const std::string &name);
/* True for "1", "True" and "true". */
bool op_arg_bool(const Context &C, const std::string &name);
/* Number arguments (given as text, "0.5"), or `fallback` when missing or not a number. */
int op_arg_int(const Context &C, const std::string &name, int fallback);
float op_arg_float(const Context &C, const std::string &name, float fallback);

}  // namespace oanim
