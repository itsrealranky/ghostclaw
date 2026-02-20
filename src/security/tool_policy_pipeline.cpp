#include "ghostclaw/security/tool_policy.hpp"

#include "ghostclaw/common/fs.hpp"

#include <algorithm>
#include <regex>
#include <unordered_set>

namespace ghostclaw::security {

namespace {

std::string normalize_key(std::string_view value) {
  return common::to_lower(common::trim(std::string(value)));
}

bool has_glob_chars(std::string_view value) {
  return value.find('*') != std::string_view::npos || value.find('?') != std::string_view::npos;
}

std::string glob_to_regex(std::string_view pattern) {
  std::string out;
  out.reserve(pattern.size() * 2 + 4);
  out += '^';
  for (char ch : pattern) {
    switch (ch) {
    case '*':
      out += ".*";
      break;
    case '?':
      out += '.';
      break;
    case '.':
    case '+':
    case '^':
    case '$':
    case '(': case ')':
    case '[': case ']':
    case '{': case '}':
    case '|':
    case '\\':
      out += '\\';
      out += ch;
      break;
    default:
      out += ch;
      break;
    }
  }
  out += '$';
  return out;
}

std::string profile_name_for_label(ToolProfile profile) {
  switch (profile) {
  case ToolProfile::Minimal:
    return "minimal";
  case ToolProfile::Coding:
    return "coding";
  case ToolProfile::Messaging:
    return "messaging";
  case ToolProfile::Full:
    return "full";
  }
  return "full";
}

} // namespace

bool ToolPolicy::empty() const { return allow.empty() && deny.empty(); }

ToolPolicyPipeline::ToolPolicyPipeline() {
  profile_policies_.insert({ToolProfile::Minimal, default_profile_policy(ToolProfile::Minimal)});
  profile_policies_.insert({ToolProfile::Coding, default_profile_policy(ToolProfile::Coding)});
  profile_policies_.insert({ToolProfile::Messaging, default_profile_policy(ToolProfile::Messaging)});
  profile_policies_.insert({ToolProfile::Full, default_profile_policy(ToolProfile::Full)});
}

void ToolPolicyPipeline::set_profile_policy(const ToolProfile profile, ToolPolicy policy) {
  profile_policies_[profile] = std::move(policy);
}

void ToolPolicyPipeline::set_provider_profile_policy(const std::string &provider,
                                                     const ToolProfile profile,
                                                     ToolPolicy policy) {
  const auto key = normalize_key(provider);
  if (key.empty()) {
    return;
  }
  provider_profile_policies_[key][profile] = std::move(policy);
}

void ToolPolicyPipeline::set_global_policy(ToolPolicy policy) {
  global_policy_ = std::move(policy);
}

void ToolPolicyPipeline::clear_global_policy() { global_policy_ = std::nullopt; }

void ToolPolicyPipeline::set_global_provider_policy(const std::string &provider, ToolPolicy policy) {
  const auto key = normalize_key(provider);
  if (key.empty()) {
    return;
  }
  global_provider_policies_[key] = std::move(policy);
}

void ToolPolicyPipeline::set_agent_policy(const std::string &agent_id, ToolPolicy policy) {
  const auto key = normalize_key(agent_id);
  if (key.empty()) {
    return;
  }
  agent_policies_[key] = std::move(policy);
}

void ToolPolicyPipeline::set_agent_provider_policy(const std::string &agent_id,
                                                   const std::string &provider,
                                                   ToolPolicy policy) {
  const auto agent_key = normalize_key(agent_id);
  const auto provider_key = normalize_key(provider);
  if (agent_key.empty() || provider_key.empty()) {
    return;
  }
  agent_provider_policies_[agent_key][provider_key] = std::move(policy);
}

void ToolPolicyPipeline::set_group_policy(const std::string &channel_id,
                                          const std::string &group_id,
                                          ToolPolicy policy) {
  const auto key = make_group_key(channel_id, group_id);
  if (key.empty()) {
    return;
  }
  group_policies_[key] = std::move(policy);
}

ToolPolicyDecision ToolPolicyPipeline::evaluate_tool(const ToolPolicyRequest &request) const {
  return evaluate_tool(request.tool_name, request);
}

ToolPolicyDecision ToolPolicyPipeline::evaluate_tool(std::string_view tool_name,
                                                     const ToolPolicyRequest &request) const {
  ToolPolicyDecision decision;
  const std::string normalized_tool = normalize_tool_name(tool_name);
  if (normalized_tool.empty()) {
    decision.allowed = false;
    decision.blocked_by = "tools.profile";
    decision.reason = "empty tool name";
    decision.trace.push_back("tools.profile: denied (empty tool name)");
    return decision;
  }

  const auto steps = build_default_pipeline_steps(request);
  for (const auto &step : steps) {
    if (!step.policy.has_value()) {
      continue;
    }

    const auto deny = expand_entries(step.policy->deny);
    for (const auto &entry : deny) {
      if (matches_pattern(normalized_tool, entry)) {
        decision.allowed = false;
        decision.blocked_by = step.label;
        decision.reason = "matched deny entry: " + entry;
        decision.trace.push_back(step.label + ": denied (" + decision.reason + ")");
        return decision;
      }
    }

    const auto allow = expand_entries(step.policy->allow);
    if (!allow.empty()) {
      const bool allowed =
          std::any_of(allow.begin(), allow.end(), [&](const std::string &entry) {
            return matches_pattern(normalized_tool, entry);
          });
      if (!allowed) {
        decision.allowed = false;
        decision.blocked_by = step.label;
        decision.reason = "not matched by allowlist";
        decision.trace.push_back(step.label + ": denied (" + decision.reason + ")");
        return decision;
      }
    }

    decision.trace.push_back(step.label + ": allow");
  }

  decision.allowed = true;
  decision.trace.push_back("decision: allow");
  return decision;
}

std::vector<std::string> ToolPolicyPipeline::filter_tools(const std::vector<std::string> &tool_names,
                                                          const ToolPolicyRequest &request) const {
  std::vector<std::string> filtered;
  filtered.reserve(tool_names.size());
  for (const auto &name : tool_names) {
    ToolPolicyRequest req = request;
    req.tool_name = name;
    if (evaluate_tool(req).allowed) {
      filtered.push_back(name);
    }
  }
  return filtered;
}

std::vector<ToolPolicyPipelineStep>
ToolPolicyPipeline::build_default_pipeline_steps(const ToolPolicyRequest &request) const {
  std::vector<ToolPolicyPipelineStep> steps;
  steps.reserve(7);

  const std::string provider = normalize_key(request.provider);
  const std::string agent_id = normalize_key(request.agent_id);

  const auto profile_it = profile_policies_.find(request.profile);
  steps.push_back(
      {profile_it == profile_policies_.end() ? std::optional<ToolPolicy>{}
                                             : std::optional<ToolPolicy>{profile_it->second},
       "tools.profile (" + profile_name_for_label(request.profile) + ")"});

  std::optional<ToolPolicy> provider_profile;
  if (!provider.empty()) {
    const auto provider_it = provider_profile_policies_.find(provider);
    if (provider_it != provider_profile_policies_.end()) {
      const auto profile_policy = provider_it->second.find(request.profile);
      if (profile_policy != provider_it->second.end()) {
        provider_profile = profile_policy->second;
      }
    }
  }
  steps.push_back(
      {provider_profile,
       provider.empty() ? "tools.byProvider.profile"
                        : "tools.byProvider.profile (" + provider + ", " +
                              profile_name_for_label(request.profile) + ")"});

  steps.push_back({global_policy_, "tools.allow"});

  std::optional<ToolPolicy> global_provider;
  if (!provider.empty()) {
    const auto it = global_provider_policies_.find(provider);
    if (it != global_provider_policies_.end()) {
      global_provider = it->second;
    }
  }
  steps.push_back(
      {global_provider,
       provider.empty() ? "tools.byProvider.allow"
                        : "tools.byProvider.allow (" + provider + ")"});

  std::optional<ToolPolicy> agent_policy;
  if (!agent_id.empty()) {
    const auto it = agent_policies_.find(agent_id);
    if (it != agent_policies_.end()) {
      agent_policy = it->second;
    }
  }
  steps.push_back({agent_policy,
                   agent_id.empty() ? "agents.{id}.tools.allow"
                                    : "agents." + agent_id + ".tools.allow"});

  std::optional<ToolPolicy> agent_provider;
  if (!agent_id.empty() && !provider.empty()) {
    const auto agent_it = agent_provider_policies_.find(agent_id);
    if (agent_it != agent_provider_policies_.end()) {
      const auto provider_it = agent_it->second.find(provider);
      if (provider_it != agent_it->second.end()) {
        agent_provider = provider_it->second;
      }
    }
  }
  steps.push_back({agent_provider,
                   agent_id.empty() ? "agents.{id}.tools.byProvider.allow"
                                    : "agents." + agent_id + ".tools.byProvider.allow"});

  std::optional<ToolPolicy> group_policy;
  const auto group_key = make_group_key(request.channel_id, request.group_id);
  if (!group_key.empty()) {
    const auto it = group_policies_.find(group_key);
    if (it != group_policies_.end()) {
      group_policy = it->second;
    }
  }

  std::string group_label = "group/channel tools.allow";
  if (!request.channel_id.empty() || !request.group_id.empty()) {
    group_label = "group/channel tools.allow (" + normalize_key(request.channel_id) + "/" +
                  normalize_key(request.group_id) + ")";
  }
  steps.push_back({group_policy, group_label});

  return steps;
}

std::vector<std::string>
ToolPolicyPipeline::expand_entries(const std::vector<std::string> &entries) {
  std::vector<std::string> expanded;
  std::unordered_set<std::string> seen;

  for (const auto &raw : entries) {
    const auto trimmed = common::trim(raw);
    if (trimmed.empty()) {
      continue;
    }

    const auto group = expand_group(trimmed);
    if (!group.empty()) {
      for (const auto &name : group) {
        if (seen.insert(name).second) {
          expanded.push_back(name);
        }
      }
      continue;
    }

    const auto pattern = normalize_pattern(trimmed);
    if (pattern.empty()) {
      continue;
    }
    if (seen.insert(pattern).second) {
      expanded.push_back(pattern);
    }
  }

  return expanded;
}

std::vector<std::string> ToolPolicyPipeline::expand_group(std::string_view group_name) {
  std::string key = normalize_key(group_name);
  if (key.empty()) {
    return {};
  }

  if (key.rfind("group:", 0) != 0) {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"fs", "group:fs"},
        {"runtime", "group:runtime"},
        {"memory", "group:memory"},
        {"sessions", "group:sessions"},
        {"skills", "group:skills"},
        {"ui", "group:ui"},
        {"automation", "group:automation"},
        {"messaging", "group:messaging"},
        {"calendar", "group:calendar"},
        {"web", "group:web"},
        {"conway", "group:conway"},
        {"soul", "group:soul"},
        {"profiler", "group:profiler"},
    };
    const auto alias_it = aliases.find(key);
    if (alias_it != aliases.end()) {
      key = alias_it->second;
    }
  }

  static const std::unordered_map<std::string, std::vector<std::string>> groups = {
      {"group:fs", {"read", "write", "edit"}},
      {"group:runtime", {"exec", "process"}},
      {"group:memory", {"memory_store", "memory_recall", "memory_forget"}},
      {"group:sessions", {"sessions", "subagents", "skills"}},
      {"group:skills", {"skills", "skill_discover", "skill_auto_install", "skill_create"}},
      {"group:ui", {"browser", "canvas"}},
      {"group:automation", {"cron", "gateway"}},
      {"group:messaging", {"message", "email", "notify", "reminder"}},
      {"group:calendar", {"calendar", "reminder"}},
      {"group:web", {"web_search", "web_fetch"}},
      // Conway Cloud tools — use glob mcp_conway_* to match all Conway MCP tools
      {"group:conway", {"mcp_conway_*"}},
      {"group:soul", {"soul_update", "soul_reflect", "soul_read"}},
      {"group:profiler", {"tool_profile_report", "self_optimize"}},
  };

  const auto it = groups.find(key);
  if (it == groups.end()) {
    return {};
  }
  return it->second;
}

