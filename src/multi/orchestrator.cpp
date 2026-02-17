#include "ghostclaw/multi/orchestrator.hpp"

#include "ghostclaw/common/fs.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace ghostclaw::multi {

namespace {

constexpr const char *RESET = "\033[0m";
constexpr const char *BOLD = "\033[1m";
constexpr const char *DIM = "\033[2m";
constexpr const char *RED = "\033[31m";
constexpr const char *GREEN = "\033[32m";
constexpr const char *YELLOW = "\033[33m";
constexpr const char *CYAN = "\033[36m";
constexpr const char *MAGENTA = "\033[35m";

std::string timestamp_now() {
  const auto now = std::chrono::system_clock::now();
  const auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &time_t_now);
#else
  localtime_r(&time_t_now, &local_time);
#endif
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local_time.tm_hour, local_time.tm_min,
                local_time.tm_sec);
  return std::string(buf);
}

} // namespace

// ── Construction / Destruction ──────────────────────────────────────────────

Orchestrator::Orchestrator(const config::Config &config, std::shared_ptr<AgentPool> pool,
                           std::shared_ptr<sessions::SessionStore> store)
    : config_(config), pool_(std::move(pool)), store_(std::move(store)) {}

Orchestrator::~Orchestrator() {
  if (running_.load()) {
    stop();
  }
}

// ── Output helpers ──────────────────────────────────────────────────────────

void Orchestrator::emit_output(const std::string &agent_id, const std::string &text,
                               bool is_error) {
  std::lock_guard<std::mutex> lock(output_mutex_);
  if (on_output_) {
    on_output_(agent_id, text, is_error);
  }
}

void Orchestrator::enqueue_to_agent(const std::string &agent_id, InternalMessage msg) {
  auto queue_it = queues_.find(agent_id);
  if (queue_it == queues_.end()) {
    emit_output("system", "no queue for agent '" + agent_id +
                               "' (is it configured in [agents." + agent_id + "]?)",
                true);
    return;
  }
  std::lock_guard<std::mutex> lock(queue_it->second->mutex);
  queue_it->second->messages.push(std::move(msg));
  queue_it->second->cv.notify_one();
}

// ── Submit ──────────────────────────────────────────────────────────────────

