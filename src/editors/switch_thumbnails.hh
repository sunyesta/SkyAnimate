#pragma once

/* Thumbnails of a switch's children, for the asset shelf and the Switch menus: each child drawn small,
 * as it looks on this frame, framed with its siblings so mouth shapes keep their size and place against
 * each other, whether the switch shows it or not. Kept until the document, the frame or the size changes;
 * only a few are drawn again each frame, the rest showing their last picture till their turn. */

#include <cstdint>

namespace oanim {

struct Document;
struct Layer;

/* The child's thumbnail, `size` pixels square: an OpenGL texture (straight alpha), 0 until drawn or
 * without OpenGL (background runs). */
unsigned int switch_thumbnail(Document &doc, const Layer &sw, const Layer &child, int size);

}  // namespace oanim
