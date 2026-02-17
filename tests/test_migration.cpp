#include "test_framework.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/migration/module.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

struct EnvGuard {
  std::string key;
  std::optional<std::string> old_value;

  EnvGuard(std::string key_, std::optional<std::string> value) : key(std::move(key_)) {
    if (const char *existing = std::getenv(key.c_str()); existing != nullptr) {
      old_value = existing;
    }
    if (value.has_value()) {
      setenv(key.c_str(), value->c_str(), 1);
    } else {
      unsetenv(key.c_str());
    }
  }

  ~EnvGuard() {
    if (old_value.has_value()) {
      setenv(key.c_str(), old_value->c_str(), 1);
    } else {
      unsetenv(key.c_str());
    }
  }
};

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

class TempDir {
public:
  explicit TempDir(const std::string &prefix) {
    static std::mt19937_64 rng{std::random_device{}()};
    path_ = std::filesystem::temp_directory_path() / (prefix + std::to_string(rng()));
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, const std::string &content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  out << content;
}

const ghostclaw::config::AgentConfig *
find_agent(const std::vector<ghostclaw::config::AgentConfig> &agents, const std::string &id) {
  for (const auto &agent : agents) {
    if (agent.id == id) {
      return &agent;
    }
  }
  return nullptr;
}

const ghostclaw::config::TeamConfig *
find_team(const std::vector<ghostclaw::config::TeamConfig> &teams, const std::string &id) {
  for (const auto &team : teams) {
    if (team.id == id) {
      return &team;
    }
  }
  return nullptr;
}

bool contains_substring(const std::vector<std::string> &lines, const std::string &needle) {
  for (const auto &line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

void register_migration_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;
  namespace cfg = ghostclaw::config;
  namespace migration = ghostclaw::migration;

  tests.push_back({"migration_detect_legacy_settings_compat_path", [] {
                     TempDir home("ghostclaw-migration-home-");
                     const EnvGuard env_home("HOME", home.path().string());
                     const EnvGuard env_legacy_home("LEGACY_CLAW_HOME", std::nullopt);
                     const std::string compat_env = std::string("TINY") + "CLAW_HOME";
                     const EnvGuard env_vendor_home(compat_env, std::nullopt);

                     const std::filesystem::path compat_dir =
                         home.path() / ("." + std::string("tiny") + "claw");
                     const std::filesystem::path settings = compat_dir / "settings.json";
                     write_file(settings, R"({"agents":{}})");

                     auto detected = migration::detect_legacy_settings_path();
                     require(detected.ok(), detected.error());
                     require(detected.value() == settings,
                             "expected compatibility settings path to be detected");
                   }});

  tests.push_back({"migration_import_replaces_multi_config", [] {
                     TempDir home("ghostclaw-migration-home-");
                     TempDir source("ghostclaw-migration-source-");
                     const EnvGuard env_home("HOME", home.path().string());
                     const ConfigOverrideGuard cfg_override(home.path() / ".ghostclaw");

                     cfg::Config base;
                     base.api_key = "test";
                     cfg::AgentConfig old_agent;
                     old_agent.id = "old";
                     old_agent.provider = "openai";
                     old_agent.model = "old-model";
                     base.multi.agents.push_back(old_agent);
                     auto saved = cfg::save_config(base);
                     require(saved.ok(), saved.error());

                     const std::filesystem::path settings = source.path() / "settings.json";
                     write_file(
                         settings,
                         R"({
  "models": {
    "provider": "claude",
    "anthropic": { "model": "sonnet" }
  },
  "agents": {
    "coder": { "name": "Coder", "provider": "claude", "model": "sonnet" },
    "reviewer": { "provider": "openai", "model": "gpt-4.1" }
  },
  "teams": {
    "dev": { "name": "Dev Team", "agents": ["coder", "reviewer"], "leader_agent": "coder" }
  }
})");

                     migration::LegacyImportOptions options;
                     options.settings_path = settings;
                     options.merge_with_existing = false;
                     options.write_config = true;

                     auto imported = migration::import_legacy_settings(options);
                     require(imported.ok(), imported.error());
                     require(imported.value().imported_agents == 2,
                             "expected two imported agents");
                     require(imported.value().imported_teams == 1,
                             "expected one imported team");
                     require(!imported.value().created_default_agent,
                             "fallback agent should not be created");

                     auto loaded = cfg::load_config();
                     require(loaded.ok(), loaded.error());
                     require(loaded.value().multi.agents.size() == 2,
                             "replace mode should overwrite prior agent list");
                     require(loaded.value().multi.teams.size() == 1, "expected one team");
                     require(loaded.value().multi.default_agent == "coder",
                             "default agent should resolve from leader_agent");
                     require(loaded.value().default_provider == "anthropic",
                             "provider should normalize from claude");
                     require(loaded.value().default_model == "claude-sonnet-4-5",
                             "model should normalize from sonnet alias");

                     const auto *coder = find_agent(loaded.value().multi.agents, "coder");
                     require(coder != nullptr, "coder agent should exist");
                     require(coder->provider == "anthropic", "coder provider should normalize");
                     require(coder->model == "claude-sonnet-4-5", "coder model should normalize");
                   }});

  tests.push_back({"migration_import_dry_run_creates_fallback_agent_only_in_result", [] {
                     TempDir home("ghostclaw-migration-home-");
                     TempDir source("ghostclaw-migration-source-");
                     const EnvGuard env_home("HOME", home.path().string());
                     const ConfigOverrideGuard cfg_override(home.path() / ".ghostclaw");

                     const std::filesystem::path settings = source.path() / "settings.json";
                     write_file(settings,
                                R"({
  "models": {
    "provider": "openai",
    "openai": { "model": "gpt-4.1" }
  }
})");

                     migration::LegacyImportOptions options;
                     options.settings_path = settings;
                     options.write_config = false;

                     auto imported = migration::import_legacy_settings(options);
                     require(imported.ok(), imported.error());
                     require(imported.value().created_default_agent,
                             "fallback default agent should be created");
                     require(imported.value().merged_config.multi.agents.size() == 1,
                             "result should include one fallback agent");
                     require(imported.value().merged_config.multi.agents[0].id == "default",
                             "fallback agent id should be default");
                     require(imported.value().merged_config.multi.agents[0].provider == "openai",
                             "fallback provider should come from defaults");
                     require(imported.value().merged_config.multi.agents[0].model == "gpt-4.1",
                             "fallback model should come from defaults");

                     auto path = cfg::config_path();
                     require(path.ok(), path.error());
                     require(!std::filesystem::exists(path.value()),
                             "dry run should not write config.toml");
                   }});

