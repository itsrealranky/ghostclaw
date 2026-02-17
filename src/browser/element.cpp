#include "ghostclaw/browser/element.hpp"

#include "ghostclaw/common/json_util.hpp"

namespace ghostclaw::browser {

ElementResolver::ElementResolver(CDPClient &client) : client_(client) {}

std::string ElementResolver::escape_js(const std::string &value) {
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

// ---------------------------------------------------------------------------
// resolve_node_to_object
// ---------------------------------------------------------------------------

common::Result<std::string>
ElementResolver::resolve_node_to_object(std::int64_t backend_node_id) {
  auto result = client_.send_command(
      "DOM.resolveNode",
      {{"backendNodeId", std::to_string(backend_node_id)}});
  if (!result.ok()) {
    return common::Result<std::string>::failure(
        "DOM.resolveNode failed: " + result.error());
  }

  // The result map stores nested objects as raw JSON strings.
  // Extract the "object" field, then get "objectId" from it.
  auto obj_it = result.value().find("object");
  if (obj_it == result.value().end()) {
    return common::Result<std::string>::failure(
        "DOM.resolveNode: no object in result");
  }

  std::string object_id = common::json_get_string(obj_it->second, "objectId");
  if (object_id.empty()) {
    return common::Result<std::string>::failure(
        "DOM.resolveNode: no objectId in result");
  }

  return common::Result<std::string>::success(std::move(object_id));
}

// ---------------------------------------------------------------------------
// call_function_on
// ---------------------------------------------------------------------------

common::Result<JsonMap>
ElementResolver::call_function_on(const std::string &object_id,
                                  const std::string &js_function) {
  auto result = client_.send_command(
      "Runtime.callFunctionOn",
      {{"objectId", object_id},
       {"functionDeclaration", js_function},
       {"returnByValue", "true"}});
  if (!result.ok()) {
    return common::Result<JsonMap>::failure(
        "Runtime.callFunctionOn failed: " + result.error());
  }
  return common::Result<JsonMap>::success(std::move(result.value()));
}

// ---------------------------------------------------------------------------
// click_by_node_id
// ---------------------------------------------------------------------------

common::Result<JsonMap> ElementResolver::click_by_node_id(std::int64_t id) {
  auto oid = resolve_node_to_object(id);
  if (!oid.ok()) {
    return common::Result<JsonMap>::failure(oid.error());
  }

  // scrollIntoViewIfNeeded + click
  auto result = call_function_on(
      oid.value(),
      "function(){this.scrollIntoViewIfNeeded();this.click();return 'ok';}");
  if (!result.ok()) {
    return common::Result<JsonMap>::failure(result.error());
  }

  JsonMap out;
  out["status"] = "clicked";
  return common::Result<JsonMap>::success(std::move(out));
}

// ---------------------------------------------------------------------------
// type_by_node_id
// ---------------------------------------------------------------------------

common::Result<JsonMap>
ElementResolver::type_by_node_id(std::int64_t id, const std::string &text) {
  auto oid = resolve_node_to_object(id);
  if (!oid.ok()) {
    return common::Result<JsonMap>::failure(oid.error());
  }

  // Scroll into view
  auto scroll = call_function_on(
      oid.value(),
      "function(){this.scrollIntoViewIfNeeded();return 'ok';}");
  if (!scroll.ok()) {
    return common::Result<JsonMap>::failure(scroll.error());
  }

  // Focus via DOM.focus
  auto focus = client_.send_command(
      "DOM.focus",
      {{"backendNodeId", std::to_string(id)}});
  if (!focus.ok()) {
    return common::Result<JsonMap>::failure("DOM.focus failed: " + focus.error());
  }

  // Dispatch key events for each character
  for (const char ch : text) {
    std::string key(1, ch);
    auto down = client_.send_command(
        "Input.dispatchKeyEvent",
        {{"type", "keyDown"}, {"key", key}, {"text", key}});
    if (!down.ok()) {
      return common::Result<JsonMap>::failure(down.error());
    }
    auto up = client_.send_command(
        "Input.dispatchKeyEvent",
        {{"type", "keyUp"}, {"key", key}});
    if (!up.ok()) {
      return common::Result<JsonMap>::failure(up.error());
    }
  }

  JsonMap out;
  out["status"] = "typed";
  out["text"] = text;
  return common::Result<JsonMap>::success(std::move(out));
}

// ---------------------------------------------------------------------------
// fill_by_node_id
// ---------------------------------------------------------------------------

common::Result<JsonMap>
ElementResolver::fill_by_node_id(std::int64_t id, const std::string &value) {
  auto oid = resolve_node_to_object(id);
  if (!oid.ok()) {
    return common::Result<JsonMap>::failure(oid.error());
  }

  auto result = call_function_on(
      oid.value(),
      "function(){this.scrollIntoViewIfNeeded();this.focus();"
      "this.value='" + escape_js(value) + "';"
      "this.dispatchEvent(new Event('input',{bubbles:true}));"
      "this.dispatchEvent(new Event('change',{bubbles:true}));"
      "return 'ok';}");
  if (!result.ok()) {
    return common::Result<JsonMap>::failure(result.error());
  }

  JsonMap out;
  out["status"] = "filled";
  out["value"] = value;
  return common::Result<JsonMap>::success(std::move(out));
}

// ---------------------------------------------------------------------------
// hover_by_node_id
// ---------------------------------------------------------------------------

common::Result<JsonMap> ElementResolver::hover_by_node_id(std::int64_t id) {
  auto oid = resolve_node_to_object(id);
  if (!oid.ok()) {
    return common::Result<JsonMap>::failure(oid.error());
  }

  auto result = call_function_on(
      oid.value(),
      "function(){this.scrollIntoViewIfNeeded();"
      "this.dispatchEvent(new MouseEvent('mouseover',{bubbles:true}));"
      "this.dispatchEvent(new MouseEvent('mouseenter',{bubbles:false}));"
      "return 'ok';}");
  if (!result.ok()) {
    return common::Result<JsonMap>::failure(result.error());
  }

  JsonMap out;
  out["status"] = "hovered";
  return common::Result<JsonMap>::success(std::move(out));
}

// ---------------------------------------------------------------------------
// select_by_node_id
// ---------------------------------------------------------------------------

common::Result<JsonMap>
ElementResolver::select_by_node_id(std::int64_t id, const std::string &value) {
  auto oid = resolve_node_to_object(id);
  if (!oid.ok()) {
    return common::Result<JsonMap>::failure(oid.error());
  }

  auto result = call_function_on(
      oid.value(),
      "function(){this.scrollIntoViewIfNeeded();"
      "this.value='" + escape_js(value) + "';"
      "this.dispatchEvent(new Event('change',{bubbles:true}));"
      "return 'ok';}");
  if (!result.ok()) {
    return common::Result<JsonMap>::failure(result.error());
  }

  JsonMap out;
  out["status"] = "selected";
  out["value"] = value;
  return common::Result<JsonMap>::success(std::move(out));
}

// ---------------------------------------------------------------------------
// focus_by_node_id
// ---------------------------------------------------------------------------

common::Result<JsonMap> ElementResolver::focus_by_node_id(std::int64_t id) {
  auto oid = resolve_node_to_object(id);
  if (!oid.ok()) {
    return common::Result<JsonMap>::failure(oid.error());
  }

  auto scroll = call_function_on(
      oid.value(),
      "function(){this.scrollIntoViewIfNeeded();return 'ok';}");
  if (!scroll.ok()) {
    return common::Result<JsonMap>::failure(scroll.error());
  }

  auto focus = client_.send_command(
      "DOM.focus",
      {{"backendNodeId", std::to_string(id)}});
  if (!focus.ok()) {
    return common::Result<JsonMap>::failure("DOM.focus failed: " + focus.error());
  }

  JsonMap out;
  out["status"] = "focused";
  return common::Result<JsonMap>::success(std::move(out));
}

// ---------------------------------------------------------------------------
// scroll_into_view
// ---------------------------------------------------------------------------

common::Result<JsonMap> ElementResolver::scroll_into_view(std::int64_t id) {
  auto oid = resolve_node_to_object(id);
  if (!oid.ok()) {
    return common::Result<JsonMap>::failure(oid.error());
  }

  auto result = call_function_on(
      oid.value(),
      "function(){this.scrollIntoView({block:'center',inline:'center'});return 'ok';}");
  if (!result.ok()) {
    return common::Result<JsonMap>::failure(result.error());
  }

  JsonMap out;
  out["status"] = "scrolled";
  return common::Result<JsonMap>::success(std::move(out));
}

} // namespace ghostclaw::browser
