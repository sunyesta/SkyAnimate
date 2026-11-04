#pragma once

/* The Motion Path tool's brushes (its shelf along the bottom of the viewport): Edit is the tool itself (clicking and
 * dragging dots, keys and handles); the Shape brushes move keys, the Speed brushes paint how fast a path is gone
 * along (anim/motion_path.hh). */

#include <cstdint>
#include <string>

namespace oanim {

enum class PathBrush : uint8_t { Edit, Grab, Smooth, PaintSpeed, Faster, Slower, BlendSpeed, EditSpeed };
inline constexpr int PATH_BRUSH_COUNT = 8;

/* "EDIT", "GRAB", "SMOOTH", "PAINT_SPEED", "FASTER", "SLOWER", "BLEND_SPEED", "EDIT_SPEED". */
const char *path_brush_id(PathBrush brush);
/* "Edit", "Grab", "Smooth", "Paint Speed", "Faster", "Slower", "Blend Speed", "Edit Speed". */
const char *path_brush_name(PathBrush brush);
bool path_brush_from_id(const std::string &id, PathBrush *r_brush);
/* The brushes painting speed under a circle (Ctrl paints the opposite). */
inline bool path_brush_is_speed(PathBrush brush)
{
  return brush == PathBrush::PaintSpeed || brush == PathBrush::Faster || brush == PathBrush::Slower ||
         brush == PathBrush::BlendSpeed;
}
/* The brushes with a circle (a radius and a strength): not Edit or Edit Speed, which take dots. */
inline bool path_brush_has_radius(PathBrush brush)
{
  return brush != PathBrush::Edit && brush != PathBrush::EditSpeed;
}

}  // namespace oanim
