#include "ghostclaw/daemon/daemon.hpp"

#include "ghostclaw/agent/engine.hpp"
#include "ghostclaw/channels/channel_manager.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/config/config.hpp"
#include "ghostclaw/daemon/pid_file.hpp"
#include "ghostclaw/daemon/state_writer.hpp"
#include "ghostclaw/gateway/server.hpp"
#include "ghostclaw/health/health.hpp"
#include "ghostclaw/heartbeat/cron_store.hpp"
#include "ghostclaw/heartbeat/engine.hpp"
#include "ghostclaw/heartbeat/scheduler.hpp"
#include "ghostclaw/observability/global.hpp"
#include "ghostclaw/runtime/app.hpp"
#include "ghostclaw/sessions/session_key.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

namespace ghostclaw::daemon {

Daemon::Daemon(const config::Config &config) : config_(config) {}

Daemon::~Daemon() { stop(); }

common::Status Daemon::start(const DaemonOptions &options) {
  if (running_) {
    return common::Status::error("daemon already running");
  }

  auto cfg_dir = config::config_dir();
  if (!cfg_dir.ok()) {
    return common::Status::error(cfg_dir.error());
  }

  auto pid = std::make_shared<PidFile>(cfg_dir.value() / "daemon.pid");
  auto pid_status = pid->acquire();
  if (!pid_status.ok()) {
    return pid_status;
  }

  auto state_writer = std::make_shared<StateWriter>(cfg_dir.value() / "daemon_state.json");
  state_writer->start();

  running_ = true;
  component_threads_.clear();

  component_threads_.push_back(std::thread([this, options]() {
    std::chrono::milliseconds backoff(2000);
    health::mark_component_starting("gateway");
    while (running_) {
      runtime::RuntimeContext context(config_);
      auto engine = context.create_agent_engine();
      if (!engine.ok()) {
        health::mark_component_error("gateway", engine.error());
        health::bump_component_restart("gateway");
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::milliseconds(60000));
        continue;
      }

      gateway::GatewayServer gateway(config_, engine.value());
      gateway::GatewayOptions gateway_options;
      gateway_options.host = options.host;
      gateway_options.port = options.port;
      auto started = gateway.start(gateway_options);
      if (!started.ok()) {
        health::mark_component_error("gateway", started.error());
        health::bump_component_restart("gateway");
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::milliseconds(60000));
        continue;
      }

      health::mark_component_ok("gateway");
      backoff = std::chrono::milliseconds(2000);

      while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      gateway.stop();
    }
  }));

  component_threads_.push_back(std::thread([this]() {
    health::mark_component_starting("channels");

    runtime::RuntimeContext context(config_);
    auto engine = context.create_agent_engine();
    if (!engine.ok()) {
      health::mark_component_error("channels", "failed to create agent engine: " + engine.error());
      return;
    }

    auto manager = channels::create_channel_manager(config_);
    auto run_mutex = std::make_shared<std::mutex>();
    auto status = manager->start_all([&manager, &engine, run_mutex](const channels::ChannelMessage &msg) {
      try {
        if (msg.content.empty()) {
          std::cerr << "[daemon][channels] skip empty message channel=" << msg.channel << "\n";
          return;
        }

        observability::record_channel_message(msg.channel, "inbound");
        const std::string reply_to = msg.recipient.empty() ? msg.sender : msg.recipient;
        if (reply_to.empty()) {
          observability::record_error("channels", "reply target missing");
          std::cerr << "[daemon][channels] drop message without reply target channel=" << msg.channel
                    << " sender=" << msg.sender << "\n";
          return;
        }

        auto session_key = sessions::make_session_key(
            {.agent_id = "ghostclaw", .channel_id = msg.channel, .peer_id = reply_to});
        if (!session_key.ok()) {
          observability::record_error("channels", "session_key_error: " + session_key.error());
          std::cerr << "[daemon][channels] session_key_error channel=" << msg.channel
                    << " peer=" << reply_to << " error=" << session_key.error() << "\n";
          return;
        }

        std::string preview = common::trim(msg.content);
        if (preview.size() > 120) {
          preview.resize(120);
          preview += "...";
        }
        std::cerr << "[daemon][channels] inbound channel=" << msg.channel << " peer=" << reply_to
                  << " session=" << session_key.value() << " text=\"" << preview << "\"\n";

        agent::AgentOptions options;
        options.session_id = session_key.value();
        options.agent_id = "ghostclaw";
        options.channel_id = msg.channel;
        options.tool_profile = "full";

        const auto response = [&]() {
          std::lock_guard<std::mutex> lock(*run_mutex);
          return engine.value()->run(msg.content, options);
        }();
        if (!response.ok()) {
          observability::record_error("channels", "agent_error: " + response.error());
          std::cerr << "[daemon][channels] agent_error session=" << session_key.value()
                    << " error=" << response.error() << "\n";
          return;
        }

        std::cerr << "[daemon][channels] agent_done session=" << session_key.value()
                  << " tool_calls=" << response.value().tool_results.size()
                  << " latency_ms=" << response.value().duration.count() << "\n";

        auto *channel = manager->get_channel(msg.channel);
        if (channel == nullptr) {
          observability::record_error("channels", "send_error: channel not found: " + msg.channel);
          std::cerr << "[daemon][channels] send_error unknown channel=" << msg.channel << "\n";
          return;
        }
        if (response.value().content.empty()) {
          std::cerr << "[daemon][channels] skip empty response session=" << session_key.value()
                    << "\n";
          return;
        }

        auto send_status = channel->send(reply_to, response.value().content);
        if (!send_status.ok()) {
          observability::record_error("channels", "send_error: " + send_status.error());
          std::cerr << "[daemon][channels] send_error session=" << session_key.value()
                    << " error=" << send_status.error() << "\n";
          return;
        }
        observability::record_channel_message(msg.channel, "outbound");
        std::cerr << "[daemon][channels] outbound_ok channel=" << msg.channel
                  << " peer=" << reply_to << " session=" << session_key.value() << "\n";
      } catch (const std::exception &ex) {
        observability::record_error("channels", std::string("callback_exception: ") + ex.what());
        std::cerr << "[daemon][channels] callback_exception " << ex.what() << "\n";
      } catch (...) {
        observability::record_error("channels", "callback_exception: unknown");
        std::cerr << "[daemon][channels] callback_exception unknown\n";
      }
    });

    if (!status.ok()) {
      health::mark_component_error("channels", status.error());
      health::bump_component_restart("channels");
      return;
    }
    health::mark_component_ok("channels");
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    manager->stop_all();
  }));

  component_threads_.push_back(std::thread([this]() {
    if (!config_.heartbeat.enabled) {
      health::mark_component_ok("heartbeat");
      return;
    }
    health::mark_component_starting("heartbeat");

    runtime::RuntimeContext context(config_);
    auto engine = context.create_agent_engine();
    if (!engine.ok()) {
      health::mark_component_error("heartbeat", engine.error());
      return;
    }

    auto workspace = config::workspace_dir();
    if (!workspace.ok()) {
      health::mark_component_error("heartbeat", workspace.error());
      return;
    }

    heartbeat::HeartbeatConfig hb_config;
    hb_config.enabled = true;
    hb_config.interval = std::chrono::minutes(config_.heartbeat.interval_minutes);
    hb_config.tasks_file = workspace.value() / config_.heartbeat.tasks_file;

    heartbeat::HeartbeatEngine heartbeat_engine(*engine.value(), hb_config);
    heartbeat_engine.start();
    health::mark_component_ok("heartbeat");
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    heartbeat_engine.stop();
  }));

  component_threads_.push_back(std::thread([this]() {
    health::mark_component_starting("scheduler");
    runtime::RuntimeContext context(config_);
    auto engine = context.create_agent_engine();
    if (!engine.ok()) {
      health::mark_component_error("scheduler", engine.error());
      return;
    }

    auto workspace = config::workspace_dir();
    if (!workspace.ok()) {
      health::mark_component_error("scheduler", workspace.error());
      return;
    }
    heartbeat::CronStore store(workspace.value() / "cron" / "jobs.db");

    // Auto-register config-driven schedules
    if (config_.daemon.auto_start_schedules) {
      for (const auto &entry : config_.daemon.schedules) {
        if (!entry.enabled) continue;
        auto parsed = heartbeat::CronExpression::parse(entry.expression);
        if (!parsed.ok()) continue;
        heartbeat::CronJob job;
        job.id = "config:" + entry.id;
        job.expression = entry.expression;
        job.command = entry.command;
        job.next_run = parsed.value().next_occurrence();
        (void)store.add_job(job);
      }
    }

    heartbeat::SchedulerConfig scheduler_config;
    scheduler_config.poll_interval =
        std::chrono::milliseconds(config_.reliability.scheduler_poll_secs * 1000);
    scheduler_config.max_retries = config_.reliability.scheduler_retries;

    heartbeat::Scheduler scheduler(store, *engine.value(), scheduler_config, &config_);
    scheduler.start();
    health::mark_component_ok("scheduler");
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    scheduler.stop();
  }));

  component_threads_.push_back(std::thread([this, pid, state_writer]() {
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    state_writer->stop();
    pid->release();
  }));

  return common::Status::success();
}

void Daemon::stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  for (auto &thread : component_threads_) {
    if (thread.joinable()) {
      try {
        thread.join();
      } catch (const std::system_error &err) {
        std::cerr << "[daemon] thread join failed: " << err.what() << "\n";
      }
    }
  }
  component_threads_.clear();
}

bool Daemon::is_running() const { return running_; }

} // namespace ghostclaw::daemon
