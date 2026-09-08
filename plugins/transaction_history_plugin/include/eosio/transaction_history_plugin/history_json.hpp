#pragma once

#include <fc/variant_object.hpp>
#include <map>

namespace eosio {

// FC serializes std::map as [[key,value], ...], not a JSON object. Accept
// those legacy history rows on read, but write new storage rows as objects.
inline fc::mutable_variant_object history_record_object(const std::map<std::string, fc::variant>& fields) {
   fc::mutable_variant_object result;
   for (const auto& [key, field] : fields) result(key, field);
   return result;
}

inline fc::mutable_variant_object history_record_object(const fc::variant& value) {
   if (value.is_object()) return fc::mutable_variant_object(value.get_object());
   return history_record_object(value.as<std::map<std::string, fc::variant>>());
}

inline std::map<std::string, fc::variant> history_record_fields(const fc::variant& value) {
   if (!value.is_object()) return value.as<std::map<std::string, fc::variant>>();
   std::map<std::string, fc::variant> result;
   for (const auto& field : value.get_object()) result.emplace(field.key(), field.value());
   return result;
}

} // namespace eosio
