#include "ghostclaw/tools/policy.hpp"

#include "ghostclaw/common/fs.hpp"

#include <unordered_map>

namespace ghostclaw::tools {

std::vector<std::string> ToolPolicy::expand_group(const std::string_view group) {
  static const std::unordered_map<std::string, std::vector<std::string>> mapping = {
      {"fs", {"file_read", "file_write", "file_edit"}},
      {"runtime", {"shell", "process_bg"}},
      {"memory", {"memory_store", "memory_recall", "memory_forget"}},
      {"web", {"web_search", "web_fetch", "browser"}},
      {"ui", {"browser", "canvas"}},
      {"sessions",
       {"sessions_list", "sessions_history", "sessions_send", "sessions_spawn", "subagents",
        "skills"}},
      {"skills", {"skills", "skill_discover", "skill_auto_install", "skill_create"}},
      {"calendar", {"calendar", "reminder"}},
      {"messaging", {"message", "email", "notify"}},
      // Conway Cloud group — matches mcp_conway_* tools by prefix convention
      // Individual Conway tools can be added explicitly: mcp_conway_sandbox_create, etc.
      {"conway",
       {"mcp_conway_sandbox_create", "mcp_conway_sandbox_exec", "mcp_conway_sandbox_delete",
        "mcp_conway_sandbox_stop", "mcp_conway_sandbox_list", "mcp_conway_pty_create",
        "mcp_conway_pty_exec", "mcp_conway_pty_close", "mcp_conway_inference_chat",
        "mcp_conway_domain_register", "mcp_conway_domain_list", "mcp_conway_domain_delete",
        "mcp_conway_domain_dns_list", "mcp_conway_domain_dns_add", "mcp_conway_domain_dns_remove",
        "mcp_conway_credits_balance", "mcp_conway_wallet_info",
        "mcp_conway_x402_fetch", "mcp_conway_x402_check"}},
      {"soul", {"soul_update", "soul_reflect", "soul_read"}},
      {"profiler", {"tool_profile_report", "self_optimize"}},
  };

  const auto it = mapping.find(common::to_lower(std::string(group)));
  if (it == mapping.end()) {
    return {};
  }
  return it->second;
}

ToolPolicy::ToolPolicy(const std::vector<std::string> &allow_groups,
                       const std::vector<std::string> &allow_tools,
                       const std::vector<std::string> &deny_tools) {
  for (const auto &group : allow_groups) {
    for (const auto &tool : expand_group(group)) {
      allowed_.insert(tool);
    }
  }

  for (const auto &tool : allow_tools) {
    allowed_.insert(common::to_lower(tool));
  }

  for (const auto &tool : deny_tools) {
    denied_.insert(common::to_lower(tool));
  }
}

bool ToolPolicy::is_allowed(const std::string_view tool_name) const {
  const std::string normalized = common::to_lower(std::string(tool_name));
  if (denied_.contains(normalized)) {
    return false;
  }
  if (allowed_.empty()) {
    return true;
  }
  return allowed_.contains(normalized);
}

} // namespace ghostclaw::tools
