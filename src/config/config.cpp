#include "ghostclaw/config/config.hpp"

#include "ghostclaw/auth/oauth.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/toml.hpp"

#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <string_view>
#include <regex>
#include <set>
#include <sstream>
#include <mutex>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ghostclaw::config {

namespace {

constexpr const char *CONFIG_FOLDER = ".ghostclaw";
constexpr const char *CONFIG_FILENAME = "config.toml";
std::optional<std::filesystem::path> g_config_path_override;
std::string g_loaded_dotenv_cache_key;
std::mutex g_dotenv_mutex;

struct MappedConfigFile {
  MappedConfigFile() = default;
  MappedConfigFile(const MappedConfigFile &) = delete;
  MappedConfigFile &operator=(const MappedConfigFile &) = delete;

  MappedConfigFile(MappedConfigFile &&other) noexcept { *this = std::move(other); }
  MappedConfigFile &operator=(MappedConfigFile &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    release();
#ifndef _WIN32
    data = other.data;
    size = other.size;
    fd = other.fd;
    mmap_used = other.mmap_used;
    other.data = nullptr;
    other.size = 0;
    other.fd = -1;
    other.mmap_used = false;
#else
    size = other.size;
#endif
    owned = std::move(other.owned);
    if (!owned.empty()) {
      data = owned.data();
      size = owned.size();
    }
    return *this;
  }

  ~MappedConfigFile() { release(); }

  [[nodiscard]] std::string_view view() const {
    if (data == nullptr || size == 0) {
      return {};
    }
    return std::string_view(data, size);
  }

  void set_owned(std::string text) {
    release();
    owned = std::move(text);
    data = owned.data();
    size = owned.size();
  }

  void set_mapped(const char *mapped_data, const std::size_t mapped_size,
#ifndef _WIN32
                  const int mapped_fd,
#endif
                  const bool mapped) {
    release();
    data = mapped_data;
    size = mapped_size;
#ifndef _WIN32
    fd = mapped_fd;
    mmap_used = mapped;
#else
    (void)mapped;
#endif
  }

private:
  void release() {
#ifndef _WIN32
    if (mmap_used && data != nullptr && size > 0) {
      (void)munmap(const_cast<char *>(data), size);
    }
    if (fd >= 0) {
      (void)close(fd);
    }
    fd = -1;
    mmap_used = false;
#endif
    data = nullptr;
    size = 0;
    owned.clear();
  }

public:
  const char *data = nullptr;
  std::size_t size = 0;
  std::string owned;
#ifndef _WIN32
  int fd = -1;
  bool mmap_used = false;
#endif
};

std::optional<std::filesystem::path> resolved_config_path_override() {
  if (g_config_path_override.has_value()) {
    return std::filesystem::path(common::expand_path(g_config_path_override->string()));
  }
  if (const char *env = std::getenv("GHOSTCLAW_CONFIG_PATH"); env != nullptr && *env != '\0') {
    return std::filesystem::path(common::expand_path(env));
  }
  return std::nullopt;
}

common::Result<MappedConfigFile> read_config_file(const std::filesystem::path &path) {
#ifndef _WIN32
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    struct stat stats {};
    if (fstat(fd, &stats) == 0 && stats.st_size >= 0) {
      const auto mapped_size = static_cast<std::size_t>(stats.st_size);
      if (mapped_size == 0) {
        MappedConfigFile empty;
        empty.set_owned({});
        return common::Result<MappedConfigFile>::success(std::move(empty));
      }

      void *mapped = mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapped != MAP_FAILED) {
        MappedConfigFile file;
        file.set_mapped(static_cast<const char *>(mapped), mapped_size, fd, true);
        return common::Result<MappedConfigFile>::success(std::move(file));
      }
    }
    (void)close(fd);
  }
#endif

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return common::Result<MappedConfigFile>::failure("Unable to open config file: " + path.string());
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  MappedConfigFile out;
  out.set_owned(buffer.str());
  return common::Result<MappedConfigFile>::success(std::move(out));
}

std::string dotenv_cache_key() {
  std::ostringstream key;
  if (const char *env_file = std::getenv("GHOSTCLAW_ENV_FILE");
      env_file != nullptr && *env_file != '\0') {
    key << "env_file=" << common::expand_path(env_file);
  }
  key << "|home=";
  if (const auto home = common::home_dir(); home.ok()) {
    key << home.value().string();
  }
  if (const auto override_path = resolved_config_path_override(); override_path.has_value()) {
    key << "|config_override=" << override_path->string();
  }
  key << "|cwd=";
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    key << cwd.string();
  }
  return key.str();
}

std::string expand_config_value(const std::string &value) {
  if (value.find('$') == std::string::npos && value.find('~') == std::string::npos) {
    return value;
  }
  return common::expand_path(value);
}

std::string strip_env_quotes(const std::string &raw) {
  std::string value = common::trim(raw);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    std::string out;
    out.reserve(value.size() - 2);
    bool escaped = false;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
      const char ch = value[i];
      if (!escaped) {
        if (ch == '\\') {
          escaped = true;
          continue;
        }
        out.push_back(ch);
        continue;
      }
      switch (ch) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '"':
      case '\\':
        out.push_back(ch);
        break;
      default:
        out.push_back(ch);
        break;
      }
      escaped = false;
    }
    return out;
  }
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

bool is_valid_env_name(const std::string &name) {
  if (name.empty()) {
    return false;
  }
  if (!(std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_')) {
    return false;
  }
  for (const char ch : name) {
    const auto uch = static_cast<unsigned char>(ch);
    if (!(std::isalnum(uch) != 0 || ch == '_')) {
      return false;
    }
  }
  return true;
}

void set_env_if_missing(const std::string &name, const std::string &value) {
  if (!is_valid_env_name(name)) {
    return;
  }
  if (const char *existing = std::getenv(name.c_str()); existing != nullptr && *existing != '\0') {
    return;
  }
#if defined(_WIN32)
  _putenv_s(name.c_str(), value.c_str());
#else
  setenv(name.c_str(), value.c_str(), 0);
#endif
}

void load_dotenv_file(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
    return;
  }

  std::ifstream file(path);
  if (!file) {
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    std::string trimmed = common::trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }
    if (common::starts_with(trimmed, "export ")) {
      trimmed = common::trim(trimmed.substr(7));
    }

    const auto eq = trimmed.find('=');
    if (eq == std::string::npos) {
      continue;
    }

    const std::string key = common::trim(trimmed.substr(0, eq));
    const std::string value = strip_env_quotes(trimmed.substr(eq + 1));
    if (key.empty()) {
      continue;
    }
    set_env_if_missing(key, value);
  }
}

void load_dotenv_files() {
  std::lock_guard<std::mutex> lock(g_dotenv_mutex);
  const std::string cache_key = dotenv_cache_key();
  if (cache_key == g_loaded_dotenv_cache_key) {
    return;
  }

  std::vector<std::filesystem::path> candidates;
  if (const char *env_file = std::getenv("GHOSTCLAW_ENV_FILE");
      env_file != nullptr && *env_file != '\0') {
    candidates.emplace_back(common::expand_path(env_file));
  }
  // Config dir .env takes priority (loaded first so set_env_if_missing keeps it)
  if (auto dir = config_dir(); dir.ok()) {
    candidates.push_back(dir.value() / ".env");
  }
  // CWD .env as fallback for dev builds
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    candidates.push_back(cwd / ".env");
  }

  for (const auto &candidate : candidates) {
    load_dotenv_file(candidate);
  }

  g_loaded_dotenv_cache_key = cache_key;
}

