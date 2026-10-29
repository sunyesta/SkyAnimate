#pragma once

/* Draws a document's layers into an offscreen image, with Moho's masking.
 *
 * Edges are anti-aliased by multisampling: each pixel holds several samples, each either inside
 * a shape or not, averaged only at the very end. Masks live in the stencil buffer, per sample,
 * so a masked layer ends exactly where its mask's edge falls and two edges that meet leave no
 * seam (no "matte lines"). Each level of nested masking group uses one stencil bit.
 *
 * Colours are premultiplied throughout. A group with less than full opacity is drawn into its
 * own image and faded as a whole. */

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "anim/layer_pick.hh"
#include "core/math.hh"
#include "data/layer.hh"
#include "gpu/brush_stroke.hh"
#include "gpu/gpu_device.hh"
#include "gpu/tessellate.hh"

namespace oanim {

struct Document;
struct Layer;
struct Vertex;
struct BrushDabs;
struct BrushImage;
struct DrawStep;

/* Local View's layers (WindowManager::local_view) with everything inside them and every group holding
 * them: the layers it shows. Empty when none of them is still there (everything shows). */
std::set<uint64_t> local_view_layers(const Document &doc, const std::vector<uint64_t> &ids);

class SceneRenderer {
 public:
  struct MaskRow;
  ~SceneRenderer();

  /* Renders into a texture of `width` x `height` pixels. `document_to_pixels` maps document
   * space to pixels with the origin at the bottom left. `quality` = pieces drawn per curve
   * segment; `samples` = anti-aliasing samples per pixel (rounded down to 1, 2, 4, 8 or 16
   * and to what the GPU supports). `background`, if given, makes this a final render: it fills
   * the image first and layers hidden from renders are left out. Otherwise the image starts
   * transparent and every visible layer is drawn (the viewport). Returns the OpenGL texture, which stays valid
   * until the next call, or 0 if OpenGL isn't available. */
  unsigned int render(const Document &doc, int width, int height, const Transform2D &document_to_pixels,
                      int quality, int samples, const Color *background = nullptr);

  /* The last render's pixels, top row first, RGBA, 8 bits each (premultiplied). */
  bool read_pixels(std::vector<uint8_t> &r_rgba, int *r_width, int *r_height);

  /* Whether the next viewport renders draw guides, the layers not rendered (off: overlays off, so the
   * viewport shows what renders). */
  void set_show_guides(bool show) { show_guides_ = show; }
  /* Media View: media layers at full opacity, shown even as guides. */
  void set_media_view(bool on) { media_view_ = on; }
  /* Whether `layer` draws at full opacity whatever its own (a media layer in Media View). */
  bool full_opacity(const Layer &layer) const;
  /* Wait for media frames to be decoded, as a final render does (a render to a file of a reference). */
  void set_wait_for_media(bool wait) { wait_for_media_ = wait; }
  /* The viewport's renders, one a frame: a drawing seen at a new zoom may be drawn as made for the
   * nearest zoom there is while the render has used its time, and is made over the next frames. */
  void set_interactive(bool interactive) { interactive_ = interactive; }
  /* A zoom moving with each step drawn (Zoom Preview off): drawings are drawn as made for the nearest zoom they
   * were made for, nothing made, so each step is quick; the render after it rests makes them all, at once. */
  void set_zooming(bool on) { zooming_ = on; }
  /* Whether the last render drew everything as it is (not some drawings from another zoom, to be made over the next
   * frames): only then can it stand for the next frame when nothing has changed. */
  bool complete() const { return complete_; }
  /* Local View: draw only these layers, what's inside them and the groups holding them (empty: all). */
  void set_local_view(const Document &doc, const std::vector<uint64_t> &ids);

