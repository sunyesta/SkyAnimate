#pragma once

/* Grids: a vector layer's points and curves laid out as `columns` × `rows` cells (the Shape tool's Grid),
 * what a Warp modifier will bend layers by. A grid is ordinary drawing (its nodes are points, its lines
 * curves through them, one along each row and one up each column, sharing the nodes where they cross), with
 * a note of which points are its nodes; every tool that edits points edits it. Each cell bends what is in it
 * as a Coons patch: the surface the four Bezier segments round it make, so a grid line curved with its
 * handles curves what lies along it, a point on a grid line stays on it, and an unchanged grid changes
 * nothing. */

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Style;
struct VectorDrawing;

struct DrawingGrid {
  std::string name = "Grid";
  int columns = 3;
  int rows = 3;
  /* The node points' ids, row by row from the bottom, each row left to right: (columns + 1) * (rows + 1). */
  std::vector<uint64_t> nodes;
  bool operator==(const DrawingGrid &) const = default;
};

/* At most this many cells each way. */
constexpr int GRID_CELLS_MAX = 32;

/* A grid drawing for the Shape tool: the box between `a` and `b` split into `columns` × `rows` cells, its
 * lines straight (handles a third of the way along them), one outlined shape of all of them; its points
 * have ids 1, 2, ... so `grids` can name them. Empty when the box is too small. */
VectorDrawing grid_drawing(float2 a, float2 b, int columns, int rows, const Style &style);

/* Whether every node is still there. */
bool grid_valid(const VectorDrawing &drawing, const DrawingGrid &grid);

/* Where a place is in a grid: its cell and where in it (s across, t up; 0 to 1 inside, beyond outside). */
struct GridPlace {
  int column = 0, row = 0;
  float2 st;
};

/* A grid's cells as patches, worked out once from a drawing to place many points (what the functions below
 * do one point at a time). */
struct GridSurface {
  int columns = 0, rows = 0;
  /* The Bezier segments along each row (row by row, left to right) and up each column (column by column,
   * bottom to top), and the nodes' bounds. */
  std::vector<std::array<float2, 4>> across, up;
  float2 lo, hi;

  bool build(const VectorDrawing &drawing, const DrawingGrid &grid);
  /* The patch of a cell at `st`; outside 0..1, on straight from its edge. */
  float2 eval(int column, int row, float2 st) const;
  bool locate(float2 p, GridPlace &r_place) const;

 private:
  float2 patch(int column, int row, float2 st) const;
};

/* Where `p` is in `grid` as `rest` has it (its cell, found by the Coons patches themselves, so a grid
 * reshaped at rest works too); a place outside takes the nearest cell's, past its edge. False if the grid
 * is broken. */
bool grid_locate(const VectorDrawing &rest, const DrawingGrid &grid, float2 p, GridPlace &r_place);
/* Where that place is in `grid` as `bent` has it: inside a cell its Coons patch; past the grid's edge, on
 * straight from it (the patch's slope at the edge). */
float2 grid_place(const VectorDrawing &bent, const DrawingGrid &grid, const GridPlace &place);
/* Both: `p` (where `rest` has it) to where `bent` takes it; `p` itself when the grid is broken. */
float2 grid_map(const VectorDrawing &rest, const VectorDrawing &bent, const DrawingGrid &grid, float2 p);

}  // namespace oanim
