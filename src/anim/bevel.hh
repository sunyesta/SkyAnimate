/* The Bevel modifier: cuts a drawing's corners off and rounds them (Blender's vertex bevel, for
 * curves): the corners sharper than an angle, or the points of a point group (their weights scaling
 * each one's amount). Each corner point becomes two, one on each side, joined by a new segment whose
 * Profile goes from inward through straight and round to square, as Blender's does; the curve on
 * either side keeps its shape exactly (the segments are trimmed, not refitted). */

#pragma once

#include "data/layer.hh"

namespace oanim {

/* `drawing` with modifier `m`'s bevels (its points' bone weights go to both points of each bevel). */
VectorDrawing drawing_bevel(const VectorDrawing &drawing, const Modifier &m);

}  // namespace oanim
