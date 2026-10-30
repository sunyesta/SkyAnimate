#pragma once

/* What the tool shelves along the bottom of the viewport (Sculpt's, Draw's) have in common, as Blender's
 * asset shelf: a strip with tabs, a search field and the tiles' size along its top and tiles under them. It
 * folds to its tabs with its arrow and sizes by its top edge. The viewport's view sits above it, so it never
 * covers the drawing. */

#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace oanim {

/* A shelf in one viewport: open or folded to its tabs, its height when open (0: one row of tiles), the tab,
 * the search text and the tiles' size in pixels (0: the default). */
struct ShelfState {
  bool open = true;
  float height = 0.0f;
  int tab = 0;
  std::string search;
  float tile = 0.0f;
};

/* A tab to show: the number `ShelfState::tab` holds for it, and its name. */
struct ShelfTab {
  int tab = 0;
  const char *name = "";
};

float shelf_tile_size(const ShelfState &shelf);
/* A tile and the name under it. */
float shelf_tile_height(const ShelfState &shelf);
/* How much of the viewport's height the shelf takes. */
float shelf_height(const ShelfState &shelf);
/* Lower-case, for searching. */
std::string shelf_lower(std::string s);

/* The strip at `min`: its background, top edge and header. False: it is folded, with no tiles to draw. */
bool shelf_frame(ShelfState &shelf, const std::vector<ShelfTab> &tabs, ImVec2 min, ImVec2 size);
/* Just the header's tabs, the tiles' size and the search, at the cursor: for a shelf in a popup. */
void shelf_tabs(ShelfState &shelf, const std::vector<ShelfTab> &tabs);

/* A tile's white background and its active or hovered ring, from `a` to `b`. */
void shelf_tile_box(ImDrawList *dl, ImVec2 a, ImVec2 b, bool active, bool hovered);
/* The name under a tile, cut to its width. */
void shelf_tile_name(ImDrawList *dl, ImVec2 a, ImVec2 b, const std::string &name, bool active);
/* The + tile that saves what is being used. */
void shelf_plus_tile(ImDrawList *dl, ImVec2 a, ImVec2 b, bool hovered);

}  // namespace oanim