void load_channel_config(Config &config, const common::TomlDocument &doc) {
  if (doc.has("channels.telegram.bot_token")) {
    TelegramConfig telegram;
    telegram.bot_token = expand_config_value(doc.get_string("channels.telegram.bot_token"));
    telegram.allowed_users = doc.get_string_array("channels.telegram.allowed_users");
    config.channels.telegram = std::move(telegram);
  }

  if (doc.has("channels.discord.bot_token")) {
    DiscordConfig discord;
    discord.bot_token = expand_config_value(doc.get_string("channels.discord.bot_token"));
    discord.guild_id = expand_config_value(doc.get_string("channels.discord.guild_id"));
    discord.allowed_users = doc.get_string_array("channels.discord.allowed_users");
    config.channels.discord = std::move(discord);
  }

  if (doc.has("channels.slack.bot_token")) {
    SlackConfig slack;
    slack.bot_token = expand_config_value(doc.get_string("channels.slack.bot_token"));
    slack.channel_id = expand_config_value(doc.get_string("channels.slack.channel_id"));
    slack.allowed_users = doc.get_string_array("channels.slack.allowed_users");
    config.channels.slack = std::move(slack);
  }

  if (doc.has("channels.matrix.homeserver")) {
    MatrixConfig matrix;
    matrix.homeserver = expand_config_value(doc.get_string("channels.matrix.homeserver"));
    matrix.access_token = expand_config_value(doc.get_string("channels.matrix.access_token"));
    matrix.room_id = expand_config_value(doc.get_string("channels.matrix.room_id"));
    config.channels.matrix = std::move(matrix);
  }

  if (doc.has("channels.imessage.allowed_contacts")) {
    IMessageConfig imessage;
    imessage.allowed_contacts = doc.get_string_array("channels.imessage.allowed_contacts");
    config.channels.imessage = std::move(imessage);
  }

  if (doc.has("channels.whatsapp.access_token")) {
    WhatsAppConfig whatsapp;
    whatsapp.access_token = expand_config_value(doc.get_string("channels.whatsapp.access_token"));
    whatsapp.phone_number_id = expand_config_value(doc.get_string("channels.whatsapp.phone_number_id"));
    whatsapp.verify_token = expand_config_value(doc.get_string("channels.whatsapp.verify_token"));
    whatsapp.allowed_numbers = doc.get_string_array("channels.whatsapp.allowed_numbers");
    config.channels.whatsapp = std::move(whatsapp);
  }

  if (doc.has("channels.webhook.secret")) {
    WebhookConfig webhook;
    webhook.secret = expand_config_value(doc.get_string("channels.webhook.secret"));
    config.channels.webhook = std::move(webhook);
  }
}

void load_tunnel_config(Config &config, const common::TomlDocument &doc) {
  config.tunnel.provider = doc.get_string("tunnel.provider", config.tunnel.provider);

  if (doc.has("tunnel.cloudflare.command_path")) {
    CloudflareConfig cloudflare;
    cloudflare.command_path = doc.get_string("tunnel.cloudflare.command_path");
    config.tunnel.cloudflare = std::move(cloudflare);
  }

  if (doc.has("tunnel.ngrok.auth_token")) {
    NgrokConfig ngrok;
    ngrok.auth_token = expand_config_value(doc.get_string("tunnel.ngrok.auth_token"));
    config.tunnel.ngrok = std::move(ngrok);
  }

  if (doc.has("tunnel.tailscale.hostname")) {
    TailscaleConfig tailscale;
    tailscale.hostname = expand_config_value(doc.get_string("tunnel.tailscale.hostname"));
    config.tunnel.tailscale = std::move(tailscale);
  }

  if (doc.has("tunnel.custom.command")) {
    CustomTunnelConfig custom;
    custom.command = expand_config_value(doc.get_string("tunnel.custom.command"));
    custom.args = doc.get_string_array("tunnel.custom.args");
    config.tunnel.custom = std::move(custom);
  }
}

bool is_valid_host(const std::string &host) {
  if (host.empty()) {
    return false;
  }

  static const std::regex host_re(
      R"(^(([a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?\.)*[a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?|((25[0-5]|2[0-4][0-9]|[01]?[0-9]?[0-9])\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9]?[0-9])|::1|\[::1\])$)");
  return std::regex_match(host, host_re);
}

std::vector<std::string> known_providers() {
  return {"openrouter",
          "anthropic",
          "openai",
          "openai-codex",
          "opencode",
          "google",
          "google-vertex",
          "google-antigravity",
          "google-gemini-cli",
          "zai",
          "vercel-ai-gateway",
          "xai",
          "grok",
          "groq",
          "cerebras",
          "mistral",
          "github-copilot",
          "huggingface",
          "moonshot",
          "kimi-coding",
          "qwen-portal",
          "synthetic",
          "minimax",
          "ollama",
          "vllm",
          "litellm",
          "xiaomi",
          "venice",
          "together",
          "qianfan",
          "deepseek",
          "fireworks",
          "perplexity",
          "cohere",
          "nvidia",
          "cloudflare-ai-gateway",
          "cloudflare",
          "glm"};
}

std::string normalize_provider_alias(const std::string &provider) {
  std::string normalized = common::to_lower(common::trim(provider));
  if (normalized == "z.ai" || normalized == "z-ai") {
    return "zai";
  }
  if (normalized == "opencode-zen") {
    return "opencode";
  }
  if (normalized == "kimi-code") {
    return "kimi-coding";
  }
  if (normalized == "cloudflare-ai") {
    return "cloudflare-ai-gateway";
  }
  return normalized;
}

bool provider_is_known(const std::string &provider) {
  if (common::starts_with(common::to_lower(common::trim(provider)), "custom:")) {
    return true;
  }
  const std::string normalized = normalize_provider_alias(provider);
  const auto providers = known_providers();
  for (const auto &known : providers) {
    if (normalized == known) {
      return true;
    }
  }
  return false;
}

void load_daemon_config(Config &config, const common::TomlDocument &doc) {
  config.daemon.auto_start_schedules =
      doc.get_bool("daemon.auto_start_schedules", config.daemon.auto_start_schedules);

  std::set<std::string> schedule_ids;
  for (const auto &[key, val] : doc.values) {
    if (common::starts_with(key, "daemon.schedules.")) {
      const auto after = key.substr(17); // skip "daemon.schedules."
      const auto dot = after.find('.');
      if (dot != std::string::npos) {
        schedule_ids.insert(after.substr(0, dot));
      }
    }
  }

  for (const auto &id : schedule_ids) {
    ScheduleEntry entry;
    entry.id = id;
    const std::string prefix = "daemon.schedules." + id + ".";
    entry.expression = doc.get_string(prefix + "expression");
    entry.command = doc.get_string(prefix + "command");
    entry.enabled = doc.get_bool(prefix + "enabled", entry.enabled);
    config.daemon.schedules.push_back(std::move(entry));
  }
}

void load_mcp_config(Config &config, const common::TomlDocument &doc) {
  std::set<std::string> server_ids;
  for (const auto &[key, val] : doc.values) {
    if (common::starts_with(key, "mcp.servers.")) {
      const auto after = key.substr(12); // skip "mcp.servers."
      const auto dot = after.find('.');
      if (dot != std::string::npos) {
        server_ids.insert(after.substr(0, dot));
      }
    }
  }

  for (const auto &id : server_ids) {
    McpServerConfig server;
    server.id = id;
    const std::string prefix = "mcp.servers." + id + ".";
    server.command = doc.get_string(prefix + "command");
    server.args = doc.get_string_array(prefix + "args");
    server.enabled = doc.get_bool(prefix + "enabled", server.enabled);

    // Scan for env.* subkeys
    const std::string env_prefix = prefix + "env.";
    for (const auto &[key, val] : doc.values) {
      if (common::starts_with(key, env_prefix)) {
        const auto env_key = key.substr(env_prefix.size());
        server.env[env_key] = doc.get_string(key);
      }
    }

    config.mcp.servers.push_back(std::move(server));
  }
}

