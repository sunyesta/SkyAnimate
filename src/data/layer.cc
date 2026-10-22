#include "data/layer.hh"

#include <algorithm>

namespace oanim {

bool modifier_can_apply(const Modifier &m)
{
  return m.type != ModifierType::Armature;
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

std::string mask_card_name(const LayerMask &card, int index)
{
  return card.name.empty() ? "Mask " + std::to_string(index + 1) : card.name;
}

std::vector<uint64_t> mask_card_layers(const LayerMask &card)
{
  std::vector<uint64_t> ids;
  for (const MaskStep &s : card.steps) {
    for (const uint64_t id : s.layers) {
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
      }
    }
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
  return Transform2D::translate(position) * Transform2D::rotate(rotation * PI / 180.0f) *
         Transform2D::scaling(scale);
}

const char *layer_type_name(LayerType type)
{
  return type == LayerType::Bone    ? "Bone"
         : type == LayerType::Group ? "Group"
         : type == LayerType::Media ? "Media"
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
                                                        {"MEDIA", "Media"}});
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
        def_float_fn<Layer>(
            "media_volume", "Volume", "How loud its sound plays (1: as recorded)", 0.0f, 4.0f,
            [](const Layer &l) { return l.media.volume; }, [](Layer &l, float v) { l.media.volume = v; }),
        def_bool_fn<Layer>(
            "media_muted", "Mute", "Silence its sound", [](const Layer &l) { return l.media.muted; },
            [](Layer &l, bool v) { l.media.muted = v; }),
    };
    return s;
  }();
  return srna;
}

}  // namespace oanim
