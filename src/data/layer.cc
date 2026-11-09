#include "data/layer.hh"

#include <algorithm>

namespace oanim {

bool modifier_makes_points(const Modifier &m)
{
  return m.type == ModifierType::Mirror || m.type == ModifierType::Bevel || m.type == ModifierType::Resample ||
         m.type == ModifierType::Boolean || modifier_bends(m.type);
}

static constexpr const char *CAGE_METHOD_IDS[] = {"MEAN_VALUE", "RIGID", "OUTLINE", "ENVELOPE"};

const char *cage_method_id(CageMethod method)
{
  return CAGE_METHOD_IDS[int(method)];
}

bool cage_method_from_id(const std::string &id, CageMethod &r_method)
{
  for (int k = 0; k < 4; k++) {
    if (id == CAGE_METHOD_IDS[k]) {
      r_method = CageMethod(k);
      return true;
    }
  }
  return false;
}

bool deform_is_envelope(const Modifier &m)
{
  return (m.type == ModifierType::CurveDeform && m.deform_curve_method == CurveMethod::Envelope) ||
         (m.type == ModifierType::CageDeform && m.deform_cage_method == CageMethod::Envelope);
}

void deform_type_set(Modifier &m, ModifierType to)
{
  if (to == m.type) {
    return;
  }
  const bool envelope = deform_is_envelope(m);
  const std::string from_name = deform_type_name(m.type);
  if (m.name.rfind(from_name, 0) == 0) {
    m.name = deform_type_name(to) + m.name.substr(from_name.size());
  }
  m.type = to;
  if (envelope && to == ModifierType::CurveDeform) {
    m.deform_curve_method = CurveMethod::Envelope;
  }
  if (envelope && to == ModifierType::CageDeform) {
    m.deform_cage_method = CageMethod::Envelope;
  }
}

static constexpr const char *MESH_METHOD_IDS[] = {"CELLS", "OUTLINE", "POINTS"};

const char *mesh_method_id(MeshMethod method)
{
  return MESH_METHOD_IDS[int(method)];
}

bool mesh_method_from_id(const std::string &id, MeshMethod &r_method)
{
  if (id == "ENVELOPE") {
    r_method = MeshMethod::Outline;
    return true;
  }
  for (int k = 0; k < 3; k++) {
    if (id == MESH_METHOD_IDS[k]) {
      r_method = MeshMethod(k);
      return true;
    }
  }
  return false;
}

static constexpr const char *ENVELOPE_METHOD_IDS[] = {"STANDARD", "BLENDED"};

const char *envelope_method_id(EnvelopeMethod method)
{
  return ENVELOPE_METHOD_IDS[int(method)];
}

bool envelope_method_from_id(const std::string &id, EnvelopeMethod &r_method)
{
  for (int k = 0; k < 2; k++) {
    if (id == ENVELOPE_METHOD_IDS[k]) {
      r_method = EnvelopeMethod(k);
      return true;
    }
  }
  return false;
}

static constexpr const char *CURVE_METHOD_IDS[] = {"SMOOTH", "ENVELOPE"};

const char *curve_method_id(CurveMethod method)
{
  return CURVE_METHOD_IDS[int(method)];
}

bool curve_method_from_id(const std::string &id, CurveMethod &r_method)
{
  for (int k = 0; k < 2; k++) {
    if (id == CURVE_METHOD_IDS[k]) {
      r_method = CurveMethod(k);
      return true;
    }
  }
  return false;
}

static constexpr const char *INFLUENCE_TYPE_IDS[] = {"EVERYWHERE", "CAPSULE"};

const char *influence_type_id(InfluenceType type)
{
  return INFLUENCE_TYPE_IDS[int(type)];
}

bool influence_type_from_id(const std::string &id, InfluenceType &r_type)
{
  for (int k = 0; k < 2; k++) {
    if (id == INFLUENCE_TYPE_IDS[k]) {
      r_type = InfluenceType(k);
      return true;
    }
  }
  return false;
}

bool modifier_can_apply(const Modifier &m)
{
  /* A deform modifier does nothing on the setup frame, where Apply works. */
  return m.type != ModifierType::Armature && !modifier_is_deform(m.type);
}

static constexpr const char *MASK_EXTEND_IDS[] = {"NONE", "UP", "DOWN", "LEFT", "RIGHT"};

const char *mask_extend_identifier(MaskExtend extend)
{
  return MASK_EXTEND_IDS[int(extend)];
}

bool mask_extend_from_identifier(const std::string &identifier, MaskExtend *r_extend)
{
  for (int i = 0; i < 5; i++) {
    if (identifier == MASK_EXTEND_IDS[i]) {
      *r_extend = MaskExtend(i);
      return true;
    }
  }
  return false;
}

static constexpr const char *MASK_COMBINE_IDS[] = {"UNION", "DIFFERENCE", "INTERSECTION"};
static constexpr const char *MASK_COMBINE_NAMES[] = {"Union", "Difference", "Intersection"};

const char *mask_combine_identifier(MaskCombine combine)
{
  return MASK_COMBINE_IDS[int(combine)];
}

const char *mask_combine_name(MaskCombine combine)
{
  return MASK_COMBINE_NAMES[int(combine)];
}

bool mask_combine_from_identifier(const std::string &identifier, MaskCombine *r_combine)
{
  for (int i = 0; i < 3; i++) {
    if (identifier == MASK_COMBINE_IDS[i]) {
      *r_combine = MaskCombine(i);
      return true;
    }
  }
  return false;
}

static constexpr const char *MASK_CUTS_IDS[] = {"ALL", "FILLS", "LINES"};

const char *mask_cuts_identifier(MaskCuts cuts)
{
  return MASK_CUTS_IDS[int(cuts)];
}

bool mask_cuts_from_identifier(const std::string &identifier, MaskCuts *r_cuts)
{
  for (int i = 0; i < 3; i++) {
    if (identifier == MASK_CUTS_IDS[i]) {
      *r_cuts = MaskCuts(i);
      return true;
    }
  }
  return false;
}

static constexpr const char *MASK_MODE_IDS[] = {"ADD", "SUBTRACT"};

const char *mask_mode_identifier(MaskMode mode)
{
  return MASK_MODE_IDS[int(mode)];
}

bool mask_mode_from_identifier(const std::string &identifier, MaskMode *r_mode)
{
  for (int i = 0; i < 2; i++) {
    if (identifier == MASK_MODE_IDS[i]) {
      *r_mode = MaskMode(i);
      return true;
    }
  }
  return false;
}

bool mask_card_ends_lines(const LayerMask &card, bool *r_flat)
{
  if (!card.end_lines || card.cuts == MaskCuts::Fills || card.clip) {
    return false;
  }
  *r_flat = card.end_lines_flat;
  return true;
}

const char *mask_verb_identifier(MaskVerb verb)
{
  return verb == MaskVerb::Add ? "ADD" : verb == MaskVerb::Remove ? "REMOVE" : "KEEP_ONLY";
}

bool mask_verb_from_identifier(const std::string &identifier, MaskVerb *r_verb)
{
  for (const MaskVerb verb : {MaskVerb::Add, MaskVerb::Remove, MaskVerb::KeepOnly}) {
    if (identifier == mask_verb_identifier(verb)) {
      *r_verb = verb;
      return true;
    }
  }
  return false;
}

MaskVerb mask_step_verb(const MaskStep &step)
{
  if (step.mode == MaskMode::Add) {
    return MaskVerb::Add;
  }
  return step.invert ? MaskVerb::KeepOnly : MaskVerb::Remove;
}

void mask_step_set_verb(MaskStep &step, MaskVerb verb)
{
  /* Show All or Hide All: there's no keeping only inside the whole canvas. */
  if (step.whole && verb == MaskVerb::KeepOnly) {
    return;
  }
  step.mode = verb == MaskVerb::Add ? MaskMode::Add : MaskMode::Subtract;
  step.invert = verb == MaskVerb::KeepOnly;
}

bool mask_step_empty(const MaskStep &step)
{
  return step.layers.empty() && step.children.empty() && !step.whole;
}

/* Whether nothing paints below step `index`: it is the first thing on the base. */
static bool mask_step_on_base(const LayerMask &card, size_t index)
{
  return std::all_of(card.steps.begin(), card.steps.begin() + std::ptrdiff_t(index), mask_step_empty);
}

static bool mask_steps_same_shape(const MaskStep &a, const MaskStep &b)
{
  return a.grow == b.grow && a.fills_only == b.fills_only && a.strokes_only == b.strokes_only && a.filled == b.filled &&
         a.inside_stroke == b.inside_stroke && a.extend == b.extend;
}

/* How a group's children make one Combine step: + then all +, all -, or all Keep Only; false if they don't. */
static bool mask_group_combine(const MaskStep &group, MaskCombine *r_combine)
{
  const std::vector<MaskStep> &kids = group.children;
  if (kids.empty() || mask_step_verb(kids[0]) != MaskVerb::Add || kids[0].invert) {
    return false;
  }
  const MaskVerb rest = kids.size() > 1 ? mask_step_verb(kids[1]) : MaskVerb::Add;
  for (size_t i = 0; i < kids.size(); i++) {
    if ((i > 0 && mask_step_verb(kids[i]) != rest) || (i > 0 && kids[i].invert != (rest == MaskVerb::KeepOnly)) ||
        !mask_steps_same_shape(kids[0], kids[i]) || kids[i].group || kids[i].whole ||
        (kids[i].layers.size() > 1 && (rest != MaskVerb::Add || kids[i].combine != MaskCombine::Union)))
    {
      return false;
    }
  }
  *r_combine = rest == MaskVerb::Add ? MaskCombine::Union
               : rest == MaskVerb::Remove ? MaskCombine::Difference
                                          : MaskCombine::Intersection;
  return true;
}

const char *mask_group_problem(const LayerMask &card, size_t index)
{
  const MaskStep &group = card.steps[index];
  if (!group.group || group.children.empty() ||
      std::any_of(group.children.begin(), group.children.end(), [](const MaskStep &s) { return mask_step_verb(s) == MaskVerb::Add; }))
  {
    return nullptr;
  }
  return "This group is empty: it starts from nothing, so it needs a + area or Show All in it";
}

LayerMask mask_card_flatten(const LayerMask &card)
{
  LayerMask flat = card;
  flat.steps.clear();
  const auto inline_children = [&](const MaskStep &group, size_t i, bool swapped) {
    for (size_t k = 0; k < group.children.size(); k++) {
      MaskStep s = group.children[k];
      s.order = int(i << 8) | int(k + 1);
      if (swapped) {
        s.mode = s.mode == MaskMode::Add ? MaskMode::Subtract : MaskMode::Add;
      }
      flat.steps.push_back(std::move(s));
    }
  };
  for (size_t i = 0; i < card.steps.size(); i++) {
    const MaskStep &s = card.steps[i];
    if (!s.group) {
      flat.steps.push_back(s);
      flat.steps.back().order = int(i << 8);
      continue;
    }
    if (s.children.empty()) {
      continue;
    }
    const MaskVerb verb = mask_step_verb(s);
    /* A + group with Inverse paints all but its shape, which its own steps don't. */
    const bool outside = verb == MaskVerb::Add && s.invert;
    if (!outside && mask_step_on_base(card, i)) {
      /* Built on the base, the group's own steps paint it: taking it from white is each of them the other way
       * (de Morgan), and keeping only it is the same steps on black. */
      if (verb == MaskVerb::Add && !card.start_shown) {
        inline_children(s, i, false);
      }
      else if (verb == MaskVerb::Remove && card.start_shown) {
        inline_children(s, i, true);
      }
      else if (verb == MaskVerb::KeepOnly && card.start_shown) {
        flat.start_shown = false;
        inline_children(s, i, false);
      }
      continue;
    }
    MaskCombine combine;
    if (!outside && verb != MaskVerb::KeepOnly &&
        std::all_of(s.children.begin(), s.children.end(), [](const MaskStep &c) { return mask_step_verb(c) == MaskVerb::Add; }))
    {
      inline_children(s, i, verb == MaskVerb::Remove);
    }
    else if (mask_group_combine(s, &combine)) {
      MaskStep one = s.children.front();
      one.layers.clear();
      for (const MaskStep &c : s.children) {
        one.layers.insert(one.layers.end(), c.layers.begin(), c.layers.end());
      }
      one.mode = s.mode;
      one.invert = s.invert;
      one.combine = combine;
      one.order = int(i << 8) | 0xff;
      flat.steps.push_back(std::move(one));
    }
    else {
      /* Stays a group: every evaluator works out its shape, as paint on nothing, and paints that. */
      MaskStep group = s;
      group.order = int(i << 8);
      for (size_t k = 0; k < group.children.size(); k++) {
        group.children[k].order = int(i << 8) | int(k + 1);
      }
      flat.steps.push_back(std::move(group));
    }
  }
  /* Hide All with nothing on it hides everything, but a card with no steps counts as no card: painted black
   * inside and outside one of its layers. */
  if (flat.steps.empty() && !flat.start_shown) {
    const std::vector<uint64_t> named = mask_card_layers(card);
    if (!named.empty()) {
      MaskStep black;
      black.mode = MaskMode::Subtract;
      black.layers = {named.front()};
      flat.steps.push_back(black);
      black.invert = true;
      flat.steps.push_back(black);
    }
  }
  return flat;
}

std::string mask_card_name(const LayerMask &card, int index)
{
  return card.name.empty() ? "Mask " + std::to_string(index + 1) : card.name;
}

std::vector<uint64_t> mask_card_layers(const LayerMask &card)
{
  std::vector<uint64_t> ids;
  const auto add = [&](auto &&self, const MaskStep &s) -> void {
    for (const uint64_t id : s.layers) {
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
      }
    }
    for (const MaskStep &child : s.children) {
      self(self, child);
    }
  };
  for (const MaskStep &s : card.steps) {
    add(add, s);
  }
  return ids;
}

