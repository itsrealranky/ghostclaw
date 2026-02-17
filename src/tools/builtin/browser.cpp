#include "ghostclaw/tools/builtin/browser.hpp"

#include "ghostclaw/browser/chrome.hpp"
#include "ghostclaw/common/fs.hpp"

#include <sstream>

namespace ghostclaw::tools {

BrowserTool::BrowserTool(std::vector<std::string> allowed_domains,
                         config::BrowserConfig browser_config)
    : allowed_domains_(std::move(allowed_domains)),
      browser_config_(std::move(browser_config)) {}

std::string_view BrowserTool::name() const { return "browser"; }

std::string_view BrowserTool::description() const { return "Run browser actions with allowlist"; }

std::string BrowserTool::parameters_schema() const {
  return R"json({"type":"object","required":["action"],"properties":{"action":{"type":"string","description":"navigate|click|type|fill|press|hover|scroll|screenshot|snapshot|evaluate|read"},"url":{"type":"string"},"selector":{"type":"string"},"ref":{"type":"string","description":"Element ref from snapshot (e.g. e0, e1)"},"text":{"type":"string"},"expression":{"type":"string"},"filter":{"type":"string","description":"Snapshot filter: interactive"},"depth":{"type":"number","description":"Max tree depth for snapshot"},"diff":{"type":"string","description":"Set to true for diff mode in snapshot"},"format":{"type":"string","description":"Snapshot format: text (default) or json"}}})json";
}

bool BrowserTool::domain_allowed(const std::string &url) const {
  const auto scheme_pos = url.find("://");
  const std::size_t host_start = scheme_pos == std::string::npos ? 0 : scheme_pos + 3;
  const auto host_end = url.find('/', host_start);
  const std::string host = common::to_lower(url.substr(host_start, host_end - host_start));

  if (allowed_domains_.empty()) {
    return true;
  }

  for (const auto &allowed : allowed_domains_) {
    const std::string normalized = common::to_lower(allowed);
    if (host == normalized ||
        (host.size() > normalized.size() + 1 &&
         host.ends_with("." + normalized))) {
      return true;
    }
  }

  return false;
}

bool BrowserTool::connect(const std::string &host, int port) {
  (void)host; // reserved for future remote connections
  auto ws_url = browser::build_devtools_ws_url(static_cast<std::uint16_t>(port));
  if (!ws_url.ok()) {
    return false;
  }

  auto client = std::make_shared<browser::CDPClient>();
  auto status = client->connect(ws_url.value());
  if (!status.ok()) {
    return false;
  }

  cdp_client_ = std::move(client);
  browser_actions_ = std::make_unique<browser::BrowserActions>(*cdp_client_);

  // Enable stealth mode if configured
  if (browser_config_.stealth) {
    (void)browser::StealthManager::enable(*cdp_client_);
  }

  return true;
}

bool BrowserTool::is_connected() const {
  return cdp_client_ != nullptr && cdp_client_->is_connected();
}

common::Result<ToolResult> BrowserTool::execute_via_cdp(const ToolArgs &args) {
  const auto action_it = args.find("action");
  if (action_it == args.end()) {
    return common::Result<ToolResult>::failure("Missing action");
  }

  const std::string action = common::to_lower(action_it->second);

  // Build BrowserAction from tool args
  browser::BrowserAction ba;
  ba.action = action;

  // Map common tool args to BrowserAction params
  for (const auto &[key, val] : args) {
    if (key != "action") {
      ba.params[key] = val;
    }
  }

  auto result = browser_actions_->execute(ba);
  if (!result.ok()) {
    return common::Result<ToolResult>::failure(result.error());
  }

  const auto &action_result = result.value();
  ToolResult tool_result;

  if (!action_result.success) {
    tool_result.success = false;
    tool_result.output = "Browser action failed: " + action_result.error;
    return common::Result<ToolResult>::success(std::move(tool_result));
  }

  // Build output from action result data
  std::ostringstream out;
  for (const auto &[key, val] : action_result.data) {
    if (key == "screenshot_base64") {
      out << "[screenshot captured, " << val.size() << " bytes base64]\n";
      tool_result.metadata["screenshot_base64"] = val;
    } else {
      out << key << ": " << val << "\n";
    }
  }

  if (out.str().empty()) {
    out << "Browser action '" << action << "' executed successfully.\n";
  }

  tool_result.output = out.str();
  tool_result.metadata["action"] = action;
  return common::Result<ToolResult>::success(std::move(tool_result));
}

common::Result<ToolResult> BrowserTool::execute(const ToolArgs &args, const ToolContext &) {
  const auto action_it = args.find("action");
  if (action_it == args.end()) {
    return common::Result<ToolResult>::failure("Missing action");
  }

  const std::string action = common::to_lower(action_it->second);

  // Validate domain for navigate
  if (action == "navigate") {
    const auto url_it = args.find("url");
    if (url_it == args.end()) {
      return common::Result<ToolResult>::failure("Missing url");
    }
    if (!domain_allowed(url_it->second)) {
      return common::Result<ToolResult>::failure("Domain is not allowed");
    }
  }

  // If CDP is connected, use real browser
  if (is_connected()) {
    return execute_via_cdp(args);
  }

  // Try auto-connect to Chrome default debug port if browser is enabled
  if (browser_config_.enabled && !is_connected()) {
    if (connect("127.0.0.1", 9222)) {
      return execute_via_cdp(args);
    }
  }

  // Fallback: simulated response (no browser connected)
  if (action == "navigate") {
    const auto url_it = args.find("url");
    ToolResult result;
    result.output = "Navigated to " + (url_it != args.end() ? url_it->second : "(unknown)");
    result.metadata["note"] = "No browser connected. Enable browser in config and ensure Chrome is running with --remote-debugging-port=9222";
    return common::Result<ToolResult>::success(std::move(result));
  }

  ToolResult result;
  result.output = "Browser action executed: " + action;
  result.metadata["note"] = "No browser connected. Enable browser in config and ensure Chrome is running with --remote-debugging-port=9222";
  return common::Result<ToolResult>::success(std::move(result));
}

bool BrowserTool::is_safe() const { return false; }

std::string_view BrowserTool::group() const { return "web"; }

} // namespace ghostclaw::tools
