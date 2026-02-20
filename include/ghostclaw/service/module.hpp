#pragma once

#include "ghostclaw/common/result.hpp"

#include <string>
#include <vector>

namespace ghostclaw::service {

[[nodiscard]] common::Result<std::string>
handle_command(const std::vector<std::string> &args, const std::string &executable_path);

} // namespace ghostclaw::service
