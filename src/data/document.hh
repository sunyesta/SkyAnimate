#pragma once

/* The document: everything that is saved in a .sky file.
 *
 * Milestone 1 only has scene settings. Layers, drawings and bones arrive in
 * milestones 2 to 4. Keep this a plain copyable value: undo snapshots copy it. */

#include <string>
#include <vector>

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
  /* The action being edited, like Moho's CurrentDocAction(). Empty means the mainline. */
  std::string action;
  /* Text blocks for the text editor. Typing in them is not part of the undo history. */
  std::vector<Text> texts;
  /* Scratch value for the milestone 1 operator and undo tests; removed once layers exist. */
  int counter = 0;

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

}  // namespace sky
