#pragma once

#include "ghostclaw/common/result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ghostclaw::peripheral {

struct PeripheralRecord {
  std::string board;
  std::string transport;
  std::string path;
};

struct PeripheralFlashOptions {
  std::optional<std::string> board;
  std::optional<std::string> port;
  std::optional<std::string> firmware;
  bool execute = false;
};

[[nodiscard]] common::Result<std::vector<PeripheralRecord>> list_peripherals();
[[nodiscard]] common::Status add_peripheral(const std::string &board, const std::string &path);
[[nodiscard]] common::Result<std::string> flash_peripheral(const std::optional<std::string> &port);
[[nodiscard]] common::Result<std::string> flash_peripheral(const PeripheralFlashOptions &options);
[[nodiscard]] common::Status setup_uno_q_bridge(const std::optional<std::string> &host);
[[nodiscard]] common::Result<std::string> flash_nucleo();

} // namespace ghostclaw::peripheral
