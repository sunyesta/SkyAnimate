/* The Subdivide modifier: splits a drawing's segments into smaller pieces before the bones bend it, so
 * a long segment bends along the bone chain instead of only at its two points (Blender's Grease Pencil
 * Subdivide, for curves). The split is exact (de Casteljau): at rest the drawing looks the same. By
 * Length puts a point at least every Spacing along each segment, measured in the setup pose so the
 * pieces don't change from frame to frame; By Count splits every segment into the same number. Corners
 * stay corners; the new points are smooth. Their bone weights blend their two end points' by how far
 * along they are (Blend), or come from the nearest bones (From Bones, done where the bones are known:
 * anim/deform.cc). Placed above the Armature modifier, where it helps. */

#pragma once

#include <vector>

#include "data/layer.hh"

namespace oanim {

/* How many pieces each segment of `curve` becomes under `m` (render: its Render Spacing). */
int subdivide_pieces(const VectorDrawing &drawing, const Curve &curve, int segment, const Modifier &m, bool render);

/* `drawing` with modifier `m`'s splits. Its own points keep their indices; `r_new_points` gets the new
 * ones' (all at the end). Detail is made final first (the new curve has no detail of its own). */
VectorDrawing drawing_subdivide(const VectorDrawing &drawing, const Modifier &m, bool render,
                                std::vector<int> *r_new_points = nullptr);

}  // namespace oanim
