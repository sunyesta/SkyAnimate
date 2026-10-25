#pragma once

/* Keeping text layers' letters in step with their text. */

#include <string>

#include "data/layer.hh"

namespace oanim {

struct Document;

/* Makes every text layer's drawing match its text, wherever the text was changed (the Text tab, the
 * Text tool, Python, a driver): its runs take their materials' looks first, then a text no longer
 * what its letters were made from has them made again. Returns whether any changed. */
bool document_sync_text(Document &doc);
/* One text layer's letters made again now. */
void text_layer_rebuild(Layer &layer);

/* A new text layer above the active one (as document_add_layer puts layers), with `body` in one run
 * of the given font and size, at `position` (document space). */
Layer &document_add_text_layer(Document &doc, const std::string &body, float2 position,
                               const std::string &font = "Segoe UI", float size = 0.12f);

/* Convert to Vectors: the text layer becomes an ordinary vector layer of its letters, as they are
 * (`split` "LETTERS" or "WORDS": a vector layer each instead, in a group named after it). */
bool text_layer_convert_to_vectors(Document &doc, uint64_t layer_id, const std::string &split = "NONE");

}  // namespace oanim
