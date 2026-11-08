#pragma once

/* Context: what an operator or panel is working on right now, like Blender's bContext. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "anim/path_brush.hh"
#include "anim/sculpt.hh"
#include "core/math.hh"
#include "data/annotations.hh"
#include "data/document.hh"
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
  EditText,
  /* Motion paths (anim/motion_path.hh): click a dot to go to its frame, drag one to key it there. */
  MotionPath
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

/* Blender Grease Pencil's selection modes (1, 2, 3). */
enum class SelectMode { Point, Edge, Stroke };

/* The kinds of pen that take away what they brush. */
inline bool draw_mode_cuts(DrawMode mode)
{
  return mode == DrawMode::FillEraser || mode == DrawMode::StrokeEraser || mode == DrawMode::Trimmer;
}
/* The kinds of pen that brush a width of their own, the others drawing lines. */
inline bool draw_mode_paints(DrawMode mode)
{
  return mode == DrawMode::FillPen || draw_mode_cuts(mode);
}

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
  /* While drawing with a pen, the line runs this far (ms) ahead of the pen's last sample, the way it is
   * going, so it keeps up with the hand (0: off). */
  float draw_prediction = 8.0f;
  /* How freehand strokes are made, set by picking a pen on the shelf (assets/draw_presets.hh; `draw_preset` names
   * it), then changed in the header. */
  InkSettings ink;
  std::string draw_preset = "Ink";
  /* What each pen on the shelf was changed to (by its name), kept while Open Animate runs, as sculpt_changes. */
  struct DrawPresetChanges {
    InkSettings ink;
    float line_width = 0.008f;
  };
  std::map<std::string, DrawPresetChanges> draw_changes;
  /* The preset each kind of pen last used (by DrawMode), so coming back to it brings it back. */
  std::map<int, std::string> draw_kind_preset;
  /* How wide the Fill Pen and the erasers are, in document units, as Style::line_width is the Stroke Pen's: not
   * the line width that shapes take. */
  float fill_size = 0.03f;
  /* The erasers and Trimmer cut every drawing being edited, not only the active one; and only selected shapes. */
  bool draw_erase_all_layers = false;
  bool draw_erase_selected = false;
  /* A stroke's ends join a point they end on. */
  bool draw_weld = false;
  /* A stroke's ends past the last line they crossed go. */
  bool draw_trim = false;
  /* The line's width as a circle round the mouse. */
  bool draw_cursor_circle = true;
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
  /* Dyntopo (at the tool settings' right end): points added as a stroke needs them, on drawings without animation,
   * and with Decimate the ones the line doesn't need taken out after it, to within the Resolution: points per unit of
   * each layer's own space (SculptDab::resolution). */
  bool sculpt_dyntopo = false;
  bool sculpt_decimate = false;
  float sculpt_resolution = 50.0f;
  /* The Select tool's Decimate Curve (mesh.decimate): the Ratio of the selected points to keep it was last run with
   * (half, the first time, so a first click does something). */
  float decimate_ratio = 0.5f;
  /* The Motion Path tool's brush (its shelf), and each brush's own radius in screen pixels and strength (Shift's
   * brush, path_brush_shifted, uses its own). */
  PathBrush path_brush = PathBrush::Edit;
  PathBrushSettings path_brushes[PATH_BRUSH_COUNT];
  PathBrushSettings &path_brush_settings(PathBrush brush) { return path_brushes[int(brush)]; }
  const PathBrushSettings &path_brush_settings(PathBrush brush) const { return path_brushes[int(brush)]; }
  /* Edit's proportional editing of dots (on/off and falloff are the points', O and Shift O): how many frames either
   * side of the dragged dot slide with it. */
  float path_proportional_size = 3.0f;
  /* The Motion Path tool's Edit Layer: the layer itself edited (its origin's path, the Transform Layer box; no bone or
   * point picked, the timeline's teal on its transform rows); off, its bones and points (the teal never on its
   * position). */
  bool path_edit_layer = false;
  /* The timing chart's and the Path Timing graph's own reach (frames either side of a dragged dot that follow it,
   * proportional editing being always on there; 0 the dot alone), sized with the wheel while dragging. */
  float path_chart_size = 3.0f;
  float path_graph_size = 3.0f;
  /* Shift click's timing blend there (anim.motion_path_timing_blend), kept for the next click. */
  float path_blend_smoothing = 0.5f;
  int path_blend_repeat = 1;
  float path_blend_radius = 3.0f;
};

/* A key press during a modal transform, by Blender event name ("E", "RET", "ESC"). */
struct ModalKey {
  std::string type;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
};

/* Window-manager state that operators may change. */
/* How wide a kind of pen draws. */
inline float &draw_width(ToolSettings &ts, DrawMode mode)
{
  return draw_mode_paints(mode) ? ts.fill_size : ts.style.line_width;
}

