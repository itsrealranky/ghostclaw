#include "ghostclaw/tools/tool_registry.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/config/config.hpp"
#include "ghostclaw/tools/builtin/browser.hpp"
#include "ghostclaw/tools/builtin/calendar.hpp"
#include "ghostclaw/tools/builtin/canvas.hpp"
#include "ghostclaw/tools/builtin/email.hpp"
#include "ghostclaw/tools/builtin/file_edit.hpp"
#include "ghostclaw/tools/builtin/file_read.hpp"
#include "ghostclaw/tools/builtin/file_write.hpp"
#include "ghostclaw/tools/builtin/memory_forget.hpp"
#include "ghostclaw/tools/builtin/memory_recall.hpp"
#include "ghostclaw/tools/builtin/memory_store.hpp"
#include "ghostclaw/tools/builtin/message.hpp"
#include "ghostclaw/tools/builtin/notify.hpp"
#include "ghostclaw/tools/builtin/reminder.hpp"
#include "ghostclaw/tools/builtin/sessions.hpp"
#include "ghostclaw/tools/builtin/shell.hpp"
#include "ghostclaw/tools/builtin/profiler_tools.hpp"
#include "ghostclaw/tools/builtin/skill_agent.hpp"
#include "ghostclaw/tools/builtin/skills.hpp"
#include "ghostclaw/tools/builtin/soul.hpp"
#include "ghostclaw/tools/builtin/web_fetch.hpp"
#include "ghostclaw/tools/builtin/web_search.hpp"
#include "ghostclaw/mcp/manager.hpp"
#include "ghostclaw/profiler/tool_profiler.hpp"

#include <iostream>
#include <memory>

namespace ghostclaw::tools {

void ToolRegistry::register_tool(std::unique_ptr<ITool> tool) {
  ITool *raw = tool.get();
  by_name_[common::to_lower(std::string(raw->name()))] = raw;
  tools_.push_back(std::move(tool));
}

ITool *ToolRegistry::get_tool(const std::string_view name) const {
  const auto it = by_name_.find(common::to_lower(std::string(name)));
  if (it == by_name_.end()) {
    return nullptr;
  }
  return it->second;
}

std::vector<ToolSpec> ToolRegistry::all_specs() const {
  std::vector<ToolSpec> specs;
  specs.reserve(tools_.size());
  for (const auto &tool : tools_) {
    specs.push_back(tool->spec());
  }
  return specs;
}

std::vector<ITool *> ToolRegistry::all_tools() const {
  std::vector<ITool *> out;
  out.reserve(tools_.size());
  for (const auto &tool : tools_) {
    out.push_back(tool.get());
  }
  return out;
}

ToolRegistry ToolRegistry::create_default(std::shared_ptr<security::SecurityPolicy> policy) {
  ToolRegistry registry;
  registry.register_tool(std::make_unique<ShellTool>(policy));
  registry.register_tool(std::make_unique<FileReadTool>(policy));
  registry.register_tool(std::make_unique<FileWriteTool>(policy));
  registry.register_tool(std::make_unique<FileEditTool>(policy));
  registry.register_tool(std::make_unique<WebSearchTool>());
  registry.register_tool(std::make_unique<WebFetchTool>());
  return registry;
}

ToolRegistry ToolRegistry::create_full(std::shared_ptr<security::SecurityPolicy> policy,
                                       memory::IMemory *memory, const config::Config &config) {
  // Create default tools but replace WebSearchTool with a configured one
  ToolRegistry registry;
  registry.register_tool(std::make_unique<ShellTool>(policy));
  registry.register_tool(std::make_unique<FileReadTool>(policy));
  registry.register_tool(std::make_unique<FileWriteTool>(policy));
  registry.register_tool(std::make_unique<FileEditTool>(policy));

  WebSearchConfig ws_config;
  ws_config.provider = config.web_search.provider;
  if (config.web_search.brave_api_key.has_value()) {
    ws_config.brave_api_key = *config.web_search.brave_api_key;
  }
  registry.register_tool(std::make_unique<WebSearchTool>(std::move(ws_config)));
  registry.register_tool(std::make_unique<WebFetchTool>());

  registry.register_tool(std::make_unique<BrowserTool>(config.browser.allowed_domains, config.browser));
  registry.register_tool(std::make_unique<CanvasTool>());
  registry.register_tool(std::make_unique<SkillsTool>());
  registry.register_tool(std::make_unique<CalendarTool>(config));
  registry.register_tool(std::make_unique<EmailTool>(config));
  registry.register_tool(std::make_unique<NotifyTool>());
  registry.register_tool(std::make_unique<ReminderTool>(config));
  registry.register_tool(std::make_unique<MessageTool>(config));

  if (memory != nullptr) {
    registry.register_tool(std::make_unique<MemoryStoreTool>(memory));
    registry.register_tool(std::make_unique<MemoryRecallTool>(memory));
    registry.register_tool(std::make_unique<MemoryForgetTool>(memory));
  }

  std::filesystem::path sessions_root;
  auto workspace = config::workspace_dir();
  if (workspace.ok()) {
    sessions_root = workspace.value() / "sessions";
  } else {
    sessions_root = std::filesystem::temp_directory_path() / "ghostclaw-sessions-fallback";
  }
  auto session_store = std::make_shared<sessions::SessionStore>(sessions_root);
  registry.register_tool(std::make_unique<SessionsListTool>(session_store));
  registry.register_tool(std::make_unique<SessionsHistoryTool>(session_store));
  registry.register_tool(std::make_unique<SessionsSendTool>(session_store));
  registry.register_tool(std::make_unique<SessionsSpawnTool>(session_store));
  registry.register_tool(std::make_unique<SubagentsTool>(session_store));

  // Soul tools — always register; only write when soul.enabled
  registry.register_tool(std::make_unique<SoulUpdateTool>(config.soul));
  registry.register_tool(std::make_unique<SoulReflectTool>(config.soul));
  registry.register_tool(std::make_unique<SoulReadTool>());

  // Autonomous skill acquisition tools
  registry.register_tool(std::make_unique<SkillDiscoverTool>());
  registry.register_tool(std::make_unique<SkillAutoInstallTool>());
  registry.register_tool(std::make_unique<SkillCreateTool>());

  // Tool profiler and self-optimization
  auto profiler = std::make_shared<profiler::ToolProfiler>();
  registry.register_tool(std::make_unique<ToolProfileReportTool>(profiler));
  registry.register_tool(std::make_unique<SelfOptimizeTool>(profiler));

  // Register MCP tools from configured servers
  if (!config.mcp.servers.empty()) {
    auto mcp_manager = std::make_shared<mcp::McpManager>(config.mcp.servers);
    auto mcp_status = mcp_manager->start_all();
    if (!mcp_status.ok()) {
      std::cerr << "[tools] MCP warning: " << mcp_status.error() << "\n";
    }
    auto mcp_tools = mcp_manager->collect_tools();
    for (auto &tool : mcp_tools) {
      registry.register_tool(std::move(tool));
    }
    // McpManager kept alive via shared_ptr in McpTool instances
  }

  return registry;
}

} // namespace ghostclaw::tools
