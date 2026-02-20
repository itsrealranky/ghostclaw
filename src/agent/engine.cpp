#include "ghostclaw/agent/engine.hpp"

#include "ghostclaw/agent/stream_parser.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/observability/global.hpp"
#include "ghostclaw/sandbox/sandbox.hpp"
#include "ghostclaw/security/approval.hpp"
#include "ghostclaw/security/external_content.hpp"
#include "ghostclaw/security/tool_policy.hpp"
#include "ghostclaw/skills/compat.hpp"
#include "ghostclaw/skills/registry.hpp"

#include <ctime>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace ghostclaw::agent {

namespace {

security::ExternalSource source_for_tool(const std::string_view name) {
  const std::string normalized = common::to_lower(std::string(name));
  if (normalized == "web_search") {
    return security::ExternalSource::WebSearch;
  }
  if (normalized == "web_fetch") {
    return security::ExternalSource::WebFetch;
  }
  if (normalized == "browser") {
    return security::ExternalSource::Browser;
  }
  return security::ExternalSource::Unknown;
}

bool should_wrap_tool_output(const std::string_view name) {
  const std::string normalized = common::to_lower(std::string(name));
  return normalized == "web_search" || normalized == "web_fetch" || normalized == "browser";
}

std::vector<skills::Skill> load_skill_catalog(const std::filesystem::path &workspace_path) {
  skills::SkillRegistry registry(workspace_path / "skills", workspace_path / ".community-skills");
  auto listed = registry.list_all();
  if (!listed.ok()) {
    observability::record_error("agent", "skills catalog load failed: " + listed.error());
    return {};
  }
  return listed.value();
}

std::vector<std::string> build_skill_index_entries(const std::vector<skills::Skill> &skills_list) {
  std::vector<std::string> out;
  out.reserve(skills_list.size());
  constexpr std::size_t kMaxEntries = 200;
  constexpr std::size_t kMaxDescChars = 180;
  for (const auto &skill : skills_list) {
    if (out.size() >= kMaxEntries) {
      break;
    }
    std::ostringstream line;
    line << skill.name << " [" << skills::skill_source_to_string(skill.source) << "]";
    if (!common::trim(skill.description).empty()) {
      std::string desc = skill.description;
      if (desc.size() > kMaxDescChars) {
        desc.resize(kMaxDescChars);
        desc += "...";
      }
      line << " - " << desc;
    }
    out.push_back(line.str());
  }
  return out;
}

std::vector<std::string> build_interactive_skill_prompts(const std::vector<skills::Skill> &skills_list) {
  std::vector<std::string> prompts;
  prompts.reserve(skills_list.size());
  for (const auto &skill : skills_list) {
    std::string body = skills::prepared_skill_instructions(skill, 3000, false);
    if (body.empty()) {
      continue;
    }
    prompts.push_back(skill.name + ":\n" + body);
  }
  return prompts;
}

} // namespace

AgentEngine::AgentEngine(const config::Config &config, std::shared_ptr<providers::Provider> provider,
                         std::unique_ptr<memory::IMemory> memory, tools::ToolRegistry tools,
                         std::filesystem::path workspace,
                         std::vector<std::string> skill_instructions)
    : config_(config), provider_(std::move(provider)), memory_(std::move(memory)),
      tools_(std::move(tools)), tool_executor_(tools_), context_builder_(workspace, config.identity),
      workspace_(std::move(workspace)), skill_instructions_(std::move(skill_instructions)) {
  auto tool_policy = std::make_shared<security::ToolPolicyPipeline>();
  if (!config_.tools.allow.groups.empty() || !config_.tools.allow.tools.empty() ||
      !config_.tools.allow.deny.empty()) {
    security::ToolPolicy policy;
    policy.allow.reserve(config_.tools.allow.groups.size() + config_.tools.allow.tools.size());
    for (const auto &group : config_.tools.allow.groups) {
      policy.allow.push_back(group);
    }
    for (const auto &tool : config_.tools.allow.tools) {
      policy.allow.push_back(tool);
    }
    policy.deny = config_.tools.allow.deny;
    tool_policy->set_global_policy(std::move(policy));
  }
  tool_executor_.set_tool_policy_pipeline(tool_policy);

  sandbox::SandboxConfig sandbox_config;
  sandbox_config.mode = sandbox::SandboxConfig::Mode::Off;
  auto sandbox_manager = std::make_shared<sandbox::SandboxManager>(sandbox_config);
  tool_executor_.set_sandbox_manager(sandbox_manager);

  security::ApprovalPolicy approval_policy;
  approval_policy.security = security::ExecSecurity::Full;
  approval_policy.ask = security::ExecAsk::Off;
  auto approval_manager = std::make_shared<security::ApprovalManager>(approval_policy);
  tool_executor_.set_approval_manager(approval_manager);
}

