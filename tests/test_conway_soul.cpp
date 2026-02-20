#include "test_framework.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/conway/module.hpp"
#include "ghostclaw/profiler/tool_profiler.hpp"
#include "ghostclaw/soul/manager.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

using ghostclaw::tests::require;

namespace {

std::filesystem::path make_temp_workspace() {
  static std::mt19937_64 rng{std::random_device{}()};
  const auto path = std::filesystem::temp_directory_path() /
                    ("ghostclaw-conway-soul-" + std::to_string(rng()));
  std::filesystem::create_directories(path);
  return path;
}

} // namespace

void register_conway_soul_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  // --- ConwayConfig defaults ---
  tests.push_back({"conway_config_defaults", []() {
    ghostclaw::config::ConwayConfig cfg;
    require(!cfg.enabled, "conway disabled by default");
    require(cfg.api_key.empty(), "api_key empty by default");
    require(cfg.api_url == "https://api.conway.tech", "default api_url mismatch");
    require(cfg.default_region == "eu-north", "default region mismatch");
    require(!cfg.survival_monitoring, "survival_monitoring off by default");
    require(cfg.low_compute_threshold_usd > 0.0, "low_compute threshold should be positive");
    require(cfg.critical_threshold_usd > 0.0, "critical threshold should be positive");
  }});

  // --- Survival tier computation ---
  tests.push_back({"conway_survival_tiers", []() {
    using namespace ghostclaw::conway;
    ghostclaw::config::ConwayConfig cfg;
    cfg.low_compute_threshold_usd = 0.50;
    cfg.critical_threshold_usd = 0.10;

    require(compute_survival_tier(cfg, 1.0) == SurvivalTier::Normal, "1.0 USD = normal");
    require(compute_survival_tier(cfg, 0.50) == SurvivalTier::Normal, "0.50 USD = normal boundary");
    require(compute_survival_tier(cfg, 0.49) == SurvivalTier::LowCompute, "0.49 USD = low_compute");
    require(compute_survival_tier(cfg, 0.10) == SurvivalTier::LowCompute, "0.10 USD = low_compute boundary");
    require(compute_survival_tier(cfg, 0.09) == SurvivalTier::Critical, "0.09 USD = critical");
    require(compute_survival_tier(cfg, 0.0) == SurvivalTier::Dead, "0.0 USD = dead");
    require(compute_survival_tier(cfg, -0.01) == SurvivalTier::Dead, "negative = dead");
  }});

  tests.push_back({"conway_survival_tier_strings", []() {
    using namespace ghostclaw::conway;
    require(survival_tier_to_string(SurvivalTier::Normal) == "normal", "normal string");
    require(survival_tier_to_string(SurvivalTier::LowCompute) == "low_compute", "low_compute string");
    require(survival_tier_to_string(SurvivalTier::Critical) == "critical", "critical string");
    require(survival_tier_to_string(SurvivalTier::Dead) == "dead", "dead string");
  }});

  tests.push_back({"conway_config_in_main_config", []() {
    ghostclaw::config::Config cfg;
    require(!cfg.conway.enabled, "conway disabled in default config");
    require(cfg.conway.api_key.empty(), "api_key empty in default config");
    require(cfg.soul.path == "SOUL.md", "default soul path");
  }});

  // --- SoulConfig defaults ---
  tests.push_back({"soul_config_defaults", []() {
    ghostclaw::config::SoulConfig cfg;
    require(!cfg.enabled, "soul disabled by default");
    require(cfg.path == "SOUL.md", "default path");
    require(cfg.git_versioned, "git versioned by default");
    require(cfg.max_reflections == 100, "default max reflections");
    require(cfg.protected_sections.empty(), "no protected sections by default");
  }});

  // --- SoulManager ---
  tests.push_back({"soul_manager_initialize", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws, {}, 100, false);

    const auto soul_path = ws / "SOUL.md";
    require(!std::filesystem::exists(soul_path), "SOUL.md should not exist yet");

    const auto status = manager.initialize("TestAgent");
    require(status.ok(), status.ok() ? "ok" : status.error());
    require(std::filesystem::exists(soul_path), "SOUL.md should exist after initialize");

    const std::string content = manager.load();
    require(!content.empty(), "SOUL.md should not be empty");
    require(content.find("# Identity") != std::string::npos, "should have Identity section");
    require(content.find("# Values") != std::string::npos, "should have Values section");
    require(content.find("# Reflections") != std::string::npos, "should have Reflections section");

    std::filesystem::remove_all(ws);
  }});

  tests.push_back({"soul_manager_update_section", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws, {}, 100, false);

    require(manager.initialize().ok(), "initialize should succeed");

    const auto status = manager.update_section("Goals", "Focus on Rust and C++ codebases.");
    require(status.ok(), status.ok() ? "ok" : status.error());

    const std::string content = manager.load();
    require(content.find("Rust and C++") != std::string::npos,
            "updated content should appear in SOUL.md");

    std::filesystem::remove_all(ws);
  }});

  tests.push_back({"soul_manager_update_creates_section", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws, {}, 100, false);

    require(manager.initialize().ok(), "initialize should succeed");

    // Update a non-existing section — it should be appended
    const auto status = manager.update_section("Tools", "Prefer shell for batch operations.");
    require(status.ok(), status.ok() ? "ok" : status.error());

    const std::string content = manager.load();
    require(content.find("# Tools") != std::string::npos, "new section should be appended");
    require(content.find("shell for batch operations") != std::string::npos, "content should appear");

    std::filesystem::remove_all(ws);
  }});

  tests.push_back({"soul_manager_append_reflection", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws, {}, 100, false);

    require(manager.initialize().ok(), "initialize should succeed");

    const auto status = manager.append_reflection("Learned to use file_edit efficiently.");
    require(status.ok(), status.ok() ? "ok" : status.error());

    const std::string content = manager.load();
    require(content.find("file_edit") != std::string::npos, "reflection text should appear");

    std::filesystem::remove_all(ws);
  }});

  tests.push_back({"soul_manager_protected_section", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws, {"Identity"}, 100, false);

    require(manager.initialize().ok(), "initialize should succeed");

    const auto status = manager.update_section("Identity", "Trying to overwrite identity.");
    require(!status.ok(), "should fail for protected section");
    require(status.error().find("protected") != std::string::npos,
            "error should mention 'protected'");

    std::filesystem::remove_all(ws);
  }});

  tests.push_back({"soul_manager_max_reflections_enforced", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws, {}, 3, false);

    require(manager.initialize().ok(), "initialize should succeed");

    for (int i = 0; i < 6; ++i) {
      require(manager.append_reflection("Reflection #" + std::to_string(i)).ok(),
              "reflection should succeed");
    }

    const std::string content = manager.load();
    // Count "- [" occurrences to check reflection count
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = content.find("- [", pos)) != std::string::npos) {
      ++count;
      ++pos;
    }
    require(count <= 3, "should not exceed max_reflections limit");

    std::filesystem::remove_all(ws);
  }});

  tests.push_back({"soul_manager_load_nonexistent_returns_empty", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws);
    const std::string content = manager.load();
    require(content.empty(), "load on nonexistent SOUL.md should return empty string");
    std::filesystem::remove_all(ws);
  }});

  tests.push_back({"soul_manager_summary_bounded", []() {
    const auto ws = make_temp_workspace();
    ghostclaw::soul::SoulManager manager(ws, {}, 100, false);
    require(manager.initialize().ok(), "initialize should succeed");

    const std::string summary = manager.summary();
    require(!summary.empty(), "summary should not be empty");
    require(summary.size() <= 1600, "summary should be bounded");

    std::filesystem::remove_all(ws);
  }});

  // --- ToolProfiler ---
  tests.push_back({"profiler_empty_on_creation", []() {
    ghostclaw::profiler::ToolProfiler profiler;
    require(profiler.total_calls() == 0, "new profiler should have 0 calls");
    const std::string report = profiler.format_report();
    require(!report.empty(), "report should not be empty even with 0 calls");
  }});

  tests.push_back({"profiler_records_calls", []() {
    ghostclaw::profiler::ToolProfiler profiler;

    profiler.record("shell", true, std::chrono::milliseconds(100));
    profiler.record("shell", true, std::chrono::milliseconds(200));
    profiler.record("shell", false, std::chrono::milliseconds(50));
    profiler.record("file_read", true, std::chrono::milliseconds(10));

    require(profiler.total_calls() == 4, "should have 4 total calls");

    const auto shell = profiler.stats_for("shell");
    require(shell.call_count == 3, "shell should have 3 calls");
    require(shell.success_count == 2, "shell should have 2 successes");
    require(shell.failure_count == 1, "shell should have 1 failure");

    const double expected = 2.0 / 3.0;
    require(std::abs(shell.success_rate() - expected) < 0.001, "success rate mismatch");

    const auto file_read = profiler.stats_for("file_read");
    require(file_read.call_count == 1, "file_read should have 1 call");
  }});

  tests.push_back({"profiler_unknown_tool_returns_zero", []() {
    ghostclaw::profiler::ToolProfiler profiler;
    const auto stats = profiler.stats_for("nonexistent_tool");
    require(stats.call_count == 0, "unknown tool should have 0 calls");
    require(stats.success_rate() == 0.0, "unknown tool success rate should be 0");
  }});

  tests.push_back({"profiler_sorted_by_failure_rate", []() {
    ghostclaw::profiler::ToolProfiler profiler;
    profiler.record("good_tool", true, std::chrono::milliseconds(10));
    profiler.record("good_tool", true, std::chrono::milliseconds(10));
    profiler.record("good_tool", true, std::chrono::milliseconds(10));
    profiler.record("bad_tool", false, std::chrono::milliseconds(10));
    profiler.record("bad_tool", false, std::chrono::milliseconds(10));
    profiler.record("bad_tool", true, std::chrono::milliseconds(10));

    const auto sorted = profiler.sorted_by_failure_rate();
    require(!sorted.empty(), "sorted list should not be empty");
    require(sorted[0].tool_name == "bad_tool", "worst tool should come first");
  }});

  tests.push_back({"profiler_sorted_by_latency", []() {
    ghostclaw::profiler::ToolProfiler profiler;
    profiler.record("fast_tool", true, std::chrono::milliseconds(5));
    profiler.record("slow_tool", true, std::chrono::milliseconds(5000));

    const auto sorted = profiler.sorted_by_latency();
    require(!sorted.empty(), "sorted list should not be empty");
    require(sorted[0].tool_name == "slow_tool", "slowest tool should come first");
  }});

  tests.push_back({"profiler_reset", []() {
    ghostclaw::profiler::ToolProfiler profiler;
    profiler.record("shell", true, std::chrono::milliseconds(10));
    require(profiler.total_calls() == 1, "should have 1 call before reset");
    profiler.reset();
    require(profiler.total_calls() == 0, "should have 0 calls after reset");
  }});

  tests.push_back({"profiler_format_report_content", []() {
    ghostclaw::profiler::ToolProfiler profiler;
    profiler.record("shell", true, std::chrono::milliseconds(50));
    profiler.record("file_read", false, std::chrono::milliseconds(20));

    const std::string report = profiler.format_report();
    require(report.find("shell") != std::string::npos, "report should contain 'shell'");
    require(report.find("file_read") != std::string::npos, "report should contain 'file_read'");
  }});

  // --- Conway MCP auto-injection logic ---
  tests.push_back({"conway_auto_inject_mcp_server", []() {
    ghostclaw::config::Config cfg;
    cfg.conway.enabled = true;
    cfg.conway.api_key = "cnwy_k_test_key";
    cfg.mcp.servers.clear();

    // Simulate auto-inject logic from load_config
    if (cfg.conway.enabled && !cfg.conway.api_key.empty()) {
      bool found = false;
      for (const auto &s : cfg.mcp.servers) {
        if (s.id == "conway") { found = true; break; }
      }
      if (!found) {
        ghostclaw::config::McpServerConfig mcp;
        mcp.id = "conway";
        mcp.command = "npx";
        mcp.args = {"conway-terminal"};
        mcp.env["CONWAY_API_KEY"] = cfg.conway.api_key;
        mcp.enabled = true;
        cfg.mcp.servers.push_back(std::move(mcp));
      }
    }

    require(cfg.mcp.servers.size() == 1, "should have exactly one MCP server");
    require(cfg.mcp.servers[0].id == "conway", "server id should be 'conway'");
    require(cfg.mcp.servers[0].command == "npx", "command should be 'npx'");
    require(!cfg.mcp.servers[0].args.empty(), "args should not be empty");
    require(cfg.mcp.servers[0].args[0] == "conway-terminal", "first arg should be 'conway-terminal'");
    require(cfg.mcp.servers[0].env.at("CONWAY_API_KEY") == "cnwy_k_test_key",
            "API key should be injected");
  }});

  tests.push_back({"conway_no_double_inject_when_server_exists", []() {
    ghostclaw::config::Config cfg;
    cfg.conway.enabled = true;
    cfg.conway.api_key = "cnwy_k_test";

    // Pre-configure a custom conway entry
    ghostclaw::config::McpServerConfig existing;
    existing.id = "conway";
    existing.command = "my-custom-conway";
    cfg.mcp.servers.push_back(std::move(existing));

    // Simulate auto-inject logic — should NOT overwrite
    if (cfg.conway.enabled && !cfg.conway.api_key.empty()) {
      bool found = false;
      for (const auto &s : cfg.mcp.servers) {
        if (s.id == "conway") { found = true; break; }
      }
      if (!found) {
        ghostclaw::config::McpServerConfig mcp;
        mcp.id = "conway";
        mcp.command = "npx";
        cfg.mcp.servers.push_back(std::move(mcp));
      }
    }

    require(cfg.mcp.servers.size() == 1, "should still have exactly one MCP server");
    require(cfg.mcp.servers[0].command == "my-custom-conway",
            "custom server should not be overwritten");
  }});
}