bool mask_step_cuts(const MaskStep &step)
{
  return step.mode == MaskMode::Subtract && !step.invert;
}

bool mask_cuts(const LayerMask &card)
{
  return card.start_shown && !card.steps.empty() && mask_step_cuts(card.steps.front());
}

bool mask_step_from_old_mode(const std::string &identifier, bool outside, bool first, MaskStep *s)
{
  if (identifier.empty()) {
    if (outside) {
      s->mode = MaskMode::Subtract;
      return true;
    }
    if (first) {
      s->mode = MaskMode::Add;
      return true;
    }
  }
  else if (identifier != "INTERSECT") {
    if (identifier == "DIFFERENCE") {
      s->mode = MaskMode::Add;
      return true;
    }
    return mask_mode_from_identifier(identifier, &s->mode);
  }
  /* Keeping only inside is hiding outside. */
  s->mode = MaskMode::Subtract;
  s->invert = !s->invert;
  return true;
}

void mask_card_start_from_steps(LayerMask &card)
{
  if (!card.steps.empty()) {
    card.start_shown = card.steps.front().mode == MaskMode::Subtract;
  }
}

Transform2D Layer::local_transform() const
{
  return layer_local_transform(position, rotation, shear, scale);
}

Transform2D layer_local_transform(const float2 position, const float rotation, const float2 shear, const float2 scale)
{
  Transform2D t = Transform2D::translate(position) * Transform2D::rotate(rotation * PI / 180.0f);
  if (shear.x != 0.0f || shear.y != 0.0f) {
    const float sx = std::clamp(shear.x, -89.0f, 89.0f) * PI / 180.0f;
    const float sy = std::clamp(shear.y, -89.0f, 89.0f) * PI / 180.0f;
    Transform2D k;
    k.a = std::cos(sx);
    k.b = std::sin(sx);
    k.c = -std::sin(sy);
    k.d = std::cos(sy);
    t = t * k;
  }
  return t * Transform2D::scaling(scale);
}

