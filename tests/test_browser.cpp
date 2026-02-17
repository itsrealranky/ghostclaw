#include "test_framework.hpp"

#include "ghostclaw/browser/a11y.hpp"
#include "ghostclaw/browser/actions.hpp"
#include "ghostclaw/browser/cdp.hpp"
#include "ghostclaw/browser/chrome.hpp"
#include "ghostclaw/browser/element.hpp"
#include "ghostclaw/browser/profiles.hpp"
#include "ghostclaw/browser/readability.hpp"
#include "ghostclaw/browser/server.hpp"
#include "ghostclaw/browser/sessions.hpp"
#include "ghostclaw/browser/stealth.hpp"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::string find_json_string_field(const std::string &json, const std::string &field) {
  const std::string key = "\"" + field + "\"";
  const auto key_pos = json.find(key);
  if (key_pos == std::string::npos) {
    return "";
  }
  const auto colon = json.find(':', key_pos + key.size());
  if (colon == std::string::npos) {
    return "";
  }
  const auto quote = json.find('"', colon + 1);
  if (quote == std::string::npos) {
    return "";
  }
  const auto end = json.find('"', quote + 1);
  if (end == std::string::npos || end <= quote) {
    return "";
  }
  return json.substr(quote + 1, end - quote - 1);
}

int find_json_int_field(const std::string &json, const std::string &field) {
  const std::string key = "\"" + field + "\"";
  const auto key_pos = json.find(key);
  if (key_pos == std::string::npos) {
    return 0;
  }
  const auto colon = json.find(':', key_pos + key.size());
  if (colon == std::string::npos) {
    return 0;
  }
  std::size_t pos = colon + 1;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
    ++pos;
  }
  std::size_t end = pos;
  while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])) != 0) {
    ++end;
  }
  if (end <= pos) {
    return 0;
  }
  return std::stoi(json.substr(pos, end - pos));
}

// Realistic accessibility tree for testing
const char *kRealisticA11yTree = R"([
  {"nodeId":"1","role":{"value":"WebArea"},"name":{"value":"Test Page"},"childIds":["2","3","4","5","6"],"backendDOMNodeId":1,"ignored":false,"properties":[]},
  {"nodeId":"2","role":{"value":"heading"},"name":{"value":"Welcome"},"childIds":[],"backendDOMNodeId":10,"ignored":false,"properties":[]},
  {"nodeId":"3","role":{"value":"button"},"name":{"value":"Submit"},"childIds":[],"backendDOMNodeId":42,"ignored":false,"properties":[]},
  {"nodeId":"4","role":{"value":"textbox"},"name":{"value":"Email"},"value":{"value":"user@test.com"},"childIds":[],"backendDOMNodeId":43,"ignored":false,"properties":[{"name":"focused","value":{"type":"booleanOrUndefined","value":"true"}}]},
  {"nodeId":"5","role":{"value":"paragraph"},"name":{"value":"Description text"},"childIds":["7"],"backendDOMNodeId":44,"ignored":false,"properties":[]},
  {"nodeId":"6","role":{"value":"link"},"name":{"value":"More info"},"childIds":[],"backendDOMNodeId":45,"ignored":false,"properties":[]},
  {"nodeId":"7","role":{"value":"StaticText"},"name":{"value":"Hello world"},"childIds":[],"backendDOMNodeId":46,"ignored":false,"properties":[]}
])";

class FakeCDPTransport final : public ghostclaw::browser::ICDPTransport {
public:
  [[nodiscard]] ghostclaw::common::Status connect(const std::string &) override {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    return ghostclaw::common::Status::success();
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    cv_.notify_all();
  }

