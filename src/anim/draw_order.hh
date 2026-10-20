#pragma once

/* Animated draw order (Rive's draw order rules, Cartoon Animator's Send to Front / Release):
 * every layer has a Draw Order setting, Normal (where the Layers editor puts it), In Front Of a
 * layer, or Behind one, keyed like visibility (each key holds until the next). A rule can name any
 * layer in the document, across groups and bone layers; a rule on a group moves the whole group.
 * Masks name their layers, so a layer keeps its masks wherever it is drawn.
 *
 * A group faded below full opacity is drawn as one image (so its overlapping children don't show
 * through each other): nothing inside it can leave it, so a rule inside it that names a layer
 * outside moves the whole faded group.
 *
 * The setting is stored as a number in an animated channel: 0 Normal, the target layer's id for
 * In Front Of, minus it for Behind. Layer ids are small whole numbers, which floats hold exactly. */

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sky {

struct Document;
struct Layer;

enum class DrawOrderMode { Normal, Front, Behind };

struct DrawOrderRule {
  DrawOrderMode mode = DrawOrderMode::Normal;
  uint64_t target = 0;
};

DrawOrderRule draw_order_rule(float value);
float draw_order_value(const DrawOrderRule &rule);
const char *draw_order_mode_identifier(DrawOrderMode mode);
bool draw_order_mode_from_identifier(const std::string &identifier, DrawOrderMode *r_mode);

/* One thing drawn, bottom first: a drawing layer, or a faded group drawn as one image. `masked`:
 * it and the groups holding it (within what was resolved) that have masks, outermost first: all of
 * their masks apply to it. */
struct DrawStep {
  const Layer *layer = nullptr;
  std::vector<const Layer *> masked;
};

using LayerShown = std::function<bool(const Layer &)>;

/* Whether any layer in `layers` (or inside them) has a rule other than Normal right now. */
bool draw_order_any_rule(const std::vector<Layer> &layers);

/* `scope` (a list of layers, top first) as drawn now, bottom first, with every rule applied.
 * Layers `shown` rejects and fully faded ones are left out. */
std::vector<DrawStep> draw_order_steps(const Document &doc, const std::vector<Layer> &scope, const LayerShown &shown);

/* Every drawing layer of the document as drawn now, bottom first (a faded group's layers in its
 * place). */
std::vector<const Layer *> draw_order_layers(const Document &doc, const LayerShown &shown);

/* For the UI: "Normal", "In Front Of Torso", "Behind Head", with " (missing)" for a layer that is
 * gone. */
std::string draw_order_label(const Document &doc, float value);

/* Sets a layer's Draw Order on the current frame: on the setup frame (0) what it is at rest (its
 * frame-0 key once it has keys), on any other frame a key there (its setup keeping what it had). */
void layer_draw_order_set(Document &doc, Layer &layer, const DrawOrderRule &rule);

/* The outermost group faded below full opacity that `layer` is inside, or nullptr: a rule of
 * `layer` naming a layer outside it moves that group as a whole. */
const Layer *draw_order_faded_group(const Document &doc, uint64_t layer);

/* Whether `ancestor` is `layer` or holds it. */
bool layer_inside(const Document &doc, uint64_t layer, uint64_t ancestor);

}  // namespace sky