void Layer::set_local_transform(const Transform2D &t, bool negative_x)
{
  float2 x_axis(t.a, t.b);
  float sx = length(x_axis);
  if (sx < 1e-12f) {
    return;
  }
  if (negative_x) {
    x_axis = -x_axis;
    sx = -sx;
  }
  position = {t.tx, t.ty};
  const float turn = std::atan2(x_axis.y, x_axis.x);
  rotation = turn * 180.0f / PI;
  /* The Y axis, unturned: its length is the scale's y and its tilt from up the shear's. */
  const float co = std::cos(-turn), si = std::sin(-turn);
  float2 y_axis(co * t.c - si * t.d, si * t.c + co * t.d);
  float sy = length(y_axis);
  if (y_axis.y < 0.0f) {
    y_axis = -y_axis;
    sy = -sy;
  }
  scale = {sx, sy};
  shear = {0.0f, std::fabs(sy) > 1e-12f ? std::atan2(-y_axis.x, y_axis.y) * 180.0f / PI : 0.0f};
}

const char *layer_type_name(LayerType type)
{
  return type == LayerType::Bone    ? "Bone"
         : type == LayerType::Group ? "Group"
         : type == LayerType::Media ? "Media"
         : type == LayerType::Text  ? "Text"
         : type == LayerType::Switch ? "Switch"
                                    : "Vector";
}