void load_conway_config(Config &config, const common::TomlDocument &doc) {
  config.conway.enabled = doc.get_bool("conway.enabled", config.conway.enabled);
  config.conway.api_key =
      expand_config_value(doc.get_string("conway.api_key", config.conway.api_key));
  config.conway.wallet_path =
      expand_config_value(doc.get_string("conway.wallet_path", config.conway.wallet_path));
  config.conway.config_path =
      expand_config_value(doc.get_string("conway.config_path", config.conway.config_path));
  config.conway.api_url = doc.get_string("conway.api_url", config.conway.api_url);
  config.conway.default_region =
      doc.get_string("conway.default_region", config.conway.default_region);
  config.conway.survival_monitoring =
      doc.get_bool("conway.survival_monitoring", config.conway.survival_monitoring);
  config.conway.low_compute_threshold_usd =
      doc.get_double("conway.low_compute_threshold_usd", config.conway.low_compute_threshold_usd);
  config.conway.critical_threshold_usd =
      doc.get_double("conway.critical_threshold_usd", config.conway.critical_threshold_usd);

  // Env var overrides for API key
  if (config.conway.api_key.empty()) {
    if (const char *env = std::getenv("CONWAY_API_KEY"); env != nullptr && *env != '\0') {
      config.conway.api_key = std::string(env);
    }
  }
}

void load_soul_config(Config &config, const common::TomlDocument &doc) {
  config.soul.enabled = doc.get_bool("soul.enabled", config.soul.enabled);
  config.soul.path = doc.get_string("soul.path", config.soul.path);
  config.soul.git_versioned = doc.get_bool("soul.git_versioned", config.soul.git_versioned);
  config.soul.protected_sections =
      doc.get_string_array("soul.protected_sections", config.soul.protected_sections);
  config.soul.max_reflections = static_cast<std::uint32_t>(
      doc.get_u64("soul.max_reflections", config.soul.max_reflections));
}

void load_google_config(Config &config, const common::TomlDocument &doc) {
  config.google.client_id =
      expand_config_value(doc.get_string("google.client_id", config.google.client_id));
  config.google.client_secret =
      expand_config_value(doc.get_string("google.client_secret", config.google.client_secret));
  if (doc.has("google.scopes")) {
    config.google.scopes = doc.get_string_array("google.scopes", config.google.scopes);
  }
  config.google.redirect_port =
      static_cast<std::uint16_t>(doc.get_u64("google.redirect_port", config.google.redirect_port));
}

void load_multi_config(Config &config, const common::TomlDocument &doc) {
  config.multi.default_agent = doc.get_string("multi.default_agent", config.multi.default_agent);
  config.multi.max_internal_messages = static_cast<std::size_t>(
      doc.get_u64("multi.max_internal_messages", config.multi.max_internal_messages));

  // Discover agent IDs by scanning keys starting with "agents."
  std::set<std::string> agent_ids;
  for (const auto &[key, val] : doc.values) {
    if (common::starts_with(key, "agents.")) {
      // key format: "agents.<id>.<field>"
      const auto after = key.substr(7); // skip "agents."
      const auto dot = after.find('.');
      if (dot != std::string::npos) {
        agent_ids.insert(after.substr(0, dot));
      }
    }
  }

  for (const auto &id : agent_ids) {
    AgentConfig agent;
    agent.id = id;
    const std::string prefix = "agents." + id + ".";
    agent.provider = doc.get_string(prefix + "provider");
    agent.model = doc.get_string(prefix + "model");
    agent.temperature = doc.get_double(prefix + "temperature", agent.temperature);
    agent.workspace_directory = doc.get_string(prefix + "workspace_directory");
    agent.system_prompt = doc.get_string(prefix + "system_prompt");
    if (doc.has(prefix + "api_key")) {
      agent.api_key = expand_config_value(doc.get_string(prefix + "api_key"));
    }
    config.multi.agents.push_back(std::move(agent));
  }

  // Discover team IDs by scanning keys starting with "teams."
  std::set<std::string> team_ids;
  for (const auto &[key, val] : doc.values) {
    if (common::starts_with(key, "teams.")) {
      const auto after = key.substr(6); // skip "teams."
      const auto dot = after.find('.');
      if (dot != std::string::npos) {
        team_ids.insert(after.substr(0, dot));
      }
    }
  }

  for (const auto &id : team_ids) {
    TeamConfig team;
    team.id = id;
    const std::string prefix = "teams." + id + ".";
    team.agents = doc.get_string_array(prefix + "agents");
    team.leader_agent = doc.get_string(prefix + "leader_agent");
    team.description = doc.get_string(prefix + "description");
    config.multi.teams.push_back(std::move(team));
  }
}

std::string bool_to_toml(bool value) { return value ? "true" : "false"; }

std::string string_array_to_toml(const std::vector<std::string> &values) {
  std::ostringstream stream;
  stream << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      stream << ", ";
    }
    stream << common::quote_toml_string(values[index]);
  }
  stream << ']';
  return stream.str();
}

} // namespace

common::Result<std::filesystem::path> config_dir() {
  if (const auto override_path = resolved_config_path_override(); override_path.has_value()) {
    std::error_code ec;
    std::filesystem::path candidate = *override_path;
    if (std::filesystem::is_directory(candidate, ec) || candidate.filename().empty()) {
      return common::ensure_dir(candidate);
    }

    auto parent = candidate.parent_path();
    if (parent.empty()) {
      parent = std::filesystem::current_path(ec);
      if (ec) {
        return common::Result<std::filesystem::path>::failure("unable to resolve current directory");
      }
    }
    return common::ensure_dir(parent);
  }

  const auto home = common::home_dir();
  if (!home.ok()) {
    return common::Result<std::filesystem::path>::failure(home.error());
  }

  return common::ensure_dir(home.value() / CONFIG_FOLDER);
}

common::Result<std::filesystem::path> config_path() {
  if (const auto override_path = resolved_config_path_override(); override_path.has_value()) {
    std::error_code ec;
    if (std::filesystem::is_directory(*override_path, ec) || override_path->filename().empty()) {
      return common::Result<std::filesystem::path>::success(*override_path / CONFIG_FILENAME);
    }
    return common::Result<std::filesystem::path>::success(*override_path);
  }

  const auto cfg_dir = config_dir();
  if (!cfg_dir.ok()) {
    return common::Result<std::filesystem::path>::failure(cfg_dir.error());
  }
  return common::Result<std::filesystem::path>::success(cfg_dir.value() / CONFIG_FILENAME);
}

common::Result<std::filesystem::path> workspace_dir() {
  const auto cfg_dir = config_dir();
  if (!cfg_dir.ok()) {
    return common::Result<std::filesystem::path>::failure(cfg_dir.error());
  }
  return common::ensure_dir(cfg_dir.value() / "workspace");
}

bool config_exists() {
  const auto path = config_path();
  return path.ok() && std::filesystem::exists(path.value());
}

void set_config_path_override(std::optional<std::filesystem::path> path) {
  if (!path.has_value()) {
    g_config_path_override = std::nullopt;
    return;
  }
  g_config_path_override = std::filesystem::path(common::expand_path(path->string()));
}

void clear_config_path_override() { g_config_path_override = std::nullopt; }

std::optional<std::filesystem::path> config_path_override() {
  return resolved_config_path_override();
}

std::string expand_config_path(const std::string &path) { return common::expand_path(path); }

void apply_env_overrides(Config &config) {
  load_dotenv_files();

  if (const char *provider = std::getenv("GHOSTCLAW_PROVIDER"); provider != nullptr && *provider) {
    config.default_provider = provider;
  }

  if (const char *model = std::getenv("GHOSTCLAW_MODEL"); model != nullptr && *model) {
    config.default_model = model;
  }

  if (const char *api_key = std::getenv("GHOSTCLAW_API_KEY"); api_key != nullptr && *api_key) {
    config.api_key = std::string(api_key);
    return;
  }

  if (config.api_key.has_value() && !common::trim(*config.api_key).empty()) {
    return;
  }

  const std::string provider = normalize_provider_alias(config.default_provider);
  if ((provider == "xai" || provider == "grok")) {
    if (const char *xai_key = std::getenv("XAI_API_KEY"); xai_key != nullptr && *xai_key) {
      config.api_key = std::string(xai_key);
    }
  }
}

