#include "ghostclaw/heartbeat/scheduler.hpp"

#include "ghostclaw/channels/send_service.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/conway/module.hpp"

#include <algorithm>
#include <iostream>
#include <thread>

namespace ghostclaw::heartbeat {

Scheduler::Scheduler(CronStore &store, agent::AgentEngine &agent, SchedulerConfig config,
                     const config::Config *runtime_config)
    : store_(store), agent_(agent), config_(config), runtime_config_(runtime_config) {}

Scheduler::~Scheduler() { stop(); }

void Scheduler::start() {
  if (running_) {
    return;
  }
  running_ = true;

  // Auto-register Conway survival monitoring heartbeat if enabled
  if (runtime_config_ != nullptr && runtime_config_->conway.enabled &&
      runtime_config_->conway.survival_monitoring) {
    ensure_survival_jobs();
  }

  thread_ = std::thread([this]() { run_loop(); });
}

void Scheduler::stop() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool Scheduler::is_running() const { return running_; }

void Scheduler::run_loop() {
  while (running_) {
    auto due_jobs = store_.get_due_jobs();
    if (due_jobs.ok()) {
      for (const auto &job : due_jobs.value()) {
        execute_job(job);
      }
    }
    const auto wait_steps = std::max<long long>(1, config_.poll_interval.count() / 100);
    for (long long i = 0; i < wait_steps && running_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void Scheduler::execute_job(const CronJob &job) {
  if (job.last_status.has_value() && *job.last_status == "__paused__") {
    return;
  }

  const auto dispatch_payload = parse_channel_dispatch_payload(job.command);
  std::string status = "ok";
  for (std::uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
    if (dispatch_payload.has_value()) {
      auto sent = dispatch_channel_payload(*dispatch_payload);
      if (sent.ok()) {
        status = "ok";
        break;
      }
      status = sent.error();
    } else {
      auto result = agent_.run(job.command);
      if (result.ok()) {
        status = "ok";
        break;
      }
      status = result.error();
    }
    if (attempt < config_.max_retries) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  if (common::starts_with(job.expression, "@at:")) {
    (void)store_.remove_job(job.id);
    return;
  }

  if (common::starts_with(job.expression, "@every:")) {
    long long interval_ms = 0;
    try {
      interval_ms = std::stoll(job.expression.substr(std::string("@every:").size()));
    } catch (...) {
      interval_ms = 0;
    }
    if (interval_ms <= 0) {
      interval_ms = 60'000;
    }
    const auto next_run = std::chrono::system_clock::now() + std::chrono::milliseconds(interval_ms);
    (void)store_.update_after_run(job.id, status, next_run);
    return;
  }

  auto expr = CronExpression::parse(job.expression);
  const auto next_run = expr.ok() ? expr.value().next_occurrence()
                                  : std::chrono::system_clock::now() + std::chrono::hours(1);
  (void)store_.update_after_run(job.id, status, next_run);
}

void Scheduler::ensure_survival_jobs() {
  constexpr const char *SURVIVAL_JOB_ID = "__conway_survival_check__";
  constexpr long long SURVIVAL_INTERVAL_MS = 30LL * 60 * 1000; // 30 minutes

  // Check if job already exists via list_jobs
  auto listed = store_.list_jobs();
  if (listed.ok()) {
    for (const auto &job : listed.value()) {
      if (job.id == SURVIVAL_JOB_ID) {
        return; // Already registered
      }
    }
  }

  CronJob job;
  job.id = SURVIVAL_JOB_ID;
  job.expression = "@every:" + std::to_string(SURVIVAL_INTERVAL_MS);
  job.command =
      "Check my Conway credit balance using the credits_balance tool. "
      "Report the balance and survival tier. "
      "If credits are below $0.10 (critical tier), output a clear warning and "
      "recommend immediate action to restore balance. "
      "If credits are below $0.50 (low compute tier), note this in your response.";
  job.next_run = std::chrono::system_clock::now() + std::chrono::minutes(30);

  auto status = store_.add_job(job);
  if (!status.ok()) {
    std::cerr << "[scheduler] warning: could not register Conway survival check: "
              << status.error() << "\n";
  } else {
    std::cerr << "[scheduler] Conway survival monitoring heartbeat registered "
                 "(every 30 minutes)\n";
  }
}

std::optional<Scheduler::ChannelDispatchPayload>
Scheduler::parse_channel_dispatch_payload(const std::string &command) const {
  const std::string trimmed = common::trim(command);
  if (trimmed.size() < 2 || trimmed.front() != '{' || trimmed.back() != '}') {
    return std::nullopt;
  }

  const auto payload = common::json_parse_flat(trimmed);
  const auto kind_it = payload.find("kind");
  if (kind_it == payload.end() || common::to_lower(common::trim(kind_it->second)) != "channel_message") {
    return std::nullopt;
  }

  ChannelDispatchPayload out;
  const auto channel_it = payload.find("channel");
  const auto to_it = payload.find("to");
  const auto text_it = payload.find("text");
  if (channel_it == payload.end() || to_it == payload.end() || text_it == payload.end()) {
    return std::nullopt;
  }
  out.channel = channel_it->second;
  out.to = to_it->second;
  out.text = text_it->second;
  const auto id_it = payload.find("id");
  if (id_it != payload.end()) {
    out.id = id_it->second;
  }
  return out;
}

common::Status
Scheduler::dispatch_channel_payload(const Scheduler::ChannelDispatchPayload &payload) const {
  if (runtime_config_ == nullptr) {
    return common::Status::error("scheduler channel dispatch unavailable: missing runtime config");
  }

  channels::SendService sender(*runtime_config_);
  return sender.send({.channel = payload.channel, .recipient = payload.to, .text = payload.text});
}

} // namespace ghostclaw::heartbeat
