#include "test_framework.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/heartbeat/cron.hpp"

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

void register_daemon_schedules_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;

  tests.push_back({"daemon_config_load_schedules", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"

[daemon]
auto_start_schedules = true

[daemon.schedules.daily_summary]
expression = "0 9 * * *"
command = "Summarize yesterday's activity"
enabled = true

[daemon.schedules.weekly_cleanup]
expression = "0 0 * * 0"
command = "Clean up old files"
enabled = true
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_daemon_schedules";
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
    require(cfg.daemon.auto_start_schedules == true, "auto_start should be true");
    require(cfg.daemon.schedules.size() == 2, "should have 2 schedules");

    bool found_daily = false;
    bool found_weekly = false;
    for (const auto &entry : cfg.daemon.schedules) {
      if (entry.id == "daily_summary") {
        found_daily = true;
        require(entry.expression == "0 9 * * *", "daily expression mismatch");
        require(entry.command == "Summarize yesterday's activity", "daily command mismatch");
        require(entry.enabled == true, "daily should be enabled");
      }
      if (entry.id == "weekly_cleanup") {
        found_weekly = true;
        require(entry.expression == "0 0 * * 0", "weekly expression mismatch");
        require(entry.command == "Clean up old files", "weekly command mismatch");
      }
    }
    require(found_daily, "should find daily_summary schedule");
    require(found_weekly, "should find weekly_cleanup schedule");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"daemon_config_empty_schedules", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_daemon_empty";
    std::filesystem::create_directories(tmp_dir);
    auto config_file = tmp_dir / "config.toml";
    {
      std::ofstream f(config_file);
      f << toml_content;
    }

    ConfigOverrideGuard guard(tmp_dir);
    auto loaded = ghostclaw::config::load_config();
    require(loaded.ok(), "config should load");
    require(loaded.value().daemon.schedules.empty(), "schedules should be empty");
    require(loaded.value().daemon.auto_start_schedules == true, "auto_start default is true");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"daemon_config_disabled_schedule", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"

[daemon.schedules.disabled_job]
expression = "0 12 * * *"
command = "disabled task"
enabled = false
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_daemon_disabled";
    std::filesystem::create_directories(tmp_dir);
    auto config_file = tmp_dir / "config.toml";
    {
      std::ofstream f(config_file);
      f << toml_content;
    }

    ConfigOverrideGuard guard(tmp_dir);
    auto loaded = ghostclaw::config::load_config();
    require(loaded.ok(), "config should load");
    require(loaded.value().daemon.schedules.size() == 1, "should have 1 schedule");
    require(loaded.value().daemon.schedules[0].enabled == false, "should be disabled");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"daemon_config_validate_bad_cron", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::ScheduleEntry entry;
    entry.id = "bad";
    entry.expression = "";
    entry.command = "test";
    config.daemon.schedules.push_back(entry);

    auto result = ghostclaw::config::validate_config(config);
    require(result.ok(), "should succeed with warning");
    bool found_warning = false;
    for (const auto &w : result.value()) {
      if (w.find("empty expression") != std::string::npos) {
        found_warning = true;
      }
    }
    require(found_warning, "should warn about empty expression");
  }});

  tests.push_back({"daemon_config_defaults", [] {
    ghostclaw::config::Config cfg;
    require(cfg.daemon.auto_start_schedules == true, "auto_start default true");
    require(cfg.daemon.schedules.empty(), "no schedules by default");
  }});
}
