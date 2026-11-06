#pragma once

/* The Motion Path tool's brushes (its shelf along the bottom of the viewport): Edit is the tool itself (clicking and
 * dragging dots, keys and handles); Grab and Smooth move keys, Blend Speed evens out how fast a path is gone along
 * (anim/motion_path.hh). Shift held swaps in a brush, as Blender's sculpt Shift is Smooth: Grab's is Smooth, Edit's
 * Blend Speed. */

#include <cstdint>
#include <string>

namespace oanim {

/* Edit takes dots, keys and handles; Grab and Smooth shape the path; Blend Speed, Pinch and Slide retime it. */
enum class PathBrush : uint8_t { Edit, Grab, Smooth, BlendSpeed, Pinch, Slide };
inline constexpr int PATH_BRUSH_COUNT = 6;

/* "EDIT", "GRAB", "SMOOTH", "BLEND_SPEED", "PINCH", "SLIDE". */
const char *path_brush_id(PathBrush brush);
/* "Edit", "Grab", "Smooth", "Blend Speed", "Pinch", "Slide". */
const char *path_brush_name(PathBrush brush);
bool path_brush_from_id(const std::string &id, PathBrush *r_brush);
/* The brushes painting speed under a circle (Ctrl paints the opposite: Pinch spreads). */
inline bool path_brush_is_speed(PathBrush brush)
{
  return brush == PathBrush::BlendSpeed || brush == PathBrush::Pinch;
}
/* The timing brushes (the shelf's Timing): they move no key and never change the path's shape, only when its dots
 * land. */
inline bool path_brush_is_timing(PathBrush brush)
{
  return path_brush_is_speed(brush) || brush == PathBrush::Slide;
}
/* The brush Shift swaps in while held: Grab's Smooth; Edit's and the timing brushes' Blend Speed; the others are their
 * own. */
inline PathBrush path_brush_shifted(PathBrush brush)
{
  return brush == PathBrush::Grab                                                     ? PathBrush::Smooth
         : brush == PathBrush::Edit || brush == PathBrush::Pinch || brush == PathBrush::Slide ? PathBrush::BlendSpeed
                                                                                      : brush;
}
/* Each brush's own size (screen pixels) and strength. */
struct PathBrushSettings {
  float radius = 50.0f;
  float strength = 0.5f;
};
/* The brushes with a circle (a radius and a strength): all but Edit, which takes dots, keys and handles. */
inline bool path_brush_has_radius(PathBrush brush)
{
  return brush != PathBrush::Edit;
}

}  // namespace oanim
