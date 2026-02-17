#pragma once

#include "ghostclaw/browser/cdp.hpp"
#include "ghostclaw/common/result.hpp"

#include <string>

namespace ghostclaw::browser {

class ReadabilityExtractor {
public:
  /// Get the extraction JavaScript IIFE.
  [[nodiscard]] static const std::string &extraction_script();

  /// Run the readability extraction on the current page and return clean text.
  [[nodiscard]] static common::Result<std::string> extract(CDPClient &client);
};

} // namespace ghostclaw::browser
