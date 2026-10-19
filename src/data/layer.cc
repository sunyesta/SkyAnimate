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
  return mask_mode == MaskMode::SubtractInvisible;
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
  return type == LayerType::Bone ? "Bone" : type == LayerType::Group ? "Group" : "Vector";
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
                                                       {{"VECTOR", "Vector"}, {"GROUP", "Group"}, {"BONE", "Bone"}});
    type_prop.set = nullptr;
    s.properties = {
        def_string<Layer>("name", "Name", "Name shown in the Layers editor", &Layer::name),
        type_prop,
        def_bool<Layer>("visible", "Visible", "Show the layer in the viewport and renders", &Layer::visible),
        def_bool<Layer>("render_visible", "Show in Renders",
                        "Draw the layer in renders; off, it still shows in the viewport", &Layer::render_visible),
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
                                  {{"MASKED", "Mask this layer"},
                                   {"NOT_MASKED", "Don't mask this layer"},
                                   {"ADD", "+ Add to mask"},
                                   {"ADD_INVISIBLE", "+ Add to mask, but keep invisible"},
                                   {"SUBTRACT_INVISIBLE", "- Subtract from mask (this layer will be invisible)"},
                                   {"CLEAR_ADD", "+ Clear the mask, then add this layer to it"},
                                   {"CLEAR_ADD_INVISIBLE", "+ Clear the mask, then add this layer invisibly to it"}}),
        def_enum<Layer, GroupMask>("group_mask", "Group Mask", "Masking for this group's children",
                                   &Layer::group_mask,
                                   {{"NONE", "No masking"},
                                    {"SHOW_ALL", "Reveal all"},
                                    {"HIDE_ALL", "Hide all"}}),
        def_bool<Layer>("mask_expand", "Expand mask by a pixel",
                        "Grow this layer's part of the mask by one pixel, hiding thin seams along its edges",
                        &Layer::mask_expand),
        def_bool<Layer>("mask_exclude_strokes", "Exclude strokes",
                        "Only this layer's fills go into the mask, not its strokes", &Layer::mask_exclude_strokes),
        def_bool<Layer>("select", "Select", "Picked in the Layers editor, for Group With Selection",
                        &Layer::selected),
    };
    return s;
  }();
  return srna;
}

}  // namespace sky
