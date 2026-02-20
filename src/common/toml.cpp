#include "ghostclaw/common/toml.hpp"

#include "ghostclaw/common/fs.hpp"

#include <charconv>
#include <sstream>

namespace ghostclaw::common {

namespace {

std::string strip_comment(const std::string &line) {
  bool in_quotes = false;
  std::string output;
  output.reserve(line.size());

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"' && (i == 0 || line[i - 1] != '\\')) {
      in_quotes = !in_quotes;
    }
    if (!in_quotes && ch == '#') {
      break;
    }
    output.push_back(ch);
  }

  return output;
}

std::vector<std::string> split_array_elements(const std::string &array_value) {
  std::vector<std::string> result;
  std::string current;
  bool in_quotes = false;

  for (std::size_t i = 0; i < array_value.size(); ++i) {
    const char ch = array_value[i];
    if (ch == '"' && (i == 0 || array_value[i - 1] != '\\')) {
      in_quotes = !in_quotes;
      current.push_back(ch);
      continue;
    }

    if (!in_quotes && ch == ',') {
      result.push_back(trim(current));
      current.clear();
      continue;
    }

    current.push_back(ch);
  }

  if (!trim(current).empty()) {
    result.push_back(trim(current));
  }

  return result;
}

std::string unquote(std::string value) {
  value = trim(value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

} // namespace

bool TomlDocument::has(const std::string &key) const { return values.contains(key); }

std::string TomlDocument::get_string(const std::string &key, const std::string &fallback) const {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }
  return unquote(it->second);
}

bool TomlDocument::get_bool(const std::string &key, bool fallback) const {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }
  const std::string normalized = to_lower(trim(it->second));
  if (normalized == "true") {
    return true;
  }
  if (normalized == "false") {
    return false;
  }
  return fallback;
}

int TomlDocument::get_int(const std::string &key, int fallback) const {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }

  const std::string normalized = trim(it->second);
  int parsed = 0;
  const auto *first = normalized.data();
  const auto *last = first + normalized.size();
  auto [ptr, ec] = std::from_chars(first, last, parsed);
  if (ec != std::errc() || ptr != last) {
    return fallback;
  }

  return parsed;
}

std::uint64_t TomlDocument::get_u64(const std::string &key, std::uint64_t fallback) const {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }

  const std::string normalized = trim(it->second);
  std::uint64_t parsed = 0;
  const auto *first = normalized.data();
  const auto *last = first + normalized.size();
  auto [ptr, ec] = std::from_chars(first, last, parsed);
  if (ec != std::errc() || ptr != last) {
    return fallback;
  }

  return parsed;
}

double TomlDocument::get_double(const std::string &key, double fallback) const {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }

  try {
    return std::stod(trim(it->second));
  } catch (...) {
    return fallback;
  }
}

std::vector<std::string>
TomlDocument::get_string_array(const std::string &key,
                               const std::vector<std::string> &fallback) const {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }

  const std::string raw = trim(it->second);
  if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') {
    return fallback;
  }

  const std::string body = raw.substr(1, raw.size() - 2);
  std::vector<std::string> values_out;
  for (const auto &element : split_array_elements(body)) {
    if (!element.empty()) {
      values_out.push_back(unquote(element));
    }
  }

  return values_out;
}

Result<TomlDocument> parse_toml(std::string_view content) {
  TomlDocument document;
  std::string current_section;
  std::size_t line_number = 0;

  std::size_t cursor = 0;
  while (cursor <= content.size()) {
    const auto newline = content.find('\n', cursor);
    const std::size_t line_begin = cursor;
    const std::size_t line_end =
        newline == std::string_view::npos ? content.size() : newline;
    std::string line(content.substr(line_begin, line_end - line_begin));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    ++line_number;
    const std::string clean_line = trim(strip_comment(line));
    if (clean_line.empty()) {
      if (newline == std::string_view::npos) {
        break;
      }
      cursor = newline + 1;
      continue;
    }

    if (clean_line.front() == '[' && clean_line.back() == ']') {
      current_section = trim(clean_line.substr(1, clean_line.size() - 2));
      if (current_section.empty()) {
        return Result<TomlDocument>::failure("Invalid empty section at line " +
                                             std::to_string(line_number));
      }
      if (newline == std::string_view::npos) {
        break;
      }
      cursor = newline + 1;
      continue;
    }

    const std::size_t equals_index = clean_line.find('=');
    if (equals_index == std::string::npos) {
      return Result<TomlDocument>::failure("Invalid key/value at line " +
                                           std::to_string(line_number));
    }

    const std::string key = trim(clean_line.substr(0, equals_index));
    const std::string value = trim(clean_line.substr(equals_index + 1));
    if (key.empty()) {
      return Result<TomlDocument>::failure("Missing key at line " +
                                           std::to_string(line_number));
    }

    const std::string full_key = current_section.empty() ? key : current_section + "." + key;
    document.values[full_key] = value;

    if (newline == std::string_view::npos) {
      break;
    }
    cursor = newline + 1;
  }

  return Result<TomlDocument>::success(std::move(document));
}

Result<TomlDocument> parse_toml(const std::string &content) {
  return parse_toml(std::string_view(content));
}

std::string quote_toml_string(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

} // namespace ghostclaw::common
