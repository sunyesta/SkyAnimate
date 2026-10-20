#include "data/layer.hh"

namespace sky {

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
    };
    return s;
  }();
  return srna;
}

}  // namespace sky
