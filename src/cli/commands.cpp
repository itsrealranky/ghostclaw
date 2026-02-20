#include "ghostclaw/cli/commands.hpp"

#include "ghostclaw/auth/google_oauth.hpp"
#include "ghostclaw/auth/oauth.hpp"
#include "ghostclaw/multi/agent_pool.hpp"
#include "ghostclaw/multi/orchestrator.hpp"
#include "ghostclaw/channels/channel_manager.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/config/config.hpp"
#include "ghostclaw/conway/module.hpp"
#include "ghostclaw/daemon/daemon.hpp"
#include "ghostclaw/doctor/diagnostics.hpp"
#include "ghostclaw/gateway/server.hpp"
#include "ghostclaw/hardware/module.hpp"
#include "ghostclaw/heartbeat/cron.hpp"
#include "ghostclaw/heartbeat/cron_store.hpp"
#include "ghostclaw/integrations/registry.hpp"
#include "ghostclaw/migration/module.hpp"
#include "ghostclaw/onboard/wizard.hpp"
#include "ghostclaw/peripheral/module.hpp"
#include "ghostclaw/providers/catalog.hpp"
#include "ghostclaw/runtime/app.hpp"
#include "ghostclaw/service/module.hpp"
#include "ghostclaw/skills/import_openclaw.hpp"
#include "ghostclaw/skills/registry.hpp"
#include "ghostclaw/soul/manager.hpp"
#include "ghostclaw/tts/tts.hpp"
#include "ghostclaw/voice/wake.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define GHOSTCLAW_ISATTY _isatty
#define GHOSTCLAW_FILENO _fileno
#else
#include <unistd.h>
#define GHOSTCLAW_ISATTY isatty
#define GHOSTCLAW_FILENO fileno
#endif

namespace ghostclaw::cli {

namespace {

std::string version_string() {
#ifdef GHOSTCLAW_VERSION
  std::string version = GHOSTCLAW_VERSION;
#else
  std::string version = "0.1.0";
#endif
#ifdef GHOSTCLAW_GIT_COMMIT
  const std::string commit = GHOSTCLAW_GIT_COMMIT;
  if (!commit.empty() && commit != "unknown") {
    version += " (" + commit + ")";
  }
#endif
  return "ghostclaw " + version;
}

std::vector<std::string> collect_args(int argc, char **argv) {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    out.emplace_back(argv[i]);
  }
  return out;
}

bool take_option(std::vector<std::string> &args, const std::string &long_name,
                 const std::string &short_name, std::string &out_value) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == long_name || args[i] == short_name) {
      if (i + 1 >= args.size()) {
        return false;
      }
      out_value = args[i + 1];
      args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i + 2));
      return true;
    }
  }
  return false;
}

bool take_flag(std::vector<std::string> &args, const std::string &name) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == name) {
      args.erase(args.begin() + static_cast<long>(i));
      return true;
    }
  }
  return false;
}

bool apply_global_options(std::vector<std::string> &args, std::string &error) {
  for (std::size_t i = 0; i < args.size();) {
    if (args[i] == "--config") {
      if (i + 1 >= args.size()) {
        error = "missing value for --config";
        return false;
      }
      config::set_config_path_override(args[i + 1]);
      args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i + 2));
      continue;
    }
    if (common::starts_with(args[i], "--config=")) {
      const auto value = args[i].substr(std::string("--config=").size());
      if (value.empty()) {
        error = "missing value for --config";
        return false;
      }
      config::set_config_path_override(value);
      args.erase(args.begin() + static_cast<long>(i));
      continue;
    }
    ++i;
  }
  return true;
}

std::string join_tokens(const std::vector<std::string> &args, const std::size_t begin = 0) {
  std::ostringstream out;
  for (std::size_t i = begin; i < args.size(); ++i) {
    if (i > begin) {
      out << ' ';
    }
    out << args[i];
  }
  return out.str();
}

std::string read_stdin_all() {
  std::ostringstream out;
  out << std::cin.rdbuf();
  return out.str();
}

bool stdin_is_tty() { return GHOSTCLAW_ISATTY(GHOSTCLAW_FILENO(stdin)) != 0; }

int run_agent(std::vector<std::string> args);

int run_onboard(std::vector<std::string> args) {
  onboard::WizardOptions options;
  const bool explicit_non_interactive = take_flag(args, "--non-interactive");
  const bool explicit_interactive = take_flag(args, "--interactive");
  options.channels_only = take_flag(args, "--channels-only");

  std::string value;
  if (take_option(args, "--api-key", "", value)) {
    options.api_key = value;
  }
  if (take_option(args, "--provider", "", value)) {
    options.provider = value;
  }
  if (take_option(args, "--model", "", value)) {
    options.model = value;
  }
  if (take_option(args, "--memory", "", value)) {
    options.memory_backend = value;
  }

  // Determine interactive mode:
  // - Explicit --interactive or --non-interactive wins
  // - If both provider and model are supplied via flags, assume non-interactive
  // - Otherwise default to interactive
  if (explicit_interactive && !stdin_is_tty()) {
    std::cerr << "--interactive requires a TTY\n";
    return 1;
  }
  if (!explicit_non_interactive && !explicit_interactive && !stdin_is_tty()) {
    options.interactive = false;
  } else if (explicit_non_interactive) {
    options.interactive = false;
  } else if (explicit_interactive) {
    options.interactive = true;
  } else if (options.provider.has_value() && options.model.has_value()) {
    options.interactive = false;
  } else {
    options.interactive = true;
  }

  options.offer_launch = true;
  auto result = onboard::run_wizard(options);
  if (!result.success) {
    std::cerr << "onboard failed: " << result.error << "\n";
    return 1;
  }
  if (result.launch_agent) {
    return run_agent({});
  }
  return 0;
}

int run_agent(std::vector<std::string> args) {
  if (!config::config_exists()) {
    if (!stdin_is_tty()) {
      std::cerr << "No configuration found and stdin is not interactive.\n";
      std::cerr << "Run 'ghostclaw onboard --non-interactive --provider <name> --model <name>' "
                   "first.\n";
      return 1;
    }
    std::cout << "No configuration found. Let's set up GhostClaw first.\n";
    onboard::WizardOptions wizard_opts;
    wizard_opts.interactive = true;
    auto ws = onboard::run_wizard(wizard_opts);
    if (!ws.success) {
      std::cerr << ws.error << "\n";
      return 1;
    }
  }
  auto context = runtime::RuntimeContext::from_disk();
  if (!context.ok()) {
    std::cerr << context.error() << "\n";
    return 1;
  }
  auto runtime_context = std::move(context.value());

  std::string message;
  std::string provider;
  std::string model;
  std::string temperature_raw;
  (void)take_option(args, "--message", "-m", message);
  (void)take_option(args, "--provider", "", provider);
  (void)take_option(args, "--model", "", model);
  (void)take_option(args, "--temperature", "-t", temperature_raw);

  agent::AgentOptions options;
  if (!provider.empty()) {
    options.provider_override = provider;
    runtime_context.mutable_config().default_provider = provider;
  }
  if (!model.empty()) {
    options.model_override = model;
    runtime_context.mutable_config().default_model = model;
  }
  if (!temperature_raw.empty()) {
    try {
      options.temperature_override = std::stod(temperature_raw);
      runtime_context.mutable_config().default_temperature = *options.temperature_override;
    } catch (...) {
      std::cerr << "invalid temperature: " << temperature_raw << "\n";
      return 1;
    }
  }

  auto engine = runtime_context.create_agent_engine();
  if (!engine.ok()) {
    std::cerr << engine.error() << "\n";
    return 1;
  }

  if (!message.empty()) {
    auto result = engine.value()->run(message, options);
    if (!result.ok()) {
      std::cerr << result.error() << "\n";
      return 1;
    }
    std::cout << result.value().content << "\n";
    return 0;
  }

  auto status = engine.value()->run_interactive(options);
  if (!status.ok()) {
    std::cerr << status.error() << "\n";
    return 1;
  }
  return 0;
}

int run_gateway(std::vector<std::string> args) {
  if (!config::config_exists()) {
    if (!stdin_is_tty()) {
      std::cerr << "No configuration found and stdin is not interactive.\n";
      std::cerr << "Run 'ghostclaw onboard --non-interactive --provider <name> --model <name>' "
                   "first.\n";
      return 1;
    }
    std::cout << "No configuration found. Let's set up GhostClaw first.\n";
    onboard::WizardOptions wizard_opts;
    wizard_opts.interactive = true;
    auto ws = onboard::run_wizard(wizard_opts);
    if (!ws.success) {
      std::cerr << ws.error << "\n";
      return 1;
    }
  }
  auto context = runtime::RuntimeContext::from_disk();
  if (!context.ok()) {
    std::cerr << context.error() << "\n";
    return 1;
  }

  gateway::GatewayOptions options;
  std::string host;
  std::string port_raw;
  std::string duration_raw;
  const bool once = take_flag(args, "--once");
  (void)take_option(args, "--host", "", host);
  (void)take_option(args, "--port", "-p", port_raw);
  (void)take_option(args, "--duration-secs", "", duration_raw);
  if (!host.empty()) {
    options.host = host;
  } else {
    options.host = context.value().config().gateway.host;
  }
  if (!port_raw.empty()) {
    try {
      options.port = static_cast<std::uint16_t>(std::stoul(port_raw));
    } catch (...) {
      std::cerr << "invalid port: " << port_raw << "\n";
      return 1;
    }
  } else {
    options.port = context.value().config().gateway.port;
  }

  auto engine = context.value().create_agent_engine();
  if (!engine.ok()) {
    std::cerr << engine.error() << "\n";
    return 1;
  }

  gateway::GatewayServer server(context.value().config(), engine.value());
  auto status = server.start(options);
  if (!status.ok()) {
    std::cerr << status.error() << "\n";
    return 1;
  }

  std::cout << "Gateway listening on " << options.host << ":" << server.port() << "\n";
  if (server.websocket_port() != 0) {
    std::cout << "WebSocket sidecar on " << context.value().config().gateway.websocket_host << ":"
              << server.websocket_port() << "\n";
  }
  if (server.public_url().has_value()) {
    std::cout << "Public URL: " << *server.public_url() << "\n";
  }
  if (!server.pairing_code().empty()) {
    std::cout << "Pairing code: " << server.pairing_code() << "\n";
  }

  if (once) {
    server.stop();
    return 0;
  }

  if (!duration_raw.empty()) {
    int duration = 0;
    try {
      duration = std::stoi(duration_raw);
    } catch (...) {
      duration = 0;
    }
    if (duration > 0) {
      std::this_thread::sleep_for(std::chrono::seconds(duration));
      server.stop();
      return 0;
    }
  }

  std::cout << "Press Enter to stop gateway...\n";
  std::string line;
  std::getline(std::cin, line);
  server.stop();
  return 0;
}

