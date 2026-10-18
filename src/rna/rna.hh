#pragma once

/* RNA: property reflection, a much smaller version of Blender's makesrna.
 *
 * Each data type describes its properties once (name, type, range, getter, setter).
 * That one description drives the Properties editor, the Python API and, later,
 * animation paths. */

#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sky::rna {

enum class PropType { Bool, Int, Float, String };

using Value = std::variant<bool, int, float, std::string>;

struct PropertyRNA {
  std::string identifier;
  std::string name;
  std::string description;
  PropType type = PropType::Int;
  /* Range for Int and Float properties. Ignored when min > max. */
  double min = 1.0;
  double max = 0.0;
  std::function<Value(const void *data)> get;
  /* Null for read-only properties. */
  std::function<void(void *data, const Value &value)> set;

  bool has_range() const { return min <= max; }
};

struct StructRNA {
  std::string identifier;
  std::string name;
  std::vector<PropertyRNA> properties;

  const PropertyRNA *find(std::string_view identifier) const;
};

/* Pointer to data plus its type, like Blender's PointerRNA. */
struct PointerRNA {
  void *data = nullptr;
  const StructRNA *type = nullptr;
};

const char *prop_type_name(PropType type);

/* Clamps a value to the property's range. */
Value clamp(const PropertyRNA &prop, const Value &value);

/* Helpers to define properties backed by plain struct members. */

template<typename T> PropertyRNA def_bool(const char *id, const char *name, const char *desc, bool T::*member)
{
  PropertyRNA prop{id, name, desc, PropType::Bool};
  prop.get = [member](const void *data) -> Value { return static_cast<const T *>(data)->*member; };
  prop.set = [member](void *data, const Value &v) { static_cast<T *>(data)->*member = std::get<bool>(v); };
  return prop;
}

template<typename T>
PropertyRNA def_int(const char *id, const char *name, const char *desc, int T::*member, int min, int max)
{
  PropertyRNA prop{id, name, desc, PropType::Int, double(min), double(max)};
  prop.get = [member](const void *data) -> Value { return static_cast<const T *>(data)->*member; };
  prop.set = [member](void *data, const Value &v) { static_cast<T *>(data)->*member = std::get<int>(v); };
  return prop;
}

template<typename T>
PropertyRNA def_float(const char *id, const char *name, const char *desc, float T::*member, float min, float max)
{
  PropertyRNA prop{id, name, desc, PropType::Float, double(min), double(max)};
  prop.get = [member](const void *data) -> Value { return static_cast<const T *>(data)->*member; };
  prop.set = [member](void *data, const Value &v) { static_cast<T *>(data)->*member = std::get<float>(v); };
  return prop;
}

template<typename T>
PropertyRNA def_string(const char *id, const char *name, const char *desc, std::string T::*member)
{
  PropertyRNA prop{id, name, desc, PropType::String};
  prop.get = [member](const void *data) -> Value { return static_cast<const T *>(data)->*member; };
  prop.set = [member](void *data, const Value &v) {
    static_cast<T *>(data)->*member = std::get<std::string>(v);
  };
  return prop;
}

}  // namespace sky::rna
