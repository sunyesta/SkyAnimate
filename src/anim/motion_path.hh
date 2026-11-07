#pragma once

/* Motion paths (Document::motion_paths): where things go over the frames around the shown one, a dot per frame,
 * as Blender's motion paths and Maya's motion trails. A bone traces its tip (or its head), so turning draws an arc
 * and moving a line; a point traces itself, through its bones; a layer with nothing picked in it traces its
 * origin. The range runs from `path_before` keys back to `path_after` keys ahead of the shown frame, by the keys
 * of what is traced and of everything moving it. Frames are worked out on a copy of the document from their keys
 * alone, as onion skins are; deforms are left out (bones and transforms only), so paths stay quick. */

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "anim/animation.hh"
#include "anim/path_brush.hh"
#include "anim/sculpt.hh"
#include "core/math.hh"
#include "data/document.hh"
#include "data/vector.hh"

namespace oanim {

struct Layer;

/* What the paths trace: for each of `editing` (the layers being edited), its selected bones or points, or with
 * none of those its origin; and every pinned target. */
std::vector<MotionPathTarget> motion_path_targets(const Document &doc, const std::vector<Layer *> &editing,
                                                  bool edit_layer = false);
/* What the timing chart and Path Timing graph show: the paths' targets, then the drawing (MotionPathTarget::drawing)
 * of each Key Whole Drawing layer among the active, picked and edited ones whose drawing is animated. A Key Whole
 * Drawing layer's points trace no path of their own. */
std::vector<MotionPathTarget> motion_path_timing_targets(const Document &doc, const std::vector<Layer *> &editing,
                                                         bool edit_layer = false);
/* The kept style of `target` (pin, colour, end), or null. */
const MotionPathTarget *motion_path_style(const Document &doc, const MotionPathTarget &target);
MotionPathTarget &motion_path_style_ensure(Document &doc, const MotionPathTarget &target);
/* The frames the paths of `targets` cover. False with nothing to trace. */
bool motion_path_range(const Document &doc, const std::vector<MotionPathTarget> &targets, int &r_first,
                       int &r_last);
/* Whether `frame` is outside the shot, before the scene's Start (Start past 0) or after its End: a path reaching back
 * or on to the keys the shot opens and ends between draws that part faded. */
bool motion_path_frame_outside(const Document &doc, int frame);
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
  /* Own Motion (Document::path_own_motion): `points` hold only the target's own motion. (Where it really goes isn't
   * shown alongside: a second path confused more than it told; Own Motion off shows it.) */
  bool own_motion = false;
  /* The path's own shape between the dots (document space, as `points`): shape[i] the points strictly between dot
   * i - 1 and dot i, where shape_known[i]. Known where the path comes from a position channel and its dots sit on its
   * stretch's curve; so the line drawn through it stays put while the dots slide along it (a timing change), and its
   * corners (a crease at a control point) stay sharp. */
  std::vector<std::vector<float2>> shape;
  std::vector<bool> shape_known;
  /* Drawn relative to something (not Own Motion): each frame's map from document space on that frame to where it is
   * drawn, which turns and scales as what it is relative to does. Empty when drawn as it is. */
  std::vector<Transform2D> spaces;
};
/* From document space on `frame` to where path `p` draws it, `at` (a place on that frame, in document space: the
 * target's) landing on its dot: the key's handles and control points as drawn, and back with inverted(). */
Transform2D motion_path_drawn(const MotionPath &p, int frame, float2 at);
/* Whether `target`'s path shows only its own motion now: Own Motion is on and the Motion Path tool is (picked or not). */
bool motion_path_own_motion(const Document &doc, const MotionPathTarget &target);
/* Whether the Motion Path tool is on (the window's): Own Motion is for editing with it; outside it paths are only
 * looked at, the real ones. */
void motion_path_tool_on(bool on);
/* The paths of `targets` over frames first..last. */
std::vector<MotionPath> motion_paths_make(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                          int first, int last);
/* A path's colour: its own, or one of a few told apart by the order it comes in. */
Color motion_path_color(const Document &doc, const MotionPathTarget &target, size_t index);
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
/* The keys behind the target's diamond on `frame` (a timing row's): its own channel's key there (a drawing's: every
 * channel's), else the key that moves it there, its layer's or a group's above (turn, move or size). With `change`,
 * each one's settings are changed by it; `r_first` gets the first one's settings before. Returns how many. */
