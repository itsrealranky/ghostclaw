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

namespace ghostclaw::multi {

using OutputCallback = std::function<void(const std::string &agent_id, const std::string &text)>;

class Orchestrator {
public:
  Orchestrator(const config::Config &config, std::shared_ptr<AgentPool> pool,
               std::shared_ptr<sessions::SessionStore> store);
  ~Orchestrator();

  Orchestrator(const Orchestrator &) = delete;
  Orchestrator &operator=(const Orchestrator &) = delete;

  void submit(const std::string &input, const std::string &channel, const std::string &sender);
  void start(OutputCallback callback);
  void stop();
  void run_interactive();
  [[nodiscard]] bool is_running() const;

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

  const config::Config &config_;
  std::shared_ptr<AgentPool> pool_;
  std::shared_ptr<sessions::SessionStore> store_;
  OutputCallback on_output_;

  std::unordered_map<std::string, std::unique_ptr<AgentQueue>> queues_;
  std::unordered_map<std::string, Conversation> conversations_;
  std::mutex conversations_mutex_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> next_message_id_{1};
  std::atomic<std::uint64_t> next_conversation_id_{1};
};

} // namespace ghostclaw::multi