  [[nodiscard]] bool is_connected() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
  }

  [[nodiscard]] ghostclaw::common::Status send_text(const std::string &payload) override {
    std::lock_guard<std::mutex> lock(mutex_);
    outbound_.push_back(payload);
    const int id = find_json_int_field(payload, "id");
    const std::string method = find_json_string_field(payload, "method");

    if (method == "Page.captureScreenshot") {
      inbound_.push_back("{\"id\":" + std::to_string(id) +
                         ",\"result\":{\"data\":\"base64-image\"}}");
    } else if (method == "Page.printToPDF") {
      inbound_.push_back("{\"id\":" + std::to_string(id) +
                         ",\"result\":{\"data\":\"base64-pdf\"}}");
    } else if (method == "Page.navigate") {
      inbound_.push_back("{\"id\":" + std::to_string(id) +
                         ",\"result\":{\"frameId\":\"frame-1\"}}");
    } else if (method == "Input.dispatchKeyEvent") {
      inbound_.push_back("{\"id\":" + std::to_string(id) + ",\"result\":{}}");
    } else if (method == "Accessibility.getFullAXTree") {
      // Return a realistic tree with multiple node types
      // The nodes must be a raw JSON array (not a quoted string) so
      // json_parse_flat stores the array as-is in the result map.
      std::string nodes = kRealisticA11yTree;
      // Strip newlines so the JSON stays on one line for safe embedding
      std::string compact;
      for (char c : nodes) {
        if (c != '\n') compact += c;
      }
      inbound_.push_back("{\"id\":" + std::to_string(id) +
                         ",\"result\":{\"nodes\":" + compact + "}}");
    } else if (method == "Runtime.evaluate") {
      inbound_.push_back(
          "{\"id\":" + std::to_string(id) +
          ",\"result\":{\"result\":{\"type\":\"string\",\"value\":\"ok\"}}}");
    } else if (method == "DOM.resolveNode") {
      // Extract backendNodeId from params
      std::string backend_id = find_json_string_field(payload, "backendNodeId");
      inbound_.push_back(
          "{\"id\":" + std::to_string(id) +
          ",\"result\":{\"object\":{\"objectId\":\"obj-" + backend_id + "\"}}}");
    } else if (method == "Runtime.callFunctionOn") {
      inbound_.push_back(
          "{\"id\":" + std::to_string(id) +
          ",\"result\":{\"result\":{\"type\":\"string\",\"value\":\"ok\"}}}");
    } else if (method == "Page.addScriptToEvaluateOnNewDocument") {
      inbound_.push_back("{\"id\":" + std::to_string(id) +
                         ",\"result\":{\"identifier\":\"1\"}}");
    } else if (method == "DOM.focus") {
      inbound_.push_back("{\"id\":" + std::to_string(id) + ",\"result\":{}}");
    } else {
      inbound_.push_back("{\"id\":" + std::to_string(id) +
                         ",\"result\":{\"product\":\"Chrome/125\"}}");
    }
    cv_.notify_all();
    return ghostclaw::common::Status::success();
  }

  [[nodiscard]] ghostclaw::common::Result<std::string>
  receive_text(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = cv_.wait_for(lock, timeout,
                                    [&]() { return !inbound_.empty() || !connected_; });
    if (!ready) {
      return ghostclaw::common::Result<std::string>::failure("timeout");
    }
    if (!connected_ && inbound_.empty()) {
      return ghostclaw::common::Result<std::string>::failure("closed");
    }
    const std::string value = inbound_.front();
    inbound_.pop_front();
    return ghostclaw::common::Result<std::string>::success(value);
  }

  void enqueue_event(const std::string &event_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    inbound_.push_back(event_json);
    cv_.notify_all();
  }

  [[nodiscard]] std::vector<std::string> get_outbound() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outbound_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool connected_ = false;
  std::deque<std::string> inbound_;
  std::vector<std::string> outbound_;
};

class FakeBrowserActions final : public ghostclaw::browser::IBrowserActions {
public:
  [[nodiscard]] ghostclaw::common::Result<ghostclaw::browser::BrowserActionResult>
  execute(const ghostclaw::browser::BrowserAction &action) override {
    seen.push_back(action);
    ghostclaw::browser::BrowserActionResult out;
    out.success = true;
    if (action.action == "navigate") {
      auto it = action.params.find("url");
      out.data["url"] = (it == action.params.end()) ? "" : it->second;
      out.data["status"] = "ok";
      return ghostclaw::common::Result<ghostclaw::browser::BrowserActionResult>::success(
          std::move(out));
    }
    if (action.action == "screenshot") {
      out.data["data"] = "base64-image";
      auto format_it = action.params.find("format");
      out.data["format"] = (format_it == action.params.end()) ? "png" : format_it->second;
      return ghostclaw::common::Result<ghostclaw::browser::BrowserActionResult>::success(
          std::move(out));
    }
    if (action.action == "snapshot") {
      out.data["snapshot"] = "e0 heading \"Welcome\"\ne1 button \"Submit\"";
      out.data["node_count"] = "2";
      out.data["ref_count"] = "2";
      return ghostclaw::common::Result<ghostclaw::browser::BrowserActionResult>::success(
          std::move(out));
    }
    if (action.action == "evaluate") {
      out.data["result"] = "ok";
      return ghostclaw::common::Result<ghostclaw::browser::BrowserActionResult>::success(
          std::move(out));
    }
    if (action.action == "read") {
      out.data["text"] = "Page text content";
      return ghostclaw::common::Result<ghostclaw::browser::BrowserActionResult>::success(
          std::move(out));
    }
    out.data["status"] = "ok";
    return ghostclaw::common::Result<ghostclaw::browser::BrowserActionResult>::success(
        std::move(out));
  }

  [[nodiscard]] ghostclaw::common::Result<std::vector<ghostclaw::browser::BrowserActionResult>>
  execute_batch(const std::vector<ghostclaw::browser::BrowserAction> &actions) override {
    std::vector<ghostclaw::browser::BrowserActionResult> out;
    out.reserve(actions.size());
    for (const auto &action : actions) {
      auto item = execute(action);
      if (!item.ok()) {
        return ghostclaw::common::Result<
            std::vector<ghostclaw::browser::BrowserActionResult>>::failure(item.error());
      }
      out.push_back(item.value());
    }
    return ghostclaw::common::Result<
        std::vector<ghostclaw::browser::BrowserActionResult>>::success(std::move(out));
  }

