#pragma once

#include "ghostclaw/browser/a11y.hpp"
#include "ghostclaw/browser/cdp.hpp"
#include "ghostclaw/common/result.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ghostclaw::browser {

struct BrowserAction {
  std::string action;
  JsonMap params;
};

struct BrowserActionResult {
  bool success = false;
  JsonMap data;
  std::string error;
};

class IBrowserActions {
public:
  virtual ~IBrowserActions() = default;
  [[nodiscard]] virtual common::Result<BrowserActionResult>
  execute(const BrowserAction &action) = 0;
  [[nodiscard]] virtual common::Result<std::vector<BrowserActionResult>>
  execute_batch(const std::vector<BrowserAction> &actions) = 0;
};

class BrowserActions final : public IBrowserActions {
public:
  explicit BrowserActions(CDPClient &client);

  [[nodiscard]] common::Result<BrowserActionResult>
  execute(const BrowserAction &action) override;
  [[nodiscard]] common::Result<std::vector<BrowserActionResult>>
  execute_batch(const std::vector<BrowserAction> &actions) override;

private:
  [[nodiscard]] common::Result<BrowserActionResult> action_navigate(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_click(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_type(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_fill(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_press(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_hover(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_drag(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_select(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_scroll(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_screenshot(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_snapshot(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_pdf(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_evaluate(const BrowserAction &action);
  [[nodiscard]] common::Result<BrowserActionResult> action_read(const BrowserAction &action);

  [[nodiscard]] static std::string escape_js_string(const std::string &value);
  [[nodiscard]] static std::string param_or_empty(const BrowserAction &action,
                                                  const std::string &key);
  [[nodiscard]] static std::optional<double> parse_double_param(const BrowserAction &action,
                                                                const std::string &key);

  CDPClient &client_;
  RefCache ref_cache_;
  A11yParser a11y_parser_;
  std::unordered_map<std::string, std::vector<A11yNode>> prev_snapshots_;
};

} // namespace ghostclaw::browser
