#pragma once

#include "ghostclaw/common/result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ghostclaw::soul {

class SoulManager {
public:
  explicit SoulManager(std::filesystem::path workspace_path,
                       const std::vector<std::string> &protected_sections = {},
                       std::uint32_t max_reflections = 100,
                       bool git_versioned = true);

  // Read the full SOUL.md content
  [[nodiscard]] std::string load() const;

  // Update a named section (creates section if missing, replaces content if exists)
  [[nodiscard]] common::Status update_section(const std::string &section,
                                              const std::string &content);

  // Append a timestamped reflection to the Reflections section
  [[nodiscard]] common::Status append_reflection(const std::string &reflection);

  // Return a short summary for system prompt injection
  [[nodiscard]] std::string summary() const;

  // Initialize SOUL.md with default structure if it doesn't exist
  [[nodiscard]] common::Status initialize(const std::string &name = "GhostClaw");

  // Return the path to SOUL.md
  [[nodiscard]] std::filesystem::path soul_path() const { return soul_path_; }

private:
  [[nodiscard]] bool is_protected(const std::string &section) const;
  void git_commit(const std::string &message) const;
  [[nodiscard]] std::size_t count_reflections(const std::string &content) const;

  std::filesystem::path soul_path_;
  std::vector<std::string> protected_sections_;
  std::uint32_t max_reflections_;
  bool git_versioned_;
};

} // namespace ghostclaw::soul
