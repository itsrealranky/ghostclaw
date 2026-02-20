#include "ghostclaw/peripheral/module.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/config/config.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace ghostclaw::peripheral {

namespace {

std::filesystem::path registry_path() {
  const auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(workspace.value() / "peripherals", ec);
  return workspace.value() / "peripherals" / "registry.tsv";
}

std::string normalize_board(std::string board) {
  board = common::to_lower(common::trim(board));
  if (board == "uno" || board == "arduino" || board == "arduino-uno" ||
      board == "arduino-uno-r3" || board == "arduino-uno-q" || board == "uno-q") {
    return "arduino-uno-q";
  }
  if (board == "nucleo" || board == "nucleo-f4" || board == "nucleo-f401re" ||
      board == "nucleo-f411re" || board == "stm32") {
    return "nucleo-f4";
  }
  if (board == "esp32" || board == "esp32-devkit" || board == "esp32c3" ||
      board == "esp32s3") {
    return "esp32";
  }
  return board;
}

bool path_exists_or_windows_com(const std::string &path) {
#if defined(_WIN32)
  return common::starts_with(common::to_lower(path), "com");
#else
  std::error_code ec;
  return std::filesystem::exists(path, ec);
#endif
}

common::Result<std::vector<PeripheralRecord>> load_registry(const std::filesystem::path &path) {
  if (path.empty()) {
    return common::Result<std::vector<PeripheralRecord>>::failure(
        "unable to resolve workspace for peripheral registry");
  }

  if (!std::filesystem::exists(path)) {
    return common::Result<std::vector<PeripheralRecord>>::success({});
  }

  std::ifstream in(path);
  if (!in) {
    return common::Result<std::vector<PeripheralRecord>>::failure(
        "failed to read peripheral registry");
  }

  std::vector<PeripheralRecord> entries;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }

    std::stringstream stream(line);
    PeripheralRecord entry;
    if (!std::getline(stream, entry.board, '\t')) {
      continue;
    }
    if (!std::getline(stream, entry.transport, '\t')) {
      continue;
    }
    std::getline(stream, entry.path);
    entry.board = normalize_board(entry.board);
    entry.transport = common::trim(entry.transport);
    entry.path = common::trim(entry.path);
    if (!entry.board.empty()) {
      entries.push_back(std::move(entry));
    }
  }

  std::sort(entries.begin(), entries.end(), [](const PeripheralRecord &lhs, const PeripheralRecord &rhs) {
    if (lhs.board == rhs.board) {
      return lhs.path < rhs.path;
    }
    return lhs.board < rhs.board;
  });
  return common::Result<std::vector<PeripheralRecord>>::success(std::move(entries));
}

common::Status save_registry(const std::filesystem::path &path,
                             const std::vector<PeripheralRecord> &entries) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return common::Status::error("failed to create peripheral directory: " + ec.message());
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return common::Status::error("failed to write peripheral registry");
  }

  for (const auto &entry : entries) {
    out << normalize_board(entry.board) << '\t' << entry.transport << '\t' << entry.path << '\n';
  }

  if (!out) {
    return common::Status::error("failed to flush peripheral registry");
  }
  return common::Status::success();
}

