#pragma once

/* The camera: what renders show, a frame on the infinite canvas.
 *
 * It is Document::camera, a layer outside the tree (CAMERA_LAYER_ID, "Camera", as the Simulation
 * settings are), so the timeline and the key tools handle its channels as any layer's: "position"
 * (the frame's centre), "rotation" (its roll, degrees) and "zoom". The zoom channel holds log2 of the
 * zoom, so keys between two zooms change it at a steady rate (zooming from 1 to 4 passes 2 halfway),
 * as a camera's zoom should; camera_zoom() gives the zoom itself.
 *
 * At zoom 1, unrolled, at the origin, the frame is 2 units tall and as wide as the document's
 * aspect (where the page always was): zoom 2 shows half as much. */

#include "core/math.hh"

namespace sky {

struct Document;
struct Layer;

/* The camera made ready (its id, name and zoom channel), for files and documents from before it. */
Layer &document_camera(Document &doc);
float camera_zoom(const Document &doc);
void camera_set_zoom(Document &doc, float zoom);
/* Document space to the camera's frame space, where the frame is [-aspect, aspect] x [-1, 1]
 * (renders map that space to pixels). */
Transform2D camera_to_frame(const Document &doc);
/* The frame's corners in document space: bottom left, bottom right, top right, top left. */
void camera_frame_corners(const Document &doc, float2 r_corners[4]);
/* The frame's half width and height in document space. */
float2 camera_half_size(const Document &doc);

}  // namespace sky
