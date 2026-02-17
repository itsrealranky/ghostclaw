#include "ghostclaw/browser/actions.hpp"

#include "ghostclaw/browser/element.hpp"
#include "ghostclaw/browser/readability.hpp"
#include "ghostclaw/common/fs.hpp"

#include <optional>

namespace ghostclaw::browser {

namespace {

common::Result<BrowserActionResult> ok_result(JsonMap data = {}) {
  BrowserActionResult result;
  result.success = true;
  result.data = std::move(data);
  return common::Result<BrowserActionResult>::success(std::move(result));
}

common::Result<BrowserActionResult> error_result(const std::string &message) {
  return common::Result<BrowserActionResult>::failure(message);
}

} // namespace

BrowserActions::BrowserActions(CDPClient &client) : client_(client) {}

common::Result<BrowserActionResult> BrowserActions::execute(const BrowserAction &action) {
  const std::string name = common::to_lower(common::trim(action.action));
  if (name.empty()) {
    return error_result("action is required");
  }

  if (name == "navigate") {
    return action_navigate(action);
  }
  if (name == "click") {
    return action_click(action);
  }
  if (name == "type") {
    return action_type(action);
  }
  if (name == "fill") {
    return action_fill(action);
  }
  if (name == "press") {
    return action_press(action);
  }
  if (name == "hover") {
    return action_hover(action);
  }
  if (name == "drag") {
    return action_drag(action);
  }
  if (name == "select") {
    return action_select(action);
  }
  if (name == "scroll") {
    return action_scroll(action);
  }
  if (name == "screenshot") {
    return action_screenshot(action);
  }
  if (name == "snapshot") {
    return action_snapshot(action);
  }
  if (name == "pdf") {
    return action_pdf(action);
  }
  if (name == "evaluate") {
    return action_evaluate(action);
  }
  if (name == "read") {
    return action_read(action);
  }
  return error_result("unsupported browser action: " + name);
}

common::Result<std::vector<BrowserActionResult>>
BrowserActions::execute_batch(const std::vector<BrowserAction> &actions) {
  if (actions.empty()) {
    return common::Result<std::vector<BrowserActionResult>>::failure(
        "actions list is empty");
  }
  std::vector<BrowserActionResult> out;
  out.reserve(actions.size());
  for (const auto &action : actions) {
    auto result = execute(action);
    if (result.ok()) {
      out.push_back(result.value());
    } else {
      BrowserActionResult failure;
      failure.success = false;
      failure.error = result.error();
      out.push_back(std::move(failure));
    }
  }
  return common::Result<std::vector<BrowserActionResult>>::success(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_navigate(const BrowserAction &action) {
  const std::string url = param_or_empty(action, "url");
  if (url.empty()) {
    return error_result("navigate requires url");
  }
  auto response = client_.send_command("Page.navigate", {{"url", url}});
  if (!response.ok()) {
    return error_result(response.error());
  }
  // Clear ref cache after navigation since elements are stale
  ref_cache_.clear();
  prev_snapshots_.clear();
  JsonMap out = response.value();
  out["url"] = url;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_click(const BrowserAction &action) {
  // Check for ref-based targeting first
  const std::string ref = param_or_empty(action, "ref");
  if (!ref.empty()) {
    auto node_id = ref_cache_.resolve(ref);
    if (!node_id) {
      return error_result("ref not found: " + ref + " — run snapshot first");
    }
    ElementResolver resolver(client_);
    auto result = resolver.click_by_node_id(*node_id);
    if (!result.ok()) {
      return error_result(result.error());
    }
    JsonMap out;
    out["ref"] = ref;
    out["status"] = "clicked";
    return ok_result(std::move(out));
  }

  std::string selector = param_or_empty(action, "selector");
  if (selector.empty()) {
    return error_result("click requires selector or ref");
  }
  const std::string js = "(function(){const el=document.querySelector('" +
                         escape_js_string(selector) +
                         "');if(!el){throw new Error('selector_not_found');}"
                         "el.click();return 'ok';})()";
  auto response = client_.evaluate_js(js);
  if (!response.ok()) {
    return error_result(response.error());
  }
  JsonMap out = response.value();
  out["selector"] = selector;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_type(const BrowserAction &action) {
  const std::string text = param_or_empty(action, "text");
  if (text.empty()) {
    return error_result("type requires text");
  }

  // Check for ref-based targeting
  const std::string ref = param_or_empty(action, "ref");
  if (!ref.empty()) {
    auto node_id = ref_cache_.resolve(ref);
    if (!node_id) {
      return error_result("ref not found: " + ref + " — run snapshot first");
    }
    ElementResolver resolver(client_);
    auto result = resolver.type_by_node_id(*node_id, text);
    if (!result.ok()) {
      return error_result(result.error());
    }
    JsonMap out;
    out["ref"] = ref;
    out["text"] = text;
    out["status"] = "typed";
    return ok_result(std::move(out));
  }

  const std::string selector = param_or_empty(action, "selector");
  std::string js;
  if (!selector.empty()) {
    js = "(function(){const el=document.querySelector('" + escape_js_string(selector) +
         "');if(!el){throw new Error('selector_not_found');}el.focus();"
         "el.value=(el.value||'')+'" +
         escape_js_string(text) +
         "';el.dispatchEvent(new Event('input',{bubbles:true}));return 'ok';})()";
  } else {
    js = "(function(){const el=document.activeElement;if(!el){throw new Error('no_active_element');}"
         "el.value=(el.value||'')+'" +
         escape_js_string(text) +
         "';el.dispatchEvent(new Event('input',{bubbles:true}));return 'ok';})()";
  }
  auto response = client_.evaluate_js(js);
  if (!response.ok()) {
    return error_result(response.error());
  }
  JsonMap out = response.value();
  out["text"] = text;
  if (!selector.empty()) {
    out["selector"] = selector;
  }
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_fill(const BrowserAction &action) {
  // Check for ref-based targeting
  const std::string ref = param_or_empty(action, "ref");
  if (!ref.empty()) {
    auto node_id = ref_cache_.resolve(ref);
    if (!node_id) {
      return error_result("ref not found: " + ref + " — run snapshot first");
    }
    const std::string value = param_or_empty(action, "value").empty()
                                  ? param_or_empty(action, "text")
                                  : param_or_empty(action, "value");
    ElementResolver resolver(client_);
    auto result = resolver.fill_by_node_id(*node_id, value);
    if (!result.ok()) {
      return error_result(result.error());
    }
    JsonMap out;
    out["ref"] = ref;
    out["status"] = "filled";
    return ok_result(std::move(out));
  }

  const std::string selector = param_or_empty(action, "selector");
  if (selector.empty()) {
    return error_result("fill requires selector or ref");
  }
  const std::string value = param_or_empty(action, "value").empty()
                                ? param_or_empty(action, "text")
                                : param_or_empty(action, "value");
  std::string js = "(function(){const el=document.querySelector('" +
                   escape_js_string(selector) +
                   "');if(!el){throw new Error('selector_not_found');}"
                   "el.focus();el.value='" +
                   escape_js_string(value) +
                   "';el.dispatchEvent(new Event('input',{bubbles:true}));"
                   "el.dispatchEvent(new Event('change',{bubbles:true}));return 'ok';})()";
  auto response = client_.evaluate_js(js);
  if (!response.ok()) {
    return error_result(response.error());
  }
  JsonMap out = response.value();
  out["selector"] = selector;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_press(const BrowserAction &action) {
  const std::string key = param_or_empty(action, "key");
  if (key.empty()) {
    return error_result("press requires key");
  }
  auto down = client_.send_command("Input.dispatchKeyEvent",
                                   {{"type", "keyDown"}, {"key", key}, {"text", key}});
  if (!down.ok()) {
    return error_result(down.error());
  }
  auto up = client_.send_command("Input.dispatchKeyEvent",
                                 {{"type", "keyUp"}, {"key", key}});
  if (!up.ok()) {
    return error_result(up.error());
  }
  JsonMap out;
  out["key"] = key;
  out["status"] = "ok";
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_hover(const BrowserAction &action) {
  // Check for ref-based targeting
  const std::string ref = param_or_empty(action, "ref");
  if (!ref.empty()) {
    auto node_id = ref_cache_.resolve(ref);
    if (!node_id) {
      return error_result("ref not found: " + ref + " — run snapshot first");
    }
    ElementResolver resolver(client_);
    auto result = resolver.hover_by_node_id(*node_id);
    if (!result.ok()) {
      return error_result(result.error());
    }
    JsonMap out;
    out["ref"] = ref;
    out["status"] = "hovered";
    return ok_result(std::move(out));
  }

  const std::string selector = param_or_empty(action, "selector");
  if (selector.empty()) {
    return error_result("hover requires selector or ref");
  }
  const std::string js = "(function(){const el=document.querySelector('" +
                         escape_js_string(selector) +
                         "');if(!el){throw new Error('selector_not_found');}"
                         "el.dispatchEvent(new MouseEvent('mouseover',{bubbles:true}));"
                         "return 'ok';})()";
  auto response = client_.evaluate_js(js);
  if (!response.ok()) {
    return error_result(response.error());
  }
  JsonMap out = response.value();
  out["selector"] = selector;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_drag(const BrowserAction &action) {
  const std::string from = param_or_empty(action, "from");
  const std::string to = param_or_empty(action, "to");
  if (from.empty() || to.empty()) {
    return error_result("drag requires from and to selectors");
  }
  const std::string js = "(function(){const src=document.querySelector('" +
                         escape_js_string(from) + "');const dst=document.querySelector('" +
                         escape_js_string(to) +
                         "');if(!src||!dst){throw new Error('selector_not_found');}"
                         "const dt=new DataTransfer();"
                         "src.dispatchEvent(new DragEvent('dragstart',{dataTransfer:dt,bubbles:true}));"
                         "dst.dispatchEvent(new DragEvent('dragover',{dataTransfer:dt,bubbles:true}));"
                         "dst.dispatchEvent(new DragEvent('drop',{dataTransfer:dt,bubbles:true}));"
                         "src.dispatchEvent(new DragEvent('dragend',{dataTransfer:dt,bubbles:true}));"
                         "return 'ok';})()";
  auto response = client_.evaluate_js(js);
  if (!response.ok()) {
    return error_result(response.error());
  }
  JsonMap out = response.value();
  out["from"] = from;
  out["to"] = to;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_select(const BrowserAction &action) {
  // Check for ref-based targeting
  const std::string ref = param_or_empty(action, "ref");
  if (!ref.empty()) {
    auto node_id = ref_cache_.resolve(ref);
    if (!node_id) {
      return error_result("ref not found: " + ref + " — run snapshot first");
    }
    const std::string value = param_or_empty(action, "value");
    if (value.empty()) {
      return error_result("select requires value");
    }
    ElementResolver resolver(client_);
    auto result = resolver.select_by_node_id(*node_id, value);
    if (!result.ok()) {
      return error_result(result.error());
    }
    JsonMap out;
    out["ref"] = ref;
    out["value"] = value;
    out["status"] = "selected";
    return ok_result(std::move(out));
  }

  const std::string selector = param_or_empty(action, "selector");
  const std::string value = param_or_empty(action, "value");
  if (selector.empty() || value.empty()) {
    return error_result("select requires selector and value");
  }
  const std::string js = "(function(){const el=document.querySelector('" +
                         escape_js_string(selector) +
                         "');if(!el){throw new Error('selector_not_found');}"
                         "el.value='" +
                         escape_js_string(value) +
                         "';el.dispatchEvent(new Event('change',{bubbles:true}));return 'ok';})()";
  auto response = client_.evaluate_js(js);
  if (!response.ok()) {
    return error_result(response.error());
  }
  JsonMap out = response.value();
  out["selector"] = selector;
  out["value"] = value;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_scroll(const BrowserAction &action) {
  // Check for ref-based targeting — scroll element into view
  const std::string ref = param_or_empty(action, "ref");
  if (!ref.empty()) {
    auto node_id = ref_cache_.resolve(ref);
    if (!node_id) {
      return error_result("ref not found: " + ref + " — run snapshot first");
    }
    ElementResolver resolver(client_);
    auto result = resolver.scroll_into_view(*node_id);
    if (!result.ok()) {
      return error_result(result.error());
    }
    JsonMap out;
    out["ref"] = ref;
    out["status"] = "scrolled";
    return ok_result(std::move(out));
  }

  const auto x = parse_double_param(action, "x").value_or(0.0);
  const auto y = parse_double_param(action, "y").value_or(500.0);
  const std::string js = "window.scrollBy(" + std::to_string(x) + "," + std::to_string(y) +
                         ");'ok'";
  auto response = client_.evaluate_js(js);
  if (!response.ok()) {
    return error_result(response.error());
  }
  JsonMap out = response.value();
  out["x"] = std::to_string(x);
  out["y"] = std::to_string(y);
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_screenshot(const BrowserAction &action) {
  const std::string format = param_or_empty(action, "format");
  common::Result<std::string> screenshot = format.empty()
                                               ? client_.capture_screenshot()
                                               : common::Result<std::string>::failure("");
  if (!format.empty()) {
    auto response = client_.send_command("Page.captureScreenshot", {{"format", format}});
    if (!response.ok()) {
      return error_result(response.error());
    }
    const auto data_it = response.value().find("data");
    if (data_it == response.value().end()) {
      return error_result("screenshot data missing");
    }
    screenshot = common::Result<std::string>::success(data_it->second);
  }
  if (!screenshot.ok()) {
    return error_result(screenshot.error());
  }
  JsonMap out;
  out["data"] = screenshot.value();
  out["format"] = format.empty() ? "png" : format;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_snapshot(const BrowserAction &action) {
  auto response = client_.get_accessibility_tree();
  if (!response.ok()) {
    return error_result(response.error());
  }

  // Extract the raw nodes JSON from the response
  auto nodes_it = response.value().find("nodes");
  if (nodes_it == response.value().end()) {
    return error_result("accessibility tree missing nodes");
  }
  const std::string &raw_nodes = nodes_it->second;

  // Parse the tree
  auto parsed = a11y_parser_.parse_tree(raw_nodes);
  if (!parsed.ok()) {
    return error_result("failed to parse a11y tree: " + parsed.error());
  }
  auto nodes = std::move(parsed.value());

  // Apply filter if requested
  const std::string filter = param_or_empty(action, "filter");
  if (filter == "interactive") {
    nodes = a11y_parser_.filter_interactive(nodes);
  }

  // Apply depth filter if requested
  const auto depth = parse_double_param(action, "depth");
  if (depth.has_value() && *depth > 0) {
    nodes = a11y_parser_.filter_depth(nodes, static_cast<int>(*depth));
  }

  // Populate ref cache for subsequent ref-based actions
  ref_cache_.populate(nodes);

  // Check for diff mode
  const std::string tab_id = param_or_empty(action, "tab_id");
  const std::string diff_flag = param_or_empty(action, "diff");

  if (diff_flag == "true" && prev_snapshots_.contains(tab_id)) {
    auto diff = a11y_parser_.compute_diff(prev_snapshots_[tab_id], nodes);
    prev_snapshots_[tab_id] = nodes;

    // Format diff output
    std::string diff_text;
    if (!diff.added.empty()) {
      diff_text += "=== Added ===\n" + a11y_parser_.format_text(diff.added);
    }
    if (!diff.removed.empty()) {
      diff_text += "=== Removed ===\n" + a11y_parser_.format_text(diff.removed);
    }
    if (!diff.changed.empty()) {
      diff_text += "=== Changed ===\n" + a11y_parser_.format_text(diff.changed);
    }
    if (diff_text.empty()) {
      diff_text = "(no changes)";
    }

    JsonMap out;
    out["diff"] = diff_text;
    out["added_count"] = std::to_string(diff.added.size());
    out["removed_count"] = std::to_string(diff.removed.size());
    out["changed_count"] = std::to_string(diff.changed.size());
    return ok_result(std::move(out));
  }

  // Cache for future diffs
  prev_snapshots_[tab_id] = nodes;

  // Format output
  const std::string format = param_or_empty(action, "format");
  JsonMap out;
  if (format == "json") {
    out["snapshot"] = a11y_parser_.format_json(nodes);
  } else {
    out["snapshot"] = a11y_parser_.format_text(nodes);
  }
  out["node_count"] = std::to_string(nodes.size());
  out["ref_count"] = std::to_string(ref_cache_.size());
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_pdf(const BrowserAction &action) {
  JsonMap params;
  const std::string landscape = param_or_empty(action, "landscape");
  if (!landscape.empty()) {
    params["landscape"] = landscape;
  }
  params["printBackground"] = "true";
  auto response = client_.send_command("Page.printToPDF", params);
  if (!response.ok()) {
    return error_result(response.error());
  }
  const auto data_it = response.value().find("data");
  if (data_it == response.value().end()) {
    return error_result("pdf data missing");
  }
  JsonMap out;
  out["data"] = data_it->second;
  return ok_result(std::move(out));
}

common::Result<BrowserActionResult>
BrowserActions::action_evaluate(const BrowserAction &action) {
  const std::string expression = param_or_empty(action, "expression");
  if (expression.empty()) {
    return error_result("evaluate requires expression");
  }
  auto response = client_.evaluate_js(expression);
  if (!response.ok()) {
    return error_result(response.error());
  }
  return ok_result(response.value());
}

common::Result<BrowserActionResult>
BrowserActions::action_read(const BrowserAction &) {
  auto text = ReadabilityExtractor::extract(client_);
  if (!text.ok()) {
    return error_result(text.error());
  }
  JsonMap out;
  out["text"] = text.value();
  return ok_result(std::move(out));
}

std::string BrowserActions::escape_js_string(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
    case '\'':
      out += "\\'";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out.push_back(ch);
      break;
    }
  }
  return out;
}

std::string BrowserActions::param_or_empty(const BrowserAction &action,
                                           const std::string &key) {
  const auto it = action.params.find(key);
  if (it == action.params.end()) {
    return "";
  }
  return it->second;
}

std::optional<double> BrowserActions::parse_double_param(const BrowserAction &action,
                                                         const std::string &key) {
  const auto it = action.params.find(key);
  if (it == action.params.end()) {
    return std::nullopt;
  }
  try {
    return std::stod(common::trim(it->second));
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace ghostclaw::browser
