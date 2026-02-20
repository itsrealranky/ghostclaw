#pragma once

#include "ghostclaw/agent/engine.hpp"
#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/heartbeat/cron_store.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace ghostclaw::heartbeat {

struct SchedulerConfig {
  std::chrono::milliseconds poll_interval{15000};
  std::uint32_t max_retries = 2;
};

class Scheduler {
public:
  Scheduler(CronStore &store, agent::AgentEngine &agent, SchedulerConfig config = {},
            const config::Config *runtime_config = nullptr);
  ~Scheduler();

  void start();
  void stop();
  [[nodiscard]] bool is_running() const;

private:
  struct ChannelDispatchPayload {
    std::string channel;
    std::string to;
    std::string text;
    std::string id;
  };

  void run_loop();
  void execute_job(const CronJob &job);
  void ensure_survival_jobs();
  [[nodiscard]] std::optional<ChannelDispatchPayload>
  parse_channel_dispatch_payload(const std::string &command) const;
  [[nodiscard]] common::Status dispatch_channel_payload(const ChannelDispatchPayload &payload) const;

  CronStore &store_;
  agent::AgentEngine &agent_;
  SchedulerConfig config_;
  const config::Config *runtime_config_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

} // namespace ghostclaw::heartbeat