int motion_path_key_settings(Document &doc, const MotionPathTarget &target, int frame,
                             const std::function<void(KeySettings &)> &change, KeySettings *r_first = nullptr);
/* Deletes those keys. Returns how many. */
int motion_path_key_delete(Document &doc, const MotionPathTarget &target, int frame);
/* Selects (or deselects) the key on `frame` of the channel that moves the dot; whether it is selected now. */
bool motion_path_key_selected(const Document &doc, const MotionPathTarget &target, int frame);
/* The stretch of the target's keys (every channel its path's keys are on) that `frame` is in or ends: the keys before
 * and at its end, stepped or moving. False before its first key's stretch or past its last key. */
bool motion_path_stretch_at(const Document &doc, const MotionPathTarget &target, int frame, int &r_start, int &r_end);
/* A stretch picked with the Motion Path tool: of `target`, from its key on `start` to its key on `end`. */
struct MotionPathPickedStretch {
  MotionPathTarget target;
  int start = 0, end = 0;
  bool operator==(const MotionPathPickedStretch &o) const
  {
    return target.same(o.target) && start == o.start && end == o.end;
  }
};
/* The stretches picked (the selected stretch the key header, the timeline's rows and the Path Timing panel share):
 * each picked dot's; each picked path handle's (an in handle's the stretch its key ends, an out handle's the next);
 * and each selected key's of the channel moving the dot of one of `key_targets`, on the side of the frame shown (with
 * the frame on the key itself, the stretch arriving at it; a path's first and last keys have one side). In that order,
 * each once. */
std::vector<MotionPathPickedStretch> motion_path_picked_stretches(
    const Document &doc, const std::vector<MotionPathTarget> &key_targets,
    const std::vector<std::pair<MotionPathTarget, int>> &dots, const std::vector<MotionPathHandlePick> &handles);
/* How many different spans of frames (first and last frame) the picked stretches cover: with one, the stretch is
 * selected (teal); with more, picks are in different stretches. */
int motion_path_picked_spans(const std::vector<MotionPathPickedStretch> &picks);
/* The channels that move the target's dot (a tip's bone angle, else its position; a drawing's every channel): the
 * timeline rows its selected stretch is shaded on. */
std::set<const void *> motion_path_dot_channels(const Document &doc, const MotionPathTarget &target);
/* Whether the target's key on `frame` is a breakdown (drawn as a blue circle). */
bool motion_path_key_is_breakdown(const Document &doc, const MotionPathTarget &target, int frame);
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
  /* Breakdowns inside the stretch (a span from main key to main key is one stretch). */
  std::vector<int> breakdowns;
  /* On 2s or 3s (its start key's interval): `progress` is the timing played every `interval` frames, a frame between
   * holding the pose of the one its step began on (played()). */
  int interval = 1;
  /* How far along the dot shows on frame `start + k`, as it plays. */
  float played(int k) const
  {
    return start + k >= end ? progress.back() : progress[size_t(k / interval * interval)];
  }
  /* Whether frame `start + k` holds the pose of an earlier frame (on 2s or 3s). */
  bool held(int k) const
  {
    return k % interval != 0 && k + start != end;
  }
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
/* Shift click in the timing chart or the Path Timing graph: the spacing around `frame` evened out, as Blender's Smooth
 * Vertices: each dot within `radius` frames moves `smoothing` (0-1, by the falloff) of the way to the midpoint of the
 * dots either side, `repeat` times, the keys staying (an overshoot is smoothed down too). Returns how many stretches
 * changed. */
int motion_path_timing_blend(Document &doc, const MotionPathTarget &target, int frame, float smoothing, int repeat,
                             float radius);
/* A Shift drag's stroke: a blend on each of `frames` in turn (a frame gone over again blends again). Returns how many
 * blends changed something. */
int motion_path_timing_blend_stroke(Document &doc, const MotionPathTarget &target, const std::vector<int> &frames,
                                    float smoothing, int repeat, float radius);
