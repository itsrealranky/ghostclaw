#include "test_framework.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/multi/agent_pool.hpp"
#include "ghostclaw/multi/types.hpp"

#include <filesystem>
#include <fstream>

namespace {

struct ConfigOverrideGuard {
  std::optional<std::filesystem::path> old_override;

  explicit ConfigOverrideGuard(std::optional<std::filesystem::path> next = std::nullopt) {
    old_override = ghostclaw::config::config_path_override();
    if (next.has_value()) {
      ghostclaw::config::set_config_path_override(*next);
    } else {
      ghostclaw::config::clear_config_path_override();
    }
  }

  ~ConfigOverrideGuard() {
    if (old_override.has_value()) {
      ghostclaw::config::set_config_path_override(*old_override);
    } else {
      ghostclaw::config::clear_config_path_override();
    }
  }
};

} // namespace

void register_multi_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;

  // ---- extract_mentions tests ----

  tests.push_back({"multi_extract_mentions_basic", [] {
    auto matches = ghostclaw::multi::extract_mentions("Please [@reviewer: check this code]");
    require(matches.size() == 1, "expected 1 mention");
    require(matches[0].target_agent_id == "reviewer", "target should be reviewer");
    require(matches[0].message == "check this code", "message mismatch");
    require(matches[0].start_pos == 7, "start_pos mismatch");
  }});

  tests.push_back({"multi_extract_mentions_multiple", [] {
    auto matches = ghostclaw::multi::extract_mentions(
        "[@coder: fix bug] and [@reviewer: review it]");
    require(matches.size() == 2, "expected 2 mentions");
    require(matches[0].target_agent_id == "coder", "first target should be coder");
    require(matches[0].message == "fix bug", "first message mismatch");
    require(matches[1].target_agent_id == "reviewer", "second target should be reviewer");
    require(matches[1].message == "review it", "second message mismatch");
  }});

  tests.push_back({"multi_extract_mentions_empty", [] {
    auto matches = ghostclaw::multi::extract_mentions("no mentions here");
    require(matches.empty(), "expected no mentions");
  }});

  tests.push_back({"multi_extract_mentions_nested", [] {
    // Nested brackets should be handled gracefully - inner content stops at first ]
    auto matches = ghostclaw::multi::extract_mentions("[@agent: hello [world]]");
    // The regex matches up to the first ], so "hello [world" is the content
    require(matches.size() == 1, "expected 1 mention from nested");
    require(matches[0].target_agent_id == "agent", "target should be agent");
  }});

  // ---- parse_route_prefix tests ----

  tests.push_back({"multi_parse_route_agent", [] {
    auto route = ghostclaw::multi::parse_route_prefix("@coder fix bug");
    require(route.has_value(), "should parse route");
    require(route->target_id == "coder", "target should be coder");
    require(route->message == "fix bug", "message should be 'fix bug'");
  }});

  tests.push_back({"multi_parse_route_no_prefix", [] {
    auto route = ghostclaw::multi::parse_route_prefix("plain text message");
    require(!route.has_value(), "should return nullopt for plain text");
  }});

  tests.push_back({"multi_parse_route_no_message", [] {
    auto route = ghostclaw::multi::parse_route_prefix("@coder");
    require(!route.has_value(), "should return nullopt when no message after agent id");
  }});

  // ---- AgentPool tests ----

  tests.push_back({"multi_agent_pool_has_agent", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig agent;
    agent.id = "coder";
    agent.provider = "anthropic";
    agent.model = "test-model";
    config.multi.agents.push_back(agent);

    ghostclaw::multi::AgentPool pool(config);
    require(pool.has_agent("coder"), "should have agent 'coder'");
    require(!pool.has_agent("unknown"), "should not have unknown agent");
  }});

  tests.push_back({"multi_agent_pool_has_team", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig coder;
    coder.id = "coder";
    config.multi.agents.push_back(coder);

    ghostclaw::config::AgentConfig reviewer;
    reviewer.id = "reviewer";
    config.multi.agents.push_back(reviewer);

    ghostclaw::config::TeamConfig team;
    team.id = "dev";
    team.agents = {"coder", "reviewer"};
    team.leader_agent = "coder";
    config.multi.teams.push_back(team);

    ghostclaw::multi::AgentPool pool(config);
    require(pool.has_team("dev"), "should have team 'dev'");
    require(!pool.has_team("unknown"), "should not have unknown team");
    require(pool.team_leader("dev") == "coder", "team leader should be coder");

    auto members = pool.team_members("dev");
    require(members.size() == 2, "team should have 2 members");
  }});

  // ---- Config loading tests ----

  tests.push_back({"multi_config_load_agents", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"

[agents.coder]
provider = "anthropic"
model = "claude-sonnet-4-20250514"
system_prompt = "You are a senior software engineer."
temperature = 0.3

[agents.reviewer]
provider = "openai"
model = "gpt-4o"
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_multi_agents";
    std::filesystem::create_directories(tmp_dir);
    auto config_file = tmp_dir / "config.toml";
    {
      std::ofstream f(config_file);
      f << toml_content;
    }

    ConfigOverrideGuard guard(tmp_dir);
    auto loaded = ghostclaw::config::load_config();
    require(loaded.ok(), "config should load: " + loaded.error());

    const auto &cfg = loaded.value();
    require(cfg.multi.agents.size() == 2, "should have 2 agents");

    bool found_coder = false;
    bool found_reviewer = false;
    for (const auto &a : cfg.multi.agents) {
      if (a.id == "coder") {
        found_coder = true;
        require(a.provider == "anthropic", "coder provider should be anthropic");
        require(a.model == "claude-sonnet-4-20250514", "coder model mismatch");
        require(a.system_prompt == "You are a senior software engineer.",
                "coder system_prompt mismatch");
        require(a.temperature == 0.3, "coder temperature should be 0.3");
      }
      if (a.id == "reviewer") {
        found_reviewer = true;
        require(a.provider == "openai", "reviewer provider should be openai");
        require(a.model == "gpt-4o", "reviewer model mismatch");
      }
    }
    require(found_coder, "should find coder agent");
    require(found_reviewer, "should find reviewer agent");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"multi_config_load_teams", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"

[agents.coder]
provider = "anthropic"
model = "test"

[agents.reviewer]
provider = "openai"
model = "test"

[teams.dev]
agents = ["coder", "reviewer"]
leader_agent = "coder"
description = "Development team"
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_multi_teams";
    std::filesystem::create_directories(tmp_dir);
    auto config_file = tmp_dir / "config.toml";
    {
      std::ofstream f(config_file);
      f << toml_content;
    }

    ConfigOverrideGuard guard(tmp_dir);
    auto loaded = ghostclaw::config::load_config();
    require(loaded.ok(), "config should load: " + loaded.error());

    const auto &cfg = loaded.value();
    require(cfg.multi.teams.size() == 1, "should have 1 team");
    require(cfg.multi.teams[0].id == "dev", "team id should be dev");
    require(cfg.multi.teams[0].leader_agent == "coder", "leader should be coder");
    require(cfg.multi.teams[0].agents.size() == 2, "team should have 2 agents");
    require(cfg.multi.teams[0].description == "Development team", "description mismatch");

    std::filesystem::remove_all(tmp_dir);
  }});
}
