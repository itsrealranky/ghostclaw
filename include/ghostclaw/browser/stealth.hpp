#pragma once

#include "ghostclaw/browser/cdp.hpp"
#include "ghostclaw/common/result.hpp"

#include <string>

namespace ghostclaw::browser {

class StealthManager {
public:
  /// Get the stealth JavaScript snippet that patches common detection vectors.
  [[nodiscard]] static const std::string &stealth_script();

  /// Inject the stealth script to run before any page script on every navigation.
  [[nodiscard]] static common::Status enable(CDPClient &client);
};

} // namespace ghostclaw::browser