std::string ToolPolicyPipeline::normalize_tool_name(std::string_view name) {
  const std::string normalized = normalize_key(name);
  if (normalized.empty()) {
    return "";
  }

  static const std::unordered_map<std::string, std::string> aliases = {
      {"file_read", "read"},
      {"file_write", "write"},
      {"file_edit", "edit"},
      {"shell", "exec"},
      {"bash", "exec"},
      {"process_bg", "process"},
      {"sessions_list", "sessions"},
      {"sessions_history", "sessions"},
      {"sessions_send", "sessions"},
      {"sessions_spawn", "sessions"},
      {"session_list", "sessions"},
      {"session_fork", "subagents"},
      {"apply-patch", "edit"},
      {"calendar", "calendar"},
      {"email", "email"},
      {"notify", "notify"},
      {"reminder", "reminder"},
      {"message", "message"},
  };

  const auto it = aliases.find(normalized);
  if (it != aliases.end()) {
    return it->second;
  }

  return normalized;
}

common::Result<ToolProfile> ToolPolicyPipeline::profile_from_string(const std::string &value) {
  const std::string normalized = normalize_key(value);
  if (normalized == "minimal") {
    return common::Result<ToolProfile>::success(ToolProfile::Minimal);
  }
  if (normalized == "coding") {
    return common::Result<ToolProfile>::success(ToolProfile::Coding);
  }
  if (normalized == "messaging") {
    return common::Result<ToolProfile>::success(ToolProfile::Messaging);
  }
  if (normalized == "full") {
    return common::Result<ToolProfile>::success(ToolProfile::Full);
  }
  return common::Result<ToolProfile>::failure("unknown tool profile: " + value);
}