std::string AgentEngine::build_system_prompt() {
  ensure_skill_catalog_loaded();
  return context_builder_.build_system_prompt(tools_.all_specs(), skill_index_entries_);
}

std::string AgentEngine::build_memory_context(const std::string &message) {
  auto recalled = memory_->recall(message, 5);
  if (!recalled.ok() || recalled.value().empty()) {
    return "";
  }

  std::ostringstream out;
  out << "[Memory Context]\n";
  for (const auto &entry : recalled.value()) {
    if (entry.score.has_value() && *entry.score < 0.3) {
      continue;
    }

    std::string preview = entry.content;
    if (preview.size() > 100) {
      preview.resize(100);
    }

    out << "- " << entry.key << ": " << preview << " (category: "
        << memory::category_to_string(entry.category) << ", relevance: "
        << (entry.score.has_value() ? *entry.score : 0.0) << ")\n";
  }
  out << "[End Memory Context]\n";
  return out.str();
}

void AgentEngine::ensure_skill_catalog_loaded() {
  std::lock_guard<std::mutex> lock(skills_mutex_);
  if (skills_loaded_) {
    return;
  }

  const auto skill_catalog = load_skill_catalog(workspace_);
  skill_index_entries_ = build_skill_index_entries(skill_catalog);
  skill_prompts_ = build_interactive_skill_prompts(skill_catalog);

  // Merge in skill instructions passed from the runtime (e.g. bundled skills).
  for (auto &instr : skill_instructions_) {
    if (!instr.empty()) {
      skill_prompts_.push_back(std::move(instr));
    }
  }

  skills_loaded_ = true;
}

std::string AgentEngine::build_relevant_skill_context(const std::string &message) const {
  const std::string query = common::trim(message);
  if (query.empty()) {
    return "";
  }

  skills::SkillRegistry registry(workspace_ / "skills", workspace_ / ".community-skills");
  auto searched = registry.search(query, true);
  if (!searched.ok()) {
    return "";
  }

  constexpr std::size_t kMaxSkills = 3;
  constexpr std::size_t kPerSkillChars = 6000;
  constexpr std::size_t kTotalChars = 14000;
  constexpr double kMinScore = 18.0;

  std::ostringstream out;
  std::size_t appended = 0;
  std::size_t emitted = 0;

  for (const auto &entry : searched.value()) {
    if (emitted >= kMaxSkills || entry.score < kMinScore) {
      break;
    }

    std::string text =
        skills::prepared_skill_instructions(entry.skill, kPerSkillChars, true);
    if (text.empty()) {
      continue;
    }

    if (appended + text.size() > kTotalChars) {
      const std::size_t remaining = kTotalChars > appended ? (kTotalChars - appended) : 0;
      if (remaining == 0) {
        break;
      }
      if (text.size() > remaining) {
        text.resize(remaining);
        text += "\n[truncated]";
      }
    }

    out << "[Skill: " << entry.skill.name << " | source="
        << skills::skill_source_to_string(entry.skill.source)
        << " | score=" << std::fixed << std::setprecision(1) << entry.score << "]\n";
    out << text << "\n\n";
    appended += text.size();
    ++emitted;
  }

  const std::string context = out.str();
  if (context.empty()) {
    return "";
  }
  return "[Relevant Skill Instructions]\n" + context + "[End Relevant Skill Instructions]\n";
}

bool AgentEngine::detect_prompt_injection(const std::string &input) const {
  return !security::detect_suspicious_patterns(input).empty();
}

