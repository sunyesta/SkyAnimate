#pragma once

/* The document: everything that is saved in a .oanim file.
 *
 * Bones arrive in milestone 4. Keep this a plain copyable value: undo snapshots copy it. */

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "data/annotations.hh"
#include "data/driver.hh"
#include "data/layer.hh"
#include "data/material.hh"
#include "data/view_settings.hh"
#include "rna/rna.hh"

namespace oanim {

/* A text block, like Blender's Text: scripts and notes saved with the document. */
struct Text {
  std::string name;
  std::string body;
  /* File the text was opened from or saved to; empty for a text that only lives in the document. */
  std::string filepath;
  bool operator==(const Text &) const = default;
};

/* How playback keeps time (Blender's Sync): every frame drawn even if that plays slower than the
 * frame rate, frames skipped to keep the true speed, or kept to the sound (which drops frames the
 * same way until the document has sound). Ordered as the RNA items. */
enum class PlaybackSync { EveryFrame, FrameDropping, Audio };
/* Blender's auto-keying modes: Add & Replace keys whatever changes (adding keys where there are none),
 * Replace only overwrites keys already on the frame. */
enum class AutoKeyMode { AddReplace, Replace };
/* Onion skins step back and forward by keys (of the layers being edited) or by frames. */
enum class OnionMode { Keyframes, Frames };
/* What a motion path is drawn relative to: as it is, as the camera sees it, or as a parent layer (or a bone of it)
 * sees it, so that thing stands still. */
enum class PathRelative { World, Camera, Parent };
/* What colours a motion path's dots: how far through its stretch's time each is (only the stretch being worked on,
 * or every stretch), how fast it goes, or the path's own colour. */
enum class PathDotColors { TimingCurrent, TimingAll, Speed, Path };

/* Something a motion path traces (anim/motion_path.hh): a bone's tip (or head) when `bone` is set, a point when
 * `point` is, else the layer's origin. Kept in Document::path_targets once it is pinned or given its own colour
 * or end, so the path is drawn that way whenever it shows. */
struct MotionPathTarget {
  uint64_t layer = 0;
  uint64_t bone = 0;
  uint64_t point = 0;
  bool head = false;
  /* A Key Whole Drawing layer's drawing as a whole: no path, only its timing (the timeline's timing chart and Path
   * Timing graph), on all its points' channels at once. */
  bool drawing = false;
  bool pinned = false;
  bool has_color = false;
  Color color;
  bool operator==(const MotionPathTarget &) const = default;
  bool same(const MotionPathTarget &o) const
  {
    return layer == o.layer && bone == o.bone && point == o.point && drawing == o.drawing;
  }
};

/* Switches linked by name (anim/switch.hh): one set of keys for the group, on its lead member's Switch
 * channel, holding entries of `names` (their ids; 0 none, -1 Rig) rather than layer ids; each member
 * shows its child answering to the entry's name (its name, or one of its aliases). */
struct SwitchLinkName {
  uint64_t id = 0;
  std::string name;
  bool operator==(const SwitchLinkName &) const = default;
};
struct SwitchLink {
  uint64_t id = 0;
  /* The member whose Switch channel holds the group's keys. */
  uint64_t lead = 0;
  std::vector<SwitchLinkName> names;
  uint64_t next_name = 1;
  bool operator==(const SwitchLink &) const = default;
};

/* What the timeline's rows show (Document::timeline_show) and the kinds of channel it filters by
 * (Document::timeline_kinds). */
enum TimelineShow : int { TIMELINE_SHOW_ANIMATED = 0, TIMELINE_SHOW_CHANGED = 1, TIMELINE_SHOW_ALL = 2 };
enum TimelineKind : uint32_t {
  TIMELINE_KIND_TRANSFORM = 1 << 0, /* a layer's (or the camera's) position, rotation, scale, shear */
  TIMELINE_KIND_BONES = 1 << 1,
  TIMELINE_KIND_POINTS = 1 << 2, /* a drawing's points (Drawing, Points) */
  TIMELINE_KIND_OPACITY = 1 << 3,
  TIMELINE_KIND_SWITCH = 1 << 4,
  TIMELINE_KIND_MEDIA = 1 << 5, /* a Media layer's clips */
  TIMELINE_KIND_OTHER = 1 << 6, /* visibility, draw order, masks, Simulation, the rest */
  TIMELINE_KIND_ALL = 0x7f,
};

struct Document {
  /* The viewports' overlays (kept as they are through undo, as Blender's are). */
  ViewportOverlays overlays;
  ViewportShading shading;
  /* The viewport's zoom and pan as last left (kept through undo too), and a number that changes when
   * a file brings its own, so the viewports take it. */
  ViewportView view;
  uint64_t view_serial = 0;
  /* Mirror editing (the viewport header's X and Y) as saved with the file: the switches themselves are
   * the tool settings', copied in when a file opens and out when it is written. */
  bool mirror_x = false;
  bool mirror_y = false;
  /* Which panels and folding rows are open, by "<editor>/<panel>" ("Properties/Transform",
   * "Timeline/<row>"), where one differs from its default: saved with the project, kept through
   * undo (see ui_fold_state). */
  std::map<std::string, bool> ui_open;
  /* The Properties panels' order where one was dragged, by tab ("Properties/Shape": the panels'
   * names, top first; see ui_boxes_begin). Saved with the project, as the folds are. */
  std::map<std::string, std::vector<std::string>> ui_panel_order;
  std::string name = "Untitled";
  int frame_start = 1;
  int frame_end = 250;
  /* New documents open on the setup frame. */
  int frame_current = 0;
  /* The frame the plain values (layer transforms, point positions) were last evaluated at;
   * -1 before the first evaluation. Not saved. */
  int evaluated_frame = -1;
  /* Edits at frames other than 0 add keys (Blender's record button, on by default), as `auto_key_mode`
   * says (its dropdown). Auto-Freeze Keys (Moho's): a bone keyed by an edit keys every other bone of
   * its bone layer shown there on that frame too, and a point keyed every point of its drawing, so the whole
   * pose and shape hold there. */
  bool auto_key = true;
  AutoKeyMode auto_key_mode = AutoKeyMode::AddReplace;
  bool auto_freeze_keys = false;
  /* Time Together (the timeline's chain toggle): changing the timing of keys changes it the same way on the linked
   * channels, those of the selected layers and bones keyed on the same frames at both ends of the stretch edited
   * (time_together_* in anim/animation.hh). */
  bool time_together = false;
  /* The timeline's Only Selected (its arrow, on for new documents): rows for the picked layers, the animated
   * layers inside them and, of a bone layer with bones picked, only those bones; off: every animated layer.
   * Lock (its padlock) keeps the layers shown when it was turned on, `timeline_locked`, whatever is picked. */
  bool timeline_only_selected = true;
  bool timeline_lock = false;
  std::vector<uint64_t> timeline_locked;
  /* The timeline's filters: which channels get rows (Animated: those with keys; Changed: those whose keys don't
   * all hold one value; All: every channel, keys or not), the kinds shown (TIMELINE_KIND_* bits) and a name to
   * look for (fuzzy, as Blender's: its letters in order). */
  int timeline_show = 0;
  uint32_t timeline_kinds = 0x7f;
  std::string timeline_search;
  /* Onion skins (the timeline's toggle beside the record button, and its popover): the drawings being
   * edited as they are on frames before and after this one, wireframes only in a colour per side,
   * fading with distance. Never rendered. `onion_step` is for Frames: every so many frames. */
  bool onion_skin = false;
  OnionMode onion_mode = OnionMode::Keyframes;
  int onion_before = 2;
  int onion_after = 0;
  int onion_step = 1;
  bool onion_fade = true;
  bool onion_all_layers = false;
  Color onion_color_before = {0.88f, 0.29f, 0.25f, 1.0f};
  Color onion_color_after = {0.25f, 0.72f, 0.36f, 1.0f};
  /* Motion paths (the onion popover's Motion Paths, anim/motion_path.hh): where the selected bones' tips, points or
   * layers go, a dot per frame from `path_before` keys back to `path_after` keys ahead (or the whole range with
   * `path_all`), a dot every `path_step` frames, red where fast and blue where still. Never rendered. */
  bool motion_paths = false;
  int path_before = 1;
  int path_after = 1;
  bool path_all = false;
  int path_step = 1;
  bool path_lines = true;
  PathDotColors path_dot_colors = PathDotColors::TimingCurrent;
  PathRelative path_relative = PathRelative::World;
  uint64_t path_relative_layer = 0;
  uint64_t path_relative_bone = 0;
  std::vector<MotionPathTarget> path_targets;
  /* The timeline's Playback popover (Blender's scene settings). The audio ones are kept for when the
   * document has sound: playing it with the animation, and short bits of it while scrubbing. */
  PlaybackSync sync = PlaybackSync::FrameDropping;
  bool play_audio = true;
  bool audio_scrub = false;
  float fps = 24.0f;
  int width = 1920;
  int height = 1080;
  /* Width and height linked (the button between them): changing one changes the other, keeping
   * `resolution_aspect` (width / height when they were linked). */
  bool resolution_linked = false;
  float resolution_aspect = 16.0f / 9.0f;
  Color background = {0.92f, 0.92f, 0.92f, 1.0f};
  /* Stroke resolution: how many straight pieces draw each curve segment, in the viewport and in
   * renders, like Blender's curve resolution. Lower is faster, higher is smoother. */
  int viewport_quality = 12;
  int render_quality = 32;
  /* Anti-aliasing samples per pixel (1, 2, 4, 8 or 16) in the viewport and in renders. More
   * samples smooth edges better and use more GPU memory. */
  int viewport_samples = 4;
  int render_samples = 16;
  /* The action being edited, like Moho's CurrentDocAction(). Empty means the mainline. Set it
   * with document_set_action: switching swaps the action's keys in. */
  std::string action;
  /* The bone layer the action being edited belongs to. */
  uint64_t action_layer = 0;
  /* Runtime: some plain value has an action bone action's amount added (see Channel::from_action_bones). */
  bool action_bones_applied = false;
  /* Bone dynamics for the whole document (Moho's): gravity and wind push every dynamic bone (by
   * its weight; the wind pushes them all the same); simulating starts at `simulation_start`. */
  float gravity = 0.0f;
  float wind = 0.0f;
  /* Radians: the way the wind blows (0: to the right). */
  float wind_direction = 0.0f;
  /* The wind isn't steady (as Unity's wind zones): gusts make it stronger and weaker and waver its
   * direction (0: steady, 0.5: between half and one and a half times as strong), coming about
   * every two seconds at a gust speed of 1 and sweeping across the drawing; turbulence is a small,
   * fast flutter on top, along the wind and across it. */
  float wind_gusts = 0.0f;
  float wind_gust_speed = 1.0f;
  float wind_turbulence = 0.0f;
  int simulation_start = 1;
  /* Keys for gravity and the wind: they are animatable, kept as channels on a layer that isn't in
   * the tree (SIMULATION_LAYER_ID, "Simulation"), so the timeline and key tools handle them as
   * any layer's; animation copies them into the fields above for each frame shown. */
  Layer simulation;
  /* The camera (data/camera.hh): a layer outside the tree too (CAMERA_LAYER_ID), for its keys. Its
   * `selected` flag is the camera picked (in the Layers editor, or by its frame in the viewport):
   * the viewport then looks through it, and moving and zooming the view move and zoom it. */
  Layer camera;
  /* How dark the canvas outside the camera's frame is in the viewport, 0 to 1 (Blender's
   * passepartout). */
  float camera_passepartout = 0.6f;
  /* Brush presets made in this project, saved with it (assets/brush_presets.hh). */
  std::vector<BrushPreset> brush_presets;
  /* Values worked out from other values every frame (data/driver.hh, anim/drivers.hh). */
  std::vector<Driver> drivers;
  /* Switches linked by name (anim/switch.hh): a Mouth in each view of a Heads switch showing the same
   * mouth. */
  std::vector<SwitchLink> switch_links;
  uint64_t next_switch_link = 1;
  /* Shared looks for shapes (data/material.hh), in the order they were made. */
  std::vector<Material> materials;
  uint64_t next_material_id = 1;
  /* Text blocks for the text editor. Typing in them is not part of the undo history. */
  std::vector<Text> texts;
  /* Notes and drawing guides over the drawing (data/annotations.hh), with the active annotation layer
   * and the picked guide (-1: none). */
  std::vector<AnnotationLayer> annotations;
  int active_annotation = 0;
  std::vector<DrawingGuide> guides;
  int active_guide = -1;
  GuideSettings guide_settings;