int run_status() {
  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << cfg.error() << "\n";
    return 1;
  }
  auto ws = config::workspace_dir();
  auto cp = config::config_path();

  std::cout << "Provider: " << cfg.value().default_provider << "\n";
  std::cout << "Model: " << cfg.value().default_model << "\n";
  std::cout << "Memory: " << cfg.value().memory.backend << "\n";
  if (cp.ok()) {
    std::cout << "Config: " << cp.value().string() << "\n";
  }
  if (ws.ok()) {
    std::cout << "Workspace: " << ws.value().string() << "\n";
  }

  // Show SOUL.md status
  if (ws.ok()) {
    const auto soul_path = ws.value() / "SOUL.md";
    if (std::filesystem::exists(soul_path)) {
      std::cout << "SOUL.md: " << soul_path.string() << "\n";
    }
  }

  // Show Conway status
  const auto &conway = cfg.value().conway;
  if (conway.enabled || !conway.api_key.empty()) {
    std::cout << "\nConway:\n";
    std::cout << "  Enabled: " << (conway.enabled ? "yes" : "no") << "\n";
    auto wallet = conway::read_wallet_address(conway);
    if (wallet.ok()) {
      std::cout << "  Wallet: " << wallet.value() << "\n";
    }
    std::cout << "  Survival monitoring: "
              << (conway.survival_monitoring ? "active" : "inactive") << "\n";
  }
  return 0;
}

int run_doctor() {
  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << "[FAIL] Config load: " << cfg.error() << "\n";
    return 1;
  }

  const auto report = doctor::run_diagnostics(cfg.value());
  doctor::print_diagnostics_report(report);
  return report.failed == 0 ? 0 : 1;
}

int run_daemon(std::vector<std::string> args) {
  if (!config::config_exists()) {
    if (!stdin_is_tty()) {
      std::cerr << "No configuration found and stdin is not interactive.\n";
      std::cerr << "Run 'ghostclaw onboard --non-interactive --provider <name> --model <name>' "
                   "first.\n";
      return 1;
    }
    std::cout << "No configuration found. Let's set up GhostClaw first.\n";
    onboard::WizardOptions wizard_opts;
    wizard_opts.interactive = true;
    auto ws = onboard::run_wizard(wizard_opts);
    if (!ws.success) {
      std::cerr << ws.error << "\n";
      return 1;
    }
  }
  auto context = runtime::RuntimeContext::from_disk();
  if (!context.ok()) {
    std::cerr << context.error() << "\n";
    return 1;
  }

  daemon::DaemonOptions options;
  std::string host;
  std::string port_raw;
  std::string duration_raw;
  (void)take_option(args, "--host", "", host);
  (void)take_option(args, "--port", "-p", port_raw);
  (void)take_option(args, "--duration-secs", "", duration_raw);
  if (!host.empty()) {
    options.host = host;
  } else {
    options.host = context.value().config().gateway.host;
  }
  if (!port_raw.empty()) {
    try {
      options.port = static_cast<std::uint16_t>(std::stoul(port_raw));
    } catch (...) {
      std::cerr << "invalid port: " << port_raw << "\n";
      return 1;
    }
  } else {
    options.port = context.value().config().gateway.port;
  }

  daemon::Daemon daemon(context.value().config());
  auto started = daemon.start(options);
  if (!started.ok()) {
    std::cerr << started.error() << "\n";
    return 1;
  }
  std::cout << "Daemon started on " << options.host << ":" << options.port << "\n";

  if (!duration_raw.empty()) {
    int duration = 0;
    try {
      duration = std::stoi(duration_raw);
    } catch (...) {
      duration = 0;
    }
    if (duration > 0) {
      std::this_thread::sleep_for(std::chrono::seconds(duration));
      daemon.stop();
      return 0;
    }
  }

  std::cout << "Press Enter to stop daemon...\n";
  std::string line;
  std::getline(std::cin, line);
  daemon.stop();
  return 0;
}

int run_cron(std::vector<std::string> args) {
  auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    std::cerr << workspace.error() << "\n";
    return 1;
  }

  heartbeat::CronStore store(workspace.value() / "cron" / "jobs.db");

  auto parse_delay = [](const std::string &value) -> common::Result<std::chrono::seconds> {
    const std::string trimmed = common::trim(value);
    if (trimmed.empty()) {
      return common::Result<std::chrono::seconds>::failure("delay value is empty");
    }

    const char suffix = trimmed.back();
    std::string number_part = trimmed;
    long long multiplier = 1;
    if (suffix == 's' || suffix == 'm' || suffix == 'h' || suffix == 'd') {
      number_part = trimmed.substr(0, trimmed.size() - 1);
      if (suffix == 'm') {
        multiplier = 60;
      } else if (suffix == 'h') {
        multiplier = 3600;
      } else if (suffix == 'd') {
        multiplier = 86400;
      }
    }

    long long number = 0;
    try {
      number = std::stoll(number_part);
    } catch (...) {
      return common::Result<std::chrono::seconds>::failure("invalid delay value: " + value);
    }
    if (number < 0) {
      return common::Result<std::chrono::seconds>::failure("delay must be non-negative");
    }
    return common::Result<std::chrono::seconds>::success(
        std::chrono::seconds(number * multiplier));
  };

  auto parse_rfc3339_utc = [](const std::string &value)
      -> common::Result<std::chrono::system_clock::time_point> {
    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
      return common::Result<std::chrono::system_clock::time_point>::failure(
          "invalid RFC3339 timestamp: " + value);
    }
    if (!stream.eof()) {
      char suffix = '\0';
      stream >> suffix;
      if (suffix != 'Z') {
        return common::Result<std::chrono::system_clock::time_point>::failure(
            "timestamp must end with Z (UTC): " + value);
      }
    }

#if defined(_WIN32)
    const std::time_t as_time_t = _mkgmtime(&tm);
#else
    const std::time_t as_time_t = timegm(&tm);
#endif
    if (as_time_t < 0) {
      return common::Result<std::chrono::system_clock::time_point>::failure(
          "failed to parse timestamp: " + value);
    }
    return common::Result<std::chrono::system_clock::time_point>::success(
        std::chrono::system_clock::from_time_t(as_time_t));
  };

  auto next_run_from_expression = [](const std::string &expression)
      -> common::Result<std::chrono::system_clock::time_point> {
    if (common::starts_with(expression, "@every:")) {
      long long every_ms = 0;
      try {
        every_ms = std::stoll(expression.substr(std::string("@every:").size()));
      } catch (...) {
        return common::Result<std::chrono::system_clock::time_point>::failure(
            "invalid @every expression");
      }
      if (every_ms <= 0) {
        return common::Result<std::chrono::system_clock::time_point>::failure(
            "invalid @every interval");
      }
      return common::Result<std::chrono::system_clock::time_point>::success(
          std::chrono::system_clock::now() + std::chrono::milliseconds(every_ms));
    }
    if (common::starts_with(expression, "@at:")) {
      long long unix_seconds = 0;
      try {
        unix_seconds = std::stoll(expression.substr(std::string("@at:").size()));
      } catch (...) {
        return common::Result<std::chrono::system_clock::time_point>::failure(
            "invalid @at expression");
      }
      auto at_time = std::chrono::system_clock::time_point(std::chrono::seconds(unix_seconds));
      if (at_time < std::chrono::system_clock::now()) {
        at_time = std::chrono::system_clock::now() + std::chrono::seconds(1);
      }
      return common::Result<std::chrono::system_clock::time_point>::success(at_time);
    }

    auto parsed = heartbeat::CronExpression::parse(expression);
    if (!parsed.ok()) {
      return common::Result<std::chrono::system_clock::time_point>::failure(parsed.error());
    }
    return common::Result<std::chrono::system_clock::time_point>::success(
        parsed.value().next_occurrence());
  };

  auto make_job_id = []() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return std::string("job-") + std::to_string(micros) + "-" +
           std::to_string(sequence.fetch_add(1));
  };

  if (args.empty() || args[0] == "list") {
    auto jobs = store.list_jobs();
    if (!jobs.ok()) {
      std::cerr << jobs.error() << "\n";
      return 1;
    }
    for (const auto &job : jobs.value()) {
      std::cout << job.id << " | " << job.expression << " | " << job.command;
      if (job.last_status.has_value() && *job.last_status == "__paused__") {
        std::cout << " | paused";
      }
      std::cout << "\n";
    }
    return 0;
  }

  if (args[0] == "add") {
    std::vector<std::string> add_args(args.begin() + 1, args.end());
    std::string timezone;
    (void)take_option(add_args, "--tz", "", timezone);
    if (add_args.size() < 2) {
      std::cerr << "usage: ghostclaw cron add <expression> [--tz <IANA_TZ>] <command>\n";
      return 1;
    }
    auto expression = heartbeat::CronExpression::parse(add_args[0]);
    if (!expression.ok()) {
      std::cerr << expression.error() << "\n";
      return 1;
    }

    std::string command = add_args[1];
    for (std::size_t i = 2; i < add_args.size(); ++i) {
      command += " " + add_args[i];
    }

    heartbeat::CronJob job;
    job.id = make_job_id();
    job.expression = add_args[0];
    job.command = command;
    job.next_run = expression.value().next_occurrence();
    auto added = store.add_job(job);
    if (!added.ok()) {
      std::cerr << added.error() << "\n";
      return 1;
    }
    if (!timezone.empty()) {
      std::cout << "Note: timezone hint '" << timezone
                << "' recorded in command input only; scheduler currently runs in local time.\n";
    }
    std::cout << "Added cron job: " << job.id << "\n";
    return 0;
  }

  if (args[0] == "add-at") {
    if (args.size() < 3) {
      std::cerr << "usage: ghostclaw cron add-at <rfc3339_timestamp> <command>\n";
      return 1;
    }
    auto at_time = parse_rfc3339_utc(args[1]);
    if (!at_time.ok()) {
      std::cerr << at_time.error() << "\n";
      return 1;
    }
    std::string command = args[2];
    for (std::size_t i = 3; i < args.size(); ++i) {
      command += " " + args[i];
    }

    heartbeat::CronJob job;
    job.id = make_job_id();
    job.expression = "@at:" + heartbeat::time_point_to_unix_string(at_time.value());
    job.command = command;
    job.next_run = at_time.value();
    auto added = store.add_job(job);
    if (!added.ok()) {
      std::cerr << added.error() << "\n";
      return 1;
    }
    std::cout << "Added one-shot cron job: " << job.id << "\n";
    return 0;
  }

  if (args[0] == "add-every") {
    if (args.size() < 3) {
      std::cerr << "usage: ghostclaw cron add-every <every_ms> <command>\n";
      return 1;
    }
    long long every_ms = 0;
    try {
      every_ms = std::stoll(args[1]);
    } catch (...) {
      std::cerr << "invalid every_ms: " << args[1] << "\n";
      return 1;
    }
    if (every_ms <= 0) {
      std::cerr << "every_ms must be > 0\n";
      return 1;
    }

    std::string command = args[2];
    for (std::size_t i = 3; i < args.size(); ++i) {
      command += " " + args[i];
    }

    heartbeat::CronJob job;
    job.id = make_job_id();
    job.expression = "@every:" + std::to_string(every_ms);
    job.command = command;
    job.next_run = std::chrono::system_clock::now() + std::chrono::milliseconds(every_ms);
    auto added = store.add_job(job);
    if (!added.ok()) {
      std::cerr << added.error() << "\n";
      return 1;
    }
    std::cout << "Added interval cron job: " << job.id << "\n";
    return 0;
  }

  if (args[0] == "once") {
    if (args.size() < 3) {
      std::cerr << "usage: ghostclaw cron once <delay> <command>\n";
      return 1;
    }
    auto delay = parse_delay(args[1]);
    if (!delay.ok()) {
      std::cerr << delay.error() << "\n";
      return 1;
    }

    std::string command = args[2];
    for (std::size_t i = 3; i < args.size(); ++i) {
      command += " " + args[i];
    }

    const auto run_at = std::chrono::system_clock::now() + delay.value();
    heartbeat::CronJob job;
    job.id = make_job_id();
    job.expression = "@at:" + heartbeat::time_point_to_unix_string(run_at);
    job.command = command;
    job.next_run = run_at;
    auto added = store.add_job(job);
    if (!added.ok()) {
      std::cerr << added.error() << "\n";
      return 1;
    }
    std::cout << "Added one-time cron job: " << job.id << "\n";
    return 0;
  }

  if (args[0] == "remove") {
    if (args.size() < 2) {
      std::cerr << "usage: ghostclaw cron remove <id>\n";
      return 1;
    }
    auto removed = store.remove_job(args[1]);
    if (!removed.ok()) {
      std::cerr << removed.error() << "\n";
      return 1;
    }
    std::cout << (removed.value() ? "Removed" : "Not found") << "\n";
    return 0;
  }

  if (args[0] == "pause" || args[0] == "resume") {
    if (args.size() < 2) {
      std::cerr << "usage: ghostclaw cron " << args[0] << " <id>\n";
      return 1;
    }
    auto jobs = store.list_jobs();
    if (!jobs.ok()) {
      std::cerr << jobs.error() << "\n";
      return 1;
    }
    auto it = std::find_if(jobs.value().begin(), jobs.value().end(),
                           [&](const heartbeat::CronJob &job) { return job.id == args[1]; });
    if (it == jobs.value().end()) {
      std::cout << "Not found\n";
      return 0;
    }

    if (args[0] == "pause") {
      const auto far_future = std::chrono::system_clock::now() + std::chrono::hours(24 * 365 * 10);
      auto paused = store.update_after_run(it->id, "__paused__", far_future);
      if (!paused.ok()) {
        std::cerr << paused.error() << "\n";
        return 1;
      }
      std::cout << "Paused\n";
      return 0;
    }

    auto next_run = next_run_from_expression(it->expression);
    if (!next_run.ok()) {
      std::cerr << next_run.error() << "\n";
      return 1;
    }
    auto resumed = store.update_after_run(it->id, "__resumed__", next_run.value());
    if (!resumed.ok()) {
      std::cerr << resumed.error() << "\n";
      return 1;
    }
    std::cout << "Resumed\n";
    return 0;
  }

  std::cerr << "unknown cron subcommand\n";
  return 1;
}

