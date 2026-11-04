#pragma once

/* The Motion Path tool's brushes (its shelf along the bottom of the viewport): Edit is the tool itself (clicking and
 * dragging dots, keys and handles); the Shape brushes move keys, the Speed brushes paint how fast a path is gone
 * along (anim/motion_path.hh). */

#include <cstdint>
#include <string>

namespace oanim {

enum class PathBrush : uint8_t { Edit, Grab, Smooth, PaintSpeed, Faster, Slower, BlendSpeed };
inline constexpr int PATH_BRUSH_COUNT = 7;

/* "EDIT", "GRAB", "SMOOTH", "PAINT_SPEED", "FASTER", "SLOWER", "BLEND_SPEED". */
const char *path_brush_id(PathBrush brush);
/* "Edit", "Grab", "Smooth", "Paint Speed", "Faster", "Slower", "Blend Speed". */
const char *path_brush_name(PathBrush brush);
bool path_brush_from_id(const std::string &id, PathBrush *r_brush);
/* The brushes painting speed under a circle (Ctrl paints the opposite). */
inline bool path_brush_is_speed(PathBrush brush)
{
  return brush == PathBrush::PaintSpeed || brush == PathBrush::Faster || brush == PathBrush::Slower ||
         brush == PathBrush::BlendSpeed;
}
/* The brushes with a circle (a radius and a strength): all but Edit, which takes dots, keys and handles. */
inline bool path_brush_has_radius(PathBrush brush)
{
  return brush != PathBrush::Edit;
}

}  // namespace oanim
