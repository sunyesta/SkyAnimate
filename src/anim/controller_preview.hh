#pragma once

/* A 2 Axis Controller's knob shown somewhere without moving it: the Spots panel's hover preview and its thumbnails.
 * Drawn from a copy of the document with the knob there and its actions played, so nothing in the document changes:
 * nothing is keyed, recorded or saved, and no undo step is made. */

#include <cstdint>

#include "core/math.hh"
#include "data/document.hh"

namespace oanim {

/* A copy of `doc` with knob `bone` of bone layer `armature` at `at` (frames), its actions played there. On frame 0,
 * where nothing plays, as on frame 1. */
Document controller_posed_copy(const Document &doc, uint64_t armature, uint64_t bone, float2 at);
/* Counts the document's changes, leaving out the revisions posed copies give their own layers (so pictures made from
 * them don't count as the document changing): the same while nothing else has changed. */
uint64_t controller_document_generation();

/* While set, the viewport draws the knob at `at` (its bones and overlays stay as they are). One at a time. */
void controller_preview_set(uint64_t armature, uint64_t bone, float2 at);
void controller_preview_clear();
bool controller_preview_active(uint64_t *r_armature = nullptr, uint64_t *r_bone = nullptr, float2 *r_at = nullptr);
/* The document to draw instead of `doc` while a preview is set (made again as `doc` changes); null with none. */
const Document *controller_preview_document(const Document &doc);
/* Changes whenever what the preview shows does (0 with none): for the viewport's "same as last time". */
uint64_t controller_preview_stamp();

}  // namespace oanim