struct WindowManager {
  bool background = false;
  /* The modifier whose Pieces or Spacing is being dragged (the layer it is on, its place there) and the UI frame it last
   * was: Show Points' dots show for it meanwhile. */
  uint64_t scrub_layer = 0;
  int scrub_modifier = -1;
  int scrub_frame = -1;
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
  /* Play Path Range (Alt Space): playback loops over these frames instead of the animation's (0: not on). */
  int play_range_first = 0;
  int play_range_last = 0;
  /* Motion paths' Snapshot: their lines as they were, drawn faintly to compare an edit against (not saved). */
  std::vector<std::vector<float2>> path_snapshot;
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
   * `frame` is the UI frame it was last held on (it stops a frame after it's let go). `step`, a masking
   * area's own (MaskStep::order): that card as far as that area. */
  struct MaskPreview {
    uint64_t layer = 0;
    bool lines = false;
    int upto = -1;
    int frame = -10;
    int step = -1;
  } mask_preview;
  /* Envelope's Range slider, while dragged: the viewport tints what `modifier` of `layer` reaches; its Blend slider (or a
   * Points mesh's): where the sides share; Points' Falloff: where the mesh's lines share. `frame` as above. */
  struct DeformRangePreview {
    enum class What { Range, Blend, Falloff };
    uint64_t layer = 0;
    int modifier = -1;
    int frame = -10;
    What what = What::Range;
  } deform_range_preview;
  /* The masking area under the mouse in the Masking tab, for mask.area_delete_hovered (`layer` 0: none). */
  struct MaskAreaHover {
    uint64_t layer = 0;
    int card = 0, step = 0, child = -1;
  } mask_area_hover;
  /* The guide line under the mouse with a guide tool (-1: none), and that X or Delete just took a guide. */
  int guide_hover = -1;
  bool guide_deleted = false;
  /* The active masking area: `step` -1 is the base, `child` >= 0 one inside that group. */
  struct MaskAreaPick {
    uint64_t layer = 0;
    int card = 0, step = -2, child = -1;
  } mask_area_pick;
  /* The picked masking areas of that mask, as the Layers editor's picked layers, each (step << 8) | (child + 1). */
  std::vector<int> mask_area_rows;
  /* The Masking tab is showing: its keys work. */
  bool mask_tab_shown = false;
  /* Inside... / Outside... (mask.pick): waiting for a click on the layer to mask `layers` with;
   * `index` >= 0 adds it to that mask of theirs instead of a new one. */
  struct MaskPick {
    bool active = false;
    /* The new row's mode (a MaskMode identifier), or empty: Add for a first row, Subtract of the
     * outside after. `verb`, or what it does to the card's shape (a MaskVerb identifier). */
    std::string mode;
    std::string verb;
    /* A new step goes in this group of the card, not on top. */
    int group = -1;
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
  /* F, Shift F or R with the Sculpt tool: the viewport sets the brush's radius ('R'), its strength ('S') or Dyntopo's
   * Resolution ('D') with the mouse until a click. */
  char sculpt_adjust_request = 0;
  /* The same for the Motion Path tool's brushes ('R' radius, 'S' strength). */
  /* The Motion Path tool's Edit: the dots selected between keys (each its target and frame; keys selected are the
   * timeline's), and what its shortcuts ask the viewport to start: Alt S scaling the timing around the selection,
   * G R S on the selected keys ('G', 'R', 'S'). */
  std::vector<std::pair<MotionPathTarget, int>> path_dots;
  /* The path handles and control points picked: handles never with keys, dots or other path things (a pick of one
   * kind drops the others), control points with keys too (L, Shift click); G R S, V and X act on them. The bones and
   * points traced stay picked, as they make the paths show. */
  std::vector<MotionPathHandlePick> path_handle_picks;
  /* The path under the mouse in the hovered viewport, as last drawn (the Motion Path tool's Edit), for L: what it
   * traces and the dot or key's frame, or a control point's stretch's end key (`path_under_end`, else 0). */
  bool path_under = false;
  MotionPathTarget path_under_target;
  int path_under_frame = 0;
  int path_under_end = 0;
  /* The dot under the mouse in the viewport or the timeline's timing chart, so the other rings it too: set each frame
   * it is hovered (`path_hover_tick` the ImGui frame it was), stale a frame later. */
  MotionPathTarget path_hover_target;
  int path_hover_frame = 0;
  int path_hover_tick = -10;
  /* The Path Timing panel's stretch: of the last thing a dot or key was picked on, the stretch ending on
   * `path_timing_end` (0: the one the frame shown is in); see motion_path_timing_stretch. */
  MotionPathTarget path_timing_target;
  int path_timing_end = 0;
  /* The frame shown when it was picked or stepped to: once the frame changes the panel follows it again. */
  int path_timing_frame = 0;
  /* The timeline's sidebar (N), with its Path Timing tab. */
  bool timeline_sidebar = false;
  bool path_scale_request = false;
  char path_keys_request = 0;
  char path_adjust_request = 0;
  /* The same for the Draw tool: the line's width ('R') or opacity ('S'). */
  char draw_adjust_request = 0;
  /* Shift Space or the header's pen button: the viewport shows the pens at the mouse. */
  bool draw_popup_request = false;
  /* The last stroke, for the Fidelity readout. */
  InkStroke draw_last;
  bool draw_last_closed = false;

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
