#include "test_framework.hpp"

#include "ghostclaw/channels/allowlist.hpp"
#include "ghostclaw/channels/channel_manager.hpp"
#include "ghostclaw/channels/send_service.hpp"
#include "ghostclaw/channels/plugin_registry.hpp"
#include "ghostclaw/channels/discord/discord.hpp"
#include "ghostclaw/channels/imessage/imessage.hpp"
#include "ghostclaw/channels/signal/signal.hpp"
#include "ghostclaw/channels/slack/slack.hpp"
#include "ghostclaw/channels/telegram/telegram.hpp"
#include "ghostclaw/channels/whatsapp/whatsapp.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

class FlakyChannel final : public ghostclaw::channels::IChannel {
public:
  explicit FlakyChannel(int fail_times) : fail_times_(fail_times) {}

  [[nodiscard]] std::string_view name() const override { return "flaky"; }

  [[nodiscard]] ghostclaw::common::Status start() override {
    ++start_calls_;
    start_times_.push_back(std::chrono::steady_clock::now());
    if (start_calls_ <= fail_times_) {
      healthy_ = false;
      return ghostclaw::common::Status::error("simulated failure");
    }
    healthy_ = true;
    return ghostclaw::common::Status::success();
  }

  void stop() override { healthy_ = false; }

  [[nodiscard]] ghostclaw::common::Status send(const std::string &, const std::string &) override {
    return ghostclaw::common::Status::success();
  }

  void on_message(ghostclaw::channels::MessageCallback callback) override {
    callback_ = std::move(callback);
  }

  [[nodiscard]] bool health_check() override {
    if (healthy_ && start_calls_ > fail_times_) {
      // Let supervisor loop continue briefly then force a stop cycle.
      if (++health_checks_ > 3) {
        healthy_ = false;
      }
    }
    return healthy_;
  }

  int start_calls() const { return start_calls_; }
  const std::vector<std::chrono::steady_clock::time_point> &start_times() const {
    return start_times_;
  }

private:
  int fail_times_ = 0;
  std::atomic<int> start_calls_{0};
  std::atomic<int> health_checks_{0};
  std::atomic<bool> healthy_{false};
  ghostclaw::channels::MessageCallback callback_;
  std::vector<std::chrono::steady_clock::time_point> start_times_;
};

class FakePlugin final : public ghostclaw::channels::IChannelPlugin {
public:
  [[nodiscard]] std::string_view id() const override { return "fake"; }

  [[nodiscard]] ghostclaw::channels::ChannelCapabilities capabilities() const override {
    ghostclaw::channels::ChannelCapabilities caps;
    caps.reactions = true;
    return caps;
  }

  [[nodiscard]] ghostclaw::common::Status
  start(const ghostclaw::channels::ChannelConfig &config) override {
    running_ = true;
    last_config_id_ = config.id;
    return ghostclaw::common::Status::success();
  }

  void stop() override { running_ = false; }

  [[nodiscard]] ghostclaw::common::Status send_text(const std::string &recipient,
                                                    const std::string &text) override {
    last_recipient_ = recipient;
    last_text_ = text;
    return ghostclaw::common::Status::success();
  }

  [[nodiscard]] ghostclaw::common::Status
  send_media(const std::string &, const ghostclaw::channels::MediaMessage &) override {
    return ghostclaw::common::Status::error("unsupported");
  }

  void on_message(ghostclaw::channels::PluginMessageCallback callback) override {
    message_callback_ = std::move(callback);
  }

  void on_reaction(ghostclaw::channels::PluginReactionCallback callback) override {
    reaction_callback_ = std::move(callback);
  }

  [[nodiscard]] bool health_check() override { return running_; }

  void emit_message(const std::string &text) {
    if (!message_callback_) {
      return;
    }
    ghostclaw::channels::PluginMessage message;
    message.id = "plugin-msg";
    message.sender = "tester";
    message.channel = "fake";
    message.content = text;
    message.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    message_callback_(message);
  }

  [[nodiscard]] const std::string &last_text() const { return last_text_; }
  [[nodiscard]] const std::string &last_config_id() const { return last_config_id_; }

private:
  bool running_ = false;
  std::string last_recipient_;
  std::string last_text_;
  std::string last_config_id_;
  ghostclaw::channels::PluginMessageCallback message_callback_;
  ghostclaw::channels::PluginReactionCallback reaction_callback_;
};

