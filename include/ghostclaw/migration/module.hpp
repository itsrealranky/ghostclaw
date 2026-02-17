#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ghostclaw::migration {

struct LegacyImportOptions {
  std::optional<std::filesystem::path> settings_path;
  bool merge_with_existing = false;
  bool write_config = true;
};

struct LegacyImportResult {
  std::filesystem::path settings_path;
  std::size_t imported_agents = 0;
  std::size_t imported_teams = 0;
  bool created_default_agent = false;
  std::vector<std::string> warnings;
  config::Config merged_config;
};

[[nodiscard]] common::Result<std::filesystem::path> detect_legacy_settings_path();

[[nodiscard]] common::Result<LegacyImportResult>
import_legacy_settings(const LegacyImportOptions &options);

} // namespace ghostclaw::migration
