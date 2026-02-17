#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/mcp/client.hpp"
#include "ghostclaw/tools/tool.hpp"

#include <memory>
#include <vector>

namespace ghostclaw::mcp {

class McpManager {
public:
  explicit McpManager(const std::vector<config::McpServerConfig> &servers);
  ~McpManager();

  McpManager(const McpManager &) = delete;
  McpManager &operator=(const McpManager &) = delete;

  [[nodiscard]] common::Status start_all();
  void stop_all();

  [[nodiscard]] std::vector<std::unique_ptr<tools::ITool>> collect_tools();

private:
  std::vector<std::shared_ptr<McpClient>> clients_;
};

} // namespace ghostclaw::mcp