std::string humanize_age(const std::chrono::system_clock::time_point timestamp) {
  const auto now = std::chrono::system_clock::now();
  const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - timestamp).count();
  if (age < 0) {
    return "just now";
  }
  if (age < 60) {
    return std::to_string(age) + "s ago";
  }
  if (age < 3600) {
    return std::to_string(age / 60) + "m ago";
  }
  if (age < 86400) {
    return std::to_string(age / 3600) + "h ago";
  }
  return std::to_string(age / 86400) + "d ago";
}

int run_models(std::vector<std::string> args) {
  if (args.empty()) {
    std::cerr << "usage: ghostclaw models refresh [--provider <id>] [--force]\n";
    return 1;
  }

  const std::string subcommand = common::to_lower(common::trim(args[0]));
  args.erase(args.begin());
  if (subcommand != "refresh") {
    std::cerr << "unknown models subcommand\n";
    return 1;
  }

  std::string provider;
  (void)take_option(args, "--provider", "", provider);
  const bool force = take_flag(args, "--force");
  if (!args.empty()) {
    std::cerr << "unknown models arguments: " << join_tokens(args) << "\n";
    return 1;
  }

  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << cfg.error() << "\n";
    return 1;
  }

  if (!provider.empty()) {
    auto refreshed = providers::refresh_model_catalog(cfg.value(), provider, force);
    if (!refreshed.ok()) {
      std::cerr << refreshed.error() << "\n";
      return 1;
    }
    std::cout << refreshed.value().provider << ": " << refreshed.value().models.size()
              << " model(s) "
              << (refreshed.value().from_cache ? "(cached " : "(updated ")
              << humanize_age(refreshed.value().updated_at) << ")\n";
    const std::size_t preview = std::min<std::size_t>(5, refreshed.value().models.size());
    for (std::size_t i = 0; i < preview; ++i) {
      std::cout << "  - " << refreshed.value().models[i] << "\n";
    }
    if (refreshed.value().models.size() > preview) {
      std::cout << "  ... and " << (refreshed.value().models.size() - preview) << " more\n";
    }
    return 0;
  }

  auto catalogs = providers::refresh_model_catalogs(cfg.value(), force);
  if (!catalogs.ok()) {
    std::cerr << catalogs.error() << "\n";
    return 1;
  }
  for (const auto &catalog : catalogs.value()) {
    std::cout << catalog.provider << ": " << catalog.models.size() << " model(s) "
              << (catalog.from_cache ? "(cached " : "(updated ")
              << humanize_age(catalog.updated_at) << ")\n";
  }
  return 0;
}

int run_providers() {
  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << cfg.error() << "\n";
    return 1;
  }

  const std::string active_provider = common::to_lower(common::trim(cfg.value().default_provider));
  const auto &catalog = providers::provider_catalog();

  std::cout << "Supported providers (" << catalog.size() << ")\n";
  for (const auto &provider : catalog) {
    bool is_active = provider.id == active_provider;
    if (!is_active) {
      for (const auto &alias : provider.aliases) {
        if (common::to_lower(alias) == active_provider) {
          is_active = true;
          break;
        }
      }
    }

    std::cout << "  " << provider.id << " - " << provider.display_name;
    if (provider.local) {
      std::cout << " [local]";
    }
    if (is_active) {
      std::cout << " [active]";
    }
    if (!provider.aliases.empty()) {
      std::cout << " (aliases: ";
      for (std::size_t i = 0; i < provider.aliases.size(); ++i) {
        if (i != 0) {
          std::cout << ", ";
        }
        std::cout << provider.aliases[i];
      }
      std::cout << ")";
    }
    std::cout << "\n";
  }
  std::cout << "  custom:<url> - OpenAI-compatible endpoint\n";
  return 0;
}

int run_hardware(std::vector<std::string> args) {
  if (args.empty()) {
    std::cerr << "usage: ghostclaw hardware <discover|introspect|info>\n";
    return 1;
  }

  const std::string subcommand = common::to_lower(common::trim(args[0]));
  args.erase(args.begin());

  if (subcommand == "discover") {
    auto devices = hardware::discover_devices();
    if (!devices.ok()) {
      std::cerr << devices.error() << "\n";
      return 1;
    }
    if (devices.value().empty()) {
      std::cout << "No USB or serial devices detected.\n";
      return 0;
    }
    for (const auto &device : devices.value()) {
      std::cout << device.path << " | " << device.board << " | " << device.transport << "\n";
    }
    return 0;
  }

  if (subcommand == "introspect") {
    if (args.empty()) {
      std::cerr << "usage: ghostclaw hardware introspect <path>\n";
      return 1;
    }
    auto info = hardware::introspect_device(args[0]);
    if (!info.ok()) {
      std::cerr << info.error() << "\n";
      return 1;
    }
    std::cout << "path: " << info.value().path << "\n";
    std::cout << "exists: " << (info.value().exists ? "yes" : "no") << "\n";
    std::cout << "serial_like: " << (info.value().serial_like ? "yes" : "no") << "\n";
    std::cout << "board: " << info.value().board << "\n";
    std::cout << "transport: " << info.value().transport << "\n";
    return info.value().exists ? 0 : 1;
  }

  if (subcommand == "info") {
    std::string chip = "stm32f401re";
    (void)take_option(args, "--chip", "", chip);
    if (!args.empty()) {
      std::cerr << "unknown hardware info arguments: " << join_tokens(args) << "\n";
      return 1;
    }
    std::cout << hardware::chip_info_summary(chip) << "\n";
    return 0;
  }

  std::cerr << "unknown hardware subcommand\n";
  return 1;
}