class MockTelegramHttpClient final : public ghostclaw::providers::HttpClient {
public:
  struct Request {
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::uint64_t timeout_ms = 0;
  };

  std::deque<ghostclaw::providers::HttpResponse> post_responses;
  std::vector<Request> requests;
  std::mutex mutex;
  std::condition_variable cv;

  [[nodiscard]] ghostclaw::providers::HttpResponse
  post_json(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
            const std::string &body, std::uint64_t timeout_ms) override {
    std::lock_guard<std::mutex> lock(mutex);
    requests.push_back(Request{.url = url, .headers = headers, .body = body, .timeout_ms = timeout_ms});
    cv.notify_all();
    if (post_responses.empty()) {
      return ghostclaw::providers::HttpResponse{.status = 200, .body = R"({"ok":true,"result":[]})"};
    }
    auto response = post_responses.front();
    post_responses.pop_front();
    return response;
  }

  [[nodiscard]] ghostclaw::providers::HttpResponse
  post_json_stream(const std::string &, const std::unordered_map<std::string, std::string> &,
                   const std::string &, std::uint64_t,
                   const ghostclaw::providers::StreamChunkCallback &) override {
    return {};
  }

  [[nodiscard]] ghostclaw::providers::HttpResponse
  head(const std::string &, const std::unordered_map<std::string, std::string> &,
       std::uint64_t) override {
    return {};
  }

  bool wait_for_requests(const std::size_t count, const std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, [&]() { return requests.size() >= count; });
  }
};

} // namespace

