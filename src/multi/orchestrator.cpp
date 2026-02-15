#include "ghostclaw/multi/orchestrator.hpp"

#include "ghostclaw/common/fs.hpp"

#include <chrono>
#include <iostream>

namespace ghostclaw::multi {

Orchestrator::Orchestrator(const config::Config &config, std::shared_ptr<AgentPool> pool,
                           std::shared_ptr<sessions::SessionStore> store)
    : config_(config), pool_(std::move(pool)), store_(std::move(store)) {}

Orchestrator::~Orchestrator() {
  if (running_.load()) {
    stop();
  }
}

void Orchestrator::submit(const std::string &input, const std::string &channel,
                          const std::string &sender) {
  // Parse route prefix (@agent_id message)
  auto route = parse_route_prefix(input);

  std::string target_agent;
  std::string message;

  if (route.has_value()) {
    // Check if target is a team -> route to team leader
    if (pool_->has_team(route->target_id)) {
      target_agent = pool_->team_leader(route->target_id);
      if (target_agent.empty()) {
        // fallback to first team member
        auto members = pool_->team_members(route->target_id);
        if (!members.empty()) {
          target_agent = members[0];
        }
      }
    } else if (pool_->has_agent(route->target_id)) {
      target_agent = route->target_id;
    } else {
      // Unknown target, use default
      target_agent = config_.multi.default_agent;
      message = input;
    }
    if (message.empty()) {
      message = route->message;
    }
  } else {
    target_agent = config_.multi.default_agent;
    message = input;
  }

  // Generate conversation ID
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

  // Build internal message
  InternalMessage msg;
  msg.id = next_message_id_.fetch_add(1);
  msg.sender_agent_id = "__user__";
  msg.target_agent_id = target_agent;
  msg.content = message;
  msg.conversation_id = conv_id;
  msg.timestamp = static_cast<std::uint64_t>(
      std::chrono::system_clock::now().time_since_epoch().count());

  // Push to target agent queue
  auto queue_it = queues_.find(target_agent);
  if (queue_it != queues_.end()) {
    std::lock_guard<std::mutex> lock(queue_it->second->mutex);
    queue_it->second->messages.push(std::move(msg));
    queue_it->second->cv.notify_one();
  } else {
    std::cerr << "[multi] warning: no queue for agent '" << target_agent << "'\n";
  }
}

void Orchestrator::start(OutputCallback callback) {
  if (running_.load()) {
    return;
  }

  on_output_ = std::move(callback);
  running_.store(true);

  // Create a queue and thread for each agent
  auto ids = pool_->agent_ids();
  for (const auto &id : ids) {
    queues_[id] = std::make_unique<AgentQueue>();
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

void Orchestrator::run_interactive() {
  auto ids = pool_->agent_ids();
  if (ids.empty()) {
    std::cerr << "No agents configured. Add [agents.<name>] sections to config.toml.\n";
    return;
  }

  std::cout << "Multi-agent mode. Available agents:";
  for (const auto &id : ids) {
    std::cout << " " << id;
  }
  std::cout << "\n";
  std::cout << "Route with @agent_id <message>. Default agent: "
            << config_.multi.default_agent << "\n";
  std::cout << "Type /quit to exit.\n\n";

  start([](const std::string &agent_id, const std::string &text) {
    std::cout << "[" << agent_id << "] " << text << "\n";
  });

  std::string line;
  while (running_.load()) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line)) {
      break;
    }
    const std::string trimmed = common::trim(line);
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed == "/quit" || trimmed == "/exit") {
      break;
    }
    submit(trimmed, "cli", "user");
  }

  stop();
}

bool Orchestrator::is_running() const { return running_.load(); }

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

void Orchestrator::process_message(const std::string &agent_id,
                                   const InternalMessage &msg) {
  // Loop protection
  {
    std::lock_guard<std::mutex> lock(conversations_mutex_);
    auto conv_it = conversations_.find(msg.conversation_id);
    if (conv_it != conversations_.end()) {
      if (conv_it->second.total_messages >= config_.multi.max_internal_messages) {
        std::cerr << "[multi] conversation " << msg.conversation_id
                  << " exceeded max_internal_messages limit, dropping message\n";
        return;
      }
      conv_it->second.total_messages++;
    }
  }

  // Get or create engine
  auto engine_result = pool_->get_or_create(agent_id);
  if (!engine_result.ok()) {
    std::cerr << "[multi] failed to create engine for " << agent_id << ": "
              << engine_result.error() << "\n";
    return;
  }

  auto &engine = engine_result.value();

  // Set agent-specific options
  agent::AgentOptions options;
  options.agent_id = agent_id;

  // Find agent config for model/temperature overrides
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
    std::cerr << "[multi] agent " << agent_id << " error: " << result.error() << "\n";
    return;
  }

  const std::string &response = result.value().content;

  // Output the response
  if (on_output_) {
    on_output_(agent_id, response);
  }

  // Decrement pending count
  {
    std::lock_guard<std::mutex> lock(conversations_mutex_);
    auto conv_it = conversations_.find(msg.conversation_id);
    if (conv_it != conversations_.end()) {
      if (conv_it->second.pending_count > 0) {
        conv_it->second.pending_count--;
      }
    }
  }

  // Extract and dispatch mentions
  dispatch_mentions(agent_id, msg.conversation_id, response);
}

void Orchestrator::dispatch_mentions(const std::string &sender_agent_id,
                                     const std::string &conv_id,
                                     const std::string &response) {
  auto mentions = extract_mentions(response);
  if (mentions.empty()) {
    return;
  }

  for (const auto &mention : mentions) {
    std::string target = mention.target_agent_id;

    // Resolve team to leader
    if (pool_->has_team(target)) {
      target = pool_->team_leader(target);
    }

    if (!pool_->has_agent(target)) {
      std::cerr << "[multi] mention target '" << mention.target_agent_id
                << "' is not a known agent, skipping\n";
      continue;
    }

    // Increment pending count
    {
      std::lock_guard<std::mutex> lock(conversations_mutex_);
      auto conv_it = conversations_.find(conv_id);
      if (conv_it != conversations_.end()) {
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

    auto queue_it = queues_.find(target);
    if (queue_it != queues_.end()) {
      std::lock_guard<std::mutex> lock(queue_it->second->mutex);
      queue_it->second->messages.push(std::move(msg));
      queue_it->second->cv.notify_one();
    }
  }
}

} // namespace ghostclaw::multi