  /* The active shape (Blender's active element): the shape last clicked, in the viewport or the
   * Shape tab's list, by its layer and its id. The Shape and Material panels show it while it's
   * selected (otherwise the topmost selected shape). */
  uint64_t active_shape_layer = 0;
  uint64_t active_shape = 0;
  /* Top-level layers, top of the stack first. */
  std::vector<Layer> layers;
  uint64_t active_layer = 0;
  /* The active layer deselected in the Layers editor (Deselect All Layers, Alt A; a click on its empty space;
   * Ctrl click on the only picked one): it stays active, for editing and keys, but isn't picked, so the
   * timeline's Only Selected shows nothing of it. A click on a layer picks it again. */
  uint64_t active_unpicked = 0;
  uint64_t next_layer_id = 1;
  /* Changes whenever layers may have been added, removed or moved (see Revision). */
  Revision tree_revision;

  /* Field by field (undo steps share settings that are the same). */
  bool operator==(const Document &) const = default;

  static const rna::StructRNA &rna();
};


/* Revisions (see Revision): a new one, never given before; one for a layer (it may have changed); one for
 * every layer, the Simulation and the camera and the layer tree (anything may have changed). */
uint64_t revision_next();
/* The newest revision given (none given since a look: no layer changed that anyone said). */
uint64_t revision_latest();
void layer_touch(Layer &layer);
void document_touch_all(Document &doc);

Text *document_text_find(Document &doc, const std::string &name);
/* "Text", then "Text.001", "Text.002", ... as Blender names data-blocks. */
std::string document_text_unique_name(const Document &doc, const std::string &base);
/* Adds a text with a unique name based on `name` and returns it. */
Text &document_text_add(Document &doc, const std::string &name, const std::string &body = "");

/* ---------------------------------------------------------------------------------
 * Layers
 */

/* The id of Document::simulation; layer_find finds it too. */
inline constexpr uint64_t SIMULATION_LAYER_ID = ~uint64_t(0) - 1;
/* The id of Document::camera; layer_find finds it too. */
inline constexpr uint64_t CAMERA_LAYER_ID = ~uint64_t(0) - 2;
/* A fingerprint of what is selected: the active layer and bone, selected layers, bones (and their
 * ends), points and handles picked on their own. The app gives a change no undo step recorded one
 * of its own ("Select"), so every selection is in the undo history. */
uint64_t document_selection_hash(const Document &doc);
Layer *layer_find(Document &doc, uint64_t id);
const Layer *layer_find(const Document &doc, uint64_t id);
/* While one lives, layer_find in `doc` looks layers up in an index made when it began instead of
 * searching the tree (thousands of layers: thousands of lookups a frame each walking all of them). Only for
 * work that doesn't add, remove or move layers in it meanwhile (drawing the scene, building the timeline's
 * rows); nested ones share the outer index. */
class LayerIndexScope {
 public:
  explicit LayerIndexScope(const Document &doc);
  ~LayerIndexScope();
  LayerIndexScope(const LayerIndexScope &) = delete;
  LayerIndexScope &operator=(const LayerIndexScope &) = delete;

