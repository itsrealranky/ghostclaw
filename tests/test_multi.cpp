#include "test_framework.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/multi/agent_pool.hpp"
#include "ghostclaw/multi/orchestrator.hpp"
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

  // ═══════════════════════════════════════════════════════════════════════════
  // extract_mentions
  // ═══════════════════════════════════════════════════════════════════════════

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
    auto matches = ghostclaw::multi::extract_mentions("[@agent: hello [world]]");
    require(matches.size() == 1, "expected 1 mention from nested");
    require(matches[0].target_agent_id == "agent", "target should be agent");
  }});

  tests.push_back({"multi_extract_mentions_with_hyphens_underscores", [] {
    auto matches = ghostclaw::multi::extract_mentions(
        "[@code-review_agent: check this]");
    require(matches.size() == 1, "expected 1 mention");
    require(matches[0].target_agent_id == "code-review_agent", "should handle hyphens/underscores");
  }});

  tests.push_back({"multi_extract_mentions_whitespace_trimmed", [] {
    auto matches = ghostclaw::multi::extract_mentions("[@agent:   lots of space  ]");
    require(matches.size() == 1, "expected 1 mention");
    require(matches[0].message == "lots of space", "should trim whitespace");
  }});

  tests.push_back({"multi_extract_mentions_end_pos", [] {
    auto matches = ghostclaw::multi::extract_mentions("[@agent: msg]");
    require(matches.size() == 1, "expected 1 mention");
    require(matches[0].start_pos == 0, "start should be 0");
    require(matches[0].end_pos == 13, "end_pos should be 13");
  }});

  // ═══════════════════════════════════════════════════════════════════════════
  // parse_route_prefix
  // ═══════════════════════════════════════════════════════════════════════════

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

  tests.push_back({"multi_parse_route_empty", [] {
    auto route = ghostclaw::multi::parse_route_prefix("");
    require(!route.has_value(), "should return nullopt for empty string");
  }});

  tests.push_back({"multi_parse_route_whitespace_only", [] {
    auto route = ghostclaw::multi::parse_route_prefix("   ");
    require(!route.has_value(), "should return nullopt for whitespace");
  }});

  tests.push_back({"multi_parse_route_at_only", [] {
    auto route = ghostclaw::multi::parse_route_prefix("@ message");
    require(!route.has_value(), "should return nullopt for bare @");
  }});

  tests.push_back({"multi_parse_route_multiword_message", [] {
    auto route = ghostclaw::multi::parse_route_prefix("@agent please fix the login bug in auth.cpp");
    require(route.has_value(), "should parse");
    require(route->target_id == "agent", "target should be agent");
    require(route->message == "please fix the login bug in auth.cpp", "full message preserved");
  }});

  tests.push_back({"multi_parse_route_leading_whitespace", [] {
    auto route = ghostclaw::multi::parse_route_prefix("  @coder fix bug");
    require(route.has_value(), "should handle leading whitespace");
    require(route->target_id == "coder", "target should be coder");
  }});

  // ═══════════════════════════════════════════════════════════════════════════
  // AgentPool
  // ═══════════════════════════════════════════════════════════════════════════

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

  tests.push_back({"multi_agent_pool_agent_ids", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig a1;
    a1.id = "alpha";
    ghostclaw::config::AgentConfig a2;
    a2.id = "beta";
    config.multi.agents = {a1, a2};

    ghostclaw::multi::AgentPool pool(config);
    auto ids = pool.agent_ids();
    require(ids.size() == 2, "should have 2 ids");
    // IDs come from unordered_map so sort for comparison
    std::sort(ids.begin(), ids.end());
    require(ids[0] == "alpha", "first should be alpha");
    require(ids[1] == "beta", "second should be beta");
  }});

  tests.push_back({"multi_agent_pool_team_leader_missing", [] {
    ghostclaw::config::Config config;
    ghostclaw::multi::AgentPool pool(config);
    require(pool.team_leader("nonexistent").empty(), "missing team should return empty leader");
    require(pool.team_members("nonexistent").empty(), "missing team should return empty members");
  }});

  tests.push_back({"multi_agent_pool_get_unknown_agent", [] {
    ghostclaw::config::Config config;
    ghostclaw::multi::AgentPool pool(config);
    auto result = pool.get_or_create("nonexistent");
    require(!result.ok(), "should fail for unknown agent");
    require(result.error().find("unknown agent") != std::string::npos, "error should mention unknown");
  }});

  // ═══════════════════════════════════════════════════════════════════════════
  // Config loading
  // ═══════════════════════════════════════════════════════════════════════════

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

  tests.push_back({"multi_config_load_multi_section", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"

[multi]
default_agent = "coder"
max_internal_messages = 25

[agents.coder]
provider = "anthropic"
model = "test"
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_multi_section";
    std::filesystem::create_directories(tmp_dir);
    auto config_file = tmp_dir / "config.toml";
    {
      std::ofstream f(config_file);
      f << toml_content;
    }

    ConfigOverrideGuard guard(tmp_dir);
    auto loaded = ghostclaw::config::load_config();
    require(loaded.ok(), "config should load: " + loaded.error());
    require(loaded.value().multi.default_agent == "coder", "default_agent should be coder");
    require(loaded.value().multi.max_internal_messages == 25, "max should be 25");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"multi_config_defaults", [] {
    ghostclaw::config::Config cfg;
    require(cfg.multi.default_agent == "ghostclaw", "default_agent should be ghostclaw");
    require(cfg.multi.max_internal_messages == 50, "max should default to 50");
    require(cfg.multi.agents.empty(), "no agents by default");
    require(cfg.multi.teams.empty(), "no teams by default");
  }});

  // ═══════════════════════════════════════════════════════════════════════════
  // Config validation
  // ═══════════════════════════════════════════════════════════════════════════

  tests.push_back({"multi_validate_team_unknown_agent", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::TeamConfig team;
    team.id = "dev";
    team.agents = {"nonexistent"};
    team.leader_agent = "nonexistent";
    config.multi.teams.push_back(team);

    auto result = ghostclaw::config::validate_config(config);
    require(!result.ok(), "should fail for unknown agent in team");
    require(result.error().find("unknown agent") != std::string::npos, "error mentions unknown");
  }});

  tests.push_back({"multi_validate_leader_not_in_team", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig a1;
    a1.id = "coder";
    ghostclaw::config::AgentConfig a2;
    a2.id = "reviewer";
    config.multi.agents = {a1, a2};

    ghostclaw::config::TeamConfig team;
    team.id = "dev";
    team.agents = {"coder"};
    team.leader_agent = "reviewer"; // reviewer is not in the team
    config.multi.teams.push_back(team);

    auto result = ghostclaw::config::validate_config(config);
    require(!result.ok(), "should fail when leader not in team");
    require(result.error().find("not in the team") != std::string::npos, "error mentions not in team");
  }});

  tests.push_back({"multi_validate_duplicate_agent_id", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig a1;
    a1.id = "coder";
    ghostclaw::config::AgentConfig a2;
    a2.id = "coder"; // duplicate
    config.multi.agents = {a1, a2};

    auto result = ghostclaw::config::validate_config(config);
    require(!result.ok(), "should fail on duplicate agent id");
    require(result.error().find("duplicate") != std::string::npos, "error mentions duplicate");
  }});

  tests.push_back({"multi_validate_agent_temperature_range", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig agent;
    agent.id = "hot";
    agent.temperature = 3.0; // too high
    config.multi.agents.push_back(agent);

    auto result = ghostclaw::config::validate_config(config);
    require(!result.ok(), "should fail on temperature > 2.0");
    require(result.error().find("temperature") != std::string::npos, "error mentions temperature");
  }});

  tests.push_back({"multi_validate_max_internal_messages_zero", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig agent;
    agent.id = "test";
    config.multi.agents.push_back(agent);
    config.multi.max_internal_messages = 0;

    auto result = ghostclaw::config::validate_config(config);
    require(!result.ok(), "should fail when max_internal_messages is 0");
  }});

  tests.push_back({"multi_validate_empty_team", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::TeamConfig team;
    team.id = "empty";
    // no agents
    config.multi.teams.push_back(team);

    auto result = ghostclaw::config::validate_config(config);
    require(!result.ok(), "should fail on team with no agents");
  }});

  tests.push_back({"multi_validate_warns_default_agent_mismatch", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig agent;
    agent.id = "coder";
    config.multi.agents.push_back(agent);
    config.multi.default_agent = "nonexistent";

    auto result = ghostclaw::config::validate_config(config);
    require(result.ok(), "should still succeed as warning");
    bool found_warning = false;
    for (const auto &w : result.value()) {
      if (w.find("default_agent") != std::string::npos) {
        found_warning = true;
      }
    }
    require(found_warning, "should warn about default_agent mismatch");
  }});

  tests.push_back({"multi_validate_warns_team_no_leader", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig agent;
    agent.id = "coder";
    config.multi.agents.push_back(agent);

    ghostclaw::config::TeamConfig team;
    team.id = "dev";
    team.agents = {"coder"};
    // no leader_agent
    config.multi.teams.push_back(team);

    auto result = ghostclaw::config::validate_config(config);
    require(result.ok(), "should succeed with warning");
    bool found_warning = false;
    for (const auto &w : result.value()) {
      if (w.find("no leader_agent") != std::string::npos) {
        found_warning = true;
      }
    }
    require(found_warning, "should warn about missing leader_agent");
  }});

  // ═══════════════════════════════════════════════════════════════════════════
  // Orchestrator - query helpers
  // ═══════════════════════════════════════════════════════════════════════════

  tests.push_back({"multi_orchestrator_list_agents", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig a1;
    a1.id = "alpha";
    ghostclaw::config::AgentConfig a2;
    a2.id = "beta";
    config.multi.agents = {a1, a2};

    auto pool = std::make_shared<ghostclaw::multi::AgentPool>(config);
    auto ws_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_orch_list";
    std::filesystem::create_directories(ws_dir);
    auto store = std::make_shared<ghostclaw::sessions::SessionStore>(ws_dir);

    ghostclaw::multi::Orchestrator orch(config, pool, store);
    auto ids = orch.list_agent_ids();
    require(ids.size() == 2, "should list 2 agents");
    require(orch.active_conversation_count() == 0, "no active conversations initially");
    require(!orch.is_running(), "should not be running");

    std::filesystem::remove_all(ws_dir);
  }});

  tests.push_back({"multi_orchestrator_list_teams", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::AgentConfig a1;
    a1.id = "coder";
    config.multi.agents.push_back(a1);

    ghostclaw::config::TeamConfig t1;
    t1.id = "dev";
    t1.agents = {"coder"};
    t1.leader_agent = "coder";
    config.multi.teams.push_back(t1);

    auto pool = std::make_shared<ghostclaw::multi::AgentPool>(config);
    auto ws_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_orch_teams";
    std::filesystem::create_directories(ws_dir);
    auto store = std::make_shared<ghostclaw::sessions::SessionStore>(ws_dir);

    ghostclaw::multi::Orchestrator orch(config, pool, store);
    auto team_ids = orch.list_team_ids();
    require(team_ids.size() == 1, "should list 1 team");
    require(team_ids[0] == "dev", "team should be dev");

    std::filesystem::remove_all(ws_dir);
  }});

  // ═══════════════════════════════════════════════════════════════════════════
  // Conversation / InternalMessage / RouteTarget structs
  // ═══════════════════════════════════════════════════════════════════════════

  tests.push_back({"multi_conversation_defaults", [] {
    ghostclaw::multi::Conversation conv;
    require(conv.pending_count == 0, "pending_count should default to 0");
    require(conv.total_messages == 0, "total_messages should default to 0");
    require(!conv.complete, "should not be complete by default");
  }});

  tests.push_back({"multi_internal_message_defaults", [] {
    ghostclaw::multi::InternalMessage msg;
    require(msg.id == 0, "id should default to 0");
    require(!msg.is_mention, "should not be mention by default");
    require(msg.timestamp == 0, "timestamp should default to 0");
  }});

  tests.push_back({"multi_route_target_defaults", [] {
    ghostclaw::multi::RouteTarget rt;
    require(!rt.is_team, "should not be team by default");
    require(rt.target_id.empty(), "target should be empty");
    require(rt.message.empty(), "message should be empty");
  }});
}