std::string replace_all(std::string text, const std::string &from, const std::string &to) {
  if (from.empty()) {
    return text;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

bool is_truthy_env(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  const std::string normalized = common::to_lower(common::trim(value));
  return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

std::vector<std::string> valid_firmware_extensions(const std::string &board) {
  const std::string normalized = normalize_board(board);
  if (normalized == "arduino-uno-q") {
    return {".hex"};
  }
  if (normalized == "nucleo-f4") {
    return {".bin"};
  }
  if (normalized == "esp32") {
    return {".bin"};
  }
  return {".bin", ".hex"};
}

bool has_allowed_extension(const std::filesystem::path &firmware_path,
                           const std::vector<std::string> &extensions) {
  const std::string ext = common::to_lower(firmware_path.extension().string());
  for (const auto &allowed : extensions) {
    if (ext == allowed) {
      return true;
    }
  }
  return false;
}

common::Result<std::filesystem::path> validate_firmware_artifact(const std::filesystem::path &firmware,
                                                                  const std::string &board) {
  std::error_code ec;
  if (!std::filesystem::exists(firmware, ec) || !std::filesystem::is_regular_file(firmware, ec)) {
    return common::Result<std::filesystem::path>::failure("firmware artifact not found: " + firmware.string());
  }

  const auto allowed_ext = valid_firmware_extensions(board);
  if (!has_allowed_extension(firmware, allowed_ext)) {
    std::ostringstream msg;
    msg << "firmware artifact extension not supported for board '" << normalize_board(board)
        << "': " << firmware.extension().string() << " (expected ";
    for (std::size_t i = 0; i < allowed_ext.size(); ++i) {
      if (i > 0) {
        msg << ", ";
      }
      msg << allowed_ext[i];
    }
    msg << ")";
    return common::Result<std::filesystem::path>::failure(msg.str());
  }

  return common::Result<std::filesystem::path>::success(firmware);
}

common::Result<std::filesystem::path> discover_firmware_artifact(const std::string &board) {
  const auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    return common::Result<std::filesystem::path>::failure(workspace.error());
  }

  const std::string normalized_board = normalize_board(board);
  const auto allowed_ext = valid_firmware_extensions(normalized_board);

  std::vector<std::filesystem::path> roots = {
      workspace.value() / "peripherals" / "firmware" / normalized_board,
      workspace.value() / "firmware" / normalized_board,
      workspace.value() / "peripherals" / "firmware",
  };

  std::filesystem::path newest_path;
  std::filesystem::file_time_type newest_time{};
  bool found = false;

  for (const auto &root : roots) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
      continue;
    }

    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec || !entry.is_regular_file()) {
        continue;
      }
      if (!has_allowed_extension(entry.path(), allowed_ext)) {
        continue;
      }

      auto write_time = entry.last_write_time(ec);
      if (ec) {
        continue;
      }
      if (!found || write_time > newest_time) {
        found = true;
        newest_time = write_time;
        newest_path = entry.path();
      }
    }
  }

  if (!found) {
    std::ostringstream msg;
    msg << "no firmware artifact found for board '" << normalized_board
        << "' under workspace peripherals/firmware";
    return common::Result<std::filesystem::path>::failure(msg.str());
  }

  return common::Result<std::filesystem::path>::success(newest_path);
}

struct FlashPlan {
  std::string board;
  std::string command_template;
  std::string required_tool;
  bool requires_port = false;
  bool has_env_override = false;
};

