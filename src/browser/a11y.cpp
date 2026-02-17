#include "ghostclaw/browser/a11y.hpp"

#include "ghostclaw/common/json_util.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace ghostclaw::browser {

// ---------------------------------------------------------------------------
// RefCache
// ---------------------------------------------------------------------------

void RefCache::populate(const std::vector<A11yNode> &nodes) {
  cache_.clear();
  for (const auto &node : nodes) {
    if (!node.ref.empty() && node.backend_node_id != 0) {
      cache_[node.ref] = node.backend_node_id;
    }
  }
}

std::optional<std::int64_t> RefCache::resolve(const std::string &ref) const {
  const auto it = cache_.find(ref);
  if (it == cache_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void RefCache::clear() { cache_.clear(); }

std::size_t RefCache::size() const { return cache_.size(); }

// ---------------------------------------------------------------------------
// A11yParser helpers
// ---------------------------------------------------------------------------

namespace {

const std::unordered_set<std::string> &ignored_roles() {
  static const std::unordered_set<std::string> roles = {
      "none", "generic", "InlineTextBox", "LineBreak"};
  return roles;
}

const std::unordered_set<std::string> &interactive_roles() {
  static const std::unordered_set<std::string> roles = {
      "button",           "link",     "textbox",  "searchbox",
      "combobox",         "listbox",  "option",   "checkbox",
      "radio",            "switch",   "slider",   "spinbutton",
      "menuitem",         "menuitemcheckbox", "menuitemradio",
      "tab",              "treeitem"};
  return roles;
}

std::string extract_property(const std::string &properties_json,
                             const std::string &prop_name) {
  auto objects = common::json_split_top_level_objects(properties_json);
  for (const auto &obj : objects) {
    std::string name = common::json_get_string(obj, "name");
    if (name == prop_name) {
      std::string val_obj = common::json_get_object(obj, "value");
      if (!val_obj.empty()) {
        std::string val_type = common::json_get_string(val_obj, "type");
        if (val_type == "boolean" || val_type == "booleanOrUndefined") {
          std::string val = common::json_get_string(val_obj, "value");
          if (val.empty()) {
            // Try numeric/literal extraction
            std::string num = common::json_get_number(val_obj, "value");
            return num;
          }
          return val;
        }
        return common::json_get_string(val_obj, "value");
      }
    }
  }
  return "";
}

bool property_is_true(const std::string &properties_json, const std::string &prop_name) {
  std::string val = extract_property(properties_json, prop_name);
  return val == "true";
}

} // namespace

bool A11yParser::is_interactive_role(const std::string &role) {
  return interactive_roles().contains(role);
}

std::string A11yParser::diff_key(const A11yNode &node) {
  return node.role + ":" + node.name + ":" + std::to_string(node.backend_node_id);
}

// ---------------------------------------------------------------------------
// parse_tree
// ---------------------------------------------------------------------------

common::Result<std::vector<A11yNode>>
A11yParser::parse_tree(const std::string &raw_nodes_json) const {
  if (raw_nodes_json.empty() || raw_nodes_json == "[]") {
    return common::Result<std::vector<A11yNode>>::success({});
  }

  auto node_strings = common::json_split_top_level_objects(raw_nodes_json);
  if (node_strings.empty()) {
    return common::Result<std::vector<A11yNode>>::success({});
  }

  // First pass: collect node IDs, child relationships, and raw data
  struct RawNode {
    std::string node_id;
    std::string role;
    std::string name;
    std::string value;
    std::int64_t backend_node_id = 0;
    std::vector<std::string> child_ids;
    std::string properties_json;
    bool ignored = false;
  };

  std::vector<RawNode> raw_nodes;
  raw_nodes.reserve(node_strings.size());

  for (const auto &node_json : node_strings) {
    RawNode raw;

    // Use flat parse to get top-level keys correctly (avoids ambiguity
    // with nested "value" keys inside role/name/value sub-objects).
    auto flat = common::json_parse_flat(node_json);

    raw.node_id = flat.contains("nodeId") ? flat["nodeId"] : "";

    // Extract role — stored as raw JSON object string by json_parse_flat
    auto role_it = flat.find("role");
    if (role_it != flat.end()) {
      raw.role = common::json_get_string(role_it->second, "value");
    }

    // Check if ignored — boolean literal stored as "true"/"false" string
    auto ignored_it = flat.find("ignored");
    if (ignored_it != flat.end() && ignored_it->second == "true") {
      raw.ignored = true;
    }

    // Extract name
    auto name_it = flat.find("name");
    if (name_it != flat.end()) {
      raw.name = common::json_get_string(name_it->second, "value");
    }

    // Extract value
    auto value_it = flat.find("value");
    if (value_it != flat.end()) {
      raw.value = common::json_get_string(value_it->second, "value");
    }

    // Extract backendDOMNodeId
    auto backend_it = flat.find("backendDOMNodeId");
    if (backend_it != flat.end()) {
      try {
        raw.backend_node_id = std::stoll(backend_it->second);
      } catch (...) {
        // ignore
      }
    }

    // Extract childIds
    auto child_it = flat.find("childIds");
    if (child_it != flat.end() && !child_it->second.empty()) {
      // child_it->second is the raw JSON array like ["2","3"]
      // We need to parse it as a string array
      // Use the array string directly with json_get_string_array won't work since
      // it expects a parent object. Parse manually.
      auto &arr_str = child_it->second;
      if (arr_str.front() == '[') {
        std::size_t pos = 1;
        while (pos < arr_str.size()) {
          pos = common::json_skip_ws(arr_str, pos);
          if (pos >= arr_str.size() || arr_str[pos] == ']') break;
          if (arr_str[pos] == ',') { ++pos; continue; }
          if (arr_str[pos] == '"') {
            auto end = common::json_find_string_end(arr_str, pos);
            if (end != std::string::npos && end > pos) {
              raw.child_ids.push_back(arr_str.substr(pos + 1, end - pos - 1));
              pos = end + 1;
            } else {
              break;
            }
          } else {
            ++pos;
          }
        }
      }
    }

    // Extract properties
    auto props_it = flat.find("properties");
    if (props_it != flat.end()) {
      raw.properties_json = props_it->second;
    }

    raw_nodes.push_back(std::move(raw));
  }

  // Build parent map to compute depth
  std::unordered_map<std::string, std::string> parent_map;
  for (const auto &raw : raw_nodes) {
    for (const auto &child_id : raw.child_ids) {
      parent_map[child_id] = raw.node_id;
    }
  }

  // Build node_id -> index map for depth computation
  std::unordered_map<std::string, std::size_t> id_to_index;
  for (std::size_t i = 0; i < raw_nodes.size(); ++i) {
    id_to_index[raw_nodes[i].node_id] = i;
  }

  // Compute depth for each node
  auto compute_depth = [&](const std::string &node_id) -> int {
    int depth = 0;
    std::string current = node_id;
    while (parent_map.contains(current)) {
      current = parent_map[current];
      ++depth;
      if (depth > 100) {
        break; // safety limit
      }
    }
    return depth;
  };

  // Second pass: build A11yNode list, filtering out ignored/generic/empty
  std::vector<A11yNode> result;
  int ref_counter = 0;

  for (const auto &raw : raw_nodes) {
    // Skip ignored nodes
    if (raw.ignored) {
      continue;
    }

    // Skip roles we don't care about
    if (ignored_roles().contains(raw.role)) {
      continue;
    }

    // Skip empty StaticText
    if (raw.role == "StaticText" && raw.name.empty()) {
      continue;
    }

    A11yNode node;
    node.ref = "e" + std::to_string(ref_counter++);
    node.role = raw.role;
    node.name = raw.name;
    node.value = raw.value;
    node.backend_node_id = raw.backend_node_id;
    node.depth = compute_depth(raw.node_id);

    // Extract disabled/focused from properties
    if (!raw.properties_json.empty()) {
      node.disabled = property_is_true(raw.properties_json, "disabled");
      node.focused = property_is_true(raw.properties_json, "focused");
    }

    result.push_back(std::move(node));
  }

  return common::Result<std::vector<A11yNode>>::success(std::move(result));
}

// ---------------------------------------------------------------------------
// filter_interactive
// ---------------------------------------------------------------------------

std::vector<A11yNode>
A11yParser::filter_interactive(const std::vector<A11yNode> &nodes) const {
  std::vector<A11yNode> result;
  int ref_counter = 0;
  for (const auto &node : nodes) {
    if (is_interactive_role(node.role)) {
      A11yNode copy = node;
      copy.ref = "e" + std::to_string(ref_counter++);
      result.push_back(std::move(copy));
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// filter_depth
// ---------------------------------------------------------------------------

std::vector<A11yNode>
A11yParser::filter_depth(const std::vector<A11yNode> &nodes, int max_depth) const {
  std::vector<A11yNode> result;
  for (const auto &node : nodes) {
    if (node.depth <= max_depth) {
      result.push_back(node);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// compute_diff
// ---------------------------------------------------------------------------

SnapshotDiff A11yParser::compute_diff(const std::vector<A11yNode> &prev,
                                      const std::vector<A11yNode> &current) const {
  SnapshotDiff diff;

  std::unordered_map<std::string, const A11yNode *> prev_map;
  for (const auto &node : prev) {
    prev_map[diff_key(node)] = &node;
  }

  std::unordered_map<std::string, const A11yNode *> curr_map;
  for (const auto &node : current) {
    curr_map[diff_key(node)] = &node;
  }

  // Find added and changed
  for (const auto &node : current) {
    const std::string key = diff_key(node);
    auto it = prev_map.find(key);
    if (it == prev_map.end()) {
      diff.added.push_back(node);
    } else {
      const A11yNode &old = *it->second;
      if (old.value != node.value || old.focused != node.focused ||
          old.disabled != node.disabled) {
        diff.changed.push_back(node);
      }
    }
  }

  // Find removed
  for (const auto &node : prev) {
    const std::string key = diff_key(node);
    if (!curr_map.contains(key)) {
      diff.removed.push_back(node);
    }
  }

  return diff;
}

// ---------------------------------------------------------------------------
// format_text
// ---------------------------------------------------------------------------

std::string A11yParser::format_text(const std::vector<A11yNode> &nodes) const {
  std::ostringstream out;
  for (const auto &node : nodes) {
    for (int i = 0; i < node.depth; ++i) {
      out << "  ";
    }
    out << node.ref << " " << node.role;
    if (!node.name.empty()) {
      out << " \"" << node.name << "\"";
    }
    if (!node.value.empty()) {
      out << " val=\"" << node.value << "\"";
    }
    if (node.focused) {
      out << " focused";
    }
    if (node.disabled) {
      out << " disabled";
    }
    out << "\n";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// format_json
// ---------------------------------------------------------------------------

std::string A11yParser::format_json(const std::vector<A11yNode> &nodes) const {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    const auto &n = nodes[i];
    out << "{";
    out << "\"ref\":\"" << common::json_escape(n.ref) << "\",";
    out << "\"role\":\"" << common::json_escape(n.role) << "\",";
    out << "\"name\":\"" << common::json_escape(n.name) << "\",";
    out << "\"depth\":" << n.depth << ",";
    out << "\"value\":\"" << common::json_escape(n.value) << "\",";
    out << "\"disabled\":" << (n.disabled ? "true" : "false") << ",";
    out << "\"focused\":" << (n.focused ? "true" : "false") << ",";
    out << "\"backendNodeId\":" << n.backend_node_id;
    out << "}";
  }
  out << "]";
  return out.str();
}

} // namespace ghostclaw::browser
