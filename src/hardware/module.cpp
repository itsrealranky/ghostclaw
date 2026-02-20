#include "ghostclaw/hardware/module.hpp"

#include "ghostclaw/common/fs.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <unordered_map>

namespace ghostclaw::hardware {

namespace {

std::string guess_board_name(const std::string &device_path) {
  const std::string path = common::to_lower(device_path);

  if (path.find("stlink") != std::string::npos || path.find("acm") != std::string::npos) {
    return "nucleo-f4";
  }
  if (path.find("usbmodem") != std::string::npos || path.find("wchusbserial") != std::string::npos ||
      path.find("arduino") != std::string::npos) {
    return "arduino-uno";
  }
  if (path.find("usbserial") != std::string::npos || path.find("cp210") != std::string::npos) {
    return "esp32";
  }
  if (path.find("com") != std::string::npos) {
    return "serial-device";
  }
  return "unknown";
}

bool is_serial_like_path(const std::string &path) {
  const std::string lower = common::to_lower(path);
  return lower.find("tty") != std::string::npos || lower.find("usb") != std::string::npos ||
         common::starts_with(lower, "com");
}

void collect_entries(const std::filesystem::path &directory, const std::string &prefix,
                     std::set<std::string> &out_paths) {
  std::error_code ec;
  if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
    return;
  }

  for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      break;
    }
    const auto name = entry.path().filename().string();
    if (!prefix.empty() && !common::starts_with(name, prefix)) {
      continue;
    }
    out_paths.insert(entry.path().string());
  }
}

} // namespace

common::Result<std::vector<DiscoveredDevice>> discover_devices() {
  std::set<std::string> paths;

#if defined(_WIN32)
  for (int i = 1; i <= 32; ++i) {
    paths.insert("COM" + std::to_string(i));
  }
#else
  collect_entries("/dev", "ttyUSB", paths);
  collect_entries("/dev", "ttyACM", paths);
  collect_entries("/dev", "tty.", paths);
  collect_entries("/dev", "cu.", paths);

  std::error_code ec;
  if (std::filesystem::exists("/dev/serial/by-id", ec) &&
      std::filesystem::is_directory("/dev/serial/by-id", ec)) {
    for (const auto &entry : std::filesystem::directory_iterator("/dev/serial/by-id", ec)) {
      if (ec) {
        break;
      }
      paths.insert(entry.path().string());
    }
  }
#endif

  std::vector<DiscoveredDevice> devices;
  devices.reserve(paths.size());
  for (const auto &path : paths) {
    DiscoveredDevice device;
    device.path = path;
    device.board = guess_board_name(path);
    device.transport = "serial";
    devices.push_back(std::move(device));
  }

  std::sort(devices.begin(), devices.end(),
            [](const DiscoveredDevice &lhs, const DiscoveredDevice &rhs) {
              return lhs.path < rhs.path;
            });
  return common::Result<std::vector<DiscoveredDevice>>::success(std::move(devices));
}

common::Result<DeviceInfo> introspect_device(const std::string &path) {
  const std::string trimmed = common::trim(path);
  if (trimmed.empty()) {
    return common::Result<DeviceInfo>::failure("device path is required");
  }

  DeviceInfo info;
  info.path = trimmed;
  info.serial_like = is_serial_like_path(trimmed);
  info.board = guess_board_name(trimmed);
  info.transport = "serial";

#if defined(_WIN32)
  info.exists = common::starts_with(common::to_lower(trimmed), "com");
#else
  std::error_code ec;
  info.exists = std::filesystem::exists(trimmed, ec);
#endif

  return common::Result<DeviceInfo>::success(std::move(info));
}

std::string chip_info_summary(const std::string &chip_name) {
  const std::string chip = common::to_lower(common::trim(chip_name));

  static const std::unordered_map<std::string, std::string> known = {
      {"stm32f401re", "ARM Cortex-M4, Flash 512KB, RAM 96KB"},
      {"stm32f411re", "ARM Cortex-M4, Flash 512KB, RAM 128KB"},
      {"esp32", "Xtensa dual-core, Flash external, RAM 520KB"},
      {"atmega328p", "8-bit AVR, Flash 32KB, SRAM 2KB"},
      {"rp2040", "ARM Cortex-M0+ dual-core, Flash external, SRAM 264KB"},
  };

  const auto it = known.find(chip);
  if (it != known.end()) {
    return it->second;
  }

  return "Unknown chip. Provide --chip with one of: stm32f401re, stm32f411re, esp32, atmega328p, rp2040";
}

} // namespace ghostclaw::hardware