void Orchestrator::submit(const std::string &input, const std::string &channel,
                          const std::string &sender) {
  auto route = parse_route_prefix(input);

  std::string target_agent;
  std::string message;

  if (route.has_value()) {
    if (pool_->has_team(route->target_id)) {
      route->is_team = true;
      target_agent = pool_->team_leader(route->target_id);
      if (target_agent.empty()) {
        auto members = pool_->team_members(route->target_id);
        if (!members.empty()) {
          target_agent = members[0];
        }
      }
      if (target_agent.empty()) {
        emit_output("system",
                    "team '" + route->target_id + "' has no agents configured", true);
        return;
      }
      message = route->message;
    } else if (pool_->has_agent(route->target_id)) {
      target_agent = route->target_id;
      message = route->message;
    } else {
      // Not a known agent or team - show error with suggestions
      emit_output("system",
                  "unknown agent or team '" + route->target_id +
                      "'. Use /agents or /teams to see available targets.",
                  true);
      return;
    }
  } else {
    target_agent = config_.multi.default_agent;
    message = input;

    // Verify default agent exists
    if (!pool_->has_agent(target_agent)) {
      auto ids = pool_->agent_ids();
      if (!ids.empty()) {
        target_agent = ids[0];
      } else {
        emit_output("system", "no agents available", true);
        return;
      }
    }
  }

  // Generate conversation
  const std::string conv_id =
      "conv-" + std::to_string(next_conversation_id_.fetch_add(1));

  {
    std::lock_guard<std::mutex> lock(conversations_mutex_);
    Conversation conv;
    conv.id = conv_id;
    conv.originator = target_agent;
    conv.origin_channel = channel;
    conv.origin_sender = sender;
    conv.pending_count = 1;
    conv.total_messages = 0;
    conversations_[conv_id] = std::move(conv);
  }

  InternalMessage msg;
  msg.id = next_message_id_.fetch_add(1);
  msg.sender_agent_id = "__user__";
  msg.target_agent_id = target_agent;
  msg.content = message;
  msg.conversation_id = conv_id;
  msg.timestamp = static_cast<std::uint64_t>(
      std::chrono::system_clock::now().time_since_epoch().count());

  enqueue_to_agent(target_agent, std::move(msg));
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void Orchestrator::start(OutputCallback callback) {
  if (running_.load()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(output_mutex_);
    on_output_ = std::move(callback);
  }
  running_.store(true);

  // Build queues BEFORE spawning threads
  auto ids = pool_->agent_ids();
  for (const auto &id : ids) {
    queues_[id] = std::make_unique<AgentQueue>();
  }

  // Now spawn threads - queues_ is fully built and won't be modified until stop()
  for (const auto &id : ids) {
    threads_.emplace_back(&Orchestrator::agent_loop, this, id);
  }
}

void Orchestrator::stop() {
  running_.store(false);

  // Wake all agent threads
  for (auto &[id, queue] : queues_) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->cv.notify_all();
  }

  for (auto &thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  threads_.clear();
  queues_.clear();
}

bool Orchestrator::is_running() const { return running_.load(); }

std::vector<std::string> Orchestrator::list_agent_ids() const {
  return pool_->agent_ids();
}

std::vector<std::string> Orchestrator::list_team_ids() const {
  std::vector<std::string> ids;
  for (const auto &team : config_.multi.teams) {
    ids.push_back(team.id);
  }
  return ids;
}

std::size_t Orchestrator::active_conversation_count() const {
  std::lock_guard<std::mutex> lock(conversations_mutex_);
  std::size_t count = 0;
  for (const auto &[id, conv] : conversations_) {
    if (!conv.complete) {
      ++count;
    }
  }
  return count;
}

// ── Agent loop ──────────────────────────────────────────────────────────────

void Orchestrator::agent_loop(const std::string &agent_id) {
  auto queue_it = queues_.find(agent_id);
  if (queue_it == queues_.end()) {
    return;
  }

  auto &queue = *queue_it->second;

  while (running_.load()) {
    InternalMessage msg;
    {
      std::unique_lock<std::mutex> lock(queue.mutex);
      queue.cv.wait_for(lock, std::chrono::milliseconds(500),
                        [&] { return !queue.messages.empty() || !running_.load(); });

      if (!running_.load() && queue.messages.empty()) {
        break;
      }
      if (queue.messages.empty()) {
        continue;
      }

      msg = std::move(queue.messages.front());
      queue.messages.pop();
    }

    process_message(agent_id, msg);
  }
}

// ── Message processing ──────────────────────────────────────────────────────

void Orchestrator::process_message(const std::string &agent_id,
                                   const InternalMessage &msg) {
  // Loop protection
  {
    std::lock_guard<std::mutex> lock(conversations_mutex_);
    auto conv_it = conversations_.find(msg.conversation_id);
    if (conv_it != conversations_.end()) {
      if (conv_it->second.total_messages >= config_.multi.max_internal_messages) {
        emit_output("system",
                    "conversation " + msg.conversation_id +
                        " exceeded message limit (" +
                        std::to_string(config_.multi.max_internal_messages) +
                        "), stopping chain",
                    true);
        conv_it->second.complete = true;
        return;
      }
      conv_it->second.total_messages++;
    }
  }

  // Get or create engine
  auto engine_result = pool_->get_or_create(agent_id);
  if (!engine_result.ok()) {
    emit_output(agent_id, "failed to initialize: " + engine_result.error(), true);
    mark_conversation_complete(msg.conversation_id);
    return;
  }

  auto &engine = engine_result.value();

  // Set agent-specific options
  agent::AgentOptions options;
  options.agent_id = agent_id;

  for (const auto &ac : config_.multi.agents) {
    if (ac.id == agent_id) {
      if (!ac.model.empty()) {
        options.model_override = ac.model;
      }
      options.temperature_override = ac.temperature;
      break;
    }
  }

  // Run the engine
  auto result = engine->run(msg.content, options);
  if (!result.ok()) {
    emit_output(agent_id, "error: " + result.error(), true);
    // Decrement pending and check completion
    {
      std::lock_guard<std::mutex> lock(conversations_mutex_);
      auto conv_it = conversations_.find(msg.conversation_id);
      if (conv_it != conversations_.end() && conv_it->second.pending_count > 0) {
        conv_it->second.pending_count--;
        if (conv_it->second.pending_count == 0) {
          conv_it->second.complete = true;
        }
      }
    }
    return;
  }

  const std::string &response = result.value().content;

  // Output the response
  emit_output(agent_id, response);

  // Extract mentions BEFORE decrementing pending (so we can adjust the count)
  auto mentions = extract_mentions(response);

  // Update conversation state
  {
    std::lock_guard<std::mutex> lock(conversations_mutex_);
    auto conv_it = conversations_.find(msg.conversation_id);
    if (conv_it != conversations_.end()) {
      if (conv_it->second.pending_count > 0) {
        conv_it->second.pending_count--;
      }
      // If no mentions will be dispatched and nothing else pending, mark complete
      if (mentions.empty() && conv_it->second.pending_count == 0) {
        conv_it->second.complete = true;
      }
    }
  }

  // Dispatch mentions
  if (!mentions.empty()) {
    dispatch_mentions(agent_id, msg.conversation_id, response);
  }
}

void Orchestrator::mark_conversation_complete(const std::string &conv_id) {
  std::lock_guard<std::mutex> lock(conversations_mutex_);
  auto conv_it = conversations_.find(conv_id);
  if (conv_it != conversations_.end()) {
    conv_it->second.complete = true;
  }
}

// ── Mention dispatch ────────────────────────────────────────────────────────

void Orchestrator::dispatch_mentions(const std::string &sender_agent_id,
                                     const std::string &conv_id,
                                     const std::string &response) {
  auto mentions = extract_mentions(response);
  if (mentions.empty()) {
    return;
  }

  std::size_t dispatched = 0;

  for (const auto &mention : mentions) {
    std::string target = mention.target_agent_id;

    // Resolve team to leader
    if (pool_->has_team(target)) {
      target = pool_->team_leader(target);
      if (target.empty()) {
        emit_output("system",
                    "agent @" + sender_agent_id + " mentioned team '" +
                        mention.target_agent_id + "' which has no leader",
                    true);
        continue;
      }
    }

    if (!pool_->has_agent(target)) {
      emit_output("system",
                  "agent @" + sender_agent_id + " mentioned unknown agent '" +
                      mention.target_agent_id + "'",
                  true);
      continue;
    }

    // Don't let agent mention itself (infinite loop)
    if (target == sender_agent_id) {
      continue;
    }

    // Increment pending count
    {
      std::lock_guard<std::mutex> lock(conversations_mutex_);
      auto conv_it = conversations_.find(conv_id);
      if (conv_it != conversations_.end()) {
        if (conv_it->second.complete) {
          continue; // Conversation already terminated
        }
        conv_it->second.pending_count++;
      }
    }

    InternalMessage msg;
    msg.id = next_message_id_.fetch_add(1);
    msg.sender_agent_id = sender_agent_id;
    msg.target_agent_id = target;
    msg.content = "[from @" + sender_agent_id + "] " + mention.message;
    msg.conversation_id = conv_id;
    msg.timestamp = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    msg.is_mention = true;

    enqueue_to_agent(target, std::move(msg));
    ++dispatched;
  }

  if (dispatched > 0) {
    emit_output("system",
                "@" + sender_agent_id + " handed off to " + std::to_string(dispatched) +
                    " agent(s)",
                false);
  }
}

// ── Interactive REPL ────────────────────────────────────────────────────────

void Orchestrator::print_interactive_banner() const {
  std::cout << "\n";
  std::cout << BOLD << CYAN << "  Multi-Agent Mode" << RESET << "\n";
  std::cout << DIM << "  ──────────────────────────────────────" << RESET << "\n";

  auto ids = pool_->agent_ids();
  std::sort(ids.begin(), ids.end());

  std::cout << BOLD << "  Agents:" << RESET;
  for (const auto &id : ids) {
    const bool is_default = (id == config_.multi.default_agent);
    std::cout << " " << GREEN << id << RESET;
    if (is_default) {
      std::cout << DIM << " (default)" << RESET;
    }
  }
  std::cout << "\n";

  if (!config_.multi.teams.empty()) {
    std::cout << BOLD << "  Teams:" << RESET;
    for (const auto &team : config_.multi.teams) {
      std::cout << " " << MAGENTA << team.id << RESET;
      std::cout << DIM << " [" << team.leader_agent;
      for (const auto &member : team.agents) {
        if (member != team.leader_agent) {
          std::cout << ", " << member;
        }
      }
      std::cout << "]" << RESET;
    }
    std::cout << "\n";
  }

  std::cout << "\n";
  std::cout << DIM << "  Usage:" << RESET << "\n";
  std::cout << "    " << DIM << "message" << RESET << "            Send to default agent ("
            << config_.multi.default_agent << ")\n";
  std::cout << "    " << YELLOW << "@agent" << RESET << " message    Send to specific agent\n";
  std::cout << "    " << YELLOW << "@team" << RESET << " message     Send to team leader\n";
  std::cout << "\n";
  std::cout << DIM << "  Agents can hand off to each other with "
            << RESET << YELLOW << "[@agent: message]" << RESET << DIM
            << " in their responses." << RESET << "\n";
  std::cout << DIM << "  Type " << RESET << YELLOW << "/help" << RESET << DIM
            << " for commands, " << RESET << YELLOW << "/quit" << RESET << DIM
            << " to exit." << RESET << "\n\n";
}

void Orchestrator::print_interactive_help() const {
  std::cout << "\n";
  std::cout << BOLD << "  Commands" << RESET << "\n";
  std::cout << "    " << YELLOW << "/agents" << RESET << DIM
            << "     List all configured agents with provider/model" << RESET << "\n";
  std::cout << "    " << YELLOW << "/teams" << RESET << DIM
            << "      List all configured teams" << RESET << "\n";
  std::cout << "    " << YELLOW << "/status" << RESET << DIM
            << "     Show active conversations and queue depths" << RESET << "\n";
  std::cout << "    " << YELLOW << "/help" << RESET << DIM
            << "       Show this help message" << RESET << "\n";
  std::cout << "    " << YELLOW << "/quit" << RESET << DIM
            << "       Exit multi-agent mode" << RESET << "\n";
  std::cout << "\n";
  std::cout << BOLD << "  Routing" << RESET << "\n";
  std::cout << "    " << DIM << "message" << RESET << "            Send to "
            << config_.multi.default_agent << " (default)\n";
  std::cout << "    " << YELLOW << "@coder" << RESET << " fix bug    Send to agent 'coder'\n";
  std::cout << "    " << YELLOW << "@dev" << RESET << " fix bug      Send to team 'dev' leader\n";
  std::cout << "\n";
  std::cout << BOLD << "  Agent Mentions" << RESET << DIM
            << " (used by agents in their responses)" << RESET << "\n";
  std::cout << "    " << YELLOW << "[@reviewer: please check this code]" << RESET << "\n";
  std::cout << "    Sends 'please check this code' to the reviewer agent.\n";
  std::cout << "\n";
}

void Orchestrator::handle_interactive_command(const std::string &command) {
  if (command == "/help" || command == "/h") {
    print_interactive_help();
    return;
  }

  if (command == "/agents" || command == "/agent") {
    auto ids = pool_->agent_ids();
    std::sort(ids.begin(), ids.end());
    std::cout << "\n";
    for (const auto &id : ids) {
      const bool is_default = (id == config_.multi.default_agent);
      std::cout << "  " << GREEN << id << RESET;
      if (is_default) {
        std::cout << DIM << " (default)" << RESET;
      }
      for (const auto &ac : config_.multi.agents) {
        if (ac.id == id) {
          if (!ac.provider.empty() || !ac.model.empty()) {
            std::cout << DIM << "  " << ac.provider;
            if (!ac.model.empty()) {
              std::cout << "/" << ac.model;
            }
            std::cout << RESET;
          }
          if (!ac.system_prompt.empty()) {
            std::string preview = ac.system_prompt.substr(0, 60);
            if (ac.system_prompt.size() > 60) {
              preview += "...";
            }
            std::cout << "\n    " << DIM << preview << RESET;
          }
          break;
        }
      }
      std::cout << "\n";
    }
    std::cout << "\n";
    return;
  }

  if (command == "/teams" || command == "/team") {
    if (config_.multi.teams.empty()) {
      std::cout << DIM << "\n  No teams configured.\n\n" << RESET;
      return;
    }
    std::cout << "\n";
    for (const auto &team : config_.multi.teams) {
      std::cout << "  " << MAGENTA << team.id << RESET;
      if (!team.description.empty()) {
        std::cout << DIM << " - " << team.description << RESET;
      }
      std::cout << "\n";
      std::cout << "    leader: " << GREEN << team.leader_agent << RESET << "\n";
      std::cout << "    agents:";
      for (const auto &member : team.agents) {
        std::cout << " " << member;
      }
      std::cout << "\n";
    }
    std::cout << "\n";
    return;
  }

  if (command == "/status") {
    auto ids = pool_->agent_ids();
    std::cout << "\n";
    std::cout << "  " << BOLD << "Agents:" << RESET << " " << ids.size() << " configured\n";
    std::cout << "  " << BOLD << "Teams:" << RESET << " " << config_.multi.teams.size()
              << " configured\n";
    std::cout << "  " << BOLD << "Active conversations:" << RESET << " "
              << active_conversation_count() << "\n";
    std::cout << "  " << BOLD << "Max internal messages:" << RESET << " "
              << config_.multi.max_internal_messages << "\n";

    // Show queue depths
    for (const auto &id : ids) {
      auto queue_it = queues_.find(id);
      if (queue_it != queues_.end()) {
        std::lock_guard<std::mutex> lock(queue_it->second->mutex);
        const auto depth = queue_it->second->messages.size();
        std::cout << "  " << DIM << "queue(" << id << "):" << RESET << " " << depth
                  << " pending\n";
      }
    }
    std::cout << "\n";
    return;
  }

  std::cout << DIM << "  Unknown command: " << command << ". Type /help for commands." << RESET
            << "\n";
}

void Orchestrator::run_interactive() {
  auto ids = pool_->agent_ids();
  if (ids.empty()) {
    std::cerr << RED << "No agents configured." << RESET
              << " Add [agents.<name>] sections to config.toml.\n\n";
    std::cerr << "Example:\n\n";
    std::cerr << "  [agents.coder]\n";
    std::cerr << "  provider = \"anthropic\"\n";
    std::cerr << "  model = \"claude-sonnet-4-20250514\"\n";
    std::cerr << "  system_prompt = \"You are a senior software engineer.\"\n\n";
    std::cerr << "  [agents.reviewer]\n";
    std::cerr << "  provider = \"openai\"\n";
    std::cerr << "  model = \"gpt-4o\"\n";
    std::cerr << "  system_prompt = \"You are a code reviewer.\"\n\n";
    std::cerr << "  [teams.dev]\n";
    std::cerr << "  agents = [\"coder\", \"reviewer\"]\n";
    std::cerr << "  leader_agent = \"coder\"\n\n";
    return;
  }

  print_interactive_banner();

  start([](const std::string &agent_id, const std::string &text, bool is_error) {
    const std::string ts = timestamp_now();
    if (agent_id == "system") {
      if (is_error) {
        std::cout << DIM << ts << RESET << " " << RED << text << RESET << "\n";
      } else {
        std::cout << DIM << ts << " " << text << RESET << "\n";
      }
    } else if (is_error) {
      std::cout << DIM << ts << RESET << " " << RED << "[" << agent_id << "] " << text
                << RESET << "\n";
    } else {
      std::cout << DIM << ts << RESET << " " << BOLD << CYAN << "[" << agent_id << "]"
                << RESET << " " << text << "\n";
    }
    std::cout << std::flush;
  });

  std::string line;
  while (running_.load()) {
    std::cout << BOLD << "> " << RESET << std::flush;
    if (!std::getline(std::cin, line)) {
      break;
    }
    const std::string trimmed = common::trim(line);
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed == "/quit" || trimmed == "/exit" || trimmed == "/q") {
      break;
    }
    if (!trimmed.empty() && trimmed[0] == '/') {
      handle_interactive_command(trimmed);
      continue;
    }
    submit(trimmed, "cli", "user");
  }

  std::cout << DIM << "\nStopping agents..." << RESET << "\n";
  stop();
  std::cout << DIM << "Goodbye." << RESET << "\n";
}

} // namespace ghostclaw::multi
