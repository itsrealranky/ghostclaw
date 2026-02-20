#include "test_framework.hpp"

#include "ghostclaw/cli/commands.hpp"
#include "ghostclaw/config/config.hpp"

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

std::filesystem::path make_temp_home() {
  static std::mt19937_64 rng{std::random_device{}()};
  const auto path = std::filesystem::temp_directory_path() /
                    ("ghostclaw-cli-features-home-" + std::to_string(rng()));
  std::filesystem::create_directories(path);
  return path;
}

int run_cli(const std::vector<std::string> &args) {
  std::vector<std::string> owned = args;
  std::vector<char *> argv;
  argv.reserve(owned.size());
  for (auto &arg : owned) {
    argv.push_back(arg.data());
  }
  return ghostclaw::cli::run_cli(static_cast<int>(argv.size()), argv.data());
}

} // namespace

void register_cli_features_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;

  tests.push_back({"cli_config_schema_command", [] {
                     const auto home = make_temp_home();
                     const EnvGuard env_home("HOME", home.string());
                     const int code = run_cli({"ghostclaw", "config", "schema"});
                     require(code == 0, "config schema command should succeed");
                   }});

  tests.push_back({"cli_providers_and_models_commands", [] {
                     const auto home = make_temp_home();
                     const EnvGuard env_home("HOME", home.string());
                     int code = run_cli({"ghostclaw", "providers"});
                     require(code == 0, "providers command should succeed");
                     code = run_cli({"ghostclaw", "models", "refresh", "--provider", "openai"});
                     require(code == 0, "models refresh command should succeed");
                   }});

  tests.push_back({"cli_hardware_and_peripheral_commands", [] {
                     const auto home = make_temp_home();
                     const EnvGuard env_home("HOME", home.string());

                     const auto fake_port = std::filesystem::temp_directory_path() /
                                            "ghostclaw-fake-serial-port";
                     {
                       std::ofstream out(fake_port);
                       out << "serial";
                     }

                     int code = run_cli({"ghostclaw", "hardware", "introspect", fake_port.string()});
                     require(code == 0, "hardware introspect should succeed for existing path");

                     code = run_cli(
                         {"ghostclaw", "peripheral", "add", "arduino-uno", fake_port.string()});
                     require(code == 0, "peripheral add should succeed");

                     code = run_cli({"ghostclaw", "peripheral", "list"});
                     require(code == 0, "peripheral list should succeed");

                     std::filesystem::remove(fake_port);
                   }});

  tests.push_back({"cli_channel_bind_telegram_and_cron_extended", [] {
                     const auto home = make_temp_home();
                     const EnvGuard env_home("HOME", home.string());

                     ghostclaw::config::Config cfg;
                     ghostclaw::config::TelegramConfig telegram;
                     telegram.bot_token = "test-token";
                     cfg.channels.telegram = telegram;
                     auto saved = ghostclaw::config::save_config(cfg);
                     require(saved.ok(), "save config should succeed");

                     int code = run_cli({"ghostclaw", "channel", "bind-telegram", "tester"});
                     require(code == 0, "channel bind-telegram should succeed");

                     auto loaded = ghostclaw::config::load_config();
                     require(loaded.ok(), loaded.error());
                     require(loaded.value().channels.telegram.has_value(),
                             "telegram config should still exist");
                     require(!loaded.value().channels.telegram->allowed_users.empty(),
                             "telegram allowlist should be populated");

                     code = run_cli({"ghostclaw", "cron", "add-every", "1000", "ping"});
                     require(code == 0, "cron add-every should succeed");
                     code = run_cli({"ghostclaw", "cron", "once", "5s", "pong"});
                     require(code == 0, "cron once should succeed");
                     code = run_cli({"ghostclaw", "cron", "list"});
                     require(code == 0, "cron list should succeed");
                   }});

  tests.push_back({"cli_service_install_status_uninstall", [] {
                     const auto home = make_temp_home();
                     const EnvGuard env_home("HOME", home.string());

                     int code = run_cli({"ghostclaw", "service", "install"});
                     require(code == 0, "service install should succeed");
                     code = run_cli({"ghostclaw", "service", "status"});
                     require(code == 0, "service status should succeed");
                     code = run_cli({"ghostclaw", "service", "uninstall"});
                     require(code == 0, "service uninstall should succeed");
                   }});
}
