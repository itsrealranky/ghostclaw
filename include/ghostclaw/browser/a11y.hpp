#pragma once

#include "ghostclaw/common/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ghostclaw::browser {

struct A11yNode {
  std::string ref;
  std::string role;
  std::string name;
  int depth = 0;
  std::string value;
  bool disabled = false;
  bool focused = false;
  std::int64_t backend_node_id = 0;
};

enum class SnapshotFilter { None, Interactive };

struct SnapshotOptions {
  SnapshotFilter filter = SnapshotFilter::None;
  int max_depth = 0;
  bool diff = false;
  bool text_format = true;
};

struct SnapshotDiff {
  std::vector<A11yNode> added;
  std::vector<A11yNode> removed;
  std::vector<A11yNode> changed;
};

class RefCache {
public:
  void populate(const std::vector<A11yNode> &nodes);
  [[nodiscard]] std::optional<std::int64_t> resolve(const std::string &ref) const;
  void clear();
  [[nodiscard]] std::size_t size() const;

private:
  std::unordered_map<std::string, std::int64_t> cache_;
};

class A11yParser {
public:
  [[nodiscard]] common::Result<std::vector<A11yNode>>
  parse_tree(const std::string &raw_nodes_json) const;

  [[nodiscard]] std::vector<A11yNode>
  filter_interactive(const std::vector<A11yNode> &nodes) const;

  [[nodiscard]] std::vector<A11yNode>
  filter_depth(const std::vector<A11yNode> &nodes, int max_depth) const;

  [[nodiscard]] SnapshotDiff
  compute_diff(const std::vector<A11yNode> &prev,
               const std::vector<A11yNode> &current) const;

  [[nodiscard]] std::string format_text(const std::vector<A11yNode> &nodes) const;
  [[nodiscard]] std::string format_json(const std::vector<A11yNode> &nodes) const;

private:
  [[nodiscard]] static bool is_interactive_role(const std::string &role);
  [[nodiscard]] static std::string diff_key(const A11yNode &node);
};

} // namespace ghostclaw::browser