common::Result<Config> load_config() {
  load_dotenv_files();

  Config config;

  const auto cfg_path_result = config_path();
  if (!cfg_path_result.ok()) {
    return common::Result<Config>::failure(cfg_path_result.error());
  }

  const auto path = cfg_path_result.value();
  if (!std::filesystem::exists(path)) {
    apply_env_overrides(config);
    return common::Result<Config>::success(std::move(config));
  }

  auto mapped_file = read_config_file(path);
  if (!mapped_file.ok()) {
    return common::Result<Config>::failure(mapped_file.error());
  }

  const auto parsed = common::parse_toml(mapped_file.value().view());
  if (!parsed.ok()) {
    return common::Result<Config>::failure(parsed.error());
  }

  const auto &doc = parsed.value();

  if (doc.has("default_provider")) {
    config.default_provider =
        expand_config_value(doc.get_string("default_provider", config.default_provider));
  } else if (doc.has("providers.default")) {
    config.default_provider =
        expand_config_value(doc.get_string("providers.default", config.default_provider));
  }
  if (doc.has("default_model")) {
    config.default_model = expand_config_value(doc.get_string("default_model", config.default_model));
  } else if (doc.has("providers.default_model")) {
    config.default_model =
        expand_config_value(doc.get_string("providers.default_model", config.default_model));
  }
  if (doc.has("default_temperature")) {
    config.default_temperature =
        doc.get_double("default_temperature", config.default_temperature);
  } else if (doc.has("providers.default_temperature")) {
    config.default_temperature =
        doc.get_double("providers.default_temperature", config.default_temperature);
  }

  if (doc.has("api_key")) {
    config.api_key = expand_config_value(doc.get_string("api_key"));
  } else {
    const std::string provider_key = normalize_provider_alias(config.default_provider);
    const std::string provider_api_key_path = "providers." + provider_key + ".api_key";
    if (doc.has(provider_api_key_path)) {
      config.api_key = expand_config_value(doc.get_string(provider_api_key_path));
    }
  }

  config.memory.backend = doc.get_string("memory.backend", config.memory.backend);
  config.memory.auto_save = doc.get_bool("memory.auto_save", config.memory.auto_save);

  // Backward compatibility for legacy memory embedding keys.
  if (doc.has("memory.embedding_provider")) {
    config.memory.embedding_provider =
        doc.get_string("memory.embedding_provider", config.memory.embedding_provider);
  } else if (doc.has("memory.embeddings.provider")) {
    config.memory.embedding_provider =
        doc.get_string("memory.embeddings.provider", config.memory.embedding_provider);
  }
  if (doc.has("memory.embedding_model")) {
    config.memory.embedding_model =
        doc.get_string("memory.embedding_model", config.memory.embedding_model);
  } else if (doc.has("memory.embeddings.model")) {
    config.memory.embedding_model =
        doc.get_string("memory.embeddings.model", config.memory.embedding_model);
  }
  if (doc.has("memory.embedding_dimensions")) {
    config.memory.embedding_dimensions = static_cast<std::size_t>(
        doc.get_u64("memory.embedding_dimensions", config.memory.embedding_dimensions));
  } else if (doc.has("memory.embeddings.dimensions")) {
    config.memory.embedding_dimensions = static_cast<std::size_t>(
        doc.get_u64("memory.embeddings.dimensions", config.memory.embedding_dimensions));
  }
  config.memory.embedding_cache_size =
      static_cast<std::size_t>(doc.get_u64("memory.embedding_cache_size", config.memory.embedding_cache_size));
  config.memory.vector_weight = doc.get_double("memory.vector_weight", config.memory.vector_weight);
  config.memory.keyword_weight = doc.get_double("memory.keyword_weight", config.memory.keyword_weight);

  config.gateway.require_pairing = doc.get_bool("gateway.require_pairing", config.gateway.require_pairing);
  config.gateway.paired_tokens = doc.get_string_array("gateway.paired_tokens", config.gateway.paired_tokens);
  config.gateway.allow_public_bind =
      doc.get_bool("gateway.allow_public_bind", config.gateway.allow_public_bind);
  config.gateway.port = static_cast<std::uint16_t>(doc.get_int("gateway.port", config.gateway.port));
  config.gateway.host = doc.get_string("gateway.host", config.gateway.host);
  config.gateway.websocket_enabled =
      doc.get_bool("gateway.websocket_enabled", config.gateway.websocket_enabled);
  config.gateway.websocket_port =
      static_cast<std::uint16_t>(doc.get_int("gateway.websocket_port", config.gateway.websocket_port));
  config.gateway.websocket_host = doc.get_string("gateway.websocket_host", config.gateway.websocket_host);
  config.gateway.websocket_tls_enabled =
      doc.get_bool("gateway.websocket_tls_enabled", config.gateway.websocket_tls_enabled);
  config.gateway.websocket_tls_cert_file =
      doc.get_string("gateway.websocket_tls_cert_file", config.gateway.websocket_tls_cert_file);
  config.gateway.websocket_tls_key_file =
      doc.get_string("gateway.websocket_tls_key_file", config.gateway.websocket_tls_key_file);
  config.gateway.session_send_policy_enabled = doc.get_bool(
      "gateway.session_send_policy_enabled", config.gateway.session_send_policy_enabled);
  config.gateway.session_send_policy_max_per_window = static_cast<std::uint32_t>(doc.get_u64(
      "gateway.session_send_policy_max_per_window",
      config.gateway.session_send_policy_max_per_window));
  config.gateway.session_send_policy_window_seconds = static_cast<std::uint32_t>(doc.get_u64(
      "gateway.session_send_policy_window_seconds",
      config.gateway.session_send_policy_window_seconds));
  if (!config.gateway.websocket_tls_cert_file.empty()) {
    config.gateway.websocket_tls_cert_file =
        expand_config_path(config.gateway.websocket_tls_cert_file);
  }
  if (!config.gateway.websocket_tls_key_file.empty()) {
    config.gateway.websocket_tls_key_file =
        expand_config_path(config.gateway.websocket_tls_key_file);
  }

  config.autonomy.level = doc.get_string("autonomy.level", config.autonomy.level);
  config.autonomy.workspace_only = doc.get_bool("autonomy.workspace_only", config.autonomy.workspace_only);
  config.autonomy.allowed_commands =
      doc.get_string_array("autonomy.allowed_commands", config.autonomy.allowed_commands);
  config.autonomy.forbidden_paths =
      doc.get_string_array("autonomy.forbidden_paths", config.autonomy.forbidden_paths);
  config.autonomy.max_actions_per_hour =
      static_cast<std::uint32_t>(doc.get_u64("autonomy.max_actions_per_hour", config.autonomy.max_actions_per_hour));
  config.autonomy.max_cost_per_day_cents =
      static_cast<std::uint32_t>(doc.get_u64("autonomy.max_cost_per_day_cents", config.autonomy.max_cost_per_day_cents));

  load_channel_config(config, doc);
  load_tunnel_config(config, doc);
  load_multi_config(config, doc);
  load_daemon_config(config, doc);
  load_mcp_config(config, doc);
  load_google_config(config, doc);
  load_conway_config(config, doc);
  load_soul_config(config, doc);

  config.observability.backend = doc.get_string("observability.backend", config.observability.backend);
  config.runtime.kind = doc.get_string("runtime.kind", config.runtime.kind);

  config.reliability.provider_retries =
      static_cast<std::uint32_t>(doc.get_u64("reliability.provider_retries", config.reliability.provider_retries));
  config.reliability.provider_backoff_ms =
      doc.get_u64("reliability.provider_backoff_ms", config.reliability.provider_backoff_ms);
  config.reliability.fallback_providers =
      doc.get_string_array("reliability.fallback_providers", config.reliability.fallback_providers);
  config.reliability.channel_initial_backoff_secs =
      doc.get_u64("reliability.channel_initial_backoff_secs", config.reliability.channel_initial_backoff_secs);
  config.reliability.channel_max_backoff_secs =
      doc.get_u64("reliability.channel_max_backoff_secs", config.reliability.channel_max_backoff_secs);
  config.reliability.scheduler_poll_secs =
      doc.get_u64("reliability.scheduler_poll_secs", config.reliability.scheduler_poll_secs);
  config.reliability.scheduler_retries =
      static_cast<std::uint32_t>(doc.get_u64("reliability.scheduler_retries", config.reliability.scheduler_retries));

  config.heartbeat.enabled = doc.get_bool("heartbeat.enabled", config.heartbeat.enabled);
  config.heartbeat.interval_minutes =
      doc.get_u64("heartbeat.interval_minutes", config.heartbeat.interval_minutes);
  config.heartbeat.tasks_file = doc.get_string("heartbeat.tasks_file", config.heartbeat.tasks_file);

  config.browser.enabled = doc.get_bool("browser.enabled", config.browser.enabled);
  config.browser.allowed_domains =
      doc.get_string_array("browser.allowed_domains", config.browser.allowed_domains);
  config.browser.session_name = doc.get_string("browser.session_name", config.browser.session_name);

  config.tools.profile = doc.get_string("tools.profile", config.tools.profile);
  config.tools.allow.groups =
      doc.get_string_array("tools.allow.groups", config.tools.allow.groups);
  config.tools.allow.tools = doc.get_string_array("tools.allow.tools", config.tools.allow.tools);
  config.tools.allow.deny = doc.get_string_array("tools.allow.deny", config.tools.allow.deny);

  config.calendar.backend = doc.get_string("calendar.backend", config.calendar.backend);
  config.calendar.default_calendar =
      doc.get_string("calendar.default_calendar", config.calendar.default_calendar);

  config.email.backend = doc.get_string("email.backend", config.email.backend);
  config.email.default_account = doc.get_string("email.default_account", config.email.default_account);
  if (doc.has("email.smtp.host") || doc.has("email.smtp.port") || doc.has("email.smtp.username") ||
      doc.has("email.smtp.password") || doc.has("email.smtp.tls")) {
    EmailSmtpConfig smtp;
    smtp.host = doc.get_string("email.smtp.host", smtp.host);
    smtp.port = static_cast<std::uint16_t>(doc.get_u64("email.smtp.port", smtp.port));
    smtp.username = doc.get_string("email.smtp.username", smtp.username);
    smtp.password = doc.get_string("email.smtp.password", smtp.password);
    smtp.tls = doc.get_bool("email.smtp.tls", smtp.tls);
    config.email.smtp = std::move(smtp);
  }

  config.reminders.default_channel =
      doc.get_string("reminders.default_channel", config.reminders.default_channel);

  config.web_search.provider = doc.get_string("web_search.provider", config.web_search.provider);
  if (doc.has("web_search.brave_api_key")) {
    config.web_search.brave_api_key = doc.get_string("web_search.brave_api_key");
  }

  config.composio.enabled = doc.get_bool("composio.enabled", config.composio.enabled);
  if (doc.has("composio.api_key")) {
    config.composio.api_key = expand_config_value(doc.get_string("composio.api_key"));
  }

  config.identity.format = doc.get_string("identity.format", config.identity.format);
  if (doc.has("identity.aieos_path")) {
    config.identity.aieos_path = expand_config_path(doc.get_string("identity.aieos_path"));
  }
  if (doc.has("identity.aieos_inline")) {
    config.identity.aieos_inline = doc.get_string("identity.aieos_inline");
  }

  config.secrets.encrypt = doc.get_bool("secrets.encrypt", config.secrets.encrypt);

  for (auto &path_entry : config.autonomy.forbidden_paths) {
    path_entry = expand_config_path(path_entry);
  }
  if (config.tunnel.cloudflare.has_value()) {
    config.tunnel.cloudflare->command_path = expand_config_path(config.tunnel.cloudflare->command_path);
  }

  // Auto-inject Conway Terminal as MCP server when conway is enabled and has an API key
  if (config.conway.enabled && !config.conway.api_key.empty()) {
    bool conway_mcp_exists = false;
    for (const auto &server : config.mcp.servers) {
      if (server.id == "conway") {
        conway_mcp_exists = true;
        break;
      }
    }
    if (!conway_mcp_exists) {
      McpServerConfig conway_mcp;
      conway_mcp.id = "conway";
      conway_mcp.command = "npx";
      conway_mcp.args = {"conway-terminal"};
      conway_mcp.env["CONWAY_API_KEY"] = config.conway.api_key;
      if (config.conway.api_url != "https://api.conway.tech") {
        conway_mcp.env["CONWAY_API_URL"] = config.conway.api_url;
      }
      conway_mcp.enabled = true;
      config.mcp.servers.push_back(std::move(conway_mcp));
    }
  }

  apply_env_overrides(config);
  return common::Result<Config>::success(std::move(config));
}