  tests.push_back({"migration_import_merge_upserts_agents_and_teams", [] {
                     TempDir home("ghostclaw-migration-home-");
                     TempDir source("ghostclaw-migration-source-");
                     const EnvGuard env_home("HOME", home.path().string());
                     const ConfigOverrideGuard cfg_override(home.path() / ".ghostclaw");

                     cfg::Config base;
                     base.api_key = "test";
                     cfg::AgentConfig keeper;
                     keeper.id = "keeper";
                     keeper.provider = "anthropic";
                     keeper.model = "old-model";
                     base.multi.agents.push_back(keeper);
                     auto saved = cfg::save_config(base);
                     require(saved.ok(), saved.error());

                     const std::filesystem::path settings = source.path() / "settings.json";
                     write_file(
                         settings,
                         R"({
  "agents": {
    "keeper": { "provider": "openai", "model": "gpt-4.1" },
    "assistant": { "provider": "claude", "model": "sonnet" }
  },
  "teams": {
    "delivery": { "name": "Delivery", "agents": ["keeper", "assistant"] }
  }
})");

                     migration::LegacyImportOptions options;
                     options.settings_path = settings;
                     options.merge_with_existing = true;
                     options.write_config = false;

                     auto imported = migration::import_legacy_settings(options);
                     require(imported.ok(), imported.error());
                     require(imported.value().imported_agents == 2,
                             "expected two imported agents");
                     require(imported.value().imported_teams == 1,
                             "expected one imported team");

                     const auto &merged = imported.value().merged_config;
                     require(merged.multi.agents.size() == 2,
                             "merge should keep/upsert existing agents");
                     const auto *updated_keeper = find_agent(merged.multi.agents, "keeper");
                     require(updated_keeper != nullptr, "keeper should still exist");
                     require(updated_keeper->provider == "openai",
                             "keeper should be updated from import");
                     require(updated_keeper->model == "gpt-4.1",
                             "keeper model should be updated");
                     const auto *assistant = find_agent(merged.multi.agents, "assistant");
                     require(assistant != nullptr, "assistant should be added");
                     require(assistant->provider == "anthropic",
                             "assistant provider should normalize from claude");
                     require(assistant->model == "claude-sonnet-4-5",
                             "assistant model should normalize");

                     const auto *delivery = find_team(merged.multi.teams, "delivery");
                     require(delivery != nullptr, "delivery team should be present");
                     require(delivery->leader_agent == "keeper",
                             "team leader should default to first member");
                     require(contains_substring(imported.value().warnings, "had no leader_agent"),
                             "missing leader warning should be present");
                   }});
}
