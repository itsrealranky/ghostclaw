#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ghostclaw::config {

struct MemoryConfig {
  std::string backend = "sqlite";
  bool auto_save = true;
  std::string embedding_provider = "openai";
  std::string embedding_model = "text-embedding-3-small";
  std::size_t embedding_dimensions = 1536;
  std::size_t embedding_cache_size = 10'000;
  double vector_weight = 0.7;
  double keyword_weight = 0.3;
};

struct GatewayConfig {
  bool require_pairing = true;
  std::vector<std::string> paired_tokens;
  bool allow_public_bind = false;
  std::uint16_t port = 8080;
  std::string host = "127.0.0.1";
  bool websocket_enabled = false;
  std::uint16_t websocket_port = 0;
  std::string websocket_host = "127.0.0.1";
  bool websocket_tls_enabled = false;
  std::string websocket_tls_cert_file;
  std::string websocket_tls_key_file;
  bool session_send_policy_enabled = true;
  std::uint32_t session_send_policy_max_per_window = 60;
  std::uint32_t session_send_policy_window_seconds = 60;
};

struct AutonomyConfig {
  std::string level = "supervised";
  bool workspace_only = true;
  std::vector<std::string> allowed_commands = {
      "git", "npm", "cargo", "ls",   "cat", "grep",
      "find", "echo", "python", "node", "make"};
  std::vector<std::string> forbidden_paths = {"/etc",   "/root", "/proc",  "/sys",
                                              "~/.ssh", "~/.gnupg", "~/.aws"};
  std::uint32_t max_actions_per_hour = 100;
  std::uint32_t max_cost_per_day_cents = 1000;
};

struct TelegramConfig {
  std::string bot_token;
  std::vector<std::string> allowed_users;
};

struct DiscordConfig {
  std::string bot_token;
  std::string guild_id;
  std::vector<std::string> allowed_users;
};

struct SlackConfig {
  std::string bot_token;
  std::string channel_id;
  std::vector<std::string> allowed_users;
};

struct MatrixConfig {
  std::string homeserver;
  std::string access_token;
  std::string room_id;
};

struct IMessageConfig {
  std::vector<std::string> allowed_contacts;
};

struct WhatsAppConfig {
  std::string access_token;
  std::string phone_number_id;
  std::string verify_token;
  std::vector<std::string> allowed_numbers;
};

struct WebhookConfig {
  std::string secret;
};

struct ChannelsConfig {
  std::optional<TelegramConfig> telegram;
  std::optional<DiscordConfig> discord;
  std::optional<SlackConfig> slack;
  std::optional<MatrixConfig> matrix;
  std::optional<IMessageConfig> imessage;
  std::optional<WhatsAppConfig> whatsapp;
  std::optional<WebhookConfig> webhook;
};

struct CloudflareConfig {
  std::string command_path;
};

struct NgrokConfig {
  std::string auth_token;
};

struct TailscaleConfig {
  std::string hostname;
};

struct CustomTunnelConfig {
  std::string command;
  std::vector<std::string> args;
};

struct TunnelConfig {
  std::string provider = "none";
  std::optional<CloudflareConfig> cloudflare;
  std::optional<NgrokConfig> ngrok;
  std::optional<TailscaleConfig> tailscale;
  std::optional<CustomTunnelConfig> custom;
};

struct ObservabilityConfig {
  std::string backend = "log";
};

struct RuntimeConfig {
  std::string kind = "native";
};

struct ReliabilityConfig {
  std::uint32_t provider_retries = 3;
  std::uint64_t provider_backoff_ms = 100;
  std::vector<std::string> fallback_providers;
  std::uint64_t channel_initial_backoff_secs = 2;
  std::uint64_t channel_max_backoff_secs = 60;
  std::uint64_t scheduler_poll_secs = 15;
  std::uint32_t scheduler_retries = 2;
};

struct HeartbeatConfig {
  bool enabled = false;
  std::uint64_t interval_minutes = 60;
  std::string tasks_file = "HEARTBEAT.md";
};

struct BrowserConfig {
  bool enabled = false;
  std::vector<std::string> allowed_domains;
  std::string session_name = "default";
  bool stealth = false;
  bool session_persistence = false;
};

struct ToolAllowConfig {
  std::vector<std::string> groups;
  std::vector<std::string> tools;
  std::vector<std::string> deny;
};

struct ToolsConfig {
  std::string profile = "full";
  ToolAllowConfig allow;
};

struct CalendarConfig {
  std::string backend = "auto";
  std::string default_calendar;
};

struct EmailSmtpConfig {
  std::string host;
  std::uint16_t port = 587;
  std::string username;
  std::string password;
  bool tls = true;
};

struct EmailConfig {
  std::string backend = "auto";
  std::string default_account;
  std::optional<EmailSmtpConfig> smtp;
};

struct RemindersConfig {
  std::string default_channel;
};

struct WebSearchConfig {
  std::string provider = "auto";  // "brave", "duckduckgo", or "auto"
  std::optional<std::string> brave_api_key;
};

struct ComposioConfig {
  bool enabled = false;
  std::optional<std::string> api_key;
};

struct IdentityConfig {
  std::string format = "openclaw";
  std::optional<std::string> aieos_path;
  std::optional<std::string> aieos_inline;
};

struct SecretsConfig {
  bool encrypt = true;
};

struct AgentConfig {
  std::string id;
  std::string provider;
  std::string model;
  double temperature = 0.7;
  std::string workspace_directory;
  std::string system_prompt;
  std::optional<std::string> api_key;
};

struct TeamConfig {
  std::string id;
  std::vector<std::string> agents;
  std::string leader_agent;
  std::string description;
};

struct MultiConfig {
  std::string default_agent = "ghostclaw";
  std::size_t max_internal_messages = 50;
  std::vector<AgentConfig> agents;
  std::vector<TeamConfig> teams;
};

struct ScheduleEntry {
  std::string id;
  std::string expression;
  std::string command;
  bool enabled = true;
};

struct DaemonConfig {
  bool auto_start_schedules = true;
  std::vector<ScheduleEntry> schedules;
};

struct McpServerConfig {
  std::string id;
  std::string command;
  std::vector<std::string> args;
  std::unordered_map<std::string, std::string> env;
  bool enabled = true;
};

struct McpConfig {
  std::vector<McpServerConfig> servers;
};

struct GoogleConfig {
  std::string client_id;
  std::string client_secret;
  std::vector<std::string> scopes = {
      "https://www.googleapis.com/auth/gmail.send",
      "https://www.googleapis.com/auth/gmail.readonly",
      "https://www.googleapis.com/auth/calendar"};
  std::uint16_t redirect_port = 8089;
};

struct Config {
  std::optional<std::string> api_key;
  std::string default_provider = "openrouter";
  std::string default_model = "gpt-4o-mini";
  double default_temperature = 0.7;
  MemoryConfig memory;
  GatewayConfig gateway;
  AutonomyConfig autonomy;
  ChannelsConfig channels;
  TunnelConfig tunnel;
  ObservabilityConfig observability;
  RuntimeConfig runtime;
  ReliabilityConfig reliability;
  HeartbeatConfig heartbeat;
  BrowserConfig browser;
  ToolsConfig tools;
  CalendarConfig calendar;
  EmailConfig email;
  RemindersConfig reminders;
  WebSearchConfig web_search;
  ComposioConfig composio;
  IdentityConfig identity;
  SecretsConfig secrets;
  MultiConfig multi;
  DaemonConfig daemon;
  McpConfig mcp;
  GoogleConfig google;
};

} // namespace ghostclaw::config
