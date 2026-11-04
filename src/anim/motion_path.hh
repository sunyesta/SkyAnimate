#pragma once

/* Motion paths (Document::motion_paths): where things go over the frames around the shown one, a dot per frame,
 * as Blender's motion paths and Maya's motion trails. A bone traces its tip (or its head), so turning draws an arc
 * and moving a line; a point traces itself, through its bones; a layer with nothing picked in it traces its
 * origin. The range runs from `path_before` keys back to `path_after` keys ahead of the shown frame, by the keys
 * of what is traced and of everything moving it. Frames are worked out on a copy of the document from their keys
 * alone, as onion skins are; deforms are left out (bones and transforms only), so paths stay quick. */

#include <cstdint>
#include <string>
#include <vector>

#include "anim/animation.hh"
#include "anim/path_brush.hh"
#include "core/math.hh"
#include "data/document.hh"
#include "data/vector.hh"

namespace oanim {

struct Layer;

/* What the paths trace: for each of `editing` (the layers being edited), its selected bones or points, or with
 * none of those its origin; and every pinned target. */
std::vector<MotionPathTarget> motion_path_targets(const Document &doc, const std::vector<Layer *> &editing);
/* The kept style of `target` (pin, colour, end), or null. */
const MotionPathTarget *motion_path_style(const Document &doc, const MotionPathTarget &target);
MotionPathTarget &motion_path_style_ensure(Document &doc, const MotionPathTarget &target);
/* The frames the paths of `targets` cover. False with nothing to trace. */
bool motion_path_range(const Document &doc, const std::vector<MotionPathTarget> &targets, int &r_first,
                       int &r_last);
/* The keys of what moves `target` (its own and those of the bones and layers above it), in the animation. */
std::vector<int> motion_path_keys(const Document &doc, const MotionPathTarget &target);

struct MotionPath {
  MotionPathTarget target;
  Color color;
  int first = 0;
  /* Where it is on each frame from `first` on, in document space (drawn relative to Document::path_relative). */
  std::vector<float2> points;
  /* How fast it moves into each frame, 0 (still) to 1 (its fastest). */
  std::vector<float> speed;
  std::vector<int> keys;
};
/* The paths of `targets` over frames first..last. */
std::vector<MotionPath> motion_paths_make(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                          int first, int last);
/* A path's colour: its own, or one of a few told apart by the order it comes in. */
Color motion_path_color(const Document &doc, const MotionPathTarget &target, size_t index);
/* The drawings to show as a ghost from a path's dot: the target's layer, or the drawings a bone layer or group
 * carries. */
std::vector<uint64_t> motion_path_ghost_layers(const Document &doc, const std::vector<MotionPathTarget> &targets);
/* A label for a target: "arm_L tip", "Face point 12", "Face". */
std::string motion_path_label(const Document &doc, const MotionPathTarget &target);
/* Red where fast, blue where still. */
Color motion_path_speed_color(float speed);
/* Time through a stretch between keys (0 just after its first key to 1 arriving at the next), the same everywhere a
 * dot is drawn (the viewport, the timeline): blue, teal, green, yellow, as viridis without its darkest end, so it
 * reads on the dark viewport and to colour-blind eyes. */
Color motion_path_time_color(float t);
/* A dot outside the stretch being worked on: light grey, a little see-through. */
Color motion_path_unlit_color();
/* How far through its stretch's time `frame` is, from the frames of the keys (sorted): (frame - start) / (end -
 * start) with start < frame < end the keys either side; false on a key or outside every stretch. */
bool motion_path_stretch_time(const std::vector<int> &keys, int frame, float &r_t, int *r_start = nullptr,
                              int *r_end = nullptr);
/* Whether the stretch from key `start` to key `end` is the one being worked on: it holds the frame shown (on a key,
 * the stretches either side both do), or one of `dots` (frames of dots selected on this path). */
bool motion_path_stretch_lit(int start, int end, int current, const std::vector<int> &dots);
/* Each of a path's dots' colour, by doc.path_dot_colors; `dots` the frames of its selected dots. Keys are drawn as
 * keys, so theirs is the unlit grey. */
std::vector<Color> motion_path_dot_colors(const Document &doc, const MotionPath &path, const std::vector<int> &dots);

/* --- Editing, the Motion Path tool's: every edit lands on keys (motion_path_record keys it whatever auto-key says). */

/* Where `target` shows now, and on `frame` (from its keys alone), in document space. */
bool motion_path_position(const Document &doc, const MotionPathTarget &target, float2 &r_at);
bool motion_path_position_at(const Document &doc, const MotionPathTarget &target, int frame, float2 &r_at);
/* Moves `target` so it shows at `at` (document space) on the frame shown, by its own values: a bone's tip by turning
 * the bone, its head by moving it, a point or a layer's origin by moving that. False where it can't (a connected
 * bone's head). */
bool motion_path_place(Document &doc, const MotionPathTarget &target, float2 at);
/* The keys of what moves `target` either side of `frame` (0 where there's none). */
void motion_path_neighbour_keys(const Document &doc, const MotionPathTarget &target, int frame, int &r_before,
                                int &r_after);
/* Ctrl while moving a dot: `at` onto the straight line between where the target is on the keys either side of the
 * frame shown (the nearest place on it). False without keys both sides. */
bool motion_path_straighten(const Document &doc, const MotionPathTarget &target, float2 &at);
/* A breakdown, as tweenMachine's: the target's own values on the frame shown become `factor` (0-1) of the way from
 * those on the key before to those on the key after, so a turning bone stays on its arc. */
bool motion_path_breakdown(Document &doc, const MotionPathTarget &target, float factor);
/* The channels of `target` itself, for picking its keys. */
KeyFilter motion_path_key_filter(const Document &doc, const MotionPathTarget &target);
/* Keys the edits on the frame shown, with auto-key on or off. */
void motion_path_record(Document &doc);
/* Dragging a dot with Edit: the dot on `frame` (between keys) slides along its path to the place on it nearest `at` (document space
 * on that frame), the keys staying on their frames: its stretch becomes Custom (as the speed brushes make it) with
 * speeds putting the dot there. With proportional editing the dots within `size` frames of it slide too, by the
 * falloff, never past the keys either side; dots never pass each other. Returns false for a frame on a key, held, or
 * in a stretch that doesn't move (Step, Pose, Cycle). */
struct PathSlide {
  bool proportional = false;
  float size = 3.0f;
  ProportionalFalloff falloff = ProportionalFalloff::Smooth;
  /* Dots kept in order between the keys (the viewport's drag along the path); off, a dot can go past its key or back
   * before its start (the timing chart's and Path Timing panel's, for overshoot). */
  bool keep_order = true;
};
bool motion_path_slide(Document &doc, const MotionPathTarget &target, int frame, float2 at, const PathSlide &slide);
/* Alt S with Edit: the dots around the one on `frame` brought `factor` as far from it along the path (under 1
 * closer, so slower there; over 1 further, faster), the keys staying on their frames. On a key, the dots either
 * side of it: under 1 it eases into and out of that pose. The dots within `slide.size` frames scale, by the falloff
 * with proportional editing, else all alike; never past a key or each other. Returns how many stretches changed. */
int motion_path_scale_spacing(Document &doc, const MotionPathTarget &target, int frame, float factor,
                              const PathSlide &slide);
/* --- The Edit brush's selection: keys (the timeline's selection, on the channel that moves the dot) and dots. */

/* The frames of the selected keys on the channel that moves each target's dot (its position, or a tip's bone
 * angle), setup frame left out. */
std::vector<std::pair<MotionPathTarget, int>> motion_path_selected_keys(const Document &doc,
                                                                        const std::vector<MotionPathTarget> &targets);
/* Whether `frame` holds a key of the channel that moves the target's dot. */
bool motion_path_is_key(const Document &doc, const MotionPathTarget &target, int frame);
/* Selects (or deselects) the key on `frame` of the channel that moves the dot; whether it is selected now. */
bool motion_path_key_selected(const Document &doc, const MotionPathTarget &target, int frame);
void motion_path_key_select(Document &doc, const MotionPathTarget &target, int frame, bool select);
/* G R S on the selected keys: `kind` 'G' moves them by `offset` (document space), 'R' turns them by `angle` and 'S'
 * scales them by `factor` about their centre, their handles turning and scaling with them; one key on its own stays,
 * its handles turning or scaling about it. Each lands on its own frame. Returns how many keys changed. */
int motion_path_keys_transform(Document &doc, const std::vector<std::pair<MotionPathTarget, int>> &keys, char kind,
                               float2 offset, float angle, float factor);
/* The selected keys' path handles made `mode`: Auto as before they were dragged, Aligned (the two in line, each
 * keeping its length) or Broken (free of each other, as they are). Returns how many keys changed. */
int motion_path_handles_type(Document &doc, const std::vector<MotionPathTarget> &targets, PathHandles mode);
/* A stretch between keys of the channel that moves a target's dot, as the timeline's timing chart shows it: from
 * `start` (its first key, or the end of its hold) to `end` (the key ending it), how far along its path (0-1 by
 * length) it has got on each of its frames. */
struct MotionPathStretch {
  int start = 0, end = 0;
  std::vector<float> progress;
};
/* Each stretch that moves (not stepped, posed or cycling). Progress can go over 1 (past its key: overshoot) or under 0
 * (backing up first). */
std::vector<MotionPathStretch> motion_path_stretches(const Document &doc, const MotionPathTarget &target);
/* The frames between keys where the dot is past the key it arrives at or back before its stretch's start. */
std::vector<int> motion_path_overshoot_frames(const Document &doc, const MotionPathTarget &target);
/* The dot on `frame` (between keys) slid to `progress` (0-1 of the way along its stretch's path; over 1 past the key,
 * as far as 2, under 0 back before the start, as far as -1), as dragging a tick in the timing chart does; the dots
 * around it by proportional editing, as motion_path_slide, but free to pass each other and the keys. */
bool motion_path_slide_progress(Document &doc, const MotionPathTarget &target, int frame, float progress,
                                const PathSlide &slide);
/* The timing shapes the Path Timing panel's buttons give a stretch, by how far along it each frame has got (t the
 * share of its time): Even t, Ease In t^2, Ease Out 1-(1-t)^2, Ease In-Out 3t^2-2t^3. */
enum class PathShape { Even, EaseIn, EaseOut, EaseInOut };
bool path_shape_from_identifier(const std::string &id, PathShape &r_shape);
/* The stretch `frame` is strictly inside made Custom with that shape, the keys staying on their frames. */
bool motion_path_stretch_shape(Document &doc, const MotionPathTarget &target, int frame, PathShape shape);
/* The dot on `frame` made to go `speed` (as motion_path_dot_speed gives it: 1 even, under 0 going back), the rest of
 * its stretch taking up the time it gains or loses, each frame by its share; false off a stretch, or when the rest
 * can't (the most it can go is less than the stretch's frames). */
bool motion_path_dot_speed_set(Document &doc, const MotionPathTarget &target, int frame, float speed);
/* The stretch the Path Timing panel shows: of `want` (the last thing a dot or key was picked on) when it is one of
 * `targets`, else the first; the one the frame shown is in (or ends), except the one ending on `want_end` while the
 * frame shown is still `picked_on` (where it was picked, or stepped to with the arrows); else the first. False when
 * nothing traced moves. */
bool motion_path_timing_stretch(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                const MotionPathTarget &want, int want_end, int picked_on, MotionPathTarget &r_target,
                                MotionPathStretch &r_stretch);
/* A key on `frame` of the channel that moves the target's dot, where it is now (the path unchanged); false if there
 * is one already. */
bool motion_path_key_add(Document &doc, const MotionPathTarget &target, int frame);
/* Deletes the selected keys of the channels that move the targets' dots (not the setup frame's). */
int motion_path_keys_delete(Document &doc, const std::vector<MotionPathTarget> &targets);

/* The stretch `frame` is in goes along its path at an even speed (Custom, every speed 1). */
bool motion_path_even_spacing(Document &doc, const MotionPathTarget &target, int frame);
/* How fast the dot on `frame` goes, as a share of its stretch's even speed (1 between keys moving evenly); 0 off any
 * stretch that moves. */
float motion_path_dot_speed(const Document &doc, const MotionPathTarget &target, int frame);

/* Editing a dot on another frame than the one shown (clicking a dot doesn't go to its frame): while it lives the
 * document is on `frame`, then back on the frame it showed, evaluated there. */
struct MotionPathOnFrame {
  Document &doc;
  int shown;
  MotionPathOnFrame(Document &doc, int frame);
  ~MotionPathOnFrame();
  MotionPathOnFrame(const MotionPathOnFrame &) = delete;
  MotionPathOnFrame &operator=(const MotionPathOnFrame &) = delete;
};
/* Smooth Arc: each of the target's keys strictly between `first` and `last` moves onto a smooth curve through the
 * keys either side of it (up to two each side). Returns how many keys moved. */
int motion_path_smooth_arc(Document &doc, const MotionPathTarget &target, int first, int last);

/* --- Path handles on position keys: a layer's origin, a bone's head (not a connected one), a point. A bone's tip goes
 * where its angle turns it, so its path has none. */

/* Whether `target` has position keys with path handles. */
bool motion_path_has_handles(const Document &doc, const MotionPathTarget &target);
/* Where `target`'s position key on `frame` is and where its handles end, in document space on that frame (a handle
 * shown a third of the way along its stretch, as a Bezier's). False with no position key there. */
bool motion_path_handles(const Document &doc, const MotionPathTarget &target, int frame, float2 &r_key, float2 &r_in,
                         float2 &r_out, PathHandles *r_mode = nullptr);
/* Which of the handles of `p`'s key on `frame` the tool shows: each shapes the stretch on its side, so it shows only
 * where some of that stretch is drawn (none past the path's ends, as when it ends on the frame shown). */
void motion_path_handle_sides(const MotionPath &p, int frame, bool &r_in, bool &r_out);
/* Moves the handle (`out`, else in) of `target`'s position key on `frame` so it ends at `at` (document space on that
 * frame); `broken` (Alt) moves it alone and breaks the pair. Only the keys change: call animation_evaluate after. */
bool motion_path_handle_set(Document &doc, const MotionPathTarget &target, int frame, bool out, float2 at,
                            bool broken);
/* Puts `target`'s selected position keys back to Auto handles. Returns how many changed. */
int motion_path_handles_auto(Document &doc, const MotionPathTarget &target);

/* --- Path brushes (anim/path_brush.hh), on the paths of `targets` over frames first..last, in document units. Every
 * brush keys what it changes whatever auto-key says. */

struct PathDab {
  float2 center;
  float radius = 0.1f;
  float strength = 0.5f;
  /* Paint Speed's speed (1: the stretch's even speed). */
  float speed = 2.0f;
  /* Ctrl: the opposite (Paint Speed 1 / speed, Faster as Slower and Slower as Faster, Blend Speed sharpening). */
  bool invert = false;
};
/* 1 at the centre, easing to 0 at `radius`. */
float path_brush_falloff(float distance, float radius);

/* Grab: the targets' own keys whose dots are inside the brush when it is pressed, where they were, and how much each
 * follows the mouse. */
struct PathGrabKey {
  MotionPathTarget target;
  int frame = 0;
  float2 start;
  float weight = 0.0f;
};
std::vector<PathGrabKey> motion_path_grab_keys(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                               int first, int last, float2 center, float radius);
/* Moves each grabbed key from where it was by `offset` times its weight and `strength`. Returns how many moved. */
int motion_path_grab(Document &doc, const std::vector<PathGrabKey> &keys, float2 offset, float strength);
/* Smooth: each of the targets' own keys under the dab toward the curve through the keys either side (Smooth Arc's),
 * and its dragged handles toward Auto. Returns how many keys changed. */
int motion_path_smooth_dab(Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                           const PathDab &dab);
/* Paint Speed, Faster, Slower or Blend Speed: the speeds of the frames under the dab, on the channel that moves each
 * target's dot, its stretches becoming Custom (each still arriving on its key's frame). Returns how many stretches
 * changed. */
int motion_path_speed_dab(Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                          PathBrush brush, const PathDab &dab);

}  // namespace oanim