/* "15,16,17": a stroke's frames, as the operator takes them. */
std::string path_frames_string(const std::vector<int> &frames);
std::vector<int> path_frames_parse(const std::string &text);
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
/* A key on `frame` of `pos` (none there yet), where its dot is: between two keys the stretch it splits keeps its way
 * and its timing (the curve and its control points split there, its speeds shared out, the new key arriving as the
 * stretch did), and the keys either side keep their handles, so no dot moves anywhere; before the first key or after
 * the last it holds the value there. Every way of keying a position goes through it. */
bool key_add_in_place(Channel<float2> &pos, int frame);
bool motion_path_key_add(Document &doc, const MotionPathTarget &target, int frame);
/* The target's keys on `frame` (each channel its path covers: a bone's, a point's, a drawing's, a layer's turn, move
 * and size) made `type`; never frame 0. Returns how many. Alt click and Alt Shift on a path make breakdowns. */
int motion_path_key_type_set(Document &doc, const MotionPathTarget &target, int frame, KeyType type);
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
  /* The document as shown, for Own Motion (Document::path_own_motion). */
  std::unique_ptr<Document> held;
  const Document *outer = nullptr;
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
/* An Auto handle with no length (Smooth flattens a key where the path turns back in both x and y) still shows, short,
 * so it can be grabbed: the way the path runs from `p`'s key on `frame` on that side (to the first dot apart from it),
 * drawn position. False when the path has no dot apart from the key on that side. */
bool motion_path_handle_stub(const MotionPath &p, int frame, bool out, float2 &r_direction);
/* Moves the handle (`out`, else in) of `target`'s position key on `frame` so it ends at `at` (document space on that
 * frame); `broken` (Alt) moves it alone and breaks the pair. Only the keys change: call animation_evaluate after. */
bool motion_path_handle_set(Document &doc, const MotionPathTarget &target, int frame, bool out, float2 at,
                            bool broken);
/* G R S on picked handles: 'G' moves each tip by `offset` (document space), 'R' turns it by `angle` and 'S' scales it
 * by `factor` about its own key; its partner follows as the handle type says (Auto becomes Aligned). Returns how many
 * changed. */
int motion_path_handles_transform(Document &doc, const std::vector<MotionPathHandlePick> &picks, char kind,
                                  float2 offset, float angle, float factor);
/* The keys of `picks` made `mode` (as motion_path_handles_type). Returns how many keys changed. */
int motion_path_handles_type_at(Document &doc, const std::vector<MotionPathHandlePick> &picks, PathHandles mode);
/* --- Control points (Key::path_controls): shape a stretch's path, no frame or timing. */

struct MotionPathControl {
  /* The key ending its stretch, and which of its control points. */
  int end = 0;
  int index = 0;
  /* The frame whose dot is nearest it, and where it is and where the path is on that frame, in document space. */
  int near_frame = 0;
  float2 at, curve;
  /* Its handles' tips (document space, as `at`), and whether they are its own yet and in line. */
  float2 in_tip, out_tip;
  bool set = false, aligned = true;
};
/* How many places of a target its paths' handles and control points were worked out at, each on a copy of the document
 * (a whole frame of it), since motion_path_places_clear; and the most threads any batch of them was shared over. For
 * tests of what editing a path costs. */
struct MotionPathPlaces {
  uint64_t places = 0;
  int threads = 0;
};
MotionPathPlaces motion_path_places();
void motion_path_places_clear();
/* Every control point of `target`'s path (position keys only). With `tips`: only those it says yes to have their
 * handles' tips worked out (each takes two more places on copies of the document); the others' tips are their point. */
std::vector<MotionPathControl> motion_path_controls(
    const Document &doc, const MotionPathTarget &target,
    const std::function<bool(const MotionPathControl &)> &tips = {});
/* A control point where `target`'s path is on `frame`, between two of its position keys (not on one); in order along
 * the stretch. Sets which it is. False with no stretch there. */
bool motion_path_control_add(Document &doc, const MotionPathTarget &target, int frame, int *r_end = nullptr,
                             int *r_index = nullptr);