std::string ToolPolicyPipeline::profile_to_string(const ToolProfile profile) {
  switch (profile) {
  case ToolProfile::Minimal:
    return "minimal";
  case ToolProfile::Coding:
    return "coding";
  case ToolProfile::Messaging:
    return "messaging";
  case ToolProfile::Full:
    return "full";
  }
  return "full";
}

ToolPolicy ToolPolicyPipeline::default_profile_policy(const ToolProfile profile) {
  switch (profile) {
  case ToolProfile::Minimal:
    return ToolPolicy{.allow = {"read"}, .deny = {}};
  case ToolProfile::Coding:
    return ToolPolicy{.allow = {"group:fs", "group:runtime", "group:sessions", "group:web"},
                      .deny = {}};
  case ToolProfile::Messaging:
    return ToolPolicy{.allow = {"group:messaging", "group:sessions", "group:web"}, .deny = {}};
  case ToolProfile::Full:
    return ToolPolicy{};
  }
  return ToolPolicy{};
}

std::string ToolPolicyPipeline::make_group_key(std::string_view channel_id,
                                               std::string_view group_id) {
  const std::string channel = normalize_key(channel_id);
  const std::string group = normalize_key(group_id);
  if (channel.empty() && group.empty()) {
    return "";
  }
  return channel + "::" + group;
}

bool ToolPolicyPipeline::matches_pattern(std::string_view name, std::string_view pattern) {
  const std::string normalized_name = normalize_tool_name(name);
  const std::string normalized_pattern = normalize_pattern(pattern);
  if (normalized_name.empty() || normalized_pattern.empty()) {
    return false;
  }
  if (normalized_pattern == "*") {
    return true;
  }

  if (!has_glob_chars(normalized_pattern)) {
    return normalized_name == normalize_tool_name(normalized_pattern);
  }

  try {
    const std::regex re(glob_to_regex(normalized_pattern));
    return std::regex_match(normalized_name, re);
  } catch (const std::exception &) {
    return false;
  }
}

std::string ToolPolicyPipeline::normalize_pattern(std::string_view pattern) {
  const std::string normalized = normalize_key(pattern);
  if (normalized.empty()) {
    return "";
  }
  if (has_glob_chars(normalized)) {
    return normalized;
  }
  return normalize_tool_name(normalized);
}

} // namespace ghostclaw::security