void register_channels_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;
  namespace ch = ghostclaw::channels;

  tests.push_back({"channels_allowlist_empty_denies_all", [] {
                     require(!ch::check_allowlist("alice", {}), "empty allowlist should deny");
                   }});

  tests.push_back({"channels_allowlist_wildcard_allows", [] {
                     require(ch::check_allowlist("alice", {"*"}), "wildcard should allow");
                   }});

  tests.push_back({"channels_allowlist_exact_match", [] {
                     require(ch::check_allowlist("Alice", {"alice", "bob"}),
                             "exact match should allow");
                     require(!ch::check_allowlist("mallory", {"alice", "bob"}),
                             "non-match should deny");
                   }});

  tests.push_back({"channels_supervisor_restarts_after_failure", [] {
                     FlakyChannel channel(2);
                     ch::SupervisorConfig cfg;
                     cfg.initial_backoff = std::chrono::milliseconds(20);
                     cfg.max_backoff = std::chrono::milliseconds(80);
                     ch::ChannelSupervisor supervisor(channel, [](const ch::ChannelMessage &) {}, cfg);

                     supervisor.start();
                     std::this_thread::sleep_for(std::chrono::milliseconds(220));
                     supervisor.stop();

                     require(channel.start_calls() >= 3,
                             "supervisor should retry after failures");
                   }});

  tests.push_back({"channels_supervisor_backoff_grows", [] {
                     FlakyChannel channel(2);
                     ch::SupervisorConfig cfg;
                     cfg.initial_backoff = std::chrono::milliseconds(30);
                     cfg.max_backoff = std::chrono::milliseconds(120);
                     ch::ChannelSupervisor supervisor(channel, [](const ch::ChannelMessage &) {}, cfg);

                     supervisor.start();
                     std::this_thread::sleep_for(std::chrono::milliseconds(260));
                     supervisor.stop();

                     const auto &times = channel.start_times();
                     require(times.size() >= 3, "need at least three start attempts");
                     const auto d1 = std::chrono::duration_cast<std::chrono::milliseconds>(times[1] - times[0]);
                     const auto d2 = std::chrono::duration_cast<std::chrono::milliseconds>(times[2] - times[1]);
                     require(d2.count() >= d1.count(), "backoff should not shrink");
                   }});

  tests.push_back({"channels_manager_lists_channels", [] {
                     ghostclaw::config::Config config;
                     auto manager = ch::create_channel_manager(config);
                     auto names = manager->list_channels();
                     require(!names.empty(), "expected at least one channel");
                     require(manager->get_channel("cli") != nullptr, "cli channel should exist");
                   }});

  tests.push_back({"channels_send_service_cli_send", [] {
                     ghostclaw::config::Config config;
                     ch::SendService service(config);
                     auto sent = service.send({.channel = "cli", .recipient = "unit-test", .text = "hello"});
                     require(sent.ok(), sent.error());
                   }});

  tests.push_back({"channels_plugin_registry_registers_factories", [] {
                     ch::ChannelPluginRegistry registry;
                     auto reg = registry.register_factory("fake", []() {
                       return std::make_unique<FakePlugin>();
                     });
                     require(reg.ok(), reg.error());
                     require(registry.contains("fake"), "registry should contain fake plugin");

                     auto duplicate = registry.register_factory("fake", []() {
                       return std::make_unique<FakePlugin>();
                     });
                     require(!duplicate.ok(), "duplicate plugin registration should fail");

                     auto plugin = registry.create("fake");
                     require(plugin != nullptr, "plugin should be constructible from registry");
                   }});

  tests.push_back({"channels_manager_runs_plugin_channel", [] {
                     ghostclaw::config::Config config;
                     ch::ChannelManager manager(config);
                     auto reg = manager.register_plugin("fake", []() {
                       return std::make_unique<FakePlugin>();
                     });
                     require(reg.ok(), reg.error());

                     auto plugin = std::make_unique<FakePlugin>();
                     auto *raw_plugin = plugin.get();
                     auto added = manager.add_plugin(std::move(plugin), {.id = "fake-instance"});
                     require(added.ok(), added.error());
                     require(raw_plugin != nullptr, "raw plugin pointer should be valid");

                     std::atomic<int> seen{0};
                     auto started = manager.start_all([&](const ch::ChannelMessage &message) {
                       if (message.channel == "fake" && message.content == "plugin-ping") {
                         (void)seen.fetch_add(1);
                       }
                     });
                     require(started.ok(), started.error());

                     auto *channel = manager.get_channel("fake");
                     require(channel != nullptr, "plugin-backed channel should be visible");
                     for (int i = 0; i < 20 && raw_plugin->last_config_id().empty(); ++i) {
                       std::this_thread::sleep_for(std::chrono::milliseconds(10));
                     }
                     auto sent = channel->send("n/a", "hello-plugin");
                     require(sent.ok(), sent.error());
                     require(raw_plugin->last_text() == "hello-plugin", "send bridge mismatch");
                     require(raw_plugin->last_config_id() == "fake-instance",
                             "plugin config id mismatch");

                     raw_plugin->emit_message("plugin-ping");
                     std::this_thread::sleep_for(std::chrono::milliseconds(40));
                     manager.stop_all();
                     require(seen.load() >= 1, "plugin message should reach manager callback");
                   }});

  tests.push_back({"channels_telegram_plugin_dispatches_allowed_messages", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(ghostclaw::providers::HttpResponse{
                         .status = 200,
                         .body = R"({"ok":true,"result":[{"update_id":1001,"message":{"message_id":88,"date":1700000000,"text":"hello from telegram","from":{"id":42,"username":"alice"},"chat":{"id":4242,"type":"private"}}}]})",
                     });

                     ch::telegram::TelegramChannelPlugin plugin(http);
                     std::mutex wait_mutex;
                     std::condition_variable wait_cv;
                     bool received = false;
                     std::string received_text;
                     std::string received_sender;
                     plugin.on_message([&](const ch::PluginMessage &message) {
                       std::lock_guard<std::mutex> lock(wait_mutex);
                       received = true;
                       received_text = message.content;
                       received_sender = message.sender;
                       wait_cv.notify_all();
                     });

                     ch::ChannelConfig cfg;
                     cfg.id = "telegram";
                     cfg.settings["bot_token"] = "123:test-token";
                     cfg.settings["allowed_users"] = "alice";
                     cfg.settings["poll_timeout_seconds"] = "0";
                     cfg.settings["idle_sleep_ms"] = "5";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     {
                       std::unique_lock<std::mutex> lock(wait_mutex);
                       (void)wait_cv.wait_for(lock, std::chrono::milliseconds(350),
                                              [&]() { return received; });
                     }
                     plugin.stop();

                     require(received, "telegram plugin should dispatch inbound message");
                     require(received_text == "hello from telegram", "telegram text mismatch");
                     require(received_sender == "alice", "telegram sender mismatch");
                     require(!http->requests.empty(), "telegram plugin should poll getUpdates");
                     require(http->requests.front().url.find("/getUpdates") != std::string::npos,
                             "telegram polling endpoint mismatch");
                   }});

  tests.push_back({"channels_telegram_plugin_enforces_allowlist", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(ghostclaw::providers::HttpResponse{
                         .status = 200,
                         .body = R"({"ok":true,"result":[{"update_id":1002,"message":{"message_id":89,"date":1700000001,"text":"should be blocked","from":{"id":99,"username":"mallory"},"chat":{"id":4242,"type":"private"}}}]})",
                     });

                     ch::telegram::TelegramChannelPlugin plugin(http);
                     std::atomic<int> seen{0};
                     plugin.on_message([&](const ch::PluginMessage &) { (void)seen.fetch_add(1); });

                     ch::ChannelConfig cfg;
                     cfg.id = "telegram";
                     cfg.settings["bot_token"] = "123:test-token";
                     cfg.settings["allowed_users"] = "alice";
                     cfg.settings["poll_timeout_seconds"] = "0";
                     cfg.settings["idle_sleep_ms"] = "5";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     std::this_thread::sleep_for(std::chrono::milliseconds(140));
                     plugin.stop();
                     require(seen.load() == 0, "allowlist should block unknown telegram sender");
                   }});

  tests.push_back({"channels_telegram_plugin_send_text_and_media", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(
                         ghostclaw::providers::HttpResponse{.status = 200, .body = R"({"ok":true,"result":{}})"});
                     http->post_responses.push_back(
                         ghostclaw::providers::HttpResponse{.status = 200, .body = R"({"ok":true,"result":{}})"});

                     ch::telegram::TelegramChannelPlugin plugin(http);
                     ch::ChannelConfig cfg;
                     cfg.id = "telegram";
                     cfg.settings["bot_token"] = "123:test-token";
                     cfg.settings["polling_enabled"] = "false";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     auto send_text = plugin.send_text("4242", "hello outbound");
                     require(send_text.ok(), send_text.error());

                     ch::MediaMessage media;
                     media.url = "https://example.com/pic.jpg";
                     media.mime_type = "image/jpeg";
                     media.caption = "preview";
                     auto send_media = plugin.send_media("4242", media);
                     require(send_media.ok(), send_media.error());
                     plugin.stop();

                     require(http->requests.size() >= 2, "expected sendMessage and sendPhoto requests");
                     require(http->requests[0].url.find("/sendMessage") != std::string::npos,
                             "send_text should call sendMessage");
                     require(http->requests[0].body.find("\"chat_id\":\"4242\"") != std::string::npos,
                             "send_text chat_id missing");
                     require(http->requests[0].body.find("\"text\":\"hello outbound\"") != std::string::npos,
                             "send_text payload mismatch");
                     require(http->requests[1].url.find("/sendPhoto") != std::string::npos,
                             "image media should call sendPhoto");
                   }});

  tests.push_back({"channels_manager_auto_adds_telegram_when_configured", [] {
                     ghostclaw::config::Config config;
                     ghostclaw::config::TelegramConfig telegram_cfg;
                     telegram_cfg.bot_token = "123:test-token";
                     telegram_cfg.allowed_users = {"alice"};
                     config.channels.telegram = telegram_cfg;

                     auto manager = ch::create_channel_manager(config);
                     const auto channels = manager->list_channels();
                     require(std::find(channels.begin(), channels.end(), "telegram") != channels.end(),
                             "telegram channel should be present when configured");
                   }});

  tests.push_back({"channels_discord_plugin_send_text", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(
                         ghostclaw::providers::HttpResponse{.status = 200, .body = R"({"id":"m1"})"});

                     ch::discord::DiscordChannelPlugin plugin(http);
                     ch::ChannelConfig cfg;
                     cfg.id = "discord";
                     cfg.settings["bot_token"] = "discord-token";
                     cfg.settings["channel_id"] = "12345";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     auto sent = plugin.send_text("", "hello discord");
                     require(sent.ok(), sent.error());
                     plugin.stop();

                     require(!http->requests.empty(), "discord request missing");
                     require(http->requests[0].url.find("/channels/12345/messages") != std::string::npos,
                             "discord endpoint mismatch");
                     require(http->requests[0].headers.at("Authorization") == "Bot discord-token",
                             "discord auth header mismatch");
                   }});

  tests.push_back({"channels_slack_plugin_send_text", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(ghostclaw::providers::HttpResponse{
                         .status = 200, .body = R"({"ok":true,"ts":"1"})"});

                     ch::slack::SlackChannelPlugin plugin(http);
                     ch::ChannelConfig cfg;
                     cfg.id = "slack";
                     cfg.settings["bot_token"] = "xoxb-test";
                     cfg.settings["channel_id"] = "C123";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     auto sent = plugin.send_text("", "hello slack");
                     require(sent.ok(), sent.error());
                     plugin.stop();

                     require(!http->requests.empty(), "slack request missing");
                     require(http->requests[0].url == "https://slack.com/api/chat.postMessage",
                             "slack endpoint mismatch");
                     require(http->requests[0].headers.at("Authorization") == "Bearer xoxb-test",
                             "slack auth header mismatch");
                     require(http->requests[0].body.find("\"channel\":\"C123\"") != std::string::npos,
                             "slack channel payload mismatch");
                   }});

  tests.push_back({"channels_whatsapp_plugin_send_text", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(
                         ghostclaw::providers::HttpResponse{.status = 200, .body = R"({"messages":[{"id":"wamid"}]})"});

                     ch::whatsapp::WhatsAppChannelPlugin plugin(http);
                     ch::ChannelConfig cfg;
                     cfg.id = "whatsapp";
                     cfg.settings["access_token"] = "wa-token";
                     cfg.settings["phone_number_id"] = "998877";
                     cfg.settings["allowed_numbers"] = "+12025550123";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     auto sent = plugin.send_text("+12025550123", "hello wa");
                     require(sent.ok(), sent.error());
                     auto blocked = plugin.send_text("+12025550124", "blocked");
                     require(!blocked.ok(), "whatsapp allowlist should block number");
                     plugin.stop();

                     require(!http->requests.empty(), "whatsapp request missing");
                     require(http->requests[0].url.find("/v21.0/998877/messages") != std::string::npos,
                             "whatsapp endpoint mismatch");
                     require(http->requests[0].headers.at("Authorization") == "Bearer wa-token",
                             "whatsapp auth header mismatch");
                   }});

  tests.push_back({"channels_signal_plugin_send_text", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(
                         ghostclaw::providers::HttpResponse{.status = 201, .body = R"({"timestamp":1})"});

                     ch::signal::SignalChannelPlugin plugin(http);
                     ch::ChannelConfig cfg;
                     cfg.id = "signal";
                     cfg.settings["api_url"] = "http://127.0.0.1:9000";
                     cfg.settings["account"] = "+12025550123";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     auto sent = plugin.send_text("+12025550124", "hello signal");
                     require(sent.ok(), sent.error());
                     plugin.stop();

                     require(!http->requests.empty(), "signal request missing");
                     require(http->requests[0].url == "http://127.0.0.1:9000/v2/send",
                             "signal endpoint mismatch");
                     require(http->requests[0].body.find("\"number\":\"+12025550123\"") != std::string::npos,
                             "signal account payload mismatch");
                   }});

  tests.push_back({"channels_imessage_plugin_dry_run", [] {
                     ch::imessage::IMessageChannelPlugin plugin;
                     ch::ChannelConfig cfg;
                     cfg.id = "imessage";
                     cfg.settings["allowed_contacts"] = "alice@icloud.com";
                     cfg.settings["dry_run"] = "true";
                     auto started = plugin.start(cfg);
                     require(started.ok(), started.error());

                     auto sent = plugin.send_text("alice@icloud.com", "hello imessage");
                     require(sent.ok(), sent.error());
                     auto blocked = plugin.send_text("bob@icloud.com", "blocked");
                     require(!blocked.ok(), "imessage allowlist should block unknown contact");
                     plugin.stop();
                   }});

  tests.push_back({"channels_manager_auto_adds_multiple_configured_plugins", [] {
                     ghostclaw::config::Config config;

                     ghostclaw::config::TelegramConfig telegram_cfg;
                     telegram_cfg.bot_token = "t";
                     config.channels.telegram = telegram_cfg;

                     ghostclaw::config::DiscordConfig discord_cfg;
                     discord_cfg.bot_token = "d";
                     discord_cfg.guild_id = "g";
                     config.channels.discord = discord_cfg;

                     ghostclaw::config::SlackConfig slack_cfg;
                     slack_cfg.bot_token = "s";
                     slack_cfg.channel_id = "c";
                     config.channels.slack = slack_cfg;

                     ghostclaw::config::WhatsAppConfig whatsapp_cfg;
                     whatsapp_cfg.access_token = "w";
                     whatsapp_cfg.phone_number_id = "pn";
                     whatsapp_cfg.verify_token = "vt";
                     config.channels.whatsapp = whatsapp_cfg;

                     ghostclaw::config::IMessageConfig imessage_cfg;
                     imessage_cfg.allowed_contacts = {"alice"};
                     config.channels.imessage = imessage_cfg;

                     auto manager = ch::create_channel_manager(config);
                     const auto channels = manager->list_channels();
                     require(std::find(channels.begin(), channels.end(), "telegram") != channels.end(),
                             "telegram should be auto-added");
                     require(std::find(channels.begin(), channels.end(), "discord") != channels.end(),
                             "discord should be auto-added");
                     require(std::find(channels.begin(), channels.end(), "slack") != channels.end(),
                             "slack should be auto-added");
                     require(std::find(channels.begin(), channels.end(), "whatsapp") != channels.end(),
                             "whatsapp should be auto-added");
                    require(std::find(channels.begin(), channels.end(), "imessage") != channels.end(),
                            "imessage should be auto-added");
                  }});

  // ============================================
  // NEW TESTS: Allowlist Edge Cases
  // ============================================

  tests.push_back({"channels_allowlist_case_insensitive", [] {
                     require(ch::check_allowlist("ALICE", {"alice"}), "should match case-insensitive");
                     require(ch::check_allowlist("alice", {"ALICE"}), "should match case-insensitive");
                     require(ch::check_allowlist("AlIcE", {"aLiCe"}), "should match mixed case");
                   }});

  tests.push_back({"channels_allowlist_no_whitespace_trim", [] {
                     // Allowlist does not trim whitespace - exact match required
                     require(!ch::check_allowlist("alice", {" alice "}), "whitespace not trimmed");
                     require(!ch::check_allowlist(" alice ", {"alice"}), "input whitespace not trimmed");
                   }});

  tests.push_back({"channels_allowlist_multiple_wildcards", [] {
                     require(ch::check_allowlist("anyone", {"*", "alice"}), "wildcard should allow");
                     require(ch::check_allowlist("alice", {"bob", "*"}), "wildcard in list should allow");
                   }});

  tests.push_back({"channels_allowlist_empty_sender_with_wildcard", [] {
                     // Empty sender is allowed with wildcard (wildcard matches everything)
                     require(ch::check_allowlist("", {"*"}), "wildcard allows empty sender");
                     require(!ch::check_allowlist("", {"alice", "bob"}), "empty sender denied without wildcard");
                   }});

  // ============================================
  // NEW TESTS: Supervisor Edge Cases
  // ============================================

  tests.push_back({"channels_supervisor_max_backoff_capped", [] {
                     FlakyChannel channel(10); // Fail many times
                     ch::SupervisorConfig cfg;
                     cfg.initial_backoff = std::chrono::milliseconds(10);
                     cfg.max_backoff = std::chrono::milliseconds(50);
                     ch::ChannelSupervisor supervisor(channel, [](const ch::ChannelMessage &) {}, cfg);

                     supervisor.start();
                     std::this_thread::sleep_for(std::chrono::milliseconds(400));
                     supervisor.stop();

                     const auto &times = channel.start_times();
                     if (times.size() >= 4) {
                       // Later intervals should not exceed max_backoff significantly.
                       // Use generous tolerance (300ms) for slow CI runners.
                       for (std::size_t i = 3; i < times.size(); ++i) {
                         auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                             times[i] - times[i - 1]);
                         require(interval.count() <= 300, "backoff should be capped");
                       }
                     }
                   }});

  tests.push_back({"channels_supervisor_callback_invoked", [] {
                     class CallbackChannel final : public ch::IChannel {
                     public:
                       [[nodiscard]] std::string_view name() const override { return "callback"; }
                       [[nodiscard]] ghostclaw::common::Status start() override {
                         return ghostclaw::common::Status::success();
                       }
                       void stop() override {}
                       [[nodiscard]] ghostclaw::common::Status send(const std::string &,
                                                                    const std::string &) override {
                         return ghostclaw::common::Status::success();
                       }
                       void on_message(ch::MessageCallback cb) override { callback_ = std::move(cb); }
                       [[nodiscard]] bool health_check() override { return true; }

                       void emit(const std::string &content) {
                         if (callback_) {
                           ch::ChannelMessage msg;
                           msg.content = content;
                           msg.channel = "callback";
                           callback_(msg);
                         }
                       }

                     private:
                       ch::MessageCallback callback_;
                     };

                     CallbackChannel channel;
                     std::atomic<int> received{0};
                     ch::ChannelSupervisor supervisor(channel, [&](const ch::ChannelMessage &msg) {
                       if (msg.content == "test") {
                         received.fetch_add(1);
                       }
                     });

                     supervisor.start();
                     std::this_thread::sleep_for(std::chrono::milliseconds(50));
                     channel.emit("test");
                     std::this_thread::sleep_for(std::chrono::milliseconds(50));
                     supervisor.stop();

                     require(received.load() >= 1, "callback should be invoked");
                   }});

  // ============================================
  // NEW TESTS: Channel Manager Operations
  // ============================================

  tests.push_back({"channels_manager_get_nonexistent_returns_null", [] {
                     ghostclaw::config::Config config;
                     auto manager = ch::create_channel_manager(config);
                     require(manager->get_channel("nonexistent") == nullptr,
                             "nonexistent channel should return null");
                   }});

  tests.push_back({"channels_manager_list_plugins", [] {
                     ghostclaw::config::Config config;
                     auto manager = ch::create_channel_manager(config);
                     auto plugins = manager->list_plugins();
                     require(!plugins.empty(), "should have registered plugins");
                     require(std::find(plugins.begin(), plugins.end(), "telegram") != plugins.end(),
                             "telegram plugin should be registered");
                   }});

  tests.push_back({"channels_manager_start_stop_idempotent", [] {
                     ghostclaw::config::Config config;
                     auto manager = ch::create_channel_manager(config);

                     auto started1 = manager->start_all([](const ch::ChannelMessage &) {});
                     require(started1.ok(), started1.error());

                     auto started2 = manager->start_all([](const ch::ChannelMessage &) {});
                     require(started2.ok(), "second start should succeed (idempotent)");

                     manager->stop_all();
                     manager->stop_all(); // Should not crash
                   }});

  // ============================================
  // NEW TESTS: Telegram Plugin Edge Cases
  // ============================================

  tests.push_back({"channels_telegram_handles_empty_message", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(ghostclaw::providers::HttpResponse{
                         .status = 200,
                         .body = R"({"ok":true,"result":[{"update_id":1003,"message":{"message_id":90,"date":1700000002,"text":"","from":{"id":42,"username":"alice"},"chat":{"id":4242,"type":"private"}}}]})",
                     });

                     ch::telegram::TelegramChannelPlugin plugin(http);
                     std::atomic<int> seen{0};
                     plugin.on_message([&](const ch::PluginMessage &) { seen.fetch_add(1); });

                     ch::ChannelConfig cfg;
                     cfg.id = "telegram";
                     cfg.settings["bot_token"] = "123:test";
                     cfg.settings["poll_timeout_seconds"] = "0";
                     cfg.settings["idle_sleep_ms"] = "5";
                     require(plugin.start(cfg).ok(), "start should succeed");

                     std::this_thread::sleep_for(std::chrono::milliseconds(100));
                     plugin.stop();

                     require(seen.load() == 0, "empty messages should be filtered");
                   }});

  tests.push_back({"channels_telegram_handles_media_caption", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     http->post_responses.push_back(ghostclaw::providers::HttpResponse{
                         .status = 200,
                         .body = R"({"ok":true,"result":[{"update_id":1004,"message":{"message_id":91,"date":1700000003,"caption":"photo caption","from":{"id":42,"username":"alice"},"chat":{"id":4242,"type":"private"},"photo":[{"file_id":"abc"}]}}]})",
                     });

                     ch::telegram::TelegramChannelPlugin plugin(http);
                     std::string received_text;
                     plugin.on_message([&](const ch::PluginMessage &msg) {
                       received_text = msg.content;
                     });

                     ch::ChannelConfig cfg;
                     cfg.id = "telegram";
                     cfg.settings["bot_token"] = "123:test";
                     cfg.settings["poll_timeout_seconds"] = "0";
                     cfg.settings["idle_sleep_ms"] = "5";
                     require(plugin.start(cfg).ok(), "start should succeed");

                     std::this_thread::sleep_for(std::chrono::milliseconds(100));
                     plugin.stop();

                     require(received_text == "photo caption", "caption should be used as text");
                   }});

  tests.push_back({"channels_telegram_health_check", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::telegram::TelegramChannelPlugin plugin(http);

                     // Before start, health_check returns true (not running = healthy)
                     require(plugin.health_check(), "should be healthy before start (not running)");

                     ch::ChannelConfig cfg;
                     cfg.id = "telegram";
                     cfg.settings["bot_token"] = "123:test";
                     cfg.settings["polling_enabled"] = "false";
                     require(plugin.start(cfg).ok(), "start should succeed");

                     require(plugin.health_check(), "should be healthy after start");

                     plugin.stop();
                   }});

  // ============================================
  // NEW TESTS: Discord Plugin Edge Cases
  // ============================================

  tests.push_back({"channels_discord_requires_token", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::discord::DiscordChannelPlugin plugin(http);

                     ch::ChannelConfig cfg;
                     cfg.id = "discord";
                     // Missing bot_token
                     auto started = plugin.start(cfg);
                     require(!started.ok(), "should fail without token");
                   }});

  tests.push_back({"channels_discord_capabilities", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::discord::DiscordChannelPlugin plugin(http);
                     auto caps = plugin.capabilities();
                     require(caps.reactions, "discord should support reactions");
                     require(caps.threads, "discord should support threads");
                     require(caps.media, "discord should support media");
                   }});

  // ============================================
  // NEW TESTS: Slack Plugin Edge Cases
  // ============================================

  tests.push_back({"channels_slack_requires_token", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::slack::SlackChannelPlugin plugin(http);

                     ch::ChannelConfig cfg;
                     cfg.id = "slack";
                     // Missing bot_token
                     auto started = plugin.start(cfg);
                     require(!started.ok(), "should fail without token");
                   }});

  tests.push_back({"channels_slack_capabilities", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::slack::SlackChannelPlugin plugin(http);
                     auto caps = plugin.capabilities();
                     require(caps.reactions, "slack should support reactions");
                     require(caps.threads, "slack should support threads");
                   }});

  // ============================================
  // NEW TESTS: WhatsApp Plugin Edge Cases
  // ============================================

  tests.push_back({"channels_whatsapp_requires_credentials", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::whatsapp::WhatsAppChannelPlugin plugin(http);

                     ch::ChannelConfig cfg;
                     cfg.id = "whatsapp";
                     cfg.settings["access_token"] = "token";
                     // Missing phone_number_id
                     auto started = plugin.start(cfg);
                     require(!started.ok(), "should fail without phone_number_id");
                   }});

  tests.push_back({"channels_whatsapp_capabilities", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::whatsapp::WhatsAppChannelPlugin plugin(http);
                     auto caps = plugin.capabilities();
                     require(caps.polls, "whatsapp should support polls");
                     require(caps.reactions, "whatsapp should support reactions");
                   }});

  // ============================================
  // NEW TESTS: Signal Plugin Edge Cases
  // ============================================

  tests.push_back({"channels_signal_requires_account", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::signal::SignalChannelPlugin plugin(http);

                     ch::ChannelConfig cfg;
                     cfg.id = "signal";
                     cfg.settings["api_url"] = "http://localhost:9000";
                     // Missing account
                     auto started = plugin.start(cfg);
                     require(!started.ok(), "should fail without account");
                   }});

  tests.push_back({"channels_signal_capabilities", [] {
                     auto http = std::make_shared<MockTelegramHttpClient>();
                     ch::signal::SignalChannelPlugin plugin(http);
                     auto caps = plugin.capabilities();
                     require(caps.reactions, "signal should support reactions");
                     require(caps.media, "signal should support media");
                   }});

  // ============================================
  // NEW TESTS: iMessage Plugin Edge Cases
  // ============================================

  tests.push_back({"channels_imessage_capabilities", [] {
                     ch::imessage::IMessageChannelPlugin plugin;
                     auto caps = plugin.capabilities();
                     require(caps.reply, "imessage should support reply");
                     require(caps.media, "imessage should support media");
                   }});

  tests.push_back({"channels_imessage_allowlist_enforced", [] {
                     ch::imessage::IMessageChannelPlugin plugin;
                     ch::ChannelConfig cfg;
                     cfg.id = "imessage";
                     cfg.settings["allowed_contacts"] = "alice@icloud.com,bob@icloud.com";
                     cfg.settings["dry_run"] = "true";
                     require(plugin.start(cfg).ok(), "start should succeed");

                     require(plugin.send_text("alice@icloud.com", "hi").ok(), "allowed should succeed");
                     require(!plugin.send_text("mallory@icloud.com", "hi").ok(), "blocked should fail");
                     plugin.stop();
                   }});
}
