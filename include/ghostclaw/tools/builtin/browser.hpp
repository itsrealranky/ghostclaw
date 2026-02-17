#pragma once

#include "ghostclaw/browser/actions.hpp"
#include "ghostclaw/browser/cdp.hpp"
#include "ghostclaw/browser/stealth.hpp"
#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/tools/tool.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ghostclaw::tools {

class BrowserTool final : public ITool {
public:
  explicit BrowserTool(std::vector<std::string> allowed_domains,
                       config::BrowserConfig browser_config = {});

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                   const ToolContext &ctx) override;

  [[nodiscard]] bool is_safe() const override;
  [[nodiscard]] std::string_view group() const override;

  /// Attempt to connect to a running Chrome DevTools instance.
  [[nodiscard]] bool connect(const std::string &host = "127.0.0.1", int port = 9222);

  /// Check if CDP client is connected.
  [[nodiscard]] bool is_connected() const;

private:
  [[nodiscard]] bool domain_allowed(const std::string &url) const;
  [[nodiscard]] common::Result<ToolResult> execute_via_cdp(const ToolArgs &args);

  std::vector<std::string> allowed_domains_;
  config::BrowserConfig browser_config_;
  std::shared_ptr<browser::CDPClient> cdp_client_;
  std::unique_ptr<browser::BrowserActions> browser_actions_;
};

} // namespace ghostclaw::tools
