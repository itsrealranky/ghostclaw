#pragma once

#include "ghostclaw/common/result.hpp"

#include <string>
#include <vector>

namespace ghostclaw::hardware {

struct DiscoveredDevice {
  std::string path;
  std::string board;
  std::string transport;
};

struct DeviceInfo {
  std::string path;
  bool exists = false;
  bool serial_like = false;
  std::string board;
  std::string transport;
};

[[nodiscard]] common::Result<std::vector<DiscoveredDevice>> discover_devices();
[[nodiscard]] common::Result<DeviceInfo> introspect_device(const std::string &path);
[[nodiscard]] std::string chip_info_summary(const std::string &chip_name);

} // namespace ghostclaw::hardware
