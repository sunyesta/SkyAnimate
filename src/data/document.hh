#pragma once

/* The document: everything that is saved in a .sky file.
 *
 * Bones arrive in milestone 4. Keep this a plain copyable value: undo snapshots copy it. */

#include <cstdint>
#include <string>
#include <vector>

#include "data/layer.hh"
#include "rna/rna.hh"

namespace sky {

/* A text block, like Blender's Text: scripts and notes saved with the document. */
struct Text {
  std::string name;
  std::string body;
  /* File the text was opened from or saved to; empty for a text that only lives in the document. */
  std::string filepath;
};

struct Document {
  std::string name = "Untitled";
  int frame_start = 1;
  int frame_end = 250;
  int frame_current = 1;
  float fps = 24.0f;
  int width = 1920;
  int height = 1080;
  Color background = {0.92f, 0.92f, 0.92f, 1.0f};
  /* Stroke resolution: how many straight pieces draw each curve segment, in the viewport and in
   * renders, like Blender's curve resolution. Lower is faster, higher is smoother. */
  int viewport_quality = 12;
  int render_quality = 32;
  /* The action being edited, like Moho's CurrentDocAction(). Empty means the mainline. */
  std::string action;
  /* Text blocks for the text editor. Typing in them is not part of the undo history. */
  std::vector<Text> texts;

  /* Top-level layers, top of the stack first. */
  std::vector<Layer> layers;
  uint64_t active_layer = 0;
  uint64_t next_layer_id = 1;

  static const rna::StructRNA &rna();
};

/* Smart actions the timeline's action menu offers: those of the selected armature.
 * Empty until armatures exist (milestone 4). */
std::vector<std::string> document_selectable_actions(const Document &doc);

Text *document_text_find(Document &doc, const std::string &name);
/* "Text", then "Text.001", "Text.002", ... as Blender names data-blocks. */
std::string document_text_unique_name(const Document &doc, const std::string &base);
/* Adds a text with a unique name based on `name` and returns it. */
Text &document_text_add(Document &doc, const std::string &name, const std::string &body = "");

/* ---------------------------------------------------------------------------------
 * Layers
 */

Layer *layer_find(Document &doc, uint64_t id);
const Layer *layer_find(const Document &doc, uint64_t id);
/* The list holding a layer (the document's or a group's children), and its index there. */
std::vector<Layer> *layer_container(Document &doc, uint64_t id, int *r_index);
/* The group containing a layer, or null at the top level. */
Layer *layer_parent(Document &doc, uint64_t id);
Layer *document_active_layer(Document &doc);
/* Layer to document transform, including every parent group. */
Transform2D layer_world_transform(const Document &doc, uint64_t id);
/* The drawing a layer shows: its own, or its reference source's. */
const VectorDrawing &layer_drawing(const Document &doc, const Layer &layer);
/* Every layer in Layers-editor order (top first), depth-first. */
std::vector<const Layer *> document_layers_flat(const Document &doc);

/* Adds a layer above the active one (inside its group) and makes it active. */
Layer &document_add_layer(Document &doc, LayerType type, const std::string &name);
/* Names like "Layer 3" that no other layer uses. */
std::string document_layer_unique_name(const Document &doc, const std::string &base);
/* A deep copy with fresh ids, for duplicating. */
Layer layer_copy_with_new_ids(Document &doc, const Layer &layer);
/* A reference to `source`: same name, its own id and settings; for a group, references of
 * each child. */
Layer layer_make_reference(Document &doc, const Layer &source);
/* Removes a layer and makes a neighbour active. References to it become ordinary layers. */
bool document_remove_layer(Document &doc, uint64_t id);
/* Moves a layer up (-1) or down (+1) among its siblings. */
bool document_move_layer(Document &doc, uint64_t id, int direction);
/* Moves a layer into a group (at the top), or out to its parent's level when target is 0. */
bool document_reparent_layer(Document &doc, uint64_t id, uint64_t new_parent, int index);
/* Turns a reference into an ordinary layer with a copy of the drawing. */
void layer_break_reference(Document &doc, uint64_t id);

}  // namespace sky
