#include "test_framework.hpp"

#include "ghostclaw/config/config.hpp"
#include "ghostclaw/mcp/client.hpp"
#include "ghostclaw/mcp/tool.hpp"

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

void register_mcp_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;

  tests.push_back({"mcp_config_load_servers", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"

[mcp.servers.filesystem]
command = "npx"
args = ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
enabled = true

[mcp.servers.postgres]
command = "npx"
args = ["-y", "@modelcontextprotocol/server-postgres"]
env.DATABASE_URL = "postgresql://localhost/mydb"
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_mcp_servers";
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
    require(cfg.mcp.servers.size() == 2, "should have 2 servers");

    bool found_filesystem = false;
    bool found_postgres = false;
    for (const auto &s : cfg.mcp.servers) {
      if (s.id == "filesystem") {
        found_filesystem = true;
        require(s.command == "npx", "filesystem command should be npx");
        require(s.args.size() == 3, "filesystem should have 3 args");
        require(s.enabled == true, "filesystem should be enabled");
      }
      if (s.id == "postgres") {
        found_postgres = true;
        require(s.command == "npx", "postgres command should be npx");
        require(s.env.count("DATABASE_URL") == 1, "should have DATABASE_URL env");
        require(s.env.at("DATABASE_URL") == "postgresql://localhost/mydb", "DATABASE_URL mismatch");
      }
    }
    require(found_filesystem, "should find filesystem server");
    require(found_postgres, "should find postgres server");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"mcp_config_empty", [] {
    const std::string toml_content = R"(
default_provider = "anthropic"
default_model = "test"
)";

    auto tmp_dir = std::filesystem::temp_directory_path() / "ghostclaw_test_mcp_empty";
    std::filesystem::create_directories(tmp_dir);
    auto config_file = tmp_dir / "config.toml";
    {
      std::ofstream f(config_file);
      f << toml_content;
    }

    ConfigOverrideGuard guard(tmp_dir);
    auto loaded = ghostclaw::config::load_config();
    require(loaded.ok(), "config should load");
    require(loaded.value().mcp.servers.empty(), "servers should be empty");

    std::filesystem::remove_all(tmp_dir);
  }});

  tests.push_back({"mcp_tool_name_format", [] {
    ghostclaw::config::McpServerConfig server_config;
    server_config.id = "filesystem";
    server_config.command = "npx";

    auto client = std::make_shared<ghostclaw::mcp::McpClient>(server_config);

    ghostclaw::mcp::McpToolInfo info;
    info.name = "read_file";
    info.description = "Read a file";
    info.input_schema_json = R"({"type":"object","properties":{}})";

    ghostclaw::mcp::McpTool tool("filesystem", info, client);
    require(tool.name() == "mcp_filesystem_read_file", "tool name should be mcp_filesystem_read_file");
  }});

  tests.push_back({"mcp_tool_name_format_with_dashes", [] {
    ghostclaw::config::McpServerConfig server_config;
    server_config.id = "test";
    server_config.command = "test";

    auto client = std::make_shared<ghostclaw::mcp::McpClient>(server_config);

    ghostclaw::mcp::McpToolInfo info;
    info.name = "list-directory";
    info.description = "List directory";
    info.input_schema_json = R"({"type":"object","properties":{}})";

    ghostclaw::mcp::McpTool tool("test", info, client);
    require(tool.name() == "mcp_test_list_directory", "dashes should become underscores");
  }});

  tests.push_back({"mcp_tool_is_not_safe", [] {
    ghostclaw::config::McpServerConfig server_config;
    server_config.id = "test";
    server_config.command = "test";

    auto client = std::make_shared<ghostclaw::mcp::McpClient>(server_config);

    ghostclaw::mcp::McpToolInfo info;
    info.name = "test_tool";
    info.description = "A test tool";
    info.input_schema_json = R"({"type":"object"})";

    ghostclaw::mcp::McpTool tool("test", info, client);
    require(!tool.is_safe(), "MCP tools should not be safe");
    require(tool.group() == "mcp", "MCP tools should be in 'mcp' group");
  }});

  tests.push_back({"mcp_config_validation", [] {
    ghostclaw::config::Config config;
    ghostclaw::config::McpServerConfig server;
    server.id = "empty_cmd";
    server.command = "";
    server.enabled = true;
    config.mcp.servers.push_back(server);

    auto result = ghostclaw::config::validate_config(config);
    require(result.ok(), "should succeed with warning");
    bool found_warning = false;
    for (const auto &w : result.value()) {
      if (w.find("empty command") != std::string::npos) {
        found_warning = true;
      }
    }
    require(found_warning, "should warn about empty command");
  }});
}