common::Status save_config(const Config &config) {
  const auto cfg_path_result = config_path();
  if (!cfg_path_result.ok()) {
    return common::Status::error(cfg_path_result.error());
  }

  const std::filesystem::path path = cfg_path_result.value();
  if (!path.parent_path().empty()) {
    std::error_code ensure_ec;
    std::filesystem::create_directories(path.parent_path(), ensure_ec);
    if (ensure_ec) {
      return common::Status::error("Failed to create config directory: " + ensure_ec.message());
    }
  }
  const std::filesystem::path tmp_path = path.string() + ".tmp";

  std::ofstream file(tmp_path, std::ios::trunc);
  if (!file) {
    return common::Status::error("Unable to write temporary config file");
  }

  file << "default_provider = " << common::quote_toml_string(config.default_provider) << "\n";
  file << "default_model = " << common::quote_toml_string(config.default_model) << "\n";
  file << "default_temperature = " << config.default_temperature << "\n";
  if (config.api_key.has_value()) {
    file << "api_key = " << common::quote_toml_string(*config.api_key) << "\n";
  }

  file << "\n[memory]\n";
  file << "backend = " << common::quote_toml_string(config.memory.backend) << "\n";
  file << "auto_save = " << bool_to_toml(config.memory.auto_save) << "\n";
  file << "embedding_provider = " << common::quote_toml_string(config.memory.embedding_provider)
       << "\n";
  file << "embedding_model = " << common::quote_toml_string(config.memory.embedding_model) << "\n";
  file << "embedding_dimensions = " << config.memory.embedding_dimensions << "\n";
  file << "embedding_cache_size = " << config.memory.embedding_cache_size << "\n";
  file << "vector_weight = " << config.memory.vector_weight << "\n";
  file << "keyword_weight = " << config.memory.keyword_weight << "\n";

  file << "\n[gateway]\n";
  file << "require_pairing = " << bool_to_toml(config.gateway.require_pairing) << "\n";
  file << "paired_tokens = " << string_array_to_toml(config.gateway.paired_tokens) << "\n";
  file << "allow_public_bind = " << bool_to_toml(config.gateway.allow_public_bind) << "\n";
  file << "port = " << config.gateway.port << "\n";
  file << "host = " << common::quote_toml_string(config.gateway.host) << "\n";
  file << "websocket_enabled = " << bool_to_toml(config.gateway.websocket_enabled) << "\n";
  file << "websocket_port = " << config.gateway.websocket_port << "\n";
  file << "websocket_host = " << common::quote_toml_string(config.gateway.websocket_host) << "\n";
  file << "websocket_tls_enabled = " << bool_to_toml(config.gateway.websocket_tls_enabled) << "\n";
  file << "websocket_tls_cert_file = "
       << common::quote_toml_string(config.gateway.websocket_tls_cert_file) << "\n";
  file << "websocket_tls_key_file = "
       << common::quote_toml_string(config.gateway.websocket_tls_key_file) << "\n";
  file << "session_send_policy_enabled = "
       << bool_to_toml(config.gateway.session_send_policy_enabled) << "\n";
  file << "session_send_policy_max_per_window = "
       << config.gateway.session_send_policy_max_per_window << "\n";
  file << "session_send_policy_window_seconds = "
       << config.gateway.session_send_policy_window_seconds << "\n";

  file << "\n[autonomy]\n";
  file << "level = " << common::quote_toml_string(config.autonomy.level) << "\n";
  file << "workspace_only = " << bool_to_toml(config.autonomy.workspace_only) << "\n";
  file << "allowed_commands = " << string_array_to_toml(config.autonomy.allowed_commands) << "\n";
  file << "forbidden_paths = " << string_array_to_toml(config.autonomy.forbidden_paths) << "\n";
  file << "max_actions_per_hour = " << config.autonomy.max_actions_per_hour << "\n";
  file << "max_cost_per_day_cents = " << config.autonomy.max_cost_per_day_cents << "\n";

  file << "\n[tunnel]\n";
  file << "provider = " << common::quote_toml_string(config.tunnel.provider) << "\n";
  if (config.tunnel.cloudflare.has_value()) {
    file << "\n[tunnel.cloudflare]\n";
    file << "command_path = "
         << common::quote_toml_string(config.tunnel.cloudflare->command_path) << "\n";
  }
  if (config.tunnel.ngrok.has_value()) {
    file << "\n[tunnel.ngrok]\n";
    file << "auth_token = " << common::quote_toml_string(config.tunnel.ngrok->auth_token)
         << "\n";
  }
  if (config.tunnel.tailscale.has_value()) {
    file << "\n[tunnel.tailscale]\n";
    file << "hostname = " << common::quote_toml_string(config.tunnel.tailscale->hostname)
         << "\n";
  }
  if (config.tunnel.custom.has_value()) {
    file << "\n[tunnel.custom]\n";
    file << "command = " << common::quote_toml_string(config.tunnel.custom->command) << "\n";
    file << "args = " << string_array_to_toml(config.tunnel.custom->args) << "\n";
  }

  if (config.channels.telegram.has_value()) {
    file << "\n[channels.telegram]\n";
    file << "bot_token = " << common::quote_toml_string(config.channels.telegram->bot_token)
         << "\n";
    file << "allowed_users = "
         << string_array_to_toml(config.channels.telegram->allowed_users) << "\n";
  }
  if (config.channels.discord.has_value()) {
    file << "\n[channels.discord]\n";
    file << "bot_token = " << common::quote_toml_string(config.channels.discord->bot_token)
         << "\n";
    file << "guild_id = " << common::quote_toml_string(config.channels.discord->guild_id)
         << "\n";
    file << "allowed_users = "
         << string_array_to_toml(config.channels.discord->allowed_users) << "\n";
  }
  if (config.channels.slack.has_value()) {
    file << "\n[channels.slack]\n";
    file << "bot_token = " << common::quote_toml_string(config.channels.slack->bot_token) << "\n";
    file << "channel_id = " << common::quote_toml_string(config.channels.slack->channel_id)
         << "\n";
    file << "allowed_users = "
         << string_array_to_toml(config.channels.slack->allowed_users) << "\n";
  }
  if (config.channels.matrix.has_value()) {
    file << "\n[channels.matrix]\n";
    file << "homeserver = " << common::quote_toml_string(config.channels.matrix->homeserver)
         << "\n";
    file << "access_token = "
         << common::quote_toml_string(config.channels.matrix->access_token) << "\n";
    file << "room_id = " << common::quote_toml_string(config.channels.matrix->room_id) << "\n";
  }
  if (config.channels.imessage.has_value()) {
    file << "\n[channels.imessage]\n";
    file << "allowed_contacts = "
         << string_array_to_toml(config.channels.imessage->allowed_contacts) << "\n";
  }
  if (config.channels.whatsapp.has_value()) {
    file << "\n[channels.whatsapp]\n";
    file << "access_token = "
         << common::quote_toml_string(config.channels.whatsapp->access_token) << "\n";
    file << "phone_number_id = "
         << common::quote_toml_string(config.channels.whatsapp->phone_number_id) << "\n";
    file << "verify_token = "
         << common::quote_toml_string(config.channels.whatsapp->verify_token) << "\n";
    file << "allowed_numbers = "
         << string_array_to_toml(config.channels.whatsapp->allowed_numbers) << "\n";
  }
  if (config.channels.webhook.has_value()) {
    file << "\n[channels.webhook]\n";
    file << "secret = " << common::quote_toml_string(config.channels.webhook->secret) << "\n";
  }

  file << "\n[observability]\n";
  file << "backend = " << common::quote_toml_string(config.observability.backend) << "\n";

  file << "\n[runtime]\n";
  file << "kind = " << common::quote_toml_string(config.runtime.kind) << "\n";

  file << "\n[tools]\n";
  file << "profile = " << common::quote_toml_string(config.tools.profile) << "\n";
  file << "\n[tools.allow]\n";
  file << "groups = " << string_array_to_toml(config.tools.allow.groups) << "\n";
  file << "tools = " << string_array_to_toml(config.tools.allow.tools) << "\n";
  file << "deny = " << string_array_to_toml(config.tools.allow.deny) << "\n";

  file << "\n[calendar]\n";
  file << "backend = " << common::quote_toml_string(config.calendar.backend) << "\n";
  file << "default_calendar = "
       << common::quote_toml_string(config.calendar.default_calendar) << "\n";

  file << "\n[email]\n";
  file << "backend = " << common::quote_toml_string(config.email.backend) << "\n";
  file << "default_account = " << common::quote_toml_string(config.email.default_account) << "\n";
  if (config.email.smtp.has_value()) {
    file << "\n[email.smtp]\n";
    file << "host = " << common::quote_toml_string(config.email.smtp->host) << "\n";
    file << "port = " << config.email.smtp->port << "\n";
    file << "username = " << common::quote_toml_string(config.email.smtp->username) << "\n";
    file << "password = " << common::quote_toml_string(config.email.smtp->password) << "\n";
    file << "tls = " << bool_to_toml(config.email.smtp->tls) << "\n";
  }

  file << "\n[reminders]\n";
  file << "default_channel = "
       << common::quote_toml_string(config.reminders.default_channel) << "\n";

  if (config.web_search.provider != "auto" || config.web_search.brave_api_key.has_value()) {
    file << "\n[web_search]\n";
    file << "provider = " << common::quote_toml_string(config.web_search.provider) << "\n";
    if (config.web_search.brave_api_key.has_value()) {
      file << "brave_api_key = "
           << common::quote_toml_string(*config.web_search.brave_api_key) << "\n";
    }
  }

  file << "\n[secrets]\n";
  file << "encrypt = " << bool_to_toml(config.secrets.encrypt) << "\n";

  if (!config.multi.agents.empty() || !config.multi.teams.empty() ||
      config.multi.default_agent != "ghostclaw" || config.multi.max_internal_messages != 50) {
    file << "\n[multi]\n";
    file << "default_agent = " << common::quote_toml_string(config.multi.default_agent) << "\n";
    file << "max_internal_messages = " << config.multi.max_internal_messages << "\n";

    for (const auto &agent : config.multi.agents) {
      file << "\n[agents." << agent.id << "]\n";
      if (!agent.provider.empty()) {
        file << "provider = " << common::quote_toml_string(agent.provider) << "\n";
      }
      if (!agent.model.empty()) {
        file << "model = " << common::quote_toml_string(agent.model) << "\n";
      }
      file << "temperature = " << agent.temperature << "\n";
      if (!agent.workspace_directory.empty()) {
        file << "workspace_directory = " << common::quote_toml_string(agent.workspace_directory) << "\n";
      }
      if (!agent.system_prompt.empty()) {
        file << "system_prompt = " << common::quote_toml_string(agent.system_prompt) << "\n";
      }
      if (agent.api_key.has_value()) {
        file << "api_key = " << common::quote_toml_string(*agent.api_key) << "\n";
      }
    }

    for (const auto &team : config.multi.teams) {
      file << "\n[teams." << team.id << "]\n";
      file << "agents = " << string_array_to_toml(team.agents) << "\n";
      if (!team.leader_agent.empty()) {
        file << "leader_agent = " << common::quote_toml_string(team.leader_agent) << "\n";
      }
      if (!team.description.empty()) {
        file << "description = " << common::quote_toml_string(team.description) << "\n";
      }
    }
  }

  // Daemon schedules
  if (!config.daemon.schedules.empty() || !config.daemon.auto_start_schedules) {
    file << "\n[daemon]\n";
    file << "auto_start_schedules = " << bool_to_toml(config.daemon.auto_start_schedules) << "\n";
    for (const auto &entry : config.daemon.schedules) {
      file << "\n[daemon.schedules." << entry.id << "]\n";
      file << "expression = " << common::quote_toml_string(entry.expression) << "\n";
      file << "command = " << common::quote_toml_string(entry.command) << "\n";
      file << "enabled = " << bool_to_toml(entry.enabled) << "\n";
    }
  }

  // MCP servers
  if (!config.mcp.servers.empty()) {
    for (const auto &server : config.mcp.servers) {
      file << "\n[mcp.servers." << server.id << "]\n";
      file << "command = " << common::quote_toml_string(server.command) << "\n";
      file << "args = " << string_array_to_toml(server.args) << "\n";
      file << "enabled = " << bool_to_toml(server.enabled) << "\n";
      if (!server.env.empty()) {
        for (const auto &[key, val] : server.env) {
          file << "env." << key << " = " << common::quote_toml_string(val) << "\n";
        }
      }
    }
  }

  // Google config
  if (!config.google.client_id.empty()) {
    file << "\n[google]\n";
    file << "client_id = " << common::quote_toml_string(config.google.client_id) << "\n";
    file << "client_secret = " << common::quote_toml_string(config.google.client_secret) << "\n";
    file << "scopes = " << string_array_to_toml(config.google.scopes) << "\n";
    file << "redirect_port = " << config.google.redirect_port << "\n";
  }

  // Conway config
  if (config.conway.enabled || !config.conway.api_key.empty()) {
    file << "\n[conway]\n";
    file << "enabled = " << bool_to_toml(config.conway.enabled) << "\n";
    if (!config.conway.api_key.empty()) {
      file << "api_key = " << common::quote_toml_string(config.conway.api_key) << "\n";
    }
    file << "wallet_path = " << common::quote_toml_string(config.conway.wallet_path) << "\n";
    file << "config_path = " << common::quote_toml_string(config.conway.config_path) << "\n";
    file << "api_url = " << common::quote_toml_string(config.conway.api_url) << "\n";
    file << "default_region = " << common::quote_toml_string(config.conway.default_region) << "\n";
    file << "survival_monitoring = " << bool_to_toml(config.conway.survival_monitoring) << "\n";
    file << "low_compute_threshold_usd = " << config.conway.low_compute_threshold_usd << "\n";
    file << "critical_threshold_usd = " << config.conway.critical_threshold_usd << "\n";
  }

  // Soul config
  if (config.soul.enabled) {
    file << "\n[soul]\n";
    file << "enabled = " << bool_to_toml(config.soul.enabled) << "\n";
    file << "path = " << common::quote_toml_string(config.soul.path) << "\n";
    file << "git_versioned = " << bool_to_toml(config.soul.git_versioned) << "\n";
    if (!config.soul.protected_sections.empty()) {
      file << "protected_sections = " << string_array_to_toml(config.soul.protected_sections) << "\n";
    }
    file << "max_reflections = " << config.soul.max_reflections << "\n";
  }

  file.close();
  if (!file) {
    return common::Status::error("Failed writing temporary config file");
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    return common::Status::error("Failed to atomically replace config: " + ec.message());
  }

  return common::Status::success();
}

