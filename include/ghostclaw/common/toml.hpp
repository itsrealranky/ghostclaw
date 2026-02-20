#pragma once

#include "ghostclaw/common/result.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ghostclaw::common {

struct TomlDocument {
  std::unordered_map<std::string, std::string> values;

  [[nodiscard]] bool has(const std::string &key) const;
  [[nodiscard]] std::string get_string(const std::string &key,
                                       const std::string &fallback = "") const;
  [[nodiscard]] bool get_bool(const std::string &key, bool fallback) const;
  [[nodiscard]] int get_int(const std::string &key, int fallback) const;
  [[nodiscard]] std::uint64_t get_u64(const std::string &key, std::uint64_t fallback) const;
  [[nodiscard]] double get_double(const std::string &key, double fallback) const;
  [[nodiscard]] std::vector<std::string>
  get_string_array(const std::string &key, const std::vector<std::string> &fallback = {}) const;
};

[[nodiscard]] Result<TomlDocument> parse_toml(const std::string &content);
[[nodiscard]] Result<TomlDocument> parse_toml(std::string_view content);
[[nodiscard]] std::string quote_toml_string(const std::string &value);

} // namespace ghostclaw::common
