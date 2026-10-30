#pragma once

/* Thumbnails of a switch's children, for the asset shelf and the Switch menus: each child drawn small,
 * as it looks on this frame, framed with its siblings so mouth shapes keep their size and place against
 * each other, whether the switch shows it or not. Kept until what it is drawn from changes (the child's layers,
 * where the switch is, the poses of the bones moving them), or the frame or the size; drawn again once the
 * document has been still for a moment, only a few each frame, the rest showing their last picture till their
 * turn. */

#include <cstdint>

namespace oanim {

struct Document;
struct Layer;

/* The child's thumbnail, `size` pixels square: an OpenGL texture (straight alpha), 0 until drawn or
 * without OpenGL (background runs). */
unsigned int switch_thumbnail(Document &doc, const Layer &sw, const Layer &child, int size);

/* While a drag or transform runs, or the animation plays (`hold`), thumbnails keep their last picture: the document changes every
 * frame then, and drawing them again each time (and reading them back from the GPU) slows the drag. */
void switch_thumbnails_hold(bool hold);

}  // namespace oanim
