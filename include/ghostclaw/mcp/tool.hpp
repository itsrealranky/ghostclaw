#pragma once

#include "ghostclaw/mcp/client.hpp"
#include "ghostclaw/tools/tool.hpp"

#include <memory>
#include <string>

namespace ghostclaw::mcp {

class McpTool final : public tools::ITool {
public:
  McpTool(std::string server_id, McpToolInfo info, std::shared_ptr<McpClient> client);

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<tools::ToolResult> execute(const tools::ToolArgs &args,
                                                           const tools::ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override;
  [[nodiscard]] std::string_view group() const override;

private:
  std::string server_id_;
  McpToolInfo info_;
  std::shared_ptr<McpClient> client_;
  std::string qualified_name_;
};

} // namespace ghostclaw::mcp
