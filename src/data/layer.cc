#include "data/layer.hh"

namespace sky {

bool Layer::mask_hides_layer() const
{
  return mask_mode == MaskMode::AddInvisible || mask_mode == MaskMode::SubtractInvisible ||
         mask_mode == MaskMode::ClearAddInvisible;
}

bool Layer::mask_adds() const
{
  return mask_mode == MaskMode::Add || mask_mode == MaskMode::AddInvisible || mask_clears();
}

bool Layer::mask_subtracts() const
{
  return mask_mode == MaskMode::Subtract || mask_mode == MaskMode::SubtractInvisible;
}

bool Layer::mask_clears() const
{
  return mask_mode == MaskMode::ClearAdd || mask_mode == MaskMode::ClearAddInvisible;
}

Transform2D Layer::local_transform() const
{
  return Transform2D::translate(position) * Transform2D::rotate(rotation * PI / 180.0f) *
         Transform2D::scaling(scale);
}

const char *layer_type_name(LayerType type)
{
  return type == LayerType::Group ? "Group" : "Vector";
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
                                                       {{"VECTOR", "Vector"}, {"GROUP", "Group"}});
    type_prop.set = nullptr;
    s.properties = {
        def_string<Layer>("name", "Name", "Name shown in the Layers editor", &Layer::name),
        type_prop,
        def_bool<Layer>("visible", "Visible", "Show the layer in the viewport and renders", &Layer::visible),
        def_bool<Layer>("locked", "Locked", "Protect the layer's drawing from editing", &Layer::locked),
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
        def_enum<Layer, MaskMode>("mask_mode", "Masking", "How this layer takes part in its group's mask",
                                  &Layer::mask_mode,
                                  {{"MASKED", "Masked"},
                                   {"NOT_MASKED", "Not Masked"},
                                   {"ADD", "Add to Mask"},
                                   {"SUBTRACT", "Subtract from Mask"},
                                   {"ADD_INVISIBLE", "Add to Mask, Invisible"},
                                   {"SUBTRACT_INVISIBLE", "Subtract from Mask, Invisible"},
                                   {"CLEAR_ADD", "Clear and Add to Mask"},
                                   {"CLEAR_ADD_INVISIBLE", "Clear and Add to Mask, Invisible"}}),
        def_enum<Layer, GroupMask>("group_mask", "Group Mask", "Masking for this group's children",
                                   &Layer::group_mask,
                                   {{"NONE", "No Masking"},
                                    {"SHOW_ALL", "Reveal All"},
                                    {"HIDE_ALL", "Hide All"}}),
    };
    return s;
  }();
  return srna;
}

}  // namespace sky
