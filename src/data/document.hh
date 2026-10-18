#pragma once

/* The document: everything that is saved in a .sky file.
 *
 * Milestone 1 only has scene settings. Layers, drawings and bones arrive in
 * milestones 2 to 4. Keep this a plain copyable value: undo snapshots copy it. */

#include <string>

#include "rna/rna.hh"

namespace sky {

struct Document {
  std::string name = "Untitled";
  int frame_start = 1;
  int frame_end = 250;
  int frame_current = 1;
  float fps = 24.0f;
  int width = 1920;
  int height = 1080;
  /* Scratch value for the milestone 1 operator and undo tests; removed once layers exist. */
  int counter = 0;

  static const rna::StructRNA &rna();
};

}  // namespace sky
