#pragma once

#include "ghostclaw/browser/cdp.hpp"
#include "ghostclaw/common/result.hpp"

#include <cstdint>
#include <string>

namespace ghostclaw::browser {

class ElementResolver {
public:
  explicit ElementResolver(CDPClient &client);

  /// Resolve a backend node ID to a remote object ID.
  [[nodiscard]] common::Result<std::string>
  resolve_node_to_object(std::int64_t backend_node_id);

  /// Call a JavaScript function on a remote object.
  [[nodiscard]] common::Result<JsonMap>
  call_function_on(const std::string &object_id, const std::string &js_function);

  /// Click an element by its backend node ID.
  [[nodiscard]] common::Result<JsonMap> click_by_node_id(std::int64_t id);

  /// Type text into an element by its backend node ID (appends to existing value).
  [[nodiscard]] common::Result<JsonMap> type_by_node_id(std::int64_t id,
                                                        const std::string &text);

  /// Fill an element by its backend node ID (replaces value).
  [[nodiscard]] common::Result<JsonMap> fill_by_node_id(std::int64_t id,
                                                        const std::string &value);

  /// Hover over an element by its backend node ID.
  [[nodiscard]] common::Result<JsonMap> hover_by_node_id(std::int64_t id);

  /// Select a value in a dropdown by its backend node ID.
  [[nodiscard]] common::Result<JsonMap>
  select_by_node_id(std::int64_t id, const std::string &value);

  /// Focus an element by its backend node ID.
  [[nodiscard]] common::Result<JsonMap> focus_by_node_id(std::int64_t id);

  /// Scroll an element into view by its backend node ID.
  [[nodiscard]] common::Result<JsonMap> scroll_into_view(std::int64_t id);

private:
  [[nodiscard]] static std::string escape_js(const std::string &value);
  CDPClient &client_;
};

} // namespace ghostclaw::browser
