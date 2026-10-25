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
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math.hh"
#include "data/layer.hh"
#include "gpu/brush_stroke.hh"
#include "gpu/tessellate.hh"

namespace oanim {

struct Document;
struct Layer;
struct Vertex;
struct BrushDabs;
struct BrushImage;

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
  /* Local View: draw only these layers, what's inside them and the groups holding them (empty: all). */
  void set_local_view(const Document &doc, const std::vector<uint64_t> &ids);

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
  struct Target {
    /* Drawn into: multisampled colour and depth-stencil. */
    unsigned int draw_framebuffer = 0;
    unsigned int color = 0;
    unsigned int depth_stencil = 0;
    /* The resolved image, averaged from the samples. */
    unsigned int resolve_framebuffer = 0;
    unsigned int texture = 0;
    bool in_use = false;
  };

  bool init();
  Target *acquire_target();
  void release_target(Target *target);
  void free_targets();
  void bind_target(const Target *target);
  void resolve(const Target *target);
  void clear(const Color &color);

  /* Draws a list of layers (top first) into the bound target, bottom first. */
  void draw_layers(const Document &doc, const std::vector<Layer> &layers, const Transform2D &parent,
                   Target *target, float opacity);
  void draw_layer(const Document &doc, const Layer &layer, const Transform2D &transform, Target *target,
                  float opacity);
  /* The mask preview (set_mask_preview), over the drawing at 90%: black, and white where the mask
   * shows the layer. */
  void draw_mask_preview(const Document &doc, const Layer &layer);
  /* Sets the stencil to the masks of `masked` (the layers whose masks apply to what is drawn next)
   * and returns the stencil bits a sample needs. */
  unsigned apply_masks(const Document &doc, const std::vector<const Layer *> &masked);
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
  /* Wherever the stencil's `test` bits equal `value`, applies `op` (with `ref`) to the `write` bits. */
  void stencil_where(unsigned test, unsigned value, unsigned write, unsigned ref, unsigned op);
  /* Writes a layer's coverage (and its children's) into one stencil bit, set or cleared. */
  void draw_coverage(const Document &doc, const Layer &layer, const Transform2D &parent, unsigned bit, bool set);
  /* Sets or clears one stencil bit everywhere. */
  void stencil_fill(unsigned bit, bool set);
  /* Stencil test for colour drawing: every bit in required_stencil_ must be set (for a layer's
   * lines, all but line_free_stencil_'s; for its fills, all but fill_free_stencil_'s; a group faded
   * as a whole goes by its fills). */
  enum class Paint { Fill, Line };
  void use_color_stencil(Paint paint = Paint::Fill);
  /* Draws a vector layer's shapes into the bound target. */
  void draw_drawing(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity);
  void draw_vertices(const std::vector<Vertex> &vertices, const Transform2D &transform);
  /* Draws brush dabs. `own_colors`: a colour brush shown in its own colours. Merged alpha
   * draws them into an image of their own first, keeping the strongest dab where they overlap. */
  /* `vaos`, `vbos` (optional): a kept step's buffers, one per image, so its stamps stay on the GPU
   * (uploaded the first time). */
  /* How opaque a dab's pixel must be to count in a mask (see PaintStep::coverage_alpha). */
  float coverage_alpha_ = 0.5f;
  void draw_dabs(const BrushDabs &dabs, const std::vector<BrushImage> &images, bool own_colors, bool merged,
                 const Transform2D &transform, float opacity, std::vector<unsigned int> *vaos = nullptr,
                 std::vector<unsigned int> *vbos = nullptr);
  unsigned int brush_texture(const BrushImage &image);
  void set_matrix(unsigned int program, const Transform2D &transform);
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
    unsigned int texture = 0;
    uint64_t serial = 0;
    int width = 0, height = 0;
  };
  /* One texture per Media layer, refilled when its frame changes. */
  std::map<uint64_t, MediaTexture> media_textures_;
  unsigned int media_program_ = 0;

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
  std::map<uint64_t, unsigned int> brush_textures_;
  unsigned int brush_program_ = 0;
  unsigned int brush_vao_ = 0;
  unsigned int brush_vbo_ = 0;
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
    /* On the GPU once drawn (uploaded once, kept with it): the triangles' buffer and layout, or the
     * stamps' per image. */
    unsigned int vao = 0, vbo = 0;
    std::vector<unsigned int> dab_vaos, dab_vbos;
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
  };
  struct Chunk {
    unsigned int vao = 0, vbo = 0;
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
  unsigned int batch_program_ = 0;
  /* The stencil test use_color_stencil last set (put back after a flush). */
  unsigned color_required_ = 0;
  void flush_batch();
  /* A step of brushed stamps, drawn at once. */
  void draw_dabs_step(PaintStep &step, const Transform2D &transform, float opacity);
  static void free_step(PaintStep &step);
  std::unordered_map<uint64_t, Tessellated> tessellated_;
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
  std::chrono::steady_clock::time_point render_start_;
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
  unsigned int color_program_ = 0;
  unsigned int composite_program_ = 0;
  unsigned int stencil_program_ = 0;
  unsigned int vao_ = 0;
  unsigned int vbo_ = 0;
  unsigned int quad_vao_ = 0;
  int width_ = 0;
  int height_ = 0;
  int target_samples_ = 0;
  Transform2D document_to_pixels_;
  std::vector<Target *> targets_;
  Target *output_ = nullptr;
};

}  // namespace oanim
