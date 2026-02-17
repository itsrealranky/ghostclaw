#include "ghostclaw/mcp/tool.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"

#include <algorithm>
#include <sstream>

namespace ghostclaw::mcp {

McpTool::McpTool(std::string server_id, McpToolInfo info, std::shared_ptr<McpClient> client)
    : server_id_(std::move(server_id)), info_(std::move(info)), client_(std::move(client)) {
  // Build qualified name: mcp_<server>_<tool>
  std::string tool_name = info_.name;
  std::replace(tool_name.begin(), tool_name.end(), '-', '_');
  std::replace(tool_name.begin(), tool_name.end(), '/', '_');
  qualified_name_ = "mcp_" + server_id_ + "_" + common::to_lower(tool_name);
}

std::string_view McpTool::name() const { return qualified_name_; }

std::string_view McpTool::description() const { return info_.description; }

std::string McpTool::parameters_schema() const { return info_.input_schema_json; }

common::Result<tools::ToolResult> McpTool::execute(const tools::ToolArgs &args,
                                                    const tools::ToolContext &) {
  // Convert ToolArgs map to JSON object
  std::ostringstream json;
  json << '{';
  bool first = true;
  for (const auto &[key, value] : args) {
    if (!first) json << ',';
    json << '"' << common::json_escape(key) << '"' << ':' << '"' << common::json_escape(value) << '"';
    first = false;
  }
  json << '}';

  auto result = client_->call_tool(info_.name, json.str());
  if (!result.ok()) {
    return common::Result<tools::ToolResult>::failure(result.error());
  }

  tools::ToolResult tool_result;
  tool_result.output = result.value();
  tool_result.success = true;
  return common::Result<tools::ToolResult>::success(std::move(tool_result));
}

bool McpTool::is_safe() const { return false; }

std::string_view McpTool::group() const { return "mcp"; }

} // namespace ghostclaw::mcp