  /* The viewport's renders draw a margin around what was asked for (an eighth of its longer side), so a pan that
   * keeps within it shows the same image moved (shown_uv) with nothing drawn. */
  void set_overscan(bool on) { overscan_ = on; }
  /* Whether the last render, moved by whole pixels, shows `document_to_pixels` (a pan within its margin, the same
   * size and zoom): then the part shown becomes that (shown_uv) and the image can be shown as it is. */
  bool pan_to(int width, int height, const Transform2D &document_to_pixels);
  /* The part of the last render's image that shows what was asked for: u0, v0, u1, v1 (v up). */
  void shown_uv(float r_uv[4]) const
  {
    for (int i = 0; i < 4; i++) {
      r_uv[i] = uv_[i];
    }
  }
  /* What the last render's image covers: its pixels to document space, and its size. */
  Transform2D image_to_document() const { return document_to_pixels_.inverted(); }
  int image_width() const { return width_; }
  int image_height() const { return height_; }
  /* Off: every render draws every layer (Viewport Shading > Layer Caching). */
  void set_layer_runs(bool on) { layer_runs_ = on; }

  /* The anti-aliasing samples the last render used. */
  int samples() const { return samples_; }
  /* A mask card's held View: the next viewport renders show `layer`'s card `upto` (its mask alone)
   * over the drawing at 90%, white where it shows the layer's fills (`lines`: its lines) and black
   * where it hides them. 0: off. */
  void set_mask_preview(uint64_t layer, bool lines, int upto)
  {
    mask_preview_layer_ = layer;
    mask_preview_lines_ = lines;
    mask_preview_upto_ = upto;
  }

 private:
  /* Drawn into: multisampled colour and depth-stencil, and the image the samples resolve into. The colour is a
   * multisample texture (with the standard sample places) when there are samples, so layer runs' images line up with
   * it sample for sample. */
  struct Target {
    gpu::Target target;
    bool in_use = false;
    /* The stencil bits stencil-then-cover fills count in (draw_stencil_fill) may not be clear everywhere: masks
     * were worked out with them since the target was cleared (each fill then clears them under its cover first). */
    mutable bool fan_dirty = true;
    /* GPU Geometry: the mask levels its depth holds (apply_masks), outermost first: a sample's depth is how many of
     * them, in order, it is inside (or more, from levels since let go of, which still hold all of these). */
    mutable std::vector<std::vector<std::vector<MaskRow>>> depth_levels;
  };
  /* What the GPU is asked to do, without naming its API (gpu_device.hh): everything below draws through it. */
  std::unique_ptr<gpu::Device> device_;

  bool init();
  Target *acquire_target();
  void release_target(Target *target);
  void free_targets();
  void bind_target(const Target *target);
  void resolve(const Target *target);
  void clear(const Color &color);