bool AgentEngine::detect_prompt_leak(const std::string &output) const {
  const std::string lower = common::to_lower(output);
  return lower.find("## safety guidelines") != std::string::npos ||
         lower.find("you are ghostclaw") != std::string::npos;
}

common::Result<AgentResponse> AgentEngine::process_with_tools(const std::string &message,
                                                              const std::string &system_prompt,
                                                              const std::string &memory_context,
                                                              const AgentOptions &options) {
  const std::string model = options.model_override.value_or(config_.default_model);
  const double temperature = options.temperature_override.value_or(config_.default_temperature);

  std::string current_prompt = message;
  std::vector<ToolCallResult> all_tool_results;
  std::string final_content;

  for (std::size_t iter = 0; iter < options.max_tool_iterations; ++iter) {
    auto response = provider_->chat_with_system_tools(
        system_prompt + "\n" + memory_context, current_prompt, model, temperature, tools_.all_specs());
    if (!response.ok()) {
      return common::Result<AgentResponse>::failure(response.error());
    }

    StreamParser parser;
    parser.feed(response.value());
    parser.finish();

    auto calls = parser.tool_calls();
    final_content = parser.accumulated_content();

    if (calls.empty()) {
      break;
    }

    std::vector<ToolCallRequest> requests;
    for (const auto &call : calls) {
      requests.push_back(ToolCallRequest{.id = call.id, .name = call.name, .arguments = call.arguments});
    }

    tools::ToolContext ctx;
    ctx.workspace_path = workspace_;
    ctx.session_id = options.session_id.value_or("default");
    ctx.agent_id = options.agent_id.value_or("ghostclaw");
    ctx.main_session_id = options.session_id.value_or("main");
    ctx.provider = config_.default_provider;
    ctx.tool_profile = options.tool_profile.value_or(config_.tools.profile);
    ctx.channel_id = options.channel_id.value_or("");
    ctx.group_id = options.group_id.value_or("");
    ctx.sandbox_enabled = true;

    auto results = tool_executor_.execute(requests, ctx);
    all_tool_results.insert(all_tool_results.end(), results.begin(), results.end());

    std::ostringstream next_message;
    next_message << message << "\n\nTool results:\n";
    for (const auto &result : results) {
      std::string output = result.result.output;
      if (should_wrap_tool_output(result.name)) {
        output = security::wrap_external_content(output, source_for_tool(result.name),
                                                 std::nullopt, std::nullopt, true);
      }
      next_message << "- " << result.id << " (" << (result.result.success ? "ok" : "error")
                   << "): " << output << "\n";
    }
    current_prompt = next_message.str();
  }

  AgentResponse out;
  out.content = final_content;
  out.tool_results = std::move(all_tool_results);
  return common::Result<AgentResponse>::success(std::move(out));
}

common::Result<AgentResponse> AgentEngine::run(const std::string &message,
                                                const AgentOptions &options) {
  ensure_skill_catalog_loaded();
  const auto start = std::chrono::steady_clock::now();
  observability::record_agent_start(provider_->name(),
                                    options.model_override.value_or(config_.default_model));

  if (detect_prompt_injection(message)) {
    std::cerr << "[warn] possible prompt injection detected\n";
  }

  const std::string system_prompt = build_system_prompt();
  std::string context = build_memory_context(message);
  const std::string skills_context = build_relevant_skill_context(message);
  if (!skills_context.empty()) {
    if (!context.empty()) {
      context += "\n";
    }
    context += skills_context;
  }

  auto result = process_with_tools(message, system_prompt, context, options);
  if (!result.ok()) {
    observability::record_error("agent", result.error());
    return result;
  }

  if (detect_prompt_leak(result.value().content)) {
    std::cerr << "[warn] possible system prompt leak detected\n";
  }

  if (config_.memory.auto_save) {
    const std::string key = "conversation_" + std::to_string(std::time(nullptr));
    (void)memory_->store(key, "User: " + message + "\nAssistant: " + result.value().content,
                         memory::MemoryCategory::Daily);
  }

  const auto end = std::chrono::steady_clock::now();
  result.value().duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  observability::record_metric(
      observability::RequestLatencyMetric{.latency = result.value().duration});
  for (const auto &tool_result : result.value().tool_results) {
    observability::record_tool_call(tool_result.name, std::chrono::milliseconds(0),
                                    tool_result.result.success);
  }
  if (result.value().usage.total_tokens > 0) {
    observability::record_metric(
        observability::TokensUsedMetric{.tokens = result.value().usage.total_tokens});
    observability::record_agent_end(
        result.value().duration,
        static_cast<std::uint64_t>(result.value().usage.total_tokens));
  } else {
    observability::record_agent_end(result.value().duration);
  }
  return result;
}