Color label_color_value(LabelColor label)
{
  switch (label) {
    case LabelColor::None:
      return {0, 0, 0, 0};
    case LabelColor::Red:
      return {0.85f, 0.3f, 0.3f, 1};
    case LabelColor::Orange:
      return {0.9f, 0.55f, 0.2f, 1};
    case LabelColor::Yellow:
      return {0.9f, 0.8f, 0.25f, 1};
    case LabelColor::Green:
      return {0.4f, 0.75f, 0.35f, 1};
    case LabelColor::Blue:
      return {0.3f, 0.55f, 0.9f, 1};
    case LabelColor::Purple:
      return {0.65f, 0.45f, 0.85f, 1};
    case LabelColor::Gray:
      return {0.6f, 0.6f, 0.6f, 1};
  }
  return {0, 0, 0, 0};
}

const rna::StructRNA &Layer::rna()
{
  static const rna::StructRNA srna = [] {
    using namespace rna;
    StructRNA s;
    s.identifier = "Layer";
    s.name = "Layer";
    PropertyRNA type_prop = def_enum<Layer, LayerType>("type", "Type", "Kind of layer", &Layer::type,
                                                       {{"VECTOR", "Vector"}, {"GROUP", "Group"}, {"BONE", "Bone"},
                                                        {"MEDIA", "Media"}, {"TEXT", "Text"}, {"SWITCH", "Switch"}});
    type_prop.set = nullptr;
    s.properties = {
        def_string<Layer>("name", "Name", "Name shown in the Layers editor", &Layer::name),
        type_prop,
        def_bool<Layer>("visible", "Visible", "Show the layer in the viewport and renders", &Layer::visible),
        def_bool<Layer>("render_visible", "Show in Renders",
                        "Draw the layer in renders; off, it is a guide: shown while you work, hidden with the "
                        "viewport's overlays (Shift Z), never rendered",
                        &Layer::render_visible),
        def_bool<Layer>("locked", "Locked", "Protect the layer's drawing from editing", &Layer::locked),
        def_bool<Layer>("edit_with_bones", "Editable in Bone Group",
                        "Inside a bone group: pick and move this layer's points along with its bones while the bone "
                        "group is the active layer. It is then in the bone group's All collection; Assign or M put "
                        "it in others",
                        &Layer::edit_with_bones),
        def_bool<Layer>("ignore_group_deforms", "Ignore Group Deforms",
                        "The deform modifiers of the groups around this drawing leave it alone",
                        &Layer::ignore_group_deforms),
        def_bool<Layer>("key_whole_drawing", "Key Whole Drawing",
                        "The drawing is keyed as a whole: an edit on a frame keys every point there, and all its "
                        "points share their keys' frames and timing, edited together in the timeline's Drawing row and "
                        "the Motion Path tool's timing chart and Path Timing graph. Turned on, every point is keyed "
                        "where any is, where it already is",
                        &Layer::key_whole_drawing),
        def_bool<Layer>("key_together", "Key Together",
                        "Everything in this layer is keyed as one movement (a group's or bone layer's layers, a "
                        "vector layer's own channels): an edit on a frame keys all of it there (move, turn, size and "
                        "opacity, bones and drawing's points), all sharing their keys' frames and timing, edited "
                        "together in the timing chart's together row. The same as its timeline row folded. Turned "
                        "on, everything is keyed where anything is, holding where it is (filler keys); turned off, "
                        "the filler keys never edited go again",
                        &Layer::key_together),
        def_bool<Layer>("link_same_named", "Link Same-Named Switches",
                        "A switch: the switches of the same name at the same place inside its different drawings "
                        "(a Mouth in each view of a Heads switch) are linked by name: picking a mouth in one picks "
                        "the same-named mouth in all of them",
                        &Layer::link_same_named),
        def_bool<Layer>("sub_bone_group", "Sub Bone Group",
                        "A bone group inside another: its bones show and can be clicked along with the bone "
                        "group around it while either is worked in, as long as it is shown (a switch of legs, "
                        "each with its own bones)",
                        &Layer::sub_bone_group),
        def_bool<Layer>("edit_descendants", "Edit Descendants",
                        "While this group is picked or active, the drawings inside it (however deep) are edited "
                        "with it, as if picked too",
                        &Layer::edit_descendants),
        def_enum<Layer, LabelColor>("label_color", "Label Color", "Colour tag in the Layers editor",
                                    &Layer::label_color,
                                    {{"NONE", "None"},
                                     {"RED", "Red"},
                                     {"ORANGE", "Orange"},
                                     {"YELLOW", "Yellow"},
                                     {"GREEN", "Green"},
                                     {"BLUE", "Blue"},
                                     {"PURPLE", "Purple"},
                                     {"GRAY", "Gray"}}),
        def_float_fn<Layer>(
            "location_x", "Location X", "Horizontal position", -100000.0f, 100000.0f,
            [](const Layer &l) { return l.position.x; }, [](Layer &l, float v) { l.position.x = v; }),
        def_float_fn<Layer>(
            "location_y", "Location Y", "Vertical position", -100000.0f, 100000.0f,
            [](const Layer &l) { return l.position.y; }, [](Layer &l, float v) { l.position.y = v; }),
        def_float<Layer>("rotation", "Rotation", "Rotation in degrees, counter-clockwise", &Layer::rotation,
                         -36000.0f, 36000.0f),
        def_float_fn<Layer>(
            "scale_x", "Scale X", "Horizontal scale", -1000.0f, 1000.0f, [](const Layer &l) { return l.scale.x; },
            [](Layer &l, float v) { l.scale.x = v; }),
        def_float_fn<Layer>(
            "scale_y", "Scale Y", "Vertical scale", -1000.0f, 1000.0f, [](const Layer &l) { return l.scale.y; },
            [](Layer &l, float v) { l.scale.y = v; }),
        def_float_fn<Layer>(
            "shear_x", "Shear X", "How far the layer's X axis tilts from square, degrees (counter-clockwise)",
            -89.0f, 89.0f, [](const Layer &l) { return l.shear.x; }, [](Layer &l, float v) { l.shear.x = v; }),
        def_float_fn<Layer>(
            "shear_y", "Shear Y", "How far the layer's Y axis tilts from square, degrees (counter-clockwise)",
            -89.0f, 89.0f, [](const Layer &l) { return l.shear.y; }, [](Layer &l, float v) { l.shear.y = v; }),
        def_float<Layer>("opacity", "Opacity", "How opaque the layer is", &Layer::opacity, 0.0f, 1.0f),
        def_bool<Layer>("select", "Select", "Picked in the Layers editor, for Group With Selection",
                        &Layer::selected),
        def_bool<Layer>("expanded", "Expanded", "A group shows its layers in the Layers editor (open)",
                        &Layer::expanded),
        def_bool<Layer>("clip_to_below", "Clip to Below",
                        "Show the layer only where the layer below it (the first one down that isn't clipped "
                        "itself) has its shapes, as Photoshop's clipping masks",
                        &Layer::clip_to_below),
        def_bool<Layer>("clip_exclude_strokes", "Exclude Strokes",
                        "Clip to Below clips to the layer below's fills only, leaving its lines out",
                        &Layer::clip_exclude_strokes),
        /* Media layers (data/media_layer.hh). */
        def_bool_fn<Layer>(
            "media_guide", "Guide",
            "Show it while you work, hidden with the viewport's overlays (Shift Z), never rendered: the same as Show in "
            "Renders off",
            [](const Layer &l) { return !l.render_visible; }, [](Layer &l, bool v) { l.render_visible = !v; }),
        def_enum_fn<Layer>(
            "media_fit", "Fit", "How the picture sits in the camera's frame (at zoom 1)",
            {{"FIT", "Fit"}, {"FILL", "Fill"}, {"STRETCH", "Stretch"}, {"ORIGINAL", "Original Size"}},
            [](const Layer &l) { return int(l.media.fit); },
            [](Layer &l, int v) { l.media.fit = MediaFit(std::clamp(v, 0, 3)); }),
        def_bool_fn<Layer>(
            "media_greyscale", "Greyscale", "Show the picture in shades of grey, so drawing over it stands out",
            [](const Layer &l) { return l.media.greyscale; }, [](Layer &l, bool v) { l.media.greyscale = v; }),
        def_float_fn<Layer>(
            "media_tint_amount", "Tint", "How far the picture is tinted towards the tint colour (a light table's look)",
            0.0f, 1.0f, [](const Layer &l) { return l.media.tint_amount; },
            [](Layer &l, float v) { l.media.tint_amount = v; }),
        def_bool_fn<Layer>(
            "media_in_front", "In Front", "Draw it over everything (unmasked) rather than at its place in the stack",
            [](const Layer &l) { return l.media.in_front; }, [](Layer &l, bool v) { l.media.in_front = v; }),
        def_bool_fn<Layer>(
            "media_dont_auto_key", "Don't Auto Key",
            "Moving, turning, scaling or fading it never sets keys, whatever the record button says (it changes it "
            "on every frame while it isn't animated); keying it by hand still does",
            [](const Layer &l) { return l.media.no_auto_key; }, [](Layer &l, bool v) { l.media.no_auto_key = v; }),
        def_float_fn<Layer>(
            "media_volume", "Volume", "How loud its sound plays (1: as recorded)", 0.0f, 4.0f,
            [](const Layer &l) { return l.media.volume; }, [](Layer &l, float v) { l.media.volume = v; }),
        def_bool_fn<Layer>(
            "media_muted", "Mute", "Silence its sound", [](const Layer &l) { return l.media.muted; },
            [](Layer &l, bool v) { l.media.muted = v; }),
        /* Text layers (data/text.hh): the layout; the runs' fonts and looks through layer.text_runs. */
        def_enum_fn<Layer>(
            "text_align", "Align", "How the lines line up: on the origin (point text) or in the box",
            {{"LEFT", "Left"}, {"CENTER", "Center"}, {"RIGHT", "Right"}, {"JUSTIFY", "Justify"}},
            [](const Layer &l) { return int(l.text.align); },
            [](Layer &l, int v) { l.text.align = TextAlign(std::clamp(v, 0, 3)); }),
        def_float_fn<Layer>(
            "text_line_spacing", "Line Spacing", "How far apart the lines are, a multiple of the font's own", 0.1f,
            10.0f, [](const Layer &l) { return l.text.line_spacing; },
            [](Layer &l, float v) { l.text.line_spacing = v; }),
        def_float_fn<Layer>(
            "text_letter_spacing", "Letter Spacing", "Space added after every letter, in ems", -1.0f, 10.0f,
            [](const Layer &l) { return l.text.letter_spacing; },
            [](Layer &l, float v) { l.text.letter_spacing = v; }),
        def_float_fn<Layer>(
            "text_word_spacing", "Word Spacing", "Space added at every space, in ems", -1.0f, 10.0f,
            [](const Layer &l) { return l.text.word_spacing; }, [](Layer &l, float v) { l.text.word_spacing = v; }),
        def_float_fn<Layer>(
            "text_shear", "Shear", "Slant: each point moves across by this much of its height above the baseline",
            -4.0f, 4.0f, [](const Layer &l) { return l.text.shear; }, [](Layer &l, float v) { l.text.shear = v; }),
        def_float_fn<Layer>(
            "text_box_width", "Box Width",
            "Lines wrap at this width (a text box); 0: point text, lines breaking only at new lines", 0.0f, 100000.0f,
            [](const Layer &l) { return l.text.box_width; }, [](Layer &l, float v) { l.text.box_width = v; }),
        def_bool_fn<Layer>(
            "text_dont_auto_key", "Don't Auto Key",
            "Moving, turning, scaling or fading it never sets keys, whatever the record button says (it changes it "
            "on every frame while it isn't animated); keying it by hand still does",
            [](const Layer &l) { return l.text.no_auto_key; }, [](Layer &l, bool v) { l.text.no_auto_key = v; }),
        def_string_fn<Layer>(
            "text_body", "Text", "The whole text (new lines as \\n); set, it keeps the runs' styles where unchanged",
            [](const Layer &l) { return text_string(l.text); },
            [](Layer &l, const std::string &v) { text_set_string(l.text, v); }),
    };
    return s;
  }();
  return srna;
}

}  // namespace oanim