  /* Layer runs (the viewport): the layers drawn, bottom first, each with a signature of all it draws from (its
   * drawing as drawn, where, how opaque, its masks and the layers they take, theirs, and so on); those whose
   * signature changed since the last render are drawn as they are ("live"), and each stretch of others between
   * them comes from an image of that stretch kept from when it was drawn, laid over what is below sample for
   * sample. Editing one layer draws it and what depends on it, not the whole document. */
  struct LayerRun {
    size_t begin = 0, end = 0;
    std::vector<uint64_t> signatures;
    Target *image = nullptr;
  };
  std::vector<LayerRun> runs_;
  /* The steps the runs were made for (their layers) and the view they were drawn in. */
  std::vector<uint64_t> run_layers_;
  uint64_t run_view_ = 0;
  /* The last render's steps and signatures, to see which changed. */
  std::vector<uint64_t> last_layers_, last_signatures_;
  uint64_t last_view_ = 0;
  gpu::DepthStencil run_depth_stencil_;
  /* This render's drawings as drawn: each layer's tessellation key with where it is drawn. */
  std::unordered_map<uint64_t, uint64_t> drawn_signature_;
  std::unordered_map<uint64_t, uint64_t> signature_memo_;
  std::vector<uint64_t> signature_busy_;
  uint64_t layer_signature(const Document &doc, const Layer &layer);
  uint64_t masks_signature(const Document &doc, const Layer &layer);
  uint64_t step_signature(const Document &doc, const DrawStep &step);
  void draw_step(const Document &doc, const DrawStep &step, Target *target);
  void draw_layers_in_runs(const Document &doc, const std::vector<DrawStep> &steps, const std::vector<char> &clean);
  /* Which steps are clean this render (pushed, as Blender's depsgraph tags and Chromium's invalidation flags rather
   * than worked out): their signature is last render's without looking, and they aren't prepared. Clean: in a kept
   * run, a plain drawing (no masks on it or its groups, no modifiers but the bones', not a reference) none of whose
   * groups or own self has a new revision, while only a few layers have one. Every so often none is, so anything
   * changed without a new revision is caught. */
  std::vector<char> clean_steps(const Document &doc, const std::vector<DrawStep> &steps, uint64_t view);
  uint64_t runs_view() const;
  std::unordered_map<uint64_t, uint64_t> seen_revisions_;
  int clean_renders_ = 0;
  /* The steps that changed in the last render that drew everything: runs are made only once the same ones change
   * again (as Flutter caches a picture only once it has been drawn unchanged for some frames), so a one-off change
   * doesn't pay for images. */
  std::vector<size_t> last_live_;
  std::set<uint64_t> skip_prepare_;
  Target *make_run_image();
  void free_run_image(Target *target);
  void free_runs();
  /* Lays a run's image over the bound target, each sample over its own. */
  void composite_run(const Target *run);
  bool overscan_ = false;
  bool layer_runs_ = true;
  float uv_[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  /* The size and mapping asked for by the last render (before its margin). */
  int asked_width_ = 0, asked_height_ = 0;
  int margin_ = 0;

  /* Draws a list of layers (top first) into the bound target, bottom first. */
  void draw_layers(const Document &doc, const std::vector<Layer> &layers, const Transform2D &parent,
                   Target *target, float opacity);
  void draw_layer(const Document &doc, const Layer &layer, const Transform2D &transform, Target *target,
                  float opacity);
  /* The mask preview (set_mask_preview), over the drawing at 90%: black, and white where the mask
   * shows the layer. */
  void draw_mask_preview(const Document &doc, const Layer &layer);
  /* Sets the stencil to the masks of `masked` (the layers whose masks apply to what is drawn next)
   * and returns the stencil bits a sample needs (and in applied_depth_, the depth levels). */
  unsigned apply_masks(const Document &doc, const std::vector<const Layer *> &masked);
  /* GPU Geometry: the cards that cut fills and lines alike (most) go into the depth buffer as levels, each layer's
   * one level, the rest into the stencil as with Classic. A level is worked out in stencil bit `bit` (its cards, as
   * Classic works one out), then kept where the levels before it hold, written as level `level` into the depth. */
  void push_depth_level(unsigned level, unsigned bit);
  /* Lets go of the depth levels past `level`: their samples back to it. */
  void pop_depth_levels(unsigned level);
  /* Depth levels a colour sample must be inside (all of the bound target's first this many). */
  unsigned depth_required_ = 0;
  unsigned applied_depth_ = 0;
  /* One card's steps, in order, painted on its start into stencil bit `result` (using `scratch` for
   * each step's shape). `nested`: inside a clip base's masks, whose own Clip to Below isn't looked at.
   * `free`: stencil bits nothing holds yet, which a Clip to Below step works out its base's masks in. */
  void evaluate_mask_rows(const Document &doc, const std::vector<MaskRow> &rows, unsigned result, unsigned scratch,
                          bool nested, unsigned free = 0);
  /* Cards that must all show, into `result`: the first there, each other one in the spare bit and
   * kept only where both hold (`free` must hold the spare for more than one). */
  void evaluate_mask_cards(const Document &doc, const std::vector<std::vector<MaskRow>> &cards, unsigned result,
                           unsigned scratch, unsigned free);
  /* A row's layer (grown or shrunk as the row says, in mask_pass_) into stencil bit `bit`: its
   * shapes as drawn, before any masks of its own. */
  void draw_mask_source(const Document &doc, const Layer &source, unsigned bit, bool nested);
  /* Clip to Below's base into stencil bit `bit` as it shows: only its shown layers, each where its
   * own masks (and its groups' inside the base) show it, worked out in `free` bits (in the nested
   * ones, one level deep, without two of them). `nested`: without their masks. */
  void draw_clip_base(const Document &doc, const Layer &layer, unsigned bit, bool nested, unsigned free,
                      std::vector<const Layer *> &chain);
  /* Wherever the stencil's `test` bits equal `value`, sets the `write` bits to `ref`'s. */
  void stencil_where(unsigned test, unsigned value, unsigned write, unsigned ref);
  /* Writes a layer's coverage (and its children's) into one stencil bit, set or cleared. */
  void draw_coverage(const Document &doc, const Layer &layer, const Transform2D &parent, unsigned bit, bool set);
  /* Sets or clears one stencil bit everywhere. */
  void stencil_fill(unsigned bit, bool set);
  /* Stencil test for colour drawing: every bit in required_stencil_ must be set (for a layer's
   * lines, all but line_free_stencil_'s; for its fills, all but fill_free_stencil_'s; a group faded
   * as a whole goes by its fills). */
  enum class Paint { Fill, Line };
  void use_color_stencil(Paint paint = Paint::Fill);
  /* What a layer's shapes, stamps, pictures and faded groups draw with (their program aside): colour under the masks
   * use_color_stencil set, or a mask's coverage into the stencil (draw_coverage). Each draw is given it whole. */
  gpu::DrawState paint_;
  /* Draws a vector layer's shapes into the bound target. */
  void draw_drawing(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity);
  void draw_vertices(const std::vector<Vertex> &vertices, const Transform2D &transform);
  /* Draws brush dabs. `own_colors`: a colour brush shown in its own colours. Merged alpha
   * draws them into an image of their own first, keeping the strongest dab where they overlap. */
  /* `meshes` (optional): a kept step's stamps, one mesh per image, so they stay on the GPU (uploaded the first
   * time). */
  /* How opaque a dab's pixel must be to count in a mask (see PaintStep::coverage_alpha). */
  float coverage_alpha_ = 0.5f;
  void draw_dabs(const BrushDabs &dabs, const std::vector<BrushImage> &images, bool own_colors, bool merged,
                 const Transform2D &transform, float opacity, std::vector<gpu::Mesh> *meshes = nullptr);
  gpu::Texture brush_texture(const BrushImage &image);
  /* Layer space -> pixels -> clip space. */
  Transform2D to_clip(const Transform2D &transform) const;
  /* Blends `source`'s resolved image onto the bound target. */
  void composite(const Target *source, float opacity);

  /* A layer this render draws: visible, and shown in renders when this is a final render (a Media
   * layer's Guide never is). */
  bool shown(const Layer &layer) const;
  bool layer_shown_through_parents(const Document &doc, const Layer &layer) const;
  float layer_opacity_through_parents(const Document &doc, const Layer &layer) const;
  /* A Media layer's picture on this frame (the nearest decoded one in the viewport until the exact
   * one comes; a final render waits for it). */
  void draw_media(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity);
  struct MediaTexture {
    gpu::Texture texture;
    uint64_t serial = 0;
  };
  /* One texture per Media layer, refilled when its frame changes. */
  std::map<uint64_t, MediaTexture> media_textures_;

  bool final_render_ = false;
  /* The viewport shows guides (layers not rendered): its overlays are on, Guides among them. */
  bool show_guides_ = true;
  bool media_view_ = false;
  bool wait_for_media_ = false;
  /* Local View's layers with everything inside them and every group holding them; empty: all. */
  std::set<uint64_t> local_view_;
  /* Drawing a mask's coverage into the stencil (no colour): brush dabs count where they're
   * at least half covered. */
  bool coverage_pass_ = false;
  const Target *bound_ = nullptr;
  /* One texture per brush image, by BrushImage::id. */
  std::map<uint64_t, gpu::Texture> brush_textures_;
  /* Pieces per curve segment for the render in progress. */
  int quality_ = 12;
  /* Within one render: each layer's evaluated drawing (bones, modifiers), worked out once though
   * masks draw a layer again for each of their passes. */
  struct Evaluated {
    VectorDrawing drawing;
    /* Where the bones carry it as one piece (see layer_evaluated_drawing). */
    Transform2D rigid;
  };
  std::unordered_map<uint64_t, Evaluated> evaluated_;
  /* Across renders: a drawing as drawn, by a hash of everything that makes it (its points, curves
   * and shapes, their brushes, the setup shape brushed lines lay their stamps on, the pixel size,
   * quality and mask pass; see draw_drawing), so a drawing that didn't change isn't tessellated or
   * stamped again. In layer space, so moving the layer keeps it. Its steps, in order: batches of
   * triangles, and brushed lines' stamps. */
  struct PaintStep {
    std::vector<Vertex> vertices;
    bool is_dabs = false;
    BrushDabs dabs;
    std::string brush;
    BrushSettings settings;
    bool own_colors = false;
    bool merged_alpha = false;
    /* In a mask, how opaque a dab's pixel must be to count: half, or (a line cleared for Inside the
     * Stroke) any that shows, so no soft edge of it is left. */
    float coverage_alpha = 0.5f;
    /* Lines (solid or brushed), drawn without the masks whose rows end them instead (End Lines at
     * Edge with Ends: Brush; see line_free_stencil_). */
    bool is_line = false;
    /* A stencil-then-cover fill (tessellate_stencil_fan): its triangles count its winding into the stencil, then its
     * last 6 vertices cover it, drawn where that isn't 0 (`even_odd`: where it is odd). */
    bool stencil_fan = false;
    bool even_odd = false;
    /* What it covers (layer space), worked out when first needed (note_bounds). */
    bool bounded = false;
    float2 lo, hi;
    /* On the GPU once drawn (uploaded once, kept with it): the triangles, or the stamps per image. */
    gpu::Mesh mesh;
    std::vector<gpu::Mesh> dab_meshes;
  };
  struct Tessellated {
    std::vector<PaintStep> steps;
    uint64_t used = 0;
  };
  /* `key`: the drawing's tessellation key, so its plain colour steps can be batched (0: drawn at once). */
  void draw_steps(std::vector<PaintStep> &steps, const Transform2D &transform, float opacity, uint64_t key = 0);
  /* Batching: plain colour steps (not a mask's coverage, not brushed stamps) wait in batch_ and go out
   * together, in their order, before anything else draws or changes what they draw with (every other
   * drawing call, a mask, a target, a clear, the end of the render flushes them first). Runs of up to 32
   * steps under the same stencil test are drawn as one: their triangles copied once into a chunk's buffer
   * (kept by which steps it holds, so only a changed drawing remakes its chunk), each vertex knowing its
   * step's slot, and the steps' places and opacities given per draw, so moving layers remakes nothing. A big
   * rig's hundreds of drawings go out in a few dozen draws instead of thousands. */
  struct Batched {
    const PaintStep *step = nullptr;
    uint64_t id = 0;
    Transform2D to_clip;
    float opacity = 1.0f;
    unsigned required = 0;
    unsigned depth = 0;
  };
  struct Chunk {
    gpu::Mesh mesh;
    int vertices = 0;
    uint64_t used = 0;
  };
  std::vector<Batched> batch_;
  /* A plain drawing's geometry hash, kept while its layer's revision stays (see Revision). */
  struct KeptHash {
    uint64_t revision = 0;
    uint64_t hash = 0;
  };
  std::unordered_map<uint64_t, KeptHash> geometry_hashes_;
  uint64_t geometry_hash(const Layer &layer, const VectorDrawing &drawing, bool modified);
  std::unordered_map<uint64_t, Chunk> chunks_;
  void flush_batch();
  /* GPU Geometry: a drawing layer less than fully opaque drawn as a whole, as a group is: at full strength into an
   * image of its own, then laid over `target` sample for sample (where the layer drew), faded, under its masks; so
   * its lines don't show its fills through them, nor its shapes each other. With a single sample, as a group. */
  bool fade_whole(const Layer &layer, float opacity) const;
  void draw_faded_drawing(const Document &doc, const Layer &layer, const Transform2D &transform, Target *target,
                          float opacity);
  /* While a faded layer draws into its image: what its steps cover (clip space), to lay only that over. */
  bool track_bounds_ = false;
  float2 bounds_lo_, bounds_hi_;
  void note_bounds(PaintStep &step, const Transform2D &transform);
  /* A stencil-then-cover fill (PaintStep::stencil_fan) under the masks `required` (and depth_required_), drawn at
   * once. */
  void draw_stencil_fill(PaintStep &step, const Transform2D &step_to_clip, float opacity, unsigned required);
  /* This render's document asks for GPU Geometry (ViewportShading::gpu_geometry). */
  bool gpu_geometry_ = false;
  /* A step of brushed stamps, drawn at once. */
  void draw_dabs_step(PaintStep &step, const Transform2D &transform, float opacity);
  void free_step(PaintStep &step);
  std::unordered_map<uint64_t, Tessellated> tessellated_;
  /* Each shape's steps as last made for a drawing (by everything but its geometry and with the zoom), and what they
   * were made from: the shape, the curves it runs along and their points. A drawing changed in a few shapes (a point
   * dragged) makes only those again, the rest taken as they were. */
  struct ShapeKept {
    Shape shape;
    std::vector<std::pair<int, Curve>> curves;
    std::vector<std::pair<int, MeshPoint>> points;
    std::vector<PaintStep> steps;
  };
  struct ShapesKept {
    std::vector<ShapeKept> shapes;
    uint64_t used = 0;
  };
  std::unordered_map<uint64_t, ShapesKept> shapes_kept_;
  /* What a drawing's steps are made from (make_steps). */
  struct MakeArgs {
    const Document *doc = nullptr;
    const Layer *layer = nullptr;
    const VectorDrawing *drawing = nullptr;
    Transform2D transform;
    float pixel = 1.0f;
    LineCutter cutter;
    ShapesKept *kept = nullptr;
  };
  /* A drawing's steps: its shapes' triangles and stamps (those of shapes as they were taken from `kept`). Safe on
   * several threads at once for different drawings in the colour pass, unless brushed or ended at masks' edges. */
  std::vector<PaintStep> make_steps(const MakeArgs &args);
  /* Before a render draws: every drawing it will draw whose steps aren't made yet (and that isn't brushed or ended at
   * masks' edges) is found, and all of them made at once on the CPU's cores; the render then finds them made. */
  struct Job {
    MakeArgs args;
    uint64_t key = 0, any_zoom = 0, anywhere = 0;
    bool movable = false;
    /* Could be drawn as made for another zoom instead, this render. */
    bool optional = false;
    float2 origin;
    std::vector<PaintStep> steps;
  };
  /* Drawings left to be made by later renders (prepare_drawings): drawn as made for another zoom in this one. */
  std::set<uint64_t> deferred_;
  bool preparing_ = false;
  std::vector<Job> jobs_;
  void prepare_drawings(const Document &doc);
  /* By everything but the zoom: the key of the last one made or drawn (see set_interactive). */
  std::unordered_map<uint64_t, uint64_t> tessellated_any_zoom_;
  /* By everything but where the drawing is (its points relative to its first): the key of one made with
   * the same shape and where its first point was, so a drawing moved whole is drawn from it, moved. */
  struct Placed {
    uint64_t key = 0;
    float2 origin;
  };
  std::unordered_map<uint64_t, Placed> tessellated_anywhere_;
  bool interactive_ = false;
  bool zooming_ = false;
  bool complete_ = true;
  std::chrono::steady_clock::time_point render_start_;
  /* Time this render has spent making drawings one at a time (the zoom's budget, see draw_drawing). */
  double made_ms_ = 0.0;
  uint64_t render_serial_ = 0;
  int samples_ = 1;
  /* Set while a layer draws its part of a group's mask: its masking options apply then. */
  struct MaskPass {
    /* Grow (or, negative, shrink) by this many pixels. */
    float grow = 0.0f;
    /* What this pass draws: everything, only areas, only the bands around areas that shrinking
     * clears, or only lines (see draw_mask_source). */
    enum Stage { All, Areas, Bands, Lines } stage = All;
    bool exclude_strokes = false;
    /* Every shape fills, filled or not (a mask's Fill Every Shape). */
    bool filled = false;
    /* A mask's Extend (MaskExtend): the shapes swept to far off that way. */
    int extend = 0;
    /* Only the strokes (a mask's Strokes Only): no fills. */
    bool exclude_fills = false;
    /* Fills cut at their lines' inner edge (a mask's Inside the Stroke, with Fills Only). */
    bool inside_stroke = false;
  } mask_pass_;
  /* Stencil bits a colour sample must have set (the masks it's inside). */
  unsigned required_stencil_ = 0;
  /* Of those, the mask levels whose rows all end lines (End Lines at Edge, Ends: Brush): the lines
   * end where those rows cut them, worked out along them, so they aren't sliced there too. */
  unsigned line_free_stencil_ = 0;
  /* What apply_masks found for line_free_stencil_. */
  unsigned applied_line_free_ = 0;
  /* Of required_stencil_, the levels a layer's fills leave out: a masked layer with rows that cut only
   * lines or only fills gets a level for its lines and one for its fills, each left out by the other. */
  unsigned fill_free_stencil_ = 0;
  unsigned applied_fill_free_ = 0;
  /* Stencil bits a mask shape is only written where they are set (its own masks), or 0. */
  unsigned coverage_test_ = 0;

 public:
  /* One step of a mask card as the stencil uses it (only the layers still there). */
  struct MaskRow {
    MaskMode mode = MaskMode::Add;
    bool invert = false;
    std::vector<uint64_t> layers;
    MaskCombine combine = MaskCombine::Union;
    float grow = 0.0f;
    bool fills_only = false;
    bool strokes_only = false;
    bool filled = false;
    bool inside_stroke = false;
    int extend = 0;
    bool clip = false;
    /* Its card's Start With (everything shown). */
    bool start_shown = false;
    bool operator==(const MaskRow &) const = default;
  };
  /* Each stencil level's cards (each its steps), which must all show: a masked layer's cards that
   * mask its fills and slice its lines, those that leave its lines (or end them with a brush), and
   * those that slice only its lines (outermost layer first). */
  using MaskLevels = std::vector<std::vector<std::vector<MaskRow>>>;

 private:
  /* The masks the stencil holds now (bit k: level k), so neighbours with the same masks reuse it. */
  MaskLevels mask_clauses_;
  uint64_t mask_preview_layer_ = 0;
  bool mask_preview_lines_ = false;
  int mask_preview_upto_ = -1;
  bool initialized_ = false;
  bool failed_ = false;
  int width_ = 0;
  int height_ = 0;
  int target_samples_ = 0;
  Transform2D document_to_pixels_;
  std::vector<Target *> targets_;
  Target *output_ = nullptr;
};

}  // namespace oanim
