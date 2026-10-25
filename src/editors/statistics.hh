#pragma once

/* The statistics at the right of the status bar, as Blender's Scene Statistics: what is being worked in
 * and how much of it is selected. Editing drawings (the layers the point tools work on): their points,
 * curves, segments and shapes; in a bone layer: its bones; otherwise the document's layers and points.
 * Then the frame and the version, each of the three turned on or off from the status bar's right-click
 * menu (saved in the preferences). */

#include <cstdint>
#include <string>
#include <vector>

namespace oanim {

struct Context;
struct Document;

struct Statistics {
  enum class Mode { Drawings, Bones, Document };
  Mode mode = Mode::Document;
  /* The active layer's name, "" without one. */
  std::string name;
  /* How many drawings are being edited (a group with Edit Descendants brings many). */
  int drawings = 0;
  /* Selected and in all, over the drawings being edited (Document: every drawing's, none selected).
   * Points above their curve's edit level are hidden and not counted. */
  int points = 0, points_selected = 0;
  int curves = 0, curves_selected = 0;
  /* A segment is selected when both its points are; a curve or shape when all of its points are. */
  int segments = 0, segments_selected = 0;
  int shapes = 0, shapes_selected = 0;
  /* The active bone layer's bones shown (in shown bone collections); selected by body or either joint. */
  int bones = 0, bones_selected = 0;
  /* Every layer in the tree; selected or active. */
  int layers = 0, layers_selected = 0;
  int frame = 0, frame_end = 0;
};

/* Worked out again only when something may have changed it (an undo step, the active layer, the frame,
 * which layers are selected, shown or locked): a big rig's points are counted once, not every frame.
 * `fresh` counts now whatever (a script's selection pushes no undo step), and the status bar shows that. */
Statistics document_statistics(Context &C, bool fresh = false);

/* What the status bar shows: Blender's "Name | Points:12/529 | ... | 0.1.0", with only the parts asked for. */
std::vector<std::string> statistics_parts(const Statistics &s, bool statistics, bool frame, const char *version);

/* 1234567 as "1,234,567". */
std::string statistics_number(int n);

}  // namespace oanim
