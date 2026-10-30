#pragma once

/* Context: what an operator or panel is working on right now, like Blender's bContext. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "anim/sculpt.hh"
#include "core/math.hh"
#include "data/annotations.hh"
#include "data/ink_stroke.hh"
#include "data/vector.hh"
#include "wm/pen_input.hh"

namespace oanim {

struct Document;
class UndoStack;

/* Arguments to an operator call by name, like the keyword arguments of a bpy.ops call:
 * oanim.ops.wm.open_mainfile(filepath="walk.oanim"). */
using OpArgs = std::map<std::string, std::string>;

/* Viewport tools, as on the toolbar. */
enum class Tool {
  Select,
  SelectLayer,
  Cursor,
  Annotate,
  Draw,
  Shape,
  SplitCurve,
  WeightSelect,
  WeightPaint,
  TransformBones,
  TransformPoints,
  Sculpt,
  /* Any layer: the box gizmo round it moves, turns and scales the layer itself (Moho's Transform Layer). */
  TransformLayer,
  /* A text layer's: typing in it, and where its lines wrap. */
  EditText
};
/* Tools that only work on the setup frame (0): drawing and rigging. */
bool tool_setup_only(Tool tool);

/* The Annotate tool's variants, picked from its flyout: notes (freehand, a straight line, the eraser) and
 * the drawing guides it makes and edits (data/annotations.hh). */
