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

enum class LayerType { Vector, Group };

/* Moho's MM_* masking modes for a layer inside a masking group. */
enum class MaskMode {
  Masked,
  NotMasked,
  Add,
  Subtract,
  AddInvisible,
  SubtractInvisible,
  ClearAdd,
  ClearAddInvisible,
};

/* Moho's GROUP_MASK_* settings for a group. */
enum class GroupMask { None, ShowAll, HideAll };

/* Label colours for the Layers editor, as in Moho. */
enum class LabelColor { None, Red, Orange, Yellow, Green, Blue, Purple, Gray };

struct Layer {
  uint64_t id = 0;
  std::string name;
  LayerType type = LayerType::Vector;
  bool visible = true;
  bool locked = false;
  /* Shown open in the Layers editor (groups). */
  bool expanded = true;
  LabelColor label_color = LabelColor::None;

  float2 position;
  /* Degrees, counter-clockwise. */
  float rotation = 0.0f;
  float2 scale = {1.0f, 1.0f};
  float opacity = 1.0f;

  MaskMode mask_mode = MaskMode::Masked;
  GroupMask group_mask = GroupMask::None;

  /* Id of the layer this one shows, or 0. */
  uint64_t reference_source = 0;

  VectorDrawing drawing;
  /* Group children, top of the stack first, as the Layers editor lists them. */
  std::vector<Layer> children;

  bool is_group() const { return type == LayerType::Group; }
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
