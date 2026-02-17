#pragma once

#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/multi/agent_pool.hpp"
#include "ghostclaw/multi/types.hpp"
#include "ghostclaw/sessions/store.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ghostclaw::multi {

/// Callback for delivering agent output to the UI layer.
/// Parameters: agent_id, text, is_error
using OutputCallback =
    std::function<void(const std::string &agent_id, const std::string &text, bool is_error)>;

class Orchestrator {
public:
  Orchestrator(const config::Config &config, std::shared_ptr<AgentPool> pool,
               std::shared_ptr<sessions::SessionStore> store);
  ~Orchestrator();

  Orchestrator(const Orchestrator &) = delete;
  Orchestrator &operator=(const Orchestrator &) = delete;

  /// Submit an external message for routing. Thread-safe after start().
  void submit(const std::string &input, const std::string &channel, const std::string &sender);

  /// Spawn per-agent worker threads and begin processing.
  void start(OutputCallback callback);

  /// Signal shutdown, drain queues, and join all threads.
  void stop();

  /// Full interactive REPL for `ghostclaw multi`.
  void run_interactive();

  [[nodiscard]] bool is_running() const;

  /// Query helpers for interactive commands.
  [[nodiscard]] std::vector<std::string> list_agent_ids() const;
  [[nodiscard]] std::vector<std::string> list_team_ids() const;
  [[nodiscard]] std::size_t active_conversation_count() const;

private:
  struct AgentQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<InternalMessage> messages;
  };

  void agent_loop(const std::string &agent_id);
  void process_message(const std::string &agent_id, const InternalMessage &msg);
  void dispatch_mentions(const std::string &sender_agent_id, const std::string &conv_id,
                         const std::string &response);
  void emit_output(const std::string &agent_id, const std::string &text, bool is_error = false);
  void enqueue_to_agent(const std::string &agent_id, InternalMessage msg);
  void mark_conversation_complete(const std::string &conv_id);
  void handle_interactive_command(const std::string &command);
  void print_interactive_banner() const;
  void print_interactive_help() const;

  const config::Config &config_;
  std::shared_ptr<AgentPool> pool_;
  std::shared_ptr<sessions::SessionStore> store_;

  mutable std::mutex output_mutex_;
  OutputCallback on_output_;

  // queues_ is populated in start() before threads launch and only cleared in stop()
  // after threads join, so read access from agent threads is safe without a lock.
  // submit() and dispatch_mentions() access individual queue mutexes.
  std::unordered_map<std::string, std::unique_ptr<AgentQueue>> queues_;

  std::unordered_map<std::string, Conversation> conversations_;
  mutable std::mutex conversations_mutex_;

  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> next_message_id_{1};
  std::atomic<std::uint64_t> next_conversation_id_{1};
};

} // namespace ghostclaw::multi
