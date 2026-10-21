#pragma once

/* Picking layers by where they show, for the Select Layer tool and Alt-click (layer.select_at): the
 * drawing layers drawn at a point, top first (Draw Order included), counting only where a layer
 * really shows: its fills and lines, and only where its masks (and its groups') let it show.
 * Media layers are never picked (references are traced over, not edited); hidden layers, guides
 * while guides are hidden, and layers outside Local View aren't either. */

#include <cstdint>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Document;
struct Layer;

struct LayerPickOptions {
  /* How near a line counts as on it, in document units (a few pixels at the view's zoom). */
  float tolerance = 0.0f;
  /* Guides (layers not rendered) show: the viewport's overlays and their Guides are on. */
  bool show_guides = true;
  /* Local View's layers, or empty for every layer. */
  std::vector<uint64_t> local_view;
};

/* The drawing layers shown at `at` (document space), top first. */
std::vector<uint64_t> layers_at(const Document &doc, float2 at, const LayerPickOptions &options);

/* Whether `layer`'s masks (its own rows, its Clip to Below, and its groups') let it show at `at`.
 * Grow and Extend aren't counted. */
bool layer_masks_show_at(const Document &doc, const Layer &layer, float2 at);

/* The group a pick with Pick: Group takes instead of `id`: its outermost group (the layer itself at
 * the top level). */
uint64_t layer_pick_group(const Document &doc, uint64_t id);

/* How a pick changes the layer selection: the picked layer alone, added to it or taken out if
 * picked already (Shift), or taken out (Ctrl). */
enum class LayerPickMode { Set, Toggle, Remove };
/* Applies a pick; returns whether the selection changed. */
bool layer_pick_apply(Document &doc, uint64_t id, LayerPickMode mode);

}  // namespace oanim