int run_peripheral(std::vector<std::string> args) {
  if (args.empty()) {
    std::cerr << "usage: ghostclaw peripheral <list|add|flash|setup-uno-q|flash-nucleo>\n";
    return 1;
  }

  const std::string subcommand = common::to_lower(common::trim(args[0]));
  args.erase(args.begin());

  if (subcommand == "list") {
    auto entries = peripheral::list_peripherals();
    if (!entries.ok()) {
      std::cerr << entries.error() << "\n";
      return 1;
    }
    if (entries.value().empty()) {
      std::cout << "No peripherals configured.\n";
      return 0;
    }
    for (const auto &entry : entries.value()) {
      std::cout << entry.board << " | " << entry.transport << " | " << entry.path << "\n";
    }
    return 0;
  }

  if (subcommand == "add") {
    if (args.size() < 2) {
      std::cerr << "usage: ghostclaw peripheral add <board> <path>\n";
      return 1;
    }
    auto added = peripheral::add_peripheral(args[0], args[1]);
    if (!added.ok()) {
      std::cerr << added.error() << "\n";
      return 1;
    }
    std::cout << "peripheral added\n";
    return 0;
  }

  if (subcommand == "flash") {
    std::string port;
    std::string board;
    std::string firmware;
    const bool execute = take_flag(args, "--execute");
    (void)take_option(args, "--port", "-p", port);
    (void)take_option(args, "--board", "-b", board);
    (void)take_option(args, "--firmware", "-f", firmware);
    if (!args.empty()) {
      std::cerr << "usage: ghostclaw peripheral flash [--board <board>] [--port <path>] [--firmware <file>] [--execute]\n";
      return 1;
    }

    peripheral::PeripheralFlashOptions options;
    options.execute = execute;
    if (!board.empty()) {
      options.board = board;
    }
    if (!port.empty()) {
      options.port = port;
    }
    if (!firmware.empty()) {
      options.firmware = firmware;
    }

    auto flashed = peripheral::flash_peripheral(options);
    if (!flashed.ok()) {
      std::cerr << flashed.error() << "\n";
      return 1;
    }
    std::cout << flashed.value() << "\n";
    return 0;
  }

  if (subcommand == "setup-uno-q") {
    std::string host;
    (void)take_option(args, "--host", "", host);
    auto configured =
        peripheral::setup_uno_q_bridge(host.empty() ? std::nullopt : std::optional<std::string>(host));
    if (!configured.ok()) {
      std::cerr << configured.error() << "\n";
      return 1;
    }
    std::cout << "uno-q bridge configured\n";
    return 0;
  }

  if (subcommand == "flash-nucleo") {
    std::string firmware;
    std::vector<std::string> flash_args = args;
    (void)take_option(flash_args, "--firmware", "-f", firmware);
    const bool execute = take_flag(flash_args, "--execute");
    if (!flash_args.empty()) {
      std::cerr << "usage: ghostclaw peripheral flash-nucleo [--firmware <file>] [--execute]\n";
      return 1;
    }

    peripheral::PeripheralFlashOptions options;
    options.board = "nucleo-f4";
    options.execute = execute;
    if (!firmware.empty()) {
      options.firmware = firmware;
    }

    auto flashed = peripheral::flash_peripheral(options);
    if (!flashed.ok()) {
      std::cerr << flashed.error() << "\n";
      return 1;
    }
    std::cout << flashed.value() << "\n";
    return 0;
  }

  std::cerr << "unknown peripheral subcommand\n";
  return 1;
}

int run_service(const std::vector<std::string> &args, const std::string &executable_path) {
  auto status = service::handle_command(args, executable_path);
  if (!status.ok()) {
    std::cerr << status.error() << "\n";
    return 1;
  }
  std::cout << status.value() << "\n";
  return 0;
}

int run_channel(const std::vector<std::string> &args) {
  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << cfg.error() << "\n";
    return 1;
  }

  auto manager = channels::create_channel_manager(cfg.value());
  if (args.empty() || args[0] == "list") {
    for (const auto &name : manager->list_channels()) {
      std::cout << name << "\n";
    }
    return 0;
  }

  if (args[0] == "doctor") {
    for (const auto &name : manager->list_channels()) {
      auto *channel = manager->get_channel(name);
      std::cout << name << ": " << (channel != nullptr && channel->health_check() ? "ok" : "error")
                << "\n";
    }
    return 0;
  }

  if (args[0] == "start") {
    return run_daemon({});
  }

  if (args[0] == "bind-telegram") {
    if (args.size() < 2) {
      std::cerr << "usage: ghostclaw channel bind-telegram <identity>\n";
      return 1;
    }
    auto mutable_cfg = cfg.value();
    if (!mutable_cfg.channels.telegram.has_value()) {
      std::cerr << "telegram is not configured\n";
      return 1;
    }
    const std::string identity = common::trim(args[1]);
    if (identity.empty()) {
      std::cerr << "identity is required\n";
      return 1;
    }

    auto &allowlist = mutable_cfg.channels.telegram->allowed_users;
    if (std::find(allowlist.begin(), allowlist.end(), identity) == allowlist.end()) {
      allowlist.push_back(identity);
    }
    auto saved = config::save_config(mutable_cfg);
    if (!saved.ok()) {
      std::cerr << saved.error() << "\n";
      return 1;
    }
    std::cout << "bound telegram identity: " << identity << "\n";
    return 0;
  }

  if (args[0] == "add") {
    if (args.size() < 3) {
      std::cerr << "usage: ghostclaw channel add <type> <json>\n";
      return 1;
    }
    const std::string type = common::to_lower(common::trim(args[1]));
    const std::string json = args[2];
    auto mutable_cfg = cfg.value();

    if (type == "telegram") {
      config::TelegramConfig telegram;
      telegram.bot_token = common::json_get_string(json, "bot_token");
      telegram.allowed_users = common::json_get_string_array(json, "allowed_users");
      if (telegram.bot_token.empty()) {
        std::cerr << "telegram requires bot_token\n";
        return 1;
      }
      mutable_cfg.channels.telegram = std::move(telegram);
    } else if (type == "discord") {
      config::DiscordConfig discord;
      discord.bot_token = common::json_get_string(json, "bot_token");
      discord.guild_id = common::json_get_string(json, "guild_id");
      discord.allowed_users = common::json_get_string_array(json, "allowed_users");
      if (discord.bot_token.empty()) {
        std::cerr << "discord requires bot_token\n";
        return 1;
      }
      mutable_cfg.channels.discord = std::move(discord);
    } else if (type == "slack") {
      config::SlackConfig slack;
      slack.bot_token = common::json_get_string(json, "bot_token");
      slack.channel_id = common::json_get_string(json, "channel_id");
      slack.allowed_users = common::json_get_string_array(json, "allowed_users");
      if (slack.bot_token.empty()) {
        std::cerr << "slack requires bot_token\n";
        return 1;
      }
      mutable_cfg.channels.slack = std::move(slack);
    } else if (type == "matrix") {
      config::MatrixConfig matrix;
      matrix.homeserver = common::json_get_string(json, "homeserver");
      matrix.access_token = common::json_get_string(json, "access_token");
      matrix.room_id = common::json_get_string(json, "room_id");
      if (matrix.homeserver.empty() || matrix.access_token.empty() || matrix.room_id.empty()) {
        std::cerr << "matrix requires homeserver, access_token, and room_id\n";
        return 1;
      }
      mutable_cfg.channels.matrix = std::move(matrix);
    } else if (type == "imessage") {
      config::IMessageConfig imessage;
      imessage.allowed_contacts = common::json_get_string_array(json, "allowed_contacts");
      mutable_cfg.channels.imessage = std::move(imessage);
    } else if (type == "whatsapp") {
      config::WhatsAppConfig whatsapp;
      whatsapp.access_token = common::json_get_string(json, "access_token");
      whatsapp.phone_number_id = common::json_get_string(json, "phone_number_id");
      whatsapp.verify_token = common::json_get_string(json, "verify_token");
      whatsapp.allowed_numbers = common::json_get_string_array(json, "allowed_numbers");
      if (whatsapp.access_token.empty() || whatsapp.phone_number_id.empty() ||
          whatsapp.verify_token.empty()) {
        std::cerr << "whatsapp requires access_token, phone_number_id, and verify_token\n";
        return 1;
      }
      mutable_cfg.channels.whatsapp = std::move(whatsapp);
    } else if (type == "webhook") {
      config::WebhookConfig webhook;
      webhook.secret = common::json_get_string(json, "secret");
      if (webhook.secret.empty()) {
        std::cerr << "webhook requires secret\n";
        return 1;
      }
      mutable_cfg.channels.webhook = std::move(webhook);
    } else {
      std::cerr << "unsupported channel type: " << type << "\n";
      return 1;
    }

    auto saved = config::save_config(mutable_cfg);
    if (!saved.ok()) {
      std::cerr << saved.error() << "\n";
      return 1;
    }
    std::cout << "channel configured: " << type << "\n";
    return 0;
  }

  if (args[0] == "remove") {
    if (args.size() < 2) {
      std::cerr << "usage: ghostclaw channel remove <name>\n";
      return 1;
    }
    const std::string name = common::to_lower(common::trim(args[1]));
    auto mutable_cfg = cfg.value();
    bool removed = false;

    if (name == "telegram") {
      removed = mutable_cfg.channels.telegram.has_value();
      mutable_cfg.channels.telegram = std::nullopt;
    } else if (name == "discord") {
      removed = mutable_cfg.channels.discord.has_value();
      mutable_cfg.channels.discord = std::nullopt;
    } else if (name == "slack") {
      removed = mutable_cfg.channels.slack.has_value();
      mutable_cfg.channels.slack = std::nullopt;
    } else if (name == "matrix") {
      removed = mutable_cfg.channels.matrix.has_value();
      mutable_cfg.channels.matrix = std::nullopt;
    } else if (name == "imessage") {
      removed = mutable_cfg.channels.imessage.has_value();
      mutable_cfg.channels.imessage = std::nullopt;
    } else if (name == "whatsapp") {
      removed = mutable_cfg.channels.whatsapp.has_value();
      mutable_cfg.channels.whatsapp = std::nullopt;
    } else if (name == "webhook") {
      removed = mutable_cfg.channels.webhook.has_value();
      mutable_cfg.channels.webhook = std::nullopt;
    } else {
      std::cerr << "unknown channel: " << name << "\n";
      return 1;
    }

    auto saved = config::save_config(mutable_cfg);
    if (!saved.ok()) {
      std::cerr << saved.error() << "\n";
      return 1;
    }
    std::cout << (removed ? "removed" : "not found") << "\n";
    return 0;
  }

  std::cerr << "unknown channel subcommand\n";
  return 1;
}