common::Result<std::vector<std::string>> validate_config(const Config &config) {
  std::vector<std::string> warnings;

  if (!provider_is_known(config.default_provider)) {
    return common::Result<std::vector<std::string>>::failure("Unknown default provider: " +
                                                              config.default_provider);
  }

  if (config.default_temperature < 0.0 || config.default_temperature > 2.0) {
    return common::Result<std::vector<std::string>>::failure(
        "default_temperature must be between 0.0 and 2.0");
  }

  const std::string memory_backend = common::to_lower(config.memory.backend);
  if (memory_backend != "sqlite" && memory_backend != "markdown" && memory_backend != "none") {
    return common::Result<std::vector<std::string>>::failure("Invalid memory.backend: " +
                                                              config.memory.backend);
  }

  const double weight_sum = config.memory.vector_weight + config.memory.keyword_weight;
  if (std::abs(weight_sum - 1.0) > 0.001) {
    warnings.push_back("memory.vector_weight + memory.keyword_weight should equal 1.0");
  }

  const std::string autonomy = common::to_lower(config.autonomy.level);
  if (autonomy != "readonly" && autonomy != "supervised" && autonomy != "full") {
    return common::Result<std::vector<std::string>>::failure("Invalid autonomy.level: " +
                                                              config.autonomy.level);
  }

  if (common::to_lower(config.runtime.kind) != "native") {
    return common::Result<std::vector<std::string>>::failure("Unsupported runtime.kind: " +
                                                              config.runtime.kind);
  }

  const std::string tool_profile = common::to_lower(common::trim(config.tools.profile));
  if (!tool_profile.empty() && tool_profile != "minimal" && tool_profile != "coding" &&
      tool_profile != "messaging" && tool_profile != "full") {
    return common::Result<std::vector<std::string>>::failure("Invalid tools.profile: " +
                                                              config.tools.profile);
  }

  if (config.email.smtp.has_value() && config.email.smtp->port == 0) {
    return common::Result<std::vector<std::string>>::failure(
        "email.smtp.port must be 1-65535");
  }

  const std::string tunnel_provider = common::to_lower(config.tunnel.provider);
  if (tunnel_provider != "none" && tunnel_provider != "cloudflare" && tunnel_provider != "ngrok" &&
      tunnel_provider != "tailscale" && tunnel_provider != "custom") {
    return common::Result<std::vector<std::string>>::failure("Invalid tunnel.provider: " +
                                                              config.tunnel.provider);
  }

  if (config.gateway.port == 0) {
    return common::Result<std::vector<std::string>>::failure("gateway.port must be 1-65535");
  }

  if (!is_valid_host(config.gateway.host)) {
    return common::Result<std::vector<std::string>>::failure("gateway.host is invalid: " +
                                                              config.gateway.host);
  }
  if (!is_valid_host(config.gateway.websocket_host)) {
    return common::Result<std::vector<std::string>>::failure("gateway.websocket_host is invalid: " +
                                                              config.gateway.websocket_host);
  }
  if (config.gateway.websocket_enabled && config.gateway.websocket_port == 0 &&
      config.gateway.port == 65535) {
    return common::Result<std::vector<std::string>>::failure(
        "gateway.websocket_port must be set when gateway.port is 65535");
  }
  if (config.gateway.websocket_tls_enabled) {
    if (!config.gateway.websocket_enabled) {
      warnings.push_back("gateway.websocket_tls_enabled is set while websocket is disabled");
    }
    if (config.gateway.websocket_tls_cert_file.empty() ||
        config.gateway.websocket_tls_key_file.empty()) {
      return common::Result<std::vector<std::string>>::failure(
          "gateway websocket TLS requires websocket_tls_cert_file and websocket_tls_key_file");
    }
    if (!std::filesystem::exists(config.gateway.websocket_tls_cert_file)) {
      return common::Result<std::vector<std::string>>::failure(
          "gateway.websocket_tls_cert_file does not exist: " +
          config.gateway.websocket_tls_cert_file);
    }
    if (!std::filesystem::exists(config.gateway.websocket_tls_key_file)) {
      return common::Result<std::vector<std::string>>::failure(
          "gateway.websocket_tls_key_file does not exist: " +
          config.gateway.websocket_tls_key_file);
    }
  }
  if (config.gateway.session_send_policy_enabled &&
      config.gateway.session_send_policy_max_per_window == 0) {
    return common::Result<std::vector<std::string>>::failure(
        "gateway.session_send_policy_max_per_window must be > 0");
  }
  if (config.gateway.session_send_policy_enabled &&
      config.gateway.session_send_policy_window_seconds == 0) {
    return common::Result<std::vector<std::string>>::failure(
        "gateway.session_send_policy_window_seconds must be > 0");
  }

  if (config.gateway.allow_public_bind && tunnel_provider == "none") {
    warnings.push_back("gateway.allow_public_bind is true without tunnel provider configured");
  }

  bool api_key_missing = !config.api_key.has_value() || common::trim(*config.api_key).empty();
  if (api_key_missing && std::getenv("GHOSTCLAW_API_KEY") != nullptr) {
    api_key_missing = false;
  }
  if (api_key_missing) {
    const std::string provider = normalize_provider_alias(config.default_provider);
    if ((provider == "xai" || provider == "grok") && std::getenv("XAI_API_KEY") != nullptr) {
      api_key_missing = false;
    }
  }
  if (api_key_missing) {
    const std::string provider = normalize_provider_alias(config.default_provider);
    if ((provider == "openai" || provider == "openai-codex") && auth::has_valid_tokens()) {
      api_key_missing = false;
    }
  }
  if (api_key_missing) {
    warnings.push_back("API key is missing (config.api_key, GHOSTCLAW_API_KEY, or provider key env)");
  }

  if (config.channels.telegram.has_value() && config.channels.telegram->bot_token.empty()) {
    return common::Result<std::vector<std::string>>::failure(
        "channels.telegram.bot_token is required when telegram is configured");
  }
  if (config.channels.discord.has_value() && config.channels.discord->bot_token.empty()) {
    return common::Result<std::vector<std::string>>::failure(
        "channels.discord.bot_token is required when discord is configured");
  }
  if (config.channels.slack.has_value() && config.channels.slack->bot_token.empty()) {
    return common::Result<std::vector<std::string>>::failure(
        "channels.slack.bot_token is required when slack is configured");
  }
  if (config.channels.matrix.has_value()) {
    const auto &m = *config.channels.matrix;
    if (m.homeserver.empty() || m.access_token.empty() || m.room_id.empty()) {
      return common::Result<std::vector<std::string>>::failure(
          "channels.matrix requires homeserver, access_token, and room_id");
    }
  }
  if (config.channels.whatsapp.has_value()) {
    const auto &w = *config.channels.whatsapp;
    if (w.access_token.empty() || w.phone_number_id.empty() || w.verify_token.empty()) {
      return common::Result<std::vector<std::string>>::failure(
          "channels.whatsapp requires access_token, phone_number_id, and verify_token");
    }
  }
  if (config.channels.webhook.has_value() && config.channels.webhook->secret.empty()) {
    return common::Result<std::vector<std::string>>::failure(
        "channels.webhook.secret is required when webhook is configured");
  }

  // Multi-agent validation
  std::set<std::string> known_agent_ids;
  for (const auto &agent : config.multi.agents) {
    if (agent.id.empty()) {
      return common::Result<std::vector<std::string>>::failure(
          "agent config has empty id");
    }
    if (known_agent_ids.find(agent.id) != known_agent_ids.end()) {
      return common::Result<std::vector<std::string>>::failure(
          "duplicate agent id: '" + agent.id + "'");
    }
    known_agent_ids.insert(agent.id);

    if (agent.temperature < 0.0 || agent.temperature > 2.0) {
      return common::Result<std::vector<std::string>>::failure(
          "agent '" + agent.id + "' temperature must be between 0.0 and 2.0");
    }
  }

  if (config.multi.max_internal_messages == 0 && !config.multi.agents.empty()) {
    return common::Result<std::vector<std::string>>::failure(
        "multi.max_internal_messages must be > 0");
  }

  std::set<std::string> known_team_ids;
  for (const auto &team : config.multi.teams) {
    if (team.id.empty()) {
      return common::Result<std::vector<std::string>>::failure(
          "team config has empty id");
    }
    if (known_team_ids.find(team.id) != known_team_ids.end()) {
      return common::Result<std::vector<std::string>>::failure(
          "duplicate team id: '" + team.id + "'");
    }
    known_team_ids.insert(team.id);

    if (team.agents.empty()) {
      return common::Result<std::vector<std::string>>::failure(
          "team '" + team.id + "' has no agents");
    }
    for (const auto &member : team.agents) {
      if (known_agent_ids.find(member) == known_agent_ids.end()) {
        return common::Result<std::vector<std::string>>::failure(
            "team '" + team.id + "' references unknown agent '" + member + "'");
      }
    }
    if (!team.leader_agent.empty()) {
      bool leader_in_team = false;
      for (const auto &member : team.agents) {
        if (member == team.leader_agent) {
          leader_in_team = true;
          break;
        }
      }
      if (!leader_in_team) {
        return common::Result<std::vector<std::string>>::failure(
            "team '" + team.id + "' leader_agent '" + team.leader_agent +
            "' is not in the team's agent list");
      }
    } else if (!team.agents.empty()) {
      warnings.push_back("team '" + team.id + "' has no leader_agent set, "
                          "first agent will be used as leader");
    }
  }

  // Warn if agent/team IDs collide (ambiguous routing)
  for (const auto &agent_id : known_agent_ids) {
    if (known_team_ids.find(agent_id) != known_team_ids.end()) {
      warnings.push_back("agent '" + agent_id +
                          "' and team '" + agent_id + "' share the same id, "
                          "team will take routing priority");
    }
  }

  // Daemon schedule validation
  for (const auto &entry : config.daemon.schedules) {
    if (entry.expression.empty()) {
      warnings.push_back("daemon schedule '" + entry.id + "' has empty expression");
    }
    if (entry.command.empty()) {
      warnings.push_back("daemon schedule '" + entry.id + "' has empty command");
    }
  }

  // MCP server validation
  for (const auto &server : config.mcp.servers) {
    if (server.enabled && server.command.empty()) {
      warnings.push_back("mcp server '" + server.id + "' has empty command");
    }
  }

  // Google config validation
  const std::string email_backend = common::to_lower(common::trim(config.email.backend));
  const std::string cal_backend = common::to_lower(common::trim(config.calendar.backend));
  if ((email_backend == "gmail" || cal_backend == "google") && config.google.client_id.empty()) {
    warnings.push_back("google.client_id is required when email.backend='gmail' or calendar.backend='google'");
  }

  if (!config.multi.default_agent.empty() && !config.multi.agents.empty()) {
    if (known_agent_ids.find(config.multi.default_agent) == known_agent_ids.end()) {
      warnings.push_back("multi.default_agent '" + config.multi.default_agent +
                          "' does not match any configured agent");
    }
  }

  return common::Result<std::vector<std::string>>::success(std::move(warnings));
}

} // namespace ghostclaw::config
