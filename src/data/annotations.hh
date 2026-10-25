/* Annotations and drawing guides: notes and visual aids over the drawing, never part of it. They aren't
 * layers, are never rendered, aren't masked or moved by bones, look the same on every frame, and the
 * drawing tools take no notice of them. Both are in document space and saved with the document (and
 * undone with it); the viewport draws them as overlays (Overlays > Annotations, Overlays > Guides). */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Document;

/* What an annotation line's end looks like, Blender's arrow styles. */
enum class AnnotationEnd : uint8_t { None, Arrow, OpenArrow, Segment, Dot };
inline constexpr int ANNOTATION_END_COUNT = 5;
const char *annotation_end_identifier(AnnotationEnd end);
const char *annotation_end_name(AnnotationEnd end);
bool annotation_end_from_identifier(const std::string &identifier, AnnotationEnd *r_end);

/* One stroke: a polyline in document space, with what its two ends look like. */
struct AnnotationStroke {
  std::vector<float2> points;
  AnnotationEnd start = AnnotationEnd::None;
  AnnotationEnd end = AnnotationEnd::None;
  bool operator==(const AnnotationStroke &) const = default;
};

/* Blender's annotation layers: strokes in one colour and thickness (screen pixels, so a note reads the
 * same at any zoom). */
struct AnnotationLayer {
  std::string name = "Note";
  Color color = {0.93f, 0.25f, 0.2f, 1.0f};
  float thickness = 3.0f;
  bool hidden = false;
  bool locked = false;
  std::vector<AnnotationStroke> strokes;
  bool operator==(const AnnotationLayer &) const = default;
};

inline constexpr float ANNOTATION_THICKNESS_MIN = 1.0f;
inline constexpr float ANNOTATION_THICKNESS_MAX = 20.0f;

/* Drawing guides, as Toon Boom's (only to look at): Line, an endless line through its two handles;
 * Perspective, one to three vanishing points with a fan of `lines` lines round each and the horizon
 * through the first two (level through a lone one); Grid, square cells from the origin handle, the
 * second handle at a cell's corner along one side (its size and turn). */
enum class GuideType : uint8_t { Line, Perspective, Grid };
const char *guide_type_identifier(GuideType type);
const char *guide_type_name(GuideType type);
bool guide_type_from_identifier(const std::string &identifier, GuideType *r_type);

struct DrawingGuide {
  GuideType type = GuideType::Line;
  /* Its handles, in document space: Line 2, Perspective 1 to 3 (the vanishing points), Grid 2. */
  std::vector<float2> points;
  /* Perspective: how many lines go round each vanishing point. */
  int lines = 24;
  bool hidden = false;
  bool locked = false;
  bool operator==(const DrawingGuide &) const = default;
};

inline constexpr int GUIDE_LINES_MIN = 4;
inline constexpr int GUIDE_LINES_MAX = 180;

/* The active annotation layer, made ("Note") when there's none. */
AnnotationLayer &annotation_layer_ensure(Document &doc);
/* The active annotation layer, or null. */
AnnotationLayer *annotation_layer_active(Document &doc);
/* `base`, or "base.001", "base.002"... so no two annotation layers share a name. */
std::string annotation_layer_unique_name(const Document &doc, const std::string &base);

/* Rubs out what's within `radius` of `center` (document space) on the shown, unlocked layers: a stroke
 * the circle crosses is split there (its ends lose their arrowheads where cut). Returns whether
 * anything changed. */
bool annotation_erase(Document &doc, float2 center, float radius);

/* The handles a guide of `type` has (Perspective: `count` of them, 1 to 3). */
int guide_point_count(GuideType type, int count = 2);
/* A guide made sensible: its handles counted for its type (missing ones put beside the others),
 * Lines in range, a Grid cell not too small. */
void guide_validate(DrawingGuide &guide);
/* The guide's lines as segments covering the box `lo`..`hi` (document space), for drawing and picking.
 * A grid whose cells are under `min_cell` across draws none (too dense to see). */
void guide_segments(const DrawingGuide &guide, float2 lo, float2 hi, float min_cell,
                    std::vector<std::pair<float2, float2>> &r_segments);
/* How far `p` is from the guide's nearest line within the box (for picking it). */
float guide_distance(const DrawingGuide &guide, float2 p, float2 lo, float2 hi, float min_cell);

}  // namespace oanim
