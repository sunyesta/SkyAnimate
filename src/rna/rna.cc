#include "rna/rna.hh"

#include <algorithm>

namespace sky::rna {

const PropertyRNA *StructRNA::find(std::string_view id) const
{
  for (const PropertyRNA &prop : properties) {
    if (prop.identifier == id) {
      return &prop;
    }
  }
  return nullptr;
}

const char *prop_type_name(PropType type)
{
  switch (type) {
    case PropType::Bool:
      return "bool";
    case PropType::Int:
      return "int";
    case PropType::Float:
      return "float";
    case PropType::String:
      return "str";
  }
  return "unknown";
}

Value clamp(const PropertyRNA &prop, const Value &value)
{
  if (!prop.has_range()) {
    return value;
  }
  if (const int *i = std::get_if<int>(&value)) {
    return std::clamp(*i, int(prop.min), int(prop.max));
  }
  if (const float *f = std::get_if<float>(&value)) {
    return std::clamp(*f, float(prop.min), float(prop.max));
  }
  return value;
}

}  // namespace sky::rna
