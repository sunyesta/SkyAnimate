#pragma once

/* X mirror editing between sided layers: with the viewport header's X on, a layer named for a side
 * (Ear_L, Ear.R, L_Ear, LeftEar...: the names bones pair by, bone_mirror_name) mirrors its point
 * edits into its partner (Ear_R) instead of across its own axis. The two are mirrored across the
 * document's vertical axis (a bone layer's, inside one), as Symmetrize Layers makes them; points pair
 * by where they are in the setup pose, so the pairs hold on every frame. */

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "data/document.hh"

namespace oanim {

/* The layer `id` mirrors into: a drawing layer with its other side's name, beside it if there is
 * one, else anywhere; null for a layer without a side or without a partner. */
Layer *layer_mirror_partner(Document &doc, uint64_t id);
/* Adds a layer of `type` above the active one, as document_add_layer does. With X mirror editing on,
 * inside a group named for a side (Arm_L) whose partner group (Arm_R) is there, the partner gets one
 * too, at the same place: the two named for their sides in the groups' way (Layer 3_L, Layer 3_R), so
 * they pair as every sided layer does. The first stays active. */
Layer &document_add_layer_mirrored(Document &doc, LayerType type, bool mirror_x);

/* X mirror editing for names: a layer renamed since `previous` (Ear_L to Lobe_L) renames its partner
 * (Ear_R to Lobe_R), when that kept its name, the new name has a side and the other side's is free. */
void document_mirror_layer_names(Document &doc, const Document &previous);

/* X mirror editing for layer order: a sided group (Arm_L) whose layers changed order since `previous`
 * reorders its partner group's (Arm_R) layers to match, each going where its counterpart (the same
 * name for the other side) went; layers without one keep their places. Not when both changed. */
void document_mirror_layer_order(Document &doc, const Document &previous);

/* Whether edits in layer `id` mirror across its own vertical axis with X on: only while it has no
 * partner to mirror into. */
bool layer_mirror_x_self(Document &doc, uint64_t id, bool mirror_x);

/* Layer `id`'s space -> its partner's, reflected across the pair's axis (setup pose). */
Transform2D layer_mirror_to_partner(Document &doc, uint64_t id, uint64_t partner);
/* Each of `points`' image in the partner: its point nearest the reflection in the setup pose (within
 * 2% of the partner's size), or -1. */
std::vector<int> layer_mirror_images(Document &doc, const Layer &layer, const Layer &partner,
                                     const std::vector<int> &points);

/* The edited layers' drawings before an edit, for layer_mirror_partners_end. */
struct PartnerMirror {
  std::vector<std::pair<uint64_t, VectorDrawing>> before;
  /* Every layer being edited: a partner's selected points move by themselves only in one of these. */
  std::vector<uint64_t> editing;
};
/* Takes the drawings of the layers `ids` that have a partner (nothing while `mirror_x` is off). */
PartnerMirror layer_mirror_partners_begin(Document &doc, const std::vector<uint64_t> &ids, bool mirror_x);
/* Each point (or handle) the edit changed in those layers moves its partner's image to match,
 * reflected; selected points of a partner that is being edited too move by themselves and are left
 * alone. Returns how many partner points changed. */
int layer_mirror_partners_end(Document &doc, const PartnerMirror &mirror);

/* The shape of `layer` whose points are the mirror images of shape `shape`'s: in the partner of a
 * sided layer, else in the same layer across its axis (not the shape itself). False when none. */
bool layer_mirror_shape(Document &doc, Layer &layer, int shape, Layer **r_layer, int *r_shape);
/* X mirror editing for looks: every shape whose look (fill, outline, colours, width, brush, material)
 * differs from `previous` gives it to its mirror shape, unless that one changed too. */
void document_mirror_looks(Document &doc, const Document &previous);

/* X mirror editing for new geometry, compared with `previous`: a new curve (Draw, Pen, Shape tool,
 * paste) gets a reflected copy with its shapes; a point the knife cut into a curve cuts the image
 * segment at the reflected spot; points grown from an open end (Extrude, the pen going on) grow the
 * image curve's end; a new shape over existing curves (Fill) is made over the image segments. In the
 * partner of a sided layer, else in the same layer across its axis; never twice (an image already
 * there, as Duplicate makes, is left). */
void document_mirror_new_geometry(Document &doc, const Document &previous);

/* Symmetrize (right-click on points) for a sided layer: its selected points and the partner's points
 * nearest their reflections (one each) are made mirror images across the pair's axis, one side copied
 * onto the other as drawing_symmetrize does inside one layer (`right_to_left`: the +x side is copied).
 * Returns that result; on `done` false nothing changed and `r_unmatched` holds the layer's selected
 * points left without a partner. */
SymmetrizeResult layer_symmetrize_with_partner(Document &doc, Layer &layer, Layer &partner, bool right_to_left,
                                               float threshold, bool setup_frame,
                                               const std::map<uint64_t, uint64_t> *weight_flip,
                                               std::vector<int> *r_unmatched);

}  // namespace oanim
