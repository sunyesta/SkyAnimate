#pragma once

/* How the project is shown: settings saved with the file that aren't part of its artwork. */

namespace oanim {

/* Which curve handles the viewport shows, as Blender's Overlays > Handles. */
/* Shape: every handle of the shapes being worked on (any of whose points is selected), plus the
 * selected points'. */
enum class HandleDisplay { None, Selected, Shape, All };
/* Which bones the viewport shows: none, those of selected bone layers, or every bone layer's. */
enum class ArmatureDisplay { Off, Selected, On };

/* What the viewports draw over the artwork, like Blender's viewport overlays: part of the project,
 * saved with it (as a .blend keeps its views' overlays) but not of undo; `show` turns them all off
 * at once. */
/* Point Size and Handle Size's range, in pixels. */
constexpr float OVERLAY_SIZE_MIN = 2.0f;
constexpr float OVERLAY_SIZE_MAX = 24.0f;

struct ViewportOverlays {
  bool show = true;
  bool grid = false;
  bool cursor = true;
  bool camera_frame = true;
  /* Camera, frame and layer in the top-left corner. */
  bool info = true;
  /* Curve wires and points of the active layer while editing it. */
  bool wireframe = true;
  bool points = true;
  HandleDisplay handles = HandleDisplay::Shape;
  /* How big points and handles' ends are drawn, in screen pixels across (Blender's Vertex Size);
   * clicks reach a little past them. */
  float point_size = 6.0f;
  float handle_size = 6.5f;
  /* The active layer's mask layers outlined, dashed and labelled ("inside head"); off, they only
   * show while a mask's symbol or Layers field is under the mouse. */
  bool mask_outlines = true;
  ArmatureDisplay armature = ArmatureDisplay::On;
  /* Blender's Relationship Lines: a dashed line from a bone that isn't connected to its parent's tip. */
  bool relationship_lines = true;
  /* Bones drawn where their keys put them, and pale ghosts where bone dynamics have them (the
   * artwork follows the ghosts); off, bones are drawn where dynamics have them. */
  bool physics_ghosts = true;
  /* Bone dynamics run live while a pose changes on a frame (trailing, swinging and settling in
   * real time); off, the bones show the frame's own result. Live in any viewport of the
   * workspace shown runs it. */
  bool live_physics = true;
  /* Bones with a custom shape (a pin) are drawn as it; off, every bone is drawn as a bone. */
  bool bone_shapes = true;
  /* Guides: layers with Show in Renders off (references, Media layers by default). With them and the
   * overlays off, the viewport shows what renders. */
  bool guides = true;

  bool operator==(const ViewportOverlays &) const = default;
};

/* Where the viewport looks: the document point at its middle and pixels per document unit (0: not
 * set, so the viewport frames the camera). Saved with the project; moving the view isn't an edit. */
struct ViewportView {
  float center_x = 0.0f;
  float center_y = 0.0f;
  float zoom = 0.0f;
};

}  // namespace oanim
