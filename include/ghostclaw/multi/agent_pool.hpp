#pragma once

#include "ghostclaw/agent/engine.hpp"
#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ghostclaw::multi {

class AgentPool {
public:
  explicit AgentPool(const config::Config &config);

  [[nodiscard]] common::Result<std::shared_ptr<agent::AgentEngine>>
  get_or_create(const std::string &agent_id);

  [[nodiscard]] bool has_agent(const std::string &id) const;
  [[nodiscard]] bool has_team(const std::string &id) const;
  [[nodiscard]] std::vector<std::string> agent_ids() const;
  [[nodiscard]] std::string team_leader(const std::string &team_id) const;
  [[nodiscard]] std::vector<std::string> team_members(const std::string &team_id) const;

private:
  [[nodiscard]] common::Result<std::shared_ptr<agent::AgentEngine>>
  create_engine(const config::AgentConfig &agent_config);

  const config::Config &config_;
  std::unordered_map<std::string, config::AgentConfig> agent_configs_;
  std::unordered_map<std::string, config::TeamConfig> team_configs_;
  std::unordered_map<std::string, std::shared_ptr<agent::AgentEngine>> engines_;
  mutable std::mutex mutex_;
};

} // namespace ghostclaw::multi
