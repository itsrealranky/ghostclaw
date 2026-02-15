#include "ghostclaw/cli/commands.hpp"

#include "ghostclaw/auth/oauth.hpp"
#include "ghostclaw/multi/agent_pool.hpp"
#include "ghostclaw/multi/orchestrator.hpp"
#include "ghostclaw/channels/channel_manager.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/config/config.hpp"
#include "ghostclaw/daemon/daemon.hpp"
#include "ghostclaw/doctor/diagnostics.hpp"
#include "ghostclaw/gateway/server.hpp"
#include "ghostclaw/heartbeat/cron.hpp"
#include "ghostclaw/heartbeat/cron_store.hpp"
#include "ghostclaw/integrations/registry.hpp"
#include "ghostclaw/onboard/wizard.hpp"
#include "ghostclaw/runtime/app.hpp"
#include "ghostclaw/skills/import_openclaw.hpp"
#include "ghostclaw/skills/registry.hpp"
#include "ghostclaw/tts/tts.hpp"
#include "ghostclaw/voice/wake.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
  if (explicit_non_interactive) {
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
  }
  if (!model.empty()) {
    options.model_override = model;
  }
  if (!temperature_raw.empty()) {
    try {
      options.temperature_override = std::stod(temperature_raw);
    } catch (...) {
      std::cerr << "invalid temperature: " << temperature_raw << "\n";
      return 1;
    }
  }

  auto engine = context.value().create_agent_engine();
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

  if (args.empty() || args[0] == "list") {
    auto jobs = store.list_jobs();
    if (!jobs.ok()) {
      std::cerr << jobs.error() << "\n";
      return 1;
    }
    for (const auto &job : jobs.value()) {
      std::cout << job.id << " | " << job.expression << " | " << job.command << "\n";
    }
    return 0;
  }

  if (args[0] == "add") {
    if (args.size() < 3) {
      std::cerr << "usage: ghostclaw cron add <expression> <command>\n";
      return 1;
    }
    auto expression = heartbeat::CronExpression::parse(args[1]);
    if (!expression.ok()) {
      std::cerr << expression.error() << "\n";
      return 1;
    }

    std::string command = args[2];
    for (std::size_t i = 3; i < args.size(); ++i) {
      command += " " + args[i];
    }

    heartbeat::CronJob job;
    job.id = "job-" + std::to_string(std::time(nullptr));
    job.expression = args[1];
    job.command = command;
    job.next_run = expression.value().next_occurrence();
    auto added = store.add_job(job);
    if (!added.ok()) {
      std::cerr << added.error() << "\n";
      return 1;
    }
    std::cout << "Added cron job: " << job.id << "\n";
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

  std::cerr << "unknown cron subcommand\n";
  return 1;
}

int run_channel(std::vector<std::string> args) {
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
  if (args[0] == "get" && args.size() >= 2) {
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

int run_config(std::vector<std::string> args) {
  if (args.empty() || args[0] == "show") {
    return run_status();
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
    orchestrator.start([](const std::string &agent_id, const std::string &text) {
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
  std::cout << "  " << GREEN << "agent" << RESET << DIM << "          Start interactive AI agent (Claude Code-style)" << RESET << "\n";
  std::cout << "  " << GREEN << "agent -m" << RESET << " MSG" << DIM << "  Run a single message and exit" << RESET << "\n\n";

  std::cout << BOLD << "  SERVICES" << RESET << "\n";
  std::cout << "  " << GREEN << "gateway" << RESET << DIM << "        Start HTTP/WebSocket API server" << RESET << "\n";
  std::cout << "  " << GREEN << "daemon" << RESET << DIM << "         Run as background daemon with channels" << RESET << "\n";
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

  std::cout << BOLD << "  OTHER" << RESET << "\n";
  std::cout << "  " << GREEN << "cron" << RESET << DIM << "           Manage scheduled tasks" << RESET << "\n";
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
  if (subcommand == "service" || subcommand == "migrate") {
    std::cout << subcommand << " command is available but not yet fully implemented.\n";
    return 0;
  }

  std::cerr << "Unknown command: " << subcommand << "\n";
  print_help();
  return 1;
}

} // namespace ghostclaw::cli
