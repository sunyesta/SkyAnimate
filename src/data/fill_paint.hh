#pragma once

/* The Fill Pen and Fill Eraser: painting a brushed area into a drawing's fills, and cutting one out of them.
 *
 * Both work on what the fills cover rather than on their points: the area is merged or cut with Clipper2 and
 * the outcome fitted back to Bezier curves, so what comes out is ordinary shapes. Lines are left alone: a fill
 * cut under an outline stays an outline, with what is left of the fill just below it. */

#include <vector>

#include "data/fill_area.hh"

namespace oanim {

/* Paints `area` (layer space) in `style`'s fill, as Illustrator's Blob Brush. It merges with the fills it overlaps
 * that look the same, have no outline and are free to rebuild (no keys, not sharing a curve), down the stack as
 * far as a different fill it overlaps; the blob stays where the topmost of them was, so lines drawn over a colour
 * stay over it. Nothing to merge with: a new shape on top. `pixel`: layer units a screen pixel stands for;
 * `tolerance`: how far (screen pixels) the curves may be off. */
void drawing_fill_paint(VectorDrawing &drawing, const std::vector<FillPiece> &area, const Style &style, float pixel,
                        float tolerance);

/* Cuts `area` out of the drawing's fills (`selected_only`: of the selected shapes'). A shape with keys can't be
 * rebuilt without losing them: it is left, and counted in `r_skipped`. False: nothing was cut. */
bool drawing_fill_erase(VectorDrawing &drawing, const std::vector<FillPiece> &area, float pixel, float tolerance,
                        bool selected_only, int *r_skipped);

}  // namespace oanim