enum class AnnotateMode { Draw, Line, Eraser, GuideLine, GuidePerspective, GuideGrid };
inline bool annotate_mode_is_guide(AnnotateMode mode)
{
  return mode == AnnotateMode::GuideLine || mode == AnnotateMode::GuidePerspective || mode == AnnotateMode::GuideGrid;
}

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
  /* The tool last used with each kind of active layer (drawings, bone layers, groups, Media, text,
   * none), as
   * Blender remembers each mode's: making that kind of layer active again picks it again. */
  Tool tool_for_layer_kind[6] = {Tool::Select, Tool::Select, Tool::Select, Tool::Select, Tool::Select, Tool::Select};
  int tool_layer_kind = -1;
  /* Ctrl Tab: the tool to go back to from Sculpt (as Blender toggles sculpt mode). */
  Tool tool_before_sculpt = Tool::Select;
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
  /* The Annotate tool: which variant, Annotate Line's end styles, the eraser's radius (screen pixels),
   * and new perspective guides' vanishing points and lines. */
  AnnotateMode annotate_mode = AnnotateMode::Draw;
  AnnotationEnd annotate_start = AnnotationEnd::None;
  AnnotationEnd annotate_end = AnnotationEnd::Arrow;
  float annotate_eraser_radius = 20.0f;
  int guide_points = 2;
  int guide_lines = 24;
  /* Fill closed freehand strokes and closed shapes, Moho's "auto fill". */
  bool draw_fill = false;
  /* The Shape tool's Grid: how many cells across and up. */
  int grid_columns = 3;
  int grid_rows = 3;
  /* Transform Layer shows the active layer's own grid across the viewport (Show Grid Lines). */
  bool layer_grid = true;
  /* The Transform Points and Transform Bones box: 0 Transform (move, turn, scale, shear), 1 Distort (each
   * corner on its own), 2 Warp (a grid of `warp_cells` each way bending what's inside). */
  int gizmo_mode = 0;
  int warp_cells = 3;
  /* Tablet pressure sets line width while drawing. */
  bool use_pressure = true;
  /* While drawing with a pen, the line runs this far (ms) ahead of the pen's last sample, the way it is
   * going, so it keeps up with the hand (0: off). */
  float draw_prediction = 8.0f;
  /* How freehand strokes are made: smoothing, the pressure curve, tapers, fidelity. */
  InkSettings ink;
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
  /* What Weight Paint paints: the bones (empty), or the active layer's deform modifier of this name. */
  std::string weight_target;
  float weight_value = 1.0f;
  float weight_strength = 0.5f;
  /* Pen pressure scales Weight Paint's radius, strength and weight (the toggles joined to them). */
  bool weight_pressure_radius = false;
  bool weight_pressure_strength = true;
  bool weight_pressure_weight = false;
  int weight_mode = 0;
  bool weight_normalize = true;
  /* Selection Mask: paint only the selected points. */
  bool weight_mask = false;
  /* Sculpt tool (anim/sculpt.hh), as Blender's Grease Pencil sculpt brushes: the brush and its
   * settings, set by picking a preset on the shelf (assets/sculpt_presets.hh; `sculpt_preset` names
   * it), then changed in the header. Radius in screen pixels. Pen pressure can scale the radius and
   * the strength; Selected Only is Blender's selection mask. */
  SculptBrush sculpt_brush = SculptBrush::Grab;
  std::string sculpt_preset = "Grab";
  /* What each brush on the shelf was changed to (by its name), kept while Open Animate runs, as a Blender brush
   * keeps its settings: going back to it brings them back rather than its saved ones. */
  struct SculptBrushChanges {
    float radius = 60.0f;
    float strength = 1.0f;
    SculptFalloff falloff = SculptFalloff::Smooth;
  };
  std::map<std::string, SculptBrushChanges> sculpt_changes;
  float sculpt_radius = 60.0f;
  float sculpt_strength = 1.0f;
  SculptFalloff sculpt_falloff = SculptFalloff::Smooth;
  bool sculpt_pressure_radius = false;
  bool sculpt_pressure_strength = true;
  bool sculpt_selected_only = false;
  /* Surface Smooth's Lump Sensitivity, 0-1 (SculptDab::surface_sensitivity). */
  float sculpt_surface_sensitivity = 0.25f;
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
  /* A render of the current frame to a PNG (oanim.wm.render_png): through the camera, `width` x
   * `height` (0: the document's), with guide layers too, and a clear background if asked. Done by the
   * window on its next frame. */
  struct RenderFile {
    bool pending = false;
    std::string path;
    int width = 0, height = 0;
    bool guides = false, transparent = false;
  } render_file;
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
    /* Wheel notches turned there (up positive). */
    float wheel = 0.0f;
    /* A pen's samples arriving with it (oanim.wm.mouse_simulate(pen=...)), in the window as ImGui counts:
     * a left press starts the pen's touch (its eraser end when `pen_eraser`), a release ends it. */
    bool has_pen = false;
    bool pen_eraser = false;
    std::vector<PenSample> pen;
  };
  std::vector<SimulatedMouse> mouse_simulated;
  /* The mouse is where the last simulated event put it, until the real one moves: it counts as over
   * the window then, though the real one may be outside it. */
  bool mouse_simulating = false;
  /* Where the last simulated event put the mouse, and whether it left a button down: a simulated drag
   * keeps the mouse there each frame, whatever the real one does, and never warps the real cursor. */
  float mouse_simulated_x = 0.0f, mouse_simulated_y = 0.0f;
  bool mouse_simulated_held = false;

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
  /* Media View (`): the viewports show every media layer at full opacity, guides too, with the
   * overlays hidden, to see the footage clearly. A view: no layer's opacity changes, nothing is
   * keyed, saved or undone. */
  bool media_view = false;
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
  /* A mask card's View button, while held: the viewport shows `layer`'s mask after its row `upto`
   * in black and white instead of the drawing (the lines' mask with `lines`, else the fills').
   * `frame` is the UI frame it was last held on (it stops a frame after it's let go). */
  struct MaskPreview {
    uint64_t layer = 0;
    bool lines = false;
    int upto = -1;
    int frame = -10;
  } mask_preview;
  /* Inside... / Outside... (mask.pick): waiting for a click on the layer to mask `layers` with;
   * `index` >= 0 adds it to that mask of theirs instead of a new one. */
  struct MaskPick {
    bool active = false;
    /* The new row's mode (a MaskMode identifier), or empty: Add for a first row, Subtract of the
     * outside after. */
    std::string mode;
    /* `index` >= 0: a new step of that card; with `step` >= 0 too, that step's layers. */
    int index = -1;
    int step = -1;
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
  /* The asset shelf: the tile whose name opens for typing (a duplicate just made), else 0. */
  uint64_t shelf_rename = 0;
  uint64_t layers_show_active = 0;
  uint64_t layers_show_active_serial = 0;
  /* Mouse position in document space while over a viewport, for operators that act at the
   * mouse (L select linked). */
  float2 mouse_document;
  bool mouse_in_viewport = false;
  /* A viewport is running a modal transform; key presses go to it instead of the keymaps. */
  bool modal_running = false;
  /* Text is being typed on the canvas (editors/text_tool.hh): the keys are its, not shortcuts. */
  bool text_editing = false;
  /* Keys pressed while it runs, in the order they arrived. The transform reads these rather
   * than ImGui's key state: ImGui spreads a click and a key that land in the same frame over
   * two frames, so a quick click then E would end the transform before ImGui showed it the E,
   * while the keymap had already passed the E over because the transform was running. */
  std::vector<ModalKey> modal_keys;
  /* Latest pen pressure, 0-1 (1 for a mouse). */
  float pen_pressure = 1.0f;
  /* The pen is near the tablet, and its eraser end is the one down (or its eraser button held). */
  bool pen_near = false;
  bool pen_eraser = false;
  /* F or Shift F with the Sculpt tool: the viewport sizes the brush's radius ('R') or strength ('S')
   * with the mouse until a click. */
  char sculpt_adjust_request = 0;

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
  /* Values the running operator chose for options it wasn't given (see op_arg_used). */
  OpArgs *op_args_used = nullptr;
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
