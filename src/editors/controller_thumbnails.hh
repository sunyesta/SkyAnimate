#pragma once

/* Thumbnails of a 2 Axis Controller's spots, for the Spots panel: the bone layer's drawing as it looks with the knob on
 * the spot (drawn from a posed copy, anim/controller_preview.hh), every spot of a controller framed alike so the poses
 * compare. Kept until the document, the frame, the spot or the size changes; drawn again once the document has been
 * still a moment, one a frame, the rest showing their last picture till their turn. */

#include <cstdint>

namespace oanim {

struct Document;
struct Layer;
struct Bone;
struct ControllerSpot;

/* The spot's thumbnail, `size` pixels square: an OpenGL texture (straight alpha), 0 until drawn or without OpenGL
 * (background runs). */
unsigned int controller_spot_thumbnail(Document &doc, const Layer &armature, const Bone &knob, const ControllerSpot &spot,
                                       int size);

}  // namespace oanim
