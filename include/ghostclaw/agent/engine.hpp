#pragma once

#include "ghostclaw/agent/context.hpp"
#include "ghostclaw/agent/tool_executor.hpp"
#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/memory/memory.hpp"
#include "ghostclaw/providers/traits.hpp"
#include "ghostclaw/tools/tool_registry.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ghostclaw::agent {

struct AgentOptions {
  std::optional<std::string> provider_override;
  std::optional<std::string> model_override;
  std::optional<double> temperature_override;
  std::optional<std::string> session_id;
  std::optional<std::string> agent_id;
  std::optional<std::string> channel_id;
  std::optional<std::string> group_id;
  std::optional<std::string> tool_profile;
  std::size_t max_tool_iterations = 10;
};

struct Usage {
  std::size_t prompt_tokens = 0;
  std::size_t completion_tokens = 0;
  std::size_t total_tokens = 0;
};

struct AgentResponse {
  std::string content;
  std::vector<ToolCallResult> tool_results;
  Usage usage;
  std::chrono::milliseconds duration{0};
};

struct StreamCallbacks {
  std::function<void(std::string_view)> on_token;
  std::function<void(const AgentResponse &)> on_done;
  std::function<void(const std::string &)> on_error;
};

class AgentEngine {
public:
  AgentEngine(const config::Config &config, std::shared_ptr<providers::Provider> provider,
              std::unique_ptr<memory::IMemory> memory, tools::ToolRegistry tools,
              std::filesystem::path workspace,
              std::vector<std::string> skill_instructions = {});

  [[nodiscard]] common::Result<AgentResponse> run(const std::string &message,
                                                  const AgentOptions &options = {});
  [[nodiscard]] common::Status run_stream(const std::string &message,
                                          const StreamCallbacks &callbacks,
                                          const AgentOptions &options = {});
  [[nodiscard]] common::Status run_interactive(const AgentOptions &options = {});

  [[nodiscard]] std::string build_system_prompt();
  [[nodiscard]] std::string build_memory_context(const std::string &message);

private:
  [[nodiscard]] common::Result<AgentResponse>
  process_with_tools(const std::string &message, const std::string &system_prompt,
                     const std::string &memory_context, const AgentOptions &options);

  [[nodiscard]] bool detect_prompt_injection(const std::string &input) const;
  [[nodiscard]] bool detect_prompt_leak(const std::string &output) const;
  [[nodiscard]] std::string build_relevant_skill_context(const std::string &message) const;
  void ensure_skill_catalog_loaded();

  const config::Config &config_;
  std::shared_ptr<providers::Provider> provider_;
  std::unique_ptr<memory::IMemory> memory_;
  tools::ToolRegistry tools_;
  ToolExecutor tool_executor_;
  ContextBuilder context_builder_;
  std::filesystem::path workspace_;
  std::vector<std::string> skill_instructions_;
  std::vector<std::string> skill_prompts_;
  std::vector<std::string> skill_index_entries_;
  bool skills_loaded_ = false;
  std::mutex skills_mutex_;
};

} // namespace ghostclaw::agent