int run_skills(std::vector<std::string> args) {
  auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    std::cerr << workspace.error() << "\n";
    return 1;
  }
  skills::SkillRegistry registry(workspace.value() / "skills",
                                 workspace.value() / ".community-skills");

  const auto print_skill = [](const skills::Skill &skill, const bool show_source) {
    std::cout << skill.name;
    if (!common::trim(skill.version).empty()) {
      std::cout << "@" << skill.version;
    }
    if (show_source) {
      std::cout << " [" << skills::skill_source_to_string(skill.source) << "]";
    }
    if (!common::trim(skill.description).empty()) {
      std::cout << " - " << skill.description;
    }
    std::cout << "\n";
  };

  if (args.empty() || args[0] == "list" || args[0] == "list-workspace") {
    auto listed = registry.list_workspace();
    if (!listed.ok()) {
      std::cerr << listed.error() << "\n";
      return 1;
    }
    for (const auto &skill : listed.value()) {
      print_skill(skill, false);
    }
    return 0;
  }
  if (args[0] == "list-community") {
    auto listed = registry.list_community();
    if (!listed.ok()) {
      std::cerr << listed.error() << "\n";
      return 1;
    }
    for (const auto &skill : listed.value()) {
      print_skill(skill, true);
    }
    return 0;
  }
  if (args[0] == "list-all") {
    auto listed = registry.list_all();
    if (!listed.ok()) {
      std::cerr << listed.error() << "\n";
      return 1;
    }
    for (const auto &skill : listed.value()) {
      print_skill(skill, true);
    }
    return 0;
  }
  if (args[0] == "search") {
    std::vector<std::string> query_args(args.begin() + 1, args.end());
    const bool workspace_only = take_flag(query_args, "--workspace-only");
    const std::string query = common::trim(join_tokens(query_args));
    if (query.empty()) {
      std::cerr << "usage: ghostclaw skills search [--workspace-only] <query>\n";
      return 1;
    }

    auto results = registry.search(query, !workspace_only);
    if (!results.ok()) {
      std::cerr << results.error() << "\n";
      return 1;
    }
    for (const auto &result : results.value()) {
      std::cout << result.skill.name << " [" << skills::skill_source_to_string(result.skill.source)
                << "] score=" << result.score;
      if (!common::trim(result.skill.description).empty()) {
        std::cout << " - " << result.skill.description;
      }
      std::cout << "\n";
    }
    return 0;
  }
  if (args[0] == "sync-github") {
    std::vector<std::string> sub(args.begin() + 1, args.end());
    const bool prune = take_flag(sub, "--prune");
    std::string branch;
    std::string skills_dir;
    (void)take_option(sub, "--branch", "", branch);
    (void)take_option(sub, "--skills-dir", "", skills_dir);
    if (sub.empty()) {
      std::cerr << "usage: ghostclaw skills sync-github [--branch BRANCH] [--skills-dir DIR] "
                   "[--prune] <repo-or-local-path>\n";
      return 1;
    }

    const std::string repo = sub[0];
    auto synced = registry.sync_github(repo, branch.empty() ? "main" : branch,
                                       skills_dir.empty() ? "skills" : skills_dir, prune);
    if (!synced.ok()) {
      std::cerr << synced.error() << "\n";
      return 1;
    }
    std::cout << "Synced " << synced.value() << " skill(s)\n";
    return 0;
  }
  if (args[0] == "install") {
    std::vector<std::string> sub(args.begin() + 1, args.end());
    const bool no_community = take_flag(sub, "--no-community");
    if (sub.empty()) {
      std::cerr << "usage: ghostclaw skills install [--no-community] <name-or-path>\n";
      return 1;
    }
    auto installed = registry.install(sub[0], !no_community);
    if (!installed.ok()) {
      std::cerr << installed.error() << "\n";
      return 1;
    }
    std::cout << (installed.value() ? "Installed" : "Already installed") << "\n";
    return 0;
  }
  if (args[0] == "remove") {
    if (args.size() < 2) {
      std::cerr << "usage: ghostclaw skills remove <name>\n";
      return 1;
    }
    auto removed = registry.remove(args[1]);
    if (!removed.ok()) {
      std::cerr << removed.error() << "\n";
      return 1;
    }
    std::cout << (removed.value() ? "Removed" : "Not found") << "\n";
    return 0;
  }
  if (args[0] == "import-openclaw") {
    std::vector<skills::OpenClawImportSource> sources = {
        {.path = std::filesystem::current_path() / "references" / "openclaw" / "skills",
         .label = "core"},
        {.path = std::filesystem::current_path() / "references" / "openclaw" / "extensions",
         .label = "extensions"},
        {.path = std::filesystem::current_path() / "references" / "openclaw" / ".agents" / "skills",
         .label = "agents"},
    };
    skills::OpenClawImportOptions options{
        .destination_root = workspace.value() / "skills",
        .sources = std::move(sources),
        .overwrite_existing = true,
    };

    auto imported = skills::import_openclaw_skills(options);
    if (!imported.ok()) {
      std::cerr << imported.error() << "\n";
      return 1;
    }

    std::cout << "Imported " << imported.value().imported << " skill(s)"
              << " (scanned=" << imported.value().scanned
              << ", skipped=" << imported.value().skipped << ")\n";
    if (!imported.value().warnings.empty()) {
      std::cout << "Warnings:\n";
      for (const auto &warning : imported.value().warnings) {
        std::cout << "- " << warning << "\n";
      }
    }
    return 0;
  }
  std::cerr << "unknown skills subcommand\n";
  std::cerr << "available: list, list-workspace, list-community, list-all, search, install, "
               "remove, sync-github, import-openclaw\n";
  return 1;
}

int run_tts(std::vector<std::string> args) {
  auto print_usage = []() {
    std::cout << "usage:\n";
    std::cout << "  ghostclaw tts list\n";
    std::cout << "  ghostclaw tts speak [options] <text>\n";
    std::cout << "options:\n";
    std::cout << "  --provider, -p <system|elevenlabs>\n";
    std::cout << "  --text, -t <text>\n";
    std::cout << "  --stdin\n";
    std::cout << "  --voice, -v <voice>\n";
    std::cout << "  --model <model>\n";
    std::cout << "  --speed <float>\n";
    std::cout << "  --out, -o <path>\n";
    std::cout << "  --dry-run\n";
    std::cout << "  --api-key <elevenlabs_api_key>\n";
    std::cout << "  --base-url <elevenlabs_base_url>\n";
    std::cout << "  --elevenlabs-voice <voice_id>\n";
    std::cout << "  --system-command <say/espeak path>\n";
    std::cout << "  --rate <words_per_minute>\n";
  };

  std::string subcommand = "speak";
  if (!args.empty() && !common::starts_with(args[0], "-")) {
    subcommand = args[0];
    args.erase(args.begin());
  }

  std::string provider = "system";
  std::string text;
  std::string voice;
  std::string model;
  std::string speed_raw;
  std::string output_path_raw;
  std::string api_key;
  std::string base_url;
  std::string elevenlabs_voice_id;
  std::string system_command;
  std::string system_rate;
  const bool dry_run = take_flag(args, "--dry-run");
  const bool read_stdin = take_flag(args, "--stdin");
  (void)take_option(args, "--provider", "-p", provider);
  (void)take_option(args, "--text", "-t", text);
  (void)take_option(args, "--voice", "-v", voice);
  (void)take_option(args, "--model", "", model);
  (void)take_option(args, "--speed", "", speed_raw);
  (void)take_option(args, "--out", "-o", output_path_raw);
  (void)take_option(args, "--api-key", "", api_key);
  (void)take_option(args, "--base-url", "", base_url);
  (void)take_option(args, "--elevenlabs-voice", "", elevenlabs_voice_id);
  (void)take_option(args, "--system-command", "", system_command);
  (void)take_option(args, "--rate", "", system_rate);

  tts::TtsEngine engine;
  tts::SystemTtsConfig system_cfg;
  system_cfg.command = system_command;
  system_cfg.dry_run = dry_run;
  if (!system_rate.empty()) {
    system_cfg.default_rate = system_rate;
  }

  tts::ElevenLabsConfig eleven_cfg;
  eleven_cfg.api_key = api_key;
  eleven_cfg.dry_run = dry_run;
  if (!base_url.empty()) {
    auto normalized = tts::normalize_elevenlabs_base_url(base_url);
    if (!normalized.ok()) {
      std::cerr << normalized.error() << "\n";
      return 1;
    }
    eleven_cfg.base_url = normalized.value();
  }
  if (!elevenlabs_voice_id.empty()) {
    eleven_cfg.default_voice_id = elevenlabs_voice_id;
  }

  auto registered = engine.register_provider(std::make_unique<tts::SystemTtsProvider>(system_cfg));
  if (!registered.ok()) {
    std::cerr << registered.error() << "\n";
    return 1;
  }
  registered = engine.register_provider(std::make_unique<tts::ElevenLabsTtsProvider>(eleven_cfg));
  if (!registered.ok()) {
    std::cerr << registered.error() << "\n";
    return 1;
  }

  if (subcommand == "help" || subcommand == "--help" || subcommand == "-h") {
    print_usage();
    return 0;
  }

  if (subcommand == "list" || subcommand == "providers") {
    auto providers = engine.list_providers();
    std::sort(providers.begin(), providers.end());
    for (const auto &id : providers) {
      std::cout << id << "\n";
    }
    return 0;
  }

  if (subcommand != "speak" && subcommand != "say") {
    std::cerr << "unknown tts subcommand\n";
    print_usage();
    return 1;
  }

  if (text.empty()) {
    text = common::trim(join_tokens(args));
  }
  if (text.empty() && read_stdin) {
    text = common::trim(read_stdin_all());
  }
  if (text.empty()) {
    std::cerr << "tts text is required\n";
    print_usage();
    return 1;
  }

  std::optional<double> speed;
  if (!speed_raw.empty()) {
    try {
      speed = std::stod(speed_raw);
    } catch (...) {
      std::cerr << "invalid speed: " << speed_raw << "\n";
      return 1;
    }
  }

  tts::TtsRequest request;
  request.text = text;
  request.dry_run = dry_run;
  if (!voice.empty()) {
    request.voice = voice;
  }
  if (!model.empty()) {
    request.model = model;
  }
  if (speed.has_value()) {
    request.speed = speed;
  }
  if (!output_path_raw.empty()) {
    request.output_path = std::filesystem::path(output_path_raw);
  }

  auto synthesized = engine.synthesize(request, provider);
  if (!synthesized.ok()) {
    std::cerr << synthesized.error() << "\n";
    return 1;
  }

  const auto &audio = synthesized.value();
  std::cout << "provider: " << audio.provider << "\n";
  std::cout << "mime: " << audio.mime_type << "\n";
  if (audio.output_path.has_value()) {
    std::cout << "output: " << audio.output_path->string() << "\n";
  }
  std::cout << "bytes: " << audio.bytes.size() << "\n";
  return 0;
}