common::Status AgentEngine::run_stream(const std::string &message, const StreamCallbacks &callbacks,
                                       const AgentOptions &options) {
  ensure_skill_catalog_loaded();
  // Keep tool-capable runs on the existing full response path to avoid exposing intermediate tool payloads.
  if (!tools_.all_specs().empty()) {
    auto result = run(message, options);
    if (!result.ok()) {
      if (callbacks.on_error) {
        callbacks.on_error(result.error());
      }
      return common::Status::error(result.error());
    }

    std::istringstream stream(result.value().content);
    std::string token;
    while (stream >> token) {
      if (callbacks.on_token) {
        callbacks.on_token(token);
      }
    }

    if (callbacks.on_done) {
      callbacks.on_done(result.value());
    }
    return common::Status::success();
  }

  const auto start = std::chrono::steady_clock::now();
  observability::record_agent_start(provider_->name(),
                                    options.model_override.value_or(config_.default_model));

  if (detect_prompt_injection(message)) {
    std::cerr << "[warn] possible prompt injection detected\n";
  }

  const std::string system_prompt = build_system_prompt();
  std::string context = build_memory_context(message);
  const std::string skills_context = build_relevant_skill_context(message);
  if (!skills_context.empty()) {
    if (!context.empty()) {
      context += "\n";
    }
    context += skills_context;
  }
  const std::string model = options.model_override.value_or(config_.default_model);
  const double temperature = options.temperature_override.value_or(config_.default_temperature);

  auto streamed = provider_->chat_with_system_stream(
      system_prompt + "\n" + context, message, model, temperature,
      [&](std::string_view chunk) {
        if (callbacks.on_token) {
          callbacks.on_token(chunk);
        }
      });
  if (!streamed.ok()) {
    observability::record_error("agent", streamed.error());
    if (callbacks.on_error) {
      callbacks.on_error(streamed.error());
    }
    return common::Status::error(streamed.error());
  }

  AgentResponse response;
  response.content = streamed.value();
  if (detect_prompt_leak(response.content)) {
    std::cerr << "[warn] possible system prompt leak detected\n";
  }

  if (config_.memory.auto_save) {
    const std::string key = "conversation_" + std::to_string(std::time(nullptr));
    (void)memory_->store(key, "User: " + message + "\nAssistant: " + response.content,
                         memory::MemoryCategory::Daily);
  }

  const auto end = std::chrono::steady_clock::now();
  response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  observability::record_metric(observability::RequestLatencyMetric{.latency = response.duration});
  observability::record_agent_end(response.duration);

  if (callbacks.on_done) {
    callbacks.on_done(response);
  }
  return common::Status::success();
}

