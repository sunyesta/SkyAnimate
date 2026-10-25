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

namespace oanim::rna {

enum class PropType { Bool, Int, Float, String, Enum };

/* Enum properties hold the item index as an int. */
using Value = std::variant<bool, int, float, std::string>;

struct EnumItem {
  /* Python name, e.g. "ADD". */
  std::string identifier;
  /* UI label, e.g. "Add to Mask". */
  std::string name;
};

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
  /* Items of an Enum property. */
  std::vector<EnumItem> items;

  bool has_range() const { return min <= max; }
  /* Index of an enum identifier, or -1. */
  int enum_index(std::string_view identifier) const;
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

/* An enum backed by an enum-class member; the item order matches the enum's values. */
template<typename T, typename E>
PropertyRNA def_enum(const char *id, const char *name, const char *desc, E T::*member, std::vector<EnumItem> items)
{
  PropertyRNA prop{id, name, desc, PropType::Enum};
  prop.items = std::move(items);
  prop.get = [member](const void *data) -> Value { return int(static_cast<const T *>(data)->*member); };
  prop.set = [member](void *data, const Value &v) { static_cast<T *>(data)->*member = E(std::get<int>(v)); };
  return prop;
}

/* A float computed or stored somewhere other than a plain member. */
template<typename T>
PropertyRNA def_float_fn(const char *id, const char *name, const char *desc, float min, float max,
                         std::function<float(const T &)> get, std::function<void(T &, float)> set)
{
  PropertyRNA prop{id, name, desc, PropType::Float, double(min), double(max)};
  prop.get = [get](const void *data) -> Value { return get(*static_cast<const T *>(data)); };
  if (set) {
    prop.set = [set](void *data, const Value &v) { set(*static_cast<T *>(data), std::get<float>(v)); };
  }
  return prop;
}

/* A bool stored somewhere other than a plain member (a nested struct's). */
template<typename T>
PropertyRNA def_bool_fn(const char *id, const char *name, const char *desc, std::function<bool(const T &)> get,
                        std::function<void(T &, bool)> set)
{
  PropertyRNA prop{id, name, desc, PropType::Bool};
  prop.get = [get](const void *data) -> Value { return get(*static_cast<const T *>(data)); };
  if (set) {
    prop.set = [set](void *data, const Value &v) { set(*static_cast<T *>(data), std::get<bool>(v)); };
  }
  return prop;
}

/* An enum (item index) stored somewhere other than a plain member. */
template<typename T>
PropertyRNA def_enum_fn(const char *id, const char *name, const char *desc, std::vector<EnumItem> items,
                        std::function<int(const T &)> get, std::function<void(T &, int)> set)
{
  PropertyRNA prop{id, name, desc, PropType::Enum};
  prop.items = std::move(items);
  prop.get = [get](const void *data) -> Value { return get(*static_cast<const T *>(data)); };
  if (set) {
    prop.set = [set](void *data, const Value &v) { set(*static_cast<T *>(data), std::get<int>(v)); };
  }
  return prop;
}

/* A read-only int computed from the data, such as a count. */
template<typename T>
PropertyRNA def_int_readonly(const char *id, const char *name, const char *desc, std::function<int(const T &)> get)
{
  PropertyRNA prop{id, name, desc, PropType::Int};
  prop.get = [get](const void *data) -> Value { return get(*static_cast<const T *>(data)); };
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

/* A string computed or stored somewhere other than a plain member. */
template<typename T>
PropertyRNA def_string_fn(const char *id, const char *name, const char *desc,
                          std::function<std::string(const T &)> get, std::function<void(T &, const std::string &)> set)
{
  PropertyRNA prop{id, name, desc, PropType::String};
  prop.get = [get](const void *data) -> Value { return get(*static_cast<const T *>(data)); };
  if (set) {
    prop.set = [set](void *data, const Value &v) { set(*static_cast<T *>(data), std::get<std::string>(v)); };
  }
  return prop;
}

}  // namespace oanim::rna
