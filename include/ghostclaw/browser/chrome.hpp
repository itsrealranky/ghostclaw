#pragma once

#include "ghostclaw/browser/profiles.hpp"
#include "ghostclaw/common/result.hpp"

#include <string>
#include <vector>

namespace ghostclaw::browser {

struct ChromeLaunchOptions {
  BrowserProfile profile;
  std::string start_url = "about:blank";
  bool headless = false;
  bool disable_gpu = true;
  bool no_first_run = true;
  bool no_default_browser_check = true;
  bool stealth = false;
};

[[nodiscard]] common::Result<std::vector<std::string>>
build_chrome_launch_args(const ChromeLaunchOptions &options);

[[nodiscard]] common::Result<std::string>
build_devtools_ws_url(std::uint16_t port, const std::string &target_path = "/devtools/browser");

} // namespace ghostclaw::browser