common::Status AgentEngine::run_interactive(const AgentOptions &options) {
  ensure_skill_catalog_loaded();
  // ── ANSI Escape Codes ──
  constexpr const char *RESET     = "\033[0m";
  constexpr const char *BOLD      = "\033[1m";
  constexpr const char *DIM       = "\033[2m";
  constexpr const char *ITALIC    = "\033[3m";
  constexpr const char *CYAN      = "\033[36m";
  constexpr const char *GREEN     = "\033[32m";
  constexpr const char *YELLOW    = "\033[33m";
  constexpr const char *MAGENTA   = "\033[35m";
  constexpr const char *RED       = "\033[31m";
  constexpr const char *BLUE      = "\033[34m";
  constexpr const char *BG_DARK   = "\033[48;5;236m";

  // ── Header Banner ──
  std::cout << "\n";
  std::cout << BOLD << CYAN << "  ╔══════════════════════════════════════════════════╗\n";
  std::cout <<                   "  ║        🐾  GhostClaw Interactive Agent          ║\n";
  std::cout <<                   "  ╚══════════════════════════════════════════════════╝" << RESET << "\n\n";

  std::cout << DIM << "  Provider: " << RESET << BOLD << provider_->name() << RESET
            << DIM << "  •  Model: " << RESET << BOLD << config_.default_model << RESET
            << DIM << "  •  Tools: " << RESET << BOLD << tools_.all_specs().size() << RESET << "\n";

  if (!skill_prompts_.empty()) {
    std::cout << DIM << "  Skills: " << RESET << BOLD << skill_prompts_.size() << " loaded" << RESET << "\n";
  }

  std::cout << "\n" << DIM << "  Type " << RESET << BOLD << "/help" << RESET
            << DIM << " for commands, " << RESET << BOLD << "/quit" << RESET
            << DIM << " to exit" << RESET << "\n";
  std::cout << DIM << "  Use " << RESET << BOLD << "\\" << RESET
            << DIM << " at end of line for multi-line input" << RESET << "\n\n";

  // ── State ──
  std::vector<std::pair<std::string, std::string>> history;
  std::size_t total_tokens = 0;
  std::size_t message_count = 0;

  // ── Skill listing helper ──
  auto list_skills = [&]() {
    if (skill_prompts_.empty()) {
      std::cout << YELLOW << "  No skills loaded." << RESET << "\n";
      std::cout << DIM << "  Run 'ghostclaw skills list' to see available skills." << RESET << "\n";
      return;
    }
    std::cout << "\n" << BOLD << MAGENTA << "  ── Loaded Skills ──" << RESET << "\n\n";
    for (std::size_t i = 0; i < skill_prompts_.size(); ++i) {
      const auto &sp = skill_prompts_[i];
      const auto colon = sp.find(':');
      std::string skill_name = (colon != std::string::npos) ? sp.substr(0, colon) : sp.substr(0, 30);
      std::cout << "  " << BOLD << GREEN << (i + 1) << ")" << RESET << " " << skill_name << "\n";
    }
    std::cout << "\n" << DIM << "  Use /skill <name> to see full skill instructions" << RESET << "\n\n";
  };

  // ── Tool listing helper ──
  auto list_tools = [&]() {
    auto specs = tools_.all_specs();
    if (specs.empty()) {
      std::cout << YELLOW << "  No tools registered." << RESET << "\n";
      return;
    }
    std::cout << "\n" << BOLD << BLUE << "  ── Available Tools ──" << RESET << "\n\n";
    for (const auto &spec : specs) {
      std::cout << "  " << BOLD << CYAN << "• " << spec.name << RESET;
      if (!spec.description.empty()) {
        std::cout << DIM << " — " << spec.description << RESET;
      }
      std::cout << "\n";
    }
    std::cout << "\n";
  };

  // ── Help display ──
  auto show_help = [&]() {
    std::cout << "\n" << BOLD << "  ── Commands ──" << RESET << "\n\n";
    std::cout << "  " << BOLD << GREEN << "/help" << RESET << DIM << "       Show this help message" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/quit" << RESET << DIM << "       Exit interactive mode" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/clear" << RESET << DIM << "      Clear conversation history" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/skills" << RESET << DIM << "     List all loaded skills" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/skill" << RESET << " <n>" << DIM << "  Show details for a skill" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/tools" << RESET << DIM << "      List available tools" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/model" << RESET << DIM << "      Show current model info" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/memory" << RESET << DIM << "     Show memory statistics" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/status" << RESET << DIM << "     Show agent status overview" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/history" << RESET << DIM << "    Show conversation history" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/export" << RESET << DIM << "     Export conversation to file" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/compact" << RESET << DIM << "    Compact history (keep last 10)" << RESET << "\n";
    std::cout << "  " << BOLD << GREEN << "/tokens" << RESET << DIM << "     Show token usage this session" << RESET << "\n\n";
    std::cout << DIM << "  Tip: End a line with \\ for multi-line input" << RESET << "\n\n";
  };

  // ── Spinner helpers ──
  auto print_thinking = [&]() {
    std::cout << "\n" << DIM << "  ⠋ thinking..." << RESET << std::flush;
  };
  auto clear_thinking = [&]() {
    std::cout << "\r" << "                      " << "\r";
  };

  // ── Main REPL Loop ──
  while (true) {
    // Prompt
    std::cout << BOLD << CYAN << "  ❯ " << RESET;

    // Read input (with multi-line support)
    std::string input;
    std::string line;
    while (true) {
      if (!std::getline(std::cin, line)) {
        // EOF
        std::cout << "\n";
        goto done;
      }
      if (!line.empty() && line.back() == '\\') {
        input += line.substr(0, line.size() - 1) + "\n";
        std::cout << DIM << "  … " << RESET;
        continue;
      }
      input += line;
      break;
    }

    input = common::trim(input);
    if (input.empty()) {
      continue;
    }

    // ── Command Dispatch ──
    if (input == "/quit" || input == "/exit" || input == "/q") {
      break;
    }

    if (input == "/help" || input == "/?") {
      show_help();
      continue;
    }

    if (input == "/clear") {
      history.clear();
      std::cout << GREEN << "  ✓ History cleared." << RESET << "\n\n";
      continue;
    }

    if (input == "/skills" || input == "/skill") {
      list_skills();
      continue;
    }

    // /skill <name_or_number> - show details
    if (input.substr(0, 7) == "/skill ") {
      const std::string query = common::trim(input.substr(7));
      bool found = false;

      // Try numeric index first
      try {
        const auto idx = std::stoul(query);
        if (idx >= 1 && idx <= skill_prompts_.size()) {
          const auto &sp = skill_prompts_[idx - 1];
          const auto colon = sp.find(':');
          std::string name = (colon != std::string::npos) ? sp.substr(0, colon) : "skill";
          std::string body = (colon != std::string::npos) ? sp.substr(colon + 1) : sp;
          std::cout << "\n" << BOLD << MAGENTA << "  ── " << name << " ──" << RESET << "\n\n";
          // Print body with indentation
          std::istringstream ss(body);
          std::string bline;
          while (std::getline(ss, bline)) {
            std::cout << "  " << bline << "\n";
          }
          std::cout << "\n";
          found = true;
        }
      } catch (...) { /* not a number */ }

      // Try name match
      if (!found) {
        const std::string lower_query = common::to_lower(query);
        for (const auto &sp : skill_prompts_) {
          const auto colon = sp.find(':');
          std::string name = (colon != std::string::npos) ? sp.substr(0, colon) : "";
          if (common::to_lower(name).find(lower_query) != std::string::npos) {
            std::string body = (colon != std::string::npos) ? sp.substr(colon + 1) : sp;
            std::cout << "\n" << BOLD << MAGENTA << "  ── " << name << " ──" << RESET << "\n\n";
            std::istringstream ss(body);
            std::string bline;
            while (std::getline(ss, bline)) {
              std::cout << "  " << bline << "\n";
            }
            std::cout << "\n";
            found = true;
            break;
          }
        }
      }

      if (!found) {
        std::cout << RED << "  Skill not found: " << query << RESET << "\n";
        std::cout << DIM << "  Use /skills to see available skills" << RESET << "\n\n";
      }
      continue;
    }

    if (input == "/tools") {
      list_tools();
      continue;
    }

    if (input == "/model") {
      std::cout << "\n" << BOLD << "  ── Model Info ──" << RESET << "\n\n";
      std::cout << "  " << DIM << "Provider:" << RESET << "  " << BOLD << provider_->name() << RESET << "\n";
      std::cout << "  " << DIM << "Model:" << RESET << "     " << BOLD << config_.default_model << RESET << "\n";
      std::cout << "  " << DIM << "Temp:" << RESET << "      " << config_.default_temperature << "\n\n";
      continue;
    }

    if (input == "/memory") {
      const auto stats = memory_->stats();
      std::cout << "\n" << BOLD << "  ── Memory ──" << RESET << "\n\n";
      std::cout << "  " << DIM << "Entries:" << RESET << "    " << BOLD << stats.total_entries << RESET << "\n";
      std::cout << "  " << DIM << "Vectors:" << RESET << "    " << BOLD << stats.total_vectors << RESET << "\n";
      std::cout << "  " << DIM << "Backend:" << RESET << "    " << config_.memory.backend << "\n\n";
      continue;
    }

    if (input == "/status") {
      std::cout << "\n" << BOLD << "  ── Agent Status ──" << RESET << "\n\n";
      std::cout << "  " << DIM << "Provider:" << RESET << "    " << BOLD << provider_->name() << RESET << "\n";
      std::cout << "  " << DIM << "Model:" << RESET << "       " << BOLD << config_.default_model << RESET << "\n";
      std::cout << "  " << DIM << "Tools:" << RESET << "       " << tools_.all_specs().size() << " registered\n";
      std::cout << "  " << DIM << "Skills:" << RESET << "      " << skill_prompts_.size() << " loaded\n";
      std::cout << "  " << DIM << "Messages:" << RESET << "    " << message_count << " this session\n";
      std::cout << "  " << DIM << "Tokens:" << RESET << "      " << total_tokens << " used\n";
      std::cout << "  " << DIM << "History:" << RESET << "     " << history.size() / 2 << " exchanges\n";
      const auto mem_stats = memory_->stats();
      std::cout << "  " << DIM << "Memory:" << RESET << "      " << mem_stats.total_entries << " entries\n\n";
      continue;
    }

    if (input == "/history") {
      if (history.empty()) {
        std::cout << DIM << "  No conversation history yet." << RESET << "\n\n";
        continue;
      }
      std::cout << "\n" << BOLD << "  ── Conversation History ──" << RESET << "\n\n";
      for (std::size_t i = 0; i < history.size(); i += 2) {
        const auto turn = i / 2 + 1;
        std::cout << DIM << "  [" << turn << "] " << RESET
                  << BOLD << CYAN << "You:" << RESET << " " << history[i].second << "\n";
        if (i + 1 < history.size()) {
          std::string preview = history[i + 1].second;
          if (preview.size() > 120) {
            preview.resize(120);
            preview += "…";
          }
          std::cout << "      " << GREEN << "AI:" << RESET << "  " << preview << "\n";
        }
      }
      std::cout << "\n";
      continue;
    }

    if (input == "/export") {
      const auto timestamp = std::to_string(std::time(nullptr));
      const std::string filename = "ghostclaw-session-" + timestamp + ".md";
      std::ofstream out(filename);
      if (!out) {
        std::cout << RED << "  Failed to create " << filename << RESET << "\n\n";
        continue;
      }
      out << "# GhostClaw Session Export\n\n";
      out << "**Provider:** " << provider_->name() << "  \n";
      out << "**Model:** " << config_.default_model << "  \n";
      out << "**Messages:** " << message_count << "  \n";
      out << "**Tokens:** " << total_tokens << "  \n\n---\n\n";
      for (std::size_t i = 0; i < history.size(); i += 2) {
        out << "### User\n" << history[i].second << "\n\n";
        if (i + 1 < history.size()) {
          out << "### Assistant\n" << history[i + 1].second << "\n\n---\n\n";
        }
      }
      std::cout << GREEN << "  ✓ Exported to " << filename << RESET << "\n\n";
      continue;
    }

    if (input == "/compact") {
      const std::size_t keep = 20; // 10 exchanges = 20 entries
      if (history.size() > keep) {
        history.erase(history.begin(), history.end() - static_cast<long>(keep));
      }
      std::cout << GREEN << "  ✓ Compacted to last " << history.size() / 2 << " exchanges." << RESET << "\n\n";
      continue;
    }

    if (input == "/tokens") {
      std::cout << "\n" << BOLD << "  ── Token Usage ──" << RESET << "\n\n";
      std::cout << "  " << DIM << "Total tokens:" << RESET << "  " << BOLD << total_tokens << RESET << "\n";
      std::cout << "  " << DIM << "Messages:" << RESET << "      " << message_count << "\n\n";
      continue;
    }

    // Unknown command
    if (!input.empty() && input[0] == '/') {
      std::cout << RED << "  Unknown command: " << input << RESET << "\n";
      std::cout << DIM << "  Type /help for available commands" << RESET << "\n\n";
      continue;
    }

    // ── Send to AI ──
    print_thinking();

    std::ostringstream prompt_with_history;
    const std::size_t history_window = std::min<std::size_t>(history.size(), 40);
    const std::size_t history_start = history.size() > history_window ? history.size() - history_window : 0;
    for (std::size_t i = history_start; i < history.size(); ++i) {
      prompt_with_history << history[i].first << ": " << history[i].second << "\n";
    }
    prompt_with_history << "user: " << input;

    const auto start = std::chrono::steady_clock::now();
    auto response = run(prompt_with_history.str(), options);
    const auto end = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    clear_thinking();

    if (!response.ok()) {
      std::cout << RED << "  ✗ Error: " << response.error() << RESET << "\n\n";
      continue;
    }

    ++message_count;
    total_tokens += response.value().usage.total_tokens;

    // Display tool calls if any
    if (!response.value().tool_results.empty()) {
      std::cout << DIM << "  ── Tool Calls ──" << RESET << "\n";
      for (const auto &tr : response.value().tool_results) {
        const char *status_icon = tr.result.success ? "✓" : "✗";
        const char *status_color = tr.result.success ? GREEN : RED;
        std::cout << "  " << status_color << status_icon << RESET
                  << DIM << " " << tr.name << RESET << "\n";
      }
      std::cout << "\n";
    }

    // Display response
    std::cout << BOLD << GREEN << "  ⬤" << RESET << " ";

    // Smart output formatting: detect code blocks and render them
    const std::string &content = response.value().content;
    std::istringstream content_stream(content);
    std::string content_line;
    bool in_code_block = false;
    bool first_line = true;

    while (std::getline(content_stream, content_line)) {
      if (!first_line) {
        std::cout << "    ";
      }
      first_line = false;

      // Detect code block boundaries
      if (content_line.starts_with("```")) {
        if (!in_code_block) {
          in_code_block = true;
          std::cout << BG_DARK << DIM << content_line << RESET << "\n";
        } else {
          in_code_block = false;
          std::cout << BG_DARK << DIM << content_line << RESET << "\n";
        }
        continue;
      }

      if (in_code_block) {
        std::cout << BG_DARK << CYAN << content_line << RESET << "\n";
      } else {
        // Bold headers
        if (content_line.starts_with("## ") || content_line.starts_with("# ")) {
          std::cout << BOLD << content_line << RESET << "\n";
        } else if (content_line.find("**") != std::string::npos) {
          std::cout << content_line << "\n";
        } else {
          std::cout << content_line << "\n";
        }
      }
    }

    // Status footer
    std::cout << "\n" << DIM << "  ";
    if (ms > 0) {
      if (ms < 1000) {
        std::cout << ms << "ms";
      } else {
        std::cout << (ms / 1000) << "." << ((ms % 1000) / 100) << "s";
      }
    }
    if (response.value().usage.total_tokens > 0) {
      std::cout << "  •  " << response.value().usage.total_tokens << " tokens";
    }
    if (!response.value().tool_results.empty()) {
      std::cout << "  •  " << response.value().tool_results.size() << " tool call(s)";
    }
    std::cout << RESET << "\n\n";

    history.emplace_back("user", input);
    history.emplace_back("assistant", response.value().content);

    // Auto-compact when history gets large
    if (history.size() > 80) {
      history.erase(history.begin(), history.begin() + 20);
    }
  }

done:
  // ── Goodbye Banner ──
  std::cout << "\n" << DIM << "  ────────────────────────────────" << RESET << "\n";
  std::cout << "  " << BOLD << "Session Summary" << RESET << "\n";
  std::cout << "  " << DIM << "Messages:" << RESET << " " << message_count
            << "  " << DIM << "Tokens:" << RESET << " " << total_tokens << "\n";
  std::cout << DIM << "  ────────────────────────────────" << RESET << "\n";
  std::cout << "\n  " << CYAN << "👋 Goodbye!" << RESET << "\n\n";
  return common::Status::success();
}

} // namespace ghostclaw::agent
