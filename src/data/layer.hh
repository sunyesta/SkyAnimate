#pragma once

/* Layers, following Moho: a tree of vector and group layers, drawn bottom of the list first.
 *
 * Masking works per group, as in Moho: a group's mask setting decides whether masking is on
 * and whether its contents start shown or hidden, and each child's masking mode adds to the
 * mask, subtracts from it, or is clipped by it.
 *
 * A reference layer is a live copy: it shows another layer's drawing (a group reference
 * holds references to the group's children) while keeping its own settings, so the same
 * artwork can be visible in one group and act as an invisible mask in another. */

#include <cstdint>
#include <string>
#include <vector>

#include "core/math.hh"
#include "data/vector.hh"
#include "rna/rna.hh"

namespace sky {

/* Bone: a group that owns a skeleton and moves its children with it (Moho's bone layer). */
enum class LayerType { Vector, Group, Bone };

/* Moho's MM_* masking modes for a layer inside a masking group. */
/* Moho's masking modes, in the order its Masking menu lists them. Subtracting always hides the
 * layer, as in Moho's menu. */
enum class MaskMode {
  Masked,
  NotMasked,
  Add,
  AddInvisible,
  SubtractInvisible,
  ClearAdd,
  ClearAddInvisible,
};

/* Moho's GROUP_MASK_* settings for a group. */
enum class GroupMask { None, ShowAll, HideAll };

/* Label colours for the Layers editor, as in Moho. */
enum class LabelColor { None, Red, Orange, Yellow, Green, Blue, Purple, Gray };

/* One bone of a bone layer's skeleton, as in Moho's M_Bone. Its frame is the parent bone's
 * (or, for a root, the bone layer's) space moved by `position`, turned by `angle` and scaled by
 * `scale`; the bone runs `length` along its own x axis. The values at frame 0 are the setup
 * pose; the difference from it is what moves bound artwork. */
struct Bone {
  uint64_t id = 0;
  std::string name;
  /* The parent bone's id, or 0 for a root. */
  uint64_t parent = 0;
  float2 position;
  /* Radians, counter-clockwise. */
  float angle = 0.0f;
  float length = 0.5f;
  float scale = 1.0f;
  /* How far automatic weights reach, as a multiple of the length (Moho's bone strength). */
  float strength = 1.0f;
  LabelColor color = LabelColor::None;
  /* Connected, as in Blender: the head sits on the parent's tail and moves with it. */
  bool connected = false;
  /* Blender's edit-bone selection: the body, and the round joints at either end. A selected body
   * has both ends selected; a connected bone's head is its parent's tail, one joint. */
  bool selected = false;
  bool select_head = false;
  bool select_tail = false;
  Channel<float2> position_keys;
  Channel<float> angle_keys;
  Channel<float> scale_keys;
};

/* Blender-style modifiers, run on a layer's drawing in stack order. Armature is the bones
 * moving the drawing: every layer inside a bone layer has exactly one, added and removed with
 * that bone layer, never deleted by hand; where it sits in the stack decides what the bones
 * move (Mirror above it mirrors the setup drawing, so each half follows its own side's bones). */
enum class ModifierType { Mirror, Armature };

struct Modifier {
  ModifierType type = ModifierType::Mirror;
  std::string name = "Mirror";
  bool show_viewport = true;
  bool show_render = true;
  /* Shown open in the Modifiers tab. */
  bool show_expanded = true;
  /* Mirror: across the layer's vertical axis (x flips) and/or its horizontal one, through
   * `center` in the layer's space. */
  bool axis_x = true;
  bool axis_y = false;
  float2 center;
  /* Points within `merge_distance` of the axis are shared by both halves, so a curve ending
   * on the axis joins its mirror image into one line (and a half outline into a whole one). */
  bool merge = true;
  float merge_distance = 0.002f;
  /* Mirrored points bound to a bone named ..._L follow ..._R (and the other way), as
   * Blender's mirror modifier flips vertex groups. */
  bool mirror_bones = true;
};

struct Layer {
  uint64_t id = 0;
  std::string name;
  LayerType type = LayerType::Vector;
  bool visible = true;
  /* Drawn in renders (Blender's camera toggle). Off, the layer still shows in the viewport. */
  bool render_visible = true;
  bool locked = false;
  /* Shown open in the Layers editor (groups). */
  bool expanded = true;
  /* Picked in the Layers editor along with the active layer (Ctrl or Shift click), for
   * Group With Selection. Not saved. */
  bool selected = false;
  LabelColor label_color = LabelColor::None;

  float2 position;
  /* Degrees, counter-clockwise. */
  float rotation = 0.0f;
  float2 scale = {1.0f, 1.0f};
  float opacity = 1.0f;
  /* Keys for the transform and opacity (see anim/channel.hh). */
  Channel<float2> position_keys;
  Channel<float> rotation_keys;
  Channel<float2> scale_keys;
  Channel<float> opacity_keys;

  MaskMode mask_mode = MaskMode::Masked;
  /* Moho's masking options for the layer's part of its group's mask: grow it by a pixel (hides
   * thin seams along mask edges), and leave out strokes so only fills mask. */
  bool mask_expand = false;
  bool mask_exclude_strokes = false;
  GroupMask group_mask = GroupMask::None;

  /* Id of the layer this one shows, or 0. */
  uint64_t reference_source = 0;

  VectorDrawing drawing;
  /* Group children, top of the stack first, as the Layers editor lists them. */
  std::vector<Layer> children;

  /* Bone layers: the skeleton, roots before their children. */
  std::vector<Bone> bones;
  uint64_t next_bone_id = 1;
  uint64_t active_bone = 0;
  /* Moho's layer binding: a layer inside a bone layer can follow one bone as a whole. */
  uint64_t parent_bone = 0;
  /* Run on the drawing in order (see ModifierType). */
  std::vector<Modifier> modifiers;
  /* Bone layers: the names of their actions, alternative timelines for the layer and everything
   * inside it (see anim/animation.hh). */
  std::vector<std::string> actions;

  bool is_group() const { return type == LayerType::Group || type == LayerType::Bone; }
  bool is_bone_layer() const { return type == LayerType::Bone; }
  bool is_reference() const { return reference_source != 0; }
  bool mask_hides_layer() const;
  bool mask_adds() const;
  bool mask_subtracts() const;
  bool mask_clears() const;
  Transform2D local_transform() const;

  static const rna::StructRNA &rna();
};

const char *layer_type_name(LayerType type);
/* Colour of a label in the Layers editor, 0 alpha for None. */
Color label_color_value(LabelColor label);

}  // namespace sky