FlashPlan build_flash_plan(const std::string &board) {
  const std::string normalized = normalize_board(board);

  auto env_template = [&](const char *name) -> std::optional<std::string> {
    const char *value = std::getenv(name);
    if (value == nullptr) {
      return std::nullopt;
    }
    const std::string trimmed = common::trim(value);
    if (trimmed.empty()) {
      return std::nullopt;
    }
    return trimmed;
  };

  if (normalized == "arduino-uno-q") {
    if (auto override = env_template("GHOSTCLAW_UNO_FLASH_COMMAND"); override.has_value()) {
      return FlashPlan{.board = normalized,
                       .command_template = *override,
                       .required_tool = "",
                       .requires_port = true,
                       .has_env_override = true};
    }
    return FlashPlan{.board = normalized,
                     .command_template = "arduino-cli upload --input-file \"{firmware}\" --port \"{port}\"",
                     .required_tool = "arduino-cli",
                     .requires_port = true,
                     .has_env_override = false};
  }

  if (normalized == "nucleo-f4") {
    if (auto override = env_template("GHOSTCLAW_NUCLEO_FLASH_COMMAND"); override.has_value()) {
      return FlashPlan{.board = normalized,
                       .command_template = *override,
                       .required_tool = "",
                       .requires_port = false,
                       .has_env_override = true};
    }
    return FlashPlan{.board = normalized,
                     .command_template = "st-flash --reset write \"{firmware}\" 0x08000000",
                     .required_tool = "st-flash",
                     .requires_port = false,
                     .has_env_override = false};
  }

  if (normalized == "esp32") {
    if (auto override = env_template("GHOSTCLAW_ESP32_FLASH_COMMAND"); override.has_value()) {
      return FlashPlan{.board = normalized,
                       .command_template = *override,
                       .required_tool = "",
                       .requires_port = true,
                       .has_env_override = true};
    }
    return FlashPlan{.board = normalized,
                     .command_template = "esptool.py --chip esp32 --port \"{port}\" write_flash 0x1000 \"{firmware}\"",
                     .required_tool = "esptool.py",
                     .requires_port = true,
                     .has_env_override = false};
  }

  if (auto override = env_template("GHOSTCLAW_FLASH_COMMAND"); override.has_value()) {
    return FlashPlan{.board = normalized,
                     .command_template = *override,
                     .required_tool = "",
                     .requires_port = true,
                     .has_env_override = true};
  }

  return FlashPlan{.board = normalized,
                   .command_template = "",
                   .required_tool = "",
                   .requires_port = true,
                   .has_env_override = false};
}

bool tool_exists(const std::string &tool) {
  if (tool.empty()) {
    return true;
  }
#if defined(_WIN32)
  const std::string command = "where " + tool + " >NUL 2>&1";
#else
  const std::string command = "command -v " + tool + " >/dev/null 2>&1";
#endif
  const int rc = std::system(command.c_str());
  return rc == 0;
}

std::optional<PeripheralRecord> choose_target_record(const std::vector<PeripheralRecord> &records,
                                                     const std::optional<std::string> &board,
                                                     const std::optional<std::string> &port) {
  const std::string board_filter = board.has_value() ? normalize_board(*board) : "";
  const std::string port_filter = port.has_value() ? common::trim(*port) : "";

  for (const auto &record : records) {
    if (!board_filter.empty() && normalize_board(record.board) != board_filter) {
      continue;
    }
    if (!port_filter.empty() && common::trim(record.path) != port_filter) {
      continue;
    }
    return record;
  }

  if (!board_filter.empty()) {
    return std::nullopt;
  }

  for (const auto &record : records) {
    if (record.transport == "serial") {
      return record;
    }
  }
  return std::nullopt;
}

} // namespace

common::Result<std::vector<PeripheralRecord>> list_peripherals() {
  return load_registry(registry_path());
}

common::Status add_peripheral(const std::string &board, const std::string &path) {
  const std::string clean_board = normalize_board(board);
  const std::string clean_path = common::trim(path);
  if (clean_board.empty() || clean_path.empty()) {
    return common::Status::error("board and path are required");
  }

  const auto registry = registry_path();
  auto loaded = load_registry(registry);
  if (!loaded.ok()) {
    return common::Status::error(loaded.error());
  }

  const std::string transport = clean_path == "native" ? "native" : "serial";

  for (const auto &entry : loaded.value()) {
    if (normalize_board(entry.board) == clean_board && entry.path == clean_path) {
      return common::Status::success();
    }
  }

  loaded.value().push_back({.board = clean_board, .transport = transport, .path = clean_path});
  return save_registry(registry, loaded.value());
}