int run_voice(std::vector<std::string> args) {
  auto print_usage = []() {
    std::cout << "usage:\n";
    std::cout << "  ghostclaw voice wake [--wake-word WORD] [--case-sensitive] [--stdin] "
                 "[--text TEXT]\n";
    std::cout << "  ghostclaw voice ptt [--stdin] [--chunk TEXT ...]\n";
  };

  if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
    print_usage();
    return 0;
  }

  const std::string subcommand = args[0];
  args.erase(args.begin());

  if (subcommand == "wake") {
    std::string text;
    std::vector<std::string> wake_words;
    const bool read_stdin = take_flag(args, "--stdin");
    const bool case_sensitive = take_flag(args, "--case-sensitive");
    (void)take_option(args, "--text", "-t", text);

    for (std::size_t i = 0; i < args.size();) {
      if (args[i] == "--wake-word" || args[i] == "-w") {
        if (i + 1 >= args.size()) {
          std::cerr << "missing value for --wake-word\n";
          return 1;
        }
        wake_words.push_back(args[i + 1]);
        args.erase(args.begin() + static_cast<long>(i),
                   args.begin() + static_cast<long>(i + 2));
        continue;
      }
      ++i;
    }

    if (text.empty()) {
      text = common::trim(join_tokens(args));
    }
    if (text.empty() && read_stdin) {
      text = common::trim(read_stdin_all());
    }
    if (text.empty()) {
      std::cerr << "wake transcript text is required\n";
      print_usage();
      return 1;
    }

    voice::WakeWordConfig config;
    config.case_sensitive = case_sensitive;
    if (!wake_words.empty()) {
      config.wake_words = wake_words;
    }
    voice::WakeWordDetector detector(config);
    const auto match = detector.detect(text);
    if (!match.detected) {
      std::cout << "no wake word detected\n";
      return 1;
    }

    std::cout << "detected: true\n";
    std::cout << "wake_word: " << match.wake_word << "\n";
    std::cout << "command: " << match.command_text << "\n";
    std::cout << "position: " << match.position << "\n";
    return 0;
  }

  if (subcommand == "ptt") {
    std::vector<std::string> chunks;
    const bool read_stdin = take_flag(args, "--stdin");

    std::string chunk;
    while (take_option(args, "--chunk", "-c", chunk)) {
      chunks.push_back(chunk);
    }
    while (take_option(args, "--text", "-t", chunk)) {
      chunks.push_back(chunk);
    }
    if (read_stdin) {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (!common::trim(line).empty()) {
          chunks.push_back(line);
        }
      }
    }
    for (const auto &arg : args) {
      if (!common::starts_with(arg, "-")) {
        chunks.push_back(arg);
      }
    }
    if (chunks.empty()) {
      std::cerr << "at least one chunk is required for voice ptt\n";
      print_usage();
      return 1;
    }

    voice::VoiceWakeController controller;
    auto status = controller.push_to_talk().start();
    if (!status.ok()) {
      std::cerr << status.error() << "\n";
      return 1;
    }

    voice::VoiceInputEvent event;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
      event = controller.process_transcript(chunks[i], i + 1 == chunks.size(), true);
    }
    controller.push_to_talk().stop();

    if (event.type != voice::VoiceInputEventType::PushToTalk) {
      std::cerr << "failed to produce push-to-talk transcript\n";
      return 1;
    }
    std::cout << event.text << "\n";
    return 0;
  }

  std::cerr << "unknown voice subcommand\n";
  print_usage();
  return 1;
}

int run_message(std::vector<std::string> args) {
  std::string channel = "cli";
  std::string to;
  std::string message;
  (void)take_option(args, "--channel", "", channel);
  (void)take_option(args, "--to", "", to);
  (void)take_option(args, "--message", "-m", message);
  if (message.empty()) {
    std::cerr << "usage: ghostclaw message --channel <name> --message <text>\n";
    return 1;
  }

  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << cfg.error() << "\n";
    return 1;
  }
  auto manager = channels::create_channel_manager(cfg.value());
  auto *ch = manager->get_channel(channel);
  if (ch == nullptr) {
    std::cerr << "unknown channel: " << channel << "\n";
    return 1;
  }
  auto status = ch->send(to, message);
  if (!status.ok()) {
    std::cerr << status.error() << "\n";
    return 1;
  }
  return 0;
}

int run_integrations(std::vector<std::string> args) {
  integrations::IntegrationRegistry registry;
  if (args.empty() || args[0] == "list") {
    for (const auto &item : registry.all()) {
      std::cout << item.name << " [" << item.category << "] - " << item.description << "\n";
    }
    return 0;
  }
  if (args[0] == "category" && args.size() >= 2) {
    for (const auto &item : registry.by_category(args[1])) {
      std::cout << item.name << " - " << item.description << "\n";
    }
    return 0;
  }
  if ((args[0] == "get" || args[0] == "info") && args.size() >= 2) {
    auto item = registry.find(args[1]);
    if (!item.has_value()) {
      std::cerr << "integration not found\n";
      return 1;
    }
    std::cout << item->name << " [" << item->category << "] - " << item->description << "\n";
    return 0;
  }
  std::cerr << "unknown integrations subcommand\n";
  return 1;
}

int run_config(const std::vector<std::string> &args) {
  if (args.empty() || args[0] == "show") {
    return run_status();
  }

  if (args[0] == "schema") {
    std::cout << config::json_schema() << "\n";
    return 0;
  }

  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << cfg.error() << "\n";
    return 1;
  }

  if (args[0] == "get") {
    if (args.size() < 2) {
      std::cerr << "usage: ghostclaw config get <key>\n";
      return 1;
    }
    const std::string &key = args[1];
    if (key == "default_provider") {
      std::cout << cfg.value().default_provider << "\n";
      return 0;
    }
    if (key == "default_model") {
      std::cout << cfg.value().default_model << "\n";
      return 0;
    }
    if (key == "memory.backend") {
      std::cout << cfg.value().memory.backend << "\n";
      return 0;
    }
    std::cerr << "unknown key: " << key << "\n";
    return 1;
  }

  if (args[0] == "set") {
    if (args.size() < 3) {
      std::cerr << "usage: ghostclaw config set <key> <value>\n";
      return 1;
    }
    const std::string &key = args[1];
    const std::string &value = args[2];
    if (key == "default_provider") {
      cfg.value().default_provider = value;
    } else if (key == "default_model") {
      cfg.value().default_model = value;
    } else if (key == "memory.backend") {
      cfg.value().memory.backend = value;
    } else {
      std::cerr << "unknown key: " << key << "\n";
      return 1;
    }
    auto saved = config::save_config(cfg.value());
    if (!saved.ok()) {
      std::cerr << saved.error() << "\n";
      return 1;
    }
    return 0;
  }

  std::cerr << "unknown config command\n";
  return 1;
}

int run_migrate(std::vector<std::string> args) {
  auto print_usage = []() {
    std::cout << "usage: ghostclaw migrate [legacy] [--settings PATH] [--merge] [--dry-run]\n";
  };

  if (!args.empty() &&
      (args[0] == "help" || args[0] == "--help" || args[0] == "-h")) {
    print_usage();
    return 0;
  }

  if (!args.empty() && !common::starts_with(args[0], "-")) {
    const std::string source = common::to_lower(common::trim(args[0]));
    const std::string compatibility_source = std::string("tiny") + "claw";
    if (source != "legacy" && source != compatibility_source) {
      std::cerr << "unknown migration source: " << args[0] << "\n";
      print_usage();
      return 1;
    }
    args.erase(args.begin());
  }

  std::string settings_path;
  bool merge_with_existing = false;
  bool dry_run = false;
  for (std::size_t i = 0; i < args.size();) {
    if (args[i] == "--merge") {
      merge_with_existing = true;
      args.erase(args.begin() + static_cast<long>(i));
      continue;
    }
    if (args[i] == "--dry-run") {
      dry_run = true;
      args.erase(args.begin() + static_cast<long>(i));
      continue;
    }
    if (args[i] == "--settings" || args[i] == "-s") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for " << args[i] << "\n";
        return 1;
      }
      settings_path = args[i + 1];
      args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i + 2));
      continue;
    }
    if (common::starts_with(args[i], "--settings=")) {
      settings_path = args[i].substr(std::string("--settings=").size());
      if (settings_path.empty()) {
        std::cerr << "missing value for --settings\n";
        return 1;
      }
      args.erase(args.begin() + static_cast<long>(i));
      continue;
    }
    ++i;
  }

  if (!args.empty()) {
    std::cerr << "unknown migrate arguments: " << join_tokens(args) << "\n";
    print_usage();
    return 1;
  }

  migration::LegacyImportOptions options;
  if (!common::trim(settings_path).empty()) {
    options.settings_path = std::filesystem::path(settings_path);
  }
  options.merge_with_existing = merge_with_existing;
  options.write_config = !dry_run;

  auto imported = migration::import_legacy_settings(options);
  if (!imported.ok()) {
    std::cerr << imported.error() << "\n";
    return 1;
  }

  const auto &result = imported.value();
  std::cout << "Imported " << result.imported_agents << " agent(s), " << result.imported_teams
            << " team(s)\n";
  if (result.created_default_agent) {
    std::cout << "No agents were found in source settings; created fallback 'default' agent.\n";
  }
  if (dry_run) {
    std::cout << "Dry run complete: config was not written.\n";
  } else {
    auto path = config::config_path();
    if (path.ok()) {
      std::cout << "Config updated: " << path.value().string() << "\n";
    }
  }
  if (!result.warnings.empty()) {
    std::cout << "Warnings:\n";
    for (const auto &warning : result.warnings) {
      std::cout << "- " << warning << "\n";
    }
  }
  return 0;
}

int run_multi(std::vector<std::string> args) {
  if (!config::config_exists()) {
    std::cerr << "No configuration found. Run 'ghostclaw onboard' first.\n";
    return 1;
  }

  auto context = runtime::RuntimeContext::from_disk();
  if (!context.ok()) {
    std::cerr << context.error() << "\n";
    return 1;
  }

  const auto &config = context.value().config();
  if (config.multi.agents.empty()) {
    std::cerr << "No agents configured. Add [agents.<name>] sections to config.toml.\n";
    std::cerr << "Example:\n";
    std::cerr << "  [agents.coder]\n";
    std::cerr << "  provider = \"anthropic\"\n";
    std::cerr << "  model = \"claude-sonnet-4-20250514\"\n";
    std::cerr << "  system_prompt = \"You are a senior software engineer.\"\n";
    return 1;
  }

  auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    std::cerr << workspace.error() << "\n";
    return 1;
  }

  auto pool = std::make_shared<multi::AgentPool>(config);
  auto store = std::make_shared<sessions::SessionStore>(workspace.value() / "sessions");
  multi::Orchestrator orchestrator(config, pool, store);

  const bool daemon_mode = take_flag(args, "--daemon");
  if (daemon_mode) {
    orchestrator.start([](const std::string &agent_id, const std::string &text, bool) {
      std::cout << "[" << agent_id << "] " << text << "\n";
    });
    std::cout << "Multi-agent daemon started. Press Enter to stop...\n";
    std::string line;
    std::getline(std::cin, line);
    orchestrator.stop();
    return 0;
  }

  orchestrator.run_interactive();
  return 0;
}

