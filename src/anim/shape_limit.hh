#pragma once

/* Limit to Shape (BoneConstraintType::LimitShape): every frame, the shape each such constraint keeps
 * its bone inside, as that shape is drawn there (bent by its own bones, moved by its layers), and the
 * outline of what the bone carries around its head, both in the bone layer's space. The pose then keeps
 * the head where that outline fits (bone_shape_limit, data/bones.hh). Look At's Eye Shape likewise, and
 * how big that shape is at frame 0. */

namespace oanim {

struct Document;

void shape_limits_update(Document &doc);

}  // namespace oanim
