#include "test_framework.hpp"

#include "ghostclaw/auth/google_oauth.hpp"
#include "ghostclaw/config/config.hpp"

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

void register_google_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;

  tests.push_back({"google_config_load", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"

[google]
client_id = "test-client-id.apps.googleusercontent.com"
client_secret = "test-client-secret"
scopes = ["https://www.googleapis.com/auth/gmail.send"]
redirect_port = 9090
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_google_config";
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
    require(cfg.google.client_id == "test-client-id.apps.googleusercontent.com",
            "client_id mismatch");
    require(cfg.google.client_secret == "test-client-secret", "client_secret mismatch");
    require(cfg.google.scopes.size() == 1, "should have 1 scope");
    require(cfg.google.scopes[0] == "https://www.googleapis.com/auth/gmail.send",
            "scope mismatch");
    require(cfg.google.redirect_port == 9090, "redirect_port should be 9090");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"google_config_defaults", [] {
    ghostclaw::config::Config cfg;
    require(cfg.google.client_id.empty(), "client_id should be empty by default");
    require(cfg.google.client_secret.empty(), "client_secret should be empty by default");
    require(cfg.google.scopes.size() == 3, "should have 3 default scopes");
    require(cfg.google.redirect_port == 8089, "default redirect_port should be 8089");
  }});

  tests.push_back({"google_config_validation_gmail_no_client_id", [] {
    ghostclaw::config::Config config;
    config.email.backend = "gmail";
    // google.client_id left empty

    auto result = ghostclaw::config::validate_config(config);
    require(result.ok(), "should succeed with warning");
    bool found_warning = false;
    for (const auto &w : result.value()) {
      if (w.find("google.client_id") != std::string::npos) {
        found_warning = true;
      }
    }
    require(found_warning, "should warn about missing google.client_id");
  }});

  tests.push_back({"google_config_validation_calendar_no_client_id", [] {
    ghostclaw::config::Config config;
    config.calendar.backend = "google";
    // google.client_id left empty

    auto result = ghostclaw::config::validate_config(config);
    require(result.ok(), "should succeed with warning");
    bool found_warning = false;
    for (const auto &w : result.value()) {
      if (w.find("google.client_id") != std::string::npos) {
        found_warning = true;
      }
    }
    require(found_warning, "should warn about missing google.client_id for calendar");
  }});

  tests.push_back({"google_tokens_save_load_roundtrip", [] {
    // This test requires a config dir to exist. We use a temp override to
    // avoid touching the real config directory. Since load/save_google_tokens
    // use config_dir() internally, we override it.
    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_google_tokens";
    std::filesystem::create_directories(tmp_dir);
    ConfigOverrideGuard guard(tmp_dir);

    ghostclaw::auth::GoogleTokens tokens;
    tokens.access_token = "test-access-token-12345";
    tokens.refresh_token = "test-refresh-token-67890";
    tokens.expires_at = 1700000000;

    auto saved = ghostclaw::auth::save_google_tokens(tokens);
    require(saved.ok(), "save should succeed: " + saved.error());

    auto loaded = ghostclaw::auth::load_google_tokens();
    require(loaded.ok(), "load should succeed: " + loaded.error());
    require(loaded.value().access_token == "test-access-token-12345", "access_token mismatch");
    require(loaded.value().refresh_token == "test-refresh-token-67890", "refresh_token mismatch");
    require(loaded.value().expires_at == 1700000000, "expires_at mismatch");

    auto deleted = ghostclaw::auth::delete_google_tokens();
    require(deleted.ok(), "delete should succeed");

    auto loaded_after = ghostclaw::auth::load_google_tokens();
    require(!loaded_after.ok(), "should fail after delete");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"google_config_no_section_uses_defaults", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_google_nosection";
    std::filesystem::create_directories(tmp_dir);
    auto config_file = tmp_dir / "config.toml";
    {
      std::ofstream f(config_file);
      f << toml_content;
    }

    ConfigOverrideGuard guard(tmp_dir);
    auto loaded = ghostclaw::config::load_config();
    require(loaded.ok(), "config should load");
    require(loaded.value().google.client_id.empty(), "client_id should be empty");
    require(loaded.value().google.scopes.size() == 3, "should have 3 default scopes");

    std::filesystem::remove_all(tmp_dir);
  }});
}