int run_login(std::vector<std::string> args) {
  if (take_flag(args, "--logout")) {
    auto status = auth::delete_tokens();
    if (!status.ok()) {
      std::cerr << status.error() << "\n";
      return 1;
    }
    std::cout << "Logged out. OAuth tokens removed.\n";
    return 0;
  }

  if (take_flag(args, "--status")) {
    if (auth::has_valid_tokens()) {
      std::cout << "Logged in (ChatGPT OAuth tokens present)\n";
    } else {
      std::cout << "Not logged in\n";
    }
    return 0;
  }

  auto http = std::make_shared<providers::CurlHttpClient>();
  auto status = auth::run_device_login(*http);
  if (!status.ok()) {
    std::cerr << "Login failed: " << status.error() << "\n";
    return 1;
  }
  return 0;
}

int run_conway(std::vector<std::string> args) {
  if (args.empty()) {
    std::cerr << "usage: ghostclaw conway <subcommand>\n";
    std::cerr << "  setup    Initialize Conway wallet and API key\n";
    std::cerr << "  status   Show Conway credit balance and wallet address\n";
    std::cerr << "  fund     Show deposit instructions to fund your wallet\n";
    return 1;
  }

  const std::string subcommand = args[0];
  args.erase(args.begin());

  if (subcommand == "setup") {
    std::cout << "Setting up Conway Terminal...\n";
    // Run conway-terminal --provision to create wallet and API key
    const int ret = std::system("npx conway-terminal --provision");
    if (ret != 0) {
      std::cerr << "Conway setup failed. Make sure Node.js is installed.\n";
      std::cerr << "Install manually: npm install -g conway-terminal\n";
      return 1;
    }

    // Try to read the generated API key and store it in config
    const std::string conway_config_path =
        common::expand_path("~/.conway/config.json");
    if (std::filesystem::exists(conway_config_path)) {
      std::ifstream conway_cfg(conway_config_path);
      if (conway_cfg) {
        std::stringstream buf;
        buf << conway_cfg.rdbuf();
        const std::string content = buf.str();

        // Extract apiKey from JSON
        std::size_t key_pos = content.find("\"apiKey\"");
        if (key_pos != std::string::npos) {
          const auto quote1 = content.find('"', key_pos + 8);
          const auto quote2 = (quote1 != std::string::npos)
                                  ? content.find('"', quote1 + 1)
                                  : std::string::npos;
          const auto quote3 = (quote2 != std::string::npos)
                                  ? content.find('"', quote2 + 1)
                                  : std::string::npos;
          if (quote2 != std::string::npos && quote3 != std::string::npos) {
            const std::string api_key = content.substr(quote2 + 1, quote3 - quote2 - 1);
            if (!api_key.empty() && api_key.find("cnwy_") != std::string::npos) {
              auto cfg = config::load_config();
              if (cfg.ok()) {
                cfg.value().conway.enabled = true;
                cfg.value().conway.api_key = api_key;
                const auto save_status = config::save_config(cfg.value());
                if (save_status.ok()) {
                  std::cout << "Conway API key saved to config.toml\n";
                } else {
                  std::cerr << "Warning: could not save Conway API key: "
                            << save_status.error() << "\n";
                  std::cout << "Add manually to config.toml:\n\n";
                  std::cout << "[conway]\n";
                  std::cout << "enabled = true\n";
                  std::cout << "api_key = \"" << api_key << "\"\n";
                }
              }
            }
          }
        }
      }
    }

    std::cout << "\nConway setup complete!\n";
    std::cout << "Run 'ghostclaw conway status' to verify your balance.\n";
    return 0;
  }

  if (subcommand == "status") {
    auto cfg = config::load_config();
    if (!cfg.ok()) {
      std::cerr << cfg.error() << "\n";
      return 1;
    }

    const auto &conway = cfg.value().conway;
    if (!conway.enabled && conway.api_key.empty()) {
      std::cerr << "Conway is not configured.\n";
      std::cerr << "Run 'ghostclaw conway setup' to initialize.\n";
      return 1;
    }

    // Read wallet address
    auto wallet = conway::read_wallet_address(conway);
    if (wallet.ok()) {
      std::cout << "Wallet: " << wallet.value() << "\n";
    } else {
      std::cout << "Wallet: not yet initialized (run 'ghostclaw conway setup')\n";
    }

    std::cout << "API key: " << (conway.api_key.empty() ? "(not set)" :
                                  conway.api_key.substr(0, 8) + "...") << "\n";
    std::cout << "API URL: " << conway.api_url << "\n";
    std::cout << "Region: " << conway.default_region << "\n";
    std::cout << "Survival monitoring: "
              << (conway.survival_monitoring ? "enabled" : "disabled") << "\n";
    if (conway.survival_monitoring) {
      std::cout << "  Low compute threshold: $" << conway.low_compute_threshold_usd << "\n";
      std::cout << "  Critical threshold: $" << conway.critical_threshold_usd << "\n";
    }
    std::cout << "\nFor live credit balance, use the agent:\n";
    std::cout << "  ghostclaw agent -m \"check my Conway credits\"\n";
    return 0;
  }

  if (subcommand == "fund") {
    auto cfg = config::load_config();
    if (!cfg.ok()) {
      std::cerr << cfg.error() << "\n";
      return 1;
    }

    const auto &conway = cfg.value().conway;
    auto wallet = conway::read_wallet_address(conway);

    std::cout << "To fund your Conway wallet with USDC:\n\n";
    if (wallet.ok()) {
      std::cout << "  Wallet address: " << wallet.value() << "\n\n";
    }
    std::cout << "Options:\n";
    std::cout << "  1. Buy credits at: https://app.conway.tech\n";
    std::cout << "  2. Send USDC (Base network) directly to your wallet address\n";
    std::cout << "\nNetwork: Base (EVM, chain ID 8453)\n";
    std::cout << "Token: USDC\n";
    return 0;
  }

  std::cerr << "unknown conway subcommand: " << subcommand << "\n";
  return 1;
}

int run_sovereign(std::vector<std::string> args) {
  if (args.empty()) {
    std::cerr << "usage: ghostclaw sovereign <subcommand>\n";
    std::cerr << "  start    Start GhostClaw in sovereign mode with survival pressure\n";
    std::cerr << "  status   Show survival tier, credits, and wallet\n";
    std::cerr << "  fund     Show funding instructions\n";
    return 1;
  }

  const std::string subcommand = args[0];
  args.erase(args.begin());

  auto cfg = config::load_config();
  if (!cfg.ok()) {
    std::cerr << cfg.error() << "\n";
    return 1;
  }

  if (subcommand == "status") {
    const auto &conway = cfg.value().conway;
    if (!conway.enabled && conway.api_key.empty()) {
      std::cerr << "Sovereign mode requires Conway. Run 'ghostclaw conway setup' first.\n";
      return 1;
    }

    auto wallet = conway::read_wallet_address(conway);
    std::cout << "Sovereign Mode Status\n";
    std::cout << std::string(40, '-') << "\n";
    if (wallet.ok()) {
      std::cout << "Wallet: " << wallet.value() << "\n";
    }
    std::cout << "Survival monitoring: "
              << (conway.survival_monitoring ? "active" : "inactive") << "\n";
    if (conway.survival_monitoring) {
      std::cout << "Tiers:\n";
      std::cout << "  normal     > $" << conway.low_compute_threshold_usd << "\n";
      std::cout << "  low_compute $" << conway.critical_threshold_usd
                << " - $" << conway.low_compute_threshold_usd << "\n";
      std::cout << "  critical   < $" << conway.critical_threshold_usd << "\n";
      std::cout << "  dead        $0.00\n";
    }
    std::cout << "\nCheck live balance: ghostclaw agent -m \"check my Conway credits\"\n";
    return 0;
  }

  if (subcommand == "start") {
    const auto &conway = cfg.value().conway;
    if (!conway.enabled || conway.api_key.empty()) {
      std::cerr << "Sovereign mode requires Conway to be configured with an API key.\n";
      std::cerr << "Run 'ghostclaw conway setup' first.\n";
      return 1;
    }

    std::cout << "Starting GhostClaw in sovereign mode...\n";
    std::cout << "The agent will autonomously monitor its Conway credit balance\n";
    std::cout << "and adapt behavior based on survival tier.\n\n";

    // Enable survival monitoring and start the agent
    cfg.value().conway.survival_monitoring = true;
    args.clear();
    return run_agent(std::move(args));
  }

  if (subcommand == "fund") {
    args.insert(args.begin(), "fund");
    return run_conway(std::move(args));
  }

  std::cerr << "unknown sovereign subcommand: " << subcommand << "\n";
  return 1;
}

int run_google(std::vector<std::string> args) {
  if (args.empty()) {
    std::cerr << "usage: ghostclaw google <subcommand>\n";
    std::cerr << "  login    Authorize GhostClaw with your Google account\n";
    std::cerr << "  logout   Remove Google OAuth tokens\n";
    std::cerr << "  status   Check Google auth status\n";
    return 1;
  }

  const std::string subcommand = args[0];
  args.erase(args.begin());

  if (subcommand == "logout") {
    auto status = auth::delete_google_tokens();
    if (!status.ok()) {
      std::cerr << status.error() << "\n";
      return 1;
    }
    std::cout << "Google tokens removed.\n";
    return 0;
  }

  if (subcommand == "status") {
    if (auth::has_valid_google_tokens()) {
      std::cout << "Google: authorized (tokens present)\n";
    } else {
      std::cout << "Google: not authorized\n";
    }
    return 0;
  }

  if (subcommand == "login") {
    auto cfg = config::load_config();
    if (!cfg.ok()) {
      std::cerr << cfg.error() << "\n";
      return 1;
    }

    if (cfg.value().google.client_id.empty()) {
      std::cerr << "google.client_id is not set in config.toml\n";
      std::cerr << "Add the following to your config:\n\n";
      std::cerr << "  [google]\n";
      std::cerr << "  client_id = \"your-client-id.apps.googleusercontent.com\"\n";
      std::cerr << "  client_secret = \"your-client-secret\"\n";
      return 1;
    }

    auto http = std::make_shared<providers::CurlHttpClient>();
    auto status = auth::run_google_login(*http, cfg.value().google);
    if (!status.ok()) {
      std::cerr << "Google login failed: " << status.error() << "\n";
      return 1;
    }
    return 0;
  }

  std::cerr << "unknown google subcommand: " << subcommand << "\n";
  return 1;
}

} // namespace