  std::vector<ghostclaw::browser::BrowserAction> seen;
};

} // namespace

void register_browser_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;
  namespace b = ghostclaw::browser;

  // =====================================================================
  // Existing tests (preserved)
  // =====================================================================

  tests.push_back({"browser_cdp_send_command_roundtrip", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     auto *raw = transport.get();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     auto result = client.send_command("Browser.getVersion");
                     require(result.ok(), result.error());
                     require(result.value().at("product") == "Chrome/125",
                             "cdp result mismatch");
                     (void)raw;
                     client.disconnect();
                   }});

  tests.push_back({"browser_cdp_event_callback", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     auto *raw = transport.get();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     std::mutex mutex;
                     std::condition_variable cv;
                     bool saw_event = false;
                     client.on_event("Network.requestWillBeSent",
                                     [&](const std::string &, const b::JsonMap &) {
                                       std::lock_guard<std::mutex> lock(mutex);
                                       saw_event = true;
                                       cv.notify_all();
                                     });

                     raw->enqueue_event(
                         R"({"method":"Network.requestWillBeSent","params":{"requestId":"1"}})");
                     std::unique_lock<std::mutex> lock(mutex);
                     const bool done = cv.wait_for(lock, std::chrono::milliseconds(300),
                                                   [&]() { return saw_event; });
                     require(done, "cdp event callback should fire");
                     client.disconnect();
                   }});

  tests.push_back({"browser_cdp_high_level_helpers", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     auto screenshot = client.capture_screenshot();
                     require(screenshot.ok(), screenshot.error());
                     require(screenshot.value() == "base64-image",
                             "capture_screenshot mismatch");

                     auto tree = client.get_accessibility_tree();
                     require(tree.ok(), tree.error());
                     require(tree.value().contains("nodes"),
                             "accessibility tree should have nodes");

                     auto eval = client.evaluate_js("1+1");
                     require(eval.ok(), eval.error());
                     require(eval.value().contains("result"),
                             "evaluate_js should return result payload");
                     client.disconnect();
                   }});

  tests.push_back({"browser_profiles_acquire_and_release", [] {
                     const auto root = std::filesystem::temp_directory_path() / "ghostclaw-browser-test";
                     std::vector<b::BrowserInstallation> injected{
                         {.kind = b::BrowserKind::Chromium,
                          .id = "chromium",
                          .display_name = "Chromium",
                          .executable = "/bin/echo",
                          .available = true}};
                     b::BrowserProfileManager manager(root, injected);

                     auto profile = manager.acquire_profile("phase11-session", "chromium");
                     require(profile.ok(), profile.error());
                     require(profile.value().devtools_port >= 18800 &&
                                 profile.value().devtools_port <= 18899,
                             "devtools port should be in reserved range");
                     require(!profile.value().color_hex.empty(), "profile color should be set");

                     const auto active = manager.list_active_profiles();
                     require(active.size() == 1, "active profile count mismatch");
                     auto released = manager.release_profile(profile.value().profile_id);
                     require(released.ok(), released.error());
                     require(manager.list_active_profiles().empty(),
                             "profile should be released");
                   }});

  tests.push_back({"browser_chrome_launch_args_and_ws_url", [] {
                     b::BrowserProfile profile;
                     profile.profile_id = "p1";
                     profile.browser_executable = "/bin/echo";
                     profile.user_data_dir = "/tmp/ghostclaw-browser-profile";
                     profile.devtools_port = 18888;

                     b::ChromeLaunchOptions options;
                     options.profile = profile;
                     options.start_url = "https://example.com";
                     options.headless = true;

                     auto args = b::build_chrome_launch_args(options);
                     require(args.ok(), args.error());
                     require(args.value().size() >= 5, "launch args should be populated");
                     require(args.value()[1].find("--remote-debugging-port=18888") == 0,
                             "missing debugging port argument");

                     auto ws = b::build_devtools_ws_url(18888, "/devtools/browser/test-id");
                     require(ws.ok(), ws.error());
                     require(ws.value() == "ws://127.0.0.1:18888/devtools/browser/test-id",
                             "devtools ws url mismatch");
                   }});

  tests.push_back({"browser_actions_execute_full_matrix", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::BrowserActions actions(client);
                     const std::vector<b::BrowserAction> batch{
                         {.action = "navigate", .params = {{"url", "https://example.com"}}},
                         {.action = "click", .params = {{"selector", "#submit"}}},
                         {.action = "type", .params = {{"text", "hello"}}},
                         {.action = "fill",
                          .params = {{"selector", "#email"}, {"value", "user@example.com"}}},
                         {.action = "press", .params = {{"key", "Enter"}}},
                         {.action = "hover", .params = {{"selector", "#menu"}}},
                         {.action = "drag", .params = {{"from", "#a"}, {"to", "#b"}}},
                         {.action = "select",
                          .params = {{"selector", "#country"}, {"value", "US"}}},
                         {.action = "scroll", .params = {{"x", "0"}, {"y", "240"}}},
                         {.action = "screenshot", .params = {{"format", "png"}}},
                         {.action = "snapshot", .params = {}},
                         {.action = "pdf", .params = {}},
                         {.action = "evaluate", .params = {{"expression", "1 + 1"}}}};

                     auto results = actions.execute_batch(batch);
                     require(results.ok(), results.error());
                     require(results.value().size() == batch.size(), "batch result count mismatch");
                     for (const auto &result : results.value()) {
                       require(result.success, "every action should report success");
                     }
                     require(results.value()[9].data.contains("data"),
                             "screenshot should return data");
                     require(results.value()[11].data["data"] == "base64-pdf",
                             "pdf data mismatch");

                     client.disconnect();
                   }});

  tests.push_back({"browser_actions_reject_unsupported_action", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::BrowserActions actions(client);
                     b::BrowserAction action;
                     action.action = "do_the_thing";
                     auto result = actions.execute(action);
                     require(!result.ok(), "unsupported action should fail");
                     require(result.error().find("unsupported browser action") != std::string::npos,
                             "unexpected unsupported action error");
                     client.disconnect();
                   }});

  tests.push_back({"browser_http_server_routes_and_tabs", [] {
                     FakeBrowserActions actions;
                     b::BrowserHttpServer server(actions);

                     b::BrowserHttpRequest open_req;
                     open_req.method = "POST";
                     open_req.path = "/tabs/open";
                     open_req.body = R"({"url":"https://example.com"})";
                     const auto open_resp = server.dispatch_for_test(open_req);
                     require(open_resp.status == 200, "tabs open should succeed");
                     const std::string tab_id = find_json_string_field(open_resp.body, "id");
                     require(!tab_id.empty(), "tabs open should return tab id");

                     b::BrowserHttpRequest list_req;
                     list_req.method = "GET";
                     list_req.path = "/tabs";
                     const auto list_resp = server.dispatch_for_test(list_req);
                     require(list_resp.status == 200, "tabs list should succeed");
                     require(list_resp.body.find(tab_id) != std::string::npos,
                             "tabs list should include opened tab");

                     b::BrowserHttpRequest navigate_req;
                     navigate_req.method = "POST";
                     navigate_req.path = "/navigate";
                     navigate_req.body = std::string("{\"tab_id\":\"") + tab_id +
                                         "\",\"url\":\"https://example.org\"}";
                     const auto navigate_resp = server.dispatch_for_test(navigate_req);
                     require(navigate_resp.status == 200, "navigate should succeed");

                     b::BrowserHttpRequest act_req;
                     act_req.method = "POST";
                     act_req.path = "/act";
                     act_req.body = R"({"action":"click","selector":"#ok"})";
                     const auto act_resp = server.dispatch_for_test(act_req);
                     require(act_resp.status == 200, "single act should succeed");
                     require(act_resp.body.find("\"success\":true") != std::string::npos,
                             "single act response should contain success");

                     b::BrowserHttpRequest batch_req;
                     batch_req.method = "POST";
                     batch_req.path = "/act";
                     batch_req.body =
                         R"({"actions":[{"action":"click","selector":"#one"},{"action":"type","text":"hello"}]})";
                     const auto batch_resp = server.dispatch_for_test(batch_req);
                     require(batch_resp.status == 200, "batch act should succeed");
                     require(batch_resp.body.find("\"count\":2") != std::string::npos,
                             "batch response should report action count");

                     b::BrowserHttpRequest screenshot_req;
                     screenshot_req.method = "POST";
                     screenshot_req.path = "/screenshot";
                     screenshot_req.body = std::string("{\"tab_id\":\"") + tab_id + "\"}";
                     const auto screenshot_resp = server.dispatch_for_test(screenshot_req);
                     require(screenshot_resp.status == 200, "screenshot should succeed");
                     require(screenshot_resp.body.find("base64-image") != std::string::npos,
                             "screenshot payload mismatch");

                     b::BrowserHttpRequest snapshot_req;
                     snapshot_req.method = "GET";
                     snapshot_req.path = "/snapshot";
                     const auto snapshot_resp = server.dispatch_for_test(snapshot_req);
                     require(snapshot_resp.status == 200, "snapshot should succeed");

                     b::BrowserHttpRequest close_req;
                     close_req.method = "DELETE";
                     close_req.path = "/tabs/" + tab_id;
                     const auto close_resp = server.dispatch_for_test(close_req);
                     require(close_resp.status == 200, "tabs close should succeed");

                     require(!actions.seen.empty(), "expected browser actions to be executed");
                   }});

  tests.push_back({"browser_http_server_start_stop", [] {
                     FakeBrowserActions actions;
                     b::BrowserHttpServer server(actions);
                     b::BrowserServerOptions options;
                     options.host = "127.0.0.1";
                     options.port = 0;
                     auto started = server.start(options);
                     if (!started.ok()) {
                       require(started.error().find("Operation not permitted") !=
                                   std::string::npos ||
                                   started.error().find("not implemented on Windows") !=
                                       std::string::npos,
                               "unexpected browser server start error");
                       return;
                     }
                     require(server.port() != 0, "server should bind ephemeral port");
                     require(server.is_running(), "server should report running");
                     server.stop();
                     require(!server.is_running(), "server should report stopped");
                   }});

  // =====================================================================
  // New tests: A11y Parser
  // =====================================================================

  tests.push_back({"browser_a11y_parse_basic_tree", [] {
                     b::A11yParser parser;
                     auto result = parser.parse_tree(kRealisticA11yTree);
                     require(result.ok(), result.error());
                     const auto &nodes = result.value();
                     // We expect: WebArea, heading, button, textbox, paragraph, link, StaticText = 7 non-ignored
                     require(nodes.size() >= 5, "should parse at least 5 nodes, got " + std::to_string(nodes.size()));

                     // Verify refs are sequential
                     require(nodes[0].ref == "e0", "first ref should be e0");
                     require(nodes[1].ref == "e1", "second ref should be e1");

                     // Verify roles are extracted
                     require(nodes[0].role == "WebArea", "first node should be WebArea");

                     // Find button and verify backend node id
                     bool found_button = false;
                     for (const auto &n : nodes) {
                       if (n.role == "button" && n.name == "Submit") {
                         found_button = true;
                         require(n.backend_node_id == 42, "button backend_node_id should be 42");
                       }
                     }
                     require(found_button, "should find Submit button");
                   }});

  tests.push_back({"browser_a11y_parse_skips_ignored", [] {
                     const std::string tree = R"([
                       {"nodeId":"1","role":{"value":"generic"},"name":{"value":""},"childIds":[],"backendDOMNodeId":1,"ignored":false,"properties":[]},
                       {"nodeId":"2","role":{"value":"none"},"name":{"value":""},"childIds":[],"backendDOMNodeId":2,"ignored":false,"properties":[]},
                       {"nodeId":"3","role":{"value":"InlineTextBox"},"name":{"value":"text"},"childIds":[],"backendDOMNodeId":3,"ignored":false,"properties":[]},
                       {"nodeId":"4","role":{"value":"button"},"name":{"value":"OK"},"childIds":[],"backendDOMNodeId":4,"ignored":false,"properties":[]},
                       {"nodeId":"5","role":{"value":"StaticText"},"name":{"value":""},"childIds":[],"backendDOMNodeId":5,"ignored":false,"properties":[]},
                       {"nodeId":"6","role":{"value":"heading"},"name":{"value":"Title"},"childIds":[],"backendDOMNodeId":6,"ignored":true,"properties":[]}
                     ])";
                     b::A11yParser parser;
                     auto result = parser.parse_tree(tree);
                     require(result.ok(), result.error());
                     // generic, none, InlineTextBox should be filtered out
                     // Empty StaticText should be filtered out
                     // Ignored heading should be filtered out
                     // Only button "OK" should remain
                     require(result.value().size() == 1, "only button should remain, got " + std::to_string(result.value().size()));
                     require(result.value()[0].role == "button", "remaining node should be button");
                     require(result.value()[0].ref == "e0", "ref should be e0");
                   }});

  tests.push_back({"browser_a11y_parse_empty", [] {
                     b::A11yParser parser;
                     auto result = parser.parse_tree("[]");
                     require(result.ok(), result.error());
                     require(result.value().empty(), "empty tree should return empty");

                     auto result2 = parser.parse_tree("");
                     require(result2.ok(), result2.error());
                     require(result2.value().empty(), "empty string should return empty");
                   }});

  tests.push_back({"browser_a11y_filter_interactive", [] {
                     b::A11yParser parser;
                     auto result = parser.parse_tree(kRealisticA11yTree);
                     require(result.ok(), result.error());

                     auto filtered = parser.filter_interactive(result.value());
                     // Only button, textbox, link should remain
                     for (const auto &node : filtered) {
                       require(node.role == "button" || node.role == "textbox" ||
                                   node.role == "link" || node.role == "checkbox" ||
                                   node.role == "radio" || node.role == "searchbox" ||
                                   node.role == "combobox" || node.role == "listbox" ||
                                   node.role == "option" || node.role == "switch" ||
                                   node.role == "slider" || node.role == "spinbutton" ||
                                   node.role == "menuitem" || node.role == "tab" ||
                                   node.role == "treeitem",
                               "filtered node should be interactive: " + node.role);
                     }
                     require(!filtered.empty(), "should have interactive nodes");
                     // Refs should be re-assigned
                     require(filtered[0].ref == "e0", "filtered refs should start at e0");
                   }});

  tests.push_back({"browser_a11y_filter_depth", [] {
                     b::A11yParser parser;
                     auto result = parser.parse_tree(kRealisticA11yTree);
                     require(result.ok(), result.error());

                     auto shallow = parser.filter_depth(result.value(), 0);
                     for (const auto &node : shallow) {
                       require(node.depth <= 0, "depth filter should respect max_depth");
                     }
                   }});

  tests.push_back({"browser_a11y_format_text", [] {
                     b::A11yParser parser;
                     auto result = parser.parse_tree(kRealisticA11yTree);
                     require(result.ok(), result.error());

                     std::string text = parser.format_text(result.value());
                     require(!text.empty(), "format_text should produce output");
                     // Should contain ref and role
                     require(text.find("e0") != std::string::npos, "should contain e0 ref");
                     require(text.find("WebArea") != std::string::npos, "should contain WebArea role");
                     // Textbox should show value
                     require(text.find("val=") != std::string::npos, "should show value for textbox");
                     // Focused textbox
                     require(text.find("focused") != std::string::npos, "should show focused state");
                   }});

  tests.push_back({"browser_a11y_format_json", [] {
                     b::A11yParser parser;
                     auto result = parser.parse_tree(kRealisticA11yTree);
                     require(result.ok(), result.error());

                     std::string json = parser.format_json(result.value());
                     require(!json.empty(), "format_json should produce output");
                     require(json.front() == '[' && json.back() == ']', "should be JSON array");
                     require(json.find("\"ref\"") != std::string::npos, "should contain ref field");
                     require(json.find("\"role\"") != std::string::npos, "should contain role field");
                   }});

  tests.push_back({"browser_a11y_diff_added_removed_changed", [] {
                     b::A11yParser parser;

                     std::vector<b::A11yNode> prev;
                     prev.push_back({.ref = "e0", .role = "button", .name = "OK", .backend_node_id = 1});
                     prev.push_back({.ref = "e1", .role = "textbox", .name = "Email", .value = "old@test.com", .backend_node_id = 2});
                     prev.push_back({.ref = "e2", .role = "link", .name = "Help", .backend_node_id = 3});

                     std::vector<b::A11yNode> current;
                     current.push_back({.ref = "e0", .role = "button", .name = "OK", .backend_node_id = 1});
                     current.push_back({.ref = "e1", .role = "textbox", .name = "Email", .value = "new@test.com", .backend_node_id = 2});
                     current.push_back({.ref = "e2", .role = "checkbox", .name = "Remember", .backend_node_id = 4});

                     auto diff = parser.compute_diff(prev, current);
                     require(diff.removed.size() == 1, "link should be removed");
                     require(diff.removed[0].role == "link", "removed should be link");
                     require(diff.added.size() == 1, "checkbox should be added");
                     require(diff.added[0].role == "checkbox", "added should be checkbox");
                     require(diff.changed.size() == 1, "textbox value should be changed");
                     require(diff.changed[0].role == "textbox", "changed should be textbox");
                   }});

  // =====================================================================
  // New tests: RefCache
  // =====================================================================

  tests.push_back({"browser_ref_cache_populate_resolve", [] {
                     b::RefCache cache;
                     std::vector<b::A11yNode> nodes;
                     nodes.push_back({.ref = "e0", .role = "button", .name = "OK", .backend_node_id = 42});
                     nodes.push_back({.ref = "e1", .role = "textbox", .name = "Email", .backend_node_id = 43});
                     cache.populate(nodes);

                     require(cache.size() == 2, "cache should have 2 entries");
                     auto resolved = cache.resolve("e0");
                     require(resolved.has_value(), "e0 should resolve");
                     require(*resolved == 42, "e0 should resolve to 42");

                     auto resolved2 = cache.resolve("e1");
                     require(resolved2.has_value(), "e1 should resolve");
                     require(*resolved2 == 43, "e1 should resolve to 43");
                   }});

  tests.push_back({"browser_ref_cache_resolve_missing", [] {
                     b::RefCache cache;
                     auto resolved = cache.resolve("e99");
                     require(!resolved.has_value(), "missing ref should return nullopt");
                   }});

  tests.push_back({"browser_ref_cache_clear", [] {
                     b::RefCache cache;
                     std::vector<b::A11yNode> nodes;
                     nodes.push_back({.ref = "e0", .role = "button", .name = "OK", .backend_node_id = 42});
                     cache.populate(nodes);
                     require(cache.size() == 1, "cache should have 1 entry");
                     cache.clear();
                     require(cache.size() == 0, "cache should be empty after clear");
                     require(!cache.resolve("e0").has_value(), "ref should not resolve after clear");
                   }});

  // =====================================================================
  // New tests: Element Resolver
  // =====================================================================

  tests.push_back({"browser_element_resolver_click", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::ElementResolver resolver(client);
                     auto result = resolver.click_by_node_id(42);
                     require(result.ok(), result.error());
                     require(result.value().at("status") == "clicked", "should report clicked");
                     client.disconnect();
                   }});

  tests.push_back({"browser_element_resolver_type", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::ElementResolver resolver(client);
                     auto result = resolver.type_by_node_id(43, "hi");
                     require(result.ok(), result.error());
                     require(result.value().at("status") == "typed", "should report typed");
                     require(result.value().at("text") == "hi", "should echo text");
                     client.disconnect();
                   }});

  tests.push_back({"browser_element_resolver_resolve_failure", [] {
                     // Test with a transport that always returns errors for DOM.resolveNode
                     // We'll just verify the error path by checking with a disconnected client
                     b::CDPClient client;
                     b::ElementResolver resolver(client);
                     auto result = resolver.click_by_node_id(999);
                     require(!result.ok(), "should fail when client is not connected");
                   }});

  // =====================================================================
  // New tests: Snapshot Pipeline (via BrowserActions)
  // =====================================================================

  tests.push_back({"browser_snapshot_with_filter", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::BrowserActions actions(client);
                     b::BrowserAction snapshot;
                     snapshot.action = "snapshot";
                     snapshot.params["filter"] = "interactive";
                     auto result = actions.execute(snapshot);
                     require(result.ok(), result.error());
                     require(result.value().success, "snapshot should succeed");
                     require(result.value().data.contains("snapshot"), "should contain snapshot");
                     require(result.value().data.contains("node_count"), "should contain node_count");
                     require(result.value().data.contains("ref_count"), "should contain ref_count");
                     client.disconnect();
                   }});

  tests.push_back({"browser_snapshot_ref_then_click", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::BrowserActions actions(client);

                     // First, take a snapshot to populate refs
                     b::BrowserAction snapshot;
                     snapshot.action = "snapshot";
                     auto snap_result = actions.execute(snapshot);
                     require(snap_result.ok(), snap_result.error());
                     require(snap_result.value().success, "snapshot should succeed");

                     // Now click by ref - the FakeCDPTransport handles DOM.resolveNode and Runtime.callFunctionOn
                     // We need to find a ref for a button node
                     // The snapshot output should contain button refs
                     b::BrowserAction click;
                     click.action = "click";
                     click.params["ref"] = "e0"; // First ref in the parsed tree
                     auto click_result = actions.execute(click);
                     require(click_result.ok(), click_result.error());
                     require(click_result.value().success, "click by ref should succeed");
                     require(click_result.value().data.at("status") == "clicked", "should report clicked");

                     client.disconnect();
                   }});

  tests.push_back({"browser_snapshot_diff_mode", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::BrowserActions actions(client);

                     // First snapshot
                     b::BrowserAction snap1;
                     snap1.action = "snapshot";
                     snap1.params["tab_id"] = "tab-1";
                     auto r1 = actions.execute(snap1);
                     require(r1.ok(), r1.error());

                     // Second snapshot with diff=true (same data, so should show no changes)
                     b::BrowserAction snap2;
                     snap2.action = "snapshot";
                     snap2.params["tab_id"] = "tab-1";
                     snap2.params["diff"] = "true";
                     auto r2 = actions.execute(snap2);
                     require(r2.ok(), r2.error());
                     require(r2.value().success, "diff snapshot should succeed");
                     require(r2.value().data.contains("diff"), "should contain diff");
                     require(r2.value().data.contains("added_count"), "should contain added_count");
                     // Same tree twice should show "(no changes)"
                     require(r2.value().data.at("diff") == "(no changes)",
                             "same tree should show no changes");

                     client.disconnect();
                   }});

  tests.push_back({"browser_snapshot_text_format", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     b::BrowserActions actions(client);
                     b::BrowserAction snapshot;
                     snapshot.action = "snapshot";
                     // default format is text
                     auto result = actions.execute(snapshot);
                     require(result.ok(), result.error());
                     const std::string &snap = result.value().data.at("snapshot");
                     // Text format should have indentation and refs
                     require(snap.find("e0") != std::string::npos, "text format should have refs");
                     require(snap.find("\n") != std::string::npos, "text format should have newlines");

                     client.disconnect();
                   }});

  // =====================================================================
  // New tests: Stealth
  // =====================================================================

  tests.push_back({"browser_stealth_script_content", [] {
                     const auto &script = b::StealthManager::stealth_script();
                     require(!script.empty(), "stealth script should not be empty");
                     require(script.find("webdriver") != std::string::npos,
                             "stealth script should patch webdriver");
                     require(script.find("chrome.runtime") != std::string::npos,
                             "stealth script should patch chrome.runtime");
                     require(script.find("plugins") != std::string::npos,
                             "stealth script should patch plugins");
                   }});

  tests.push_back({"browser_stealth_enable_sends_command", [] {
                     auto transport = std::make_unique<FakeCDPTransport>();
                     auto *raw = transport.get();
                     b::CDPClient client(std::move(transport));
                     auto connected = client.connect("ws://127.0.0.1:9222/devtools/browser");
                     require(connected.ok(), connected.error());

                     auto status = b::StealthManager::enable(client);
                     require(status.ok(), status.error());

                     // Verify the command was sent
                     auto outbound = raw->get_outbound();
                     bool found = false;
                     for (const auto &msg : outbound) {
                       if (msg.find("Page.addScriptToEvaluateOnNewDocument") != std::string::npos) {
                         found = true;
                         break;
                       }
                     }
                     require(found, "stealth should send Page.addScriptToEvaluateOnNewDocument");
                     client.disconnect();
                   }});

  // =====================================================================
  // New tests: Readability
  // =====================================================================

  tests.push_back({"browser_readability_script_content", [] {
                     const auto &script = b::ReadabilityExtractor::extraction_script();
                     require(!script.empty(), "readability script should not be empty");
                     require(script.find("article") != std::string::npos,
                             "readability script should look for article");
                     require(script.find("innerText") != std::string::npos,
                             "readability script should extract text");
                   }});

  // =====================================================================
  // New tests: Session Persistence
  // =====================================================================

  tests.push_back({"browser_session_save_load_roundtrip", [] {
                     const auto dir = std::filesystem::temp_directory_path() / "ghostclaw-session-test";
                     std::filesystem::create_directories(dir);

                     b::SessionPersistence session(dir.string());

                     std::vector<b::SavedTab> tabs;
                     tabs.push_back({.url = "https://example.com", .title = "Example"});
                     tabs.push_back({.url = "https://test.org", .title = "Test"});

                     auto save_status = session.save(tabs);
                     require(save_status.ok(), save_status.error());

                     auto loaded = session.load();
                     require(loaded.ok(), loaded.error());
                     require(loaded.value().size() == 2, "should load 2 tabs");
                     require(loaded.value()[0].url == "https://example.com", "first tab url mismatch");
                     require(loaded.value()[0].title == "Example", "first tab title mismatch");
                     require(loaded.value()[1].url == "https://test.org", "second tab url mismatch");

                     // Cleanup
                     std::filesystem::remove_all(dir);
                   }});

  tests.push_back({"browser_session_clear", [] {
                     const auto dir = std::filesystem::temp_directory_path() / "ghostclaw-session-clear-test";
                     std::filesystem::create_directories(dir);

                     b::SessionPersistence session(dir.string());

                     std::vector<b::SavedTab> tabs;
                     tabs.push_back({.url = "https://example.com", .title = "Example"});
                     auto save_status = session.save(tabs);
                     require(save_status.ok(), save_status.error());

                     auto clear_status = session.clear();
                     require(clear_status.ok(), clear_status.error());

                     auto loaded = session.load();
                     require(loaded.ok(), loaded.error());
                     require(loaded.value().empty(), "should be empty after clear");

                     // Cleanup
                     std::filesystem::remove_all(dir);
                   }});

  // =====================================================================
  // New tests: HTTP Server Integration
  // =====================================================================

  tests.push_back({"browser_http_snapshot_query_params", [] {
                     FakeBrowserActions actions;
                     b::BrowserHttpServer server(actions);

                     // Open a tab first
                     b::BrowserHttpRequest open_req;
                     open_req.method = "POST";
                     open_req.path = "/tabs/open";
                     open_req.body = R"({"url":"about:blank"})";
                     (void)server.dispatch_for_test(open_req);

                     // Snapshot with filter query param
                     b::BrowserHttpRequest snapshot_req;
                     snapshot_req.method = "GET";
                     snapshot_req.path = "/snapshot";
                     snapshot_req.query["filter"] = "interactive";
                     snapshot_req.query["format"] = "json";
                     const auto resp = server.dispatch_for_test(snapshot_req);
                     require(resp.status == 200, "snapshot with params should succeed");

                     // Verify the action received the params
                     bool found_snapshot = false;
                     for (const auto &action : actions.seen) {
                       if (action.action == "snapshot") {
                         found_snapshot = true;
                         auto filter_it = action.params.find("filter");
                         require(filter_it != action.params.end(), "filter param should be forwarded");
                         require(filter_it->second == "interactive", "filter should be interactive");
                         auto format_it = action.params.find("format");
                         require(format_it != action.params.end(), "format param should be forwarded");
                         require(format_it->second == "json", "format should be json");
                       }
                     }
                     require(found_snapshot, "snapshot action should be executed");
                   }});

  tests.push_back({"browser_http_read_endpoint", [] {
                     FakeBrowserActions actions;
                     b::BrowserHttpServer server(actions);

                     // Open a tab first
                     b::BrowserHttpRequest open_req;
                     open_req.method = "POST";
                     open_req.path = "/tabs/open";
                     open_req.body = R"({"url":"about:blank"})";
                     (void)server.dispatch_for_test(open_req);

                     b::BrowserHttpRequest read_req;
                     read_req.method = "GET";
                     read_req.path = "/read";
                     const auto resp = server.dispatch_for_test(read_req);
                     require(resp.status == 200, "read should succeed");
                     require(resp.body.find("\"status\":\"ok\"") != std::string::npos,
                             "read response should contain status ok");

                     // Verify the read action was called
                     bool found_read = false;
                     for (const auto &action : actions.seen) {
                       if (action.action == "read") {
                         found_read = true;
                       }
                     }
                     require(found_read, "read action should be executed");
                   }});
}
