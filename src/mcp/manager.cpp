#include "ghostclaw/mcp/manager.hpp"

#include "ghostclaw/mcp/tool.hpp"

#include <iostream>

namespace ghostclaw::mcp {

McpManager::McpManager(const std::vector<config::McpServerConfig> &servers) {
  for (const auto &server : servers) {
    if (!server.enabled) continue;
    clients_.push_back(std::make_shared<McpClient>(server));
  }
}

McpManager::~McpManager() { stop_all(); }

common::Status McpManager::start_all() {
  std::vector<std::string> errors;

  for (auto &client : clients_) {
    auto status = client->start();
    if (!status.ok()) {
      std::cerr << "[mcp] failed to start server '" << client->server_id()
                << "': " << status.error() << "\n";
      errors.push_back(client->server_id() + ": " + status.error());
    }
  }

  if (errors.size() == clients_.size() && !clients_.empty()) {
    return common::Status::error("all MCP servers failed to start");
  }

  return common::Status::success();
}

void McpManager::stop_all() {
  for (auto &client : clients_) {
    client->stop();
  }
}

std::vector<std::unique_ptr<tools::ITool>> McpManager::collect_tools() {
  std::vector<std::unique_ptr<tools::ITool>> tools;

  for (auto &client : clients_) {
    if (!client->is_running()) continue;

    auto tool_list = client->list_tools();
    if (!tool_list.ok()) {
      std::cerr << "[mcp] failed to list tools for server '" << client->server_id()
                << "': " << tool_list.error() << "\n";
      continue;
    }

    for (auto &info : tool_list.value()) {
      tools.push_back(
          std::make_unique<McpTool>(client->server_id(), std::move(info), client));
    }
  }

  return tools;
}

} // namespace ghostclaw::mcp