void print_help() {
  constexpr const char *RESET   = "\033[0m";
  constexpr const char *BOLD    = "\033[1m";
  constexpr const char *DIM     = "\033[2m";
  constexpr const char *CYAN    = "\033[36m";
  constexpr const char *GREEN   = "\033[32m";
  constexpr const char *YELLOW  = "\033[33m";

  std::cout << "\n";
  std::cout << BOLD << CYAN << "  🐾 GhostClaw" << RESET << DIM
            << " — Ghost Protocol. Claw Execution. Zero Compromise." << RESET << "\n";
  std::cout << DIM << "  " << version_string() << RESET << "\n\n";

  std::cout << BOLD << "  USAGE" << RESET << "\n";
  std::cout << DIM << "  $ " << RESET << "ghostclaw [--config PATH] <command> [options]\n\n";

  std::cout << BOLD << "  GETTING STARTED" << RESET << "\n";
  std::cout << "  " << GREEN << "onboard" << RESET << DIM << "        Interactive setup wizard" << RESET << "\n";
  std::cout << "  " << GREEN << "login" << RESET << DIM << "          Login with ChatGPT (no API key needed)" << RESET << "\n";
  std::cout << "  " << GREEN << "google login" << RESET << DIM << "   Authorize GhostClaw with your Google account" << RESET << "\n";
  std::cout << "  " << GREEN << "agent" << RESET << DIM << "          Start interactive AI agent session" << RESET << "\n";
  std::cout << "  " << GREEN << "agent -m" << RESET << " MSG" << DIM << "  Run a single message and exit" << RESET << "\n\n";

  std::cout << BOLD << "  SERVICES" << RESET << "\n";
  std::cout << "  " << GREEN << "gateway" << RESET << DIM << "        Start HTTP/WebSocket API server" << RESET << "\n";
  std::cout << "  " << GREEN << "daemon" << RESET << DIM << "         Run as background daemon with channels" << RESET << "\n";
  std::cout << "  " << GREEN << "service" << RESET << DIM << "        Manage background service lifecycle" << RESET << "\n";
  std::cout << "  " << GREEN << "migrate" << RESET << DIM << "        Import legacy settings into GhostClaw" << RESET << "\n";
  std::cout << "  " << GREEN << "multi" << RESET << DIM << "          Multi-agent team collaboration mode" << RESET << "\n";
  std::cout << "  " << GREEN << "channel" << RESET << DIM << "        Manage messaging channels (Telegram, Slack, etc)" << RESET << "\n\n";

  std::cout << BOLD << "  SKILLS & TOOLS" << RESET << "\n";
  std::cout << "  " << GREEN << "skills list" << RESET << DIM << "    List installed skills" << RESET << "\n";
  std::cout << "  " << GREEN << "skills search" << RESET << DIM << "  Search for skills" << RESET << "\n";
  std::cout << "  " << GREEN << "skills install" << RESET << DIM << " Install a skill" << RESET << "\n\n";
  std::cout << "  " << GREEN << "skills import-openclaw" << RESET << DIM
            << " Import all OpenClaw reference skills" << RESET << "\n\n";

  std::cout << BOLD << "  DIAGNOSTICS" << RESET << "\n";
  std::cout << "  " << GREEN << "status" << RESET << DIM << "         Show system status" << RESET << "\n";
  std::cout << "  " << GREEN << "doctor" << RESET << DIM << "         Run health diagnostics" << RESET << "\n";
  std::cout << "  " << GREEN << "config show" << RESET << DIM << "    Display current configuration" << RESET << "\n\n";

  std::cout << BOLD << "  CONWAY & SOVEREIGN MODE" << RESET << "\n";
  std::cout << "  " << GREEN << "conway setup" << RESET << DIM << "   Initialize Conway wallet + API key" << RESET << "\n";
  std::cout << "  " << GREEN << "conway status" << RESET << DIM << "  Show Conway wallet and credit info" << RESET << "\n";
  std::cout << "  " << GREEN << "conway fund" << RESET << DIM << "    Show USDC deposit instructions" << RESET << "\n";
  std::cout << "  " << GREEN << "sovereign start" << RESET << DIM << " Run agent in sovereign mode (survival pressure)" << RESET << "\n\n";

  std::cout << BOLD << "  OTHER" << RESET << "\n";
  std::cout << "  " << GREEN << "cron" << RESET << DIM << "           Manage scheduled tasks" << RESET << "\n";
  std::cout << "  " << GREEN << "models" << RESET << DIM << "         Refresh/list provider model catalogs" << RESET << "\n";
  std::cout << "  " << GREEN << "providers" << RESET << DIM << "      List provider IDs and aliases" << RESET << "\n";
  std::cout << "  " << GREEN << "hardware" << RESET << DIM << "       Discover and inspect hardware devices" << RESET << "\n";
  std::cout << "  " << GREEN << "peripheral" << RESET << DIM << "     Configure hardware peripherals" << RESET << "\n";
  std::cout << "  " << GREEN << "tts" << RESET << DIM << "            Text-to-speech" << RESET << "\n";
  std::cout << "  " << GREEN << "voice" << RESET << DIM << "          Voice control (wake word / push-to-talk)" << RESET << "\n";
  std::cout << "  " << GREEN << "message" << RESET << DIM << "        Send message to a channel" << RESET << "\n";
  std::cout << "  " << GREEN << "version" << RESET << DIM << "        Show version" << RESET << "\n\n";

  std::cout << BOLD << "  INTERACTIVE MODE COMMANDS" << RESET << DIM << " (inside 'ghostclaw agent')" << RESET << "\n";
  std::cout << "  " << YELLOW << "/help" << RESET << "  " << YELLOW << "/skills" << RESET
            << "  " << YELLOW << "/skill <name>" << RESET << "  " << YELLOW << "/tools" << RESET
            << "  " << YELLOW << "/model" << RESET << "  " << YELLOW << "/memory" << RESET
            << "  " << YELLOW << "/status" << RESET << "\n";
  std::cout << "  " << YELLOW << "/history" << RESET << "  " << YELLOW << "/export" << RESET
            << "  " << YELLOW << "/compact" << RESET << "  " << YELLOW << "/tokens" << RESET
            << "  " << YELLOW << "/clear" << RESET << "  " << YELLOW << "/quit" << RESET << "\n";
  std::cout << "\n";
}

int run_cli(int argc, char **argv) {
  if (argc <= 1) {
    if (!config::config_exists()) {
      if (!stdin_is_tty()) {
        std::cerr << "No configuration found and stdin is not interactive.\n";
        std::cerr << "Run 'ghostclaw onboard --non-interactive --provider <name> --model <name>' "
                     "to bootstrap.\n";
        return 1;
      }
      // First run: auto-launch the onboarding wizard
      onboard::WizardOptions wizard_opts;
      wizard_opts.interactive = true;
      wizard_opts.offer_launch = true;
      auto result = onboard::run_wizard(wizard_opts);
      if (!result.success) {
        std::cerr << "onboard failed: " << result.error << "\n";
        return 1;
      }
      if (result.launch_agent) {
        return run_agent({});
      }
      return 0;
    }
    print_help();
    return 0;
  }

  std::vector<std::string> args = collect_args(argc - 1, argv + 1);
  std::string global_error;
  if (!apply_global_options(args, global_error)) {
    std::cerr << global_error << "\n";
    return 1;
  }

  if (args.empty()) {
    if (!config::config_exists()) {
      if (!stdin_is_tty()) {
        std::cerr << "No configuration found and stdin is not interactive.\n";
        std::cerr << "Run 'ghostclaw onboard --non-interactive --provider <name> --model <name>' "
                     "to bootstrap.\n";
        return 1;
      }
      onboard::WizardOptions wizard_opts;
      wizard_opts.interactive = true;
      wizard_opts.offer_launch = true;
      auto result = onboard::run_wizard(wizard_opts);
      if (!result.success) {
        std::cerr << "onboard failed: " << result.error << "\n";
        return 1;
      }
      if (result.launch_agent) {
        return run_agent({});
      }
      return 0;
    }
    print_help();
    return 0;
  }

  const std::string subcommand = args[0];
  args.erase(args.begin());

  if (subcommand == "--help" || subcommand == "-h" || subcommand == "help") {
    print_help();
    return 0;
  }
  if (subcommand == "--version" || subcommand == "-V" || subcommand == "version") {
    std::cout << version_string() << "\n";
    return 0;
  }
  if (subcommand == "config-path") {
    auto path_result = config::config_path();
    if (!path_result.ok()) {
      std::cerr << path_result.error() << "\n";
      return 1;
    }
    std::cout << path_result.value().string() << "\n";
    return 0;
  }
  if (subcommand == "onboard") {
    return run_onboard(std::move(args));
  }
  if (subcommand == "agent") {
    return run_agent(std::move(args));
  }
  if (subcommand == "gateway") {
    return run_gateway(std::move(args));
  }
  if (subcommand == "status") {
    return run_status();
  }
  if (subcommand == "doctor") {
    return run_doctor();
  }
  if (subcommand == "login") {
    return run_login(std::move(args));
  }
  if (subcommand == "config") {
    return run_config(std::move(args));
  }
  if (subcommand == "daemon") {
    return run_daemon(std::move(args));
  }
  if (subcommand == "cron") {
    return run_cron(std::move(args));
  }
  if (subcommand == "models") {
    return run_models(std::move(args));
  }
  if (subcommand == "providers") {
    return run_providers();
  }
  if (subcommand == "channel") {
    return run_channel(std::move(args));
  }
  if (subcommand == "skills") {
    return run_skills(std::move(args));
  }
  if (subcommand == "tts") {
    return run_tts(std::move(args));
  }
  if (subcommand == "voice") {
    return run_voice(std::move(args));
  }
  if (subcommand == "integrations") {
    return run_integrations(std::move(args));
  }
  if (subcommand == "multi") {
    return run_multi(std::move(args));
  }
  if (subcommand == "message") {
    return run_message(std::move(args));
  }
  if (subcommand == "google") {
    return run_google(std::move(args));
  }
  if (subcommand == "conway") {
    return run_conway(std::move(args));
  }
  if (subcommand == "sovereign") {
    return run_sovereign(std::move(args));
  }
  if (subcommand == "hardware") {
    return run_hardware(std::move(args));
  }
  if (subcommand == "peripheral") {
    return run_peripheral(std::move(args));
  }
  if (subcommand == "migrate") {
    return run_migrate(std::move(args));
  }
  if (subcommand == "service") {
    return run_service(args, argc > 0 && argv != nullptr ? argv[0] : "ghostclaw");
  }

  std::cerr << "Unknown command: " << subcommand << "\n";
  print_help();
  return 1;
}

} // namespace ghostclaw::cli
