#include "ghostclaw/multi/agent_pool.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/memory/memory.hpp"
#include "ghostclaw/observability/factory.hpp"
#include "ghostclaw/observability/global.hpp"
#include "ghostclaw/providers/factory.hpp"
#include "ghostclaw/security/policy.hpp"
#include "ghostclaw/tools/tool_registry.hpp"

#include <filesystem>

namespace ghostclaw::multi {

AgentPool::AgentPool(const config::Config &config) : config_(config) {
  for (const auto &agent : config_.multi.agents) {
    agent_configs_[agent.id] = agent;
  }
  for (const auto &team : config_.multi.teams) {
    team_configs_[team.id] = team;
  }
}

common::Result<std::shared_ptr<agent::AgentEngine>>
AgentPool::get_or_create(const std::string &agent_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto cached = engines_.find(agent_id);
  if (cached != engines_.end()) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::success(cached->second);
  }

  auto config_it = agent_configs_.find(agent_id);
  if (config_it == agent_configs_.end()) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(
        "unknown agent: " + agent_id);
  }

  auto result = create_engine(config_it->second);
  if (!result.ok()) {
    return result;
  }

  engines_[agent_id] = result.value();
  return result;
}

bool AgentPool::has_agent(const std::string &id) const {
  return agent_configs_.find(id) != agent_configs_.end();
}

bool AgentPool::has_team(const std::string &id) const {
  return team_configs_.find(id) != team_configs_.end();
}

std::vector<std::string> AgentPool::agent_ids() const {
  std::vector<std::string> ids;
  ids.reserve(agent_configs_.size());
  for (const auto &[id, cfg] : agent_configs_) {
    ids.push_back(id);
  }
  return ids;
}

std::string AgentPool::team_leader(const std::string &team_id) const {
  auto it = team_configs_.find(team_id);
  if (it == team_configs_.end()) {
    return "";
  }
  return it->second.leader_agent;
}

std::vector<std::string> AgentPool::team_members(const std::string &team_id) const {
  auto it = team_configs_.find(team_id);
  if (it == team_configs_.end()) {
    return {};
  }
  return it->second.agents;
}

common::Result<std::shared_ptr<agent::AgentEngine>>
AgentPool::create_engine(const config::AgentConfig &agent_config) {
  observability::set_global_observer(observability::create_observer(config_));

  // Determine workspace path
  std::filesystem::path workspace_path;
  if (!agent_config.workspace_directory.empty()) {
    workspace_path = std::filesystem::path(
        config::expand_config_path(agent_config.workspace_directory));
  } else {
    auto ws = config::workspace_dir();
    if (!ws.ok()) {
      return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(ws.error());
    }
    workspace_path = ws.value() / "agents" / agent_config.id;
  }

  std::error_code ec;
  std::filesystem::create_directories(workspace_path, ec);
  if (ec) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(
        "failed to create agent workspace: " + ec.message());
  }

  // Determine provider, model, api_key with fallbacks
  const std::string provider_name =
      agent_config.provider.empty() ? config_.default_provider : agent_config.provider;
  const std::optional<std::string> api_key =
      agent_config.api_key.has_value() ? agent_config.api_key : config_.api_key;

  auto provider =
      providers::create_reliable_provider(provider_name, api_key, config_.reliability);
  if (!provider.ok()) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(provider.error());
  }

  auto mem = memory::create_memory(config_, workspace_path);
  if (mem == nullptr) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(
        "failed to create memory backend for agent: " + agent_config.id);
  }

  auto policy = security::SecurityPolicy::from_config(config_);
  if (!policy.ok()) {
    return common::Result<std::shared_ptr<agent::AgentEngine>>::failure(policy.error());
  }
  auto policy_ptr = std::make_shared<security::SecurityPolicy>(std::move(policy.value()));

  auto registry = tools::ToolRegistry::create_full(policy_ptr, mem.get(), config_);

  // Build skill instructions from system prompt
  std::vector<std::string> skill_instructions;
  if (!agent_config.system_prompt.empty()) {
    skill_instructions.push_back(agent_config.system_prompt);
  }

  auto engine = std::make_shared<agent::AgentEngine>(
      config_, provider.value(), std::move(mem), std::move(registry), workspace_path,
      std::move(skill_instructions));

  return common::Result<std::shared_ptr<agent::AgentEngine>>::success(std::move(engine));
}

} // namespace ghostclaw::multi