/* Moves a control point to `at` (document space, as seen on frame `near`). */
bool motion_path_control_set(Document &doc, const MotionPathTarget &target, int end, int index, int near_frame, float2 at);
/* Deletes the control points among `picks` (the points themselves, not their handles). Returns how many. */
int motion_path_controls_delete(Document &doc, const std::vector<MotionPathHandlePick> &picks);
/* Moves a control point's handle (`out` the one toward the stretch's key) so its tip is at `at` (document space, as
 * seen on frame `near`), as a vector point's handle is dragged: its handles are its own from then on; Aligned, the
 * other turns to stay in line, keeping its length, unless `alone` (Alt), which moves just this one and breaks the
 * line. */
bool motion_path_control_handle_set(Document &doc, const MotionPathTarget &target, int end, int index, bool out,
                                    int near_frame, float2 at, bool alone);
/* Align or Unalign the control points among `picks` (or those whose handles are), as a vector point's handles: one
 * handle picked alone turns into line with the other; the point (or both its handles): both turn, halfway each.
 * Returns how many changed. */
int motion_path_controls_align(Document &doc, const std::vector<MotionPathHandlePick> &picks, bool align);
/* The control points whose handles are among `picks`: their handles worked out smooth again, from the points either
 * side, and aligned. Returns how many changed. */
int motion_path_control_handles_reset(Document &doc, const std::vector<MotionPathHandlePick> &picks);
/* Whether a pick (a path handle or a control point) is still there. */
bool motion_path_pick_valid(const Document &doc, const MotionPathHandlePick &pick);

/* Puts `target`'s selected position keys back to Auto handles. Returns how many changed. */
int motion_path_handles_auto(Document &doc, const MotionPathTarget &target);

/* --- Path brushes (anim/path_brush.hh), on the paths of `targets` over frames first..last, in document units. Every
 * brush keys what it changes whatever auto-key says. */

struct PathDab {
  float2 center;
  float radius = 0.1f;
  float strength = 0.5f;
  /* Ctrl: the opposite (Blend Speed sharpening). */
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
/* Slide's press: the dot between keys nearest `at` within `radius` (document units, as drawn) on the paths of
 * `targets` over first..last, how many frames either side of it have dots inside the circle (at least 1), and where it
 * is drawn less where it is. False with none there. */
bool motion_path_slide_pick(const Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                            float2 at, float radius, MotionPathTarget &r_target, int &r_frame, int &r_reach,
                            float2 &r_offset);
int motion_path_speed_dab(Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                          PathBrush brush, const PathDab &dab);

/* Sculpting motion paths as the Sculpt tool sculpts lines (Grab, Smooth, Surface Smooth, exactly theirs): when a
 * stroke starts, each path traced with keys of its own (a layer's origin, a point, a bone's head or a bone moving its
 * tip) is made into a line in a drawing of its own, its keys and control points the line's points and the handles the
 * path runs along its handles, and the Sculpt tool's stroke is begun on it (sampled densely, each point anchored to its
 * sample). Each dab sculpts that line, then the path is given back from it: each key goes where its point went, its
 * handles as the fitted line has them (an Auto key becoming Aligned), each control point where it went. No key is
 * added or removed, the setup key (frame 0) and keys off the paths shown stay, and each stretch reshaped keeps its
 * timing: it becomes Custom with the speeds it had by length along it, so its dots stay at the same share of the way. */
struct PathSculptState;
struct PathSculptStroke {
  std::shared_ptr<PathSculptState> state;
  bool empty() const;
};
/* Whether `target`'s path can be sculpted so (a channel of position keys, two at least). */
bool motion_path_sculptable(const Document &doc, const MotionPathTarget &target);
/* `dab` is the Sculpt tool's, in document space; its brush Grab, Smooth or Surface Smooth. first..last: the frames
 * the paths show. */
PathSculptStroke motion_path_sculpt_begin(const Document &doc, const std::vector<MotionPathTarget> &targets, int first,
                                          int last, const SculptDab &dab);
/* Returns how many paths changed. */
int motion_path_sculpt_dab(Document &doc, PathSculptStroke &stroke, const SculptDab &dab);
/* A shape brush's dab (Grab, Smooth, Surface Smooth) from its settings, in document space. */
SculptDab motion_path_sculpt_dab_make(PathBrush brush, const PathBrushSettings &settings, float2 center, float radius);

}  // namespace oanim