common::Result<std::string> flash_peripheral(const PeripheralFlashOptions &options) {
  auto listed = list_peripherals();
  if (!listed.ok()) {
    return common::Result<std::string>::failure(listed.error());
  }

  const auto selected = choose_target_record(listed.value(), options.board, options.port);

  std::string board = options.board.has_value() ? normalize_board(*options.board) : "";
  std::string port = options.port.has_value() ? common::trim(*options.port) : "";

  if (selected.has_value()) {
    if (board.empty()) {
      board = normalize_board(selected->board);
    }
    if (port.empty()) {
      port = selected->path;
    }
  }

  if (board.empty()) {
    board = "arduino-uno-q";
  }

  auto plan = build_flash_plan(board);
  if (plan.command_template.empty()) {
    return common::Result<std::string>::failure(
        "no flash workflow configured for board '" + board + "' (set a board-specific flash command env var)");
  }

  if (plan.requires_port && port.empty()) {
    return common::Result<std::string>::failure(
        "serial port is required for board '" + board + "' (use --port or add peripheral mapping)");
  }
  if (plan.requires_port && !path_exists_or_windows_com(port)) {
    return common::Result<std::string>::failure("serial port not found: " + port);
  }

  std::filesystem::path firmware_path;
  if (options.firmware.has_value() && !common::trim(*options.firmware).empty()) {
    firmware_path = std::filesystem::path(common::trim(*options.firmware));
    auto validated = validate_firmware_artifact(firmware_path, board);
    if (!validated.ok()) {
      return common::Result<std::string>::failure(validated.error());
    }
    firmware_path = validated.value();
  } else {
    auto discovered = discover_firmware_artifact(board);
    if (!discovered.ok()) {
      return common::Result<std::string>::failure(discovered.error());
    }
    firmware_path = discovered.value();
  }

  std::string command = plan.command_template;
  command = replace_all(command, "{board}", board);
  command = replace_all(command, "{port}", port);
  command = replace_all(command, "{firmware}", firmware_path.string());

  const bool execute = options.execute || is_truthy_env("GHOSTCLAW_FLASH_EXECUTE") ||
                       plan.has_env_override;

  if (!execute) {
    std::ostringstream msg;
    msg << "validated firmware " << firmware_path.string() << " for board '" << board << "'";
    if (!port.empty()) {
      msg << " on " << port;
    }
    msg << " (preview command: " << command << ")";
    return common::Result<std::string>::success(msg.str());
  }

  if (!plan.required_tool.empty() && !tool_exists(plan.required_tool)) {
    return common::Result<std::string>::failure(
        "required flashing tool not found in PATH: " + plan.required_tool);
  }

  const int rc = std::system(command.c_str());
  if (rc != 0) {
    return common::Result<std::string>::failure("flash command failed with exit code " +
                                                std::to_string(rc));
  }

  std::ostringstream msg;
  msg << "flashed " << board << " using " << firmware_path.string();
  if (!port.empty()) {
    msg << " on " << port;
  }
  return common::Result<std::string>::success(msg.str());
}

common::Result<std::string> flash_peripheral(const std::optional<std::string> &port) {
  PeripheralFlashOptions options;
  options.port = port;
  return flash_peripheral(options);
}

common::Status setup_uno_q_bridge(const std::optional<std::string> &host) {
  const auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    return common::Status::error(workspace.error());
  }

  const std::string resolved_host = host.has_value() && !common::trim(*host).empty()
                                        ? common::trim(*host)
                                        : "127.0.0.1";

  const auto path = workspace.value() / "peripherals" / "uno-q.env";
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return common::Status::error("failed to create peripheral directory: " + ec.message());
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return common::Status::error("failed to write uno-q bridge config");
  }
  out << "HOST=" << resolved_host << "\n";
  out << "PORT=8091\n";
  if (!out) {
    return common::Status::error("failed to flush uno-q bridge config");
  }

  return common::Status::success();
}

common::Result<std::string> flash_nucleo() {
  PeripheralFlashOptions options;
  options.board = "nucleo-f4";
  if (const char *firmware = std::getenv("GHOSTCLAW_NUCLEO_FIRMWARE");
      firmware != nullptr && *firmware != '\0') {
    options.firmware = std::string(firmware);
  }
  return flash_peripheral(options);
}

} // namespace ghostclaw::peripheral
