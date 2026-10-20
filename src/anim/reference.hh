#pragma once

/* Reference layers: a reference shows its original's drawing as it is now, with its own changes
 * kept on top (see ReferenceSettings in data/layer.hh).
 *
 * Its drawing is built from the original whenever the document is evaluated, and edits made to it
 * with the ordinary tools (a colour in the Shape panel, G on its points, Weight Paint) are turned
 * into overrides by comparing it with what was built, as Blender's library overrides are found:
 * a moved point becomes an offset (keyed like any edit: its keys add to the original's motion), a
 * changed look a per-setting override of that shape. What a reference can't keep (added or
 * deleted points, changed curves) is undone with a message: Break Reference makes it an ordinary
 * layer. */

#include <cstdint>
#include <string>

#include "core/math.hh"
#include "data/vector.hh"

namespace sky {

struct Document;
struct Layer;

/* The reference's drawing built from its original, in the original's layer space: the original's
 * drawing now (or as on the setup frame: `setup`, or Follow Original's Animation off) with the
 * reference's offsets, looks and colour swaps, and its own weights with Match Original off. */
VectorDrawing reference_build(const Document &doc, const Layer &ref, bool setup);
/* Builds every reference's drawing anew (what was picked stays picked). A reference whose original
 * is gone becomes an ordinary layer, keeping what it showed. */
void references_build(Document &doc);
/* Turns edits made to references' drawings since they were built into their overrides (undoing,
 * with a message, what can't be kept), then builds them again. */
void references_capture(Document &doc);

/* Break Reference: an ordinary layer with the drawing it showed, its offsets added to its points'
 * keys (so its animation stays as it was), its hidden shapes left out. */
void reference_break(Document &doc, Layer &ref);

/* Match Original: from the reference's drawing (in the original's layer space) to the reference's
 * own layer space, so it sits where the original does; the top reference's own transform (and
 * everything inside a referenced group) moves it from there. */
Transform2D reference_match_transform(const Document &doc, const Layer &ref);

/* Turns Match Original on or off. Off, the reference starts with the original's weights where its
 * own bone layer has bones of the same names (or automatic ones). */
void reference_set_match(Document &doc, Layer &ref, bool on);

/* What a change is about: every one, every one of a kind, or one (by point or shape id, or a colour
 * swap's index; a Look's `detail` can name just one of its settings: "fill", "line", "line_width",
 * "brush", "has_fill", "has_outline", "hidden"); Channel is one layer channel it took over (`detail`:
 * "position", "rotation", "scale" or "opacity"); Selected, the points picked on it; Styles, every
 * style change (colours, line widths, brushes, fill and outline, colour swaps; not hidden shapes).
 * Points take their handles along. */
enum class ReferenceKind { All, Points, Looks, Swaps, Weights, Channels, Point, Look, Swap, Channel, Selected, Styles };
bool reference_kind_from_identifier(const std::string &id, ReferenceKind *r_kind);
/* Drops overrides, so those things follow the original again. Returns how many went. */
int reference_revert(Document &doc, Layer &ref, ReferenceKind kind, uint64_t which = 0,
                     const std::string &detail = "");
/* Writes overrides into the original (so every reference and the original get them) and drops
 * them. Offsets add to the original's points (their keys too); looks and colour swaps recolour its
 * shapes. Hiding a shape and own weights stay this reference's. Returns how many went. */
int reference_apply(Document &doc, Layer &ref, ReferenceKind kind, uint64_t which = 0,
                    const std::string &detail = "");
/* How many changes of its own a reference has (its own weights count as one). */
int reference_change_count(const Layer &ref);
/* Drops overrides of points and shapes the original no longer has. Returns how many went. */
int reference_remove_unused(Document &doc, Layer &ref);
/* Sync Channel to Original: a layer channel the reference took over ("position", "rotation",
 * "scale", "opacity") follows the original again (its own keys go). */
void reference_sync_channel(Layer &ref, const std::string &name);
/* Hides (or shows again) one of the original's shapes on this reference. */
void reference_hide_shape(Layer &ref, uint64_t shape_id, bool hide);
/* Swaps a colour of the original for another on this reference (`to` == `from` removes it). */
void reference_swap_color(Layer &ref, Color from, Color to, bool fill, bool line);
/* The original's shape look as this reference starts from it: with its colour swaps. */
Style reference_swapped_style(const Layer &ref, const Style &style);

}  // namespace sky