 private:
  bool owner_ = false;
};
/* Within a LayerIndexScope of `doc`: whether no layer has a Boolean modifier (false outside one). */
bool layer_index_no_booleans(const Document &doc);
/* Whether a LayerIndexScope of `doc` is in effect. */
bool layer_index_active(const Document &doc);
/* The list holding a layer (the document's or a group's children), and its index there. */
std::vector<Layer> *layer_container(Document &doc, uint64_t id, int *r_index);
/* The group containing a layer, or null at the top level. */
Layer *layer_parent(Document &doc, uint64_t id);
/* What a layer with Clip to Below clips to: the first layer below it in its group that isn't clipped
 * itself (null at the bottom). */
const Layer *layer_clip_base(const Document &doc, const Layer &layer);
/* A layer's mask cards as they apply: its own, then its Clip to Below as a card hiding what's outside
 * what its base shows (LayerMask::clip). */
std::vector<LayerMask> layer_mask_cards(const Document &doc, const Layer &layer);
Layer *document_active_layer(Document &doc);
/* Makes the topmost shape of `layer` running through `point` (or along segment `edge`, when its curve
 * is >= 0) the active shape; nothing changes when no shape does. */
void document_set_active_shape(Document &doc, const Layer &layer, int point, ShapeEdge edge = {-1, 0});
/* Layer to document transform, including every parent group. */
Transform2D layer_world_transform(const Document &doc, uint64_t id);
/* Layer `to`'s space to layer `from`'s: layer_world_transform(from).inverted() * layer_world_transform(to), but worked
 * out from the groups they share down, so it stays exactly the same while only those groups move (a character's root
 * walked across, its top group scaled). */
Transform2D layer_relative_transform(const Document &doc, uint64_t from, uint64_t to);
/* The same with the bones at rest (layer_setup_world_transform's). */
Transform2D layer_setup_relative_transform(const Document &doc, uint64_t from, uint64_t to);
/* Layer `to` as it is now, in layer `from`'s space as that was at setup: what moved `from` itself doesn't count. */
Transform2D layer_relative_to_setup(const Document &doc, uint64_t from, uint64_t to);
/* The same with the bones at rest (as on the setup frame): where a layer is before any Parent Bone
 * moves it. */
Transform2D layer_setup_world_transform(const Document &doc, uint64_t id);
/* The drawing a layer shows: its own, or its reference source's. */
const VectorDrawing &layer_drawing(const Document &doc, const Layer &layer);
/* Every layer in Layers-editor order (top first), depth-first. */
std::vector<const Layer *> document_layers_flat(const Document &doc);

/* Adds a layer above the active one (inside its group) and makes it active. */
Layer &document_add_layer(Document &doc, LayerType type, const std::string &name);
/* Names like "Layer 3" that no other layer uses. */
std::string document_layer_unique_name(const Document &doc, const std::string &base);
/* A deep copy with fresh ids, for duplicating. */
Layer layer_copy_with_new_ids(Document &doc, const Layer &layer,
                              std::map<uint64_t, uint64_t> *r_remap = nullptr);
/* A reference to `source`: same name, its own id and settings; for a group, references of
 * each child. */
Layer layer_make_reference(Document &doc, const Layer &source);
/* Points `layer`'s masks at the layers `remap` gives for the ones they name (copies of them). */
void layer_masks_remap(Layer &layer, const std::map<uint64_t, uint64_t> &remap);
/* Every link `layer` (only it, not its children) has to other layers by id, put through `map`: its reference's
 * source, its masks' layers, Draw Order (and its keys), its bones' custom shapes and the layers they stand in
 * for, its bone constraints' shape layers, and its modifiers' layers (a deform's shape, a Boolean's targets;
 * shapes are named by id within those layers, so they follow). Copying keeps a group's links relative: what
 * named a layer copied with it names the copy. */
void layer_links_map(Layer &layer, const std::function<uint64_t(uint64_t)> &map);
/* Removes a layer and makes a neighbour active. References to it become ordinary layers. */
bool document_remove_layer(Document &doc, uint64_t id);
/* Moves a layer up (-1) or down (+1) among its siblings. */
bool document_move_layer(Document &doc, uint64_t id, int direction);
/* Moves a layer into a group (at the top), or out to its parent's level when target is 0. */
bool document_reparent_layer(Document &doc, uint64_t id, uint64_t new_parent, int index);
/* Turns a reference into an ordinary layer with a copy of the drawing. */
void layer_break_reference(Document &doc, uint64_t id);

}  // namespace oanim
